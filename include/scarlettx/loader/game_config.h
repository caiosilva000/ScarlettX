#pragma once

#include <string>
#include <vector>

#include "scarlettx/core/status.h"
#include "scarlettx/core/types.h"

namespace scarlettx {

struct GameExecutableEntry {
    std::string name;
    std::string id;
    std::string target_device_family;
    std::string alias;
    bool is_dev_only = false;
};

struct GameConfig {
    std::string identity_name;
    std::string publisher;
    std::string version;
    std::string title_id;
    std::string display_name;
    std::string config_version;
    ConsoleTarget console_target = ConsoleTarget::Unknown;
    // True when the config names no TargetDeviceFamily and console_target was
    // taken from the executable's imports instead (see InferConsoleTargetFromImports).
    bool target_inferred = false;
    std::vector<GameExecutableEntry> executables;
    std::string source_path;

    bool IsNativeScarlett() const noexcept {
        return console_target == ConsoleTarget::XboxGen9Scarlett;
    }

    // A GDK package built for PC. It links the desktop d3d12.dll, not d3d12_xs.dll,
    // so only the loader and the non-graphics shims take part in running it.
    bool IsPcDesktop() const noexcept {
        return console_target == ConsoleTarget::PcDesktop;
    }

    const GameExecutableEntry* PrimaryExecutable() const noexcept;
};

// For a config that names no TargetDeviceFamily, which the GDK allows: the
// platform the executable was built for, read from the graphics DLLs it imports.
// Scarlett titles link the *_xs.dll libraries, Xbox One titles the *_x.dll ones,
// and a desktop GDK build links the plain d3d12.dll / dxgi.dll. Unknown when the
// imports show none of these.
ConsoleTarget InferConsoleTargetFromImports(const std::vector<std::string>& import_dll_names);

Status ParseGameConfigXml(const std::string& xml_content, GameConfig& out_config);
Status LoadGameConfigFile(const std::string& file_path, GameConfig& out_config);

}  // namespace scarlettx
