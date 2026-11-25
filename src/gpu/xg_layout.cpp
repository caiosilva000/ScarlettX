// ScarlettX — Xbox texture memory layout (swizzled texel addressing).
#include "scarlettx/gpu/xg_layout.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace scarlettx::gpu::xg {
namespace {

constexpr uint32_t kMode4KbS = XG_SWIZZLE_MODE_4KB_S;
constexpr uint32_t kMode256BS = 1;
constexpr uint32_t kBlockBytes = 4096;
constexpr uint32_t kMaxMips = 15;

// Source of each offset bit inside a 4 KB block: 'x'/'y' and the coordinate bit.
struct BitSource {
    char axis;
    uint8_t bit;
};
struct BlockEquation {
    uint32_t bytes_per_element;
    BitSource bits[12];
};

constexpr BitSource N{0, 0};
#define SX_X(n) BitSource{'x', n}
#define SX_Y(n) BitSource{'y', n}
constexpr BlockEquation kEquations[] = {
    {1, {SX_X(0), SX_X(1), SX_X(2), SX_X(3), SX_Y(0), SX_Y(1), SX_Y(2), SX_Y(3), SX_Y(4), SX_X(4), SX_Y(5), SX_X(5)}},
    {2, {N, SX_X(0), SX_X(1), SX_X(2), SX_Y(0), SX_Y(1), SX_Y(2), SX_X(3), SX_Y(3), SX_X(4), SX_Y(4), SX_X(5)}},
    {4, {N, N, SX_X(0), SX_X(1), SX_Y(0), SX_Y(1), SX_Y(2), SX_X(2), SX_Y(3), SX_X(3), SX_Y(4), SX_X(4)}},
    {8, {N, N, N, SX_X(0), SX_Y(0), SX_Y(1), SX_X(1), SX_X(2), SX_Y(2), SX_X(3), SX_Y(3), SX_X(4)}},
    {16, {N, N, N, N, SX_Y(0), SX_Y(1), SX_X(0), SX_X(1), SX_Y(2), SX_X(2), SX_Y(3), SX_X(3)}},
};
#undef SX_X
#undef SX_Y

// Offsets of the mips packed into the tail block, in 256-byte units.
constexpr uint32_t kTailOffset256[8] = {8, 6, 5, 4, 3, 2, 1, 0};

const BlockEquation* FindEquation(uint32_t bytes_per_element) {
    for (const auto& eq : kEquations) {
        if (eq.bytes_per_element == bytes_per_element) return &eq;
    }
    return nullptr;
}

// Number of coordinate bits of `axis` among offset bits below `limit`.
uint32_t AxisBits(const BlockEquation& eq, char axis, uint32_t limit) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < limit && i < 12; ++i) {
        if (eq.bits[i].axis == axis) ++n;
    }
    return n;
}

uint32_t AxisOffset(const BlockEquation& eq, char axis, uint32_t coord) {
    uint32_t off = 0;
    for (uint32_t i = 0; i < 12; ++i) {
        if (eq.bits[i].axis == axis && ((coord >> eq.bits[i].bit) & 1u)) off |= 1u << i;
    }
    return off;
}

// Coordinate whose in-block offset is `offset`.
uint32_t AxisOrigin(const BlockEquation& eq, char axis, uint32_t offset) {
    uint32_t coord = 0;
    for (uint32_t i = 0; i < 12; ++i) {
        if (eq.bits[i].axis == axis && ((offset >> i) & 1u)) coord |= 1u << eq.bits[i].bit;
    }
    return coord;
}

uint32_t Log2(uint32_t v) {
    uint32_t n = 0;
    while ((1u << n) < v) ++n;
    return n;
}

