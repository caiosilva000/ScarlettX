// Directly indexed descriptors on the guest-facing route: retirement and remapping.
//
// A translated native compute program writes 64 dwords through the raw UAV its descriptor table
// names. The translated shader takes that view from the descriptor heap by index, and the shim
// works the index out from the title's table at every dispatch. Buffers A, B and C, a heap of
// four descriptors, and a different c0 in every dispatch so that each result names its dispatch:
//
//   1. slot 0 views A; dispatch                      -> A holds dispatch 1
//   2. slot 0 is given a view of B; dispatch         -> B holds 2, A still holds 1 (no stale binding)
//   3. dispatch into B, copy B out in the same list,
//      release B before the list has run             -> the copy holds 3 (in flight, kept alive)
//   4. slot 0 is given a view of C; dispatch         -> C holds 4, A still holds 1
//   5. one list: table slot 1 (A), table slot 0 (C),
//      table slot 1 (A) again, new constants each    -> A holds 7, C holds 6
//   6. one list: the same table and constants twice,
//      then another program's pipeline and back      -> A holds 9, C holds 8 (bindings reused, then not)
//
// Exit codes: 0 pass, 1 a buffer held the wrong dwords, 2 a call failed, 3 no device,
// 4 the fixture could not be built.
#include "sx_check.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <windows.h>
#include <d3d12.h>

#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "scarlettx/gpu/victory_shader_bridge.h"

namespace {

template <typename... Args>
intptr_t Call(void* object, int slot, Args... args) {
    using Fn = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, Args...);
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[slot])(object, args...);
}
uintptr_t P(const void* p) { return reinterpret_cast<uintptr_t>(p); }

constexpr uint32_t kDwords = 64;
constexpr uint32_t kBytes = kDwords * 4;
constexpr uint32_t kC1 = 0xABCD1234u;

uint32_t Kernel(uint32_t i, uint32_t c0, uint32_t k) {
    uint32_t x = i + c0;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return (x + kC1) ^ k;
}

struct Rig {
    void* device = nullptr;
    void* queue = nullptr;
    void* fence = nullptr;
    HANDLE event = nullptr;
    uint64_t fence_value = 0;
    void* heap = nullptr;
    void* root = nullptr;
    void* pso[2] = {};  // two programs: literals kLiteral[0] and kLiteral[1]
    uintptr_t cpu0 = 0, gpu0 = 0;
    uint32_t inc = 0;
    int failures = 0;

