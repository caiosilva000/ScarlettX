// Real-title proxy path: what ID3D12CommandQueue::PresentX puts in the output window.
//
// Two distinct synthetic frames are uploaded through the guest-facing proxies and presented one
// after the other, the window is resized once, and a fresh frame is presented at the new size.
// Each stage prints a line with a wall-clock time, the window's client rectangle on screen and
// a GDI probe of the window, and then holds for SCARLETTX_PRESENT_HOLD_MS (default 300) so that
// something outside the process (a screen capture) can check what is on screen in that stage.
//
//   frame A: left half red   (230, 30, 30), right half green  ( 30, 200,  30)
//   frame B: left half blue  ( 30, 60, 230), right half yellow (240, 220,  30)
//
// The API results are asserted; the GDI probe is reported only, because a flip-model swap chain
// is not required to be readable through the window's device context.
#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "sx_check.h"

#include <windows.h>

#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

template <typename... Args>
intptr_t Call(void* object, int slot, Args... args) {
    void** vtable = *reinterpret_cast<void***>(object);
    using Fn = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, Args...);
    return reinterpret_cast<Fn>(vtable[slot])(object, args...);
}

constexpr int kRelease = 2;
constexpr int kDevCreateCommandQueue = 8;
constexpr int kDevCreateCommandAllocator = 9;
constexpr int kDevCreateCommandList = 12;
constexpr int kDevCreateCommittedResource = 27;
constexpr int kDevCreateFence = 36;
constexpr int kResMap = 8;
constexpr int kResUnmap = 9;
constexpr int kListClose = 9;
constexpr int kListReset = 10;
constexpr int kListCopyTextureRegion = 16;
constexpr int kListResourceBarrier = 26;
constexpr int kQueueExecuteCommandLists = 10;
constexpr int kQueueSignal = 14;
constexpr int kQueuePresentX = 30;
constexpr int kFenceGetCompletedValue = 8;

constexpr uint32_t kWidth = 256;
constexpr uint32_t kHeight = 144;
constexpr uint32_t kPitch = kWidth * 4;  // already a multiple of 256

struct Rgb {
    uint8_t r, g, b;
};
constexpr Rgb kFrameA[2] = {{230, 30, 30}, {30, 200, 30}};
constexpr Rgb kFrameB[2] = {{30, 60, 230}, {240, 220, 30}};

struct Context {
    void* device = nullptr;
    void* queue = nullptr;
    void* allocator = nullptr;
    void* list = nullptr;
    void* fence = nullptr;
    void* upload = nullptr;
    uint64_t fence_value = 0;
    bool list_open = true;  // a new command list starts open
};

void* CreateResource(void* device, D3D12_HEAP_TYPE heap, const D3D12_RESOURCE_DESC& desc,
                     D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    void* res = nullptr;
    const intptr_t hr = Call(device, kDevCreateCommittedResource, &hp, uintptr_t{0}, &desc,
                             static_cast<uintptr_t>(state), static_cast<void*>(nullptr),
                             static_cast<void*>(nullptr), &res);
    SX_REQUIRE(hr == 0 && res != nullptr);
    return res;
}

// Fills `texture` with the two-colour frame and waits for the copy on the fence.
void UploadFrame(Context& c, void* texture, const Rgb halves[2]) {
    uint8_t* up = nullptr;
    SX_REQUIRE(Call(c.upload, kResMap, uintptr_t{0}, static_cast<void*>(nullptr), &up) == 0 && up);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const Rgb& px = halves[x < kWidth / 2 ? 0 : 1];
            uint8_t* dst = up + static_cast<size_t>(y) * kPitch + x * 4;
            dst[0] = px.r;
            dst[1] = px.g;
            dst[2] = px.b;
            dst[3] = 255;
        }
    }
    Call(c.upload, kResUnmap, uintptr_t{0}, static_cast<void*>(nullptr));

    if (!c.list_open) SX_REQUIRE(Call(c.list, kListReset, c.allocator, static_cast<void*>(nullptr)) == 0);
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = static_cast<ID3D12Resource*>(texture);  // proxy pointer, as a guest passes it
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    Call(c.list, kListResourceBarrier, uintptr_t{1}, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = static_cast<ID3D12Resource*>(texture);
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = static_cast<ID3D12Resource*>(c.upload);
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = kWidth;
    src.PlacedFootprint.Footprint.Height = kHeight;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = kPitch;
    Call(c.list, kListCopyTextureRegion, &dst, uintptr_t{0}, uintptr_t{0}, uintptr_t{0}, &src,
         static_cast<void*>(nullptr));
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    Call(c.list, kListResourceBarrier, uintptr_t{1}, &barrier);
    SX_REQUIRE(Call(c.list, kListClose) == 0);
    c.list_open = false;

    void* lists[] = {c.list};
    Call(c.queue, kQueueExecuteCommandLists, uintptr_t{1}, static_cast<void**>(lists));
    const uint64_t value = ++c.fence_value;
    SX_REQUIRE(Call(c.queue, kQueueSignal, c.fence, static_cast<uintptr_t>(value)) == 0);
    for (int i = 0; i < 5000 && static_cast<uint64_t>(Call(c.fence, kFenceGetCompletedValue)) < value; ++i) {
        Sleep(1);
    }
    SX_REQUIRE(static_cast<uint64_t>(Call(c.fence, kFenceGetCompletedValue)) >= value);
}

