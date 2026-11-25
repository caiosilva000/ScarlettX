// Synthetic title that renders SCARLETTX_MAX_FRAMES frames (default 2) and then stalls.
#include <cstdlib>
#include <iostream>
#include <string>

#include <windows.h>

#include "scarlettx/gpu/d3d12_xs_abi.h"

int main() {
    if (XGameRuntimeInitialize() != 0) {
        std::cerr << "[stall_probe] XGameRuntimeInitialize failed\n";
        return 1;
    }

    const uint32_t device_type = XSystemGetDeviceType();
    uint64_t xuid = 0;
    if (XUserGetDefaultXuid(&xuid) != 0 || xuid == 0) {
        std::cerr << "[stall_probe] XUserGetDefaultXuid failed\n";
        return 2;
    }

    XTaskQueueHandle task_queue = nullptr;
    if (XTaskQueueCreate(XTaskQueueDispatchMode_Manual,
                         XTaskQueueDispatchMode_Manual,
                         &task_queue) != XGR_S_OK ||
        task_queue == nullptr) {
        std::cerr << "[stall_probe] XTaskQueueCreate failed\n";
        return 3;
    }

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    params.PreferWarp = 1;
    uint64_t d3d_dev = 0;
    if (D3D12XboxCreateSyntheticDevice(&params, &d3d_dev) != 0) {
        std::cerr << "[stall_probe] D3D12XboxCreateSyntheticDevice failed\n";
        return 4;
    }

    D3D12XBOX_CAPABILITIES caps{};
    D3D12XboxQueryCapabilities(d3d_dev, &caps);
    std::cout << "[stall_probe] DeviceType=" << device_type
              << " XUID=0x" << std::hex << xuid << std::dec
              << " Adapter=\"" << caps.AdapterName << "\""
              << " DXR_Tier=" << caps.RaytracingTier << "\n";

    uint32_t frames = 2;
    if (const char* env_frames = std::getenv("SCARLETTX_MAX_FRAMES")) {
        frames = static_cast<uint32_t>(std::strtoul(env_frames, nullptr, 10));
        if (frames == 0) frames = 1;
    }
    const char* raster_dump = std::getenv("SCARLETTX_RASTER_DUMP");

    uint64_t first_hash = 0;
    for (uint32_t f = 0; f < frames; ++f) {
        uint64_t token = 0;
        if (D3D12XboxWaitFrameEventX(d3d_dev, &token) != 0) {
            return 5;
        }
        uint64_t hash = 0;
        const char* dump = (f == 0) ? raster_dump : nullptr;
        if (D3D12XboxRenderTestTriangle(d3d_dev, 256, 256, dump, &hash) != 0) {
            std::cerr << "[stall_probe] D3D12XboxRenderTestTriangle failed\n";
            return 6;
        }
        if (f == 0) {
            first_hash = hash;
        } else if (hash != first_hash) {
            std::cerr << "[stall_probe] Non-deterministic frame hash!\n";
            return 7;
        }

        D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
        plane.Token = token;
        plane.Width = 256;
        plane.Height = 256;
        if (D3D12XboxPresentX(d3d_dev, &plane) != 0) {
            return 8;
        }
        std::cout << "[stall_probe] Frame " << f << " token=" << token
                  << " hash=0x" << std::hex << hash << std::dec << "\n";
    }

    // The point of this fixture: after its frames it stops making progress and never exits, as a
    // title stuck in a wait does. A runner has to end it by its timeout.
    std::cout << "[stall_probe] frames done; waiting forever" << std::endl;
    for (;;) Sleep(1000);
}
