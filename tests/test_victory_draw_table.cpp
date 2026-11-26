// A translated pixel shader that samples a texture through a guest descriptor table, synthetic
// only, on the guest-facing route: console-shaped containers handed to the d3d12_xs shim's
// CreateGraphicsPipelineState, root arguments and draws on the shim's command list, submitted on
// the shim's queue.
//
// The pixel program (bridge variant 17) samples the texture its table names at (x / 64, y / 64)
// with an immediate point sampler and adds its four root constants; the vertex program is the
// viewport-covering triangle (variant 9). Textures are 8x8, the target 64x64, so pixel (x, y)
// must hold texel (x / 8, y / 8) plus the constants. Texture A and texture B differ in every
// texel. The title's root signature: parameter 0 four root constants, parameter 1 a table with
// one shader resource view.
//
//   1  pipeline creation translates both stages
//   2  slot 2 views A; draw                                   -> every pixel A
//   3  slot 2 is given B's view after that draw completed     -> every pixel B
//   4  one list: table slot 3 (A) left half, table slot 2 (B)
//      right half with other constants, then the left half's
//      table and constants again for the top-left quarter     -> A | B + constants, binds counted
//   5  the table never set                                    -> draw dropped, target untouched
//   6  slot 5 holds a buffer's unordered access view          -> draw dropped, target untouched
//   7  slot 6 was never written                               -> draw dropped, target untouched
//   8  one list: translated left half, a desktop pipeline's
//      draw right half, translated again top-left quarter     -> A | colour, title root restored once
//   9  a pixel shader whose table view is not a texture       -> pipeline refused, with the reason
//  11  CopyDescriptorsSimple: A's view into the slot the
//      table names, then B's view over it                     -> A, then B
//  12  CopyDescriptors, two destination and two source
//      ranges, into slots at nonzero offsets                  -> the copied slots and their neighbours
//  13  a buffer's unordered access view copied over a
//      sampled slot, by each call                             -> draw dropped, target untouched
//  14  which descriptor writes make a draw check its view
//      again: other slots and heaps (never), the sampled
//      slot by each call, a heap switch, a reset list, a
//      slot reused after its texture was released, a UAV
//      copied over the sampled slot                           -> exact frames; views validated as counted
//  10  released in flight, each with objects of its own:
//      the pipeline, the descriptor heap, the title's root
//      signature, the texture                                 -> the draw still shows the texture
//
// The host device is used directly only for the render target, its clear and readback, and for
// filling the textures. Exit codes: 0 pass, 1 a check failed, 2 a call failed, 3 no device,
// 4 a fixture could not be built, 5 a submission did not complete.
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
#include "scarlettx/gpu/victory_shader_bridge.h"

namespace {

template <typename... Args>
intptr_t Call(void* object, int slot, Args... args) {
    using Fn = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, Args...);
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[slot])(object, args...);
}
uintptr_t P(const void* p) { return reinterpret_cast<uintptr_t>(p); }

constexpr UINT kSize = 64;
constexpr UINT kTexels = 8;
constexpr uint8_t kClear[4] = {16, 32, 48, 255};
constexpr UINT kRowPitch = 256;
const char* const kTag = "[victory-draw-table]";

uint32_t FloatBits(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    return bits;
}

// Texel (i, j) of texture `which` (0 A, 1 B): different in every texel and between the two.
void Texel(int which, UINT i, UINT j, uint8_t out[4]) {
    out[0] = static_cast<uint8_t>(which == 0 ? 8 * i + 3 : 200 - 8 * i);
    out[1] = static_cast<uint8_t>(which == 0 ? 8 * j + 5 : 190 - 8 * j);
    out[2] = which == 0 ? 0x40 : 0x80;
    out[3] = 200;
}

const char* kDesktopHlsl = R"(
cbuffer Draw : register(b0) { float4 colour; };
float4 VSMain(uint vid : SV_VertexID) : SV_POSITION {
    return float4(float((vid << 1) & 2) * 2.0f - 1.0f, float(vid & 2) * 2.0f - 1.0f, 0.0f, 1.0f);
}
float4 PSMain() : SV_TARGET { return colour; }
// Slow on purpose: a few of these keep the queue busy while the next list waits behind them.
float4 PSBusy(float4 pos : SV_POSITION) : SV_TARGET {
    uint v = uint(pos.x) * 0x9E3779B1u ^ uint(pos.y) * 0x85EBCA77u;
    for (uint i = 0u; i < 8192u; ++i) {
        v ^= v << 13;
        v ^= v >> 17;
        v ^= v << 5;
    }
    return float4(float(v & 255u) / 255.0f, 0.0f, 0.0f, 1.0f);
}
)";

struct Region {
    LONG left, top, right, bottom;
};
const Region kWhole{0, 0, kSize, kSize};
const Region kLeft{0, 0, kSize / 2, kSize};
const Region kRight{kSize / 2, 0, kSize, kSize};
const Region kTopLeft{0, 0, kSize / 2, kSize / 2};

struct Rig {
    void* shim_device = nullptr;
    ID3D12Device* device = nullptr;
    void* queue = nullptr;
    void* fence = nullptr;
    HANDLE event = nullptr;
    uint64_t fence_value = 0;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12Resource* target = nullptr;
    ID3D12Resource* readback = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    UINT increment = 0;
    int failures = 0;
    bool stuck = false;

    void Check(bool ok, const char* what) {
        std::printf("%s %s: %s\n", kTag, ok ? "ok" : "CHECK FAILED", what);
        if (!ok) ++failures;
    }
    int Fail(const char* api, HRESULT hr) const {
        std::printf("%s FAILED api=%s hr=0x%08x\n", kTag, api, static_cast<uint32_t>(hr));
        return 2;
    }