uint32_t AlignUp32(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

uint32_t TailRegionBytes(uint32_t slot) { return slot == 0 ? 0x800u : (slot == 1 ? 0x200u : 0x100u); }

uint32_t BlockBytesForMode(uint32_t mode) {
    if (mode == 0) return 256;
    if (mode <= 3) return 256;
    if (mode <= 7 || (mode >= 20 && mode <= 23)) return 4096;
    return 65536;
}

struct MipView {
    const XG_MIPLEVEL_LAYOUT* ml = nullptr;
    const BlockEquation* eq = nullptr;
    uint32_t block_w = 0, block_h = 0;
    uint32_t origin_x = 0, origin_y = 0;  // position inside the tail block
    uint32_t pitch_blocks = 0;
    uint64_t base = 0;
};

bool ViewMip(const XG_RESOURCE_LAYOUT& layout, uint32_t mip, uint32_t slice, MipView* view) {
    if (!IsAddressable(layout) || mip >= layout.MipLevels || mip >= kMaxMips) return false;
    const XG_PLANE_LAYOUT& plane = layout.Plane[0];
    const XG_MIPLEVEL_LAYOUT& ml = plane.MipLayout[mip];
    if (slice >= std::max<uint32_t>(1, ml.DepthOrArraySize)) return false;
    const BlockEquation* eq = FindEquation(plane.BytesPerElement);
    view->ml = &ml;
    view->eq = eq;
    view->block_w = 1u << AxisBits(*eq, 'x', 12);
    view->block_h = 1u << AxisBits(*eq, 'y', 12);
    view->base = static_cast<uint64_t>(slice) * plane.Reserved10;
    if (ml.Reserved64) {
        const uint32_t in_block = static_cast<uint32_t>(ml.OffsetBytes) & (kBlockBytes - 1);
        view->origin_x = AxisOrigin(*eq, 'x', in_block);
        view->origin_y = AxisOrigin(*eq, 'y', in_block);
        view->pitch_blocks = 1;
        view->base += ml.OffsetBytes - in_block;
    } else {
        view->pitch_blocks = ml.PaddedWidthElements / view->block_w;
        view->base += ml.OffsetBytes;
    }
    return true;
}

uint64_t XOffset(const MipView& v, uint32_t x) {
    x += v.origin_x;
    return static_cast<uint64_t>(x / v.block_w) * kBlockBytes + AxisOffset(*v.eq, 'x', x % v.block_w);
}
uint64_t YOffset(const MipView& v, uint32_t y) {
    y += v.origin_y;
    return static_cast<uint64_t>(y / v.block_h) * v.pitch_blocks * kBlockBytes +
           AxisOffset(*v.eq, 'y', y % v.block_h);
}

// Copies a subresource between linear rows and resource memory. Along x the block equations keep
// 16 bytes together (four 4-byte elements, two 8-byte ones, ...), so a row is a list of runs
// that are contiguous on both sides; each run is one copy, and the common 16-byte run a
// fixed-size one. Element-by-element copying cost about 4 ns an element.
bool CopySubresource(const XG_RESOURCE_LAYOUT& layout, uint32_t mip, uint32_t slice, uint8_t* resource,
                     uint8_t* linear, uint32_t linear_pitch, bool into_resource) {
    MipView v;
    if (!ViewMip(layout, mip, slice, &v)) return false;
    const uint32_t bpe = layout.Plane[0].BytesPerElement;
    const uint32_t w = v.ml->WidthElements;
    const uint32_t h = v.ml->HeightElements;
    struct Run {
        uint64_t resource_offset;
        uint32_t linear_offset;
        uint32_t bytes;
    };
    std::vector<Run> runs;
    for (uint32_t x = 0; x < w;) {
        const uint64_t start = XOffset(v, x);
        uint32_t n = 1;
        while (x + n < w && XOffset(v, x + n) == start + static_cast<uint64_t>(n) * bpe) ++n;
        runs.push_back({start, x * bpe, n * bpe});
        x += n;
    }
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* res_row = resource + v.base + YOffset(v, y);
        uint8_t* lin_row = linear + static_cast<uint64_t>(y) * linear_pitch;
        for (const Run& r : runs) {
            uint8_t* res = res_row + r.resource_offset;
            uint8_t* lin = lin_row + r.linear_offset;
            if (r.bytes == 16) {
                if (into_resource) std::memcpy(res, lin, 16);
                else std::memcpy(lin, res, 16);
            } else if (into_resource) {
                std::memcpy(res, lin, r.bytes);
            } else {
                std::memcpy(lin, res, r.bytes);
            }
        }
    }
    return true;
}

}  // namespace

uint32_t FormatBytesPerElement(uint32_t dxgi_format) {
    // Bytes per pixel, or per 4x4 block for block-compressed formats.
    switch (dxgi_format) {
        case 1: case 2: case 3: case 4:
            return 16;  // R32G32B32A32
        case 5: case 6: case 7: case 8:
            return 12;  // R32G32B32
        case 9: case 10: case 11: case 12: case 13: case 14:
        case 15: case 16: case 17: case 18:
        case 19: case 20: case 21: case 22:
            return 8;  // R16G16B16A16, R32G32, R32G8X24
        case 48: case 49: case 50: case 51: case 52:
        case 53: case 54: case 55: case 56: case 57: case 58: case 59:
        case 85: case 86: case 115:
            return 2;  // R8G8, R16, B5G6R5, B5G5R5A1, B4G4R4A4
        case 60: case 61: case 62: case 63: case 64: case 65:
            return 1;  // R8, A8
        case 70: case 71: case 72:  // BC1
        case 79: case 80: case 81:  // BC4
            return 8;
        case 73: case 74: case 75:  // BC2
        case 76: case 77: case 78:  // BC3
        case 82: case 83: case 84:  // BC5
        case 94: case 95: case 96:  // BC6H
        case 97: case 98: case 99:  // BC7
            return 16;
        default:
            return 4;  // 32-bit formats (R8G8B8A8, B8G8R8A8, R10G10B10A2, R32, D24S8, ...)
    }
}

