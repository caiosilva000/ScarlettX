// A lane exchange across the halves of a console wave, synthetic only, on the guest-facing
// route (a console-shaped container handed to the d3d12_xs shim's device object, dispatched
// through its command list).
//
// The program (the bridge's synthetic_lane_exchange_program) runs one group of L threads as one
// console wave. Lane l holds (l * 0x10203) ^ c0, different in every lane, and two
// ds_bpermute_b32 move the values between lanes:
//
//   dword l       the value of lane L - 1 - l   (the wave reversed)
//   dword L + l   the value of lane l ^ L/2     (the halves exchanged)
//
//   exchange32    L = 32: the control. The halves are lanes 0..15 and 16..31.
//   exchange64    L = 64: lane 31 takes lane 32's value and lane 32 takes lane 31's, and every
//                 lane reads from the other half of the wave.
//
// The expected values are calculated here, on the CPU, from the description above. Every dword
// of the buffer is checked: what the program does not write must still hold the sentinel.
//
// Each case ends in one class:
//   native_exact     exact, and the adapter reports waves of L lanes
//   emulated_exact   exact, although the adapter does not report waves of L lanes
//   unsupported      the translator or the host refused the pipeline; nothing was dispatched
//   incorrect        the pipeline was accepted and the result is wrong, or the dispatch did not
//                    complete
//   hang             a call did not return within the watchdog's time
//
// Exit codes: 0 every case is exact or unsupported, and exchange32 is exact where the adapter
// reports waves of 32; 1 a case is incorrect or hung; 2 a call failed; 3 no device; 4 a fixture
// could not be built.
#include "victory_guest_rig.h"

#include <array>
#include <string>

#include "scarlettx/gpu/victory_shader_bridge.h"

