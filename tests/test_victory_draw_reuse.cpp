// A translated graphics pipeline's bindings across draws, on the guest-facing route.
//
// The shim keeps a translated pipeline's host root signature and user data on the host list from
// one translated draw to the next, and puts the title's own root signature back only when
// something needs it. Sixteen scissored draws in two command-list recordings go through every
// case, each into its own 4-pixel column of a 64x16 target, and every pixel is read back:
//
//   translated draws with the colour unchanged, changed, and set to the same value again;
//   a desktop pipeline between translated draws, with the colour set before and after the switch;
//   the title setting its root signature again between translated draws;
//   a second recording after Reset, which starts with nothing bound.
//
// The vertex and pixel programs are the synthetic native fixtures of test_victory_draw_guest
// (a full-target triangle; the colour in four root constants) and the same two programs in HLSL.
//
// Exit codes: 0 pass, 1 a column showed the wrong colour or a count was off, 2 a call failed,
// 3 no device, 4 a fixture could not be built.
#include "sx_check.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

constexpr uint32_t kW = 64, kH = 16, kColumn = 4, kPitch = 256;

const char* kHlsl = R"(
cbuffer Draw : register(b0) { float4 colour; };
float4 VSMain(uint vid : SV_VertexID) : SV_POSITION {
    return float4(float((vid << 1) & 2) * 2.0f - 1.0f, float(vid & 2) * 2.0f - 1.0f, 0.0f, 1.0f);
}
float4 PSMain() : SV_TARGET { return colour; }
)";

struct Step {
    bool translated;   // which pipeline draws
    int colour;        // >= 0: set this colour before the draw; -1: leave the root constants alone
    bool set_root;     // the title sets its root signature again before the draw
    bool new_list;     // close, submit and reset the command list before this step
    uint32_t want;     // index of the colour the column must show
};
uint32_t Colour(int i) { return 0xFF000000u | ((static_cast<uint32_t>(i) * 0x00371B0Du + 0x00102030u) & 0x00FFFFFFu); }

}  // namespace

