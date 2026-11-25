#pragma once

// Blend-state normalization for host pipeline creation.
//
// A console title may ask for blending into a render target whose format the host cannot blend
// into (integer formats such as R8_UINT). The console leaves blending out for such a target; host
// D3D12 refuses the whole pipeline. Before creating a pipeline, blending is switched off for
// exactly those targets and kept for the rest.
//
// Provenance: the safeguard is the one described by victory-amd commit
// 46dfc101ab29b741b4109913740428b73e6087e5 ("GPU: no blending into render targets the host cannot
// blend into", GPL-3.0-or-later). This is an independent implementation written from that
// behaviour's description and the D3D12 documentation (D3D12_FEATURE_FORMAT_SUPPORT,
// D3D12_FORMAT_SUPPORT1_BLENDABLE, D3D12_BLEND_DESC); no code was copied from it.

#if defined(_WIN32)

#include <cstdint>
#include <functional>

#include <windows.h>
#include <d3d12.h>

namespace scarlettx {

struct BlendNormalizeResult {
    uint32_t disabled_mask = 0;  // bit i: blending was switched off for render target i
    bool expanded = false;       // a shared description was split into one per target
    bool changed() const noexcept { return disabled_mask != 0; }
};

// `blendable(format)` says whether the host blends into `format`. Only the first
// `num_render_targets` targets with a known format are looked at. LogicOpEnable and LogicOp are
// never changed. A shared description (IndependentBlendEnable FALSE) becomes independent only
// when some targets keep blending and others lose it.
BlendNormalizeResult NormalizeBlendForTargets(D3D12_BLEND_DESC& blend, UINT num_render_targets,
                                              const DXGI_FORMAT* formats,
                                              const std::function<bool(DXGI_FORMAT)>& blendable);

// The same for a pipeline description, asking `device` (D3D12_FEATURE_FORMAT_SUPPORT) about each
// format. A format the device cannot answer for is left as the title asked.
BlendNormalizeResult NormalizeBlendForDevice(ID3D12Device* device, D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);

}  // namespace scarlettx

#endif  // _WIN32
