// Xbox texture memory layout: struct ABI, swizzled addressing and the xg_xs
// texture computer. Expected numbers are the console library's answers for
// the same inputs.
#include "scarlettx/gpu/xg_layout.h"
#include "scarlettx/gpu/xg_xs_abi.h"
#include "sx_check.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace xg = scarlettx::gpu::xg;

namespace {

XG_TEXTURE2D_DESC Desc(uint32_t w, uint32_t h, uint32_t mips, uint32_t format, uint32_t array = 1) {
    XG_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = mips;
    d.ArraySize = array;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.BindFlags = 0x8;
    d.SwizzleMode = XG_SWIZZLE_MODE_4KB_S;
    return d;
}

uint64_t Offset(const XG_RESOURCE_LAYOUT& l, uint32_t mip, uint32_t slice, uint32_t x, uint32_t y) {
    uint64_t off = ~0ull;
    SX_REQUIRE(xg::TexelElementOffset(l, mip, slice, x, y, &off));
    return off;
}

constexpr uint32_t kRgba8 = 28;
constexpr uint32_t kBc1 = 71;
constexpr uint32_t kBc3 = 77;
constexpr uint32_t kR8 = 61;

void TestSingleMip() {
    auto l = std::make_unique<XG_RESOURCE_LAYOUT>();
    xg::ComputeTexture2DLayout(Desc(700, 284, 1, kRgba8), l.get());
    SX_REQUIRE(l->SizeBytes == 0xC6000);
    SX_REQUIRE(l->BaseAlignmentBytes == 0x1000);
    SX_REQUIRE(l->MipLevels == 1 && l->Planes == 1);
    const XG_PLANE_LAYOUT& p = l->Plane[0];
    SX_REQUIRE(p.Usage == 1 && p.SizeBytes == 0xC6000 && p.Reserved10 == 0xC6000);
    SX_REQUIRE(p.BytesPerElement == 4 && p.Reserved2C == 1 && p.Reserved30 == 0);
    const XG_MIPLEVEL_LAYOUT& m = p.MipLayout[0];
    SX_REQUIRE(m.SizeBytes == 0xC6000 && m.OffsetBytes == 0 && m.Slice2DSizeBytes == 0xC6000);
    SX_REQUIRE(m.PitchPixels == 704 && m.PitchBytes == 2816 && m.AlignmentBytes == 0x1000);
    SX_REQUIRE(m.PaddedWidthElements == 704 && m.PaddedHeightElements == 288);
    SX_REQUIRE(m.WidthElements == 700 && m.HeightElements == 284);
    SX_REQUIRE(m.ReservedWidth == 700 && m.ReservedHeight == 284);
    SX_REQUIRE(m.SwizzleMode == XG_SWIZZLE_MODE_4KB_S && m.Reserved60 == 1 && m.Reserved64 == 0);

    // 32x32 texel blocks of 4 KB, stored row by row (22 blocks per row).
    SX_REQUIRE(Offset(*l, 0, 0, 0, 0) == 0);
    SX_REQUIRE(Offset(*l, 0, 0, 1, 0) == 0x4);
    SX_REQUIRE(Offset(*l, 0, 0, 2, 0) == 0x8);
    SX_REQUIRE(Offset(*l, 0, 0, 4, 0) == 0x80);
    SX_REQUIRE(Offset(*l, 0, 0, 8, 0) == 0x200);
    SX_REQUIRE(Offset(*l, 0, 0, 16, 0) == 0x800);
    SX_REQUIRE(Offset(*l, 0, 0, 32, 0) == 0x1000);
    SX_REQUIRE(Offset(*l, 0, 0, 0, 1) == 0x10);
    SX_REQUIRE(Offset(*l, 0, 0, 0, 4) == 0x40);
    SX_REQUIRE(Offset(*l, 0, 0, 0, 8) == 0x100);
    SX_REQUIRE(Offset(*l, 0, 0, 0, 16) == 0x400);
    SX_REQUIRE(Offset(*l, 0, 0, 0, 32) == 0x16000);
    SX_REQUIRE(Offset(*l, 0, 0, 0, 256) == 0xB0000);

    // A single mip is never packed into a tail, however small.
    xg::ComputeTexture2DLayout(Desc(16, 16, 1, kRgba8), l.get());
    SX_REQUIRE(l->SizeBytes == 0x1000 && l->Plane[0].Reserved2C == 1 && l->Plane[0].Reserved30 == 0);
}

void TestMipChain() {
    auto l = std::make_unique<XG_RESOURCE_LAYOUT>();
    xg::ComputeTexture2DLayout(Desc(256, 256, 9, kRgba8), l.get());
    const XG_PLANE_LAYOUT& p = l->Plane[0];
    SX_REQUIRE(l->SizeBytes == 0x56000);
    SX_REQUIRE(p.Reserved2C == 4 && p.Reserved30 == 5);
    // Smallest mip first: the tail block, then mips 3, 2, 1, 0.
    const uint64_t offsets[9] = {0x16000, 0x6000, 0x2000, 0x1000, 0x800, 0x600, 0x500, 0x400, 0x300};
    const uint64_t sizes[9] = {0x40000, 0x10000, 0x4000, 0x1000, 0x800, 0x200, 0x100, 0x100, 0x100};
    for (uint32_t m = 0; m < 9; ++m) {
        SX_REQUIRE(p.MipLayout[m].OffsetBytes == offsets[m]);
        SX_REQUIRE(p.MipLayout[m].SizeBytes == sizes[m]);
        SX_REQUIRE(p.MipLayout[m].Reserved64 == (m >= 4 ? 1u : 0u));
        SX_REQUIRE(p.MipLayout[m].SwizzleMode == (m >= 4 ? 1u : 5u));
    }
    SX_REQUIRE(p.MipLayout[4].PaddedWidthElements == 16 && p.MipLayout[4].PaddedHeightElements == 32);
    SX_REQUIRE(p.MipLayout[5].PaddedWidthElements == 8 && p.MipLayout[5].PaddedHeightElements == 16);
    SX_REQUIRE(p.MipLayout[6].PaddedWidthElements == 8 && p.MipLayout[6].PaddedHeightElements == 8);
    SX_REQUIRE(Offset(*l, 4, 0, 0, 0) == 0x800);
    SX_REQUIRE(Offset(*l, 4, 0, 15, 15) == 0xBFC);
    SX_REQUIRE(Offset(*l, 5, 0, 0, 1) == 0x610);
    SX_REQUIRE(Offset(*l, 5, 0, 7, 7) == 0x6FC);
    SX_REQUIRE(Offset(*l, 6, 0, 3, 3) == 0x53C);

    // Array slices repeat the whole chain.
    xg::ComputeTexture2DLayout(Desc(256, 256, 9, kRgba8, 2), l.get());
    SX_REQUIRE(l->SizeBytes == 0xAC000 && l->Plane[0].Reserved10 == 0x56000);
    SX_REQUIRE(Offset(*l, 0, 1, 0, 0) == 0x6C000);
    SX_REQUIRE(Offset(*l, 8, 1, 0, 0) == 0x56300);

    // Other element sizes.
    xg::ComputeTexture2DLayout(Desc(256, 256, 9, kBc1), l.get());
    SX_REQUIRE(l->SizeBytes == 0xB000 && l->Plane[0].Reserved2C == 2 && l->Plane[0].Reserved30 == 7);
    SX_REQUIRE(l->Plane[0].MipLayout[0].OffsetBytes == 0x3000 && l->Plane[0].MipLayout[0].PitchPixels == 256);
    xg::ComputeTexture2DLayout(Desc(256, 256, 9, kBc3), l.get());
    SX_REQUIRE(l->SizeBytes == 0x16000 && l->Plane[0].Reserved2C == 3);
    xg::ComputeTexture2DLayout(Desc(256, 256, 9, kR8), l.get());
    SX_REQUIRE(l->SizeBytes == 0x16000 && l->Plane[0].Reserved2C == 3);

    // Odd sizes: the tail test uses the power-of-two size, padding rounds up.
    xg::ComputeTexture2DLayout(Desc(33, 17, 3, kRgba8), l.get());
    SX_REQUIRE(l->SizeBytes == 0x4000 && l->Plane[0].Reserved2C == 2 && l->Plane[0].Reserved30 == 1);
    xg::ComputeTexture2DLayout(Desc(16, 32, 3, kRgba8), l.get());
    SX_REQUIRE(l->SizeBytes == 0x1000 && l->Plane[0].Reserved2C == 0 && l->Plane[0].Reserved30 == 3);
    xg::ComputeTexture2DLayout(Desc(32, 16, 3, kRgba8), l.get());
    SX_REQUIRE(l->SizeBytes == 0x2000 && l->Plane[0].Reserved2C == 1 && l->Plane[0].Reserved30 == 2);
}

void RoundTrip(const XG_TEXTURE2D_DESC& d) {
    auto l = std::make_unique<XG_RESOURCE_LAYOUT>();
    xg::ComputeTexture2DLayout(d, l.get());
    SX_REQUIRE(xg::IsAddressable(*l));
    const uint32_t bpe = l->Plane[0].BytesPerElement;
    std::vector<uint8_t> memory(static_cast<size_t>(l->SizeBytes), 0);
    std::vector<uint32_t> owner(memory.size(), 0);  // which element wrote each byte
    uint32_t id = 0;
    std::vector<std::vector<uint8_t>> sources;
    for (uint32_t slice = 0; slice < d.ArraySize; ++slice) {
        for (uint32_t mip = 0; mip < l->MipLevels; ++mip) {
            const XG_MIPLEVEL_LAYOUT& m = l->Plane[0].MipLayout[mip];
            const uint32_t pitch = m.WidthElements * bpe + 12;  // caller-chosen row pitch
            std::vector<uint8_t> src(static_cast<size_t>(pitch) * m.HeightElements);
            for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>(i * 7 + mip * 31 + slice * 101 + 1);
            SX_REQUIRE(xg::CopyIntoSubresource(*l, memory.data(), mip, slice, src.data(), pitch));
            // No two elements of any subresource may share memory, and every element must be
            // where the addressing says it is (the copy moves runs of elements, not single ones).
            for (uint32_t y = 0; y < m.HeightElements; ++y) {
                for (uint32_t x = 0; x < m.WidthElements; ++x) {
                    const uint64_t off = Offset(*l, mip, slice, x, y);
                    SX_REQUIRE(off + bpe <= memory.size());
                    SX_REQUIRE(std::memcmp(memory.data() + off, src.data() + static_cast<size_t>(y) * pitch + x * bpe, bpe) == 0);
                    ++id;
                    for (uint32_t b = 0; b < bpe; ++b) {
                        SX_REQUIRE(owner[off + b] == 0);
                        owner[off + b] = id;
                    }
                }
            }
            sources.push_back(std::move(src));
        }
    }
    size_t index = 0;
    for (uint32_t slice = 0; slice < d.ArraySize; ++slice) {
        for (uint32_t mip = 0; mip < l->MipLevels; ++mip, ++index) {
            const XG_MIPLEVEL_LAYOUT& m = l->Plane[0].MipLayout[mip];
            const uint32_t pitch = m.WidthElements * bpe + 12;
            std::vector<uint8_t> out(sources[index].size(), 0);
            SX_REQUIRE(xg::CopyFromSubresource(*l, memory.data(), mip, slice, out.data(), pitch));
            for (uint32_t y = 0; y < m.HeightElements; ++y) {
                SX_REQUIRE(std::memcmp(out.data() + static_cast<size_t>(y) * pitch,
                                       sources[index].data() + static_cast<size_t>(y) * pitch,
                                       m.WidthElements * bpe) == 0);
            }
        }
    }
}

void TestRoundTrip() {
    RoundTrip(Desc(700, 284, 1, kRgba8));
    RoundTrip(Desc(100, 60, 7, kRgba8));
    RoundTrip(Desc(256, 256, 9, kBc1));
    RoundTrip(Desc(64, 64, 7, kBc3, 3));
    RoundTrip(Desc(131, 77, 8, kR8));
    RoundTrip(Desc(90, 50, 7, 49));   // R8G8: two bytes an element
    RoundTrip(Desc(70, 33, 7, 10));   // R16G16B16A16_FLOAT: eight bytes an element
    RoundTrip(Desc(256, 256, 1, kRgba8));
    RoundTrip(Desc(40, 24, 6, kR8, 2));
    RoundTrip(Desc(128, 64, 8, 49));  // R8G8
    RoundTrip(Desc(48, 80, 7, 10));   // R16G16B16A16_FLOAT
}

// The texture computer as a title uses it: through its vtable.
template <typename Fn>
Fn Slot(void* object, int index) {
    void** vtable = *reinterpret_cast<void***>(object);
    return reinterpret_cast<Fn>(vtable[index]);
}

void TestComputer() {
    // Sampled textures, render targets and depth buffers.
    SX_REQUIRE(XGComputeOptimalSwizzleMode(3, kRgba8, 700, 284, 1, 1, 0x8, 0) == 5);
    SX_REQUIRE(XGComputeOptimalSwizzleMode(3, kBc3, 1024, 1024, 1, 1, 0x0, 0) == 5);
    SX_REQUIRE(XGComputeOptimalSwizzleMode(3, kRgba8, 1920, 1080, 1, 1, 0x28, 0) == 27);
    SX_REQUIRE(XGComputeOptimalSwizzleMode(3, 40, 1920, 1080, 1, 1, 0x40, 0) == 24);

    // The description exactly as the title lays it out in memory.
    uint32_t raw[16] = {};
    raw[0] = 256;   // +0x00 width
    raw[1] = 256;   // +0x04 height
    raw[2] = 9;     // +0x08 mip levels
    raw[3] = 1;     // +0x0C array size
    raw[4] = kRgba8;  // +0x10 format
    raw[5] = 1;     // +0x14 sample count
    raw[8] = 0x8;   // +0x20 bind flags
    raw[11] = 5;    // +0x2C swizzle mode
    void* computer = nullptr;
    SX_REQUIRE(XGCreateTexture2DComputer(reinterpret_cast<const XG_TEXTURE2D_DESC*>(raw), &computer) == 0);
    SX_REQUIRE(computer != nullptr);

    using RefFn = uint32_t (SCARLETTX_XG_CALL*)(void*);
    using LayoutFn = int32_t (SCARLETTX_XG_CALL*)(void*, void*);
    using SizeFn = uint64_t (SCARLETTX_XG_CALL*)(void*);
    using MipFn = uint64_t (SCARLETTX_XG_CALL*)(void*, uint32_t, uint32_t);
    using TexelFn = uint64_t (SCARLETTX_XG_CALL*)(void*, uint32_t, uint32_t, uint64_t, uint32_t, uint32_t, uint32_t);

    // Slot 2 is the layout query; the object must survive it.
    std::vector<uint8_t> out(0x2000, 0xCD);
    SX_REQUIRE(Slot<LayoutFn>(computer, 2)(computer, out.data()) == 0);
    auto rd32 = [&](size_t off) { uint32_t v; std::memcpy(&v, out.data() + off, 4); return v; };
    auto rd64 = [&](size_t off) { uint64_t v; std::memcpy(&v, out.data() + off, 8); return v; };
    SX_REQUIRE(rd64(0x00) == 0x56000);        // resource size
    SX_REQUIRE(rd64(0x08) == 0x1000);         // base alignment
    SX_REQUIRE(rd32(0x10) == 9 && rd32(0x14) == 1);
    SX_REQUIRE(rd32(0x40) == 4);              // bytes per element
    SX_REQUIRE(rd32(0x44) == 4);              // first mip of the tail
    SX_REQUIRE(rd64(0x50) == 0x40000);        // mip 0 size
    SX_REQUIRE(rd64(0x58) == 0x16000);        // mip 0 offset
    SX_REQUIRE(rd32(0x50 + 0x38) == 256 && rd32(0x50 + 0x3C) == 256);
    SX_REQUIRE(rd32(0x50 + 0x64) == 0);
    SX_REQUIRE(rd64(0x50 + 0x70 + 0x08) == 0x6000);        // mip 1 offset
    SX_REQUIRE(rd32(0x50 + 4 * 0x70 + 0x64) == 1);         // mip 4 is in the tail
    SX_REQUIRE(rd64(0x50 + 4 * 0x70 + 0x08) == 0x800);

    SX_REQUIRE(Slot<SizeFn>(computer, 3)(computer) == 0x56000);
    SX_REQUIRE(Slot<SizeFn>(computer, 4)(computer) == 0x1000);
    SX_REQUIRE(Slot<MipFn>(computer, 5)(computer, 0, 1) == 0x6000);
    SX_REQUIRE(Slot<TexelFn>(computer, 6)(computer, 0, 0, 1, 1, 0, 0) == 0x16014);
    SX_REQUIRE(Slot<TexelFn>(computer, 6)(computer, 0, 4, 15, 15, 0, 0) == 0xBFC);

    SX_REQUIRE(Slot<RefFn>(computer, 0)(computer) == 2);  // AddRef
    SX_REQUIRE(Slot<RefFn>(computer, 1)(computer) == 1);  // Release
    SX_REQUIRE(Slot<RefFn>(computer, 1)(computer) == 0);

    // A mode without texel addressing: every (mip, slice) still gets its own
    // memory inside the resource.
    raw[0] = 64;
    raw[1] = 64;
    raw[2] = 2;
    raw[3] = 2;
    raw[11] = 0;
    void* linear = nullptr;
    SX_REQUIRE(XGCreateTexture2DComputer(reinterpret_cast<const XG_TEXTURE2D_DESC*>(raw), &linear) == 0);
    const uint64_t linear_size = Slot<SizeFn>(linear, 3)(linear);
    struct Range {
        uint64_t first, last;
    } ranges[4];
    int n = 0;
    for (uint32_t slice = 0; slice < 2; ++slice) {
        for (uint32_t mip = 0; mip < 2; ++mip) {
            const uint32_t dim = 64u >> mip;
            ranges[n].first = Slot<TexelFn>(linear, 6)(linear, 0, mip, 0, 0, slice, 0);
            ranges[n].last = Slot<TexelFn>(linear, 6)(linear, 0, mip, dim - 1, dim - 1, slice, 0) + 4;
            SX_REQUIRE(ranges[n].first < ranges[n].last && ranges[n].last <= linear_size);
            ++n;
        }
    }
    for (int a = 0; a < 4; ++a) {
        for (int b = a + 1; b < 4; ++b) {
            SX_REQUIRE(ranges[a].last <= ranges[b].first || ranges[b].last <= ranges[a].first);
        }
    }
    SX_REQUIRE(Slot<RefFn>(linear, 1)(linear) == 0);
}

// ---------------------------------------------------------------------------------------------
// Parity with a reference xg_xs.dll, and what ScarlettX reports for mip tails.
//
// The descriptions are generated here: a mip chain that crosses into the packed tail, arrays of
// several slices with a mip tail, and single-mip controls. For each, the layout ScarlettX reports
// is printed (tail boundary, and per mip its mode, offset and the two per-mip fields a title
// reads), and every subresource is written with its own pattern, checked for overlap with every
// other, and read back.
//
// With SCARLETTX_XG_REFERENCE_DLL set to a reference library (Microsoft's offline PC xg_xs.dll,
// which is not in this repository and must stay local), the same descriptions are given to it
// through the same exported entry and vtable slot a title uses, and the two layouts are compared
// dword by dword, mismatches listed by field. Without it the external comparison is BLOCKED and
// says so; nothing is assumed about what the reference would answer.
struct ParityCase {
    const char* name;
    XG_TEXTURE2D_DESC desc;
};

std::vector<ParityCase> ParityCases() {
    return {
        {"single-mip control 256x256 RGBA8", Desc(256, 256, 1, kRgba8)},
        {"single-mip control 700x284 RGBA8", Desc(700, 284, 1, kRgba8)},
        {"mip chain into the tail 256x256x9 RGBA8", Desc(256, 256, 9, kRgba8)},
        {"mip chain into the tail 100x60x7 RGBA8", Desc(100, 60, 7, kRgba8)},
        {"mip chain into the tail 256x256x9 BC1", Desc(256, 256, 9, kBc1)},
        {"array of 2 with a mip tail 256x256x9 RGBA8", Desc(256, 256, 9, kRgba8, 2)},
        {"array of 3 with a mip tail 64x64x7 BC3", Desc(64, 64, 7, kBc3, 3)},
        {"array of 4 with a mip tail 128x128x8 RGBA8", Desc(128, 128, 8, kRgba8, 4)},
    };
}

// What a field at `offset` of XG_RESOURCE_LAYOUT is, for the mismatch table.
std::string FieldName(size_t offset) {
    char text[96];
    const size_t plane0 = offsetof(XG_RESOURCE_LAYOUT, Plane);
    const size_t mips0 = plane0 + offsetof(XG_PLANE_LAYOUT, MipLayout);
    if (offset < plane0) {
        static const char* const kHead[] = {"SizeBytes", "SizeBytes+4", "BaseAlignmentBytes", "BaseAlignmentBytes+4", "MipLevels", "Planes"};
        return offset / 4 < 6 ? kHead[offset / 4] : "resource header";
    }
    if (offset >= sizeof(XG_PLANE_LAYOUT) + plane0) {
        std::snprintf(text, sizeof(text), "beyond plane 0 (+0x%zx)", offset);
        return text;
    }
    if (offset < mips0) {
        std::snprintf(text, sizeof(text), "plane 0 +0x%02zx%s", offset - plane0,
                      offset - plane0 == offsetof(XG_PLANE_LAYOUT, Reserved2C) ? " (first tail mip)"
                      : offset - plane0 == offsetof(XG_PLANE_LAYOUT, Reserved30) ? " (tail mips)" : "");
        return text;
    }
    const size_t mip = (offset - mips0) / sizeof(XG_MIPLEVEL_LAYOUT);
    const size_t in = (offset - mips0) % sizeof(XG_MIPLEVEL_LAYOUT);
    const char* what = in == 0x00 ? "SizeBytes" : in == 0x08 ? "OffsetBytes" : in == 0x10 ? "Slice2DSizeBytes"
                       : in == 0x18 ? "PitchPixels" : in == 0x1C ? "PitchBytes" : in == 0x20 ? "AlignmentBytes"
                       : in == 0x24 ? "PaddedWidthElements" : in == 0x28 ? "PaddedHeightElements"
                       : in == 0x30 ? "WidthElements" : in == 0x34 ? "HeightElements"
                       : in == 0x48 ? "SwizzleMode" : in == 0x60 ? "Reserved60" : in == 0x64 ? "Reserved64 (in tail)" : "";
    std::snprintf(text, sizeof(text), "mip %zu +0x%02zx %s", mip, in, what);
    return text;
}

void TestParity() {
    using CreateFn = int32_t (SCARLETTX_XG_CALL*)(const XG_TEXTURE2D_DESC*, void**);
    using LayoutFn = int32_t (SCARLETTX_XG_CALL*)(void*, void*);
    using RefFn = uint32_t (SCARLETTX_XG_CALL*)(void*);
    CreateFn reference = nullptr;
#if defined(_WIN32)
    const char* path = std::getenv("SCARLETTX_XG_REFERENCE_DLL");
    if (path && path[0]) {
        const HMODULE module = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module) {
            std::printf("[xg-parity] external parity: BLOCKED (LoadLibrary failed for the reference library, error %lu)\n", GetLastError());
        } else {
            reference = reinterpret_cast<CreateFn>(reinterpret_cast<void*>(GetProcAddress(module, "XGCreateTexture2DComputer")));
            WIN32_FILE_ATTRIBUTE_DATA info{};
            GetFileAttributesExA(path, GetFileExInfoStandard, &info);
            std::printf("[xg-parity] reference library: %s, %lu bytes%s (record its version and SHA-256 beside this log)\n", path,
                        info.nFileSizeLow, reference ? "" : ", but it has no XGCreateTexture2DComputer");
            if (!reference) std::printf("[xg-parity] external parity: BLOCKED (the library does not export the entry)\n");
        }
    } else {
        std::printf("[xg-parity] external parity: BLOCKED (no reference library; set SCARLETTX_XG_REFERENCE_DLL to a local one)\n");
    }
#else
    std::printf("[xg-parity] external parity: BLOCKED (not a Windows build)\n");
#endif

