#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx {

struct PeSectionInfo {
    std::string name;
    uint32_t virtual_address = 0;
    uint32_t virtual_size = 0;
    uint32_t raw_data_offset = 0;
    uint32_t raw_data_size = 0;
    uint32_t characteristics = 0;
};

struct PeImportSymbol {
    bool by_ordinal = false;
    uint16_t ordinal = 0;
    std::string name;
};

struct PeImportModule {
    std::string dll_name;
    std::string redirected_target;
    bool is_scarlett_hle = false;
    std::vector<PeImportSymbol> symbols;
};

struct PeImageInfo {
    std::string file_path;
    bool is_pe64 = false;
    uint16_t machine = 0;
    uint16_t subsystem = 0;
    uint64_t image_base = 0;
    uint32_t entry_point_rva = 0;
    uint32_t size_of_image = 0;
    std::vector<PeSectionInfo> sections;
    std::vector<PeImportModule> imports;
};

class DllRedirectionTable {
public:
    DllRedirectionTable();

    void RegisterRedirect(const std::string& guest_dll, const std::string& host_dll, bool is_hle);
    bool Resolve(const std::string& guest_dll, std::string& out_host_dll, bool& out_is_hle) const;

    // A PC (desktop GDK) title is built against the desktop DirectStorage interfaces and
    // ships that runtime (dstorage.dll, dstoragecore.dll) in its package. The dstorage_xs
    // shim implements the console interface, so for a PC target these two names are left
    // to the package's own DLLs. dstorage_xs.dll itself stays redirected.
    void UsePackageDirectStorage();

private:
    struct Entry {
        std::string host_dll;
        bool is_hle = false;
    };
    std::unordered_map<std::string, Entry> entries_;
};

Status InspectPeFile(const std::string& pe_path,
                     const DllRedirectionTable& redirects,
                     PeImageInfo& out_info);

}  // namespace scarlettx
