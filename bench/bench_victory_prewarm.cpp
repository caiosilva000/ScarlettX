// Opt-in benchmark: does preparing a shader and its pipeline ahead of time shorten the first use?
//
// One process runs one workload under one condition and appends its timings to two CSV files.
// A driver (tools/sx_bench_prewarm.py) launches it many times.
//
//   --workload compute    translated compute: synthetic GFX10 -> victory-shader bridge -> DXIL ->
//                         D3D12Backend probe session (backend path, not the shim's guest route)
//   --workload draw       translated draw: synthetic GFX10 vertex + pixel programs -> bridge ->
//                         DXIL -> D3D12Backend probe session (backend path)
//   --workload vector     an integer kernel over --elements dwords (salt = its literal k), for
//                         comparing host backends on the same numbers: cold path, warm dispatch
//                         and a hash of the output for fixed constants
//   --workload cache      what a cache could save for a shader of --rounds size: the translator's
//                         first call apart from this shader's translation, pipeline creation the
//                         first time and again, and pipeline blobs (--corrupt-blob also offers
//                         the host a damaged one). --shape branch0..branch3 gives every round a
//                         lane-divergent if/else (1 nested, 2 with stores, 3 with the descriptor
//                         loaded again on each side); the process's working set is reported too
//   --workload guest      guest-facing compute: the vector kernel as a console-shaped container
//                         through the d3d12_xs shim's device object and command list (salt = k)
//   --workload control    graphics control without translation: the existing offscreen indexed
//                         draw (HLSL -> d3dcompiler, pipeline built inside every call)
//
//   --condition cold        nothing prepared: the first use translates, builds the pipeline and submits
//   --condition prewarmed   translate and build the pipeline first; the first use is only a submission
//   --condition primed      as prewarmed, plus one throwaway submission before the first use (extra:
//                           shows whether a host defers work to the first submission)
//   --condition steady      pipeline built and three warm-up submissions made; no first use is timed
//
// Every launch then runs --iterations timed submissions with the pipeline in place (the steady
// loop), with the validation read-back outside the loop. Times are wall-clock milliseconds
// around the calls named in each metric; `gpu_ms` is a timestamp-query duration and is reported
// only when the host returns usable timestamps. These are offscreen submission timings, not
// frame pacing.
//
// Exit codes: 0 pass, 1 a result was not exact, 2 a D3D12 call failed, 3 no device,
// 4 translation failed, 64 usage.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d12.h>
#include <psapi.h>

#include "scarlettx/gpu/d3d12_backend.h"
#include "scarlettx/gpu/victory_shader_bridge.h"
#include "victory_guest_rig.h"

namespace {

double MsBetween(const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    return static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}
LARGE_INTEGER Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t;
}
uint64_t Fnv(const std::vector<uint8_t>& bytes) {
    uint64_t h = 14695981039346656037ull;
    for (uint8_t b : bytes) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}
uint32_t FloatBits(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    return bits;
}
double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return -1.0;
    std::sort(v.begin(), v.end());
    const double rank = p * static_cast<double>(v.size() - 1);
    const size_t lo = static_cast<size_t>(rank);
    const size_t hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (rank - static_cast<double>(lo));
}

struct Args {
    std::string workload = "compute";
    std::string condition = "cold";
    std::string series = "default";
    std::string csv;
    std::string samples;
    uint32_t iterations = 200;
    uint32_t salt = 0;
    uint32_t launch = 0;
    uint32_t hold_ms = 0;
    uint32_t elements = 1u << 20;
    uint32_t rounds = 1;
    int branch_shape = -1;  // --shape branch0..branch3: the kernel with a lane-divergent if/else in every round
    bool corrupt_blob = false;
};

struct Recorder {
    const Args& args;
    const char* shader_path;
    uint64_t shader_hash = 0;
    FILE* csv = nullptr;
    FILE* samples = nullptr;

    void Metric(const char* name, double value) {
        std::printf("[bench] %s=%.4f\n", name, value);
        if (csv) {
            std::fprintf(csv, "%s,%u,%s,\"%s\",%s,%u,%016llx,%s,%.6f\n", args.series.c_str(), args.launch,
                         args.workload.c_str(), shader_path, args.condition.c_str(), args.salt,
                         static_cast<unsigned long long>(shader_hash), name, value);
        }
    }
    void Sample(uint32_t iteration, const scarlettx::ProbeSubmitTiming& t) {
        if (samples) {
            std::fprintf(samples, "%s,%u,%s,%s,%u,%u,%.6f,%.6f,%.6f\n", args.series.c_str(), args.launch,
                         args.workload.c_str(), args.condition.c_str(), args.salt, iteration, t.record_ms,
                         t.submit_to_fence_ms, t.gpu_ms);
        }
    }
};

constexpr uint32_t kSentinel = 0xA5A5A5A5u;
constexpr uint32_t kUavDwords = 4;
constexpr uint32_t kSize = 64;
constexpr uint8_t kClear[4] = {16, 32, 48, 255};

const uint32_t kComputeInputs[][4] = {
    {1, 2, 3, 4}, {0x12345678u, 0x9ABCDEF0u, 0x0F0F0F0Fu, 0x11111111u}, {0xFFFFFFFFu, 1, 0, 0},
    {0, 0, 0xFFFFFFFFu, 1}, {0xDEADBEEFu, 1, 0xCAFEF00Du, 0x80000000u},
};
const uint8_t kColours[][4] = {{32, 224, 96, 255}, {255, 0, 128, 64}, {1, 2, 3, 4}, {200, 100, 50, 25}};

int Fail(const char* what, const scarlettx::Status& st, const scarlettx::OffscreenProbeReport& report) {
    std::printf("[bench] FAILED %s api=%s hr=0x%08x (%s)\n", what, report.failed_api.empty() ? "?" : report.failed_api.c_str(),
                static_cast<uint32_t>(report.failed_hr), st.message.c_str());
    return 2;
}

