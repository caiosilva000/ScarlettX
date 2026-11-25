// A two-target pipeline that asks for MAX blending on both targets, one of them an integer
// format the host cannot blend into, created through the d3d12_xs shim as a title would, then
// drawn and read back.
//
//   target 0  R8G8B8A8_UNORM  blendable: the two draws must combine as MAX
//   target 1  R8_UINT         not blendable: the second draw must simply overwrite the first
//
//   test_d3d12_blend_mrt               the check above
//   test_d3d12_blend_mrt --raw-probe   informational: what the host does with the title's
//                                      description unchanged (may fail or abort; always exit 0)
//   add --warp to ask for WARP first
//
// Exit codes: 0 pass, 1 readback mismatch, 2 a call failed (name and HRESULT printed),
// 3 no device, 4 the host says it blends into R8_UINT (nothing to check).
#include "sx_check.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include "scarlettx/gpu/d3d12_xs_abi.h"

namespace {

constexpr UINT kSize = 16;
constexpr DXGI_FORMAT kFormats[2] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8_UINT};

// Draw A then draw B. MAX of the colours differs from either; the ids go down, so an overwrite
// (7) differs from a MAX (200).
constexpr uint8_t kColorA[4] = {51, 204, 102, 255};
constexpr uint8_t kColorB[4] = {204, 51, 153, 128};
constexpr uint8_t kWantColor[4] = {204, 204, 153, 255};
constexpr uint32_t kIdA = 200;
constexpr uint32_t kIdB = 7;

const char* kHlsl = R"(
cbuffer Draw : register(b0) { float4 color; uint id; };
float4 VSMain(uint vid : SV_VertexID) : SV_POSITION {
    return float4(float((vid << 1) & 2) * 2.0f - 1.0f, float(vid & 2) * 2.0f - 1.0f, 0.0f, 1.0f);
}
struct Out { float4 c : SV_Target0; uint i : SV_Target1; };
Out PSMain() { Out o; o.c = color; o.i = id; return o; }
)";

using Slot4 = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, uintptr_t, uintptr_t, uintptr_t);
using Slot6 = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

void* Slot(void* object, int index) { return (*reinterpret_cast<void***>(object))[index]; }

int Fail(const char* api, HRESULT hr) {
    std::printf("[blend-mrt] FAILED api=%s hr=0x%08x\n", api, static_cast<uint32_t>(hr));
    std::fflush(stdout);
    return 2;
}

D3D12_RENDER_TARGET_BLEND_DESC MaxBlend() {
    D3D12_RENDER_TARGET_BLEND_DESC rt{};
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_ONE;
    rt.DestBlend = D3D12_BLEND_ONE;
    rt.BlendOp = D3D12_BLEND_OP_MAX;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_ONE;
    rt.BlendOpAlpha = D3D12_BLEND_OP_MAX;
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    return rt;
}

}  // namespace

