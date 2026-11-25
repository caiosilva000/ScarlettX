// Real-title proxy path: guest-texture uploads that are still in flight.
//
// The shim uploads texture memory from staging it reuses, and does not wait for the copy. Three
// things have to hold while copies are in flight, and each is made to matter here by keeping the
// first queue busy with slow draws so that its uploads are queued behind them:
//
//   1. a draw submitted to a second queue straight after an upload on the first must sample the
//      new texels (the shim makes that submission wait for the upload);
//   2. six writes and submissions in a row, none waited for, must each be sampled as written:
//      there are four staging slots, so two of them are reused only after their copies ran;
//   3. a texture released straight after the submission that draws it must still be drawn
//      (the staging keeps the host texture until its copy has run).
//
// And one thing about many textures, whose memory the shim takes from shared reservations and
// asks about once per reservation rather than once per texture:
//
//   4. of forty textures, only the ones written are uploaded, the others keep what they had,
//      and memory given back by released textures and handed out again starts clean.
//
// A draw samples the texture texel for pixel into a band of eight rows of a 64x64 target.
//
// How busy "busy" is, is measured, not assumed: one slow draw into a small area is timed, and the
// area and the number of draws are chosen so that a busy block lasts about 100 ms on the adapter
// at hand. A software rasterizer then gets a small area instead of minutes of work, and each part
// still reports whether its submission was in flight when it mattered (required).
//
// Every submission, signal and wait prints a flushed stage line with both queues' signaled and
// completed fence values, so a run that stops says where, and whether a fence was still moving.
// A wait that does not complete in 120 s ends the test with exit code 5 and those values.
//
// Exit codes: 0 pass, 1 a band showed the wrong texels or a part was not in flight, 2 a call
// failed, 3 no device, 5 a wait did not complete.
#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "scarlettx/gpu/xg_layout.h"
#include "sx_check.h"

#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
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
constexpr uint32_t kPitch = 256;
constexpr uint32_t kBand = 8;
constexpr uint32_t kBusySize = 2048;

const char* kHlsl = R"(
Texture2D<float4> tex : register(t0);
float4 VSMain(uint vid : SV_VertexID) : SV_POSITION {
    return float4(float((vid << 1) & 2) * 2.0f - 1.0f, float(vid & 2) * 2.0f - 1.0f, 0.0f, 1.0f);
}
float4 PSMain(float4 pos : SV_POSITION) : SV_TARGET {
    return tex.Load(int3(int(pos.x), int(pos.y), 0));
}
// Slow on purpose: keeps a queue busy for tens of milliseconds a draw.
float4 PSBusy(float4 pos : SV_POSITION) : SV_TARGET {
    uint v = uint(pos.x) * 0x9E3779B1u ^ uint(pos.y) * 0x85EBCA77u;
    for (uint i = 0u; i < 4096u; ++i) {
        v ^= v << 13;
        v ^= v >> 17;
        v ^= v << 5;
    }
    return float4(float(v & 255u) / 255.0f, 0.0f, 0.0f, 1.0f);
}
)";

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

struct Rig {
    void* device = nullptr;
    void* queue[2] = {};
    void* fence[2] = {};
    uint64_t fence_value[2] = {};
    HANDLE event = nullptr;
    void* srv_heap = nullptr;
    void* root = nullptr;
    void* pso = nullptr;
    void* pso_busy = nullptr;
    void* target = nullptr;
    void* busy_target = nullptr;
    void* readback = nullptr;
    uintptr_t rtv = 0, busy_rtv = 0, srv_cpu = 0, srv_gpu = 0;
    uint32_t srv_inc = 0;
    XG_RESOURCE_LAYOUT* layout = nullptr;
    int failures = 0;
    uint32_t busy_size = 256;    // side of the area a busy draw shades (calibrated)
    uint32_t busy_draws = 6;     // draws in a busy block (calibrated)
    ULONGLONG start_ms = GetTickCount64();