// A workload behind one interface, so the conditions are written once.
struct Workload {
    scarlettx::D3D12Backend& backend;
    uint32_t salt;
    bool compute;
    std::vector<uint8_t> shader_a;  // compute DXIL, or the vertex shader
    std::vector<uint8_t> shader_b;  // the pixel shader
    scarlettx::VictoryComputeBinding binding;  // compute, or the pixel stage
    uint32_t session = 0;
    uint32_t submissions = 0;

    scarlettx::Status Translate() {
        scarlettx::VictoryShaderInfo info;
        if (compute) return scarlettx::TranslateVictorySyntheticCompute(info, binding, shader_a, salt);
        scarlettx::VictoryComputeBinding vs_binding;
        return scarlettx::TranslateVictorySyntheticDraw(info, vs_binding, shader_a, binding, shader_b, salt);
    }
    scarlettx::Status Prepare(scarlettx::OffscreenProbeReport& report, scarlettx::ProbePrepareTiming& timing) {
        if (compute) {
            scarlettx::ComputeProbeDesc desc;
            desc.shader = shader_a.data();
            desc.shader_size = shader_a.size();
            desc.root_constants.assign(binding.user_data_dwords, 0);
            desc.uav_dwords = kUavDwords;
            desc.uav_fill = kSentinel;
            return backend.PrepareComputeProbe(desc, session, report, &timing);
        }
        scarlettx::DrawProbeDesc desc;
        desc.vs = shader_a.data();
        desc.vs_size = shader_a.size();
        desc.ps = shader_b.data();
        desc.ps_size = shader_b.size();
        desc.ps_constants.assign(binding.user_data_dwords, 0);
        desc.width = kSize;
        desc.height = kSize;
        desc.viewport_width = kSize / 2;
        std::memcpy(desc.clear_rgba, kClear, 4);
        return backend.PrepareDrawProbe(desc, session, report, &timing);
    }
    std::vector<uint32_t> Constants(uint32_t n) const {
        std::vector<uint32_t> user_data(binding.user_data_dwords, 0);
        for (const auto& u : binding.user_data) {
            if (u.root != 0) continue;  // the table's heap offset stays 0: the view is at index 0
            user_data[u.index] = compute ? kComputeInputs[n % 5][u.dword]
                                         : FloatBits(static_cast<float>(kColours[n % 4][u.dword]) / 255.0f);
        }
        return user_data;
    }
    scarlettx::Status Submit(scarlettx::OffscreenProbeReport& report, scarlettx::ProbeSubmitTiming* timing) {
        const auto st = backend.SubmitProbeWork(session, Constants(submissions), report, timing);
        if (st.ok()) ++submissions;
        return st;
    }
    // Checks what the last submission left. 0 exact, 1 not exact, 2 a call failed.
    int Validate(const char* when, uint32_t* exact_out, uint32_t* total_out) {
        const uint32_t n = submissions - 1;
        scarlettx::OffscreenProbeReport report;
        uint32_t exact = 0;
        uint32_t total = 0;
        if (compute) {
            std::vector<uint32_t> out;
            const auto st = backend.ReadComputeProbe(session, out, report);
            if (!st.ok()) return Fail("read-back", st, report);
            const uint32_t* c = kComputeInputs[n % 5];
            const uint32_t want[kUavDwords] = {((c[0] + c[1]) ^ c[2]) + c[3], kSentinel, kSentinel, kSentinel};
            total = kUavDwords;
            for (uint32_t i = 0; i < kUavDwords; ++i) exact += out[i] == want[i];
        } else {
            scarlettx::FrameBufferRgba8 frame;
            const auto st = backend.ReadDrawProbe(session, frame, report);
            if (!st.ok()) return Fail("read-back", st, report);
            total = kSize * kSize;
            for (uint32_t y = 0; y < kSize; ++y) {
                for (uint32_t x = 0; x < kSize; ++x) {
                    const uint8_t* want = x < kSize / 2 ? kColours[n % 4] : kClear;
                    exact += std::memcmp(&frame.pixels[(static_cast<size_t>(y) * kSize + x) * 4], want, 4) == 0;
                }
            }
        }
        std::printf("[bench] validate %s: %s exact %u/%u (fence %llu/%llu wait %u)\n", when, compute ? "dwords" : "pixels",
                    exact, total, static_cast<unsigned long long>(report.fence_completed),
                    static_cast<unsigned long long>(report.fence_signaled), report.wait_result);
        *exact_out += exact;
        *total_out += total;
        return exact == total ? 0 : 1;
    }
};

