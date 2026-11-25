// Xbox texture memory layout: struct ABI, swizzled addressing and the xg_xs
// texture computer. Expected numbers are the console library's answers for
// the same inputs.
#include "scarlettx/gpu/xg_layout.h"
#include "scarlettx/gpu/xg_xs_abi.h"
#include "sx_check.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

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

}  // namespace

int main() {
    TestSingleMip();
    TestMipChain();
    TestRoundTrip();
    TestComputer();
    std::printf("test_xg_layout: OK\n");
    return 0;
}
