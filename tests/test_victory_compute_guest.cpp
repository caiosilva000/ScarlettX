// Translated compute through the guest-facing route, synthetic only.
//
// A console-shaped shader container (DXBC, a DXIL part with an XDAT index, the native code in an
// XBCH/FSSM blob) is written by the bridge and handed to the d3d12_xs shim the way a title hands
// it over: CreateComputePipelineState on the shim's device object, with the title's own root
// signature (four root constants, one descriptor table). The compute state and the dispatch go
// through the shim's command-list object. The host device is used directly only for plumbing:
// the buffer, its fill and its readback.
//
// The program: out[0] = ((c0 + c1) ^ c2) + c3, one thread, `out` the raw UAV the title's table
// points at (heap index 3, so the table-to-heap-index mapping is exercised).
//
// Controls: a malformed native stream, a console container without native code, a native
// program the translator refuses, binding layouts the route cannot make, and an ordinary
// desktop DXIL shader, which must go to the host untouched.
//
// Exit codes: 0 pass, 1 a check failed, 2 a D3D12 call failed (name and HRESULT printed),
// 3 no device, 4 the fixture could not be built.
#include "sx_check.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d12.h>

#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "scarlettx/gpu/victory_shader_bridge.h"

namespace {

constexpr uint32_t kUavDwords = 4;
constexpr uint32_t kSentinel = 0xA5A5A5A5u;
constexpr UINT kHeapSlots = 8;
constexpr UINT kViewSlot = 3;

uint32_t Expected(const uint32_t c[4]) { return ((c[0] + c[1]) ^ c[2]) + c[3]; }

const uint32_t kCases[][4] = {
    {1, 2, 3, 4},
    {0x12345678u, 0x9ABCDEF0u, 0x0F0F0F0Fu, 0x11111111u},
    {0xFFFFFFFFu, 1, 0, 0},
    {0, 0, 0xFFFFFFFFu, 1},
    {0xDEADBEEFu, 0x00000001u, 0xCAFEF00Du, 0x80000000u},
};

template <typename... Args>
intptr_t Call(void* object, int slot, Args... args) {
    using Fn = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, Args...);
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[slot])(object, args...);
}
uintptr_t P(const void* p) { return reinterpret_cast<uintptr_t>(p); }

int Fail(const char* api, HRESULT hr) {
    std::printf("[victory-guest] FAILED api=%s hr=0x%08x\n", api, static_cast<uint32_t>(hr));
    std::fflush(stdout);
    return 2;
}

D3D12XBOX_SHADER_TRANSLATION_STATS Stats() {
    D3D12XBOX_SHADER_TRANSLATION_STATS s{};
    SX_REQUIRE(D3D12XboxGetShaderTranslationStats(&s) == 0);
    return s;
}

struct Rig {
    void* shim_device = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    UINT64 fence_value = 0;
    void* shim_heap = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12Resource* uav = nullptr;
    ID3D12Resource* upload = nullptr;
    ID3D12Resource* readback = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE view_handle{};

    HRESULT MakeBuffer(D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                       ID3D12Resource** out) {
        D3D12_HEAP_PROPERTIES heap_props{};
        heap_props.Type = type;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = kUavDwords * 4;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer.Flags = flags;
        return device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &buffer, state, nullptr,
                                               IID_PPV_ARGS(out));
    }
};