int RunTranslated(const Args& args, scarlettx::D3D12Backend& backend, Recorder& rec, double init_ms) {
    Workload w{backend, args.salt, args.workload == "compute", {}, {}, {}};
    const bool cold = args.condition == "cold";
    const bool steady = args.condition == "steady";
    uint32_t exact = 0;
    uint32_t total = 0;
    uint32_t fences = 0;

    // Translation and pipeline creation: inside the first use when cold, ahead of it otherwise.
    LARGE_INTEGER start = Now();
    const auto translated = w.Translate();
    const double translate_ms = MsBetween(start, Now());
    if (!translated.ok()) {
        std::printf("[bench] translate FAILED: %s\n", translated.message.c_str());
        return 4;
    }
    std::vector<uint8_t> all = w.shader_a;
    all.insert(all.end(), w.shader_b.begin(), w.shader_b.end());
    rec.shader_hash = Fnv(all);
    scarlettx::OffscreenProbeReport report;
    scarlettx::ProbePrepareTiming prepare;
    auto st = w.Prepare(report, prepare);
    if (!st.ok()) return Fail("prepare", st, report);
    const double pipeline_ms = prepare.root_signature_ms + prepare.pipeline_ms;
    std::printf("[bench] shader bytes=%zu fnv=%016llx gpu_timestamps=%s\n", all.size(),
                static_cast<unsigned long long>(rec.shader_hash), backend.ProbeHasGpuTimestamps(w.session) ? "heap created" : "no");

    double priming_ms = 0.0;
    if (args.condition == "primed" || steady) {
        const uint32_t warmups = steady ? 3 : 1;
        start = Now();
        for (uint32_t i = 0; i < warmups; ++i) {
            st = w.Submit(report, nullptr);
            if (!st.ok()) return Fail("warm-up submission", st, report);
            ++fences;
        }
        priming_ms = MsBetween(start, Now());
    }

    rec.Metric("backend_init_ms", init_ms);
    rec.Metric("shader_bytes", static_cast<double>(all.size()));
    rec.Metric("translate_ms", translate_ms);
    rec.Metric("root_signature_ms", prepare.root_signature_ms);
    rec.Metric("pipeline_create_ms", prepare.pipeline_ms);
    rec.Metric("resources_ms", prepare.resources_ms);
    if (priming_ms > 0.0) rec.Metric("priming_submissions_ms", priming_ms);

    if (!steady) {
        // The first use.
        scarlettx::ProbeSubmitTiming first;
        st = w.Submit(report, &first);
        if (!st.ok()) return Fail("first submission", st, report);
        ++fences;
        const double first_submit_ms = first.record_ms + first.submit_to_fence_ms;
        rec.Metric("first_record_ms", first.record_ms);
        rec.Metric("first_submit_to_fence_ms", first.submit_to_fence_ms);
        rec.Metric("first_submission_ms", first_submit_ms);
        if (first.gpu_ms >= 0.0) rec.Metric("first_gpu_ms", first.gpu_ms);
        // What a caller waits for at first use, and everything spent up to the first result.
        const double prepared_ms = translate_ms + pipeline_ms;
        rec.Metric("first_use_latency_ms", (cold ? prepared_ms : 0.0) + first_submit_ms);
        rec.Metric("preparation_ms", cold ? 0.0 : prepared_ms + priming_ms);
        rec.Metric("total_to_first_result_ms", prepared_ms + priming_ms + first_submit_ms);
        if (const int rc = w.Validate("first use", &exact, &total)) return rc;
    }

    // The steady loop: the pipeline is in place, nothing is read back inside it.
    std::vector<double> submit;
    std::vector<double> gpu;
    submit.reserve(args.iterations);
    for (uint32_t i = 0; i < args.iterations; ++i) {
        scarlettx::ProbeSubmitTiming t;
        st = w.Submit(report, &t);
        if (!st.ok()) return Fail("steady submission", st, report);
        if (report.fence_completed < report.fence_signaled || report.wait_result != 0) {
            std::printf("[bench] FAILED a steady fence did not complete\n");
            return 2;
        }
        ++fences;
        submit.push_back(t.record_ms + t.submit_to_fence_ms);
        if (t.gpu_ms >= 0.0) gpu.push_back(t.gpu_ms);
        rec.Sample(i, t);
    }
    if (const int rc = w.Validate("after the steady loop", &exact, &total)) return rc;
    rec.Metric("steady_iterations", static_cast<double>(submit.size()));
    rec.Metric("steady_submission_median_ms", Percentile(submit, 0.5));
    rec.Metric("steady_submission_p95_ms", Percentile(submit, 0.95));
    rec.Metric("steady_submission_min_ms", Percentile(submit, 0.0));
    rec.Metric("steady_submission_max_ms", Percentile(submit, 1.0));
    rec.Metric("gpu_timestamp_samples", static_cast<double>(gpu.size()));
    if (!gpu.empty()) {
        rec.Metric("steady_gpu_median_ms", Percentile(gpu, 0.5));
        rec.Metric("steady_gpu_p95_ms", Percentile(gpu, 0.95));
    }
    rec.Metric("fences_completed", static_cast<double>(fences));
    rec.Metric("validated_exact", static_cast<double>(exact));
    rec.Metric("validated_total", static_cast<double>(total));
    backend.ReleaseProbe(w.session);

    // The same shader and pipeline a second time in this process: what a repeat costs once the
    // translator is warm and the host has built this pipeline before. Outside every figure above.
    Workload again{backend, args.salt, w.compute, {}, {}, {}};
    start = Now();
    const auto retranslated = again.Translate();
    const double retranslate_ms = MsBetween(start, Now());
    scarlettx::ProbePrepareTiming reprepare;
    if (retranslated.ok() && again.Prepare(report, reprepare).ok()) {
        rec.Metric("translate_repeat_ms", retranslate_ms);
        rec.Metric("pipeline_recreate_ms", reprepare.pipeline_ms);
        backend.ReleaseProbe(again.session);
    }
    return 0;
}

// The vector kernel, for comparing host backends on the same numbers (see
// BuildVictoryVectorKernelContainer): element i gets (xorshift(i + c0) + c1) ^ k. One launch
// measures the cold path (translate, build the pipeline, first dispatch) and then the warm
// dispatch, and prints a hash of the output for fixed constants so another backend's output can
// be compared with it.
uint32_t VectorKernel(uint32_t i, uint32_t c0, uint32_t c1, uint32_t k) {
    uint32_t x = i + c0;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return (x + c1) ^ k;
}

