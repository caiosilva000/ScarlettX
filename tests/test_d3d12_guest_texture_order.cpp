// Real-title proxy path: the order of a guest write and a GPU copy into the same texture.
//
// A texture is filled through the address GetGPUVirtualAddress returns in several submissions
// running, so that the shim stops re-arming its write watch ("unarmed"). Then:
//
//   1. the guest writes bytes A                      -> a sampled draw must show A
//   2. a command-list copy puts bytes B into the     -> a sampled draw must show B
//      same subresource, and is waited for              (the copy is visible before the rewrite)
//   3. the guest writes A again, byte for byte       -> the next sampled draw must show A
//   4. the copy of B again, and no guest write       -> two sampled draws must both show B
//                                                       (nothing uploads without a write)
//
// Every step is its own submission and is waited for on the fence before the next one starts.
// "Shows" means every pixel of a 64x64 target drawn by sampling the texture texel for pixel.
//
// Exit codes: 0 pass, 1 a draw showed the wrong bytes, 2 a call failed, 3 no device.
#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "scarlettx/gpu/xg_layout.h"
#include "sx_check.h"

#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace xg = scarlettx::gpu::xg;

namespace {

template <typename... Args>
intptr_t Call(void* object, int slot, Args... args) {
    void** vtable = *reinterpret_cast<void***>(object);
    using Fn = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, Args...);
    return reinterpret_cast<Fn>(vtable[slot])(object, args...);
}
uintptr_t P(const void* p) { return reinterpret_cast<uintptr_t>(p); }

constexpr int kDevCreateCommandQueue = 8;
constexpr int kDevCreateCommandAllocator = 9;
constexpr int kDevCreateGraphicsPipelineState = 10;
constexpr int kDevCreateCommandList = 12;
constexpr int kDevCreateDescriptorHeap = 14;
constexpr int kDevCreateRootSignature = 16;
constexpr int kDevCreateShaderResourceView = 18;
constexpr int kDevCreateRenderTargetView = 20;
constexpr int kDevCreateCommittedResource = 27;
constexpr int kDevCreateFence = 36;
constexpr int kHeapGetCpuStart = 9;
constexpr int kHeapGetGpuStart = 10;
constexpr int kResMap = 8;
constexpr int kResUnmap = 9;
constexpr int kResGetGPUVirtualAddress = 11;
constexpr int kAllocReset = 8;
constexpr int kFenceGetCompletedValue = 8;
constexpr int kFenceSetEventOnCompletion = 9;
constexpr int kListClose = 9;
constexpr int kListReset = 10;
constexpr int kListDrawInstanced = 12;
constexpr int kListCopyTextureRegion = 16;
constexpr int kListIASetPrimitiveTopology = 20;
constexpr int kListRSSetViewports = 21;
constexpr int kListRSSetScissorRects = 22;
constexpr int kListSetPipelineState = 25;
constexpr int kListResourceBarrier = 26;
constexpr int kListSetDescriptorHeaps = 32;
constexpr int kListSetGraphicsRootSignature = 34;
constexpr int kListSetGraphicsRootDescriptorTable = 36;
constexpr int kListOMSetRenderTargets = 50;
constexpr int kQueueExecuteCommandLists = 10;
constexpr int kQueueSignal = 14;

constexpr uint32_t kSize = 64;
constexpr uint32_t kPitch = 256;  // 64 texels of 4 bytes, already a multiple of 256

const char* kHlsl = R"(
Texture2D<float4> tex : register(t0);
float4 VSMain(uint vid : SV_VertexID) : SV_POSITION {
    return float4(float((vid << 1) & 2) * 2.0f - 1.0f, float(vid & 2) * 2.0f - 1.0f, 0.0f, 1.0f);
}
float4 PSMain(float4 pos : SV_POSITION) : SV_TARGET {
    return tex.Load(int3(int(pos.x), int(pos.y), 0));
}
)";

struct Rig {
    void* device = nullptr;
    void* queue = nullptr;
    void* allocator = nullptr;
    void* list = nullptr;
    void* fence = nullptr;
    HANDLE event = nullptr;
    uint64_t fence_value = 0;
    void* texture = nullptr;
    void* target = nullptr;
    void* staging = nullptr;
    void* readback = nullptr;
    void* srv_heap = nullptr;
    void* root = nullptr;
    void* pso = nullptr;
    uintptr_t rtv = 0;
    uintptr_t srv_gpu = 0;
    uint8_t* memory = nullptr;
    std::unique_ptr<XG_RESOURCE_LAYOUT> layout;
    int failures = 0;

