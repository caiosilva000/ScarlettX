// A translated draw on the guest-facing route, synthetic only.
//
// The vertex and pixel programs of test_victory_draw_e2e, each as a console-shaped shader
// container (DXBC with ISG1 and OSG1 signature parts and a DXIL part whose XDAT index holds the
// native code), are handed to the d3d12_xs shim the way a title's pipeline arrives:
// CreateGraphicsPipelineState on the shim's device object, with the title's own root signature
// (parameter 0: four root constants). Root signature, pipeline, root constants, topology,
// viewport, scissor, render target and the draw go through the shim's command-list object. The
// host device is used directly only for plumbing: the render target, its clear and its readback.
//
// The viewport is the left half of a 64x64 target: the left half must be the drawn colour and
// the right half the clear colour, for four colours: 16,384 pixels in all.
//
// Controls: an ordinary desktop pipeline (HLSL) must pass through untranslated and draw exactly;
// a translated pixel shader whose bindings go beyond root constants, a malformed native stream
// and a mix of native and desktop stages must each be refused with a diagnostic; a draw without
// the title's root constants must be dropped and leave the target untouched.
//
// Exit codes: 0 pass, 1 a check failed, 2 a D3D12 call failed (name and HRESULT printed),
// 3 no device, 4 a fixture could not be built.
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

constexpr UINT kSize = 64;
constexpr uint8_t kClear[4] = {16, 32, 48, 255};
constexpr UINT kRowPitch = 256;  // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, enough for 64 RGBA8 texels

template <typename... Args>
intptr_t Call(void* object, int slot, Args... args) {
    using Fn = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, Args...);
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[slot])(object, args...);
}
uintptr_t P(const void* p) { return reinterpret_cast<uintptr_t>(p); }

int Fail(const char* api, HRESULT hr) {
    std::printf("[victory-draw-guest] FAILED api=%s hr=0x%08x\n", api, static_cast<uint32_t>(hr));
    return 2;
}

D3D12XBOX_GRAPHICS_TRANSLATION_STATS Stats() {
    D3D12XBOX_GRAPHICS_TRANSLATION_STATS s{};
    SX_REQUIRE(D3D12XboxGetGraphicsTranslationStats(&s) == 0);
    return s;
}

uint32_t FloatBits(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    return bits;
}

// The desktop control: the same picture from HLSL, its colour in the same four root constants.
const char* kDesktopHlsl = R"(
cbuffer Draw : register(b0) { float4 colour; };
float4 VSMain(uint vid : SV_VertexID) : SV_POSITION {
    return float4(float((vid << 1) & 2) * 2.0f - 1.0f, float(vid & 2) * 2.0f - 1.0f, 0.0f, 1.0f);
}
float4 PSMain() : SV_TARGET { return colour; }
)";

struct Rig {
    void* shim_device = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    UINT64 fence_value = 0;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12Resource* target = nullptr;
    ID3D12Resource* readback = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    void* title_root = nullptr;  // parameter 0: four root constants

    HRESULT CreateRoot(const D3D12_ROOT_SIGNATURE_DESC& desc, void** out_root) {
        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        const HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (FAILED(hr)) return hr;
        *out_root = nullptr;
        return static_cast<HRESULT>(Call(shim_device, 16, uintptr_t{0}, P(blob->GetBufferPointer()),
                                         static_cast<uintptr_t>(blob->GetBufferSize()), uintptr_t{0}, P(out_root)));
    }

    // CreateGraphicsPipelineState on the shim's device object (vtable slot 10).
    HRESULT CreatePipeline(void* shim_root, const void* vs, size_t vs_size, const void* ps, size_t ps_size,
                           void** out_pso) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = static_cast<ID3D12RootSignature*>(shim_root);
        desc.VS = {vs, vs_size};
        desc.PS = {ps, ps_size};
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