int RunVector(const Args& args, scarlettx::D3D12Backend& backend, Recorder& rec, double init_ms) {
    const uint32_t n = args.elements;
    const uint32_t k = args.salt;
    LARGE_INTEGER start = Now();
    std::vector<uint8_t> container;
    scarlettx::VictoryShaderInfo info;
    scarlettx::VictoryComputeBinding binding;
    std::vector<uint8_t> dxil;
    const bool built = scarlettx::BuildVictoryVectorKernelContainer(k, container).ok();
    const auto outcome = built ? scarlettx::TranslateVictoryCompute(container.data(), container.size(), info, binding, dxil)
                               : scarlettx::VictoryComputeOutcome::Unavailable;
    const double translate_ms = MsBetween(start, Now());
    std::string why;
    if (outcome != scarlettx::VictoryComputeOutcome::Translated ||
        !scarlettx::VictoryComputeBindingSupported(binding, why)) {
        std::printf("[bench] translate FAILED: %s %s %s\n", scarlettx::VictoryComputeOutcomeName(outcome),
                    info.message.c_str(), why.c_str());
        return 4;
    }
    rec.shader_hash = Fnv(dxil);
    std::printf("[bench] shader bytes=%zu fnv=%016llx elements=%u k=%u groups=%u threads_per_group=%u\n", dxil.size(),
                static_cast<unsigned long long>(rec.shader_hash), n, k, n / 32, binding.threads[0]);

    scarlettx::ComputeProbeDesc desc;
    desc.shader = dxil.data();
    desc.shader_size = dxil.size();
    desc.root_constants.assign(binding.user_data_dwords, 0);
    desc.uav_dwords = n;
    desc.uav_fill = kSentinel;
    desc.groups[0] = n / 32;
    scarlettx::OffscreenProbeReport report;
    scarlettx::ProbePrepareTiming prepare;
    uint32_t session = 0;
    auto st = backend.PrepareComputeProbe(desc, session, report, &prepare);
    if (!st.ok()) return Fail("prepare", st, report);

    auto constants = [&](uint32_t c0, uint32_t c1) {
        std::vector<uint32_t> user_data(binding.user_data_dwords, 0);
        for (const auto& u : binding.user_data) {
            if (u.root == 0 && u.dword == 0) user_data[u.index] = c0;
            if (u.root == 0 && u.dword == 1) user_data[u.index] = c1;
        }
        return user_data;
    };
    uint64_t exact = 0;
    uint64_t total = 0;
    std::vector<uint32_t> out;
    auto validate = [&](const char* when, uint32_t c0, uint32_t c1) -> int {
        scarlettx::OffscreenProbeReport read;
        const auto rs = backend.ReadComputeProbe(session, out, read);
        if (!rs.ok()) return Fail("read-back", rs, read);
        uint32_t ok = 0;
        for (uint32_t i = 0; i < n; ++i) ok += out[i] == VectorKernel(i, c0, c1, k);
        std::printf("[bench] validate %s: dwords exact %u/%u\n", when, ok, n);
        exact += ok;
        total += n;
        return ok == n ? 0 : 1;
    };

    // Cold: the first dispatch, with everything it needed.
    scarlettx::ProbeSubmitTiming first;
    st = backend.SubmitProbeWork(session, constants(1, 2), report, &first);
    if (!st.ok()) return Fail("first submission", st, report);
    const double first_ms = first.record_ms + first.submit_to_fence_ms;
    rec.Metric("backend_init_ms", init_ms);
    rec.Metric("elements", static_cast<double>(n));
    rec.Metric("translate_ms", translate_ms);
    rec.Metric("pipeline_create_ms", prepare.root_signature_ms + prepare.pipeline_ms);
    rec.Metric("resources_ms", prepare.resources_ms);
    rec.Metric("first_submission_ms", first_ms);
    rec.Metric("cold_compile_ms", translate_ms + prepare.root_signature_ms + prepare.pipeline_ms + first_ms);
    if (const int rc = validate("first dispatch", 1, 2)) return rc;

    // Warm: dispatches with the pipeline in place, each waited for; nothing read back inside.
    std::vector<double> warm;
    uint32_t c0 = 0;
    uint32_t c1 = 0;
    for (uint32_t i = 0; i < args.iterations; ++i) {
        c0 = 0x1000u + i * 7919u;
        c1 = 0xABCD0000u ^ i;
        scarlettx::ProbeSubmitTiming t;
        st = backend.SubmitProbeWork(session, constants(c0, c1), report, &t);
        if (!st.ok()) return Fail("warm submission", st, report);
        if (report.fence_completed < report.fence_signaled || report.wait_result != 0) return 2;
        warm.push_back(t.record_ms + t.submit_to_fence_ms);
        rec.Sample(i, t);
    }
    if (const int rc = validate("after the warm loop", c0, c1)) return rc;
    rec.Metric("warm_iterations", static_cast<double>(warm.size()));
    rec.Metric("warm_dispatch_median_ms", Percentile(warm, 0.5));
    rec.Metric("warm_dispatch_p95_ms", Percentile(warm, 0.95));
    rec.Metric("warm_dispatch_min_ms", Percentile(warm, 0.0));

    // The output for fixed constants, hashed, to set beside another backend's.
    st = backend.SubmitProbeWork(session, constants(0x12345678u, 0x9E3779B9u), report, nullptr);
    if (!st.ok()) return Fail("canonical submission", st, report);
    if (const int rc = validate("canonical constants", 0x12345678u, 0x9E3779B9u)) return rc;
    uint64_t h = 14695981039346656037ull;
    for (uint32_t v : out) {
        for (int b = 0; b < 4; ++b) {
            h ^= (v >> (8 * b)) & 0xFF;
            h *= 1099511628211ull;
        }
    }
    std::printf("[bench] canonical_output_fnv=%016llx first=%08x last=%08x\n", static_cast<unsigned long long>(h), out[0],
                out[n - 1]);
    rec.Metric("validated_exact", static_cast<double>(exact));
    rec.Metric("validated_total", static_cast<double>(total));
    backend.ReleaseProbe(session);
    return 0;
}

