#include "scarlettx/gpu/blend_normalize.h"

#if defined(_WIN32)

namespace scarlettx {

namespace {

constexpr UINT kMaxTargets = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;

}  // namespace

BlendNormalizeResult NormalizeBlendForTargets(D3D12_BLEND_DESC& blend, UINT num_render_targets,
                                              const DXGI_FORMAT* formats,
                                              const std::function<bool(DXGI_FORMAT)>& blendable) {
    BlendNormalizeResult result;
    if (!formats || !blendable) return result;
    const UINT count = num_render_targets < kMaxTargets ? num_render_targets : kMaxTargets;
    const bool shared = !blend.IndependentBlendEnable;

    // Which active targets ask for blending the host cannot do, and which keep theirs.
    uint32_t unblendable = 0;
    uint32_t kept = 0;
    for (UINT i = 0; i < count; ++i) {
        if (formats[i] == DXGI_FORMAT_UNKNOWN) continue;
        if (!blend.RenderTarget[shared ? 0 : i].BlendEnable) continue;
        if (blendable(formats[i])) {
            kept |= 1u << i;
        } else {
            unblendable |= 1u << i;
        }
    }
    if (!unblendable) return result;

    if (shared && !kept) {
        // Every target that would blend loses it: the one description still fits them all.
        blend.RenderTarget[0].BlendEnable = FALSE;
    } else {
        if (shared) {
            for (UINT i = 1; i < kMaxTargets; ++i) blend.RenderTarget[i] = blend.RenderTarget[0];
            blend.IndependentBlendEnable = TRUE;
            result.expanded = true;
        }
        for (UINT i = 0; i < count; ++i) {
            if (unblendable & (1u << i)) blend.RenderTarget[i].BlendEnable = FALSE;
        }
    }
    result.disabled_mask = unblendable;
    return result;
}

BlendNormalizeResult NormalizeBlendForDevice(ID3D12Device* device, D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
    if (!device) return {};
    return NormalizeBlendForTargets(desc.BlendState, desc.NumRenderTargets, desc.RTVFormats,
                                    [device](DXGI_FORMAT format) {
                                        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
                                        support.Format = format;
                                        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                                                               &support, sizeof(support)))) {
                                            return true;  // no answer: leave the title's state alone
                                        }
                                        return (support.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE) != 0;
                                    });
}

}  // namespace scarlettx

#endif