    int Init(bool prefer_warp) {
        _putenv_s("SCARLETTX_REAL_TITLE", "1");
        SetEnvironmentVariableA("SCARLETTX_REAL_TITLE", "1");
        D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
        params.Version = 1;
        params.PreferWarp = prefer_warp ? 1 : 0;
        const int32_t created = D3D12XboxCreateDevice(nullptr, &params, &__uuidof(ID3D12Device), &shim_device);
        if (created < 0 || !shim_device) {
            std::printf("[victory-draw-guest] D3D12XboxCreateDevice FAILED hr=0x%08x\n", static_cast<uint32_t>(created));
            return 3;
        }
        SX_REQUIRE(D3D12XboxGetNativeObject(shim_device, reinterpret_cast<void**>(&device)) == 0 && device);
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HRESULT hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
        if (FAILED(hr)) return Fail("CreateCommandQueue", hr);
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(hr)) return Fail("CreateFence", hr);
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.NumDescriptors = 1;
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap));
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
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                             &clear, IID_PPV_ARGS(&target));
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

        D3D12_ROOT_PARAMETER constants{};
        constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        constants.Constants.Num32BitValues = 4;
        constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC title_rs{};
        title_rs.NumParameters = 1;
        title_rs.pParameters = &constants;
        title_rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        hr = CreateRoot(title_rs, &title_root);
        if (FAILED(hr) || !title_root) return Fail("shim CreateRootSignature (slot 16)", hr);
        return 0;
    }

    // One draw as a title makes it, on the shim's command list. `colour` null leaves the root
    // constants unset. Returns 0 and the 64x64 RGBA8 frame, or an exit code.
    int Draw(void* shim_pso, const uint8_t* colour, std::vector<uint8_t>& out) {
        void* shim_alloc = nullptr;
        HRESULT hr = static_cast<HRESULT>(
            Call(shim_device, 9, static_cast<uintptr_t>(D3D12_COMMAND_LIST_TYPE_DIRECT), uintptr_t{0}, P(&shim_alloc)));
        if (FAILED(hr) || !shim_alloc) return Fail("shim CreateCommandAllocator (slot 9)", hr);
        void* shim_list = nullptr;
        hr = static_cast<HRESULT>(Call(shim_device, 12, uintptr_t{0},
                                       static_cast<uintptr_t>(D3D12_COMMAND_LIST_TYPE_DIRECT), P(shim_alloc),
                                       uintptr_t{0}, uintptr_t{0}, P(&shim_list)));
        if (FAILED(hr) || !shim_list) return Fail("shim CreateCommandList (slot 12)", hr);
        ID3D12GraphicsCommandList* list = nullptr;
        SX_REQUIRE(D3D12XboxGetNativeObject(shim_list, reinterpret_cast<void**>(&list)) == 0 && list);

        // Plumbing on the host list: the clear.
        float clear[4];
        for (int i = 0; i < 4; ++i) clear[i] = static_cast<float>(kClear[i]) / 255.0f;
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);

        // The title's draw, on the shim's list.
        const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kSize / 2), static_cast<float>(kSize), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(kSize), static_cast<LONG>(kSize)};
        Call(shim_list, 50, uintptr_t{1}, P(&rtv), uintptr_t{0}, uintptr_t{0});          // OMSetRenderTargets
        Call(shim_list, 34, P(title_root_for_draw), uintptr_t{0}, uintptr_t{0});         // SetGraphicsRootSignature
        Call(shim_list, 25, P(shim_pso), uintptr_t{0}, uintptr_t{0});                    // SetPipelineState
        if (colour) {
            uint32_t constants[4];
            for (int i = 0; i < 4; ++i) constants[i] = FloatBits(static_cast<float>(colour[i]) / 255.0f);
            Call(shim_list, 40, uintptr_t{0}, uintptr_t{4}, P(constants), uintptr_t{0});  // SetGraphicsRoot32BitConstants
        }
        Call(shim_list, 20, static_cast<uintptr_t>(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST), uintptr_t{0}, uintptr_t{0});
        Call(shim_list, 21, uintptr_t{1}, P(&vp), uintptr_t{0});                          // RSSetViewports
        Call(shim_list, 22, uintptr_t{1}, P(&scissor), uintptr_t{0});                     // RSSetScissorRects
        Call(shim_list, 12, uintptr_t{3}, uintptr_t{1}, uintptr_t{0}, uintptr_t{0});      // DrawInstanced

        // Plumbing: copy the target out.
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
        hr = static_cast<HRESULT>(Call(shim_list, 9, uintptr_t{0}, uintptr_t{0}, uintptr_t{0}));  // Close
        if (FAILED(hr)) return Fail("shim ID3D12GraphicsCommandList::Close (slot 9)", hr);

        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        const UINT64 want = ++fence_value;
        hr = queue->Signal(fence, want);
        if (FAILED(hr)) return Fail("ID3D12CommandQueue::Signal", hr);
        DWORD wait = 0;
        if (fence->GetCompletedValue() < want) {
            hr = fence->SetEventOnCompletion(want, event);
            if (FAILED(hr)) return Fail("ID3D12Fence::SetEventOnCompletion", hr);
            wait = WaitForSingleObject(event, 10000);
        }
        const UINT64 completed = fence->GetCompletedValue();
        std::printf("[victory-draw-guest] fence signaled=%llu completed=%llu wait_result=%lu\n",
                    static_cast<unsigned long long>(want), static_cast<unsigned long long>(completed),
                    static_cast<unsigned long>(wait));
        if (completed < want) return Fail("ID3D12Fence::GetCompletedValue (fence not reached)", device->GetDeviceRemovedReason());

        void* mapped = nullptr;
        hr = readback->Map(0, nullptr, &mapped);
        if (FAILED(hr) || !mapped) return Fail("ID3D12Resource::Map(Readback)", FAILED(hr) ? hr : E_POINTER);
        out.resize(static_cast<size_t>(kSize) * kSize * 4);
        for (UINT y = 0; y < kSize; ++y) {
            std::memcpy(out.data() + static_cast<size_t>(y) * kSize * 4,
                        static_cast<const uint8_t*>(mapped) + static_cast<size_t>(y) * kRowPitch, kSize * 4);
        }
        readback->Unmap(0, nullptr);
        list->Release();
        return 0;
    }

    void* title_root_for_draw = nullptr;
};

