// Translated compute conformance, synthetic only, on the guest-facing route (console-shaped
// containers handed to the d3d12_xs shim's device object, dispatched through its command list).
//
//   groups        dispatch group ids and an offset store: Dispatch(3,2,1), one thread per group,
//                 group (x,y) writes ((c0 + x) ^ c1) + (y << 16) to dword 2 + x + 3y
//   lanes32       a wave32 lane mask: 32 threads, lane l writes l + c2 where l < c0 (selected
//                 through EXEC with s_and_saveexec), c1 elsewhere
//   lanes32count  the same, plus the number of selected lanes (s_bcnt1 of the mask, a
//                 cross-lane value) at dword 32
//   lanes64       the lane mask as a wave64 program: 64 threads, 64-bit masks
//
// Every dword of a 96-dword buffer is checked: what a case does not write must still hold the
// sentinel. The adapter's wave capability is printed, and each lane case says whether the host
// could have run it as real waves of that size.
//
// A lane case whose pipeline the host refuses is an explicit SKIP (the host cannot run that wave
// size); `groups` and `lanes32` must pass. Exit codes: 0 pass, 1 a required check failed,
// 2 a call failed, 3 no device, 4 a fixture could not be built.
#include "victory_guest_rig.h"

#include <array>
#include <functional>
#include <string>

#include "scarlettx/gpu/victory_shader_bridge.h"

namespace {

constexpr uint32_t kDwords = 96;
using Expect = std::function<void(const uint32_t c[4], std::vector<uint32_t>& want)>;

struct Case {
    const char* name;
    uint32_t variant;
    UINT groups[3];
    uint32_t lanes;      // threads per group the program declares
    bool required;       // a host refusal is a failure, not a skip
    std::vector<std::array<uint32_t, 4>> inputs;
    Expect expect;
};

}  // namespace