int main() {
    _putenv_s("SCARLETTX_REAL_TITLE", "1");
    SetEnvironmentVariableA("SCARLETTX_REAL_TITLE", "1");
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    params.PreferWarp = 1;
    void* device = nullptr;
    if (D3D12XboxCreateDevice(nullptr, &params, &__uuidof(ID3D12Device), &device) < 0 || !device) {
        std::printf("[draw-reuse] D3D12XboxCreateDevice FAILED\n");
        return 3;
    }
    void *queue = nullptr, *allocator = nullptr, *list = nullptr, *fence = nullptr;
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    SX_REQUIRE(Call(device, 8, P(&qdesc), uintptr_t{0}, P(&queue)) == 0 && queue);
    SX_REQUIRE(Call(device, 9, static_cast<uintptr_t>(D3D12_COMMAND_LIST_TYPE_DIRECT), uintptr_t{0}, P(&allocator)) == 0);
    SX_REQUIRE(Call(device, 12, uintptr_t{0}, static_cast<uintptr_t>(D3D12_COMMAND_LIST_TYPE_DIRECT), P(allocator),
                    uintptr_t{0}, uintptr_t{0}, P(&list)) == 0 && list);
    SX_REQUIRE(Call(device, 36, uintptr_t{0}, uintptr_t{0}, uintptr_t{0}, P(&fence)) == 0 && fence);
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint64_t fence_value = 0;

    auto resource = [&](D3D12_HEAP_TYPE heap, const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heap;
        void* res = nullptr;
        SX_REQUIRE(Call(device, 27, P(&hp), uintptr_t{0}, P(&desc), static_cast<uintptr_t>(state), uintptr_t{0},
                        uintptr_t{0}, P(&res)) == 0 && res);
        return res;
    };
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kW;
    td.Height = kH;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    void* target = resource(D3D12_HEAP_TYPE_DEFAULT, td, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = static_cast<uint64_t>(kPitch) * kH;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* readback = resource(D3D12_HEAP_TYPE_READBACK, bd, D3D12_RESOURCE_STATE_COPY_DEST);
    void* rtv_heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    SX_REQUIRE(Call(device, 14, P(&hd), uintptr_t{0}, P(&rtv_heap)) == 0 && rtv_heap);
    const uintptr_t rtv = static_cast<uintptr_t>(Call(rtv_heap, 9));
    Call(device, 20, P(target), uintptr_t{0}, rtv);

    // The title's root signature: parameter 0, four root constants.
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.Num32BitValues = 4;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 1;
    rs.pParameters = &param;
    ID3DBlob *rs_blob = nullptr, *err = nullptr, *vs = nullptr, *ps = nullptr;
    SX_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &err)));
    void* root = nullptr;
    SX_REQUIRE(Call(device, 16, uintptr_t{0}, P(rs_blob->GetBufferPointer()), static_cast<uintptr_t>(rs_blob->GetBufferSize()),
                    uintptr_t{0}, P(&root)) == 0 && root);
    SX_REQUIRE(SUCCEEDED(D3DCompile(kHlsl, std::strlen(kHlsl), "reuse.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs, &err)));
    SX_REQUIRE(SUCCEEDED(D3DCompile(kHlsl, std::strlen(kHlsl), "reuse.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &ps, &err)));
    std::vector<uint8_t> nvs, nps;
    if (!scarlettx::BuildVictorySyntheticContainer(9, nvs).ok() || !scarlettx::BuildVictorySyntheticContainer(10, nps).ok()) {
        std::printf("[draw-reuse] the native fixtures could not be built\n");
        return 4;
    }
    auto pipeline = [&](const void* v, size_t vn, const void* p, size_t pn) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = static_cast<ID3D12RootSignature*>(root);
        d.VS = {v, vn};
        d.PS = {p, pn};
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = 0xFFFFFFFFu;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        void* pso = nullptr;
        SX_REQUIRE(Call(device, 10, P(&d), uintptr_t{0}, P(&pso)) == 0 && pso);
        return pso;
    };
    void* desktop = pipeline(vs->GetBufferPointer(), vs->GetBufferSize(), ps->GetBufferPointer(), ps->GetBufferSize());
    void* translated = pipeline(nvs.data(), nvs.size(), nps.data(), nps.size());
    D3D12XBOX_GRAPHICS_TRANSLATION_STATS gs{};
    SX_REQUIRE(D3D12XboxGetGraphicsTranslationStats(&gs) == 0 && gs.graphics_translated == 1);

    const Step steps[] = {
        // translated pipeline?, colour set (-1: kept), root set again, new recording, colour wanted
        {true, 0, false, false, 0},    //  0 first translated draw
        {true, -1, false, false, 0},   //  1 nothing changed: the bindings are reused
        {true, 1, false, false, 1},    //  2 the colour changed
        {true, 1, false, false, 1},    //  3 the same colour set again
        {false, -1, false, false, 1},  //  4 desktop pipeline: the title's root and colour must be back
        {false, 2, false, false, 2},   //  5 desktop, new colour
        {true, -1, false, false, 2},   //  6 translated again, with the colour the desktop draw used
        {true, 3, false, false, 3},    //  7 translated, new colour
        {false, 4, false, false, 4},   //  8 the colour was set while the translated root was bound
        {true, 5, true, false, 5},     //  9 the title set its root signature again (arguments stay)
        {true, -1, true, false, 5},    // 10 root signature set again, nothing else: still colour 5
        {false, -1, true, false, 5},   // 11 the same before a desktop draw
        {true, 6, true, true, 6},      // 12 a new recording: nothing is bound after Reset
        {true, -1, false, false, 6},   // 13 reused within the new recording
        {false, 7, false, false, 7},   // 14 desktop in the new recording
        {true, 7, false, false, 7},    // 15 translated with the same colour as the desktop draw
    };
    const uint32_t count = sizeof(steps) / sizeof(steps[0]);

    auto begin = [&](bool clear) {
        const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f};
        Call(list, 21, uintptr_t{1}, P(&vp), uintptr_t{0});
        const D3D12_CPU_DESCRIPTOR_HANDLE handle{rtv};
        Call(list, 50, uintptr_t{1}, P(&handle), uintptr_t{0}, uintptr_t{0});
        if (clear) {
            const float c[4] = {16.0f / 255.0f, 32.0f / 255.0f, 48.0f / 255.0f, 1.0f};
            Call(list, 52, rtv, P(c), uintptr_t{0}, uintptr_t{0});
        }
        Call(list, 20, static_cast<uintptr_t>(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST), uintptr_t{0}, uintptr_t{0});
    };
    auto submit = [&] {
        SX_REQUIRE(Call(list, 9) == 0);
        void* lists[] = {list};
        Call(queue, 10, uintptr_t{1}, P(lists), uintptr_t{0});
        const uint64_t value = ++fence_value;
        SX_REQUIRE(Call(queue, 14, P(fence), static_cast<uintptr_t>(value), uintptr_t{0}) == 0);
        if (static_cast<uint64_t>(Call(fence, 8)) < value) {
            SX_REQUIRE(Call(fence, 9, static_cast<uintptr_t>(value), P(event), uintptr_t{0}) == 0);
            SX_REQUIRE(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0);
        }
    };

    D3D12XBOX_TRANSLATED_DRAW_BIND_STATS before{}, after{};
    SX_REQUIRE(D3D12XboxGetTranslatedDrawBindStats(&before) == 0);
    begin(true);  // the list is open after creation
    Call(list, 34, P(root), uintptr_t{0}, uintptr_t{0});
    void* bound = nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        const Step& st = steps[i];
        if (st.new_list) {
            submit();
            SX_REQUIRE(Call(allocator, 8) == 0);
            SX_REQUIRE(Call(list, 10, P(allocator), uintptr_t{0}, uintptr_t{0}) == 0);
            begin(false);
            bound = nullptr;
        }
        if (st.set_root) Call(list, 34, P(root), uintptr_t{0}, uintptr_t{0});
        void* pso = st.translated ? translated : desktop;
        if (pso != bound) {
            Call(list, 25, P(pso), uintptr_t{0}, uintptr_t{0});
            bound = pso;
        }
        if (st.colour >= 0) {
            const uint32_t v = Colour(st.colour);
            const float c[4] = {static_cast<float>(v & 255u) / 255.0f, static_cast<float>((v >> 8) & 255u) / 255.0f,
                                static_cast<float>((v >> 16) & 255u) / 255.0f, 1.0f};
            Call(list, 40, uintptr_t{0}, uintptr_t{4}, P(c), uintptr_t{0});
        }
        const D3D12_RECT r{static_cast<LONG>(i * kColumn), 0, static_cast<LONG>((i + 1) * kColumn), static_cast<LONG>(kH)};
        Call(list, 22, uintptr_t{1}, P(&r), uintptr_t{0});
        Call(list, 12, uintptr_t{3}, uintptr_t{1}, uintptr_t{0}, uintptr_t{0});
    }
    // Read the target back in the same recording.
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = static_cast<ID3D12Resource*>(target);
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    Call(list, 26, uintptr_t{1}, P(&b), uintptr_t{0});
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = static_cast<ID3D12Resource*>(target);
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = static_cast<ID3D12Resource*>(readback);
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = kW;
    dst.PlacedFootprint.Footprint.Height = kH;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kPitch;
    Call(list, 16, P(&dst), uintptr_t{0}, uintptr_t{0}, uintptr_t{0}, P(&src), uintptr_t{0});
    submit();
    SX_REQUIRE(D3D12XboxGetTranslatedDrawBindStats(&after) == 0);

    uint8_t* data = nullptr;
    SX_REQUIRE(Call(readback, 8, uintptr_t{0}, uintptr_t{0}, P(&data)) == 0 && data);
    int failures = 0;
    uint32_t exact_total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t want = Colour(static_cast<int>(steps[i].want));
        uint32_t exact = 0, got_first = 0;
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = i * kColumn; x < (i + 1) * kColumn; ++x) {
                uint32_t got;
                std::memcpy(&got, data + static_cast<size_t>(y) * kPitch + x * 4, 4);
                if (got == want) ++exact;
                else if (!got_first) got_first = got;
            }
        }
        exact_total += exact;
        const bool ok = exact == kColumn * kH;
        std::printf("[draw-reuse] step %2u %-10s %s%s%s: %u/%u pixels 0x%08x -> %s", i, steps[i].translated ? "translated" : "desktop",
                    steps[i].colour >= 0 ? "colour set" : "colour kept", steps[i].set_root ? ", root set again" : "",
                    steps[i].new_list ? ", new recording" : "", exact, kColumn * kH, want, ok ? "ok" : "WRONG");
        if (!ok) std::printf(" (got 0x%08x)", got_first);
        std::printf("\n");
        if (!ok) ++failures;
    }
    D3D12XBOX_GRAPHICS_TRANSLATION_STATS gs2{};
    D3D12XboxGetGraphicsTranslationStats(&gs2);
    std::printf("[draw-reuse] pixels exact %u/%u; translated draws %llu (want 11), skipped %llu (want 0)\n", exact_total, kW * kH,
                static_cast<unsigned long long>(gs2.draws_translated), static_cast<unsigned long long>(gs2.draws_skipped));
    std::printf("[draw-reuse] host root binds %llu, user-data sets %llu, title root restores %llu, title arguments replayed %llu "
                "(informational)\n",
                static_cast<unsigned long long>(after.host_root_binds - before.host_root_binds),
                static_cast<unsigned long long>(after.user_data_sets - before.user_data_sets),
                static_cast<unsigned long long>(after.title_root_restores - before.title_root_restores),
                static_cast<unsigned long long>(after.title_arguments_replayed - before.title_arguments_replayed));
    if (gs2.draws_translated != 11 || gs2.draws_skipped != 0) ++failures;
    if (failures) {
        std::printf("test_victory_draw_reuse: FAIL\n");
        return 1;
    }
    std::printf("test_victory_draw_reuse: OK\n");
    return 0;
}