namespace {

constexpr uint32_t kDwords = 192;
constexpr uint32_t kMultiplier = 0x00010203u;
constexpr DWORD kWatchdogMs = 60000;

struct Case {
    const char* name;
    uint32_t variant;
    uint32_t lanes;
};

uint32_t LaneValue(uint32_t lane, uint32_t c0) { return (lane * kMultiplier) ^ c0; }

uint32_t Read32(const std::vector<uint8_t>& bytes, size_t at) {
    uint32_t v = 0;
    std::memcpy(&v, bytes.data() + at, 4);
    return v;
}

// The wave lane range a DXIL container says it expects: MinimumExpectedWaveLaneCount and
// MaximumExpectedWaveLaneCount of the runtime info in its PSV0 part (0 and 0xFFFFFFFF when the
// shader does not depend on the wave size).
bool ExpectedWaveLanes(const std::vector<uint8_t>& dxil, uint32_t& min, uint32_t& max) {
    if (dxil.size() < 32 || std::memcmp(dxil.data(), "DXBC", 4) != 0) return false;
    const uint32_t parts = Read32(dxil, 28);
    for (uint32_t i = 0; i < parts; ++i) {
        if (32 + 4 * static_cast<size_t>(i) + 4 > dxil.size()) return false;
        const size_t at = Read32(dxil, 32 + 4 * static_cast<size_t>(i));
        if (at + 8 > dxil.size()) return false;
        if (std::memcmp(dxil.data() + at, "PSV0", 4) != 0) continue;
        const size_t data = at + 8;
        if (data + 4 > dxil.size()) return false;
        const uint32_t info_size = Read32(dxil, data);
        if (info_size < 24 || data + 4 + info_size > dxil.size()) return false;
        min = Read32(dxil, data + 4 + 16);
        max = Read32(dxil, data + 4 + 20);
        return true;
    }
    return false;
}

// What the translated form of a fixture expects of the host's waves, from the bridge directly
// (the shim translates the same container again at pipeline creation).
void PrintExpectedWaves(const char* label, const std::vector<uint8_t>& container) {
    scarlettx::VictoryShaderInfo info;
    scarlettx::VictoryComputeBinding binding;
    std::vector<uint8_t> dxil;
    const auto outcome = scarlettx::TranslateVictoryCompute(container.data(), container.size(), info, binding, dxil);
    uint32_t min = 0;
    uint32_t max = 0;
    if (outcome != scarlettx::VictoryComputeOutcome::Translated || !ExpectedWaveLanes(dxil, min, max)) {
        std::printf("[victory-wave] %s translated: outcome=%s, no wave range read\n", label,
                    scarlettx::VictoryComputeOutcomeName(outcome));
        return;
    }
    if (min == 0 && max == 0xFFFFFFFFu) {
        std::printf("[victory-wave] %s translated: %zu bytes of DXIL, threads %u,%u,%u, expects waves of any size\n",
                    label, dxil.size(), binding.threads[0], binding.threads[1], binding.threads[2]);
    } else {
        std::printf("[victory-wave] %s translated: %zu bytes of DXIL, threads %u,%u,%u, expects waves of %u..%u lanes\n",
                    label, dxil.size(), binding.threads[0], binding.threads[1], binding.threads[2], min, max);
    }
}

struct Work {
    sxrig::GuestRig* rig;
    const Case* test;
    bool host_has_wave;
    uint32_t lanes_min;
    uint32_t lanes_max;
    const char* cls = "hang";
    int rc = 0;
};

DWORD WINAPI RunCase(void* raw) {
    Work& work = *static_cast<Work*>(raw);
    sxrig::GuestRig& rig = *work.rig;
    const Case& test = *work.test;
    const uint32_t lanes = test.lanes;

    std::vector<uint8_t> container;
    if (!scarlettx::BuildVictorySyntheticContainer(test.variant, container).ok()) {
        std::printf("[victory-wave] case=%s the fixture could not be built\n", test.name);
        work.rc = 4;
        return 0;
    }
    PrintExpectedWaves((std::string("case=") + test.name).c_str(), container);
    const auto before = sxrig::Stats();
    void* pso = nullptr;
    const HRESULT hr = rig.CreatePipeline(rig.title_root, container, &pso);
    const auto after = sxrig::Stats();
    std::printf("[victory-wave] case=%s container=%zu bytes pipeline hr=0x%08x translated=+%llu rejected=+%llu\n",
                test.name, container.size(), static_cast<uint32_t>(hr),
                static_cast<unsigned long long>(after.compute_translated - before.compute_translated),
                static_cast<unsigned long long>(after.compute_rejected - before.compute_rejected));
    if (FAILED(hr) || !pso) {
        const bool translator = after.compute_rejected != before.compute_rejected;
        std::printf("[victory-wave] case=%s refused by=%s hr=0x%08x\n", test.name,
                    translator ? "the shim (translation or binding)" : "the host (pipeline creation)",
                    static_cast<uint32_t>(hr));
        work.cls = "unsupported";
        return 0;
    }

    const std::array<uint32_t, 3> seeds = {0u, 0xDEADBEEFu, 0x80000001u};
    const UINT groups[3] = {1, 1, 1};
    uint32_t exact = 0;
    uint32_t total = 0;
    uint32_t boundary_ok = 0;
    uint32_t boundary_total = 0;
    bool completed = true;
    for (const uint32_t c0 : seeds) {
        const uint32_t constants[4] = {c0, 0, 0, 0};
        std::vector<uint32_t> out;
        if (const int rc = rig.RunDispatch(rig.title_root, pso, constants, 4, 1, groups, out)) {
            std::printf("[victory-wave] case=%s c0=%08x the dispatch did not complete (code %d)\n", test.name, c0, rc);
            completed = false;
            break;
        }
        // Expected, from the program's description alone.
        std::vector<uint32_t> want(kDwords, sxrig::kSentinel);
        for (uint32_t l = 0; l < lanes; ++l) {
            want[l] = LaneValue(lanes - 1 - l, c0);
            want[lanes + l] = LaneValue(l ^ (lanes / 2), c0);
        }
        uint32_t ok = 0;
        uint32_t sentinels_ok = 0;
        long first_bad = -1;
        for (uint32_t i = 0; i < kDwords; ++i) {
            if (out[i] == want[i]) {
                ++ok;
                if (i >= 2 * lanes) ++sentinels_ok;
            } else if (first_bad < 0) {
                first_bad = i;
            }
        }
        exact += ok;
        total += kDwords;
        // The two lanes either side of the middle of the wave, in both exchanges.
        const uint32_t lo = lanes / 2 - 1;
        const uint32_t hi = lanes / 2;
        for (const uint32_t at : {lo, hi, lanes + lo, lanes + hi}) {
            ++boundary_total;
            if (out[at] == want[at]) ++boundary_ok;
        }
        std::printf("[victory-wave] case=%s c0=%08x dwords exact %u/%u sentinels untouched %u/%u\n", test.name, c0, ok,
                    kDwords, sentinels_ok, kDwords - 2 * lanes);
        std::printf("[victory-wave] case=%s c0=%08x lane %u took %08x (lane %u holds %08x); lane %u took %08x (lane %u holds %08x)\n",
                    test.name, c0, lo, out[lo], hi, LaneValue(hi, c0), hi, out[hi], lo, LaneValue(lo, c0));
        if (first_bad >= 0) {
            std::printf("[victory-wave] case=%s c0=%08x first mismatch at dword %ld: got %08x want %08x\n", test.name, c0,
                        first_bad, out[first_bad], want[first_bad]);
            // What the reversed half looks like, so the kind of error can be read off the log.
            std::printf("[victory-wave] case=%s c0=%08x reversed, got:", test.name, c0);
            for (uint32_t l = 0; l < lanes; ++l) std::printf(" %08x", out[l]);
            std::printf("\n[victory-wave] case=%s c0=%08x reversed, want:", test.name, c0);
            for (uint32_t l = 0; l < lanes; ++l) std::printf(" %08x", want[l]);
            std::printf("\n");
        }
    }
    const bool all_exact = completed && exact == total;
    std::printf("[victory-wave] case=%s dispatches=%zu threads_per_group=%u dwords exact %u/%u boundary %u/%u\n",
                test.name, seeds.size(), lanes, exact, total, boundary_ok, boundary_total);
    work.cls = !all_exact ? "incorrect" : work.host_has_wave ? "native_exact" : "emulated_exact";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool prefer_warp = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--warp") prefer_warp = true;
        else {
            std::fprintf(stderr, "usage: test_victory_wave64_exchange [--warp]\n");
            return 64;
        }
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    sxrig::GuestRig rig;
    rig.tag = "[victory-wave]";
    if (const int rc = rig.Init(prefer_warp, kDwords, 8, 5)) return rc;

    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
    const HRESULT caps_hr =
        rig.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1));
    const bool have_caps = SUCCEEDED(caps_hr);
    const uint32_t lanes_min = have_caps ? options1.WaveLaneCountMin : 0;
    const uint32_t lanes_max = have_caps ? options1.WaveLaneCountMax : 0;
    std::printf("[victory-wave] adapter wave capability: query=0x%08x wave_ops=%s lanes_min=%u lanes_max=%u total_lanes=%u\n",
                static_cast<uint32_t>(caps_hr), have_caps && options1.WaveOps ? "yes" : "no", lanes_min, lanes_max,
                have_caps ? options1.TotalLaneCount : 0);

    // For comparison: the wave64 lane mask of the conformance test, which reads no other lane.
    {
        std::vector<uint8_t> mask64;
        if (scarlettx::BuildVictorySyntheticContainer(7, mask64).ok()) PrintExpectedWaves("reference=lanes64-mask", mask64);
    }

    const Case cases[] = {{"exchange32", 13, 32}, {"exchange64", 14, 64}};
    int failures = 0;
    for (const Case& test : cases) {
        Work work{};
        work.rig = &rig;
        work.test = &test;
        work.lanes_min = lanes_min;
        work.lanes_max = lanes_max;
        work.host_has_wave = have_caps && options1.WaveOps && lanes_min <= test.lanes && test.lanes <= lanes_max;
        // A host that takes a wave size it cannot run may never return; the case runs on its own
        // thread so that this is a result, not a stuck test.
        const HANDLE thread = CreateThread(nullptr, 0, RunCase, &work, 0, nullptr);
        SX_REQUIRE(thread != nullptr);
        const DWORD waited = WaitForSingleObject(thread, kWatchdogMs);
        const bool hung = waited != WAIT_OBJECT_0;
        if (!hung && work.rc != 0) return work.rc;
        const char* cls = hung ? "hang" : work.cls;
        std::printf("[victory-wave] case=%s lanes=%u adapter_range=%u..%u host_reports_wave=%s class=%s\n", test.name,
                    test.lanes, lanes_min, lanes_max, work.host_has_wave ? "yes" : "no", cls);
        const std::string c = cls;
        const bool bad = c == "incorrect" || c == "hang" || (work.host_has_wave && c != "native_exact");
        if (bad) ++failures;
        if (hung) {
            // The stuck call holds the device; nothing after it can be trusted.
            std::printf("[victory-wave] FAIL (%d failed)\n", failures);
            std::fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 1);
        }
        CloseHandle(thread);
    }
    std::printf("[victory-wave] %s (%d failed)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