// Guest-facing compute: the vector kernel as a console-shaped container, handed to the d3d12_xs
// shim's device object and dispatched through its command list, as a title's shader is. The
// shim translates inside CreateComputePipelineState, so translation and pipeline creation are one
// guest call; the shim reports how that call split.
int RunGuest(const Args& args, Recorder& rec) {
    constexpr uint32_t kElements = 1024;                 // 32 groups of 32 threads
    constexpr uint32_t kBufferDwords = kElements + 32;   // the tail must keep its sentinel
    const UINT groups[3] = {kElements / 32, 1, 1};
    const uint32_t k = args.salt;
    const bool cold = args.condition == "cold";
    const bool steady = args.condition == "steady";

    sxrig::GuestRig rig;
    rig.tag = "[bench]";
    LARGE_INTEGER start = Now();
    if (const int rc = rig.Init(false, kBufferDwords, 8, 5)) return rc;
    if (const int rc = rig.PrepareTimed()) return rc;
    const double init_ms = MsBetween(start, Now());
    std::vector<uint8_t> container;
    if (!scarlettx::BuildVictoryVectorKernelContainer(k, container).ok()) {
        std::printf("[bench] the container could not be built\n");
        return 4;
    }

    // Translation and pipeline creation: one guest call.
    start = Now();
    void* pso = nullptr;
    const HRESULT hr = rig.CreatePipeline(rig.title_root, container, &pso);
    const double create_ms = MsBetween(start, Now());
    if (FAILED(hr) || !pso) return rig.Fail("shim CreateComputePipelineState (slot 11)", hr);
    const auto stats = sxrig::Stats();
    if (stats.compute_translated != 1) {
        std::printf("[bench] FAILED: the shim did not translate the container\n");
        return 1;
    }
    const double translate_ms = static_cast<double>(stats.last_translate_us) / 1000.0;
    const double pipeline_ms = static_cast<double>(stats.last_host_pipeline_us) / 1000.0;
    rec.shader_hash = Fnv(container);
    std::printf("[bench] native=%llu bytes translated=%llu bytes k=%u\n",
                static_cast<unsigned long long>(stats.last_native_bytes),
                static_cast<unsigned long long>(stats.last_translated_bytes), k);

    uint32_t exact = 0;
    uint32_t total = 0;
    uint32_t fences = 0;
    uint32_t submissions = 0;
    uint32_t c[4] = {0, 0, 0, 0};
    auto submit = [&](double* record, double* wait) {
        c[0] = 0x1000u + submissions * 7919u;
        c[1] = 0xABCD0000u ^ submissions;
        ++submissions;
        const int rc = rig.TimedDispatch(rig.title_root, pso, c, 4, 1, groups, record, wait);
        if (rc == 0) ++fences;
        return rc;
    };
    auto validate = [&](const char* when) -> int {
        std::vector<uint32_t> out;
        if (const int rc = rig.ReadBack(out)) return rc;
        uint32_t ok = 0;
        for (uint32_t i = 0; i < kElements; ++i) ok += out[i] == VectorKernel(i, c[0], c[1], k);
        for (uint32_t i = kElements; i < kBufferDwords; ++i) ok += out[i] == sxrig::kSentinel;
        std::printf("[bench] validate %s: dwords exact %u/%u\n", when, ok, kBufferDwords);
        exact += ok;
        total += kBufferDwords;
        return ok == kBufferDwords ? 0 : 1;
    };

    double priming_ms = 0.0;
    if (args.condition == "primed" || steady) {
        start = Now();
        for (uint32_t i = 0; i < (steady ? 3u : 1u); ++i) {
            if (const int rc = submit(nullptr, nullptr)) return rc;
        }
        priming_ms = MsBetween(start, Now());
    }
    rec.Metric("backend_init_ms", init_ms);
    rec.Metric("shader_bytes", static_cast<double>(stats.last_translated_bytes));
    rec.Metric("native_bytes", static_cast<double>(stats.last_native_bytes));
    rec.Metric("translate_ms", translate_ms);
    rec.Metric("pipeline_create_ms", pipeline_ms);
    rec.Metric("guest_create_pipeline_ms", create_ms);
    if (priming_ms > 0.0) rec.Metric("priming_submissions_ms", priming_ms);
    if (!steady) {
        double record = 0;
        double wait = 0;
        if (const int rc = submit(&record, &wait)) return rc;
        const double first_ms = record + wait;
        rec.Metric("first_record_ms", record);
        rec.Metric("first_submit_to_fence_ms", wait);
        rec.Metric("first_submission_ms", first_ms);
        rec.Metric("first_use_latency_ms", (cold ? create_ms : 0.0) + first_ms);
        rec.Metric("preparation_ms", cold ? 0.0 : create_ms + priming_ms);
        rec.Metric("total_to_first_result_ms", create_ms + priming_ms + first_ms);
        if (const int rc = validate("first use")) return rc;
    }
    std::vector<double> samples;
    for (uint32_t i = 0; i < args.iterations; ++i) {
        scarlettx::ProbeSubmitTiming t;
        if (const int rc = submit(&t.record_ms, &t.submit_to_fence_ms)) return rc;
        samples.push_back(t.record_ms + t.submit_to_fence_ms);
        rec.Sample(i, t);
    }
    if (const int rc = validate("after the steady loop")) return rc;
    const auto after = sxrig::Stats();
    if (after.dispatches_translated != submissions || after.dispatches_skipped != 0) {
        std::printf("[bench] FAILED: %llu of %u dispatches ran the translated pipeline\n",
                    static_cast<unsigned long long>(after.dispatches_translated), submissions);
        return 1;
    }
    rec.Metric("steady_iterations", static_cast<double>(samples.size()));
    rec.Metric("steady_submission_median_ms", Percentile(samples, 0.5));
    rec.Metric("steady_submission_p95_ms", Percentile(samples, 0.95));
    rec.Metric("steady_submission_min_ms", Percentile(samples, 0.0));
    rec.Metric("steady_submission_max_ms", Percentile(samples, 1.0));
    rec.Metric("fences_completed", static_cast<double>(fences));
    rec.Metric("validated_exact", static_cast<double>(exact));
    rec.Metric("validated_total", static_cast<double>(total));
    return 0;
}

