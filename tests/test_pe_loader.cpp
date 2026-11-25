#include <algorithm>
#include "sx_check.h"
#include <cctype>
#include <filesystem>
#include <iostream>

#include "scarlettx/loader/pe_loader.h"

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

}  // namespace

int main() {
    {
        // Console targets get the DirectStorage shim; a PC target keeps its package's runtime.
        scarlettx::DllRedirectionTable table;
        std::string host;
        bool hle = false;
        SX_REQUIRE(table.Resolve("DSTORAGE.dll", host, hle) && hle && host == "dstorage_xs.dll");
        SX_REQUIRE(table.Resolve("dstoragecore.dll", host, hle) && hle && host == "dstorage_xs.dll");
        table.UsePackageDirectStorage();
        SX_REQUIRE(!table.Resolve("DSTORAGE.dll", host, hle));
        SX_REQUIRE(!table.Resolve("dstoragecore.dll", host, hle));
        SX_REQUIRE(table.Resolve("dstorage_xs.dll", host, hle) && hle && host == "dstorage_xs.dll");
        SX_REQUIRE(table.Resolve("xgameruntime.dll", host, hle) && hle);
    }
    scarlettx::DllRedirectionTable redirects;
    const std::filesystem::path rt_exe =
        std::filesystem::path(SCARLETTX_BINARY_DIR) / "packages" / "rt_showcase" / "rt_showcase.exe";
    SX_REQUIRE(std::filesystem::exists(rt_exe));

    scarlettx::PeImageInfo info;
    auto st = scarlettx::InspectPeFile(rt_exe.string(), redirects, info);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(info.is_pe64);
    SX_REQUIRE(!info.sections.empty());
    SX_REQUIRE(!info.imports.empty());

    bool found_d3d12_xs = false;
    bool found_xgr = false;
    bool found_bvh = false;
    for (const auto& mod : info.imports) {
        const std::string name = ToLower(mod.dll_name);
        if (name == "d3d12_xs.dll") {
            found_d3d12_xs = true;
            SX_REQUIRE(mod.is_scarlett_hle);
            SX_REQUIRE(!mod.symbols.empty());
        } else if (name == "xgameruntime.dll") {
            found_xgr = true;
            SX_REQUIRE(mod.is_scarlett_hle);
        } else if (name == "xboxbvhbuilder.dll") {
            found_bvh = true;
            SX_REQUIRE(mod.is_scarlett_hle);
        }
    }

    SX_REQUIRE(found_d3d12_xs);
    SX_REQUIRE(found_xgr);
    SX_REQUIRE(found_bvh);

    std::cout << "[test_pe_loader] PASSED (imports=" << info.imports.size() << ")\n";
    return 0;
}