    // The title's root signature: four root constants, then a table with one shader resource view.
    void* TitleRoot() {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 4;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &range;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 2;
        desc.pParameters = params;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        SX_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)));
        void* root = nullptr;
        SX_REQUIRE(Call(shim_device, 16, uintptr_t{0}, P(blob->GetBufferPointer()),
                        static_cast<uintptr_t>(blob->GetBufferSize()), uintptr_t{0}, P(&root)) == 0 && root);
        blob->Release();
        return root;
    }

    HRESULT CreatePipeline(void* root, const std::vector<uint8_t>& vs, const std::vector<uint8_t>& ps, void** out_pso) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = static_cast<ID3D12RootSignature*>(root);
        desc.VS = {vs.data(), vs.size()};
        desc.PS = {ps.data(), ps.size()};
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = 0xFFFFFFFFu;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        *out_pso = nullptr;
        return static_cast<HRESULT>(Call(shim_device, 10, P(&desc), uintptr_t{0}, P(out_pso)));
    }

    struct Heap {
        void* proxy = nullptr;
        uintptr_t cpu = 0;
        uintptr_t gpu = 0;
    };
    Heap ViewHeap(UINT count) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = count;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Heap heap;
        SX_REQUIRE(Call(shim_device, 14, P(&desc), uintptr_t{0}, P(&heap.proxy)) == 0 && heap.proxy);
        heap.cpu = static_cast<uintptr_t>(Call(heap.proxy, 9));
        heap.gpu = static_cast<uintptr_t>(Call(heap.proxy, 10));
        return heap;
    }

    // An 8x8 RGBA8 texture made on the shim's device and filled on the host.
    void* Texture(int which) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kTexels;
        desc.Height = kTexels;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        void* proxy = nullptr;
        SX_REQUIRE(Call(shim_device, 27, P(&heap), uintptr_t{0}, P(&desc), static_cast<uintptr_t>(D3D12_RESOURCE_STATE_COPY_DEST),
                        uintptr_t{0}, uintptr_t{0}, P(&proxy)) == 0 && proxy);
        ID3D12Resource* native = nullptr;
        SX_REQUIRE(D3D12XboxGetNativeObject(proxy, reinterpret_cast<void**>(&native)) == 0 && native);

        D3D12_HEAP_PROPERTIES upload_heap{};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buf{};
        buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf.Width = static_cast<UINT64>(kRowPitch) * kTexels;
        buf.Height = 1;
        buf.DepthOrArraySize = 1;
        buf.MipLevels = 1;
        buf.SampleDesc.Count = 1;
        buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* upload = nullptr;
        SX_REQUIRE(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buf,
                                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))));
        uint8_t* mapped = nullptr;
        SX_REQUIRE(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) && mapped);
        for (UINT j = 0; j < kTexels; ++j) {
            for (UINT i = 0; i < kTexels; ++i) Texel(which, i, j, mapped + j * kRowPitch + i * 4);
        }
        upload->Unmap(0, nullptr);

        void* shim_list = NewList();
        ID3D12GraphicsCommandList* list = Native(shim_list);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = native;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = kTexels;
        src.PlacedFootprint.Footprint.Height = kTexels;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = kRowPitch;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = native;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
        SX_REQUIRE(Wait(Submit(shim_list)));
        upload->Release();
        native->Release();
        return proxy;
    }

    // CreateShaderResourceView on the shim's device (slot 18): the texture's view into `slot`.
    void TextureView(const Heap& heap, UINT slot, void* texture) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        Call(shim_device, 18, P(texture), P(&view), heap.cpu + static_cast<uintptr_t>(slot) * increment);
    }

    void* NewList() {
        void* allocator = nullptr;
        void* list = nullptr;
        SX_REQUIRE(Call(shim_device, 9, static_cast<uintptr_t>(D3D12_COMMAND_LIST_TYPE_DIRECT), uintptr_t{0}, P(&allocator)) == 0);
        SX_REQUIRE(Call(shim_device, 12, uintptr_t{0}, static_cast<uintptr_t>(D3D12_COMMAND_LIST_TYPE_DIRECT), P(allocator),
                        uintptr_t{0}, uintptr_t{0}, P(&list)) == 0 && list);
        return list;
    }
    // A list together with its allocator, for a title that resets and records again.
    void* NewListKeepingAllocator(void** out_allocator) {
        void* list = nullptr;
        SX_REQUIRE(Call(shim_device, 9, static_cast<uintptr_t>(D3D12_COMMAND_LIST_TYPE_DIRECT), uintptr_t{0}, P(out_allocator)) == 0);
        SX_REQUIRE(Call(shim_device, 12, uintptr_t{0}, static_cast<uintptr_t>(D3D12_COMMAND_LIST_TYPE_DIRECT), P(*out_allocator),
                        uintptr_t{0}, uintptr_t{0}, P(&list)) == 0 && list);
        return list;
    }
    ID3D12GraphicsCommandList* Native(void* shim_list) {
        ID3D12GraphicsCommandList* list = nullptr;
        SX_REQUIRE(D3D12XboxGetNativeObject(shim_list, reinterpret_cast<void**>(&list)) == 0 && list);
        list->Release();  // the shim's list keeps it
        return list;
    }
    uint64_t Submit(void* shim_list) {
        SX_REQUIRE(Call(shim_list, 9) == 0);  // Close
        void* lists[] = {shim_list};
        Call(queue, 10, uintptr_t{1}, P(lists), uintptr_t{0});  // ExecuteCommandLists
        const uint64_t value = ++fence_value;
        SX_REQUIRE(Call(queue, 14, P(fence), static_cast<uintptr_t>(value), uintptr_t{0}) == 0);  // Signal
        return value;
    }
    bool Wait(uint64_t value) {
        DWORD wait = 0;
        if (static_cast<uint64_t>(Call(fence, 8)) < value) {
            SX_REQUIRE(Call(fence, 9, static_cast<uintptr_t>(value), P(event), uintptr_t{0}) == 0);
            wait = WaitForSingleObject(event, 15000);
        }
        const uint64_t completed = static_cast<uint64_t>(Call(fence, 8));
        std::printf("%s fence signaled=%llu completed=%llu wait_result=%lu\n", kTag, static_cast<unsigned long long>(value),
                    static_cast<unsigned long long>(completed), static_cast<unsigned long>(wait));
        if (completed < value) stuck = true;
        return completed >= value;
    }

    // What a title sets before its draws: the target, the heap, its root signature, the state.
    void Begin(void* shim_list, const Heap* heap, void* root) {
        ID3D12GraphicsCommandList* list = Native(shim_list);
        float clear[4];
        for (int i = 0; i < 4; ++i) clear[i] = static_cast<float>(kClear[i]) / 255.0f;
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f};
        Call(shim_list, 50, uintptr_t{1}, P(&rtv), uintptr_t{0}, uintptr_t{0});  // OMSetRenderTargets
        if (heap) {
            void* heaps[] = {heap->proxy};
            Call(shim_list, 32, uintptr_t{1}, P(heaps), uintptr_t{0});           // SetDescriptorHeaps
        }
        Call(shim_list, 34, P(root), uintptr_t{0}, uintptr_t{0});                // SetGraphicsRootSignature
        Call(shim_list, 20, static_cast<uintptr_t>(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST), uintptr_t{0}, uintptr_t{0});
        Call(shim_list, 21, uintptr_t{1}, P(&vp), uintptr_t{0});                 // RSSetViewports
    }
    void Pipeline(void* shim_list, void* pso) { Call(shim_list, 25, P(pso), uintptr_t{0}, uintptr_t{0}); }
    void Constants(void* shim_list, const uint8_t add[4]) {
        uint32_t c[4];
        for (int i = 0; i < 4; ++i) c[i] = FloatBits(static_cast<float>(add[i]) / 255.0f);
        Call(shim_list, 40, uintptr_t{0}, uintptr_t{4}, P(c), uintptr_t{0});     // SetGraphicsRoot32BitConstants
    }
    void Table(void* shim_list, const Heap& heap, UINT slot) {
        Call(shim_list, 36, uintptr_t{1}, heap.gpu + static_cast<uintptr_t>(slot) * increment, uintptr_t{0});
    }
    void DrawIn(void* shim_list, const Region& r) {
        const D3D12_RECT scissor{r.left, r.top, r.right, r.bottom};
        Call(shim_list, 22, uintptr_t{1}, P(&scissor), uintptr_t{0});            // RSSetScissorRects
        Call(shim_list, 12, uintptr_t{3}, uintptr_t{1}, uintptr_t{0}, uintptr_t{0});  // DrawInstanced
    }
    // Copies the target out at the end of the list and submits it. The frame is read by Frame().
    uint64_t End(void* shim_list) {
        ID3D12GraphicsCommandList* list = Native(shim_list);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = target;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = target;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width = kSize;
        dst.PlacedFootprint.Footprint.Height = kSize;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = kRowPitch;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        return Submit(shim_list);
    }
    std::vector<uint8_t> Frame() {
        std::vector<uint8_t> out(static_cast<size_t>(kSize) * kSize * 4);
        void* mapped = nullptr;
        SX_REQUIRE(SUCCEEDED(readback->Map(0, nullptr, &mapped)) && mapped);
        for (UINT y = 0; y < kSize; ++y) {
            std::memcpy(out.data() + static_cast<size_t>(y) * kSize * 4, static_cast<const uint8_t*>(mapped) + static_cast<size_t>(y) * kRowPitch,
                        kSize * 4);
        }
        readback->Unmap(0, nullptr);
        return out;
    }

    int Init() {
        _putenv_s("SCARLETTX_REAL_TITLE", "1");
        SetEnvironmentVariableA("SCARLETTX_REAL_TITLE", "1");
        D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
        params.Version = 1;
        params.PreferWarp = 0;
        const int32_t created = D3D12XboxCreateDevice(nullptr, &params, &__uuidof(ID3D12Device), &shim_device);
        if (created < 0 || !shim_device) {
            std::printf("%s D3D12XboxCreateDevice FAILED hr=0x%08x\n", kTag, static_cast<uint32_t>(created));
            return 3;
        }
        SX_REQUIRE(D3D12XboxGetNativeObject(shim_device, reinterpret_cast<void**>(&device)) == 0 && device);
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        SX_REQUIRE(Call(shim_device, 8, P(&queue_desc), uintptr_t{0}, P(&queue)) == 0 && queue);
        SX_REQUIRE(Call(shim_device, 36, uintptr_t{0}, uintptr_t{0}, uintptr_t{0}, P(&fence)) == 0 && fence);
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.NumDescriptors = 1;
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        HRESULT hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap));
        if (FAILED(hr)) return Fail("CreateDescriptorHeap(RTV)", hr);
        rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC tex{};
        tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tex.Width = kSize;
        tex.Height = kSize;
        tex.DepthOrArraySize = 1;
        tex.MipLevels = 1;
        tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        tex.SampleDesc.Count = 1;
        tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        for (int i = 0; i < 4; ++i) clear.Color[i] = static_cast<float>(kClear[i]) / 255.0f;
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                             IID_PPV_ARGS(&target));
        if (FAILED(hr)) return Fail("CreateCommittedResource(target)", hr);
        device->CreateRenderTargetView(target, nullptr, rtv);
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
        hr = device->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&readback));
        if (FAILED(hr)) return Fail("CreateCommittedResource(readback)", hr);
        return 0;
    }
};