// One dispatch: the title's calls go to the shim's command list, in the order a title makes
// them. `constants` is root parameter 0 as the title sets it; `table_param` < 0 leaves the
// descriptor table unset. Returns 0 and the read-back buffer, or an exit code.
int RunDispatch(Rig& rig, void* shim_root, void* shim_pso, const uint32_t* constants, UINT constant_count,
                int table_param, std::vector<uint32_t>& out) {
    void* shim_alloc = nullptr;
    HRESULT hr = static_cast<HRESULT>(Call(rig.shim_device, 9, static_cast<uintptr_t>(D3D12_COMMAND_LIST_TYPE_DIRECT),
                                           uintptr_t{0}, P(&shim_alloc)));
    if (FAILED(hr) || !shim_alloc) return Fail("shim CreateCommandAllocator (slot 9)", hr);
    void* shim_list = nullptr;
    hr = static_cast<HRESULT>(Call(rig.shim_device, 12, uintptr_t{0},
                                   static_cast<uintptr_t>(D3D12_COMMAND_LIST_TYPE_DIRECT), P(shim_alloc), uintptr_t{0},
                                   uintptr_t{0}, P(&shim_list)));
    if (FAILED(hr) || !shim_list) return Fail("shim CreateCommandList (slot 12)", hr);
    ID3D12GraphicsCommandList* list = nullptr;
    SX_REQUIRE(D3D12XboxGetNativeObject(shim_list, reinterpret_cast<void**>(&list)) == 0 && list);

    // Plumbing on the host list: put the sentinel in the buffer.
    auto transition = [&](D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = rig.uav;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
    };
    list->CopyBufferRegion(rig.uav, 0, rig.upload, 0, kUavDwords * 4);
    transition(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // The title's compute state, on the shim's list.
    void* heaps[] = {rig.shim_heap};
    Call(shim_list, 32, uintptr_t{1}, P(heaps), uintptr_t{0});                 // SetDescriptorHeaps
    Call(shim_list, 33, P(shim_root), uintptr_t{0}, uintptr_t{0});             // SetComputeRootSignature
    Call(shim_list, 25, P(shim_pso), uintptr_t{0}, uintptr_t{0});              // SetPipelineState
    Call(shim_list, 39, uintptr_t{0}, static_cast<uintptr_t>(constant_count), P(constants),
         uintptr_t{0});                                                        // SetComputeRoot32BitConstants
    if (table_param >= 0) {
        Call(shim_list, 35, static_cast<uintptr_t>(table_param), static_cast<uintptr_t>(rig.view_handle.ptr),
             uintptr_t{0});                                                    // SetComputeRootDescriptorTable
    }
    Call(shim_list, 14, uintptr_t{1}, uintptr_t{1}, uintptr_t{1});             // Dispatch

    transition(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(rig.readback, 0, rig.uav, 0, kUavDwords * 4);
    transition(D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    hr = static_cast<HRESULT>(Call(shim_list, 9, uintptr_t{0}, uintptr_t{0}, uintptr_t{0}));  // Close
    if (FAILED(hr)) return Fail("shim ID3D12GraphicsCommandList::Close (slot 9)", hr);

    ID3D12CommandList* lists[] = {list};
    rig.queue->ExecuteCommandLists(1, lists);
    const UINT64 target = ++rig.fence_value;
    hr = rig.queue->Signal(rig.fence, target);
    if (FAILED(hr)) return Fail("ID3D12CommandQueue::Signal", hr);
    DWORD wait = 0;
    if (rig.fence->GetCompletedValue() < target) {
        hr = rig.fence->SetEventOnCompletion(target, rig.event);
        if (FAILED(hr)) return Fail("ID3D12Fence::SetEventOnCompletion", hr);
        wait = WaitForSingleObject(rig.event, 10000);
    }
    const UINT64 completed = rig.fence->GetCompletedValue();
    std::printf("[victory-guest] fence signaled=%llu completed=%llu wait_result=%lu\n",
                static_cast<unsigned long long>(target), static_cast<unsigned long long>(completed),
                static_cast<unsigned long>(wait));
    if (completed < target) return Fail("ID3D12Fence::GetCompletedValue (fence not reached)", rig.device->GetDeviceRemovedReason());

    void* mapped = nullptr;
    hr = rig.readback->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped) return Fail("ID3D12Resource::Map(Readback)", FAILED(hr) ? hr : E_POINTER);
    out.resize(kUavDwords);
    std::memcpy(out.data(), mapped, kUavDwords * 4);
    rig.readback->Unmap(0, nullptr);
    list->Release();
    return 0;
}

// Creates a compute pipeline on the shim's device object from `shader` and the title's root.
HRESULT CreatePipeline(Rig& rig, void* shim_root, const std::vector<uint8_t>& shader, void** out_pso) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = static_cast<ID3D12RootSignature*>(shim_root);
    desc.CS.pShaderBytecode = shader.data();
    desc.CS.BytecodeLength = shader.size();
    *out_pso = nullptr;
    return static_cast<HRESULT>(Call(rig.shim_device, 11, P(&desc), uintptr_t{0}, P(out_pso)));
}

HRESULT CreateRoot(Rig& rig, const D3D12_ROOT_SIGNATURE_DESC& desc, void** out_root) {
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) return hr;
    *out_root = nullptr;
    return static_cast<HRESULT>(Call(rig.shim_device, 16, uintptr_t{0}, P(blob->GetBufferPointer()),
                                     static_cast<uintptr_t>(blob->GetBufferSize()), uintptr_t{0}, P(out_root)));
}

}  // namespace