// What a cache could save, for a shader of a chosen size (--rounds): the translator's first call
// in the process apart from the cost of translating this shader, pipeline creation the first time
// and again, and whether the host hands out pipeline blobs a persistent cache could store.
uint32_t VectorKernelRounds(uint32_t i, uint32_t c0, uint32_t c1, uint32_t k, uint32_t rounds) {
    uint32_t x = i + c0;
    for (uint32_t r = 0; r < rounds; ++r) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
    }
    return (x + c1) ^ k;
}

uint32_t BranchKernelRounds(uint32_t i, uint32_t c0, uint32_t c1, uint32_t k, uint32_t rounds, int shape) {
    uint32_t x = i + c0;
    for (uint32_t r = 0; r < rounds; ++r) {
        if ((x & (1u << (r % 5))) == 0) {
            x ^= x << 13;
            if (shape == 1 && (x & 1u) == 0) x ^= x >> 7;
        } else {
            x ^= x >> 17;
        }
        x ^= x << 5;
    }
    return (x + c1) ^ k;
}

// This process's working set now and at its highest, in megabytes. Under CrossOver the host
// compiles Metal shaders partly in a service outside this process, which this does not see.
void WorkingSetMb(double& now_mb, double& peak_mb) {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    now_mb = peak_mb = -1.0;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        now_mb = static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
        peak_mb = static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
    }
}

int RunCacheAssessment(const Args& args, scarlettx::D3D12Backend& backend, Recorder& rec, double init_ms) {
    const uint32_t k = args.salt;
    const uint32_t n = 4096;
    const int shape = args.branch_shape;
    const bool branch = shape >= 0;
    auto translate = [branch, shape](uint32_t kk, uint32_t rounds, std::vector<uint8_t>& container,
                              scarlettx::VictoryComputeBinding& binding, std::vector<uint8_t>& dxil, double& ms) {
        scarlettx::VictoryShaderInfo info;
        const auto built = branch ? scarlettx::BuildVictoryBranchKernelContainer(kk, container, rounds, static_cast<uint32_t>(shape))
                                  : scarlettx::BuildVictoryVectorKernelContainer(kk, container, rounds);
        if (!built.ok()) return false;
        const LARGE_INTEGER start = Now();
        const auto outcome = scarlettx::TranslateVictoryCompute(container.data(), container.size(), info, binding, dxil);
        ms = MsBetween(start, Now());
        return outcome == scarlettx::VictoryComputeOutcome::Translated;
    };
    std::vector<uint8_t> container;
    std::vector<uint8_t> dxil;
    scarlettx::VictoryComputeBinding binding;
    double first_call_ms = 0;
    double target_ms = 0;
    double repeat_ms = 0;
    // A different, small shader first: the translator's first call in this process.
    if (!translate(k ^ 0x5A5A5A5Au, 1, container, binding, dxil, first_call_ms) ||
        !translate(k, args.rounds, container, binding, dxil, target_ms)) {
        std::printf("[bench] translate FAILED\n");
        return 4;
    }
    std::vector<uint8_t> again;
    scarlettx::VictoryComputeBinding binding_again;
    std::vector<uint8_t> container_again;
    if (!translate(k, args.rounds, container_again, binding_again, again, repeat_ms) || again != dxil) {
        std::printf("[bench] FAILED: translating the same container twice gave different shaders\n");
        return 1;
    }
    rec.shader_hash = Fnv(dxil);
    std::printf("[bench] shape=%s%s rounds=%u container=%zu bytes dxil=%zu bytes fnv=%016llx deterministic=yes\n",
                branch ? "branch" : "linear", branch ? std::to_string(shape).c_str() : "", args.rounds, container.size(),
                dxil.size(),
                static_cast<unsigned long long>(rec.shader_hash));
    double ws_now = 0;
    double ws_peak = 0;
    WorkingSetMb(ws_now, ws_peak);
    rec.Metric("working_set_after_translate_mb", ws_now);
    rec.Metric("peak_working_set_after_translate_mb", ws_peak);
    rec.Metric("backend_init_ms", init_ms);
    rec.Metric("rounds", static_cast<double>(args.rounds));
    rec.Metric("container_bytes", static_cast<double>(container.size()));
    rec.Metric("dxil_bytes", static_cast<double>(dxil.size()));
    rec.Metric("translate_first_call_ms", first_call_ms);
    rec.Metric("translate_target_ms", target_ms);
    rec.Metric("translate_target_repeat_ms", repeat_ms);

    // Pipeline creation on the host device, the first time and again, and pipeline blobs.
    auto* device = static_cast<ID3D12Device*>(backend.NativeDevice());
    D3D12_ROOT_PARAMETER user_data{};
    user_data.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    user_data.Constants.Num32BitValues = binding.user_data_dwords;
    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 1;
    rs_desc.pParameters = &user_data;
    rs_desc.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                                                            D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);
    ID3DBlob* rs_blob = nullptr;
    ID3DBlob* rs_error = nullptr;
    ID3D12RootSignature* root = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_error);
    if (SUCCEEDED(hr)) {
        hr = device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&root));
    }
    if (FAILED(hr)) {
        std::printf("[bench] FAILED api=CreateRootSignature hr=0x%08x\n", static_cast<uint32_t>(hr));
        return 2;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root;
    pso_desc.CS = {dxil.data(), dxil.size()};
    auto create = [&](const char* metric, ID3D12PipelineState** out) {
        const LARGE_INTEGER start = Now();
        const HRESULT r = device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(out));
        const double ms = MsBetween(start, Now());
        std::printf("[bench] %s hr=0x%08x\n", metric, static_cast<uint32_t>(r));
        if (SUCCEEDED(r)) rec.Metric(metric, ms);
        return r;
    };
    ID3D12PipelineState* pso = nullptr;
    ID3D12PipelineState* pso2 = nullptr;
    if (FAILED(create("pipeline_create_ms", &pso)) || FAILED(create("pipeline_recreate_ms", &pso2))) return 2;

    // Pipeline blobs: what a persistent pipeline cache would store and hand back.
    ID3DBlob* blob = nullptr;
    const HRESULT blob_hr = pso->GetCachedBlob(&blob);
    const size_t blob_bytes = SUCCEEDED(blob_hr) && blob ? blob->GetBufferSize() : 0;
    std::printf("[bench] GetCachedBlob hr=0x%08x bytes=%zu\n", static_cast<uint32_t>(blob_hr), blob_bytes);
    rec.Metric("cached_blob_available", SUCCEEDED(blob_hr) && blob_bytes ? 1.0 : 0.0);
    rec.Metric("cached_blob_bytes", static_cast<double>(blob_bytes));
    if (SUCCEEDED(blob_hr) && blob_bytes) {
        std::vector<uint8_t> bytes(static_cast<const uint8_t*>(blob->GetBufferPointer()),
                                   static_cast<const uint8_t*>(blob->GetBufferPointer()) + blob_bytes);
        pso_desc.CachedPSO = {bytes.data(), bytes.size()};
        ID3D12PipelineState* from_blob = nullptr;
        const HRESULT r = create("pipeline_from_blob_ms", &from_blob);
        rec.Metric("pipeline_from_blob_ok", SUCCEEDED(r) ? 1.0 : 0.0);
        if (args.corrupt_blob) {
            // A damaged entry must be refused, not crash the process or build a wrong pipeline.
            for (size_t i = bytes.size() / 2; i < bytes.size() / 2 + 16 && i < bytes.size(); ++i) bytes[i] ^= 0xFF;
            ID3D12PipelineState* from_corrupt = nullptr;
            std::printf("[bench] creating a pipeline from a corrupted blob\n");
            const HRESULT c = device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&from_corrupt));
            std::printf("[bench] corrupted blob hr=0x%08x pso=%s\n", static_cast<uint32_t>(c), from_corrupt ? "created" : "none");
            rec.Metric("corrupt_blob_refused", FAILED(c) ? 1.0 : 0.0);
            bytes.resize(bytes.size() / 2);
            pso_desc.CachedPSO = {bytes.data(), bytes.size()};
            from_corrupt = nullptr;
            const HRESULT t = device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&from_corrupt));
            std::printf("[bench] truncated blob hr=0x%08x pso=%s\n", static_cast<uint32_t>(t), from_corrupt ? "created" : "none");
            rec.Metric("truncated_blob_refused", FAILED(t) ? 1.0 : 0.0);
        }
        pso_desc.CachedPSO = {};
    }

    // The shader has to be right, whatever its size.
    scarlettx::ComputeProbeDesc desc;
    desc.shader = dxil.data();
    desc.shader_size = dxil.size();
    desc.root_constants.assign(binding.user_data_dwords, 0);
    desc.uav_dwords = n;
    desc.uav_fill = kSentinel;
    desc.groups[0] = n / 32;
    for (const auto& u : binding.user_data) {
        if (u.root == 0 && u.dword == 0) desc.root_constants[u.index] = 0x12345678u;
        if (u.root == 0 && u.dword == 1) desc.root_constants[u.index] = 0x9E3779B9u;
    }
    std::vector<uint32_t> out;
    scarlettx::OffscreenProbeReport report;
    const LARGE_INTEGER start = Now();
    const auto st = backend.RunComputeProbe(desc, out, report);
    if (!st.ok()) return Fail("dispatch", st, report);
    rec.Metric("probe_run_ms", MsBetween(start, Now()));
    uint32_t exact = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t want = branch ? BranchKernelRounds(i, 0x12345678u, 0x9E3779B9u, k, args.rounds, shape)
                                     : VectorKernelRounds(i, 0x12345678u, 0x9E3779B9u, k, args.rounds);
        exact += out[i] == want;
    }
    WorkingSetMb(ws_now, ws_peak);
    rec.Metric("working_set_at_end_mb", ws_now);
    rec.Metric("peak_working_set_at_end_mb", ws_peak);
    std::printf("[bench] validate: dwords exact %u/%u (fence %llu/%llu wait %u)\n", exact, n,
                static_cast<unsigned long long>(report.fence_completed), static_cast<unsigned long long>(report.fence_signaled),
                report.wait_result);
    rec.Metric("validated_exact", static_cast<double>(exact));
    rec.Metric("validated_total", static_cast<double>(n));
    return exact == n ? 0 : 1;
}

