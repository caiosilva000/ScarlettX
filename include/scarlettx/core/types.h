#pragma once

#include <cstddef>
#include <cstdint>

namespace scarlettx {

using GuestVirtualAddress = uint64_t;
using GpuVirtualAddress = uint64_t;
using FramePipelineToken = uint64_t;
using XHandle = uint64_t;

enum class ConsoleTarget : uint32_t {
    Unknown = 0,
    XboxOne = 1,
    XboxOneGen9Aware = 2,
    XboxGen9Scarlett = 3,
    PcDesktop = 4,  // GDK package built for PC (TargetDeviceFamily="PC")
};

enum class DeviceType : uint32_t {
    Unknown = 0,
    Pc = 1,
    XboxOne = 2,
    XboxOneS = 3,
    XboxOneX = 4,
    XboxOneXDevkit = 5,
    XboxScarlettLockhart = 6,   // Xbox Series S
    XboxScarlettAnaconda = 7,   // Xbox Series X
    XboxScarlettDevkit = 8,
};

enum class RaytracingBackend : uint32_t {
    Auto = 0,
    HardwareDxr = 1,
    SoftwareBvh = 2,
};

}  // namespace scarlettx