// What a region of the frame should hold.
struct Want {
    Region region;
    int texture;            // 0 A, 1 B, -1 a flat colour, -2 the clear colour
    uint8_t add[4];         // added to the texel, or the flat colour
};

// Compares the frame with the expectations (later entries cover earlier ones; what none covers
// must be the clear colour). Returns the number of exact pixels.
uint32_t Exact(const std::vector<uint8_t>& frame, const std::vector<Want>& wants, const char* what) {
    uint32_t exact = 0;
    long first = -1;
    uint8_t first_want[4] = {};
    for (UINT y = 0; y < kSize; ++y) {
        for (UINT x = 0; x < kSize; ++x) {
            uint8_t want[4];
            std::memcpy(want, kClear, 4);
            for (const Want& w : wants) {
                if (static_cast<LONG>(x) < w.region.left || static_cast<LONG>(x) >= w.region.right ||
                    static_cast<LONG>(y) < w.region.top || static_cast<LONG>(y) >= w.region.bottom) {
                    continue;
                }
                if (w.texture == -2) {
                    std::memcpy(want, kClear, 4);
                } else if (w.texture == -1) {
                    std::memcpy(want, w.add, 4);
                } else {
                    Texel(w.texture, x / (kSize / kTexels), y / (kSize / kTexels), want);
                    for (int c = 0; c < 4; ++c) want[c] = static_cast<uint8_t>(want[c] + w.add[c]);
                }
            }
            const uint8_t* got = &frame[(static_cast<size_t>(y) * kSize + x) * 4];
            if (std::memcmp(got, want, 4) == 0) {
                ++exact;
            } else if (first < 0) {
                first = static_cast<long>(y) * kSize + x;
                std::memcpy(first_want, want, 4);
            }
        }
    }
    std::printf("%s %s: pixels exact %u/%u", kTag, what, exact, kSize * kSize);
    if (first >= 0) {
        const uint8_t* got = &frame[static_cast<size_t>(first) * 4];
        std::printf(" (first mismatch at %ld,%ld: got %u,%u,%u,%u want %u,%u,%u,%u)", first % kSize, first / kSize, got[0], got[1],
                    got[2], got[3], first_want[0], first_want[1], first_want[2], first_want[3]);
    }
    std::printf("\n");
    return exact;
}

D3D12XBOX_GRAPHICS_TRANSLATION_STATS Stats() {
    D3D12XBOX_GRAPHICS_TRANSLATION_STATS s{};
    SX_REQUIRE(D3D12XboxGetGraphicsTranslationStats(&s) == 0);
    return s;
}
D3D12XBOX_TRANSLATED_DRAW_BIND_STATS Binds() {
    D3D12XBOX_TRANSLATED_DRAW_BIND_STATS s{};
    SX_REQUIRE(D3D12XboxGetTranslatedDrawBindStats(&s) == 0);
    return s;
}

