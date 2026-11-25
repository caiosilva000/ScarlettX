// End to end, synthetic only: a hand-assembled GFX10 (RDNA2) compute program is translated to
// DXIL by the pinned victory-shader bridge, dispatched through D3D12Backend, and its output
// buffer read back.
//
// The program (built inside the bridge from instruction encoders, no title data) runs one
// thread: out[0] = ((c0 + c1) ^ c2) + c3, the constants being its four root constants and
// `out` the raw UAV at heap index 0.
//
//   test_victory_compute_e2e            hardware adapter
//   test_victory_compute_e2e --warp     ask for WARP first (the backend falls back to hardware)
//
// Exit codes: 0 pass, 1 readback mismatch, 2 a D3D12 call failed (its name and HRESULT are
// printed), 3 no device, 4 translation failed, 5 the shader needs bindings this test does not make.
#include "sx_check.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "scarlettx/gpu/d3d12_backend.h"
#include "scarlettx/gpu/victory_shader_bridge.h"

namespace {

constexpr uint32_t kUavDwords = 4;
constexpr uint32_t kSentinel = 0xA5A5A5A5u;
constexpr uint32_t kRootConstants = 0;
constexpr uint32_t kRootTable = 1;

uint32_t Expected(const uint32_t c[4]) { return ((c[0] + c[1]) ^ c[2]) + c[3]; }

}  // namespace

