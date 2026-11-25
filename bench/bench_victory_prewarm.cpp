// Opt-in benchmark: does preparing a shader and its pipeline ahead of time shorten the first use?
//
// One process runs one workload under one condition and appends its timings to two CSV files.
// A driver (tools/sx_bench_prewarm.py) launches it many times.
//
//   --workload compute    translated compute: synthetic GFX10 -> victory-shader bridge -> DXIL ->
//                         D3D12Backend probe session (backend path, not the shim's guest route)
//   --workload draw       translated draw: synthetic GFX10 vertex + pixel programs -> bridge ->
//                         DXIL -> D3D12Backend probe session (backend path)
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

#include "scarlettx/gpu/d3d12_backend.h"
#include "scarlettx/gpu/victory_shader_bridge.h"

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
        else if (a == "--hold-ms") args.hold_ms = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else {
            std::fprintf(stderr, "bench_victory_prewarm: unknown argument %s\n", a.c_str());
            return 64;
        }
    }
    const bool translated = args.workload == "compute" || args.workload == "draw";
    const bool known_condition = args.condition == "cold" || args.condition == "prewarmed" ||
                                 args.condition == "primed" || args.condition == "steady";
    if ((!translated && args.workload != "control") || !known_condition || args.iterations == 0) {
        std::fprintf(stderr, "usage: bench_victory_prewarm --workload compute|draw|control --condition "
                             "cold|prewarmed|primed|steady [--iterations N] [--salt S] [--launch N] [--series NAME] "
                             "[--csv FILE] [--samples FILE] [--hold-ms MS]\n");
        return 64;
    }
    const char* shader_path =
        args.workload == "compute" ? "translated compute: GFX10 -> victory-shader bridge -> DXIL -> D3D12Backend (backend path)"
        : args.workload == "draw" ? "translated draw: GFX10 VS+PS -> victory-shader bridge -> DXIL -> D3D12Backend (backend path)"
                                  : "control: HLSL -> d3dcompiler -> D3D12Backend indexed draw, pipeline built per call";
    Recorder rec{args, shader_path};
    if (!args.csv.empty()) rec.csv = std::fopen(args.csv.c_str(), "a");
    if (!args.samples.empty()) rec.samples = std::fopen(args.samples.c_str(), "a");

    std::printf("[bench] workload=%s condition=%s series=%s launch=%u salt=%u iterations=%u\n", args.workload.c_str(),
                args.condition.c_str(), args.series.c_str(), args.launch, args.salt, args.iterations);
    std::printf("[bench] shader_path=%s\n", shader_path);
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

    const int rc = translated ? RunTranslated(args, backend, rec, init_ms) : RunControl(args, backend, rec, init_ms);
    if (rec.csv) std::fclose(rec.csv);
    if (rec.samples) std::fclose(rec.samples);
    std::printf("[bench] %s\n", rc == 0 ? "PASS" : "FAIL");
    if (args.hold_ms) Sleep(args.hold_ms);  // lets the driver look at what the process has loaded
    return rc;
}