    void* CreateResource(D3D12_HEAP_TYPE heap, const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = heap;
        void* res = nullptr;
        SX_REQUIRE(Call(device, kDevCreateCommittedResource, P(&hp), uintptr_t{0}, P(&desc), static_cast<uintptr_t>(state),
                        uintptr_t{0}, uintptr_t{0}, P(&res)) == 0 && res);
        return res;
    }
    void Begin() {
        SX_REQUIRE(Call(allocator, kAllocReset) == 0);
        SX_REQUIRE(Call(list, kListReset, P(allocator), uintptr_t{0}, uintptr_t{0}) == 0);
    }
    // Closes, submits and waits for the fence: nothing of the next step starts before this one is done.
    void RunAndWait() {
        SX_REQUIRE(Call(list, kListClose) == 0);
        void* lists[] = {list};
        Call(queue, kQueueExecuteCommandLists, uintptr_t{1}, P(lists), uintptr_t{0});
        const uint64_t value = ++fence_value;
        SX_REQUIRE(Call(queue, kQueueSignal, P(fence), static_cast<uintptr_t>(value), uintptr_t{0}) == 0);
        if (static_cast<uint64_t>(Call(fence, kFenceGetCompletedValue)) < value) {
            SX_REQUIRE(Call(fence, kFenceSetEventOnCompletion, static_cast<uintptr_t>(value), P(event), uintptr_t{0}) == 0);
            SX_REQUIRE(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0);
        }
        SX_REQUIRE(static_cast<uint64_t>(Call(fence, kFenceGetCompletedValue)) >= value);
    }
    void Barrier(void* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = static_cast<ID3D12Resource*>(res);
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        Call(list, kListResourceBarrier, uintptr_t{1}, P(&b), uintptr_t{0});
    }
    // The guest writes the texture's own memory, as a title does.
    void GuestWrite(const std::vector<uint32_t>& texels) {
        SX_REQUIRE(xg::CopyIntoSubresource(*layout, memory, 0, 0, texels.data(), kSize * 4));
    }
    // A command-list copy of `texels` into the texture, submitted and waited for.
    void GpuCopy(const std::vector<uint32_t>& texels) {
        uint8_t* up = nullptr;
        SX_REQUIRE(Call(staging, kResMap, uintptr_t{0}, uintptr_t{0}, P(&up)) == 0 && up);
        for (uint32_t y = 0; y < kSize; ++y) std::memcpy(up + static_cast<size_t>(y) * kPitch, texels.data() + y * kSize, kSize * 4);
        Call(staging, kResUnmap, uintptr_t{0}, uintptr_t{0});
        Begin();
        Barrier(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = static_cast<ID3D12Resource*>(texture);
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = static_cast<ID3D12Resource*>(staging);
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = kSize;
        src.PlacedFootprint.Footprint.Height = kSize;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = kPitch;
        Call(list, kListCopyTextureRegion, P(&dst), uintptr_t{0}, uintptr_t{0}, uintptr_t{0}, P(&src), uintptr_t{0});
        Barrier(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        RunAndWait();
    }
    // Draws the texture texel for pixel, reads the target back and compares it with `want`.
    void DrawAndExpect(const std::vector<uint32_t>& want, const char* step, const char* what) {
        Begin();
        void* heaps[] = {srv_heap};
        Call(list, kListSetDescriptorHeaps, uintptr_t{1}, P(heaps), uintptr_t{0});
        Call(list, kListSetGraphicsRootSignature, P(root), uintptr_t{0}, uintptr_t{0});
        Call(list, kListSetPipelineState, P(pso), uintptr_t{0}, uintptr_t{0});
        const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(kSize), static_cast<LONG>(kSize)};
        Call(list, kListRSSetViewports, uintptr_t{1}, P(&vp), uintptr_t{0});
        Call(list, kListRSSetScissorRects, uintptr_t{1}, P(&scissor), uintptr_t{0});
        const D3D12_CPU_DESCRIPTOR_HANDLE handle{rtv};
        Call(list, kListOMSetRenderTargets, uintptr_t{1}, P(&handle), uintptr_t{0}, uintptr_t{0});
        Call(list, kListIASetPrimitiveTopology, static_cast<uintptr_t>(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST), uintptr_t{0},
             uintptr_t{0});
        Call(list, kListSetGraphicsRootDescriptorTable, uintptr_t{0}, srv_gpu, uintptr_t{0});
        Call(list, kListDrawInstanced, uintptr_t{3}, uintptr_t{1}, uintptr_t{0}, uintptr_t{0});
        Barrier(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = static_cast<ID3D12Resource*>(target);
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = static_cast<ID3D12Resource*>(readback);
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width = kSize;
        dst.PlacedFootprint.Footprint.Height = kSize;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = kPitch;
        Call(list, kListCopyTextureRegion, P(&dst), uintptr_t{0}, uintptr_t{0}, uintptr_t{0}, P(&src), uintptr_t{0});
        Barrier(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        RunAndWait();

        uint8_t* data = nullptr;
        SX_REQUIRE(Call(readback, kResMap, uintptr_t{0}, uintptr_t{0}, P(&data)) == 0 && data);
        uint32_t exact = 0, first_got = 0;
        bool have_bad = false;
        for (uint32_t y = 0; y < kSize; ++y) {
            for (uint32_t x = 0; x < kSize; ++x) {
                uint32_t got;
                std::memcpy(&got, data + static_cast<size_t>(y) * kPitch + x * 4, 4);
                if (got == want[y * kSize + x]) {
                    ++exact;
                } else if (!have_bad) {
                    have_bad = true;
                    first_got = got;
                }
            }
        }
        Call(readback, kResUnmap, uintptr_t{0}, uintptr_t{0});
        const bool ok = exact == kSize * kSize;
        std::printf("[write-order] %-28s sampled draw: %u/%u pixels show %s -> %s", step, exact, kSize * kSize, what,
                    ok ? "ok" : "WRONG");
        if (!ok) std::printf(" (first wrong pixel 0x%08x, wanted 0x%08x)", first_got, want[0]);
        std::printf("\n");
        std::fflush(stdout);
        if (!ok) ++failures;
    }
};

std::vector<uint32_t> Pattern(uint32_t id) {
    std::vector<uint32_t> t(kSize * kSize);
    for (uint32_t i = 0; i < t.size(); ++i) {
        uint32_t h = (i * 0x9E3779B1u) ^ (id * 0x85EBCA77u);
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 13;
        t[i] = h;
    }
    return t;
}

}  // namespace

int main() {
    _putenv_s("SCARLETTX_REAL_TITLE", "1");
    SetEnvironmentVariableA("SCARLETTX_REAL_TITLE", "1");

    Rig r;
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    params.PreferWarp = 1;
    const IID device_iid = __uuidof(ID3D12Device);
    if (D3D12XboxCreateDevice(nullptr, &params, &device_iid, &r.device) < 0 || !r.device) {
        std::printf("[write-order] D3D12XboxCreateDevice FAILED\n");
        return 3;
    }
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    SX_REQUIRE(Call(r.device, kDevCreateCommandQueue, P(&qdesc), uintptr_t{0}, P(&r.queue)) == 0 && r.queue);
    SX_REQUIRE(Call(r.device, kDevCreateCommandAllocator, static_cast<uintptr_t>(D3D12_COMMAND_LIST_TYPE_DIRECT),
                    uintptr_t{0}, P(&r.allocator)) == 0 && r.allocator);
    SX_REQUIRE(Call(r.device, kDevCreateCommandList, uintptr_t{0}, static_cast<uintptr_t>(D3D12_COMMAND_LIST_TYPE_DIRECT),
                    P(r.allocator), uintptr_t{0}, uintptr_t{0}, P(&r.list)) == 0 && r.list);
    SX_REQUIRE(Call(r.list, kListClose) == 0);
    SX_REQUIRE(Call(r.device, kDevCreateFence, uintptr_t{0}, uintptr_t{0}, uintptr_t{0}, P(&r.fence)) == 0 && r.fence);
    r.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kSize;
    td.Height = kSize;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    r.texture = r.CreateResource(D3D12_HEAP_TYPE_DEFAULT, td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    D3D12_RESOURCE_DESC rd = td;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    r.target = r.CreateResource(D3D12_HEAP_TYPE_DEFAULT, rd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = static_cast<uint64_t>(kPitch) * kSize;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    r.staging = r.CreateResource(D3D12_HEAP_TYPE_UPLOAD, bd, D3D12_RESOURCE_STATE_GENERIC_READ);
    r.readback = r.CreateResource(D3D12_HEAP_TYPE_READBACK, bd, D3D12_RESOURCE_STATE_COPY_DEST);

    r.memory = reinterpret_cast<uint8_t*>(Call(r.texture, kResGetGPUVirtualAddress));
    SX_REQUIRE(r.memory != nullptr);
    XG_TEXTURE2D_DESC xd{};
    xd.Width = kSize;
    xd.Height = kSize;
    xd.MipLevels = 1;
    xd.ArraySize = 1;
    xd.Format = 28;  // DXGI_FORMAT_R8G8B8A8_UNORM
    xd.SampleDesc.Count = 1;
    xd.BindFlags = 0x8;
    xd.SwizzleMode = XG_SWIZZLE_MODE_4KB_S;
    r.layout = std::make_unique<XG_RESOURCE_LAYOUT>();
    xg::ComputeTexture2DLayout(xd, r.layout.get());

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    SX_REQUIRE(Call(r.device, kDevCreateDescriptorHeap, P(&hd), uintptr_t{0}, P(&r.srv_heap)) == 0 && r.srv_heap);
    void* rtv_heap = nullptr;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    SX_REQUIRE(Call(r.device, kDevCreateDescriptorHeap, P(&hd), uintptr_t{0}, P(&rtv_heap)) == 0 && rtv_heap);
    r.rtv = static_cast<uintptr_t>(Call(rtv_heap, kHeapGetCpuStart));
    r.srv_gpu = static_cast<uintptr_t>(Call(r.srv_heap, kHeapGetGpuStart));
    Call(r.device, kDevCreateRenderTargetView, P(r.target), uintptr_t{0}, r.rtv);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    Call(r.device, kDevCreateShaderResourceView, P(r.texture), P(&srv), static_cast<uintptr_t>(Call(r.srv_heap, kHeapGetCpuStart)));

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 1;
    rs.pParameters = &param;
    ID3DBlob* rs_blob = nullptr;
    ID3DBlob* err = nullptr;
    SX_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &err)));
    SX_REQUIRE(Call(r.device, kDevCreateRootSignature, uintptr_t{0}, P(rs_blob->GetBufferPointer()),
                    static_cast<uintptr_t>(rs_blob->GetBufferSize()), uintptr_t{0}, P(&r.root)) == 0 && r.root);
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    SX_REQUIRE(SUCCEEDED(D3DCompile(kHlsl, std::strlen(kHlsl), "order.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs, &err)));
    SX_REQUIRE(SUCCEEDED(D3DCompile(kHlsl, std::strlen(kHlsl), "order.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &ps, &err)));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = static_cast<ID3D12RootSignature*>(r.root);
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xFFFFFFFFu;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    SX_REQUIRE(Call(r.device, kDevCreateGraphicsPipelineState, P(&pd), uintptr_t{0}, P(&r.pso)) == 0 && r.pso);

    const std::vector<uint32_t> a = Pattern(0xA), b = Pattern(0xB);

    // Written in four submissions running: from the second one on the shim leaves the texture's
    // write watch unarmed and tells writes apart by comparing the memory with the last upload.
    for (uint32_t warm = 1; warm <= 4; ++warm) {
        const std::vector<uint32_t> w = Pattern(100 + warm);
        r.GuestWrite(w);
        r.DrawAndExpect(w, "warm-up write", "the bytes just written");
    }
    r.GuestWrite(a);
    r.DrawAndExpect(a, "1 guest writes A", "A");
    r.GpuCopy(b);
    r.DrawAndExpect(b, "2 GPU copy of B", "B");
    r.GuestWrite(a);  // the same bytes as the last upload
    r.DrawAndExpect(a, "3 guest rewrites A", "A");
    r.GpuCopy(b);
    r.DrawAndExpect(b, "4 GPU copy of B again", "B");
    r.DrawAndExpect(b, "4 no guest write", "B");

    if (r.failures) {
        std::printf("test_d3d12_guest_texture_order: FAIL (%d step(s) showed the wrong bytes)\n", r.failures);
        return 1;
    }
    std::printf("test_d3d12_guest_texture_order: OK\n");
    return 0;
}
