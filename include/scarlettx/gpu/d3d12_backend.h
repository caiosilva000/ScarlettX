#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx {

struct D3D12AdapterInfo {
    std::string description;
    bool is_warp = false;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t raytracing_tier = 0;  // Matches D3D12_RAYTRACING_TIER (0, 10=1.0, 11=1.1)
    bool supports_device5 = false;
};

struct FrameBufferRgba8 {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;  // RGBA8, row-major, 4 bytes per pixel

    uint64_t ComputeFnv1aHash() const noexcept;
    Status WritePpm(const std::string& file_path) const;
};

// Offscreen probe: either a render-target clear alone, or the clear followed by an indexed draw.
enum class OffscreenProbeMode { ClearOnly, IndexedQuad };

struct OffscreenProbeReport {
    std::string failed_api;     // First D3D12/D3DCompile call that failed, empty on success.
    int32_t failed_hr = 0;      // Its HRESULT.
    uint64_t fence_signaled = 0;
    uint64_t fence_completed = 0;
    uint32_t wait_result = 0;   // WaitForSingleObject result, 0 = WAIT_OBJECT_0 (or no wait needed).
    uint32_t index_count = 0;   // Indices submitted with DrawIndexedInstanced (0 for ClearOnly).
};

// Compute probe: one compute shader, its user data as 32-bit root constants at b0 (space 0), and
// one raw buffer UAV at index 0 of a directly indexed CBV/SRV/UAV heap (SM 6.6 dynamic resources).
struct ComputeProbeDesc {
    const uint8_t* shader = nullptr;       // DXIL container
    size_t shader_size = 0;
    std::vector<uint32_t> root_constants;  // every dword of b0
    uint32_t uav_dwords = 4;               // size of the raw UAV
    uint32_t uav_fill = 0;                 // each dword's value before the dispatch
    uint32_t groups[3] = {1, 1, 1};
};

// Draw probe: a vertex and a pixel shader given as DXIL, each with its user data as 32-bit root
// constants at b0 (space 0; root parameter 0 for the vertex stage, 1 for the pixel stage), no
// vertex buffers, three vertices into an RGBA8 render target.
struct DrawProbeDesc {
    const uint8_t* vs = nullptr;
    size_t vs_size = 0;
    const uint8_t* ps = nullptr;
    size_t ps_size = 0;
    std::vector<uint32_t> vs_constants;  // may be empty: one unused dword is declared
    std::vector<uint32_t> ps_constants;
    uint32_t width = 64;
    uint32_t height = 64;
    uint32_t viewport_width = 64;        // the viewport starts at 0,0 and spans the full height
    uint8_t clear_rgba[4] = {0, 0, 0, 0};
};

class D3D12Backend {
public:
    D3D12Backend();
    ~D3D12Backend();

    D3D12Backend(const D3D12Backend&) = delete;
    D3D12Backend& operator=(const D3D12Backend&) = delete;

    Status Initialize(bool prefer_warp = true, bool enable_debug_layer = false);
    void Shutdown();
    bool IsInitialized() const noexcept;

    const D3D12AdapterInfo& AdapterInfo() const noexcept { return adapter_info_; }
    void* NativeDevice() const noexcept;
    void* NativeDevice5() const noexcept;

    // Compiles HLSL shaders via D3DCompile, renders an RGB triangle on D3D12, and reads back RGBA8.
    Status RenderTestTriangle(uint32_t width, uint32_t height, FrameBufferRgba8& out_frame);

    // Clears an RGBA8 render target to clear_rgba (exact UNORM8 values), optionally draws an
    // indexed quad with a constant colour, waits on a fence and reads every pixel back.
    Status RenderOffscreenProbe(OffscreenProbeMode mode, uint32_t width, uint32_t height,
                                const uint8_t clear_rgba[4], const uint8_t quad_rgba[4],
                                FrameBufferRgba8& out_frame, OffscreenProbeReport& report);

    // Fills the UAV, dispatches the shader once, waits on a fence and reads the whole UAV back.
    Status RunComputeProbe(const ComputeProbeDesc& desc, std::vector<uint32_t>& out_dwords,
                           OffscreenProbeReport& report);

    // Clears the target, draws three vertices with the two shaders, waits on a fence and reads
    // every pixel back.
    Status RunDrawProbe(const DrawProbeDesc& desc, FrameBufferRgba8& out_frame, OffscreenProbeReport& report);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    D3D12AdapterInfo adapter_info_{};
};

}  // namespace scarlettx