    uint32_t mismatches = 0;
    uint32_t compared = 0;
    for (const ParityCase& c : ParityCases()) {
        auto ours = std::make_unique<XG_RESOURCE_LAYOUT>();
        void* computer = nullptr;
        SX_REQUIRE(XGCreateTexture2DComputer(&c.desc, &computer) == 0 && computer);
        std::memset(ours.get(), 0, sizeof(*ours));
        SX_REQUIRE(Slot<LayoutFn>(computer, 2)(computer, ours.get()) == 0);
        Slot<RefFn>(computer, 1)(computer);
        const XG_PLANE_LAYOUT& p = ours->Plane[0];
        std::printf("[xg-parity] case \"%s\": size 0x%llx, mips %u, slices %u, slice stride 0x%llx, first tail mip %u, tail mips %u\n",
                    c.name, static_cast<unsigned long long>(ours->SizeBytes), ours->MipLevels, c.desc.ArraySize,
                    static_cast<unsigned long long>(p.Reserved10), p.Reserved2C, p.Reserved30);
        for (uint32_t m = 0; m < ours->MipLevels; ++m) {
            const XG_MIPLEVEL_LAYOUT& mip = p.MipLayout[m];
            std::printf("[xg-parity]   mip %u: mode %u offset 0x%llx size 0x%llx padded %ux%u in_tail %u reserved60 %u\n", m,
                        mip.SwizzleMode, static_cast<unsigned long long>(mip.OffsetBytes),
                        static_cast<unsigned long long>(mip.SizeBytes), mip.PaddedWidthElements, mip.PaddedHeightElements,
                        mip.Reserved64, mip.Reserved60);
        }
        // Every subresource written with its own pattern, none overlapping another, all read back.
        RoundTrip(c.desc);
        std::printf("[xg-parity]   round trip: %u subresources written, checked for overlap and read back exactly\n",
                    ours->MipLevels * c.desc.ArraySize);
        if (!reference) continue;
        // The reference may write past our structure (records we do not model): give it room.
        std::vector<uint8_t> theirs(0x4000, 0);
        void* ref_computer = nullptr;
        if (reference(&c.desc, &ref_computer) != 0 || !ref_computer) {
            std::printf("[xg-parity]   reference refused this description\n");
            ++mismatches;
            continue;
        }
        Slot<LayoutFn>(ref_computer, 2)(ref_computer, theirs.data());
        Slot<RefFn>(ref_computer, 1)(ref_computer);
        ++compared;
        const size_t used = offsetof(XG_RESOURCE_LAYOUT, Plane) + offsetof(XG_PLANE_LAYOUT, MipLayout) +
                            ours->MipLevels * sizeof(XG_MIPLEVEL_LAYOUT);
        uint32_t here = 0;
        for (size_t at = 0; at + 4 <= used; at += 4) {
            uint32_t a = 0, b = 0;
            std::memcpy(&a, reinterpret_cast<const uint8_t*>(ours.get()) + at, 4);
            std::memcpy(&b, theirs.data() + at, 4);
            if (a == b) continue;
            ++here;
            std::printf("[xg-parity]   MISMATCH +0x%04zx %-40s scarlettx 0x%08x reference 0x%08x\n", at, FieldName(at).c_str(), a, b);
        }
        // What the reference wrote beyond the records ScarlettX models (tiling records live there).
        size_t beyond = 0;
        for (size_t at = sizeof(XG_RESOURCE_LAYOUT); at < theirs.size(); ++at) beyond += theirs[at] != 0;
        std::printf("[xg-parity]   compared %zu bytes: %u mismatching dwords; reference wrote %zu non-zero bytes beyond ScarlettX's structure\n",
                    used, here, beyond);
        mismatches += here;
    }
    if (reference) {
        std::printf("[xg-parity] external parity: %s (%u descriptions compared, %u mismatching dwords)\n",
                    mismatches == 0 ? "PASS" : "FAIL", compared, mismatches);
        SX_REQUIRE(mismatches == 0);
    }
}

}  // namespace

int main() {
    TestSingleMip();
    TestMipChain();
    TestRoundTrip();
    TestComputer();
    TestParity();
    std::printf("test_xg_layout: OK\n");
    return 0;
}