bool IsBlockCompressed(uint32_t dxgi_format) {
    return (dxgi_format >= 70 && dxgi_format <= 84) || (dxgi_format >= 94 && dxgi_format <= 99);
}

bool IsAddressable(const XG_RESOURCE_LAYOUT& layout) {
    if (layout.Planes != 1 || layout.MipLevels == 0 || layout.MipLevels > kMaxMips) return false;
    const XG_PLANE_LAYOUT& plane = layout.Plane[0];
    if (!FindEquation(plane.BytesPerElement)) return false;
    const uint32_t mode = plane.MipLayout[0].SwizzleMode;
    return mode == kMode4KbS || (plane.MipLayout[0].Reserved64 && mode == kMode256BS);
}

bool TexelElementOffset(const XG_RESOURCE_LAYOUT& layout, uint32_t mip, uint32_t slice,
                        uint32_t x, uint32_t y, uint64_t* offset) {
    MipView v;
    if (!offset || !ViewMip(layout, mip, slice, &v)) return false;
    *offset = v.base + XOffset(v, x) + YOffset(v, y);
    return true;
}

bool SubresourceRange(const XG_RESOURCE_LAYOUT& layout, uint32_t mip, uint32_t slice,
                      uint64_t* offset, uint64_t* size) {
    if (!offset || !size || layout.Planes != 1 || mip >= layout.MipLevels || mip >= kMaxMips) return false;
    const XG_PLANE_LAYOUT& plane = layout.Plane[0];
    const XG_MIPLEVEL_LAYOUT& ml = plane.MipLayout[mip];
    if (slice >= std::max<uint32_t>(1, ml.DepthOrArraySize)) return false;
    const uint64_t base = static_cast<uint64_t>(slice) * plane.Reserved10;
    if (ml.Reserved64) {
        *offset = base + (ml.OffsetBytes & ~static_cast<uint64_t>(kBlockBytes - 1));
        *size = kBlockBytes;
    } else {
        *offset = base + ml.OffsetBytes;
        *size = ml.SizeBytes;
    }
    return true;
}

bool CopyIntoSubresource(const XG_RESOURCE_LAYOUT& layout, void* resource_base, uint32_t mip,
                         uint32_t slice, const void* src, uint32_t src_row_pitch) {
    if (!resource_base || !src) return false;
    return CopySubresource(layout, mip, slice, static_cast<uint8_t*>(resource_base),
                           const_cast<uint8_t*>(static_cast<const uint8_t*>(src)), src_row_pitch, true);
}

bool CopyFromSubresource(const XG_RESOURCE_LAYOUT& layout, const void* resource_base, uint32_t mip,
                         uint32_t slice, void* dst, uint32_t dst_row_pitch) {
    if (!resource_base || !dst) return false;
    return CopySubresource(layout, mip, slice, const_cast<uint8_t*>(static_cast<const uint8_t*>(resource_base)),
                           static_cast<uint8_t*>(dst), dst_row_pitch, false);
}