HWND FindPresentWindow() {
    HWND hwnd = nullptr;
    while ((hwnd = FindWindowExA(nullptr, hwnd, "ScarlettXPresentWindowClass", nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId()) return hwnd;
    }
    return nullptr;
}

unsigned long long UnixMillis() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const unsigned long long ticks = (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (ticks - 116444736000000000ull) / 10000ull;
}

DWORD HoldMs() {
    const char* v = std::getenv("SCARLETTX_PRESENT_HOLD_MS");
    const long ms = (v && v[0]) ? std::strtol(v, nullptr, 10) : 300;
    return static_cast<DWORD>(ms < 0 ? 0 : (ms > 60000 ? 60000 : ms));
}

// One stage: present, report, hold. `stage` is the line a capture is matched against.
void PresentStage(Context& c, void* frame, const char* stage, const char* frame_name, HWND* hwnd_io) {
    uint64_t plane[8] = {};
    plane[1] = 1;
    plane[2] = reinterpret_cast<uint64_t>(frame);
    const intptr_t hr = Call(c.queue, kQueuePresentX, uintptr_t{1}, plane, static_cast<void*>(nullptr));
    SX_REQUIRE(hr == 0);

    if (!*hwnd_io) *hwnd_io = FindPresentWindow();
    const HWND hwnd = *hwnd_io;
    SX_REQUIRE(hwnd != nullptr && IsWindow(hwnd) && IsWindowVisible(hwnd));

    Sleep(150);  // let the compositor take the frame before probing
    RECT client = {};
    SX_REQUIRE(GetClientRect(hwnd, &client));
    POINT origin = {0, 0};
    ClientToScreen(hwnd, &origin);
    const int w = client.right - client.left;
    const int h = client.bottom - client.top;

    COLORREF left = CLR_INVALID;
    COLORREF right = CLR_INVALID;
    if (const HDC dc = GetDC(hwnd)) {
        left = GetPixel(dc, w / 4, h / 2);
        right = GetPixel(dc, (w * 3) / 4, h / 2);
        ReleaseDC(hwnd, dc);
    }
    std::printf("[present] stage=%s frame=%s presentx_hr=0x%08x t_ms=%llu client=%dx%d screen_origin=%ld,%ld "
                "gdi_left=%06lx gdi_right=%06lx (gdi is 0x00bbggrr, ffffffff = unreadable)\n",
                stage, frame_name, static_cast<unsigned>(hr), UnixMillis(), w, h, origin.x, origin.y,
                static_cast<unsigned long>(left), static_cast<unsigned long>(right));
    std::fflush(stdout);
    Sleep(HoldMs());
    std::printf("[present] stage=%s end t_ms=%llu\n", stage, UnixMillis());
    std::fflush(stdout);
}

DWORD WINAPI Watchdog(void* done) {
    if (WaitForSingleObject(static_cast<HANDLE>(done), 240000) == WAIT_TIMEOUT) {
        std::fprintf(stderr, "test_d3d12_present_frames: timed out\n");
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 1);
    }
    return 0;
}

}  // namespace

