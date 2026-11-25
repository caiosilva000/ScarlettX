// Unit tests for NormalizeBlendForTargets: no device, the host's answers are given.
#include "sx_check.h"

#include <cstdio>
#include <cstring>

#include "scarlettx/gpu/blend_normalize.h"

namespace {

using scarlettx::NormalizeBlendForTargets;

bool HostBlends(DXGI_FORMAT f) { return f != DXGI_FORMAT_R8_UINT && f != DXGI_FORMAT_R32_UINT; }

D3D12_RENDER_TARGET_BLEND_DESC MaxBlend(UINT8 write_mask = D3D12_COLOR_WRITE_ENABLE_ALL) {
    D3D12_RENDER_TARGET_BLEND_DESC rt{};
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_ONE;
    rt.DestBlend = D3D12_BLEND_ONE;
    rt.BlendOp = D3D12_BLEND_OP_MAX;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_ONE;
    rt.BlendOpAlpha = D3D12_BLEND_OP_MAX;
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = write_mask;
    return rt;
}

// Everything but BlendEnable.
bool SameExceptEnable(D3D12_RENDER_TARGET_BLEND_DESC a, D3D12_RENDER_TARGET_BLEND_DESC b) {
    a.BlendEnable = b.BlendEnable = FALSE;
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

}  // namespace

int main() {
    // Shared description, a blendable and an integer target: split, blending kept on 0, off on 1.
    {
        D3D12_BLEND_DESC blend{};
        blend.RenderTarget[0] = MaxBlend(0x7);
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8_UINT};
        const auto r = NormalizeBlendForTargets(blend, 2, formats, HostBlends);
        SX_REQUIRE(r.changed() && r.expanded && r.disabled_mask == 0x2);
        SX_REQUIRE(blend.IndependentBlendEnable == TRUE);
        SX_REQUIRE(blend.RenderTarget[0].BlendEnable == TRUE);
        SX_REQUIRE(blend.RenderTarget[1].BlendEnable == FALSE);
        // Both keep the title's factors, operation and write mask.
        SX_REQUIRE(SameExceptEnable(blend.RenderTarget[0], MaxBlend(0x7)));
        SX_REQUIRE(SameExceptEnable(blend.RenderTarget[1], MaxBlend(0x7)));
        SX_REQUIRE(blend.RenderTarget[0].BlendOp == D3D12_BLEND_OP_MAX);
    }
    // Shared description, three targets, the integer one last (float, unorm, uint).
    {
        D3D12_BLEND_DESC blend{};
        blend.RenderTarget[0] = MaxBlend();
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM,
                                       DXGI_FORMAT_R8_UINT};
        const auto r = NormalizeBlendForTargets(blend, 3, formats, HostBlends);
        SX_REQUIRE(r.expanded && r.disabled_mask == 0x4);
        SX_REQUIRE(blend.RenderTarget[0].BlendEnable && blend.RenderTarget[1].BlendEnable);
        SX_REQUIRE(!blend.RenderTarget[2].BlendEnable);
    }
    // Shared description, every target blendable: untouched, still shared.
    {
        D3D12_BLEND_DESC blend{};
        blend.RenderTarget[0] = MaxBlend();
        const D3D12_BLEND_DESC before = blend;
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};
        const auto r = NormalizeBlendForTargets(blend, 2, formats, HostBlends);
        SX_REQUIRE(!r.changed() && !r.expanded);
        SX_REQUIRE(std::memcmp(&blend, &before, sizeof(blend)) == 0);
    }
    // Shared description, no target blendable: blending off, and no need to split.
    {
        D3D12_BLEND_DESC blend{};
        blend.RenderTarget[0] = MaxBlend();
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R32_UINT};
        const auto r = NormalizeBlendForTargets(blend, 2, formats, HostBlends);
        SX_REQUIRE(r.changed() && !r.expanded && r.disabled_mask == 0x3);
        SX_REQUIRE(blend.IndependentBlendEnable == FALSE);
        SX_REQUIRE(blend.RenderTarget[0].BlendEnable == FALSE);
        SX_REQUIRE(SameExceptEnable(blend.RenderTarget[0], MaxBlend()));
    }
    // Shared description without blending: untouched, and the host is not even asked.
    {
        D3D12_BLEND_DESC blend{};
        blend.RenderTarget[0] = MaxBlend();
        blend.RenderTarget[0].BlendEnable = FALSE;
        const D3D12_BLEND_DESC before = blend;
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8_UINT};
        int asked = 0;
        const auto r = NormalizeBlendForTargets(blend, 2, formats, [&](DXGI_FORMAT) {
            ++asked;
            return false;
        });
        SX_REQUIRE(!r.changed() && asked == 0);
        SX_REQUIRE(std::memcmp(&blend, &before, sizeof(blend)) == 0);
    }
    // Independent descriptions: only the enabled, unblendable target changes.
    {
        D3D12_BLEND_DESC blend{};
        blend.IndependentBlendEnable = TRUE;
        blend.RenderTarget[0] = MaxBlend(0xF);
        blend.RenderTarget[1] = MaxBlend(0x1);
        blend.RenderTarget[2] = MaxBlend(0x3);
        blend.RenderTarget[2].BlendEnable = FALSE;  // integer target the title already left alone
        blend.RenderTarget[3] = MaxBlend(0x5);      // beyond the pipeline's target count
        const D3D12_BLEND_DESC before = blend;
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R32_UINT,
                                       DXGI_FORMAT_R8_UINT};
        const auto r = NormalizeBlendForTargets(blend, 3, formats, HostBlends);
        SX_REQUIRE(r.changed() && !r.expanded && r.disabled_mask == 0x2);
        SX_REQUIRE(blend.IndependentBlendEnable == TRUE);
        SX_REQUIRE(blend.RenderTarget[1].BlendEnable == FALSE);
        SX_REQUIRE(SameExceptEnable(blend.RenderTarget[1], before.RenderTarget[1]));
        SX_REQUIRE(std::memcmp(&blend.RenderTarget[0], &before.RenderTarget[0], sizeof(before.RenderTarget[0])) == 0);
        SX_REQUIRE(std::memcmp(&blend.RenderTarget[2], &before.RenderTarget[2], sizeof(before.RenderTarget[0])) == 0);
        SX_REQUIRE(std::memcmp(&blend.RenderTarget[3], &before.RenderTarget[3], sizeof(before.RenderTarget[0])) == 0);
    }
    // Independent descriptions, all fine: untouched.
    {
        D3D12_BLEND_DESC blend{};
        blend.IndependentBlendEnable = TRUE;
        blend.RenderTarget[0] = MaxBlend();
        blend.RenderTarget[1] = MaxBlend();
        blend.RenderTarget[1].BlendEnable = FALSE;
        const D3D12_BLEND_DESC before = blend;
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8_UINT};
        SX_REQUIRE(!NormalizeBlendForTargets(blend, 2, formats, HostBlends).changed());
        SX_REQUIRE(std::memcmp(&blend, &before, sizeof(blend)) == 0);
    }
    // A logic-op state is not blending: left exactly as it is, integer target or not.
    {
        D3D12_BLEND_DESC blend{};
        blend.RenderTarget[0].LogicOpEnable = TRUE;
        blend.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_XOR;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        const D3D12_BLEND_DESC before = blend;
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8G8B8A8_UNORM};
        SX_REQUIRE(!NormalizeBlendForTargets(blend, 2, formats, HostBlends).changed());
        SX_REQUIRE(std::memcmp(&blend, &before, sizeof(blend)) == 0);
    }
    // A target slot without a format is not an active target.
    {
        D3D12_BLEND_DESC blend{};
        blend.RenderTarget[0] = MaxBlend();
        const D3D12_BLEND_DESC before = blend;
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN};
        SX_REQUIRE(!NormalizeBlendForTargets(blend, 2, formats, [](DXGI_FORMAT f) {
                        return f == DXGI_FORMAT_R8G8B8A8_UNORM;
                    }).changed());
        SX_REQUIRE(std::memcmp(&blend, &before, sizeof(blend)) == 0);
    }
    // No targets, or no format list: nothing to do.
    {
        D3D12_BLEND_DESC blend{};
        blend.RenderTarget[0] = MaxBlend();
        SX_REQUIRE(!NormalizeBlendForTargets(blend, 0, nullptr, HostBlends).changed());
        SX_REQUIRE(blend.RenderTarget[0].BlendEnable == TRUE);
    }
    std::printf("[test_blend_normalize] PASS\n");
    return 0;
}
