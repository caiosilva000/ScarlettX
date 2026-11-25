// Shared plumbing for tests that drive translated compute through the d3d12_xs shim's
// guest-facing objects: a shim device in real-title mode, a title-style root signature and
// descriptor heap made on it, and a raw UAV buffer filled with a sentinel before every dispatch.
// The host device is used directly only for the buffer, its fill and its readback.
#pragma once

#include "sx_check.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <windows.h>
#include <d3d12.h>

#include "scarlettx/gpu/d3d12_xs_abi.h"

namespace sxrig {

constexpr uint32_t kSentinel = 0xA5A5A5A5u;

template <typename... Args>
intptr_t Call(void* object, int slot, Args... args) {
    using Fn = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, Args...);
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[slot])(object, args...);
}
inline uintptr_t P(const void* p) { return reinterpret_cast<uintptr_t>(p); }

inline D3D12XBOX_SHADER_TRANSLATION_STATS Stats() {
    D3D12XBOX_SHADER_TRANSLATION_STATS s{};
    SX_REQUIRE(D3D12XboxGetShaderTranslationStats(&s) == 0);
    return s;
}

struct GuestRig {
    const char* tag = "[rig]";
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
    void* title_root = nullptr;  // parameter 0: four root constants; parameter 1: a table of one UAV
    uint32_t uav_dwords = 0;
    UINT view_slot = 0;

    int Fail(const char* api, HRESULT hr) const {
        std::printf("%s FAILED api=%s hr=0x%08x\n", tag, api, static_cast<uint32_t>(hr));
        std::fflush(stdout);
        return 2;
    }