void ComputeTexture2DLayout(const XG_TEXTURE2D_DESC& desc, XG_RESOURCE_LAYOUT* out_ptr) {
    XG_RESOURCE_LAYOUT& out = *out_ptr;
    std::memset(&out, 0, sizeof(out));

    const uint32_t width = std::max<uint32_t>(1, desc.Width);
    const uint32_t height = std::max<uint32_t>(1, desc.Height);
    const uint32_t array_size = std::max<uint32_t>(1, desc.ArraySize);
    const uint32_t mips = std::clamp<uint32_t>(desc.MipLevels == 0 ? 1u : desc.MipLevels, 1u, kMaxMips);
    const uint32_t bpe = FormatBytesPerElement(desc.Format);
    const bool bc = IsBlockCompressed(desc.Format);
    const BlockEquation* eq = FindEquation(bpe);
    const bool swizzled = desc.SwizzleMode == kMode4KbS && eq != nullptr;

    // Block shape in elements. For modes without texel addressing only the
    // padding is modelled, with a near-square block of the mode's size.
    const uint32_t block_bytes = swizzled ? kBlockBytes : BlockBytesForMode(desc.SwizzleMode);
    uint32_t block_w = 0, block_h = 0;
    if (swizzled) {
        block_w = 1u << AxisBits(*eq, 'x', 12);
        block_h = 1u << AxisBits(*eq, 'y', 12);
    } else {
        const uint32_t elems_log2 = Log2(std::max<uint32_t>(1, block_bytes / std::max<uint32_t>(1, bpe)));
        block_w = 1u << ((elems_log2 + 1) / 2);
        block_h = 1u << (elems_log2 / 2);
    }

    out.Dimension = 3;  // D3D12_RESOURCE_DIMENSION_TEXTURE2D
    out.MipLevels = mips;
    out.Planes = 1;
    out.BaseAlignmentBytes = block_bytes;

    XG_PLANE_LAYOUT& plane = out.Plane[0];
    plane.Usage = 1;
    plane.BaseAlignmentBytes = block_bytes;
    plane.BytesPerElement = bpe;

    uint32_t we[kMaxMips], he[kMaxMips];
    uint32_t first_tail = mips;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t mw = std::max<uint32_t>(1, width >> m);
        const uint32_t mh = std::max<uint32_t>(1, height >> m);
        we[m] = bc ? std::max<uint32_t>(1, (mw + 3) / 4) : mw;
        he[m] = bc ? std::max<uint32_t>(1, (mh + 3) / 4) : mh;
        XG_MIPLEVEL_LAYOUT& ml = plane.MipLayout[m];
        ml.WidthElements = we[m];
        ml.HeightElements = he[m];
        ml.ReservedWidth = mw;
        ml.ReservedHeight = mh;
        ml.DepthOrArraySize = array_size;
        ml.PaddedDepthOrArraySize = array_size;
        ml.SampleCount = std::max<uint32_t>(1, desc.SampleDesc.Count);
        ml.Reserved60 = 1;
        // Mips that fit in half a block share one tail block (mipped textures only).
        // The test uses the top mip rounded up to a power of two.
        const uint32_t tw = std::max<uint32_t>(1, (1u << Log2(we[0])) >> m);
        const uint32_t th = std::max<uint32_t>(1, (1u << Log2(he[0])) >> m);
        if (swizzled && mips > 1 && first_tail == mips && tw <= block_w / 2 && th <= block_h) {
            first_tail = m;
        }
    }

    uint64_t offset = first_tail < mips ? block_bytes : 0;
    for (uint32_t i = first_tail; i-- > 0;) {
        XG_MIPLEVEL_LAYOUT& ml = plane.MipLayout[i];
        // Padding rounds the mip size up, while the reported size rounds down.
        ml.PaddedWidthElements = AlignUp32(std::max<uint32_t>(1, (we[0] + (1u << i) - 1) >> i), block_w);
        ml.PaddedHeightElements = AlignUp32(std::max<uint32_t>(1, (he[0] + (1u << i) - 1) >> i), block_h);
        ml.PitchPixels = ml.PaddedWidthElements * (bc ? 4u : 1u);
        ml.PitchBytes = ml.PaddedWidthElements * bpe;
        ml.AlignmentBytes = block_bytes;
        ml.SwizzleMode = desc.SwizzleMode;
        ml.SizeBytes = static_cast<uint64_t>(ml.PaddedWidthElements) * ml.PaddedHeightElements * bpe;
        ml.Slice2DSizeBytes = ml.SizeBytes;
        ml.OffsetBytes = offset;
        offset += ml.SizeBytes;
    }
    for (uint32_t m = first_tail; m < mips; ++m) {
        const uint32_t slot = std::min<uint32_t>(m - first_tail, 7);
        const uint32_t region = TailRegionBytes(slot);
        const uint32_t region_bits = Log2(region);
        XG_MIPLEVEL_LAYOUT& ml = plane.MipLayout[m];
        ml.PaddedWidthElements = 1u << AxisBits(*eq, 'x', region_bits);
        ml.PaddedHeightElements = 1u << AxisBits(*eq, 'y', region_bits);
        ml.PitchPixels = ml.PaddedWidthElements * (bc ? 4u : 1u);
        ml.PitchBytes = ml.PaddedWidthElements * bpe;
        ml.AlignmentBytes = 256;
        ml.SwizzleMode = kMode256BS;
        ml.SizeBytes = region;
        ml.Slice2DSizeBytes = region;
        ml.OffsetBytes = kTailOffset256[slot] * 256u;
        ml.Reserved64 = 1;
    }

    plane.Reserved10 = offset;  // one array slice
    plane.Reserved2C = first_tail;
    plane.Reserved30 = mips - first_tail;
    plane.SizeBytes = offset * array_size;
    out.SizeBytes = plane.SizeBytes;
}

}  // namespace scarlettx::gpu::xg