// The untranslated control: the existing indexed-draw probe, which compiles its HLSL and builds
// its pipeline inside every call.
int RunControl(const Args& args, scarlettx::D3D12Backend& backend, Recorder& rec, double init_ms) {
    const uint8_t clear[4] = {64, 128, 192, 255};
    const uint8_t quad[4] = {32, 224, 96, 255};
    std::vector<double> calls;
    uint32_t exact = 0;
    uint32_t total = 0;
    const uint32_t count = std::max<uint32_t>(2, args.iterations);
    for (uint32_t i = 0; i < count; ++i) {
        scarlettx::FrameBufferRgba8 frame;
        scarlettx::OffscreenProbeReport report;
        const LARGE_INTEGER start = Now();
        const auto st = backend.RenderOffscreenProbe(scarlettx::OffscreenProbeMode::IndexedQuad, kSize, kSize, clear,
                                                     quad, frame, report);
        calls.push_back(MsBetween(start, Now()));
        if (!st.ok()) return Fail("control draw", st, report);
        if (report.fence_completed < report.fence_signaled || report.wait_result != 0) return 2;
        if (i == 0 || i + 1 == count) {
            for (uint32_t y = 0; y < kSize; ++y) {
                for (uint32_t x = 0; x < kSize; ++x) {
                    const bool in_quad = x >= kSize / 4 && x < kSize * 3 / 4 && y >= kSize / 4 && y < kSize * 3 / 4;
                    exact += std::memcmp(&frame.pixels[(static_cast<size_t>(y) * kSize + x) * 4], in_quad ? quad : clear, 4) == 0;
                    ++total;
                }
            }
        }
    }
    std::printf("[bench] validate control: pixels exact %u/%u\n", exact, total);
    rec.Metric("backend_init_ms", init_ms);
    rec.Metric("control_first_call_ms", calls.front());
    const std::vector<double> rest(calls.begin() + 1, calls.end());
    rec.Metric("control_repeat_calls", static_cast<double>(rest.size()));
    rec.Metric("control_repeat_call_median_ms", Percentile(rest, 0.5));
    rec.Metric("control_repeat_call_p95_ms", Percentile(rest, 0.95));
    rec.Metric("validated_exact", static_cast<double>(exact));
    rec.Metric("validated_total", static_cast<double>(total));
    return exact == total ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--workload") args.workload = next();
        else if (a == "--condition") args.condition = next();
        else if (a == "--series") args.series = next();
        else if (a == "--csv") args.csv = next();
        else if (a == "--samples") args.samples = next();
        else if (a == "--iterations") args.iterations = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--salt") args.salt = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--launch") args.launch = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--elements") args.elements = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--rounds") args.rounds = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--shape") {
            const std::string s = next();
            args.branch_shape = s.rfind("branch", 0) == 0 ? (s.size() > 6 ? std::atoi(s.c_str() + 6) : 0) : -1;
            if (args.branch_shape > 3) {
                std::fprintf(stderr, "bench_victory_prewarm: --shape is linear or branch0 to branch3\n");
                return 64;
            }
        }
        else if (a == "--corrupt-blob") args.corrupt_blob = true;
        else if (a == "--hold-ms") args.hold_ms = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else {
            std::fprintf(stderr, "bench_victory_prewarm: unknown argument %s\n", a.c_str());
            return 64;
        }
    }
    const bool translated = args.workload == "compute" || args.workload == "draw";
    const bool known_condition = args.condition == "cold" || args.condition == "prewarmed" ||
                                 args.condition == "primed" || args.condition == "steady";
    const bool vector = args.workload == "vector";
    if (vector && (args.elements == 0 || args.elements % 32 != 0)) {
        std::fprintf(stderr, "bench_victory_prewarm: --elements must be a positive multiple of 32\n");
        return 64;
    }
    const bool cache = args.workload == "cache";
    const bool guest = args.workload == "guest";
    if (cache && (args.rounds == 0 || args.rounds > 4096)) {
        std::fprintf(stderr, "bench_victory_prewarm: --rounds must be 1 to 4096\n");
        return 64;
    }
    if ((!translated && !vector && !cache && !guest && args.workload != "control") || !known_condition ||
        args.iterations == 0) {
        std::fprintf(stderr, "usage: bench_victory_prewarm --workload compute|draw|guest|control|vector|cache --condition "
                             "cold|prewarmed|primed|steady [--iterations N] [--salt S] [--launch N] [--series NAME] "
                             "[--csv FILE] [--samples FILE] [--hold-ms MS] [--elements N] [--rounds N] "
                             "[--corrupt-blob]\n");
        return 64;
    }
    const char* shader_path =
        args.workload == "compute" ? "translated compute: GFX10 -> victory-shader bridge -> DXIL -> D3D12Backend (backend path)"
        : args.workload == "draw" ? "translated draw: GFX10 VS+PS -> victory-shader bridge -> DXIL -> D3D12Backend (backend path)"
        : guest ? "guest-facing compute: GFX10 container -> d3d12_xs CreateComputePipelineState (device object) -> bridge -> DXIL -> shim command list"
        : vector || cache ? "vector kernel: GFX10 container -> victory-shader bridge -> DXIL -> D3D12Backend (backend path)"
                                  : "control: HLSL -> d3dcompiler -> D3D12Backend indexed draw, pipeline built per call";
    Recorder rec{args, shader_path};
    if (!args.csv.empty()) rec.csv = std::fopen(args.csv.c_str(), "a");
    if (!args.samples.empty()) rec.samples = std::fopen(args.samples.c_str(), "a");

    std::printf("[bench] workload=%s condition=%s series=%s launch=%u salt=%u iterations=%u\n", args.workload.c_str(),
                args.condition.c_str(), args.series.c_str(), args.launch, args.salt, args.iterations);
    std::printf("[bench] shader_path=%s\n", shader_path);
    if (guest) {
        // The shim makes its own device; its adapter line goes to stderr.
        const int guest_rc = RunGuest(args, rec);
        if (rec.csv) std::fclose(rec.csv);
        if (rec.samples) std::fclose(rec.samples);
        std::printf("[bench] %s\n", guest_rc == 0 ? "PASS" : "FAIL");
        if (args.hold_ms) Sleep(args.hold_ms);
        return guest_rc;
    }
    scarlettx::D3D12Backend backend;
    LARGE_INTEGER start = Now();
    const auto init = backend.Initialize(false, false);
    const double init_ms = MsBetween(start, Now());
    if (!init.ok()) {
        std::printf("[bench] backend=D3D12 Initialize FAILED: %s\n", init.message.c_str());
        return 3;
    }
    const auto& adapter = backend.AdapterInfo();
    std::printf("[bench] backend=D3D12 adapter=\"%s\" vendor=0x%04x device=0x%04x warp=%s\n", adapter.description.c_str(),
                adapter.vendor_id, adapter.device_id, adapter.is_warp ? "yes" : "no");

    const int rc = translated ? RunTranslated(args, backend, rec, init_ms)
                   : vector ? RunVector(args, backend, rec, init_ms)
                   : cache ? RunCacheAssessment(args, backend, rec, init_ms) : RunControl(args, backend, rec, init_ms);
    if (rec.csv) std::fclose(rec.csv);
    if (rec.samples) std::fclose(rec.samples);
    std::printf("[bench] %s\n", rc == 0 ? "PASS" : "FAIL");
    if (args.hold_ms) Sleep(args.hold_ms);  // lets the driver look at what the process has loaded
    return rc;
}