int main(int argc, char** argv) {
    bool prefer_warp = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--warp") {
            prefer_warp = true;
        } else {
            std::fprintf(stderr, "usage: test_victory_compute_e2e [--warp]\n");
            return 64;
        }
    }

    // 1. GFX10 -> DXIL.
    scarlettx::VictoryShaderInfo info;
    scarlettx::VictoryComputeBinding binding;
    std::vector<uint8_t> dxil;
    const auto translated = scarlettx::TranslateVictorySyntheticCompute(info, binding, dxil);
    if (!translated.ok()) {
        std::printf("[victory-compute] translate FAILED: %s\n", translated.message.c_str());
        return 4;
    }
    std::printf("[victory-compute] translated: dxil=%zu bytes magic=%.4s stage_mask=0x%x threads=%u,%u,%u\n",
                dxil.size(), dxil.size() >= 4 ? reinterpret_cast<const char*>(dxil.data()) : "????",
                info.stage_mask, binding.threads[0], binding.threads[1], binding.threads[2]);
    std::printf("[victory-compute] binding: user_data_dwords=%u entries=%zu root_cbv=%u root_srv=%u root_uav=%u "
                "samplers=%u static_samplers=%u flags=0x%x\n",
                binding.user_data_dwords, binding.user_data.size(), binding.root_cbv_count,
                binding.root_srv_count, binding.root_uav_count, binding.sampler_count,
                binding.static_sampler_count, binding.flags);
    for (const auto& u : binding.user_data) {
        std::printf("[victory-compute]   user_data[%u] <- root slot %u type %u dword %u\n", u.index, u.slot,
                    u.root, u.dword);
    }
    SX_REQUIRE(dxil.size() > 32 && std::string(reinterpret_cast<const char*>(dxil.data()), 4) == "DXBC");
    SX_REQUIRE(info.stage_mask == (1u << 7));  // compute

    // 2. The binding must be one this test can make: b0 root constants and one heap view.
    if (binding.root_cbv_count || binding.root_srv_count || binding.root_uav_count || binding.sampler_count ||
        binding.static_sampler_count || binding.flags || binding.user_data_dwords == 0 ||
        binding.threads[0] != 1 || binding.threads[1] != 1 || binding.threads[2] != 1) {
        std::printf("[victory-compute] FAIL: the shader asks for bindings beyond root constants and a heap view\n");
        return 5;
    }
    uint32_t constants_seen = 0;
    uint32_t tables_seen = 0;
    for (const auto& u : binding.user_data) {
        if (u.index >= binding.user_data_dwords) return 5;
        if (u.root == kRootConstants && u.slot == 0 && u.dword < 4) {
            constants_seen |= 1u << u.dword;
        } else if (u.root == kRootTable && u.slot == 1 && u.dword == 0) {
            ++tables_seen;
        } else {
            std::printf("[victory-compute] FAIL: unexpected user-data entry\n");
            return 5;
        }
    }
    SX_REQUIRE(constants_seen == 0xF);
    SX_REQUIRE(tables_seen == 1);

    scarlettx::D3D12Backend backend;
    const auto init = backend.Initialize(prefer_warp, false);
    if (!init.ok()) {
        std::printf("[victory-compute] backend=D3D12 Initialize FAILED: %s\n", init.message.c_str());
        return 3;
    }
    const auto& adapter = backend.AdapterInfo();
    std::printf("[victory-compute] backend=D3D12 adapter=\"%s\" vendor=0x%04x device=0x%04x warp=%s requested_warp=%s\n",
                adapter.description.c_str(), adapter.vendor_id, adapter.device_id, adapter.is_warp ? "yes" : "no",
                prefer_warp ? "yes" : "no");
    std::fflush(stdout);

    // 3. Dispatch and read back, several inputs so a constant result cannot pass.
    const uint32_t cases[][4] = {
        {1, 2, 3, 4},
        {0x12345678u, 0x9ABCDEF0u, 0x0F0F0F0Fu, 0x11111111u},
        {0xFFFFFFFFu, 1, 0, 0},                    // the add wraps to 0
        {0, 0, 0xFFFFFFFFu, 1},                    // the xor gives all ones, the add wraps to 0
        {0xDEADBEEFu, 0x00000001u, 0xCAFEF00Du, 0x80000000u},
    };
    uint32_t exact = 0;
    uint32_t total = 0;
    int rc = 0;
    for (const auto& c : cases) {
        scarlettx::ComputeProbeDesc desc;
        desc.shader = dxil.data();
        desc.shader_size = dxil.size();
        desc.root_constants.assign(binding.user_data_dwords, 0);
        for (const auto& u : binding.user_data) {
            // A table's dword is the byte offset of its first descriptor in the heap: index 0.
            desc.root_constants[u.index] = u.root == kRootConstants ? c[u.dword] : 0;
        }
        desc.uav_dwords = kUavDwords;
        desc.uav_fill = kSentinel;

        std::vector<uint32_t> out;
        scarlettx::OffscreenProbeReport report;
        const auto st = backend.RunComputeProbe(desc, out, report);
        std::printf("[victory-compute] fence signaled=%llu completed=%llu wait_result=%u\n",
                    static_cast<unsigned long long>(report.fence_signaled),
                    static_cast<unsigned long long>(report.fence_completed), report.wait_result);
        if (!st.ok()) {
            std::printf("[victory-compute] FAILED api=%s hr=0x%08x (%s)\n",
                        report.failed_api.empty() ? "?" : report.failed_api.c_str(),
                        static_cast<uint32_t>(report.failed_hr), st.message.c_str());
            std::fflush(stdout);
            return 2;
        }
        // Dword 0 is the result; the shader must leave the other dwords alone.
        const uint32_t want[kUavDwords] = {Expected(c), kSentinel, kSentinel, kSentinel};
        bool ok = out.size() == kUavDwords;
        for (uint32_t i = 0; ok && i < kUavDwords; ++i) ok = out[i] == want[i];
        for (uint32_t i = 0; i < kUavDwords; ++i) {
            ++total;
            if (i < out.size() && out[i] == want[i]) ++exact;
        }
        std::printf("[victory-compute] in=%08x,%08x,%08x,%08x out=%08x,%08x,%08x,%08x want=%08x %s\n", c[0], c[1],
                    c[2], c[3], out.size() > 0 ? out[0] : 0, out.size() > 1 ? out[1] : 0,
                    out.size() > 2 ? out[2] : 0, out.size() > 3 ? out[3] : 0, want[0], ok ? "exact" : "MISMATCH");
        if (!ok) rc = 1;
    }
    std::printf("[victory-compute] dwords exact %u/%u\n", exact, total);
    std::printf("[victory-compute] %s\n", rc == 0 ? "PASS" : "FAIL");
    std::fflush(stdout);
    return rc;
}
