// A translated draw, synthetic only: a hand-assembled GFX10 vertex program and pixel program
// are translated to DXIL by the victory-shader bridge and drawn offscreen by D3D12Backend, and
// every pixel is read back.
//
// The vertex program makes a triangle covering the viewport from the vertex id (no vertex
// buffers); the pixel program writes its four root constants to render target 0. The viewport
// is the left half of a 64x64 target, so the left half must be the drawn colour and the right
// half the clear colour: a vertex program that put the triangle elsewhere, or a pixel program
// that wrote something else, shows.
//
// This is the backend path (the bridge called directly). Graphics pipelines are not translated
// on the d3d12_xs shim's guest-facing route yet.
//
// Exit codes: 0 pass, 1 pixel mismatch, 2 a D3D12 call failed (name and HRESULT printed),
// 3 no device, 4 translation failed, 5 the shaders need bindings this test does not make.
#include "sx_check.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "scarlettx/gpu/d3d12_backend.h"
#include "scarlettx/gpu/victory_shader_bridge.h"

namespace {

constexpr uint32_t kSize = 64;
constexpr uint8_t kClear[4] = {16, 32, 48, 255};

uint32_t FloatBits(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    return bits;
}

bool PlainBinding(const scarlettx::VictoryComputeBinding& b) {
    return !b.root_cbv_count && !b.root_srv_count && !b.root_uav_count && !b.sampler_count &&
           !b.static_sampler_count && !b.flags;
}

}  // namespace

int main(int argc, char** argv) {
    bool prefer_warp = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--warp") prefer_warp = true;
        else {
            std::fprintf(stderr, "usage: test_victory_draw_e2e [--warp]\n");
            return 64;
        }
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    scarlettx::VictoryShaderInfo info;
    scarlettx::VictoryComputeBinding vs_binding;
    scarlettx::VictoryComputeBinding ps_binding;
    std::vector<uint8_t> vs;
    std::vector<uint8_t> ps;
    const auto translated = scarlettx::TranslateVictorySyntheticDraw(info, vs_binding, vs, ps_binding, ps);
    if (!translated.ok()) {
        std::printf("[victory-draw] translate FAILED: %s\n", translated.message.c_str());
        return 4;
    }
    std::printf("[victory-draw] translated: vs=%zu bytes ps=%zu bytes stage_mask=0x%x\n", vs.size(), ps.size(),
                info.stage_mask);
    std::printf("[victory-draw] binding: vs user_data_dwords=%u entries=%zu, ps user_data_dwords=%u entries=%zu\n",
                vs_binding.user_data_dwords, vs_binding.user_data.size(), ps_binding.user_data_dwords,
                ps_binding.user_data.size());
    SX_REQUIRE(info.stage_mask == ((1u << 4) | (1u << 5)));  // vertex and pixel
    if (!PlainBinding(vs_binding) || !PlainBinding(ps_binding) || !vs_binding.user_data.empty() ||
        ps_binding.user_data_dwords != 4 || ps_binding.user_data.size() != 4) {
        std::printf("[victory-draw] FAIL: the shaders ask for bindings beyond the pixel stage's root constants\n");
        return 5;
    }
    for (const auto& u : ps_binding.user_data) {
        if (u.root != 0 || u.slot != 0 || u.dword >= 4 || u.index >= 4) return 5;
    }

    scarlettx::D3D12Backend backend;
    const auto init = backend.Initialize(prefer_warp, false);
    if (!init.ok()) {
        std::printf("[victory-draw] backend=D3D12 Initialize FAILED: %s\n", init.message.c_str());
        return 3;
    }
    const auto& adapter = backend.AdapterInfo();
    std::printf("[victory-draw] backend=D3D12 adapter=\"%s\" vendor=0x%04x device=0x%04x warp=%s requested_warp=%s\n",
                adapter.description.c_str(), adapter.vendor_id, adapter.device_id, adapter.is_warp ? "yes" : "no",
                prefer_warp ? "yes" : "no");

    // Colours as exact UNORM8 values; several, so a constant output cannot pass.
    const uint8_t colours[][4] = {{32, 224, 96, 255}, {255, 0, 128, 64}, {1, 2, 3, 4}, {200, 100, 50, 25}};
    int rc = 0;
    uint32_t exact_total = 0;
    uint32_t pixel_total = 0;
    for (const auto& colour : colours) {
        scarlettx::DrawProbeDesc desc;
        desc.vs = vs.data();
        desc.vs_size = vs.size();
        desc.ps = ps.data();
        desc.ps_size = ps.size();
        desc.ps_constants.assign(ps_binding.user_data_dwords, 0);
        for (const auto& u : ps_binding.user_data) {
            desc.ps_constants[u.index] = FloatBits(static_cast<float>(colour[u.dword]) / 255.0f);
        }
        desc.width = kSize;
        desc.height = kSize;
        desc.viewport_width = kSize / 2;
        std::memcpy(desc.clear_rgba, kClear, 4);

        scarlettx::FrameBufferRgba8 frame;
        scarlettx::OffscreenProbeReport report;
        const auto st = backend.RunDrawProbe(desc, frame, report);
        std::printf("[victory-draw] fence signaled=%llu completed=%llu wait_result=%u vertices=%u\n",
                    static_cast<unsigned long long>(report.fence_signaled),
                    static_cast<unsigned long long>(report.fence_completed), report.wait_result, report.index_count);
        if (!st.ok()) {
            std::printf("[victory-draw] FAILED api=%s hr=0x%08x (%s)\n",
                        report.failed_api.empty() ? "?" : report.failed_api.c_str(),
                        static_cast<uint32_t>(report.failed_hr), st.message.c_str());
            return 2;
        }
        uint32_t drawn_exact = 0;
        uint32_t clear_exact = 0;
        long first_bad = -1;
        for (uint32_t y = 0; y < kSize; ++y) {
            for (uint32_t x = 0; x < kSize; ++x) {
                const uint8_t* px = &frame.pixels[(static_cast<size_t>(y) * kSize + x) * 4];
                const bool drawn = x < kSize / 2;
                const uint8_t* want = drawn ? colour : kClear;
                if (std::memcmp(px, want, 4) == 0) {
                    (drawn ? drawn_exact : clear_exact)++;
                } else if (first_bad < 0) {
                    first_bad = static_cast<long>(y * kSize + x);
                    std::printf("[victory-draw] first mismatch at (%u,%u): got %u,%u,%u,%u want %u,%u,%u,%u\n", x, y,
                                px[0], px[1], px[2], px[3], want[0], want[1], want[2], want[3]);
                }
            }
        }
        const uint32_t half = kSize * kSize / 2;
        std::printf("[victory-draw] colour %u,%u,%u,%u: drawn half exact %u/%u, untouched half exact %u/%u hash=0x%016llx\n",
                    colour[0], colour[1], colour[2], colour[3], drawn_exact, half, clear_exact, half,
                    static_cast<unsigned long long>(frame.ComputeFnv1aHash()));
        exact_total += drawn_exact + clear_exact;
        pixel_total += kSize * kSize;
        if (drawn_exact != half || clear_exact != half) rc = 1;
    }
    std::printf("[victory-draw] pixels exact %u/%u\n", exact_total, pixel_total);
    std::printf("[victory-draw] %s\n", rc == 0 ? "PASS" : "FAIL");
    return rc;
}
