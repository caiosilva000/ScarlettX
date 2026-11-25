#pragma once

#include <cstdint>
#include <string>

#include "scarlettx/core/types.h"

namespace scarlettx {

struct EmulatorConfig {
    bool headless = true;
    bool prefer_warp = false;
    bool enable_debug_layer = false;
    bool trace = false;
    bool stats = false;
    int32_t fps_cap = -1;  // --fcap: frames per second, 0 = no cap, -1 = the title's own rate
    bool in_process = false;
    bool force_relocate = false;
    bool allow_pc_target = false;  // --allow-pc-target: also boot GDK packages built for PC
    uint32_t render_width = 320;
    uint32_t render_height = 240;
    uint32_t max_frames = 0;  // 0 = unlimited / controlled by guest
    DeviceType emulated_device = DeviceType::XboxScarlettAnaconda;
    RaytracingBackend rt_backend = RaytracingBackend::Auto;
    std::string shim_directory;
    std::string in_process_entry = "ScarlettGuestMain";
    std::string rt_dump_path;
    std::string raster_dump_path;
};

}  // namespace scarlettx