int main(int argc, char** argv) {
    bool prefer_warp = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--warp") prefer_warp = true;
        else {
            std::fprintf(stderr, "usage: test_victory_compute_conformance [--warp]\n");
            return 64;
        }
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const Expect lane_mask = [](const uint32_t c[4], std::vector<uint32_t>& want) {
        for (uint32_t l = 0; l < 32; ++l) want[l] = l < c[0] ? l + c[2] : c[1];
    };
    const std::vector<std::array<uint32_t, 4>> lane_inputs32 = {
        {0, 0x11110000u, 0x100, 0}, {13, 0x22220000u, 0x200, 0}, {32, 0x33330000u, 0xFFFFFFF0u, 0},
        {40, 0x44440000u, 7, 0},    {1, 0x55550000u, 0, 0},
    };
    std::vector<Case> cases;
    cases.push_back({"groups", 4, {3, 2, 1}, 1, true,
                     {{1, 2, 0, 0}, {0xFFFFFFFFu, 0x0F0F0F0Fu, 0, 0}, {0x12345678u, 0xFFFFFFFFu, 0, 0}},
                     [](const uint32_t c[4], std::vector<uint32_t>& want) {
                         for (uint32_t y = 0; y < 2; ++y) {
                             for (uint32_t x = 0; x < 3; ++x) want[2 + x + 3 * y] = ((c[0] + x) ^ c[1]) + (y << 16);
                         }
                     }});
    cases.push_back({"lanes32", 5, {1, 1, 1}, 32, true, lane_inputs32, lane_mask});
    cases.push_back({"lanes32count", 6, {1, 1, 1}, 32, false, lane_inputs32,
                     [lane_mask](const uint32_t c[4], std::vector<uint32_t>& want) {
                         lane_mask(c, want);
                         want[32] = c[0] < 32 ? c[0] : 32;
                     }});
    cases.push_back({"lanes64", 7, {1, 1, 1}, 64, false,
                     {{0, 0x11110000u, 0x100, 0}, {13, 0x22220000u, 0x200, 0}, {37, 0x33330000u, 0x300, 0},
                      {64, 0x44440000u, 0x400, 0}, {100, 0x55550000u, 5, 0}},
                     [](const uint32_t c[4], std::vector<uint32_t>& want) {
                         for (uint32_t l = 0; l < 64; ++l) want[l] = l < c[0] ? l + c[2] : c[1];
                     }});

    sxrig::GuestRig rig;
    rig.tag = "[victory-conf]";
    if (const int rc = rig.Init(prefer_warp, kDwords, 8, 5)) return rc;

    // The adapter's wave capability, from the device itself.
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
    const HRESULT caps_hr =
        rig.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1));
    const bool have_caps = SUCCEEDED(caps_hr);
    std::printf("[victory-conf] adapter wave capability: query=0x%08x wave_ops=%s lanes_min=%u lanes_max=%u total_lanes=%u\n",
                static_cast<uint32_t>(caps_hr), have_caps && options1.WaveOps ? "yes" : "no",
                have_caps ? options1.WaveLaneCountMin : 0, have_caps ? options1.WaveLaneCountMax : 0,
                have_caps ? options1.TotalLaneCount : 0);

    int failures = 0;
    for (const auto& test : cases) {
        std::vector<uint8_t> container;
        if (!scarlettx::BuildVictorySyntheticContainer(test.variant, container).ok()) {
            std::printf("[victory-conf] case=%s the fixture could not be built\n", test.name);
            return 4;
        }
        const bool host_has_wave = have_caps && options1.WaveOps && options1.WaveLaneCountMin <= test.lanes &&
                                   test.lanes <= options1.WaveLaneCountMax;
        const auto before = sxrig::Stats();
        void* pso = nullptr;
        const HRESULT hr = rig.CreatePipeline(rig.title_root, container, &pso);
        const auto after = sxrig::Stats();
        std::printf("[victory-conf] case=%s container=%zu bytes pipeline hr=0x%08x translated=+%llu rejected=+%llu\n",
                    test.name, container.size(), static_cast<uint32_t>(hr),
                    static_cast<unsigned long long>(after.compute_translated - before.compute_translated),
                    static_cast<unsigned long long>(after.compute_rejected - before.compute_rejected));
        if (FAILED(hr) || !pso) {
            const bool translator_refused = after.compute_rejected != before.compute_rejected &&
                                            hr == static_cast<HRESULT>(0x80004001u);
            if (test.required || translator_refused) {
                std::printf("[victory-conf] case=%s verdict=FAIL (%s)\n", test.name,
                            translator_refused ? "the translator refused the program" : "the host refused a required pipeline");
                ++failures;
            } else {
                std::printf("[victory-conf] case=%s verdict=SKIP (the host refused the pipeline: it does not run "
                            "waves of %u lanes; adapter range %u..%u)\n",
                            test.name, test.lanes, options1.WaveLaneCountMin, options1.WaveLaneCountMax);
            }
            continue;
        }
        uint32_t exact = 0;
        uint32_t total = 0;
        uint32_t dispatches = 0;
        for (const auto& input : test.inputs) {
            std::vector<uint32_t> out;
            if (const int rc = rig.RunDispatch(rig.title_root, pso, input.data(), 4, 1, test.groups, out)) return rc;
            ++dispatches;
            std::vector<uint32_t> want(kDwords, sxrig::kSentinel);
            test.expect(input.data(), want);
            uint32_t ok = 0;
            long first_bad = -1;
            for (uint32_t i = 0; i < kDwords; ++i) {
                if (out[i] == want[i]) ++ok;
                else if (first_bad < 0) first_bad = i;
            }
            exact += ok;
            total += kDwords;
            if (first_bad >= 0) {
                std::printf("[victory-conf] case=%s in=%08x,%08x,%08x %u/%u exact, first mismatch at dword %ld: got %08x want %08x\n",
                            test.name, input[0], input[1], input[2], ok, kDwords, first_bad, out[first_bad], want[first_bad]);
            } else {
                std::printf("[victory-conf] case=%s in=%08x,%08x,%08x %u/%u exact\n", test.name, input[0], input[1],
                            input[2], ok, kDwords);
            }
        }
        const bool pass = exact == total;
        std::printf("[victory-conf] case=%s dispatches=%u groups=%u,%u,%u threads_per_group=%u dwords exact %u/%u verdict=%s\n",
                    test.name, dispatches, test.groups[0], test.groups[1], test.groups[2], test.lanes, exact, total,
                    pass ? "PASS" : "FAIL");
        if (test.lanes > 1) {
            // Whether this could have been a real wave of the console's size on this host.
            std::printf("[victory-conf] case=%s wave: console wave of %u lanes; host %s (adapter range %u..%u)%s\n",
                        test.name, test.lanes,
                        host_has_wave ? "reports waves of that size" : "does not report waves of that size",
                        options1.WaveLaneCountMin, options1.WaveLaneCountMax,
                        pass && !host_has_wave ? "; the exact result is the translation's per-thread form, not a genuine wave of that size" : "");
        }
        if (!pass) ++failures;
    }
    std::printf("[victory-conf] %s (%d failed)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