// Pixels matching: the drawn colour in the left half (or the clear colour everywhere when
// `colour` is null), the clear colour in the right half.
void Count(const std::vector<uint8_t>& frame, const uint8_t* colour, uint32_t* drawn, uint32_t* untouched) {
    *drawn = 0;
    *untouched = 0;
    for (UINT y = 0; y < kSize; ++y) {
        for (UINT x = 0; x < kSize; ++x) {
            const uint8_t* px = &frame[(static_cast<size_t>(y) * kSize + x) * 4];
            const bool left = x < kSize / 2;
            const uint8_t* want = left && colour ? colour : kClear;
            if (std::memcmp(px, want, 4) == 0) (left ? *drawn : *untouched)++;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool prefer_warp = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--warp") prefer_warp = true;
        else {
            std::fprintf(stderr, "usage: test_victory_draw_guest [--warp]\n");
            return 64;
        }
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("[victory-draw-guest] %s: %s\n", ok ? "ok" : "CHECK FAILED", what);
        if (!ok) ++failures;
    };

    std::vector<uint8_t> vs;
    std::vector<uint8_t> ps;
    std::vector<uint8_t> vs_malformed;
    std::vector<uint8_t> ps_with_table;
    if (!scarlettx::BuildVictorySyntheticContainer(9, vs).ok() || !scarlettx::BuildVictorySyntheticContainer(10, ps).ok() ||
        !scarlettx::BuildVictorySyntheticContainer(11, vs_malformed).ok() ||
        !scarlettx::BuildVictorySyntheticContainer(12, ps_with_table).ok()) {
        std::printf("[victory-draw-guest] the synthetic containers could not be built\n");
        return 4;
    }
    std::printf("[victory-draw-guest] containers: vs=%zu bytes ps=%zu bytes\n", vs.size(), ps.size());

    // The binding layouts the graphics route can and cannot make (no device needed).
    {
        scarlettx::VictoryComputeBinding b;
        std::string why;
        b.user_data_dwords = 4;
        b.user_data = {{0, 0, 0, 0}, {1, 0, 0, 1}};
        check(scarlettx::VictoryGraphicsBindingSupported(b, why), "root constants are a supported layout");
        scarlettx::VictoryComputeBinding none;
        check(scarlettx::VictoryGraphicsBindingSupported(none, why), "a stage without user data is a supported layout");
        auto unsupported = [&](scarlettx::VictoryComputeBinding v, const char* what) {
            std::string reason;
            const bool refused = !scarlettx::VictoryGraphicsBindingSupported(v, reason);
            std::printf("[victory-draw-guest]   %s -> %s\n", what, refused ? reason.c_str() : "(accepted)");
            check(refused && !reason.empty(), what);
        };
        auto v = b; v.user_data.push_back({2, 1, 1, 0}); unsupported(v, "unsupported layout: a descriptor table");
        v = b; v.static_sampler_count = 1; unsupported(v, "unsupported layout: a static sampler");
        v = b; v.root_cbv_count = 1; unsupported(v, "unsupported layout: a root constant buffer");
        v = b; v.flags = 8; unsupported(v, "unsupported layout: computed heap indices");
        v = b; v.user_data_dwords = 36; unsupported(v, "unsupported layout: user data too large for a stage");
    }

    Rig rig;
    if (const int rc = rig.Init(prefer_warp)) return rc;
    rig.title_root_for_draw = rig.title_root;

    // 1. The native containers, through the guest-facing pipeline creation.
    auto before = Stats();
    void* pso = nullptr;
    HRESULT hr = rig.CreatePipeline(rig.title_root, vs.data(), vs.size(), ps.data(), ps.size(), &pso);
    auto after = Stats();
    std::printf("[victory-draw-guest] shim CreateGraphicsPipelineState(native containers) hr=0x%08x translated=+%llu "
                "passed_through=+%llu rejected=+%llu\n",
                static_cast<uint32_t>(hr), static_cast<unsigned long long>(after.graphics_translated - before.graphics_translated),
                static_cast<unsigned long long>(after.graphics_passed_through - before.graphics_passed_through),
                static_cast<unsigned long long>(after.graphics_rejected - before.graphics_rejected));
    if (FAILED(hr) || !pso) return Fail("shim CreateGraphicsPipelineState (slot 10), native containers", hr);
    check(after.graphics_translated - before.graphics_translated == 1 &&
              after.graphics_passed_through == before.graphics_passed_through,
          "the native vertex and pixel containers were recognized and translated at pipeline creation");

    // 2. Four draws through the shim's command list.
    const uint8_t colours[][4] = {{32, 224, 96, 255}, {255, 0, 128, 64}, {1, 2, 3, 4}, {200, 100, 50, 25}};
    const uint32_t half = kSize * kSize / 2;
    before = Stats();
    uint32_t exact = 0;
    for (const auto& colour : colours) {
        std::vector<uint8_t> frame;
        if (const int rc = rig.Draw(pso, colour, frame)) return rc;
        uint32_t drawn = 0;
        uint32_t untouched = 0;
        Count(frame, colour, &drawn, &untouched);
        std::printf("[victory-draw-guest] colour %u,%u,%u,%u: drawn half exact %u/%u, untouched half exact %u/%u\n",
                    colour[0], colour[1], colour[2], colour[3], drawn, half, untouched, half);
        exact += drawn + untouched;
    }
    after = Stats();
    std::printf("[victory-draw-guest] pixels exact %u/%u, translated draws=+%llu\n", exact, 4 * kSize * kSize,
                static_cast<unsigned long long>(after.draws_translated - before.draws_translated));
    check(exact == 4 * kSize * kSize, "every pixel of the four translated draws is exact");
    check(after.draws_translated - before.draws_translated == 4 && after.draws_skipped == before.draws_skipped,
          "all four draws ran the translated pipeline");

    // 3. A draw without the title's root constants: dropped, the target keeps its clear colour.
    {
        before = Stats();
        std::vector<uint8_t> frame;
        if (const int rc = rig.Draw(pso, nullptr, frame)) return rc;
        after = Stats();
        uint32_t left = 0;
        uint32_t right = 0;
        Count(frame, nullptr, &left, &right);
        check(after.draws_skipped - before.draws_skipped == 1 && left == half && right == half,
              "control: a draw without the root constants is dropped and draws nothing");
    }

    // 4. Refusals at pipeline creation, each with a diagnostic.
    auto rejected = [&](const std::vector<uint8_t>& v, const void* p, size_t p_size, const char* what) {
        const auto b = Stats();
        void* refused_pso = nullptr;
        const HRESULT r = rig.CreatePipeline(rig.title_root, v.data(), v.size(), p, p_size, &refused_pso);
        const auto a = Stats();
        std::printf("[victory-draw-guest] %s: hr=0x%08x pso=%s rejected=+%llu translated=+%llu\n", what,
                    static_cast<uint32_t>(r), refused_pso ? "created" : "none",
                    static_cast<unsigned long long>(a.graphics_rejected - b.graphics_rejected),
                    static_cast<unsigned long long>(a.graphics_translated - b.graphics_translated));
        check(FAILED(r) && !refused_pso && a.graphics_rejected - b.graphics_rejected == 1 &&
                  a.graphics_translated == b.graphics_translated && a.graphics_passed_through == b.graphics_passed_through,
              what);
    };
    rejected(vs, ps_with_table.data(), ps_with_table.size(),
             "control: a translated pixel shader with a descriptor-table binding is refused");
    rejected(vs_malformed, ps.data(), ps.size(), "control: a malformed native vertex stream is refused");

    // 5. Desktop shaders: pass through untranslated and draw exactly; and a mix is refused.
    {
        ID3DBlob* dvs = nullptr;
        ID3DBlob* dps = nullptr;
        ID3DBlob* err = nullptr;
        hr = D3DCompile(kDesktopHlsl, std::strlen(kDesktopHlsl), "desktop.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0,
                        &dvs, &err);
        if (FAILED(hr)) return Fail("D3DCompile(VSMain)", hr);
        hr = D3DCompile(kDesktopHlsl, std::strlen(kDesktopHlsl), "desktop.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0,
                        &dps, &err);
        if (FAILED(hr)) return Fail("D3DCompile(PSMain)", hr);
        rejected(vs, dps->GetBufferPointer(), dps->GetBufferSize(),
                 "control: a native vertex shader with a desktop pixel shader is refused");

        before = Stats();
        void* desktop_pso = nullptr;
        hr = rig.CreatePipeline(rig.title_root, dvs->GetBufferPointer(), dvs->GetBufferSize(), dps->GetBufferPointer(),
                                dps->GetBufferSize(), &desktop_pso);
        after = Stats();
        std::printf("[victory-draw-guest] shim CreateGraphicsPipelineState(desktop shaders) hr=0x%08x passed_through=+%llu "
                    "translated=+%llu\n",
                    static_cast<uint32_t>(hr),
                    static_cast<unsigned long long>(after.graphics_passed_through - before.graphics_passed_through),
                    static_cast<unsigned long long>(after.graphics_translated - before.graphics_translated));
        if (FAILED(hr) || !desktop_pso) return Fail("shim CreateGraphicsPipelineState (slot 10), desktop shaders", hr);
        check(after.graphics_passed_through - before.graphics_passed_through == 1 &&
                  after.graphics_translated == before.graphics_translated,
              "control: desktop shaders pass through untranslated");
        before = Stats();
        std::vector<uint8_t> frame;
        if (const int rc = rig.Draw(desktop_pso, colours[3], frame)) return rc;
        after = Stats();
        uint32_t drawn = 0;
        uint32_t untouched = 0;
        Count(frame, colours[3], &drawn, &untouched);
        std::printf("[victory-draw-guest] desktop control: drawn half exact %u/%u, untouched half exact %u/%u\n", drawn, half,
                    untouched, half);
        check(drawn == half && untouched == half && after.draws_translated == before.draws_translated &&
                  after.draws_skipped == before.draws_skipped,
              "control: the desktop pipeline draws exactly, as the host's own draw");
    }

    std::printf("[victory-draw-guest] %s (%d failed checks)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