    HRESULT MakeBuffer(D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                       ID3D12Resource** out) {
        D3D12_HEAP_PROPERTIES heap_props{};
        heap_props.Type = type;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = static_cast<UINT64>(uav_dwords) * 4;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer.Flags = flags;
        return device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &buffer, state, nullptr,
                                               IID_PPV_ARGS(out));
    }

    HRESULT CreateRoot(const D3D12_ROOT_SIGNATURE_DESC& desc, void** out_root) {
        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        const HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (FAILED(hr)) return hr;
        *out_root = nullptr;
        return static_cast<HRESULT>(Call(shim_device, 16, uintptr_t{0}, P(blob->GetBufferPointer()),
                                         static_cast<uintptr_t>(blob->GetBufferSize()), uintptr_t{0}, P(out_root)));
    }

    // CreateComputePipelineState on the shim's device object (vtable slot 11).
    HRESULT CreatePipeline(void* shim_root, const std::vector<uint8_t>& shader, void** out_pso) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = static_cast<ID3D12RootSignature*>(shim_root);
        desc.CS.pShaderBytecode = shader.data();
        desc.CS.BytecodeLength = shader.size();
        *out_pso = nullptr;
        return static_cast<HRESULT>(Call(shim_device, 11, P(&desc), uintptr_t{0}, P(out_pso)));
    }

    // 0, or an exit code (3 no device, 2 a call failed).
    int Init(bool prefer_warp, uint32_t dwords, UINT heap_slots, UINT slot) {
        uav_dwords = dwords;
        view_slot = slot;
        _putenv_s("SCARLETTX_REAL_TITLE", "1");
        SetEnvironmentVariableA("SCARLETTX_REAL_TITLE", "1");
        D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
        params.Version = 1;
        params.PreferWarp = prefer_warp ? 1 : 0;
        const int32_t created = D3D12XboxCreateDevice(nullptr, &params, &__uuidof(ID3D12Device), &shim_device);
        if (created < 0 || !shim_device) {
            std::printf("%s D3D12XboxCreateDevice FAILED hr=0x%08x\n", tag, static_cast<uint32_t>(created));
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
        hr = MakeBuffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_DEST, &uav);
        if (FAILED(hr)) return Fail("CreateCommittedResource(UAV)", hr);
        hr = MakeBuffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, &upload);
        if (FAILED(hr)) return Fail("CreateCommittedResource(Upload)", hr);
        hr = MakeBuffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &readback);
        if (FAILED(hr)) return Fail("CreateCommittedResource(Readback)", hr);
        void* mapped = nullptr;
        hr = upload->Map(0, nullptr, &mapped);
        if (FAILED(hr) || !mapped) return Fail("ID3D12Resource::Map(Upload)", FAILED(hr) ? hr : E_POINTER);
        for (uint32_t i = 0; i < uav_dwords; ++i) std::memcpy(static_cast<uint8_t*>(mapped) + i * 4, &kSentinel, 4);
        upload->Unmap(0, nullptr);

        // The title's descriptor heap, made on the shim's device object (slot 14).
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = heap_slots;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = static_cast<HRESULT>(Call(shim_device, 14, P(&heap_desc), uintptr_t{0}, P(&shim_heap)));
        if (FAILED(hr) || !shim_heap) return Fail("shim CreateDescriptorHeap (slot 14)", hr);
        SX_REQUIRE(D3D12XboxGetNativeObject(shim_heap, reinterpret_cast<void**>(&heap)) == 0 && heap);
        const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = uav_dwords;
        view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(view_slot) * increment;
        device->CreateUnorderedAccessView(uav, nullptr, &view, cpu);
        view_handle = heap->GetGPUDescriptorHandleForHeapStart();
        view_handle.ptr += static_cast<UINT64>(view_slot) * increment;

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
        hr = CreateRoot(title_rs, &title_root);
        if (FAILED(hr) || !title_root) return Fail("shim CreateRootSignature (slot 16)", hr);
        return 0;
    }

    // One dispatch as a title makes it, on the shim's command list: heaps, root signature,
    // pipeline, root constants at parameter 0, the table at `table_param` (unless negative),
    // Dispatch(groups). Returns 0 and the whole buffer, or an exit code.
    int RunDispatch(void* shim_root, void* shim_pso, const uint32_t* constants, UINT constant_count,
                    int table_param, const UINT groups[3], std::vector<uint32_t>& out) {
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

        auto transition = [&](D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = uav;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &barrier);
        };
        const UINT64 bytes = static_cast<UINT64>(uav_dwords) * 4;
        list->CopyBufferRegion(uav, 0, upload, 0, bytes);
        transition(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        void* heaps[] = {shim_heap};
        Call(shim_list, 32, uintptr_t{1}, P(heaps), uintptr_t{0});                 // SetDescriptorHeaps
        Call(shim_list, 33, P(shim_root), uintptr_t{0}, uintptr_t{0});             // SetComputeRootSignature
        Call(shim_list, 25, P(shim_pso), uintptr_t{0}, uintptr_t{0});              // SetPipelineState
        Call(shim_list, 39, uintptr_t{0}, static_cast<uintptr_t>(constant_count), P(constants),
             uintptr_t{0});                                                        // SetComputeRoot32BitConstants
        if (table_param >= 0) {
            Call(shim_list, 35, static_cast<uintptr_t>(table_param), static_cast<uintptr_t>(view_handle.ptr),
                 uintptr_t{0});                                                    // SetComputeRootDescriptorTable
        }
        Call(shim_list, 14, static_cast<uintptr_t>(groups[0]), static_cast<uintptr_t>(groups[1]),
             static_cast<uintptr_t>(groups[2]));                                   // Dispatch

        transition(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback, 0, uav, 0, bytes);
        transition(D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        hr = static_cast<HRESULT>(Call(shim_list, 9, uintptr_t{0}, uintptr_t{0}, uintptr_t{0}));  // Close
        if (FAILED(hr)) return Fail("shim ID3D12GraphicsCommandList::Close (slot 9)", hr);

        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        const UINT64 target = ++fence_value;
        hr = queue->Signal(fence, target);
        if (FAILED(hr)) return Fail("ID3D12CommandQueue::Signal", hr);
        DWORD wait = 0;
        if (fence->GetCompletedValue() < target) {
            hr = fence->SetEventOnCompletion(target, event);
            if (FAILED(hr)) return Fail("ID3D12Fence::SetEventOnCompletion", hr);
            wait = WaitForSingleObject(event, 10000);
        }
        const UINT64 completed = fence->GetCompletedValue();
        std::printf("%s fence signaled=%llu completed=%llu wait_result=%lu\n", tag,
                    static_cast<unsigned long long>(target), static_cast<unsigned long long>(completed),
                    static_cast<unsigned long>(wait));
        if (completed < target) {
            return Fail("ID3D12Fence::GetCompletedValue (fence not reached)", device->GetDeviceRemovedReason());
        }
        void* mapped = nullptr;
        hr = readback->Map(0, nullptr, &mapped);
        if (FAILED(hr) || !mapped) return Fail("ID3D12Resource::Map(Readback)", FAILED(hr) ? hr : E_POINTER);
        out.resize(uav_dwords);
        std::memcpy(out.data(), mapped, static_cast<size_t>(bytes));
        readback->Unmap(0, nullptr);
        list->Release();
        return 0;
    }
};

}  // namespace sxrig