    void* Buffer(D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = type;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = kBytes;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = flags;
        void* res = nullptr;
        SX_REQUIRE(Call(device, 27, P(&hp), uintptr_t{0}, P(&d), static_cast<uintptr_t>(state), uintptr_t{0}, uintptr_t{0},
                        P(&res)) == 0 && res);
        return res;
    }
    void* Target() { return Buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }
    void* Readback() { return Buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST); }
    // Puts a raw view of `buffer` in descriptor `slot`: what a title does when it remaps a slot.
    void View(uint32_t slot, void* buffer) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = kDwords;
        view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        Call(device, 19, P(buffer), uintptr_t{0}, P(&view), cpu0 + static_cast<uintptr_t>(slot) * inc);
    }
    void* NewList(int program) {
        void* allocator = nullptr;
        void* list = nullptr;
        SX_REQUIRE(Call(device, 9, static_cast<uintptr_t>(D3D12_COMMAND_LIST_TYPE_DIRECT), uintptr_t{0}, P(&allocator)) == 0);
        SX_REQUIRE(Call(device, 12, uintptr_t{0}, static_cast<uintptr_t>(D3D12_COMMAND_LIST_TYPE_DIRECT), P(allocator),
                        uintptr_t{0}, uintptr_t{0}, P(&list)) == 0 && list);
        void* heaps[] = {heap};
        Call(list, 32, uintptr_t{1}, P(heaps), uintptr_t{0});
        Call(list, 33, P(root), uintptr_t{0}, uintptr_t{0});
        Call(list, 25, P(pso[program]), uintptr_t{0}, uintptr_t{0});
        return list;
    }
    void Table(void* list, uint32_t slot) { Call(list, 35, uintptr_t{1}, gpu0 + static_cast<uintptr_t>(slot) * inc, uintptr_t{0}); }
    void Constants(void* list, uint32_t c0) {
        const uint32_t c[4] = {c0, kC1, 0, 0};
        Call(list, 39, uintptr_t{0}, uintptr_t{4}, P(c), uintptr_t{0});
    }
    // A dispatch, then a UAV barrier: a later dispatch that writes the same buffer comes after it.
    void Dispatch(void* list) {
        Call(list, 14, static_cast<uintptr_t>(kDwords / 32), uintptr_t{1}, uintptr_t{1});
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        Call(list, 26, uintptr_t{1}, P(&b), uintptr_t{0});
    }
    void CopyOut(void* list, void* buffer, void* readback) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = static_cast<ID3D12Resource*>(buffer);
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        Call(list, 26, uintptr_t{1}, P(&b), uintptr_t{0});
        Call(list, 15, P(readback), uintptr_t{0}, P(buffer), uintptr_t{0}, static_cast<uintptr_t>(kBytes));
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        Call(list, 26, uintptr_t{1}, P(&b), uintptr_t{0});
    }
    uint64_t Submit(void* list) {
        SX_REQUIRE(Call(list, 9) == 0);
        void* lists[] = {list};
        Call(queue, 10, uintptr_t{1}, P(lists), uintptr_t{0});
        const uint64_t value = ++fence_value;
        SX_REQUIRE(Call(queue, 14, P(fence), static_cast<uintptr_t>(value), uintptr_t{0}) == 0);
        return value;
    }
    void Wait(uint64_t value) {
        if (static_cast<uint64_t>(Call(fence, 8)) < value) {
            SX_REQUIRE(Call(fence, 9, static_cast<uintptr_t>(value), P(event), uintptr_t{0}) == 0);
            SX_REQUIRE(WaitForSingleObject(event, 20000) == WAIT_OBJECT_0);
        }
    }
    void ExpectReadback(void* readback, const char* what, uint32_t c0, uint32_t k) {
        uint8_t* data = nullptr;
        SX_REQUIRE(Call(readback, 8, uintptr_t{0}, uintptr_t{0}, P(&data)) == 0 && data);
        uint32_t exact = 0, first = 0;
        for (uint32_t i = 0; i < kDwords; ++i) {
            uint32_t got;
            std::memcpy(&got, data + i * 4, 4);
            if (got == Kernel(i, c0, k)) ++exact;
            else if (!first) first = got;
        }
        Call(readback, 9, uintptr_t{0}, uintptr_t{0});
        const bool ok = exact == kDwords;
        std::printf("[remap] %-46s %u/%u dwords -> %s", what, exact, kDwords, ok ? "ok" : "WRONG");
        if (!ok) std::printf(" (first wrong dword 0x%08x, wanted 0x%08x)", first, Kernel(0, c0, k));
        std::printf("\n");
        std::fflush(stdout);
        if (!ok) ++failures;
    }
    // Copies `buffer` out in a list of its own, waits, and compares.
    void Expect(void* buffer, const char* what, uint32_t c0, uint32_t k) {
        void* readback = Readback();
        void* list = NewList(0);
        CopyOut(list, buffer, readback);
        Wait(Submit(list));
        ExpectReadback(readback, what, c0, k);
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
    if (D3D12XboxCreateDevice(nullptr, &params, &__uuidof(ID3D12Device), &r.device) < 0 || !r.device) {
        std::printf("[remap] D3D12XboxCreateDevice FAILED\n");
        return 3;
    }
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    SX_REQUIRE(Call(r.device, 8, P(&qdesc), uintptr_t{0}, P(&r.queue)) == 0 && r.queue);
    SX_REQUIRE(Call(r.device, 36, uintptr_t{0}, uintptr_t{0}, uintptr_t{0}, P(&r.fence)) == 0 && r.fence);
    r.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    SX_REQUIRE(Call(r.device, 14, P(&hd), uintptr_t{0}, P(&r.heap)) == 0 && r.heap);
    r.cpu0 = static_cast<uintptr_t>(Call(r.heap, 9));
    r.gpu0 = static_cast<uintptr_t>(Call(r.heap, 10));
    r.inc = static_cast<uint32_t>(Call(r.device, 15, static_cast<uintptr_t>(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)));

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 4;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &range;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = rp;
    ID3DBlob *blob = nullptr, *err = nullptr;
    SX_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)));
    SX_REQUIRE(Call(r.device, 16, uintptr_t{0}, P(blob->GetBufferPointer()), static_cast<uintptr_t>(blob->GetBufferSize()),
                    uintptr_t{0}, P(&r.root)) == 0 && r.root);
    const uint32_t literal[2] = {0x0BADF00Du, 0x13572468u};
    for (int i = 0; i < 2; ++i) {
        std::vector<uint8_t> kernel;
        if (!scarlettx::BuildVictoryVectorKernelContainer(literal[i], kernel).ok()) {
            std::printf("[remap] the native fixture could not be built\n");
            return 4;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
        cd.pRootSignature = static_cast<ID3D12RootSignature*>(r.root);
        cd.CS = {kernel.data(), kernel.size()};
        SX_REQUIRE(Call(r.device, 11, P(&cd), uintptr_t{0}, P(&r.pso[i])) == 0 && r.pso[i]);
    }
    D3D12XBOX_SHADER_TRANSLATION_STATS st{};
    SX_REQUIRE(D3D12XboxGetShaderTranslationStats(&st) == 0 && st.compute_translated == 2);

    void* a = r.Target();
    void* b = r.Target();
    void* c = r.Target();
    const uint32_t k0 = literal[0], k1 = literal[1];

    // 1. slot 0 views A.
    r.View(0, a);
    r.View(1, a);  // slot 1 keeps a view of A for part 5
    {
        void* list = r.NewList(0);
        r.Constants(list, 1);
        r.Table(list, 0);
        r.Dispatch(list);
        r.Wait(r.Submit(list));
    }
    r.Expect(a, "1 A after the first dispatch", 1, k0);

    // 2. slot 0 remapped to B.
    r.View(0, b);
    {
        void* list = r.NewList(0);
        r.Constants(list, 2);
        r.Table(list, 0);
        r.Dispatch(list);
        r.Wait(r.Submit(list));
    }
    r.Expect(b, "2 B after slot 0 was given its view", 2, k0);
    r.Expect(a, "2 A untouched by the remapped slot", 1, k0);

    // 3. B retired while a dispatch into it and a copy out of it are in flight.
    {
        void* readback = r.Readback();
        void* list = r.NewList(0);
        r.Constants(list, 3);
        r.Table(list, 0);
        r.Dispatch(list);
        r.CopyOut(list, b, readback);
        const uint64_t value = r.Submit(list);
        SX_REQUIRE(Call(b, 2) == 0);  // Release: the title's last reference, before the list has run
        r.Wait(value);
        r.ExpectReadback(readback, "3 B, released while in flight", 3, k0);
    }

    // 4. slot 0 remapped to C after B's retirement.
    r.View(0, c);
    {
        void* list = r.NewList(0);
        r.Constants(list, 4);
        r.Table(list, 0);
        r.Dispatch(list);
        r.Wait(r.Submit(list));
    }
    r.Expect(c, "4 C after slot 0 was given its view", 4, k0);
    r.Expect(a, "4 A untouched", 1, k0);

    // 5. one list, three dispatches, the table and the constants changed before each.
    {
        void* list = r.NewList(0);
        r.Table(list, 1);
        r.Constants(list, 5);
        r.Dispatch(list);
        r.Table(list, 0);
        r.Constants(list, 6);
        r.Dispatch(list);
        r.Table(list, 1);
        r.Constants(list, 7);
        r.Dispatch(list);
        r.Wait(r.Submit(list));
    }
    r.Expect(a, "5 A: slot 1, then slot 0, then slot 1 again", 7, k0);
    r.Expect(c, "5 C: the dispatch in between", 6, k0);

    // 6. one list: two dispatches with nothing changed in between, then the other program with a
    //    new table and constants, then the first program again with the table it had.
    {
        void* list = r.NewList(0);
        r.Table(list, 0);
        r.Constants(list, 8);
        r.Dispatch(list);
        r.Dispatch(list);                                    // C <- 8 twice, program 0
        Call(list, 25, P(r.pso[1]), uintptr_t{0}, uintptr_t{0});
        r.Table(list, 1);
        r.Constants(list, 9);
        r.Dispatch(list);                                    // A <- 9, program 1
        Call(list, 25, P(r.pso[0]), uintptr_t{0}, uintptr_t{0});
        r.Table(list, 0);
        r.Dispatch(list);                                    // C <- 9, program 0
        r.Wait(r.Submit(list));
    }
    r.Expect(a, "6 A: the other program, new table and constants", 9, k1);
    r.Expect(c, "6 C: first program again, same table", 9, k0);

    SX_REQUIRE(D3D12XboxGetShaderTranslationStats(&st) == 0);
    std::printf("[remap] translated dispatches %llu (want 11), skipped %llu (want 0)\n",
                static_cast<unsigned long long>(st.dispatches_translated), static_cast<unsigned long long>(st.dispatches_skipped));
    if (st.dispatches_translated != 11 || st.dispatches_skipped != 0) ++r.failures;
    if (r.failures) {
        std::printf("test_victory_compute_remap: FAIL\n");
        return 1;
    }
    std::printf("test_victory_compute_remap: OK\n");
    return 0;
}