int main() {
    // Select the guest-facing proxies (also set by CTest for this test).
    _putenv_s("SCARLETTX_REAL_TITLE", "1");

    const HANDLE done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    SX_REQUIRE(done != nullptr);
    const HANDLE watchdog = CreateThread(nullptr, 0, &Watchdog, done, 0, nullptr);
    SX_REQUIRE(watchdog != nullptr);

    Context c;
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    params.PreferWarp = 1;
    const IID device_iid = __uuidof(ID3D12Device);
    SX_REQUIRE(D3D12XboxCreateDevice(nullptr, &params, &device_iid, &c.device) >= 0);
    SX_REQUIRE(c.device != nullptr);

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    SX_REQUIRE(Call(c.device, kDevCreateCommandQueue, &qdesc, static_cast<void*>(nullptr), &c.queue) == 0);
    SX_REQUIRE(Call(c.device, kDevCreateCommandAllocator, uintptr_t{D3D12_COMMAND_LIST_TYPE_DIRECT},
                    static_cast<void*>(nullptr), &c.allocator) == 0);
    SX_REQUIRE(Call(c.device, kDevCreateCommandList, uintptr_t{0}, uintptr_t{D3D12_COMMAND_LIST_TYPE_DIRECT},
                    c.allocator, static_cast<void*>(nullptr), static_cast<void*>(nullptr), &c.list) == 0);
    SX_REQUIRE(Call(c.device, kDevCreateFence, uintptr_t{0}, uintptr_t{0}, static_cast<void*>(nullptr),
                    &c.fence) == 0);
    SX_REQUIRE(c.queue && c.allocator && c.list && c.fence);

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kWidth;
    td.Height = kHeight;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    void* frame_a = CreateResource(c.device, D3D12_HEAP_TYPE_DEFAULT, td, D3D12_RESOURCE_STATE_COMMON);
    void* frame_b = CreateResource(c.device, D3D12_HEAP_TYPE_DEFAULT, td, D3D12_RESOURCE_STATE_COMMON);

    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = static_cast<uint64_t>(kPitch) * kHeight;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    c.upload = CreateResource(c.device, D3D12_HEAP_TYPE_UPLOAD, bd, D3D12_RESOURCE_STATE_GENERIC_READ);

    UploadFrame(c, frame_a, kFrameA);
    UploadFrame(c, frame_b, kFrameB);
    std::printf("[present] frames uploaded: A=red|green B=blue|yellow %ux%u hold_ms=%lu\n", kWidth, kHeight,
                static_cast<unsigned long>(HoldMs()));
    std::fflush(stdout);

    HWND hwnd = nullptr;
    PresentStage(c, frame_a, "1-frame-A", "A", &hwnd);
    PresentStage(c, frame_b, "2-frame-B", "B", &hwnd);

    // Resize once: half the client size, from the thread that presents. The swap chain keeps the
    // frame's size and stretches, so the next frame must fill the new client area.
    RECT before = {};
    SX_REQUIRE(GetClientRect(hwnd, &before));
    RECT wanted = {0, 0, (before.right - before.left) / 2, (before.bottom - before.top) / 2};
    AdjustWindowRect(&wanted, static_cast<DWORD>(GetWindowLongPtrA(hwnd, GWL_STYLE)), FALSE);
    SX_REQUIRE(SetWindowPos(hwnd, nullptr, 0, 0, wanted.right - wanted.left, wanted.bottom - wanted.top,
                            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
    RECT after = {};
    SX_REQUIRE(GetClientRect(hwnd, &after));
    std::printf("[present] resized client %ldx%ld -> %ldx%ld\n", before.right - before.left,
                before.bottom - before.top, after.right - after.left, after.bottom - after.top);
    SX_REQUIRE(after.right - after.left == (before.right - before.left) / 2);
    SX_REQUIRE(after.bottom - after.top == (before.bottom - before.top) / 2);

    PresentStage(c, frame_a, "3-frame-A-resized", "A", &hwnd);
    PresentStage(c, frame_b, "4-frame-B-resized", "B", &hwnd);

    // Clean close: the window goes away with the queue.
    SX_REQUIRE(Call(c.queue, kRelease) == 0);
    SX_REQUIRE(!IsWindow(hwnd));
    std::printf("[present] window closed with the queue\n");

    SetEvent(done);
    WaitForSingleObject(watchdog, INFINITE);
    CloseHandle(watchdog);
    std::printf("test_d3d12_present_frames: OK (4 presents, 1 resize; on-screen content is checked by the capture)\n");
    return 0;
}