D3D12XBOX_TRANSLATED_VIEW_CHECK_STATS ViewChecks() {
    D3D12XBOX_TRANSLATED_VIEW_CHECK_STATS s{};
    SX_REQUIRE(D3D12XboxGetTranslatedViewCheckStats(&s) == 0);
    return s;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::vector<uint8_t> vs, ps, ps_buffer_table;
    if (!scarlettx::BuildVictorySyntheticContainer(9, vs).ok() || !scarlettx::BuildVictorySyntheticContainer(17, ps).ok() ||
        !scarlettx::BuildVictorySyntheticContainer(12, ps_buffer_table).ok()) {
        std::printf("%s the synthetic containers could not be built\n", kTag);
        return 4;
    }
    Rig r;
    if (const int rc = r.Init()) return rc;
    const uint32_t all = kSize * kSize;
    const uint8_t zero[4] = {0, 0, 0, 0};
    const uint8_t plus[4] = {10, 20, 30, 40};
    uint32_t pixels_exact = 0;
    uint32_t pixels_total = 0;
    auto frame_check = [&](const std::vector<Want>& wants, const char* what) {
        const uint32_t exact = Exact(r.Frame(), wants, what);
        pixels_exact += exact;
        pixels_total += all;
        r.Check(exact == all, what);
    };

    // 1. Pipeline creation.
    void* root = r.TitleRoot();
    auto before = Stats();
    void* pso = nullptr;
    HRESULT hr = r.CreatePipeline(root, vs, ps, &pso);
    auto after = Stats();
    std::printf("%s shim CreateGraphicsPipelineState(vertex, textured pixel) hr=0x%08x translated=+%llu rejected=+%llu\n", kTag,
                static_cast<uint32_t>(hr), static_cast<unsigned long long>(after.graphics_translated - before.graphics_translated),
                static_cast<unsigned long long>(after.graphics_rejected - before.graphics_rejected));
    if (FAILED(hr) || !pso) return r.Fail("shim CreateGraphicsPipelineState (slot 10), textured pixel shader", hr);
    r.Check(after.graphics_translated - before.graphics_translated == 1, "1 the textured pipeline was translated at creation");

    Rig::Heap heap = r.ViewHeap(8);
    void* tex_a = r.Texture(0);
    void* tex_b = r.Texture(1);
    r.TextureView(heap, 2, tex_a);
    r.TextureView(heap, 3, tex_a);

    // 2. Slot 2 views A.
    {
        void* list = r.NewList();
        r.Begin(list, &heap, root);
        r.Pipeline(list, pso);
        r.Constants(list, zero);
        r.Table(list, heap, 2);
        r.DrawIn(list, kWhole);
        if (!r.Wait(r.End(list))) return 5;
        frame_check({{kWhole, 0, {0, 0, 0, 0}}}, "2 slot 2 views texture A");
    }
    // 3. The same slot is given B's view after the first draw completed.
    r.TextureView(heap, 2, tex_b);
    {
        void* list = r.NewList();
        r.Begin(list, &heap, root);
        r.Pipeline(list, pso);
        r.Constants(list, zero);
        r.Table(list, heap, 2);
        r.DrawIn(list, kWhole);
        if (!r.Wait(r.End(list))) return 5;
        frame_check({{kWhole, 1, {0, 0, 0, 0}}}, "3 slot 2 was given texture B's view");
    }
    // 4. One list: two tables, new constants, and the first bindings again.
    {
        const auto binds_before = Binds();
        const auto stats_before = Stats();
        void* list = r.NewList();
        r.Begin(list, &heap, root);
        r.Pipeline(list, pso);
        r.Constants(list, zero);
        r.Table(list, heap, 3);
        r.DrawIn(list, kLeft);       // A
        r.Table(list, heap, 2);
        r.Constants(list, plus);
        r.DrawIn(list, kRight);      // B + constants
        r.DrawIn(list, kRight);      // the same bindings again: nothing to put on the list
        r.Table(list, heap, 3);
        r.DrawIn(list, kTopLeft);    // A + constants
        if (!r.Wait(r.End(list))) return 5;
        frame_check({{kLeft, 0, {0, 0, 0, 0}}, {kRight, 1, {10, 20, 30, 40}}, {kTopLeft, 0, {10, 20, 30, 40}}},
                    "4 tables switched between draws in one list");
        const auto binds = Binds();
        const auto stats = Stats();
        std::printf("%s 4 draws translated=+%llu skipped=+%llu host_root_binds=+%llu user_data_sets=+%llu title_root_restores=+%llu\n",
                    kTag, static_cast<unsigned long long>(stats.draws_translated - stats_before.draws_translated),
                    static_cast<unsigned long long>(stats.draws_skipped - stats_before.draws_skipped),
                    static_cast<unsigned long long>(binds.host_root_binds - binds_before.host_root_binds),
                    static_cast<unsigned long long>(binds.user_data_sets - binds_before.user_data_sets),
                    static_cast<unsigned long long>(binds.title_root_restores - binds_before.title_root_restores));
        r.Check(stats.draws_translated - stats_before.draws_translated == 4 && stats.draws_skipped == stats_before.draws_skipped,
                "4 four translated draws, none dropped");
        // One host root signature for the list; the pixel stage's user data three times (the
        // repeated draw sets nothing), the vertex stage's once.
        r.Check(binds.host_root_binds - binds_before.host_root_binds == 1 && binds.user_data_sets - binds_before.user_data_sets == 4,
                "4 bindings are reused: one host root bind, the repeated draw sets nothing");
    }
    // 5 to 7. Draws that must be dropped, each with its reason on stderr.
    struct Dropped {
        const char* what;
        int table_slot;  // -1: the table is not set
    };
    {
        // Slot 5: a buffer's unordered access view. Slot 6: never written.
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 256;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        void* buffer = nullptr;
        SX_REQUIRE(Call(r.shim_device, 27, P(&hp), uintptr_t{0}, P(&bd), static_cast<uintptr_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                        uintptr_t{0}, uintptr_t{0}, P(&buffer)) == 0 && buffer);
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = 64;
        view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        Call(r.shim_device, 19, P(buffer), uintptr_t{0}, P(&view), heap.cpu + static_cast<uintptr_t>(5) * r.increment);
    }
    for (const Dropped& d : {Dropped{"5 the descriptor table was never set", -1},
                             Dropped{"6 the table names a buffer's unordered access view", 5},
                             Dropped{"7 the table names a descriptor that was never written", 6}}) {
        const auto stats_before = Stats();
        void* list = r.NewList();
        r.Begin(list, &heap, root);
        r.Pipeline(list, pso);
        r.Constants(list, zero);
        if (d.table_slot >= 0) r.Table(list, heap, static_cast<UINT>(d.table_slot));
        r.DrawIn(list, kWhole);
        if (!r.Wait(r.End(list))) return 5;
        const auto stats = Stats();
        frame_check({}, d.what);
        r.Check(stats.draws_skipped - stats_before.draws_skipped == 1 && stats.draws_translated == stats_before.draws_translated,
                "the draw was dropped, not run");
    }
    // 8. A desktop pipeline between two translated draws in one list.
    void* desktop_pso = nullptr;
    void* busy_pso = nullptr;
    {
        ID3DBlob* dvs = nullptr;
        ID3DBlob* dps = nullptr;
        ID3DBlob* dbusy = nullptr;
        ID3DBlob* errors = nullptr;
        hr = D3DCompile(kDesktopHlsl, std::strlen(kDesktopHlsl), "desktop.hlsl", nullptr, nullptr, "PSBusy", "ps_5_0", 0, 0, &dbusy, &errors);
        if (FAILED(hr)) return r.Fail("D3DCompile(PSBusy)", hr);
        hr = D3DCompile(kDesktopHlsl, std::strlen(kDesktopHlsl), "desktop.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &dvs, &errors);
        if (FAILED(hr)) return r.Fail("D3DCompile(VSMain)", hr);
        hr = D3DCompile(kDesktopHlsl, std::strlen(kDesktopHlsl), "desktop.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &dps, &errors);
        if (FAILED(hr)) return r.Fail("D3DCompile(PSMain)", hr);
        const auto* v = static_cast<const uint8_t*>(dvs->GetBufferPointer());
        const auto* p = static_cast<const uint8_t*>(dps->GetBufferPointer());
        hr = r.CreatePipeline(root, std::vector<uint8_t>(v, v + dvs->GetBufferSize()), std::vector<uint8_t>(p, p + dps->GetBufferSize()),
                              &desktop_pso);
        if (FAILED(hr) || !desktop_pso) return r.Fail("shim CreateGraphicsPipelineState (slot 10), desktop shaders", hr);
        const auto* b = static_cast<const uint8_t*>(dbusy->GetBufferPointer());
        hr = r.CreatePipeline(root, std::vector<uint8_t>(v, v + dvs->GetBufferSize()), std::vector<uint8_t>(b, b + dbusy->GetBufferSize()),
                              &busy_pso);
        if (FAILED(hr) || !busy_pso) return r.Fail("shim CreateGraphicsPipelineState (slot 10), busy shader", hr);
    }
    {
        const uint8_t colour[4] = {200, 100, 50, 25};
        const auto binds_before = Binds();
        void* list = r.NewList();
        r.Begin(list, &heap, root);
        r.Pipeline(list, pso);
        r.Constants(list, zero);
        r.Table(list, heap, 3);
        r.DrawIn(list, kLeft);            // translated: A
        r.Pipeline(list, desktop_pso);
        r.Constants(list, colour);
        r.DrawIn(list, kRight);           // desktop: the colour, from the title's own root
        r.Pipeline(list, pso);
        r.Constants(list, plus);
        r.DrawIn(list, kTopLeft);         // translated again: A + constants, the table still set
        if (!r.Wait(r.End(list))) return 5;
        frame_check({{kLeft, 0, {0, 0, 0, 0}}, {kRight, -1, {200, 100, 50, 25}}, {kTopLeft, 0, {10, 20, 30, 40}}},
                    "8 a desktop pipeline's draw between two translated draws");
        const auto binds = Binds();
        r.Check(binds.title_root_restores - binds_before.title_root_restores == 1 &&
                    binds.host_root_binds - binds_before.host_root_binds == 2,
                "8 the title's root was put back once, the host root bound twice");
    }
    // 9. A pixel shader whose table view is not a texture is refused at pipeline creation.
    {
        before = Stats();
        void* refused = nullptr;
        hr = r.CreatePipeline(root, vs, ps_buffer_table, &refused);
        after = Stats();
        std::printf("%s shim CreateGraphicsPipelineState(pixel shader with a buffer view in its table) hr=0x%08x rejected=+%llu\n", kTag,
                    static_cast<uint32_t>(hr), static_cast<unsigned long long>(after.graphics_rejected - before.graphics_rejected));
        r.Check(hr == static_cast<HRESULT>(0x80004001u) && !refused && after.graphics_rejected - before.graphics_rejected == 1,
                "9 a table view that is not a texture is refused at pipeline creation");
    }
    // 11 to 13. Descriptor copies. The views are made in a source heap and reach the heap the
    // table points into only through the device's CopyDescriptorsSimple (slot 24) and
    // CopyDescriptors (slot 23). Source: 0 A, 1 B, 2 a buffer's unordered access view.
    {
        Rig::Heap src = r.ViewHeap(4);
        Rig::Heap dst = r.ViewHeap(12);
        r.TextureView(src, 0, tex_a);
        r.TextureView(src, 1, tex_b);
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = 256;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            void* buffer = nullptr;
            SX_REQUIRE(Call(r.shim_device, 27, P(&hp), uintptr_t{0}, P(&bd), static_cast<uintptr_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                            uintptr_t{0}, uintptr_t{0}, P(&buffer)) == 0 && buffer);
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            view.Buffer.NumElements = 64;
            view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            Call(r.shim_device, 19, P(buffer), uintptr_t{0}, P(&view), src.cpu + static_cast<uintptr_t>(2) * r.increment);
        }
        const uintptr_t kViews = static_cast<uintptr_t>(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto at = [&](const Rig::Heap& h, UINT slot) { return h.cpu + static_cast<uintptr_t>(slot) * r.increment; };
        auto copy_simple = [&](UINT dst_slot, UINT src_slot) {
            Call(r.shim_device, 24, uintptr_t{1}, at(dst, dst_slot), at(src, src_slot), kViews);
        };
        // One frame: the table on `slot`, the whole target.
        auto draw_slot = [&](UINT slot) {
            void* list = r.NewList();
            r.Begin(list, &dst, root);
            r.Pipeline(list, pso);
            r.Constants(list, zero);
            r.Table(list, dst, slot);
            r.DrawIn(list, kWhole);
            return r.Wait(r.End(list));
        };

        // 11. CopyDescriptorsSimple: A's view into the slot the table names, then B's over it.
        copy_simple(4, 0);
        if (!draw_slot(4)) return 5;
        frame_check({{kWhole, 0, {0, 0, 0, 0}}}, "11 CopyDescriptorsSimple put A's view in slot 4");
        copy_simple(4, 1);
        if (!draw_slot(4)) return 5;
        frame_check({{kWhole, 1, {0, 0, 0, 0}}}, "11 CopyDescriptorsSimple put B's view over it");

        // 12. CopyDescriptors with several ranges. The neighbours 7, 8 and 11 are written first,
        //     directly; then destination ranges {9, 10} and {6} take source ranges {1} and {0, 1}:
        //     9 <- B, 10 <- A, 6 <- B.
        r.TextureView(dst, 7, tex_b);
        r.TextureView(dst, 8, tex_a);
        r.TextureView(dst, 11, tex_a);
        {
            const D3D12_CPU_DESCRIPTOR_HANDLE dst_starts[2] = {{at(dst, 9)}, {at(dst, 6)}};
            const UINT dst_sizes[2] = {2, 1};
            const D3D12_CPU_DESCRIPTOR_HANDLE src_starts[2] = {{at(src, 1)}, {at(src, 0)}};
            const UINT src_sizes[2] = {1, 2};
            Call(r.shim_device, 23, uintptr_t{2}, P(dst_starts), P(dst_sizes), uintptr_t{2}, P(src_starts), P(src_sizes), kViews);
        }
        {
            // Six draws in one list, a cell of the target each: the copied slots and their neighbours.
            struct Cell {
                UINT slot;
                int texture;
            };
            const Cell cells[6] = {{9, 1}, {10, 0}, {6, 1}, {8, 0}, {11, 0}, {7, 1}};
            const auto stats_before = Stats();
            void* list = r.NewList();
            r.Begin(list, &dst, root);
            r.Pipeline(list, pso);
            r.Constants(list, zero);
            std::vector<Want> wants;
            for (int i = 0; i < 6; ++i) {
                const Region cell{16 * (i % 4), 32 * (i / 4), 16 * (i % 4) + 16, 32 * (i / 4) + 32};
                r.Table(list, dst, cells[i].slot);
                r.DrawIn(list, cell);
                wants.push_back({cell, cells[i].texture, {0, 0, 0, 0}});
            }
            if (!r.Wait(r.End(list))) return 5;
            frame_check(wants, "12 CopyDescriptors, two ranges each way: slots 9, 10, 6 copied; 8, 11, 7 intact");
            const auto stats = Stats();
            r.Check(stats.draws_translated - stats_before.draws_translated == 6 && stats.draws_skipped == stats_before.draws_skipped,
                    "12 six translated draws, none dropped");
        }

        // 13. A valid view of the wrong kind copied over a sampled slot: the next draw is dropped.
        struct Wrong {
            const char* what;
            UINT slot;
            bool simple;
        };
        for (const Wrong& w : {Wrong{"13 CopyDescriptors put a buffer's unordered access view over slot 9", 9, false},
                               Wrong{"13 CopyDescriptorsSimple put a buffer's unordered access view over slot 4", 4, true}}) {
            if (w.simple) {
                copy_simple(w.slot, 2);
            } else {
                const D3D12_CPU_DESCRIPTOR_HANDLE dst_start{at(dst, w.slot)};
                const D3D12_CPU_DESCRIPTOR_HANDLE src_start{at(src, 2)};
                const UINT one = 1;
                Call(r.shim_device, 23, uintptr_t{1}, P(&dst_start), P(&one), uintptr_t{1}, P(&src_start), P(&one), kViews);
            }
            const auto stats_before = Stats();
            if (!draw_slot(w.slot)) return 5;
            const auto stats = Stats();
            frame_check({}, w.what);
            r.Check(stats.draws_skipped - stats_before.draws_skipped == 1 && stats.draws_translated == stats_before.draws_translated,
                    "13 the draw was dropped, not run");
        }
        // And the slot beside the overwritten one still holds its texture.
        if (!draw_slot(10)) return 5;
        frame_check({{kWhole, 0, {0, 0, 0, 0}}}, "13 slot 10, beside the overwritten slot 9, still views A");
    }

    // 14. Which descriptor writes make a translated draw look at its binding again. A draw whose
    //     arguments did not change must check its view again only when a descriptor it uses was
    //     written, not when any other descriptor was.
    {
        Rig::Heap src = r.ViewHeap(4);
        Rig::Heap own = r.ViewHeap(12);
        Rig::Heap other = r.ViewHeap(12);
        r.TextureView(src, 0, tex_a);
        r.TextureView(src, 1, tex_b);
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = 256;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            void* buffer = nullptr;
            SX_REQUIRE(Call(r.shim_device, 27, P(&hp), uintptr_t{0}, P(&bd), static_cast<uintptr_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                            uintptr_t{0}, uintptr_t{0}, P(&buffer)) == 0 && buffer);
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            view.Buffer.NumElements = 64;
            view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            Call(r.shim_device, 19, P(buffer), uintptr_t{0}, P(&view), src.cpu + static_cast<uintptr_t>(2) * r.increment);
        }
        const uintptr_t kViews = static_cast<uintptr_t>(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto at = [&](const Rig::Heap& h, UINT slot) { return h.cpu + static_cast<uintptr_t>(slot) * r.increment; };
        auto copy_simple = [&](const Rig::Heap& h, UINT dst_slot, UINT src_slot) {
            Call(r.shim_device, 24, uintptr_t{1}, at(h, dst_slot), at(src, src_slot), kViews);
        };
        // CopyDescriptors with two destination ranges, {first} and {second, second + 1}, from the
        // source ranges {src_first} and {src_second, src_second} (the last two one at a time).
        auto copy_ranges = [&](const Rig::Heap& h, UINT first, UINT second, UINT src_first, UINT src_second) {
            const D3D12_CPU_DESCRIPTOR_HANDLE dst_starts[2] = {{at(h, first)}, {at(h, second)}};
            const UINT dst_sizes[2] = {1, 2};
            const D3D12_CPU_DESCRIPTOR_HANDLE src_starts[3] = {{at(src, src_first)}, {at(src, src_second)}, {at(src, src_second)}};
            const UINT src_sizes[3] = {1, 1, 1};
            Call(r.shim_device, 23, uintptr_t{2}, P(dst_starts), P(dst_sizes), uintptr_t{3}, P(src_starts), P(src_sizes), kViews);
        };
        const Region kNothing{0, 0, 0, 0};  // a draw that shades no pixel: it only makes the binding
        auto cell = [](int i) { return Region{16 * (i % 4), 32 * (i / 4), 16 * (i % 4) + 16, 32 * (i / 4) + 32}; };
        auto begin = [&](void* list, const Rig::Heap& h, UINT slot) {
            r.Begin(list, &h, root);
            r.Pipeline(list, pso);
            r.Constants(list, zero);
            r.Table(list, h, slot);
        };
        auto validated_since = [&](const D3D12XBOX_TRANSLATED_VIEW_CHECK_STATS& before_checks) {
            return static_cast<long long>(ViewChecks().views_validated - before_checks.views_validated);
        };

        // 14a. Unrelated descriptors written between draws, three ways: the binding stands.
        r.TextureView(own, 2, tex_a);
        {
            const auto checks = ViewChecks();
            void* list = r.NewList();
            begin(list, own, 2);
            r.DrawIn(list, cell(0));
            r.TextureView(own, 7, tex_b);          // CreateShaderResourceView into another slot
            r.DrawIn(list, cell(1));
            copy_simple(own, 8, 1);                // CopyDescriptorsSimple into another slot
            r.DrawIn(list, cell(2));
            copy_ranges(own, 5, 9, 1, 2);          // CopyDescriptors into 5 and 9, 10 (a UAV among them)
            r.DrawIn(list, cell(3));
            r.TextureView(other, 2, tex_b);        // the same index in another heap
            r.DrawIn(list, cell(4));
            if (!r.Wait(r.End(list))) return 5;
            frame_check({{cell(0), 0, {0, 0, 0, 0}}, {cell(1), 0, {0, 0, 0, 0}}, {cell(2), 0, {0, 0, 0, 0}}, {cell(3), 0, {0, 0, 0, 0}},
                         {cell(4), 0, {0, 0, 0, 0}}},
                        "14a five draws of A with other descriptors written in between");
            const long long validated = validated_since(checks);
            std::printf("%s 14a views validated: %lld (want 1: the first draw)\n", kTag, validated);
            r.Check(validated == 1, "14a writes to descriptors the draw does not use leave its binding alone");
        }
        // 14b. The sampled descriptor overwritten with B's view, three ways: checked again, and B.
        for (int way = 0; way < 3; ++way) {
            static const char* const kWays[] = {"14b the sampled slot rewritten by CreateShaderResourceView",
                                                "14b the sampled slot rewritten by CopyDescriptorsSimple",
                                                "14b the sampled slot rewritten by CopyDescriptors (second range)"};
            const UINT slot = 3;
            r.TextureView(own, slot, tex_a);
            r.TextureView(own, slot + 1, tex_a);
            const auto checks = ViewChecks();
            void* list = r.NewList();
            begin(list, own, slot);
            r.DrawIn(list, kNothing);              // the binding is made, and found to be A's view
            if (way == 0) r.TextureView(own, slot, tex_b);
            else if (way == 1) copy_simple(own, slot, 1);
            else copy_ranges(own, 11, slot, 0, 1); // 11 <- A; slot and slot + 1 <- B
            r.DrawIn(list, kWhole);
            if (!r.Wait(r.End(list))) return 5;
            frame_check({{kWhole, 1, {0, 0, 0, 0}}}, kWays[way]);
            const long long validated = validated_since(checks);
            std::printf("%s %s: views validated %lld (want 2)\n", kTag, kWays[way], validated);
            r.Check(validated == 2, "14b a write to the sampled descriptor is checked again");
        }
        // 14c. A heap switch in one list: the same index in another heap is another view.
        r.TextureView(own, 2, tex_a);
        r.TextureView(other, 2, tex_b);
        {
            const auto checks = ViewChecks();
            void* list = r.NewList();
            begin(list, own, 2);
            r.DrawIn(list, kLeft);
            void* heaps[] = {other.proxy};
            Call(list, 32, uintptr_t{1}, P(heaps), uintptr_t{0});  // SetDescriptorHeaps
            r.Table(list, other, 2);
            r.DrawIn(list, kRight);
            if (!r.Wait(r.End(list))) return 5;
            frame_check({{kLeft, 0, {0, 0, 0, 0}}, {kRight, 1, {0, 0, 0, 0}}}, "14c the heap switched between two draws, same index");
            r.Check(validated_since(checks) == 2, "14c a heap switch is checked again");
        }
        // 14d. The list reset and recorded again with the same state: checked again.
        {
            void* allocator = nullptr;
            void* list = r.NewListKeepingAllocator(&allocator);
            begin(list, own, 2);
            r.DrawIn(list, kWhole);
            if (!r.Wait(r.End(list))) return 5;
            frame_check({{kWhole, 0, {0, 0, 0, 0}}}, "14d before the reset: A");
            r.TextureView(own, 2, tex_b);
            const auto checks = ViewChecks();
            SX_REQUIRE(Call(allocator, 8) == 0);                                   // ID3D12CommandAllocator::Reset
            SX_REQUIRE(Call(list, 10, P(allocator), uintptr_t{0}) == 0);           // ID3D12GraphicsCommandList::Reset
            begin(list, own, 2);
            r.DrawIn(list, kWhole);
            if (!r.Wait(r.End(list))) return 5;
            frame_check({{kWhole, 1, {0, 0, 0, 0}}}, "14d the list reset, the slot now B's view");
            r.Check(validated_since(checks) == 1, "14d a reset list checks its view again");
        }
        // 14e. A slot reused after its texture was retired.
        {
            void* retired = r.Texture(0);
            r.TextureView(own, 6, retired);
            void* list = r.NewList();
            begin(list, own, 6);
            r.DrawIn(list, kWhole);
            if (!r.Wait(r.End(list))) return 5;
            frame_check({{kWhole, 0, {0, 0, 0, 0}}}, "14e slot 6 views a texture of its own");
            SX_REQUIRE(Call(retired, 2) == 0);     // the title's last reference
            r.TextureView(own, 6, tex_b);          // the slot is given another texture's view
            list = r.NewList();
            begin(list, own, 6);
            r.DrawIn(list, kWhole);
            if (!r.Wait(r.End(list))) return 5;
            frame_check({{kWhole, 1, {0, 0, 0, 0}}}, "14e slot 6 reused for B after that texture was released");
        }
        // 14f. A buffer's unordered access view copied over the sampled slot after the binding
        //      was made: the next draw is dropped.
        for (int way = 0; way < 2; ++way) {
            static const char* const kWays[] = {"14f a UAV copied over the sampled slot by CopyDescriptorsSimple",
                                                "14f a UAV copied over the sampled slot by CopyDescriptors (second range)"};
            r.TextureView(own, 3, tex_a);
            r.TextureView(own, 4, tex_a);
            const auto stats_before = Stats();
            void* list = r.NewList();
            begin(list, own, 3);
            r.DrawIn(list, kNothing);
            if (way == 0) copy_simple(own, 3, 2);
            else copy_ranges(own, 11, 3, 0, 2);
            r.DrawIn(list, kWhole);
            if (!r.Wait(r.End(list))) return 5;
            const auto stats = Stats();
            frame_check({}, kWays[way]);
            r.Check(stats.draws_skipped - stats_before.draws_skipped == 1, "14f the draw after the copy was dropped");
        }
    }

    // 10. Objects released while the draw that uses them is in flight. Each case has objects of
    //     its own, so a failure leaves the others standing.
    // For a diagnosis, SCARLETTX_TEST_INFLIGHT picks the cases by letter: "a" to "d" release in
    // flight as usual; a capital holds the object until the fence has completed and releases it
    // then (the control). SCARLETTX_TEST_INFLIGHT_GAP says what happens between submitting the
    // list under test and the release: nothing (unset), "sleep:<ms>", "query" (one read of the
    // fence), "print" (one line to stdout), "yield", "spin:<n>" (n empty loop turns, no call at
    // all) or "tight" (nothing, and no clock read either: the release follows the submission as
    // closely as in the plain test). Otherwise the steps are timed and reported after the wait. A submission that does not complete in time
    // is watched for another 20 seconds. With neither variable set the cases run as always.
    const char* const pick = std::getenv("SCARLETTX_TEST_INFLIGHT");
    const char* const gap_env = std::getenv("SCARLETTX_TEST_INFLIGHT_GAP");
    const std::string gap = gap_env ? gap_env : "";
    LARGE_INTEGER tick_rate{};
    QueryPerformanceFrequency(&tick_rate);
    const auto now_us = [&tick_rate]() {
        LARGE_INTEGER t{};
        QueryPerformanceCounter(&t);
        return static_cast<long long>(t.QuadPart * 1000000 / tick_rate.QuadPart);
    };
    for (int which = 0; which < 4 && !r.stuck; ++which) {
        static const char* const kNames[] = {"10a the pipeline released in flight", "10b the descriptor heap released in flight",
                                             "10c the title's root signature released in flight", "10d the texture released in flight"};
        const bool hold = pick && std::strchr(pick, 'A' + which) != nullptr;
        if (pick && !hold && std::strchr(pick, 'a' + which) == nullptr) continue;
        const bool timed = (pick || gap_env) && gap != "tight" && gap.rfind("spin:", 0) != 0;
        long long t_busy = 0;
        uint64_t busy_value = 0;
        void* own_root = r.TitleRoot();
        void* own_pso = nullptr;
        hr = r.CreatePipeline(own_root, vs, ps, &own_pso);
        if (FAILED(hr) || !own_pso) return r.Fail("shim CreateGraphicsPipelineState (slot 10), lifetime case", hr);
        Rig::Heap own_heap = r.ViewHeap(4);
        void* own_tex = r.Texture(which & 1);
        r.TextureView(own_heap, 1, own_tex);
        // Slow draws first, not waited for: the list under test is queued behind them.
        {
            void* busy = r.NewList();
            r.Begin(busy, nullptr, root);
            r.Pipeline(busy, busy_pso);
            for (int i = 0; i < 24; ++i) r.DrawIn(busy, kWhole);
            busy_value = r.Submit(busy);
            if (timed) t_busy = now_us();
        }
        void* list = r.NewList();
        r.Begin(list, &own_heap, own_root);
        r.Pipeline(list, own_pso);
        r.Constants(list, zero);
        r.Table(list, own_heap, 1);
        r.DrawIn(list, kWhole);
        const uint64_t value = r.End(list);
        const long long t_list = timed ? now_us() : 0;
        void* released = which == 0 ? own_pso : which == 1 ? own_heap.proxy : which == 2 ? own_root : own_tex;
        if (hold) {
            // The control: nothing is released while the list is in flight.
            const bool done = r.Wait(value);
            const long long t_done = now_us();
            const intptr_t left_after = done ? Call(released, 2) : -1;
            std::printf("%s probe %s: CONTROL, held. busy list fence %llu at 0 us; list under test fence %llu at +%lld us; wait ended at "
                        "+%lld us, %s; then Release returned %lld\n", kTag, kNames[which], static_cast<unsigned long long>(busy_value),
                        static_cast<unsigned long long>(value), t_list - t_busy, t_done - t_busy, done ? "completed" : "DID NOT COMPLETE",
                        static_cast<long long>(left_after));
            if (!done) break;
            frame_check({{kWhole, which & 1, {0, 0, 0, 0}}}, "10 control: held until the fence completed");
            continue;
        }
        // Only what the diagnosis asks for happens between the submission and the release.
        uint64_t seen_in_gap = 0;
        if (gap.rfind("sleep:", 0) == 0) Sleep(static_cast<DWORD>(std::atoi(gap.c_str() + 6)));
        else if (gap == "query") seen_in_gap = static_cast<uint64_t>(Call(r.fence, 8));
        else if (gap == "print") { std::printf("%s probe gap line\n", kTag); std::fflush(stdout); }
        else if (gap == "yield") SwitchToThread();
        else if (gap.rfind("spin:", 0) == 0) {
            volatile int sink = 0;
            for (int k = 0, n = std::atoi(gap.c_str() + 5); k < n; ++k) sink = sink + 1;
        }
        const long long t_before_release = timed ? now_us() : 0;
        // The title's last reference, while the list has not run yet.
        const intptr_t left = Call(released, 2);
        const long long t_released = now_us();
        const uint64_t completed_after_release = static_cast<uint64_t>(Call(r.fence, 8));
        const bool in_flight = completed_after_release < value;
        std::printf("%s %s: Release returned %lld, list still in flight afterwards: %s\n", kTag, kNames[which],
                    static_cast<long long>(left), in_flight ? "yes" : "no");
        r.Check(in_flight, "10 the release came while the list was in flight");
        const bool completed_in_time = r.Wait(value);
        const long long t_wait_end = now_us();
        if (pick || gap_env) {
            std::printf("%s probe %s: gap=%s. busy list signalled fence %llu, list under test fence %llu; fence right after the "
                        "release: %llu (read in the gap: %llu); wait for %llu %s, fence then %llu\n", kTag, kNames[which],
                        gap.empty() ? "none" : gap.c_str(), static_cast<unsigned long long>(busy_value),
                        static_cast<unsigned long long>(value), static_cast<unsigned long long>(completed_after_release),
                        static_cast<unsigned long long>(seen_in_gap), static_cast<unsigned long long>(value),
                        completed_in_time ? "completed" : "TIMED OUT (15000 ms)", static_cast<unsigned long long>(Call(r.fence, 8)));
            if (timed) {
                std::printf("%s probe %s: times from the busy list's submission: list under test submitted at +%lld us; Release began "
                            "at +%lld us and took %lld us; the wait ended at +%lld us\n", kTag, kNames[which], t_list - t_busy,
                            t_before_release - t_busy, t_released - t_before_release, t_wait_end - t_busy);
            }
            for (int second = 1; !completed_in_time && second <= 20; ++second) {
                Sleep(1000);
                const uint64_t now = static_cast<uint64_t>(Call(r.fence, 8));
                if (now >= value || second % 10 == 0) {
                    std::printf("%s probe %s: %d s after the timeout the fence is at %llu of %llu\n", kTag, kNames[which], second,
                                static_cast<unsigned long long>(now), static_cast<unsigned long long>(value));
                }
                if (now >= value) break;
            }
        }
        if (!completed_in_time) {
            r.Check(false, kNames[which]);
            std::printf("%s %s: the submission did not complete; the remaining cases are not run\n", kTag, kNames[which]);
            break;
        }
        frame_check({{kWhole, which & 1, {0, 0, 0, 0}}}, kNames[which]);
    }

    std::printf("%s pixels exact %u/%u over %u frames\n", kTag, pixels_exact, pixels_total, pixels_total / all);
    std::printf("%s %s (%d failed)\n", kTag, r.failures == 0 && !r.stuck ? "PASS" : "FAIL", r.failures);
    if (r.stuck) {
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 5);
    }
    return r.failures == 0 ? 0 : 1;
}