int main(int argc, char** argv) {
    bool prefer_warp = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--warp") prefer_warp = true;
        else {
            std::fprintf(stderr, "usage: test_victory_compute_guest [--warp]\n");
            return 64;
        }
    }
    // Unbuffered: if a host call hangs or aborts, what ran before it is still on record.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("[victory-guest] %s: %s\n", ok ? "ok" : "CHECK FAILED", what);
        if (!ok) ++failures;
    };

    // Binding layouts the route can and cannot make (no device needed).
    {
        scarlettx::VictoryComputeBinding b;
        std::string why;
        b.user_data_dwords = 8;
        b.user_data = {{0, 0, 0, 0}, {4, 1, 1, 0}};
        check(scarlettx::VictoryComputeBindingSupported(b, why), "root constants and one table are a supported layout");
        auto unsupported = [&](scarlettx::VictoryComputeBinding v, const char* what) {
            std::string reason;
            const bool refused = !scarlettx::VictoryComputeBindingSupported(v, reason);
            std::printf("[victory-guest]   %s -> %s\n", what, refused ? reason.c_str() : "(accepted)");
            check(refused && !reason.empty(), what);
        };
        auto v = b; v.root_cbv_count = 1; unsupported(v, "unsupported layout: a root constant buffer");
        v = b; v.root_uav_count = 1; unsupported(v, "unsupported layout: a root buffer view");
        v = b; v.static_sampler_count = 1; unsupported(v, "unsupported layout: a static sampler");
        v = b; v.flags = 1; unsupported(v, "unsupported layout: title memory");
        v = b; v.user_data.push_back({5, 2, 2, 0}); unsupported(v, "unsupported layout: user data from a root descriptor");
        v = b; v.user_data_dwords = 68; unsupported(v, "unsupported layout: user data larger than a root signature");
        v = b; v.user_data.push_back({9, 0, 0, 0}); unsupported(v, "unsupported layout: an entry outside the user data");
    }

    std::vector<uint8_t> fixture;
    std::vector<uint8_t> malformed;
    std::vector<uint8_t> no_native;
    std::vector<uint8_t> refused;
    if (!scarlettx::BuildVictorySyntheticContainer(0, fixture).ok() ||
        !scarlettx::BuildVictorySyntheticContainer(1, malformed).ok() ||
        !scarlettx::BuildVictorySyntheticContainer(2, no_native).ok() ||
        !scarlettx::BuildVictorySyntheticContainer(3, refused).ok()) {
        std::printf("[victory-guest] the synthetic containers could not be built\n");
        return 4;
    }
    std::printf("[victory-guest] fixture container: %zu bytes, magic=%.4s, part=%.4s, index=%.4s\n", fixture.size(),
                reinterpret_cast<const char*>(fixture.data()), reinterpret_cast<const char*>(fixture.data() + 36),
                reinterpret_cast<const char*>(fixture.data() + 44 + 0x18));

    _putenv_s("SCARLETTX_REAL_TITLE", "1");
    SetEnvironmentVariableA("SCARLETTX_REAL_TITLE", "1");
    Rig rig;
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    params.PreferWarp = prefer_warp ? 1 : 0;
    const int32_t created = D3D12XboxCreateDevice(nullptr, &params, &__uuidof(ID3D12Device), &rig.shim_device);
    if (created < 0 || !rig.shim_device) {
        std::printf("[victory-guest] D3D12XboxCreateDevice FAILED hr=0x%08x\n", static_cast<uint32_t>(created));
        return 3;
    }
    SX_REQUIRE(D3D12XboxGetNativeObject(rig.shim_device, reinterpret_cast<void**>(&rig.device)) == 0 && rig.device);

    // Host plumbing.
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT hr = rig.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&rig.queue));
    if (FAILED(hr)) return Fail("CreateCommandQueue", hr);
    hr = rig.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&rig.fence));
    if (FAILED(hr)) return Fail("CreateFence", hr);
    rig.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = rig.MakeBuffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_DEST, &rig.uav);
    if (FAILED(hr)) return Fail("CreateCommittedResource(UAV)", hr);
    hr = rig.MakeBuffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, &rig.upload);
    if (FAILED(hr)) return Fail("CreateCommittedResource(Upload)", hr);
    hr = rig.MakeBuffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &rig.readback);
    if (FAILED(hr)) return Fail("CreateCommittedResource(Readback)", hr);
    void* mapped = nullptr;
    hr = rig.upload->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped) return Fail("ID3D12Resource::Map(Upload)", FAILED(hr) ? hr : E_POINTER);
    for (uint32_t i = 0; i < kUavDwords; ++i) std::memcpy(static_cast<uint8_t*>(mapped) + i * 4, &kSentinel, 4);
    rig.upload->Unmap(0, nullptr);

    // The title's descriptor heap, made on the shim's device object; its UAV sits at slot 3.
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = kHeapSlots;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = static_cast<HRESULT>(Call(rig.shim_device, 14, P(&heap_desc), uintptr_t{0}, P(&rig.shim_heap)));
    if (FAILED(hr) || !rig.shim_heap) return Fail("shim CreateDescriptorHeap (slot 14)", hr);
    SX_REQUIRE(D3D12XboxGetNativeObject(rig.shim_heap, reinterpret_cast<void**>(&rig.heap)) == 0 && rig.heap);
    const UINT increment = rig.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    view.Buffer.NumElements = kUavDwords;
    view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = rig.heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(kViewSlot) * increment;
    rig.device->CreateUnorderedAccessView(rig.uav, nullptr, &view, cpu);
    rig.view_handle = rig.heap->GetGPUDescriptorHandleForHeapStart();
    rig.view_handle.ptr += static_cast<UINT64>(kViewSlot) * increment;

    // The title's root signature: parameter 0 four root constants, parameter 1 a table of one UAV.
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER title_params[2]{};
    title_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    title_params[0].Constants.Num32BitValues = 4;
    title_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    title_params[1].DescriptorTable.NumDescriptorRanges = 1;
    title_params[1].DescriptorTable.pDescriptorRanges = &range;
    D3D12_ROOT_SIGNATURE_DESC title_rs{};
    title_rs.NumParameters = 2;
    title_rs.pParameters = title_params;
    void* shim_root = nullptr;
    hr = CreateRoot(rig, title_rs, &shim_root);
    if (FAILED(hr) || !shim_root) return Fail("shim CreateRootSignature (slot 16)", hr);

    // 1. The native container, through the guest-facing pipeline creation.
    auto before = Stats();
    void* shim_pso = nullptr;
    hr = CreatePipeline(rig, shim_root, fixture, &shim_pso);
    auto after = Stats();
    std::printf("[victory-guest] shim CreateComputePipelineState(native container) hr=0x%08x translated=+%llu "
                "passed_through=+%llu rejected=+%llu\n",
                static_cast<uint32_t>(hr), static_cast<unsigned long long>(after.compute_translated - before.compute_translated),
                static_cast<unsigned long long>(after.compute_passed_through - before.compute_passed_through),
                static_cast<unsigned long long>(after.compute_rejected - before.compute_rejected));
    std::fflush(stdout);
    if (FAILED(hr) || !shim_pso) return Fail("shim CreateComputePipelineState (slot 11), native container", hr);
    check(after.compute_translated - before.compute_translated == 1 &&
              after.compute_passed_through == before.compute_passed_through,
          "the native container was recognized and translated at pipeline creation");

    // 2. Dispatches through the shim's command list.
    before = Stats();
    uint32_t exact = 0;
    uint32_t total = 0;
    for (const auto& c : kCases) {
        std::vector<uint32_t> out;
        if (const int rc = RunDispatch(rig, shim_root, shim_pso, c, 4, 1, out)) return rc;
        const uint32_t want[kUavDwords] = {Expected(c), kSentinel, kSentinel, kSentinel};
        bool ok = true;
        for (uint32_t i = 0; i < kUavDwords; ++i) {
            ++total;
            if (out[i] == want[i]) ++exact;
            else ok = false;
        }
        std::printf("[victory-guest] in=%08x,%08x,%08x,%08x out=%08x,%08x,%08x,%08x want=%08x %s\n", c[0], c[1], c[2],
                    c[3], out[0], out[1], out[2], out[3], want[0], ok ? "exact" : "MISMATCH");
    }
    after = Stats();
    std::printf("[victory-guest] dwords exact %u/%u, translated dispatches=+%llu\n", exact, total,
                static_cast<unsigned long long>(after.dispatches_translated - before.dispatches_translated));
    check(exact == total, "every read-back dword is exact");
    check(after.dispatches_translated - before.dispatches_translated == 5 &&
              after.dispatches_skipped == before.dispatches_skipped,
          "all five dispatches ran the translated pipeline");

    // 3. A dispatch without the title's descriptor table: dropped with a diagnostic, buffer untouched.
    {
        before = Stats();
        std::vector<uint32_t> out;
        if (const int rc = RunDispatch(rig, shim_root, shim_pso, kCases[0], 4, -1, out)) return rc;
        after = Stats();
        bool untouched = true;
        for (uint32_t v : out) untouched = untouched && v == kSentinel;
        check(after.dispatches_skipped - before.dispatches_skipped == 1 && untouched,
              "control: a dispatch without the descriptor table is dropped and writes nothing");
    }

    // 4. Controls at pipeline creation.
    auto rejected = [&](const std::vector<uint8_t>& shader, const char* what) {
        const auto b = Stats();
        void* pso = nullptr;
        const HRESULT r = CreatePipeline(rig, shim_root, shader, &pso);
        const auto a = Stats();
        std::printf("[victory-guest] %s: hr=0x%08x pso=%s rejected=+%llu translated=+%llu\n", what,
                    static_cast<uint32_t>(r), pso ? "created" : "none",
                    static_cast<unsigned long long>(a.compute_rejected - b.compute_rejected),
                    static_cast<unsigned long long>(a.compute_translated - b.compute_translated));
        check(FAILED(r) && !pso && a.compute_rejected - b.compute_rejected == 1 &&
                  a.compute_translated == b.compute_translated && a.compute_passed_through == b.compute_passed_through,
              what);
    };
    rejected(malformed, "control: a malformed native stream is refused");
    rejected(no_native, "control: a console container without native code is refused");
    rejected(refused, "control: a native program the translator refuses is refused");

    // 5. An ordinary desktop DXIL shader goes to the host as it is. The translated fixture is one:
    // here the "title" is a desktop program with the host-shaped root signature, setting the
    // user data itself.
    {
        scarlettx::VictoryShaderInfo info;
        scarlettx::VictoryComputeBinding binding;
        std::vector<uint8_t> dxil;
        const auto outcome = scarlettx::TranslateVictoryCompute(fixture.data(), fixture.size(), info, binding, dxil);
        check(outcome == scarlettx::VictoryComputeOutcome::Translated && binding.user_data_dwords == 8,
              "the fixture translates outside the shim as well");
        scarlettx::VictoryComputeBinding ignored_binding;
        std::vector<uint8_t> ignored;
        check(scarlettx::TranslateVictoryCompute(dxil.data(), dxil.size(), info, ignored_binding, ignored) ==
                  scarlettx::VictoryComputeOutcome::Desktop,
              "desktop DXIL is classified as desktop");

        D3D12_ROOT_PARAMETER desktop_param{};
        desktop_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        desktop_param.Constants.Num32BitValues = binding.user_data_dwords;
        D3D12_ROOT_SIGNATURE_DESC desktop_rs{};
        desktop_rs.NumParameters = 1;
        desktop_rs.pParameters = &desktop_param;
        desktop_rs.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(
            D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
            D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);
        void* desktop_root = nullptr;
        hr = CreateRoot(rig, desktop_rs, &desktop_root);
        if (FAILED(hr) || !desktop_root) return Fail("shim CreateRootSignature (desktop)", hr);
        before = Stats();
        void* desktop_pso = nullptr;
        hr = CreatePipeline(rig, desktop_root, dxil, &desktop_pso);
        after = Stats();
        std::printf("[victory-guest] shim CreateComputePipelineState(desktop DXIL) hr=0x%08x passed_through=+%llu translated=+%llu\n",
                    static_cast<uint32_t>(hr),
                    static_cast<unsigned long long>(after.compute_passed_through - before.compute_passed_through),
                    static_cast<unsigned long long>(after.compute_translated - before.compute_translated));
        if (FAILED(hr) || !desktop_pso) return Fail("shim CreateComputePipelineState (slot 11), desktop DXIL", hr);
        check(after.compute_passed_through - before.compute_passed_through == 1 &&
                  after.compute_translated == before.compute_translated,
              "control: desktop DXIL passes through untranslated");

        const uint32_t* c = kCases[4];
        uint32_t user_data[8] = {c[0], c[1], c[2], c[3], kViewSlot * 32, 0, 0, 0};
        before = Stats();
        std::vector<uint32_t> out;
        if (const int rc = RunDispatch(rig, desktop_root, desktop_pso, user_data, 8, -1, out)) return rc;
        after = Stats();
        check(out[0] == Expected(c) && out[1] == kSentinel && out[2] == kSentinel && out[3] == kSentinel &&
                  after.dispatches_translated == before.dispatches_translated &&
                  after.dispatches_skipped == before.dispatches_skipped,
              "control: the desktop pipeline dispatches with the title's own bindings, exact");
    }

    std::printf("[victory-guest] %s (%d failed checks)\n", failures == 0 ? "PASS" : "FAIL", failures);
    std::fflush(stdout);
    return failures == 0 ? 0 : 1;
}