int main(int argc, char** argv) {
    bool raw_probe = false;
    bool prefer_warp = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--raw-probe") raw_probe = true;
        else if (arg == "--warp") prefer_warp = true;
        else {
            std::fprintf(stderr, "usage: test_d3d12_blend_mrt [--raw-probe] [--warp]\n");
            return 64;
        }
    }

    // Real-title mode: the shim hands out its own device object, whose CreateGraphicsPipelineState
    // (vtable slot 10) is the one a title's pipelines go through.
    _putenv_s("SCARLETTX_REAL_TITLE", "1");
    SetEnvironmentVariableA("SCARLETTX_REAL_TITLE", "1");

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    params.PreferWarp = prefer_warp ? 1 : 0;
    void* shim_device = nullptr;
    const int32_t created = D3D12XboxCreateDevice(nullptr, &params, &__uuidof(ID3D12Device), &shim_device);
    if (created < 0 || !shim_device) {
        std::printf("[blend-mrt] D3D12XboxCreateDevice FAILED hr=0x%08x\n", static_cast<uint32_t>(created));
        return 3;
    }
    ID3D12Device* device = nullptr;
    SX_REQUIRE(D3D12XboxGetNativeObject(shim_device, reinterpret_cast<void**>(&device)) == 0 && device);

    // What the host says about the two formats.
    bool blendable[2] = {};
    for (int i = 0; i < 2; ++i) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
        support.Format = kFormats[i];
        const HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
        if (FAILED(hr)) return Fail("ID3D12Device::CheckFeatureSupport(FORMAT_SUPPORT)", hr);
        blendable[i] = (support.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE) != 0;
        std::printf("[blend-mrt] format %d support1=0x%08x blendable=%s render_target=%s\n",
                    static_cast<int>(kFormats[i]), static_cast<uint32_t>(support.Support1),
                    blendable[i] ? "yes" : "no",
                    (support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) ? "yes" : "no");
    }
    std::fflush(stdout);

    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(kHlsl, std::strlen(kHlsl), "blend_mrt.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", 0,
                            0, &vs, &err);
    if (FAILED(hr)) return Fail("D3DCompile(VSMain)", hr);
    hr = D3DCompile(kHlsl, std::strlen(kHlsl), "blend_mrt.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &ps,
                    &err);
    if (FAILED(hr)) return Fail("D3DCompile(PSMain)", hr);

    D3D12_ROOT_PARAMETER draw_param{};
    draw_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    draw_param.Constants.Num32BitValues = 8;
    draw_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 1;
    rs_desc.pParameters = &draw_param;
    ID3DBlob* rs_blob = nullptr;
    hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &err);
    if (FAILED(hr)) return Fail("D3D12SerializeRootSignature", hr);
    void* shim_root = nullptr;
    hr = static_cast<HRESULT>(reinterpret_cast<Slot6>(Slot(shim_device, 16))(
        shim_device, 0, reinterpret_cast<uintptr_t>(rs_blob->GetBufferPointer()), rs_blob->GetBufferSize(), 0,
        reinterpret_cast<uintptr_t>(&shim_root)));
    if (FAILED(hr) || !shim_root) return Fail("shim CreateRootSignature (slot 16)", hr);
    ID3D12RootSignature* root = nullptr;
    SX_REQUIRE(D3D12XboxGetNativeObject(shim_root, reinterpret_cast<void**>(&root)) == 0 && root);

    // The title's description: one blend description shared by both targets, MAX.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso_desc.BlendState.IndependentBlendEnable = FALSE;
    pso_desc.BlendState.RenderTarget[0] = MaxBlend();
    pso_desc.SampleMask = 0xFFFFFFFFu;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 2;
    pso_desc.RTVFormats[0] = kFormats[0];
    pso_desc.RTVFormats[1] = kFormats[1];
    pso_desc.SampleDesc.Count = 1;

    if (raw_probe) {
        // Straight to the host, nothing normalized.
        pso_desc.pRootSignature = root;
        std::printf("[blend-mrt] raw-probe: creating the pipeline on the host with the title's blend state\n");
        std::fflush(stdout);
        ID3D12PipelineState* raw = nullptr;
        hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&raw));
        std::printf("[blend-mrt] raw-probe: host CreateGraphicsPipelineState hr=0x%08x pso=%s\n",
                    static_cast<uint32_t>(hr), raw ? "created" : "none");
        std::fflush(stdout);
        return 0;
    }
    if (blendable[1] || !blendable[0]) {
        std::printf("[blend-mrt] the host's answers leave nothing to check here\n");
        return 4;
    }

    // Through the shim, as a title's pipeline arrives. The root signature is the shim's object.
    pso_desc.pRootSignature = static_cast<ID3D12RootSignature*>(shim_root);
    const uint64_t adjusted_before = D3D12XboxBlendAdjustedPipelineCount();
    void* shim_pso = nullptr;
    hr = static_cast<HRESULT>(reinterpret_cast<Slot4>(Slot(shim_device, 10))(
        shim_device, reinterpret_cast<uintptr_t>(&pso_desc), 0, reinterpret_cast<uintptr_t>(&shim_pso)));
    std::printf("[blend-mrt] shim CreateGraphicsPipelineState (2 targets, shared MAX blend) hr=0x%08x\n",
                static_cast<uint32_t>(hr));
    std::fflush(stdout);
    if (FAILED(hr) || !shim_pso) return Fail("shim CreateGraphicsPipelineState (slot 10)", hr);
    // The shim must have taken blending off the integer target itself: some hosts tolerate the
    // title's state and some refuse the pipeline, and the result has to be the same on both.
    const uint64_t adjusted = D3D12XboxBlendAdjustedPipelineCount() - adjusted_before;
    std::printf("[blend-mrt] pipelines the shim adjusted: %llu (want 1)\n", static_cast<unsigned long long>(adjusted));
    std::fflush(stdout);
    if (adjusted != 1) {
        std::printf("[blend-mrt] FAIL: the shim passed the unblendable target's blending to the host\n");
        return 1;
    }
    // The title's own description must not have been edited.
    SX_REQUIRE(pso_desc.BlendState.IndependentBlendEnable == FALSE);
    SX_REQUIRE(pso_desc.BlendState.RenderTarget[0].BlendEnable == TRUE);
    ID3D12PipelineState* pso = nullptr;
    SX_REQUIRE(D3D12XboxGetNativeObject(shim_pso, reinterpret_cast<void**>(&pso)) == 0 && pso);

    // Host-side plumbing to draw with it.
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12Resource* targets[2] = {};
    ID3D12Resource* readback[2] = {};
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) return Fail("CreateCommandQueue", hr);
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(hr)) return Fail("CreateCommandAllocator", hr);
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, pso, IID_PPV_ARGS(&list));
    if (FAILED(hr)) return Fail("CreateCommandList", hr);
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) return Fail("CreateFence", hr);
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.NumDescriptors = 2;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap));
    if (FAILED(hr)) return Fail("CreateDescriptorHeap(RTV)", hr);
    const UINT rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2];
    rtvs[0] = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtvs[1].ptr = rtvs[0].ptr + rtv_size;

    const UINT texel_bytes[2] = {4, 1};
    constexpr UINT kRowPitch = 256;  // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, enough for 16 texels
    for (int i = 0; i < 2; ++i) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC tex{};
        tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tex.Width = kSize;
        tex.Height = kSize;
        tex.DepthOrArraySize = 1;
        tex.MipLevels = 1;
        tex.Format = kFormats[i];
        tex.SampleDesc.Count = 1;
        tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = kFormats[i];
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                             &clear, IID_PPV_ARGS(&targets[i]));
        if (FAILED(hr)) return Fail(i ? "CreateCommittedResource(R8_UINT target)" : "CreateCommittedResource(RGBA8 target)", hr);
        device->CreateRenderTargetView(targets[i], nullptr, rtvs[i]);

        D3D12_HEAP_PROPERTIES rb_heap{};
        rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buf{};
        buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf.Width = static_cast<UINT64>(kRowPitch) * kSize;
        buf.Height = 1;
        buf.DepthOrArraySize = 1;
        buf.MipLevels = 1;
        buf.SampleDesc.Count = 1;
        buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = device->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&readback[i]));
        if (FAILED(hr)) return Fail("CreateCommittedResource(Readback)", hr);
    }

    const float zero[4] = {0, 0, 0, 0};
    list->OMSetRenderTargets(2, rtvs, FALSE, nullptr);
    list->ClearRenderTargetView(rtvs[0], zero, 0, nullptr);
    list->ClearRenderTargetView(rtvs[1], zero, 0, nullptr);
    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(kSize), static_cast<LONG>(kSize)};
    list->SetGraphicsRootSignature(root);
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto draw = [&](const uint8_t color[4], uint32_t id) {
        struct {
            float color[4];
            uint32_t id;
            uint32_t pad[3];
        } constants{};
        for (int c = 0; c < 4; ++c) constants.color[c] = static_cast<float>(color[c]) / 255.0f;
        constants.id = id;
        list->SetGraphicsRoot32BitConstants(0, 8, &constants, 0);
        list->DrawInstanced(3, 1, 0, 0);  // one triangle covering the whole target
    };
    draw(kColorA, kIdA);
    draw(kColorB, kIdB);
    for (int i = 0; i < 2; ++i) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = targets[i];
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = targets[i];
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback[i];
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format = kFormats[i];
        dst.PlacedFootprint.Footprint.Width = kSize;
        dst.PlacedFootprint.Footprint.Height = kSize;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = kRowPitch;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    hr = list->Close();
    if (FAILED(hr)) return Fail("ID3D12GraphicsCommandList::Close", hr);
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    hr = queue->Signal(fence, 1);
    if (FAILED(hr)) return Fail("ID3D12CommandQueue::Signal", hr);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    DWORD wait = 0;
    if (fence->GetCompletedValue() < 1) {
        hr = fence->SetEventOnCompletion(1, event);
        if (FAILED(hr)) return Fail("ID3D12Fence::SetEventOnCompletion", hr);
        wait = WaitForSingleObject(event, 10000);
    }
    const UINT64 completed = fence->GetCompletedValue();
    std::printf("[blend-mrt] fence signaled=1 completed=%llu wait_result=%lu draws=2\n",
                static_cast<unsigned long long>(completed), static_cast<unsigned long>(wait));
    if (completed < 1) return Fail("ID3D12Fence::GetCompletedValue (fence not reached)", device->GetDeviceRemovedReason());

    const UINT total = kSize * kSize;
    UINT exact[2] = {};
    uint8_t first[2][4] = {};
    for (int i = 0; i < 2; ++i) {
        void* mapped = nullptr;
        hr = readback[i]->Map(0, nullptr, &mapped);
        if (FAILED(hr) || !mapped) return Fail("ID3D12Resource::Map(Readback)", FAILED(hr) ? hr : E_POINTER);
        const auto* bytes = static_cast<const uint8_t*>(mapped);
        const uint8_t want_id[1] = {static_cast<uint8_t>(kIdB)};
        const uint8_t* want = i == 0 ? kWantColor : want_id;
        std::memcpy(first[i], bytes, texel_bytes[i]);
        for (UINT y = 0; y < kSize; ++y) {
            for (UINT x = 0; x < kSize; ++x) {
                if (std::memcmp(bytes + static_cast<size_t>(y) * kRowPitch + x * texel_bytes[i], want,
                                texel_bytes[i]) == 0) {
                    ++exact[i];
                }
            }
        }
        readback[i]->Unmap(0, nullptr);
    }
    std::printf("[blend-mrt] target 0 RGBA8_UNORM: pixels exact %u/%u got %u,%u,%u,%u want %u,%u,%u,%u (MAX of %u,%u,%u,%u and %u,%u,%u,%u)\n",
                exact[0], total, first[0][0], first[0][1], first[0][2], first[0][3], kWantColor[0], kWantColor[1],
                kWantColor[2], kWantColor[3], kColorA[0], kColorA[1], kColorA[2], kColorA[3], kColorB[0],
                kColorB[1], kColorB[2], kColorB[3]);
    std::printf("[blend-mrt] target 1 R8_UINT: pixels exact %u/%u got %u want %u (second draw overwrites %u; a MAX would be %u)\n",
                exact[1], total, first[1][0], kIdB, kIdA, kIdA);
    const bool pass = exact[0] == total && exact[1] == total;
    std::printf("[blend-mrt] %s\n", pass ? "PASS" : "FAIL");
    std::fflush(stdout);
    return pass ? 0 : 1;
}
