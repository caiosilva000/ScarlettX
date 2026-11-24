// Offscreen D3D12Backend probe: exact render-target clear, then an indexed draw.
//
//   test_d3d12_offscreen_probe clear      clear only, every pixel must equal the clear colour
//   test_d3d12_offscreen_probe indexed    clear + DrawIndexedInstanced quad, every pixel exact
//   test_d3d12_offscreen_probe            both, clear first
//   add --warp to ask for the WARP adapter first (the backend falls back to hardware)
//
// Exit codes: 0 pass, 1 pixel mismatch, 2 a D3D12 call failed (its name and HRESULT are
// printed), 3 no device. The read-back frame is written as a PPM next to the fixtures.
#include "sx_check.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#include "scarlettx/gpu/d3d12_backend.h"

namespace {

constexpr uint32_t kSize = 64;
constexpr uint8_t kClear[4] = {64, 128, 192, 255};
constexpr uint8_t kQuad[4] = {32, 224, 96, 255};

bool InsideQuad(uint32_t x, uint32_t y) {
    return x >= kSize / 4 && x < kSize * 3 / 4 && y >= kSize / 4 && y < kSize * 3 / 4;
}

int RunProbe(scarlettx::D3D12Backend& backend, scarlettx::OffscreenProbeMode mode) {
    const bool indexed = mode == scarlettx::OffscreenProbeMode::IndexedQuad;
    const char* name = indexed ? "indexed" : "clear";
    scarlettx::FrameBufferRgba8 frame;
    scarlettx::OffscreenProbeReport report;
    const auto st = backend.RenderOffscreenProbe(mode, kSize, kSize, kClear, kQuad, frame, report);

    std::printf("[offscreen:%s] fence signaled=%llu completed=%llu wait_result=%u index_count=%u\n", name,
                static_cast<unsigned long long>(report.fence_signaled),
                static_cast<unsigned long long>(report.fence_completed), report.wait_result,
                report.index_count);
    if (!st.ok()) {
        std::printf("[offscreen:%s] FAILED api=%s hr=0x%08x (%s)\n", name,
                    report.failed_api.empty() ? "?" : report.failed_api.c_str(),
                    static_cast<uint32_t>(report.failed_hr), st.message.c_str());
        std::fflush(stdout);
        return 2;
    }

    uint32_t exact = 0;
    uint32_t quad_exact = 0;
    uint32_t clear_exact = 0;
    long first_bad = -1;
    for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) {
            const uint8_t* px = &frame.pixels[(static_cast<size_t>(y) * kSize + x) * 4];
            const bool in_quad = indexed && InsideQuad(x, y);
            const uint8_t* want = in_quad ? kQuad : kClear;
            if (std::memcmp(px, want, 4) == 0) {
                ++exact;
                (in_quad ? quad_exact : clear_exact)++;
            } else if (first_bad < 0) {
                first_bad = static_cast<long>(y * kSize + x);
                std::printf("[offscreen:%s] first mismatch at (%u,%u): got %u,%u,%u,%u want %u,%u,%u,%u\n",
                            name, x, y, px[0], px[1], px[2], px[3], want[0], want[1], want[2], want[3]);
            }
        }
    }
    const uint32_t total = kSize * kSize;
    const uint32_t quad_total = indexed ? (kSize / 2) * (kSize / 2) : 0;
    std::printf("[offscreen:%s] pixels exact %u/%u (clear %u/%u, quad %u/%u) hash=0x%016llx\n", name, exact,
                total, clear_exact, total - quad_total, quad_exact, quad_total,
                static_cast<unsigned long long>(frame.ComputeFnv1aHash()));

    const auto ppm = std::filesystem::path(SCARLETTX_BINARY_DIR) /
                     (std::string("test_d3d12_offscreen_") + name + ".ppm");
    const auto wrote = frame.WritePpm(ppm.string());
    std::printf("[offscreen:%s] ppm %s %s\n", name, ppm.string().c_str(), wrote.ok() ? "written" : "NOT written");

    const bool pass = exact == total;
    std::printf("[offscreen:%s] %s\n", name, pass ? "PASS" : "FAIL");
    std::fflush(stdout);
    return pass ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    bool want_clear = true;
    bool want_indexed = true;
    bool prefer_warp = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "clear") {
            want_indexed = false;
        } else if (arg == "indexed") {
            want_clear = false;
        } else if (arg == "--warp") {
            prefer_warp = true;
        } else {
            std::fprintf(stderr, "usage: test_d3d12_offscreen_probe [clear|indexed] [--warp]\n");
            return 64;
        }
    }

    scarlettx::D3D12Backend backend;
    const auto init = backend.Initialize(prefer_warp, false);
    if (!init.ok()) {
        std::printf("[offscreen] backend=D3D12 Initialize FAILED: %s\n", init.message.c_str());
        return 3;
    }
    const auto& info = backend.AdapterInfo();
    std::printf("[offscreen] backend=D3D12 adapter=\"%s\" vendor=0x%04x device=0x%04x warp=%s requested_warp=%s\n",
                info.description.c_str(), info.vendor_id, info.device_id, info.is_warp ? "yes" : "no",
                prefer_warp ? "yes" : "no");
    std::fflush(stdout);

    int rc = 0;
    if (want_clear) {
        rc = RunProbe(backend, scarlettx::OffscreenProbeMode::ClearOnly);
        if (rc != 0) return rc;
    }
    if (want_indexed) {
        rc = RunProbe(backend, scarlettx::OffscreenProbeMode::IndexedQuad);
    }
    return rc;
}