    uint64_t Completed(int q) { return static_cast<uint64_t>(Call(fence[q], kFenceGetCompletedValue)); }
    // A flushed line saying where the test is and what both fences say.
    void Mark(const char* stage) {
        std::printf("[in-flight] t_ms=%llu stage=%s q0 signaled=%llu completed=%llu q1 signaled=%llu completed=%llu\n",
                    static_cast<unsigned long long>(GetTickCount64() - start_ms), stage,
                    static_cast<unsigned long long>(fence_value[0]), static_cast<unsigned long long>(Completed(0)),
                    static_cast<unsigned long long>(fence_value[1]), static_cast<unsigned long long>(Completed(1)));
        std::fflush(stdout);
    }
    // Whether queue `q` had not yet reached `value` when asked: the work behind it was in flight.
    void RequireInFlight(const char* part, int q, uint64_t value) {
        const uint64_t completed = Completed(q);
        const bool yes = completed < value;
        std::printf("[in-flight] %s: in flight when it mattered: %s (queue %d completed %llu, needed below %llu)\n", part,
                    yes ? "yes" : "NO", q, static_cast<unsigned long long>(completed), static_cast<unsigned long long>(value));
        std::fflush(stdout);
        if (!yes) ++failures;
    }

    void* CreateResource(D3D12_HEAP_TYPE heap, const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = heap;
        void* res = nullptr;
        SX_REQUIRE(Call(device, kDevCreateCommittedResource, P(&hp), uintptr_t{0}, P(&desc), static_cast<uintptr_t>(state),
                        uintptr_t{0}, uintptr_t{0}, P(&res)) == 0 && res);
        return res;
    }
    // A texture with guest memory, and a view of it in descriptor `slot`.
    void* GuestTexture(uint32_t slot, uint8_t** memory) {
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kSize;
        td.Height = kSize;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        void* tex = CreateResource(D3D12_HEAP_TYPE_DEFAULT, td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        *memory = reinterpret_cast<uint8_t*>(Call(tex, kResGetGPUVirtualAddress));
        SX_REQUIRE(*memory != nullptr);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        Call(device, kDevCreateShaderResourceView, P(tex), P(&srv), srv_cpu + static_cast<uintptr_t>(slot) * srv_inc);
        return tex;
    }
    void Write(uint8_t* memory, const std::vector<uint32_t>& texels) {
        SX_REQUIRE(xg::CopyIntoSubresource(*layout, memory, 0, 0, texels.data(), kSize * 4));
    }
    // A new allocator and list for every submission: nothing here waits before recording the next.
    void* NewList() {
        void* allocator = nullptr;
        void* list = nullptr;
        SX_REQUIRE(Call(device, kDevCreateCommandAllocator, static_cast<uintptr_t>(D3D12_COMMAND_LIST_TYPE_DIRECT),
                        uintptr_t{0}, P(&allocator)) == 0 && allocator);
        SX_REQUIRE(Call(device, kDevCreateCommandList, uintptr_t{0}, static_cast<uintptr_t>(D3D12_COMMAND_LIST_TYPE_DIRECT),
                        P(allocator), uintptr_t{0}, uintptr_t{0}, P(&list)) == 0 && list);
        void* heaps[] = {srv_heap};
        Call(list, kListSetDescriptorHeaps, uintptr_t{1}, P(heaps), uintptr_t{0});
        Call(list, kListSetGraphicsRootSignature, P(root), uintptr_t{0}, uintptr_t{0});
        Call(list, kListIASetPrimitiveTopology, static_cast<uintptr_t>(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST), uintptr_t{0},
             uintptr_t{0});
        return list;
    }
    // Submits without waiting; returns the fence value that marks it done.
    uint64_t Submit(int q, void* list, const char* what = "list") {
        SX_REQUIRE(Call(list, kListClose) == 0);
        void* lists[] = {list};
        char stage[96];
        std::snprintf(stage, sizeof(stage), "submit:%s:q%d:before-execute", what, q);
        Mark(stage);
        Call(queue[q], kQueueExecuteCommandLists, uintptr_t{1}, P(lists), uintptr_t{0});
        const uint64_t value = ++fence_value[q];
        SX_REQUIRE(Call(queue[q], kQueueSignal, P(fence[q]), static_cast<uintptr_t>(value), uintptr_t{0}) == 0);
        std::snprintf(stage, sizeof(stage), "submit:%s:q%d:signaled", what, q);
        Mark(stage);
        return value;
    }
    void Wait(int q, uint64_t value) {
        char stage[96];
        std::snprintf(stage, sizeof(stage), "wait:q%d:for-%llu:begin", q, static_cast<unsigned long long>(value));
        Mark(stage);
        if (Completed(q) < value) {
            SX_REQUIRE(Call(fence[q], kFenceSetEventOnCompletion, static_cast<uintptr_t>(value), P(event), uintptr_t{0}) == 0);
            // The fence value decides, not the event: the event is shared by both fences, and an
            // earlier registration can set it late (seen on the Windows software adapter), so a
            // set event only means "look again". A line every 10 s shows a slow queue's fence
            // moving and a stuck one's standing still.
            const ULONGLONG begin = GetTickCount64();
            ULONGLONG next_line = begin + 10000;
            while (Completed(q) < value && GetTickCount64() - begin < 120000) {
                WaitForSingleObject(event, 50);
                if (GetTickCount64() >= next_line) {
                    std::snprintf(stage, sizeof(stage), "wait:q%d:for-%llu:still-waiting-%llus", q,
                                  static_cast<unsigned long long>(value),
                                  static_cast<unsigned long long>((GetTickCount64() - begin) / 1000));
                    Mark(stage);
                    next_line += 10000;
                }
            }
            if (Completed(q) < value) {
                std::snprintf(stage, sizeof(stage), "wait:q%d:for-%llu:GAVE-UP", q, static_cast<unsigned long long>(value));
                Mark(stage);
                std::printf("test_d3d12_guest_texture_inflight: FAIL (a wait did not complete in 120 s)\n");
                std::fflush(stdout);
                TerminateProcess(GetCurrentProcess(), 5);
            }
        }
        std::snprintf(stage, sizeof(stage), "wait:q%d:for-%llu:done", q, static_cast<unsigned long long>(value));
        Mark(stage);
    }
    // Keeps queue `q` busy: `draws` slow full-target draws, not waited for.
    uint64_t Busy(int q, uint32_t draws) {
        void* list = NewList();
        Call(list, kListSetPipelineState, P(pso_busy), uintptr_t{0}, uintptr_t{0});
        const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kBusySize), static_cast<float>(kBusySize), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(busy_size), static_cast<LONG>(busy_size)};
        Call(list, kListRSSetViewports, uintptr_t{1}, P(&vp), uintptr_t{0});
        Call(list, kListRSSetScissorRects, uintptr_t{1}, P(&scissor), uintptr_t{0});
        const D3D12_CPU_DESCRIPTOR_HANDLE handle{busy_rtv};
        Call(list, kListOMSetRenderTargets, uintptr_t{1}, P(&handle), uintptr_t{0}, uintptr_t{0});
        Call(list, kListSetGraphicsRootDescriptorTable, uintptr_t{0}, srv_gpu, uintptr_t{0});
        for (uint32_t i = 0; i < draws; ++i) Call(list, kListDrawInstanced, uintptr_t{3}, uintptr_t{1}, uintptr_t{0}, uintptr_t{0});
        return Submit(q, list, "busy");
    }
    // Draws texture `slot` into band `band` of the target, on queue `q`, not waited for.
    uint64_t DrawBand(int q, uint32_t slot, uint32_t band) {
        void* list = NewList();
        Call(list, kListSetPipelineState, P(pso), uintptr_t{0}, uintptr_t{0});
        const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, static_cast<LONG>(band * kBand), static_cast<LONG>(kSize), static_cast<LONG>((band + 1) * kBand)};
        Call(list, kListRSSetViewports, uintptr_t{1}, P(&vp), uintptr_t{0});
        Call(list, kListRSSetScissorRects, uintptr_t{1}, P(&scissor), uintptr_t{0});
        const D3D12_CPU_DESCRIPTOR_HANDLE handle{rtv};
        Call(list, kListOMSetRenderTargets, uintptr_t{1}, P(&handle), uintptr_t{0}, uintptr_t{0});
        Call(list, kListSetGraphicsRootDescriptorTable, uintptr_t{0}, srv_gpu + static_cast<uintptr_t>(slot) * srv_inc, uintptr_t{0});
        Call(list, kListDrawInstanced, uintptr_t{3}, uintptr_t{1}, uintptr_t{0}, uintptr_t{0});
        return Submit(q, list, "band");
    }
    // Reads the target back (queue 0, waited for) and compares band by band.
    void ExpectBands(const char* part, const std::vector<std::vector<uint32_t>>& want) {
        void* list = NewList();
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = static_cast<ID3D12Resource*>(target);
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        Call(list, kListResourceBarrier, uintptr_t{1}, P(&b), uintptr_t{0});
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
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        Call(list, kListResourceBarrier, uintptr_t{1}, P(&b), uintptr_t{0});
        Wait(0, Submit(0, list, "readback"));
        uint8_t* data = nullptr;
        SX_REQUIRE(Call(readback, kResMap, uintptr_t{0}, uintptr_t{0}, P(&data)) == 0 && data);
        for (uint32_t band = 0; band < want.size(); ++band) {
            uint32_t exact = 0;
            for (uint32_t y = band * kBand; y < (band + 1) * kBand; ++y) {
                for (uint32_t x = 0; x < kSize; ++x) {
                    uint32_t got;
                    std::memcpy(&got, data + static_cast<size_t>(y) * kPitch + x * 4, 4);
                    if (got == want[band][y * kSize + x]) ++exact;
                }
            }
            const bool ok = exact == kBand * kSize;
            std::printf("[in-flight] %s band %u: %u/%u texels as written -> %s\n", part, band, exact, kBand * kSize, ok ? "ok" : "WRONG");
            if (!ok) ++failures;
        }
        std::fflush(stdout);
        Call(readback, kResUnmap, uintptr_t{0}, uintptr_t{0});
    }
};

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
        std::printf("[in-flight] D3D12XboxCreateDevice FAILED\n");
        return 3;
    }
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    for (int q = 0; q < 2; ++q) {
        SX_REQUIRE(Call(r.device, kDevCreateCommandQueue, P(&qdesc), uintptr_t{0}, P(&r.queue[q])) == 0 && r.queue[q]);
        SX_REQUIRE(Call(r.device, kDevCreateFence, uintptr_t{0}, uintptr_t{0}, uintptr_t{0}, P(&r.fence[q])) == 0 && r.fence[q]);
    }
    r.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 64;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    SX_REQUIRE(Call(r.device, kDevCreateDescriptorHeap, P(&hd), uintptr_t{0}, P(&r.srv_heap)) == 0 && r.srv_heap);
    void* rtv_heap = nullptr;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    SX_REQUIRE(Call(r.device, kDevCreateDescriptorHeap, P(&hd), uintptr_t{0}, P(&rtv_heap)) == 0 && rtv_heap);
    r.srv_cpu = static_cast<uintptr_t>(Call(r.srv_heap, kHeapGetCpuStart));
    r.srv_gpu = static_cast<uintptr_t>(Call(r.srv_heap, kHeapGetGpuStart));
    r.srv_inc = static_cast<uint32_t>(Call(r.device, 15, static_cast<uintptr_t>(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)));
    r.rtv = static_cast<uintptr_t>(Call(rtv_heap, kHeapGetCpuStart));
    r.busy_rtv = r.rtv + static_cast<uintptr_t>(Call(r.device, 15, static_cast<uintptr_t>(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)));

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = kSize;
    rd.Height = kSize;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    r.target = r.CreateResource(D3D12_HEAP_TYPE_DEFAULT, rd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    rd.Width = kBusySize;
    rd.Height = kBusySize;
    r.busy_target = r.CreateResource(D3D12_HEAP_TYPE_DEFAULT, rd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Call(r.device, kDevCreateRenderTargetView, P(r.target), uintptr_t{0}, r.rtv);
    Call(r.device, kDevCreateRenderTargetView, P(r.busy_target), uintptr_t{0}, r.busy_rtv);
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = static_cast<uint64_t>(kPitch) * kSize;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    r.readback = r.CreateResource(D3D12_HEAP_TYPE_READBACK, bd, D3D12_RESOURCE_STATE_COPY_DEST);

    XG_TEXTURE2D_DESC xd{};
    xd.Width = kSize;
    xd.Height = kSize;
    xd.MipLevels = 1;
    xd.ArraySize = 1;
    xd.Format = 28;  // DXGI_FORMAT_R8G8B8A8_UNORM
    xd.SampleDesc.Count = 1;
    xd.BindFlags = 0x8;
    xd.SwizzleMode = XG_SWIZZLE_MODE_4KB_S;
    auto layout = std::make_unique<XG_RESOURCE_LAYOUT>();
    xg::ComputeTexture2DLayout(xd, layout.get());
    r.layout = layout.get();

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
    ID3DBlob *rs_blob = nullptr, *err = nullptr, *vs = nullptr, *ps = nullptr, *ps_busy = nullptr;
    SX_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &err)));
    SX_REQUIRE(Call(r.device, kDevCreateRootSignature, uintptr_t{0}, P(rs_blob->GetBufferPointer()),
                    static_cast<uintptr_t>(rs_blob->GetBufferSize()), uintptr_t{0}, P(&r.root)) == 0 && r.root);
    SX_REQUIRE(SUCCEEDED(D3DCompile(kHlsl, std::strlen(kHlsl), "inflight.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs, &err)));
    SX_REQUIRE(SUCCEEDED(D3DCompile(kHlsl, std::strlen(kHlsl), "inflight.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &ps, &err)));
    SX_REQUIRE(SUCCEEDED(D3DCompile(kHlsl, std::strlen(kHlsl), "inflight.hlsl", nullptr, nullptr, "PSBusy", "ps_5_0", 0, 0, &ps_busy, &err)));
    auto pipeline = [&](ID3DBlob* pixel) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = static_cast<ID3D12RootSignature*>(r.root);
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pd.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = 0xFFFFFFFFu;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1;
        void* pso = nullptr;
        SX_REQUIRE(Call(r.device, kDevCreateGraphicsPipelineState, P(&pd), uintptr_t{0}, P(&pso)) == 0 && pso);
        return pso;
    };
    r.pso = pipeline(ps);
    r.pso_busy = pipeline(ps_busy);

    uint8_t* memory = nullptr;
    void* texture = r.GuestTexture(0, &memory);
    (void)texture;
    // Both pipelines drawn with once, and the texture written twice, before anything depends on timing.
    r.Mark("setup:done");
    r.Write(memory, Pattern(1));
    r.Wait(0, r.Busy(0, 1));
    r.Write(memory, Pattern(2));
    r.Wait(0, r.DrawBand(0, 0, 0));
    // How long a busy draw takes here: timed on a 256x256 area, then the area is scaled so that one
    // draw is about 20 ms (64 to 2048 a side) and a block is as many draws as make about 100 ms.
    {
        const ULONGLONG t0 = GetTickCount64();
        r.Wait(0, r.Busy(0, 2));
        const double per_draw_ms = std::max(0.05, static_cast<double>(GetTickCount64() - t0) / 2.0);
        const double scale = std::sqrt(20.0 / per_draw_ms);
        r.busy_size = static_cast<uint32_t>(std::min(2048.0, std::max(64.0, 256.0 * scale)));
        const double expected_ms = per_draw_ms * (static_cast<double>(r.busy_size) * r.busy_size) / (256.0 * 256.0);
        r.busy_draws = static_cast<uint32_t>(std::min(6.0, std::max(1.0, std::ceil(100.0 / std::max(1.0, expected_ms)))));
        std::printf("[in-flight] calibration: a busy draw over 256x256 took %.1f ms; busy area %ux%u (about %.0f ms a draw), "
                    "%u draws a block\n", per_draw_ms, r.busy_size, r.busy_size, expected_ms, r.busy_draws);
        std::fflush(stdout);
    }

    // 1. Queue 0 is busy; a write, a submission to queue 0 (which takes the upload), then a draw
    //    on queue 1 at once. Three rounds, each into its own band.
    {
        std::vector<std::vector<uint32_t>> want;
        uint64_t last0 = 0, last1 = 0;
        r.Mark("part1:begin");
        for (uint32_t round = 0; round < 3; ++round) {
            const uint64_t block = r.Busy(0, r.busy_draws);
            want.push_back(Pattern(10 + round));
            r.Write(memory, want.back());
            last0 = r.Busy(0, 1);                 // the first submission after the write: the upload goes with it
            r.RequireInFlight("1 second queue", 0, block);  // the upload is queued behind the busy block
            last1 = r.DrawBand(1, 0, round);      // another queue, straight away
            r.Wait(1, last1);                     // the next round's write must not race this draw
        }
        r.Wait(0, last0);
        r.ExpectBands("1 second queue", want);
    }

    // 2. Queue 0 busy, then six writes, each followed at once by a draw of it on queue 0.
    {
        std::vector<std::vector<uint32_t>> want;
        r.Mark("part2:begin");
        const uint64_t block = r.Busy(0, r.busy_draws);
        uint64_t last = 0;
        for (uint32_t i = 0; i < 6; ++i) {
            want.push_back(Pattern(20 + i));
            r.Write(memory, want.back());
            last = r.DrawBand(0, 0, i);
            // After the fourth, every staging slot holds an upload queued behind the busy block.
            // The fifth submission then has to wait for the oldest slot, which lets the block
            // finish, so this can only be asked here.
            if (i == 3) r.RequireInFlight("2 six in flight (four slots taken)", 0, block);
        }
        r.Wait(0, last);
        r.ExpectBands("2 six in flight", want);
    }

    // 3. A second texture, written, drawn, and released before its upload can have run.
    {
        uint8_t* memory2 = nullptr;
        void* second = r.GuestTexture(1, &memory2);
        const std::vector<uint32_t> p = Pattern(30);
        r.Mark("part3:begin");
        const uint64_t block = r.Busy(0, r.busy_draws);
        r.Write(memory2, p);
        const uint64_t last = r.DrawBand(0, 1, 0);
        SX_REQUIRE(Call(second, 2) == 0);  // Release: the title's last reference
        r.RequireInFlight("3 released early", 0, block);  // released before its upload could run
        r.Wait(0, last);
        r.ExpectBands("3 released early", {p});
    }

    // 4. Forty textures. Every step is waited for; this part is about which textures get uploaded.
    {
        r.Mark("part4:begin");
        constexpr uint32_t kCount = 40, kFirstSlot = 8;
        std::vector<void*> tex(kCount);
        std::vector<uint8_t*> mem(kCount);
        std::vector<std::vector<uint32_t>> have(kCount);
        for (uint32_t i = 0; i < kCount; ++i) {
            tex[i] = r.GuestTexture(kFirstSlot + i, &mem[i]);
            have[i] = Pattern(100 + i);
            r.Write(mem[i], have[i]);
        }
        const uint32_t show[8] = {0, 1, 7, 13, 20, 26, 33, 39};
        auto check = [&](const char* part) {
            uint64_t last = 0;
            std::vector<std::vector<uint32_t>> want;
            for (uint32_t b = 0; b < 8; ++b) {
                last = r.DrawBand(0, kFirstSlot + show[b], b);
                want.push_back(have[show[b]]);
            }
            r.Wait(0, last);
            r.ExpectBands(part, want);
        };
        check("4 forty written");
        // Three of the shown ones written again, twice running; the rest left alone.
        for (uint32_t pass = 0; pass < 2; ++pass) {
            for (uint32_t i : {1u, 20u, 39u}) {
                have[i] = Pattern(200 + i + pass * 50);
                r.Write(mem[i], have[i]);
            }
            check(pass == 0 ? "4 three rewritten" : "4 three rewritten again");
        }
        check("4 nothing written");
        // Every odd texture released, then created again in the same slot: its memory may be the
        // memory another just gave back. Only two of the new ones are written.
        for (uint32_t i = 1; i < kCount; i += 2) SX_REQUIRE(Call(tex[i], 2) == 0);
        for (uint32_t i = 1; i < kCount; i += 2) {
            tex[i] = r.GuestTexture(kFirstSlot + i, &mem[i]);
            have[i].assign(kSize * kSize, 0);  // a new texture nobody wrote: zero
        }
        for (uint32_t i : {7u, 33u}) {
            have[i] = Pattern(400 + i);
            r.Write(mem[i], have[i]);
        }
        have[26] = Pattern(426);
        r.Write(mem[26], have[26]);  // one that was never released
        check("4 released and created again");
    }

    if (r.failures) {
        std::printf("test_d3d12_guest_texture_inflight: FAIL (%d check(s) failed)\n", r.failures);
        return 1;
    }
    std::printf("test_d3d12_guest_texture_inflight: OK\n");
    return 0;
}
