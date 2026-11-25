// Opt-in synthetic frame-rate benchmark for the guest-facing d3d12_xs route (docs/fps-benchmark.md).
//
// One process runs one stress mode in one variant on one path and writes its numbers to two CSV
// files. tools/sx_bench_fps.py launches it many times, alternating baseline and candidate.
//
// Everything that is timed goes through the objects a title gets: the shim's device, command
// allocator, command list, queue, fence, resources and descriptor heaps, called by vtable slot.
// The scene and the frame loop start from tests/synthetic_games/scarlett_arena: a floor, beacons
// that move on a scripted path and pulse on every fourth frame, and per frame
// "wait for the frame, record, submit, present".
//
//   --mode draws    small draws and descriptor changes: --draws 4x4-pixel indexed quads per frame,
//                   a cheap textured shader.
//                     --variant fixed      one descriptor table bound per frame
//                     --variant pertable   SetGraphicsRootDescriptorTable before every draw,
//                                          cycling over --textures textures with equal contents,
//                                          so the picture is the same as `fixed`
//   --mode tdraws   the draw path with a translated pipeline: --draws draws per frame, each a
//                   scissored full-target triangle (vertex id only, colour in four root
//                   constants), 4x4 pixels. The shaders are the synthetic native vertex and pixel
//                   programs of tests/test_victory_draw_guest.cpp, translated inside the shim's
//                   CreateGraphicsPipelineState, or the same two programs in HLSL.
//                     --variant translated-same      root constants set once per frame
//                     --variant translated-changing  root constants set before every draw
//                     --variant desktop-same, desktop-changing   the HLSL pipeline, same draws
//                   Reports the host root-signature binds and argument replays the shim made.
//   --mode tdispatch  directly indexed descriptors: --draws dispatches per frame of a translated
//                   native compute program that writes 64 dwords through the raw UAV its
//                   descriptor table names; the translated shader takes the view from the
//                   descriptor heap by index, which the shim works out from the table at every
//                   dispatch. --descriptors UAVs sit in the heap.
//                     --variant same                table and constants set once per frame
//                     --variant changing-table      a different descriptor before every dispatch
//                     --variant changing-constants  the root constants before every dispatch
//                   Every buffer is read back after the last frame and compared with the CPU.
//   --mode ttex     a translated pixel program that samples a texture through a guest descriptor
//                   table (the fixture of tests/test_victory_draw_table.cpp): --draws scissored
//                   4x4 draws per frame. The shim checks the view a table names whenever the
//                   arguments or any descriptor changed.
//                     --variant table-same        one table per frame: the check runs once
//                     --variant table-changing    the table alternates between two views per draw
//                     --variant descriptor-write  an unrelated descriptor is written before every
//                                                 draw, which makes the shim check again
//   --mode pixel    pixel shading: a fixed scene (floor + 24 beacons) at --width x --height.
//                     --variant light      one integer hash per pixel
//                     --variant heavy      the same hash followed by --rounds xorshift rounds
//   --mode upload   texture streaming: --upload-count textures of --upload-size^2 RGBA8 texels
//                   rewritten every frame (fixed bytes per frame), then drawn 1:1.
//                     --variant percopy        baseline: a new staging buffer per texture and one
//                                              ExecuteCommandLists per copy
//                     --variant batched        one change: all copies in the frame's command list
//                     --variant reuse          one change: staging buffers kept per in-flight slot
//                                              and reused once that slot's fence has completed
//                     --variant batched-reuse  both
//                     --variant guestmem       the console route: texels written through the
//                                              address GetGPUVirtualAddress returns; the shim
//                                              notices the writes and uploads at submission
//
//   --path offscreen   uncapped throughput with --inflight frames in flight: the loop waits only
//                      to recycle a slot whose fence has not completed, and to drain at the end.
//                      The headline is completed frames per second.
//   --path serialized  one frame at a time, each waited for: submission-to-fence per frame.
//                      Labelled separately; it is not a frame rate.
//   --path windowed    as offscreen, plus WaitFrameEventX (when --interval-us is not 0) and
//                      PresentX per frame. Frame-number markers are drawn along the bottom edge so
//                      a screen capture can tell which frame is on screen.
//
//   --shader-source desktop      HLSL through D3DCompile (the only source available today)
//   --shader-source translated   native RDNA2 graphics through the shim: not available until the
//                                shim's CreateGraphicsPipelineState translates; exits 5
//
// Before timing: every pipeline is created and drawn with (warm-up frames run the same code).
// Outside the timed loop: the render target of every in-flight slot is read back after the drain
// and compared with a picture computed on the CPU for the frame that slot last held, and
// --check-frames further frames are run with a copy to a read-back buffer and compared the same
// way. All times are wall-clock (QueryPerformanceCounter); the host returns no GPU timestamps.
//
// Exit codes: 0 pass, 1 a read-back was not exact or a fence did not complete, 2 a call failed,
// 3 no device, 5 the requested shader source is not available, 64 usage.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "scarlettx/gpu/victory_shader_bridge.h"
#include "scarlettx/gpu/xg_layout.h"

namespace xg = scarlettx::gpu::xg;

namespace {

template <typename... Args>
intptr_t Call(void* object, int slot, Args... args) {
    using Fn = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, Args...);
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[slot])(object, args...);
}
uintptr_t P(const void* p) { return reinterpret_cast<uintptr_t>(p); }

// Vtable slots of the shim's objects (src/shims/d3d12_xs_shim.cpp).
constexpr int kRelease = 2;
constexpr int kDevCreateCommandQueue = 8;
constexpr int kDevCreateCommandAllocator = 9;
constexpr int kDevCreateGraphicsPipelineState = 10;
constexpr int kDevCreateCommandList = 12;
constexpr int kDevCreateDescriptorHeap = 14;
constexpr int kDevGetDescriptorHandleIncrementSize = 15;
constexpr int kDevCreateRootSignature = 16;
constexpr int kDevCreateComputePipelineState = 11;
constexpr int kDevCreateShaderResourceView = 18;
constexpr int kDevCreateUnorderedAccessView = 19;
constexpr int kDevCreateRenderTargetView = 20;
constexpr int kDevCreateCommittedResource = 27;
constexpr int kDevCreateFence = 36;
constexpr int kDevGetDeviceRemovedReason = 37;
constexpr int kDevSetFrameIntervalX = 78;
constexpr int kDevWaitFrameEventX = 80;
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
constexpr int kListDrawIndexedInstanced = 13;
constexpr int kListDispatch = 14;
constexpr int kListCopyBufferRegion = 15;
constexpr int kListCopyTextureRegion = 16;
constexpr int kListIASetPrimitiveTopology = 20;
constexpr int kListRSSetViewports = 21;
constexpr int kListRSSetScissorRects = 22;
constexpr int kListSetPipelineState = 25;
constexpr int kListResourceBarrier = 26;
constexpr int kListSetDescriptorHeaps = 32;
constexpr int kListSetComputeRootSignature = 33;
constexpr int kListSetGraphicsRootSignature = 34;
constexpr int kListSetComputeRootDescriptorTable = 35;
constexpr int kListSetComputeRoot32BitConstants = 39;
constexpr int kListSetGraphicsRootDescriptorTable = 36;
constexpr int kListSetGraphicsRoot32BitConstants = 40;
constexpr int kListIASetIndexBuffer = 47;
constexpr int kListIASetVertexBuffers = 48;
constexpr int kListOMSetRenderTargets = 50;
constexpr int kListClearRenderTargetView = 52;
constexpr int kQueueExecuteCommandLists = 10;
constexpr int kQueueSignal = 14;
constexpr int kQueuePresentX = 30;

int64_t g_qpc_freq = 1;
int64_t Ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
double Us(int64_t from, int64_t to) { return static_cast<double>(to - from) * 1e6 / static_cast<double>(g_qpc_freq); }
unsigned long long UnixMicros() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const unsigned long long ticks = (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (ticks - 116444736000000000ull) / 10ull;
}

double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return -1.0;
    std::sort(v.begin(), v.end());
    const double rank = p * static_cast<double>(v.size() - 1);
    const size_t lo = static_cast<size_t>(rank);
    const size_t hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (rank - static_cast<double>(lo));
}

int Fail(const char* api, intptr_t hr) {
    std::printf("[fps] FAILED api=%s hr=0x%08x\n", api, static_cast<uint32_t>(hr));
    std::fflush(stdout);
    return 2;
}

// ---------------------------------------------------------------------------------------------
// The scene, as numbers. Both the recorder and the CPU reference read these.
// ---------------------------------------------------------------------------------------------

constexpr uint32_t kClearDword = 0xFF302010u;  // R 16, G 32, B 48, A 255
constexpr uint32_t kCell = 4;                  // draws mode: a draw covers kCell x kCell pixels
constexpr uint32_t kTexSize = 16;              // draws mode: texture edge
constexpr uint32_t kBeacons = 24;              // pixel mode: quads on top of the floor
constexpr uint32_t kPayloads = 8;              // upload mode: texel payloads in rotation
constexpr uint32_t kStamps = 16;               // upload mode: texels per texture unique to a frame
constexpr uint32_t kMarkerBits = 16;
constexpr uint32_t kDispatchDwords = 64;       // tdispatch: dwords a dispatch writes (two groups of 32)
constexpr uint32_t kDispatchLiteral = 0x5A17C0DEu;  // tdispatch: the kernel's literal k

uint32_t Hash3(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = (a * 0x9E3779B1u) ^ (b * 0x85EBCA77u) ^ (c * 0xC2B2AE3Du);
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

struct Rect {
    uint32_t x0, y0, x1, y1;
};

// Pixel mode: the arena floor, then the beacons. Each beacon walks a path fixed by the seed and,
// as in scarlett_arena's input script, is larger ("pulsed") on every fourth frame.
void PixelScene(uint32_t w, uint32_t h, uint32_t seed, uint32_t frame, std::vector<Rect>& out) {
    out.clear();
    out.push_back({0, 0, w, h});
    const bool pulsed = (frame % 4) == 2;
    const uint32_t bw = pulsed ? w / 6 : w / 8;
    const uint32_t bh = pulsed ? h / 6 : h / 8;
    for (uint32_t i = 1; i <= kBeacons; ++i) {
        const uint32_t base_x = Hash3(seed, i, 1), base_y = Hash3(seed, i, 2);
        const uint32_t step_x = 1 + Hash3(seed, i, 3) % 9, step_y = 1 + Hash3(seed, i, 4) % 5;
        const uint32_t x0 = (base_x + frame * step_x) % (w - bw + 1);
        const uint32_t y0 = (base_y + frame * step_y) % (h - bh + 1);
        out.push_back({x0, y0, x0 + bw, y0 + bh});
    }
}

// Draws mode: draw j of a frame covers this cell (the pattern shifts by 37 cells per frame).
uint32_t DrawCell(uint32_t j, uint32_t frame, uint32_t cells) { return (j + (frame % cells) * 37u) % cells; }

// tdraws mode: the colour in the four root constants, as bytes R, G, B, A in a dword.
// `pair` (the switch probe) gives two consecutive draws one colour.
uint32_t TDrawColor(bool changing, bool pair, uint32_t j, uint32_t frame) {
    if (!changing) return 0xFF400000u | (frame & 0xFFFFu);
    const uint32_t k = pair ? (j & ~1u) : j;
    return 0xFF000000u | ((frame & 0xFFu) << 16) | (k & 0xFFFFu);
}

// ttex mode: the two 8x8 textures of the fixture, and the colour a draw adds to what it samples.
uint32_t TTexTexel(uint32_t which, uint32_t i, uint32_t j) {
    const uint32_t r = which == 0 ? 8 * i + 3 : 200 - 8 * i;
    const uint32_t g = which == 0 ? 8 * j + 5 : 190 - 8 * j;
    const uint32_t b = which == 0 ? 0x40u : 0x80u;
    return r | (g << 8) | (b << 16) | (200u << 24);
}
void TTexAdd(uint32_t frame, uint32_t add[4]) {
    add[0] = frame & 31u;
    add[1] = (frame >> 5) & 31u;
    add[2] = 0;
    add[3] = 0;
}

// The frame-number markers of the windowed path: squares of w/32 along the bottom edge.
// 16 bits of the frame number (white = 1), a block that alternates magenta and cyan, then a red
// and a green block that never change (for whoever decodes a capture).
uint32_t MarkerCount() { return kMarkerBits + 3; }
Rect MarkerRect(uint32_t w, uint32_t h, uint32_t k) {
    const uint32_t bs = w / 32;
    return {k * bs, h - bs, (k + 1) * bs, h};
}
uint32_t MarkerColor(uint32_t frame, uint32_t k) {
    if (k < kMarkerBits) return ((frame >> k) & 1u) ? 0xFFFFFFFFu : 0xFF000000u;
    if (k == kMarkerBits) return (frame & 1u) ? 0xFFFF00FFu : 0xFFFFFF00u;
    return k == kMarkerBits + 1 ? 0xFF0000FFu : 0xFF00FF00u;
}

// tdispatch: the kernel's arithmetic for the CPU, and the constants a dispatch runs with.
uint32_t DispatchKernel(uint32_t i, uint32_t c0, uint32_t c1) {
    uint32_t x = i + c0;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return (x + c1) ^ kDispatchLiteral;
}
uint32_t DispatchC0(bool per_dispatch, uint32_t j, uint32_t frame) { return 0x1000u + frame * 7919u + (per_dispatch ? j * 31u : 0u); }

// The pixel shaders' arithmetic, written again for the CPU.
uint32_t MixPixel(uint32_t x, uint32_t y, uint32_t frame, uint32_t draw_id, uint32_t seed) {
    return (x * 0x9E3779B1u) ^ (y * 0x85EBCA77u) ^ (frame * 0xC2B2AE3Du) ^ (draw_id * 0x27D4EB2Fu) ^ seed;
}
uint32_t HeavyRounds(uint32_t v, uint32_t rounds) {
    for (uint32_t i = 0; i < rounds; ++i) {
        v ^= v << 13;
        v ^= v >> 17;
        v ^= v << 5;
        v += 0x9E3779B9u;
    }
    return v;
}

// tdraws mode, desktop variants: the same two programs as the translated fixtures.
const char* kTDrawHlsl = R"(
cbuffer Draw : register(b0) { float4 colour; };
float4 VSMain(uint vid : SV_VertexID) : SV_POSITION {
    return float4(float((vid << 1) & 2) * 2.0f - 1.0f, float(vid & 2) * 2.0f - 1.0f, 0.0f, 1.0f);
}
float4 PSMain() : SV_TARGET { return colour; }
)";

const char* kHlslTemplate = R"(
cbuffer Draw : register(b0) {
    uint4 rect;      // x0, y0, x1, y1 in pixels
    uint2 target;    // render target size
    uint draw_id;
    uint frame;
    uint seed;
    uint2 origin;    // upload mode: where the texture's texel (0,0) lands
    uint color;      // solid colour, R in the low byte
};
Texture2D<float4> tex : register(t0);

float4 VSMain(float2 corner : POSITION) : SV_POSITION {
    float2 lo = float2(float(rect.x), float(rect.y));
    float2 hi = float2(float(rect.z), float(rect.w));
    float2 p = lo + (hi - lo) * corner;
    return float4(p.x * 2.0f / float(target.x) - 1.0f, 1.0f - p.y * 2.0f / float(target.y), 0.0f, 1.0f);
}
float4 Unpack(uint v) {
    return float4(float(v & 255u), float((v >> 8) & 255u), float((v >> 16) & 255u), float(v >> 24)) / 255.0f;
}
uint Mix(uint px, uint py) {
    return (px * 0x9E3779B1u) ^ (py * 0x85EBCA77u) ^ (frame * 0xC2B2AE3Du) ^ (draw_id * 0x27D4EB2Fu) ^ seed;
}
float4 PSTex(float4 pos : SV_POSITION) : SV_TARGET {
    uint px = uint(pos.x);
    uint py = uint(pos.y);
    float4 t = tex.Load(int3(int(px & 15u), int(py & 15u), 0));
    return float4(t.r, t.g, float(draw_id & 255u) / 255.0f, float((draw_id >> 8) & 255u) / 255.0f);
}
float4 PSCopy(float4 pos : SV_POSITION) : SV_TARGET {
    uint px = uint(pos.x) - origin.x;
    uint py = uint(pos.y) - origin.y;
    return tex.Load(int3(int(px), int(py), 0));
}
float4 PSLight(float4 pos : SV_POSITION) : SV_TARGET {
    return Unpack(Mix(uint(pos.x), uint(pos.y)));
}
float4 PSHeavy(float4 pos : SV_POSITION) : SV_TARGET {
    uint v = Mix(uint(pos.x), uint(pos.y));
    for (uint i = 0u; i < %uu; ++i) {
        v ^= v << 13;
        v ^= v >> 17;
        v ^= v << 5;
        v += 0x9E3779B9u;
    }
    return Unpack(v);
}
float4 PSSolid(float4 pos : SV_POSITION) : SV_TARGET {
    return Unpack(color);
}
)";

struct DrawConstants {
    uint32_t rect[4];
    uint32_t target[2];
    uint32_t draw_id;
    uint32_t frame;
    uint32_t seed;
    uint32_t origin[2];
    uint32_t color;
};
static_assert(sizeof(DrawConstants) == 48, "twelve root constants");

// ---------------------------------------------------------------------------------------------

struct Args {
    std::string mode = "draws";
    std::string variant;
    std::string path = "offscreen";
    std::string shader_source = "desktop";
    std::string series = "default";
    std::string csv;
    std::string frames_csv;
    uint32_t launch = 0;
    uint32_t frames = 600;
    uint32_t warmup = 30;
    uint32_t inflight = 3;
    uint32_t check_frames = 2;
    uint32_t seed = 0x5CA41E77u;
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t draws = 4000;
    uint32_t textures = 16;
    uint32_t descriptors = 64;
    uint32_t rounds = 1024;
    uint32_t upload_count = 16;
    uint32_t upload_size = 256;
    uint32_t interval_us = 16667;
    uint32_t hold_ms = 0;
    uint32_t idle_guest_textures = 0;
    bool markers = false;
};

struct FrameSample {
    double t_start_us = 0;     // from the start of the timed loop
    double wait_us = 0;        // waiting for this slot's previous frame (recycling)
    double pace_us = 0;        // WaitFrameEventX (windowed, when an interval is set)
    double prep_us = 0;        // CPU preparation: resets, generated texels, recording, Close
    double submit_us = 0;      // ExecuteCommandLists and Signal (all of this frame's submissions)
    double present_us = 0;     // PresentX call
    double serialized_us = 0;  // serialized path: first ExecuteCommandLists to fence completion
    unsigned long long present_unix_us = 0;
    int64_t present_tick = 0;  // when PresentX was called
    uint64_t completed_after = 0;  // fence value completed when the frame's calls returned
    uint32_t draws = 0;
    uint32_t constant_sets = 0;
    uint32_t table_binds = 0;
    uint32_t submissions = 0;
    uint32_t uploads = 0;
    uint64_t upload_bytes = 0;
};

struct Slot {
    void* alloc = nullptr;
    void* list = nullptr;
    void* copy_alloc = nullptr;  // upload mode, per-copy submissions
    void* copy_list = nullptr;
    void* rt = nullptr;
    uintptr_t rtv = 0;
    void* readback = nullptr;
    uint64_t fence_value = 0;
    uint32_t last_frame = 0;
    bool used = false;
    std::vector<void*> staging;         // reuse variants: one per streamed texture
    std::vector<uint8_t*> staging_map;
    std::vector<void*> retired;         // fresh staging buffers, released when the slot is recycled
};

struct Bench {
    Args a;
    void* dev = nullptr;
    ID3D12Device* native_dev = nullptr;  // for the adapter record only
    void* queue = nullptr;
    void* fence = nullptr;
    HANDLE event = nullptr;
    uint64_t fence_value = 0;
    uint64_t fence_waits = 0;
    void* srv_heap = nullptr;
    void* rtv_heap = nullptr;
    uintptr_t srv_cpu0 = 0, srv_gpu0 = 0, rtv_cpu0 = 0;
    uint32_t srv_inc = 0, rtv_inc = 0;
    void* root = nullptr;
    void* pso_main = nullptr;   // the mode's pipeline
    void* pso_solid = nullptr;  // markers
    void* troot = nullptr;           // tdraws: the title's root signature (four root constants)
    void* pso_t_desktop = nullptr;   // tdraws: HLSL pipeline
    void* pso_t_translated = nullptr;  // tdraws: translated native pipeline (null without the bridge)
    void* xroot = nullptr;             // ttex: four root constants and a table of one shader resource view
    void* pso_ttex = nullptr;          // ttex: translated vertex and textured pixel programs
    void* ttex_texture[2] = {};        // ttex: textures A and B; views in descriptors 1 and 2, 3 is spare
    void* croot = nullptr;             // tdispatch: the title's compute root (four constants, a table of one UAV)
    void* pso_compute = nullptr;       // tdispatch: translated native compute pipeline
    std::vector<void*> ubuf;           // tdispatch: one buffer per descriptor
    std::vector<uint32_t> ubuf_c0;     // the c0 of the last dispatch recorded into each
    std::vector<uint8_t> ubuf_written;
    uint32_t c1 = 0;
    void* vb = nullptr;
    void* ib = nullptr;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW ibv{};
    std::vector<Slot> slots;
    void* plumb_alloc = nullptr;  // set-up uploads and read-backs, never timed
    void* plumb_list = nullptr;
    uint32_t rt_w = 0, rt_h = 0, rt_pitch = 0;

    // draws mode
    std::vector<void*> tex_equal;   // SRV slots [0, textures)
    std::vector<void*> tex_probe;   // SRV slots [textures, 2*textures): distinct contents
    // upload mode
    std::vector<void*> tex_stream;  // SRV slots [0, upload_count)
    std::vector<std::vector<uint32_t>> payload;        // kPayloads x size^2 texels
    std::vector<std::vector<uint8_t>> payload_cooked;  // the same in console layout (guestmem)
    std::vector<uint8_t*> guest_base;
    std::unique_ptr<XG_RESOURCE_LAYOUT> guest_layout;
    uint64_t guest_bytes = 0;
    uint64_t stamp_offset[kStamps] = {};
    uint32_t staging_pitch = 0;
    uint64_t staging_bytes = 0;
    uint32_t grid_cols = 0;

    bool Upload() const { return a.mode == "upload"; }
    bool TDraws() const { return a.mode == "tdraws"; }
    bool TDispatch() const { return a.mode == "tdispatch"; }
    bool TTex() const { return a.mode == "ttex"; }
    uint32_t MarkerSrvSlot() const { return TDispatch() ? a.descriptors : 0; }
    bool TChanging() const { return a.variant.find("changing") != std::string::npos; }
    bool PerCopy() const { return a.variant == "percopy" || a.variant == "reuse"; }
    bool Reuse() const { return a.variant == "reuse" || a.variant == "batched-reuse"; }
    bool GuestMem() const { return a.variant == "guestmem"; }

    uintptr_t SrvCpu(uint32_t i) const { return srv_cpu0 + static_cast<uintptr_t>(i) * srv_inc; }
    uintptr_t SrvGpu(uint32_t i) const { return srv_gpu0 + static_cast<uintptr_t>(i) * srv_inc; }
    uint64_t Completed() const { return static_cast<uint64_t>(Call(fence, kFenceGetCompletedValue)); }

    void* CreateResource(D3D12_HEAP_TYPE heap, const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state,
                         const D3D12_CLEAR_VALUE* clear = nullptr) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heap;
        void* res = nullptr;
        const intptr_t hr = Call(dev, kDevCreateCommittedResource, P(&hp), uintptr_t{0}, P(&desc),
                                 static_cast<uintptr_t>(state), P(clear), uintptr_t{0}, P(&res));
        if (hr != 0 || !res) {
            Fail("shim CreateCommittedResource (slot 27)", hr);
            std::exit(2);
        }
        return res;
    }
    void* CreateBuffer(D3D12_HEAP_TYPE heap, uint64_t bytes) {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return CreateResource(heap, bd,
                              heap == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                             : D3D12_RESOURCE_STATE_COPY_DEST);
    }
    void* CreateTexture(uint32_t w, uint32_t h, bool render_target) {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w;
        td.Height = h;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        if (!render_target) return CreateResource(D3D12_HEAP_TYPE_DEFAULT, td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        clear.Color[0] = 16.0f / 255.0f;
        clear.Color[1] = 32.0f / 255.0f;
        clear.Color[2] = 48.0f / 255.0f;
        clear.Color[3] = 1.0f;
        return CreateResource(D3D12_HEAP_TYPE_DEFAULT, td, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear);
    }
    uint8_t* Map(void* res) {
        uint8_t* data = nullptr;
        const intptr_t hr = Call(res, kResMap, uintptr_t{0}, uintptr_t{0}, P(&data));
        if (hr != 0 || !data) {
            Fail("shim ID3D12Resource::Map (slot 8)", hr);
            std::exit(2);
        }
        return data;
    }
    void Unmap(void* res) { Call(res, kResUnmap, uintptr_t{0}, uintptr_t{0}); }

    void Barrier(void* list, void* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = static_cast<ID3D12Resource*>(res);  // the shim's object, as a title passes it
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        Call(list, kListResourceBarrier, uintptr_t{1}, P(&b), uintptr_t{0});
    }
    // Staging buffer -> texture, with the texture taken out of and put back into the shader state.
    void RecordTextureCopy(void* list, void* texture, void* staging, uint32_t size, uint32_t pitch) {
        Barrier(list, texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = static_cast<ID3D12Resource*>(texture);
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = static_cast<ID3D12Resource*>(staging);
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = size;
        src.PlacedFootprint.Footprint.Height = size;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = pitch;
        Call(list, kListCopyTextureRegion, P(&dst), uintptr_t{0}, uintptr_t{0}, uintptr_t{0}, P(&src), uintptr_t{0});
        Barrier(list, texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    void Execute(void* list) {
        void* lists[] = {list};
        Call(queue, kQueueExecuteCommandLists, uintptr_t{1}, P(lists), uintptr_t{0});
    }
    // Signals the next fence value on the queue. 0 on failure.
    uint64_t Signal() {
        const uint64_t value = ++fence_value;
        const intptr_t hr = Call(queue, kQueueSignal, P(fence), static_cast<uintptr_t>(value), uintptr_t{0});
        if (hr != 0) {
            Fail("shim ID3D12CommandQueue::Signal (slot 14)", hr);
            std::exit(2);
        }
        return value;
    }
    void WaitFor(uint64_t value) {
        if (Completed() >= value) return;
        ++fence_waits;
        const intptr_t hr = Call(fence, kFenceSetEventOnCompletion, static_cast<uintptr_t>(value), P(event), uintptr_t{0});
        if (hr != 0) {
            Fail("shim ID3D12Fence::SetEventOnCompletion (slot 9)", hr);
            std::exit(2);
        }
        if (WaitForSingleObject(event, 30000) != WAIT_OBJECT_0 || Completed() < value) {
            std::printf("[fps] FAILED fence %llu not reached (completed %llu) removed_reason=0x%08x\n",
                        static_cast<unsigned long long>(value), static_cast<unsigned long long>(Completed()),
                        static_cast<uint32_t>(Call(dev, kDevGetDeviceRemovedReason)));
            std::fflush(stdout);
            std::exit(1);
        }
    }
    void PlumbBegin() {
        Call(plumb_alloc, kAllocReset);
        Call(plumb_list, kListReset, P(plumb_alloc), uintptr_t{0}, uintptr_t{0});
    }
    void PlumbRun() {
        Call(plumb_list, kListClose);
        Execute(plumb_list);
        WaitFor(Signal());
    }

    // ---- upload mode: texel data -------------------------------------------------------------
    uint32_t PayloadIndex(uint32_t frame, uint32_t t) const { return (frame * 5u + t * 3u) % kPayloads; }
    static uint32_t StampValue(uint32_t frame, uint32_t t, uint32_t k) { return Hash3(frame + 0x51u, t, k); }
    void StampPos(uint32_t k, uint32_t* x, uint32_t* y) const {
        *x = k * (a.upload_size / kStamps) + 1;
        *y = k * (a.upload_size / kStamps) + 2;
    }
    // Rows for a staging buffer: the payload, then the texels that belong to this frame alone.
    void FillStaging(uint8_t* dst, uint32_t frame, uint32_t t) const {
        const uint32_t s = a.upload_size;
        const uint32_t* src = payload[PayloadIndex(frame, t)].data();
        if (staging_pitch == s * 4) {
            std::memcpy(dst, src, static_cast<size_t>(s) * s * 4);
        } else {
            for (uint32_t y = 0; y < s; ++y) std::memcpy(dst + static_cast<size_t>(y) * staging_pitch, src + y * s, s * 4);
        }
        for (uint32_t k = 0; k < kStamps; ++k) {
            uint32_t x, y;
            StampPos(k, &x, &y);
            const uint32_t v = StampValue(frame, t, k);
            std::memcpy(dst + static_cast<size_t>(y) * staging_pitch + x * 4, &v, 4);
        }
    }
    // The same texels written into the texture's own (console-layout) memory.
    void FillGuest(uint32_t frame, uint32_t t) const {
        std::memcpy(guest_base[t], payload_cooked[PayloadIndex(frame, t)].data(), guest_bytes);
        for (uint32_t k = 0; k < kStamps; ++k) {
            const uint32_t v = StampValue(frame, t, k);
            std::memcpy(guest_base[t] + stamp_offset[k], &v, 4);
        }
    }

    // ---- recording one frame through the shim's command list ---------------------------------
    void SetConstants(void* list, const DrawConstants& c) {
        Call(list, kListSetGraphicsRoot32BitConstants, uintptr_t{0}, uintptr_t{12}, P(&c), uintptr_t{0});
    }
    void DrawQuad(void* list) {
        Call(list, kListDrawIndexedInstanced, uintptr_t{6}, uintptr_t{1}, uintptr_t{0}, uintptr_t{0}, uintptr_t{0});
    }

    // Records and submits frame `frame` into `slot`. `probe` (draws mode) binds the textures with
    // distinct contents, one per draw. `readback` adds a copy of the finished target.
    // Submission time is returned through `s`; the caller times the whole call for preparation.
    void Frame(Slot& slot, uint32_t frame, bool probe, void* readback, FrameSample& s) {
        double submit_us = 0;
        int64_t first_submit = 0;
        Call(slot.alloc, kAllocReset);
        Call(slot.list, kListReset, P(slot.alloc), uintptr_t{0}, uintptr_t{0});
        void* list = slot.list;

        if (Upload()) {
            const uint32_t n = a.upload_count;
            if (!slot.retired.empty()) {  // the slot's fence has completed: its staging can go
                for (void* r : slot.retired) Call(r, kRelease);
                slot.retired.clear();
            }
            if (PerCopy()) Call(slot.copy_alloc, kAllocReset);
            for (uint32_t t = 0; t < n; ++t) {
                if (GuestMem()) {
                    FillGuest(frame, t);
                    s.upload_bytes += guest_bytes;
                    ++s.uploads;
                    continue;
                }
                void* staging = nullptr;
                if (Reuse()) {
                    staging = slot.staging[t];
                    FillStaging(slot.staging_map[t], frame, t);
                } else {
                    staging = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, staging_bytes);
                    uint8_t* mapped = Map(staging);
                    FillStaging(mapped, frame, t);
                    Unmap(staging);
                    slot.retired.push_back(staging);
                }
                s.upload_bytes += staging_bytes;
                ++s.uploads;
                if (PerCopy()) {
                    Call(slot.copy_list, kListReset, P(slot.copy_alloc), uintptr_t{0}, uintptr_t{0});
                    RecordTextureCopy(slot.copy_list, tex_stream[t], staging, a.upload_size, staging_pitch);
                    Call(slot.copy_list, kListClose);
                    const int64_t t0 = Ticks();
                    if (!first_submit) first_submit = t0;
                    Execute(slot.copy_list);
                    submit_us += Us(t0, Ticks());
                    ++s.submissions;
                } else {
                    RecordTextureCopy(list, tex_stream[t], staging, a.upload_size, staging_pitch);
                }
            }
        }

        void* heaps[] = {srv_heap};
        Call(list, kListSetDescriptorHeaps, uintptr_t{1}, P(heaps), uintptr_t{0});
        const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(rt_w), static_cast<float>(rt_h), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(rt_w), static_cast<LONG>(rt_h)};
        Call(list, kListRSSetViewports, uintptr_t{1}, P(&vp), uintptr_t{0});
        Call(list, kListRSSetScissorRects, uintptr_t{1}, P(&scissor), uintptr_t{0});
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv{slot.rtv};
        Call(list, kListOMSetRenderTargets, uintptr_t{1}, P(&rtv), uintptr_t{0}, uintptr_t{0});
        const float clear[4] = {16.0f / 255.0f, 32.0f / 255.0f, 48.0f / 255.0f, 1.0f};
        Call(list, kListClearRenderTargetView, slot.rtv, P(clear), uintptr_t{0}, uintptr_t{0});
        Call(list, kListIASetPrimitiveTopology, static_cast<uintptr_t>(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST), uintptr_t{0},
             uintptr_t{0});
        if (TDraws()) {
            // The title's root signature and pipeline, then per draw: a scissor rectangle, the
            // colour (once per frame, or before every draw), three vertices. The probe switches
            // between the translated and the desktop pipeline and changes the colour every
            // second draw, so one list holds reused, changed and switched bindings.
            const uint32_t cols = rt_w / kCell;
            const uint32_t cells = cols * (rt_h / kCell);
            const bool changing = probe || TChanging();
            void* base_pso = a.variant.rfind("translated", 0) == 0 ? pso_t_translated : pso_t_desktop;
            Call(list, kListSetGraphicsRootSignature, P(troot), uintptr_t{0}, uintptr_t{0});
            Call(list, kListSetPipelineState, P(base_pso), uintptr_t{0}, uintptr_t{0});
            void* bound = base_pso;
            auto set_colour = [&](uint32_t j) {
                const uint32_t v = TDrawColor(changing, probe, j, frame);
                const float colour[4] = {static_cast<float>(v & 255u) / 255.0f, static_cast<float>((v >> 8) & 255u) / 255.0f,
                                         static_cast<float>((v >> 16) & 255u) / 255.0f, static_cast<float>(v >> 24) / 255.0f};
                Call(list, kListSetGraphicsRoot32BitConstants, uintptr_t{0}, uintptr_t{4}, P(colour), uintptr_t{0});
                ++s.constant_sets;
            };
            if (!changing) set_colour(0);
            for (uint32_t j = 0; j < a.draws; ++j) {
                const uint32_t cell = DrawCell(j, frame, cells);
                const LONG x = static_cast<LONG>((cell % cols) * kCell), y = static_cast<LONG>((cell / cols) * kCell);
                const D3D12_RECT r{x, y, x + static_cast<LONG>(kCell), y + static_cast<LONG>(kCell)};
                Call(list, kListRSSetScissorRects, uintptr_t{1}, P(&r), uintptr_t{0});
                if (probe) {
                    void* want = (j % 3 == 2) ? pso_t_desktop : pso_t_translated;
                    if (want != bound) {
                        Call(list, kListSetPipelineState, P(want), uintptr_t{0}, uintptr_t{0});
                        bound = want;
                    }
                    if ((j & 1u) == 0) set_colour(j);
                } else if (changing) {
                    set_colour(j);
                }
                Call(list, kListDrawInstanced, uintptr_t{3}, uintptr_t{1}, uintptr_t{0}, uintptr_t{0});
                ++s.draws;
            }
            Call(list, kListRSSetScissorRects, uintptr_t{1}, P(&scissor), uintptr_t{0});
        }
        if (TTex()) {
            // The title's root signature and the translated pipeline; the colour once per frame;
            // per draw a scissor rectangle and, by variant, the table or an unrelated descriptor.
            const uint32_t cols = rt_w / kCell;
            const uint32_t cells = cols * (rt_h / kCell);
            const bool per_table = a.variant == "table-changing";
            const bool per_write = a.variant == "descriptor-write";
            Call(list, kListSetGraphicsRootSignature, P(xroot), uintptr_t{0}, uintptr_t{0});
            Call(list, kListSetPipelineState, P(pso_ttex), uintptr_t{0}, uintptr_t{0});
            uint32_t add[4];
            TTexAdd(frame, add);
            const float colour[4] = {static_cast<float>(add[0]) / 255.0f, static_cast<float>(add[1]) / 255.0f, 0.0f, 0.0f};
            Call(list, kListSetGraphicsRoot32BitConstants, uintptr_t{0}, uintptr_t{4}, P(colour), uintptr_t{0});
            ++s.constant_sets;
            Call(list, kListSetGraphicsRootDescriptorTable, uintptr_t{1}, SrvGpu(1), uintptr_t{0});
            ++s.table_binds;
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            for (uint32_t j = 0; j < a.draws; ++j) {
                const uint32_t cell = DrawCell(j, frame, cells);
                const LONG x = static_cast<LONG>((cell % cols) * kCell), y = static_cast<LONG>((cell / cols) * kCell);
                const D3D12_RECT r{x, y, x + static_cast<LONG>(kCell), y + static_cast<LONG>(kCell)};
                Call(list, kListRSSetScissorRects, uintptr_t{1}, P(&r), uintptr_t{0});
                if (per_table) {
                    Call(list, kListSetGraphicsRootDescriptorTable, uintptr_t{1}, SrvGpu(1 + (j & 1u)), uintptr_t{0});
                    ++s.table_binds;
                }
                if (per_write) {
                    // A descriptor no draw uses, written again: any descriptor write makes the shim
                    // look at the bound table's view once more.
                    Call(dev, kDevCreateShaderResourceView, P(ttex_texture[0]), P(&view), SrvCpu(3));
                    ++s.uploads;  // reported as descriptor writes for this mode
                }
                Call(list, kListDrawInstanced, uintptr_t{3}, uintptr_t{1}, uintptr_t{0}, uintptr_t{0});
                ++s.draws;
            }
            Call(list, kListRSSetScissorRects, uintptr_t{1}, P(&scissor), uintptr_t{0});
        }
        if (TDispatch()) {
            // The title's compute root and the translated pipeline, then per dispatch: the table
            // (once per frame, or a different descriptor every time), the constants (once, or
            // every time), two groups of 32.
            const bool per_table = a.variant == "changing-table";
            const bool per_constants = a.variant == "changing-constants";
            Call(list, kListSetComputeRootSignature, P(croot), uintptr_t{0}, uintptr_t{0});
            Call(list, kListSetPipelineState, P(pso_compute), uintptr_t{0}, uintptr_t{0});
            auto set_constants = [&](uint32_t j) {
                const uint32_t constants[4] = {DispatchC0(per_constants, j, frame), c1, 0, 0};
                Call(list, kListSetComputeRoot32BitConstants, uintptr_t{0}, uintptr_t{4}, P(constants), uintptr_t{0});
                ++s.constant_sets;
                return constants[0];
            };
            uint32_t c0 = set_constants(0);
            uint32_t slot_index = 0;
            Call(list, kListSetComputeRootDescriptorTable, uintptr_t{1}, SrvGpu(0), uintptr_t{0});
            ++s.table_binds;
            for (uint32_t j = 0; j < a.draws; ++j) {
                if (per_table) {
                    slot_index = j % a.descriptors;
                    Call(list, kListSetComputeRootDescriptorTable, uintptr_t{1}, SrvGpu(slot_index), uintptr_t{0});
                    ++s.table_binds;
                }
                if (per_constants && j) c0 = set_constants(j);
                Call(list, kListDispatch, static_cast<uintptr_t>(kDispatchDwords / 32), uintptr_t{1}, uintptr_t{1});
                if (per_constants) {
                    // Every dispatch writes the same buffer with different values, so each needs a
                    // UAV barrier to come after the one before. The other two variants write equal
                    // values wherever they write twice and go without. The barrier is most of
                    // this variant's frame on this host.
                    D3D12_RESOURCE_BARRIER uav{};
                    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    Call(list, kListResourceBarrier, uintptr_t{1}, P(&uav), uintptr_t{0});
                }
                ubuf_c0[slot_index] = c0;
                ubuf_written[slot_index] = 1;
                ++s.draws;
            }
        }
        Call(list, kListSetGraphicsRootSignature, P(root), uintptr_t{0}, uintptr_t{0});
        Call(list, kListIASetVertexBuffers, uintptr_t{0}, uintptr_t{1}, P(&vbv));
        Call(list, kListIASetIndexBuffer, P(&ibv), uintptr_t{0}, uintptr_t{0});
        if (!TDraws()) Call(list, kListSetPipelineState, P(pso_main), uintptr_t{0}, uintptr_t{0});

        DrawConstants c{};
        c.target[0] = rt_w;
        c.target[1] = rt_h;
        c.frame = frame;
        c.seed = a.seed;
        if (a.mode == "draws") {
            const uint32_t cols = rt_w / kCell;
            const uint32_t cells = cols * (rt_h / kCell);
            const bool per_draw = probe || a.variant == "pertable";
            const uint32_t first = probe ? a.textures : 0;
            if (!per_draw) {
                Call(list, kListSetGraphicsRootDescriptorTable, uintptr_t{1}, SrvGpu(0), uintptr_t{0});
                ++s.table_binds;
            }
            for (uint32_t j = 0; j < a.draws; ++j) {
                const uint32_t cell = DrawCell(j, frame, cells);
                c.rect[0] = (cell % cols) * kCell;
                c.rect[1] = (cell / cols) * kCell;
                c.rect[2] = c.rect[0] + kCell;
                c.rect[3] = c.rect[1] + kCell;
                c.draw_id = j;
                SetConstants(list, c);
                if (per_draw) {
                    Call(list, kListSetGraphicsRootDescriptorTable, uintptr_t{1}, SrvGpu(first + j % a.textures),
                         uintptr_t{0});
                    ++s.table_binds;
                }
                DrawQuad(list);
                ++s.draws;
            }
        } else if (TDraws() || TDispatch() || TTex()) {
            // done above: tdraws with the title's own root signature, tdispatch on the compute side
        } else if (a.mode == "pixel") {
            static std::vector<Rect> scene;
            PixelScene(rt_w, rt_h, a.seed, frame, scene);
            Call(list, kListSetGraphicsRootDescriptorTable, uintptr_t{1}, SrvGpu(0), uintptr_t{0});
            ++s.table_binds;
            for (uint32_t i = 0; i < scene.size(); ++i) {
                c.rect[0] = scene[i].x0;
                c.rect[1] = scene[i].y0;
                c.rect[2] = scene[i].x1;
                c.rect[3] = scene[i].y1;
                c.draw_id = i;
                SetConstants(list, c);
                DrawQuad(list);
                ++s.draws;
            }
        } else {  // upload: every streamed texture, texel for pixel
            const uint32_t sz = a.upload_size;
            for (uint32_t t = 0; t < a.upload_count; ++t) {
                c.rect[0] = (t % grid_cols) * sz;
                c.rect[1] = (t / grid_cols) * sz;
                c.rect[2] = c.rect[0] + sz;
                c.rect[3] = c.rect[1] + sz;
                c.origin[0] = c.rect[0];
                c.origin[1] = c.rect[1];
                c.draw_id = t;
                SetConstants(list, c);
                Call(list, kListSetGraphicsRootDescriptorTable, uintptr_t{1}, SrvGpu(t), uintptr_t{0});
                ++s.table_binds;
                DrawQuad(list);
                ++s.draws;
            }
        }
        if (a.markers) {
            if (TDraws() || TDispatch() || TTex()) {
                Call(list, kListSetGraphicsRootDescriptorTable, uintptr_t{1}, SrvGpu(MarkerSrvSlot()), uintptr_t{0});
            }
            Call(list, kListSetPipelineState, P(pso_solid), uintptr_t{0}, uintptr_t{0});
            for (uint32_t k = 0; k < MarkerCount(); ++k) {
                const Rect r = MarkerRect(rt_w, rt_h, k);
                c.rect[0] = r.x0;
                c.rect[1] = r.y0;
                c.rect[2] = r.x1;
                c.rect[3] = r.y1;
                c.color = MarkerColor(frame, k);
                SetConstants(list, c);
                DrawQuad(list);
                ++s.draws;
            }
        }
        if (readback) RecordReadback(list, slot.rt, readback);
        const intptr_t closed = Call(list, kListClose);
        if (closed != 0) {
            Fail("shim ID3D12GraphicsCommandList::Close (slot 9)", closed);
            std::exit(2);
        }

        const int64_t t0 = Ticks();
        if (!first_submit) first_submit = t0;
        Execute(list);
        slot.fence_value = Signal();
        const int64_t t1 = Ticks();
        ++s.submissions;
        s.submit_us = submit_us + Us(t0, t1);
        slot.last_frame = frame;
        slot.used = true;
        if (a.path == "serialized") {
            WaitFor(slot.fence_value);
            s.serialized_us = Us(first_submit, Ticks());
        }
    }

    void RecordReadback(void* list, void* rt, void* readback) {
        Barrier(list, rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = static_cast<ID3D12Resource*>(rt);
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = static_cast<ID3D12Resource*>(readback);
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width = rt_w;
        dst.PlacedFootprint.Footprint.Height = rt_h;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = rt_pitch;
        Call(list, kListCopyTextureRegion, P(&dst), uintptr_t{0}, uintptr_t{0}, uintptr_t{0}, P(&src), uintptr_t{0});
        Barrier(list, rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    // ---- the CPU's picture of a frame --------------------------------------------------------
    void Expected(uint32_t frame, bool probe, std::vector<uint32_t>& img) const {
        img.assign(static_cast<size_t>(rt_w) * rt_h, kClearDword);
        if (TTex()) {
            // The pixel program samples at (x / 64, y / 64) with a clamping point sampler: texel
            // (min(x / 8, 7), min(y / 8, 7)) of the texture the draw's table names, plus the colour.
            const uint32_t cols = rt_w / kCell;
            const uint32_t cells = cols * (rt_h / kCell);
            const uint32_t shift = ((frame % cells) * 37u) % cells;
            uint32_t add[4];
            TTexAdd(frame, add);
            for (uint32_t y = 0; y < rt_h; ++y) {
                for (uint32_t x = 0; x < rt_w; ++x) {
                    const uint32_t cell = (y / kCell) * cols + x / kCell;
                    const uint32_t j = (cell + cells - shift) % cells;
                    if (j >= a.draws) continue;
                    const uint32_t t = TTexTexel(a.variant == "table-changing" ? (j & 1u) : 0u, std::min(x / 8u, 7u), std::min(y / 8u, 7u));
                    img[static_cast<size_t>(y) * rt_w + x] = t + add[0] + (add[1] << 8);
                }
            }
        } else if (TDispatch()) {
            // nothing is drawn: the target keeps its clear colour, the buffers are checked apart
        } else if (a.mode == "draws") {
            const uint32_t cols = rt_w / kCell;
            const uint32_t cells = cols * (rt_h / kCell);
            const uint32_t shift = ((frame % cells) * 37u) % cells;
            for (uint32_t y = 0; y < rt_h; ++y) {
                for (uint32_t x = 0; x < rt_w; ++x) {
                    const uint32_t cell = (y / kCell) * cols + x / kCell;
                    const uint32_t j = (cell + cells - shift) % cells;
                    if (j >= a.draws) continue;
                    const uint32_t texel = probe ? Hash3(77 + j % a.textures, x & 15u, y & 15u) : Hash3(7, x & 15u, y & 15u);
                    img[static_cast<size_t>(y) * rt_w + x] = (texel & 0xFFFFu) | ((j & 0xFFFFu) << 16);
                }
            }
        } else if (TDraws()) {
            const uint32_t cols = rt_w / kCell;
            const uint32_t cells = cols * (rt_h / kCell);
            const uint32_t shift = ((frame % cells) * 37u) % cells;
            for (uint32_t y = 0; y < rt_h; ++y) {
                for (uint32_t x = 0; x < rt_w; ++x) {
                    const uint32_t cell = (y / kCell) * cols + x / kCell;
                    const uint32_t j = (cell + cells - shift) % cells;
                    if (j < a.draws) img[static_cast<size_t>(y) * rt_w + x] = TDrawColor(probe || TChanging(), probe, j, frame);
                }
            }
        } else if (a.mode == "pixel") {
            std::vector<Rect> scene;
            PixelScene(rt_w, rt_h, a.seed, frame, scene);
            std::vector<uint8_t> owner(static_cast<size_t>(rt_w) * rt_h, 0);
            for (uint32_t i = 1; i < scene.size(); ++i) {
                for (uint32_t y = scene[i].y0; y < scene[i].y1; ++y) {
                    std::memset(owner.data() + static_cast<size_t>(y) * rt_w + scene[i].x0, static_cast<int>(i),
                                scene[i].x1 - scene[i].x0);
                }
            }
            const uint32_t rounds = a.variant == "heavy" ? a.rounds : 0;
            for (uint32_t y = 0; y < rt_h; ++y) {
                for (uint32_t x = 0; x < rt_w; ++x) {
                    const size_t at = static_cast<size_t>(y) * rt_w + x;
                    img[at] = HeavyRounds(MixPixel(x, y, frame, owner[at], a.seed), rounds);
                }
            }
        } else {
            const uint32_t sz = a.upload_size;
            for (uint32_t t = 0; t < a.upload_count; ++t) {
                const uint32_t ox = (t % grid_cols) * sz, oy = (t / grid_cols) * sz;
                const uint32_t p = PayloadIndex(frame, t);
                for (uint32_t y = 0; y < sz; ++y) {
                    for (uint32_t x = 0; x < sz; ++x) {
                        img[static_cast<size_t>(oy + y) * rt_w + ox + x] = Hash3(p + 1000u, x, y);
                    }
                }
                for (uint32_t k = 0; k < kStamps; ++k) {
                    uint32_t x, y;
                    StampPos(k, &x, &y);
                    img[static_cast<size_t>(oy + y) * rt_w + ox + x] = StampValue(frame, t, k);
                }
            }
        }
        if (a.markers) {
            for (uint32_t k = 0; k < MarkerCount(); ++k) {
                const Rect r = MarkerRect(rt_w, rt_h, k);
                const uint32_t v = MarkerColor(frame, k);
                for (uint32_t y = r.y0; y < r.y1; ++y) {
                    for (uint32_t x = r.x0; x < r.x1; ++x) img[static_cast<size_t>(y) * rt_w + x] = v;
                }
            }
        }
    }

    // Compares a read-back buffer with the CPU's picture. Returns the number of exact pixels.
    uint64_t Compare(void* readback, uint32_t frame, bool probe, const char* what) {
        std::vector<uint32_t> want;
        Expected(frame, probe, want);
        const uint8_t* data = Map(readback);
        uint64_t exact = 0;
        bool have_bad = false;
        uint32_t bad_x = 0, bad_y = 0, bad_got = 0, bad_want = 0;
        for (uint32_t y = 0; y < rt_h; ++y) {
            const uint8_t* row = data + static_cast<size_t>(y) * rt_pitch;
            for (uint32_t x = 0; x < rt_w; ++x) {
                uint32_t got;
                std::memcpy(&got, row + x * 4, 4);
                if (got == want[static_cast<size_t>(y) * rt_w + x]) {
                    ++exact;
                } else if (!have_bad) {
                    have_bad = true;
                    bad_x = x;
                    bad_y = y;
                    bad_got = got;
                    bad_want = want[static_cast<size_t>(y) * rt_w + x];
                }
            }
        }
        Unmap(readback);
        const uint64_t total = static_cast<uint64_t>(rt_w) * rt_h;
        std::printf("[fps] check %s frame=%u pixels exact %llu/%llu", what, frame, static_cast<unsigned long long>(exact),
                    static_cast<unsigned long long>(total));
        if (exact != total) {
            std::printf(" first mismatch at (%u,%u) got 0x%08x want 0x%08x", bad_x, bad_y, bad_got, bad_want);
        }
        std::printf("\n");
        std::fflush(stdout);
        return exact;
    }
};

bool Compile(const std::string& hlsl, const char* entry, const char* target, ID3DBlob** out) {
    ID3DBlob* err = nullptr;
    const HRESULT hr = D3DCompile(hlsl.data(), hlsl.size(), "bench_sx_fps.hlsl", nullptr, nullptr, entry, target, 0, 0, out, &err);
    if (FAILED(hr)) {
        std::printf("[fps] FAILED api=D3DCompile(%s) hr=0x%08x %s\n", entry, static_cast<uint32_t>(hr),
                    err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        std::fflush(stdout);
        return false;
    }
    return true;
}

bool ParseArgs(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        if (k == "--markers") {
            a.markers = true;
            continue;
        }
        if (i + 1 >= argc) return false;
        const char* v = argv[++i];
        auto u = [&] { return static_cast<uint32_t>(std::strtoul(v, nullptr, 0)); };
        if (k == "--mode") a.mode = v;
        else if (k == "--variant") a.variant = v;
        else if (k == "--path") a.path = v;
        else if (k == "--shader-source") a.shader_source = v;
        else if (k == "--series") a.series = v;
        else if (k == "--csv") a.csv = v;
        else if (k == "--frames-csv") a.frames_csv = v;
        else if (k == "--launch") a.launch = u();
        else if (k == "--frames") a.frames = u();
        else if (k == "--warmup") a.warmup = u();
        else if (k == "--inflight") a.inflight = u();
        else if (k == "--check-frames") a.check_frames = u();
        else if (k == "--seed") a.seed = u();
        else if (k == "--width") a.width = u();
        else if (k == "--height") a.height = u();
        else if (k == "--draws") a.draws = u();
        else if (k == "--textures") a.textures = u();
        else if (k == "--descriptors") a.descriptors = u();
        else if (k == "--rounds") a.rounds = u();
        else if (k == "--upload-count") a.upload_count = u();
        else if (k == "--upload-size") a.upload_size = u();
        else if (k == "--interval-us") a.interval_us = u();
        else if (k == "--hold-ms") a.hold_ms = u();
        else if (k == "--idle-guest-textures") a.idle_guest_textures = u();
        else return false;
    }
    const bool mode_ok =
        (a.mode == "draws" && (a.variant == "fixed" || a.variant == "pertable")) ||
        (a.mode == "pixel" && (a.variant == "light" || a.variant == "heavy")) ||
        (a.mode == "ttex" && (a.variant == "table-same" || a.variant == "table-changing" || a.variant == "descriptor-write")) ||
        (a.mode == "tdispatch" && (a.variant == "same" || a.variant == "changing-table" || a.variant == "changing-constants")) ||
        (a.mode == "tdraws" && (a.variant == "translated-same" || a.variant == "translated-changing" ||
                                a.variant == "desktop-same" || a.variant == "desktop-changing")) ||
        (a.mode == "upload" && (a.variant == "percopy" || a.variant == "batched" || a.variant == "reuse" ||
                                a.variant == "batched-reuse" || a.variant == "guestmem"));
    const bool path_ok = a.path == "offscreen" || a.path == "serialized" || a.path == "windowed";
    const bool source_ok = a.shader_source == "desktop" || a.shader_source == "translated";
    return mode_ok && path_ok && source_ok && a.frames > 0 && a.inflight > 0 && a.inflight <= 16 && a.width >= 64 &&
           a.height >= 64 && a.width % 32 == 0 && a.textures > 0 && a.textures <= 64 && a.upload_count > 0 &&
           a.upload_count <= 64 && a.upload_size >= 32 && a.rounds <= 4096 && a.descriptors > 0 && a.descriptors <= 65536;
}

}  // namespace

int main(int argc, char** argv) {
    Bench b;
    Args& a = b.a;
    if (!ParseArgs(argc, argv, a)) {
        std::fprintf(stderr,
                     "usage: bench_sx_fps --mode draws|tdraws|ttex|tdispatch|pixel|upload --variant <see the top of bench_sx_fps.cpp>\n"
                     "  [--path offscreen|serialized|windowed] [--shader-source desktop|translated]\n"
                     "  [--frames N] [--warmup N] [--inflight N] [--check-frames N] [--seed N]\n"
                     "  [--width N --height N] [--draws N] [--textures N] [--rounds N]\n"
                     "  [--upload-count N] [--upload-size N] [--interval-us N] [--markers] [--hold-ms N]\n"
                     "  [--idle-guest-textures N]\n"
                     "  [--series NAME] [--launch N] [--csv FILE] [--frames-csv FILE]\n");
        return 64;
    }
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpc_freq = freq.QuadPart;
    if (a.path == "serialized") a.inflight = 1;
    if (a.path == "windowed") a.markers = true;
    const bool translated_variant = a.mode == "tdraws" && a.variant.rfind("translated", 0) == 0;
    const char* shader_path = translated_variant
                                  ? "synthetic native VS/PS -> shim CreateGraphicsPipelineState (translated by the "
                                    "victory-shader bridge) -> shim command list (guest-facing route)"
                                  : "desktop HLSL -> D3DCompile -> shim CreateGraphicsPipelineState (guest-facing route)";
    // The translated route supports root constants only, so the modes that bind a descriptor
    // table or a vertex buffer (draws, pixel, upload) have no translated form yet: tdraws is the
    // translated workload.
    if (a.shader_source == "translated") {
        std::printf("[fps] shader_source=translated is not available for --mode %s: the shim's translated graphics route "
                    "supports root constants only. Use --mode tdraws --variant translated-same|translated-changing.\n",
                    a.mode.c_str());
        return 5;
    }

    _putenv_s("SCARLETTX_REAL_TITLE", "1");
    SetEnvironmentVariableA("SCARLETTX_REAL_TITLE", "1");
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1;
    params.PreferWarp = 0;
    const int32_t created = D3D12XboxCreateDevice(nullptr, &params, &__uuidof(ID3D12Device), &b.dev);
    if (created < 0 || !b.dev) {
        std::printf("[fps] D3D12XboxCreateDevice FAILED hr=0x%08x\n", static_cast<uint32_t>(created));
        return 3;
    }
    // Environment record only: which adapter the shim's device sits on.
    if (D3D12XboxGetNativeObject(b.dev, reinterpret_cast<void**>(&b.native_dev)) == 0 && b.native_dev) {
        const LUID luid = b.native_dev->GetAdapterLuid();
        IDXGIFactory4* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory) {
            IDXGIAdapter1* adapter = nullptr;
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 d{};
                adapter->GetDesc1(&d);
                if (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) {
                    char name[128] = {};
                    WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
                    std::printf("[fps] adapter=\"%s\" vendor=0x%04x device=0x%04x software=%d\n", name, d.VendorId,
                                d.DeviceId, (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? 1 : 0);
                }
                adapter->Release();
            }
            factory->Release();
        }
    }
    DEVMODEA dm{};
    dm.dmSize = sizeof(dm);
    const DWORD display_hz = EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? dm.dmDisplayFrequency : 0;
    std::printf("[fps] mode=%s variant=%s path=%s shader_path=\"%s\" frames=%u warmup=%u inflight=%u seed=0x%08x\n",
                a.mode.c_str(), a.variant.c_str(), a.path.c_str(), shader_path, a.frames, a.warmup, a.inflight, a.seed);

    // ---- queue, fence, heaps -----------------------------------------------------------------
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    intptr_t hr = Call(b.dev, kDevCreateCommandQueue, P(&queue_desc), uintptr_t{0}, P(&b.queue));
    if (hr != 0 || !b.queue) return Fail("shim CreateCommandQueue (slot 8)", hr);
    hr = Call(b.dev, kDevCreateFence, uintptr_t{0}, uintptr_t{0}, uintptr_t{0}, P(&b.fence));
    if (hr != 0 || !b.fence) return Fail("shim CreateFence (slot 36)", hr);
    b.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (a.mode == "upload") {
        b.grid_cols = 1;
        while (b.grid_cols * b.grid_cols < a.upload_count) ++b.grid_cols;
        const uint32_t rows = (a.upload_count + b.grid_cols - 1) / b.grid_cols;
        b.rt_w = b.grid_cols * a.upload_size;
        b.rt_h = rows * a.upload_size;
        if (b.rt_w % 32 != 0) {
            std::fprintf(stderr, "bench_sx_fps: upload grid width %u is not a multiple of 32\n", b.rt_w);
            return 64;
        }
    } else {
        b.rt_w = a.width;
        b.rt_h = a.height;
    }
    b.rt_pitch = (b.rt_w * 4 + 255u) & ~255u;
    if ((a.mode == "draws" || a.mode == "tdraws" || a.mode == "ttex") && a.draws > (b.rt_w / kCell) * (b.rt_h / kCell)) {
        std::fprintf(stderr, "bench_sx_fps: --draws %u does not fit %ux%u\n", a.draws, b.rt_w, b.rt_h);
        return 64;
    }

    const uint32_t srv_count = a.mode == "draws" ? a.textures * 2
                               : a.mode == "upload" ? a.upload_count
                               : a.mode == "tdispatch" ? a.descriptors + 1
                               : a.mode == "ttex" ? 4 : 1;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = srv_count;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = Call(b.dev, kDevCreateDescriptorHeap, P(&heap_desc), uintptr_t{0}, P(&b.srv_heap));
    if (hr != 0 || !b.srv_heap) return Fail("shim CreateDescriptorHeap(CBV_SRV_UAV) (slot 14)", hr);
    // Checked frames get slots of their own, after the in-flight ones, so they can run straight
    // after the timed loop without overwriting what the loop left in its render targets.
    const uint32_t check_slots = std::min<uint32_t>(a.check_frames, 8) + (a.mode == "draws" || a.mode == "tdraws" ? 1 : 0);
    const uint32_t rt_count = a.inflight + check_slots;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = rt_count;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = Call(b.dev, kDevCreateDescriptorHeap, P(&heap_desc), uintptr_t{0}, P(&b.rtv_heap));
    if (hr != 0 || !b.rtv_heap) return Fail("shim CreateDescriptorHeap(RTV) (slot 14)", hr);
    b.srv_cpu0 = static_cast<uintptr_t>(Call(b.srv_heap, kHeapGetCpuStart));
    b.srv_gpu0 = static_cast<uintptr_t>(Call(b.srv_heap, kHeapGetGpuStart));
    b.rtv_cpu0 = static_cast<uintptr_t>(Call(b.rtv_heap, kHeapGetCpuStart));
    b.srv_inc = static_cast<uint32_t>(Call(b.dev, kDevGetDescriptorHandleIncrementSize,
                                           static_cast<uintptr_t>(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)));
    b.rtv_inc = static_cast<uint32_t>(
        Call(b.dev, kDevGetDescriptorHandleIncrementSize, static_cast<uintptr_t>(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)));

    // ---- root signature and pipelines --------------------------------------------------------
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER root_params[2]{};
    root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_params[0].Constants.Num32BitValues = 12;
    root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_params[1].DescriptorTable.NumDescriptorRanges = 1;
    root_params[1].DescriptorTable.pDescriptorRanges = &range;
    root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 2;
    rs_desc.pParameters = root_params;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* rs_blob = nullptr;
    ID3DBlob* rs_err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err))) {
        return Fail("D3D12SerializeRootSignature", E_FAIL);
    }
    hr = Call(b.dev, kDevCreateRootSignature, uintptr_t{0}, P(rs_blob->GetBufferPointer()),
              static_cast<uintptr_t>(rs_blob->GetBufferSize()), uintptr_t{0}, P(&b.root));
    if (hr != 0 || !b.root) return Fail("shim CreateRootSignature (slot 16)", hr);

    std::string hlsl(std::strlen(kHlslTemplate) + 32, '\0');
    hlsl.resize(static_cast<size_t>(std::snprintf(hlsl.data(), hlsl.size(), kHlslTemplate, a.rounds)));
    const char* ps_entry = a.mode == "draws" || a.mode == "tdraws" ? "PSTex" : (a.mode == "upload" ? "PSCopy" : (a.variant == "heavy" ? "PSHeavy" : "PSLight"));
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3DBlob* ps_solid = nullptr;
    const int64_t compile_start = Ticks();
    if (!Compile(hlsl, "VSMain", "vs_5_0", &vs) || !Compile(hlsl, ps_entry, "ps_5_0", &ps) ||
        !Compile(hlsl, "PSSolid", "ps_5_0", &ps_solid)) {
        return 2;
    }
    const double compile_ms = Us(compile_start, Ticks()) / 1000.0;
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    auto make_pipeline = [&](ID3DBlob* pixel, void** out) -> intptr_t {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = static_cast<ID3D12RootSignature*>(b.root);  // the shim's object
        d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        d.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = 0xFFFFFFFFu;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.InputLayout = {layout, 1};
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        *out = nullptr;
        return Call(b.dev, kDevCreateGraphicsPipelineState, P(&d), uintptr_t{0}, P(out));
    };
    const int64_t pso_start = Ticks();
    hr = make_pipeline(ps, &b.pso_main);
    if (hr != 0 || !b.pso_main) return Fail("shim CreateGraphicsPipelineState (slot 10)", hr);
    const double pso_ms = Us(pso_start, Ticks()) / 1000.0;
    hr = make_pipeline(ps_solid, &b.pso_solid);
    if (hr != 0 || !b.pso_solid) return Fail("shim CreateGraphicsPipelineState, markers (slot 10)", hr);

    // ---- tdraws: the title's root signature (four root constants) and the two pipelines -------
    bool have_translated = false;
    double translated_pso_ms = 0;
    if (a.mode == "tdraws") {
        D3D12_ROOT_PARAMETER tparam{};
        tparam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        tparam.Constants.Num32BitValues = 4;
        D3D12_ROOT_SIGNATURE_DESC trs{};
        trs.NumParameters = 1;
        trs.pParameters = &tparam;
        ID3DBlob* trs_blob = nullptr;
        if (FAILED(D3D12SerializeRootSignature(&trs, D3D_ROOT_SIGNATURE_VERSION_1, &trs_blob, &rs_err))) {
            return Fail("D3D12SerializeRootSignature(tdraws)", E_FAIL);
        }
        hr = Call(b.dev, kDevCreateRootSignature, uintptr_t{0}, P(trs_blob->GetBufferPointer()),
                  static_cast<uintptr_t>(trs_blob->GetBufferSize()), uintptr_t{0}, P(&b.troot));
        if (hr != 0 || !b.troot) return Fail("shim CreateRootSignature, tdraws (slot 16)", hr);
        auto make_tdraw = [&](const void* vsb, size_t vsn, const void* psb, size_t psn, void** out) -> intptr_t {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
            d.pRootSignature = static_cast<ID3D12RootSignature*>(b.troot);
            d.VS = {vsb, vsn};
            d.PS = {psb, psn};
            d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            d.SampleMask = 0xFFFFFFFFu;
            d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            d.RasterizerState.DepthClipEnable = TRUE;
            d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            d.NumRenderTargets = 1;
            d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = 1;
            *out = nullptr;
            return Call(b.dev, kDevCreateGraphicsPipelineState, P(&d), uintptr_t{0}, P(out));
        };
        const std::string thlsl = kTDrawHlsl;
        ID3DBlob* tvs = nullptr;
        ID3DBlob* tps = nullptr;
        if (!Compile(thlsl, "VSMain", "vs_5_0", &tvs) || !Compile(thlsl, "PSMain", "ps_5_0", &tps)) return 2;
        hr = make_tdraw(tvs->GetBufferPointer(), tvs->GetBufferSize(), tps->GetBufferPointer(), tps->GetBufferSize(),
                        &b.pso_t_desktop);
        if (hr != 0 || !b.pso_t_desktop) return Fail("shim CreateGraphicsPipelineState, tdraws desktop (slot 10)", hr);
        // The synthetic native vertex (9) and pixel (10) programs, as console-shaped containers.
        std::vector<uint8_t> nvs, nps;
        if (scarlettx::BuildVictorySyntheticContainer(9, nvs).ok() && scarlettx::BuildVictorySyntheticContainer(10, nps).ok()) {
            const int64_t t0 = Ticks();
            hr = make_tdraw(nvs.data(), nvs.size(), nps.data(), nps.size(), &b.pso_t_translated);
            translated_pso_ms = Us(t0, Ticks()) / 1000.0;
            if (hr != 0 || !b.pso_t_translated) return Fail("shim CreateGraphicsPipelineState, translated (slot 10)", hr);
            D3D12XBOX_GRAPHICS_TRANSLATION_STATS gs{};
            D3D12XboxGetGraphicsTranslationStats(&gs);
            if (gs.graphics_translated != 1) {
                std::printf("[fps] FAILED the shim did not translate the native pipeline (graphics_translated=%llu)\n",
                            static_cast<unsigned long long>(gs.graphics_translated));
                return 2;
            }
            have_translated = true;
        } else if (translated_variant) {
            std::printf("[fps] translated variant NOT AVAILABLE: this build has no victory-shader bridge\n");
            return 5;
        }
    }

    // ---- ttex: root signature and the translated textured pipeline (its textures come later) ---
    if (a.mode == "ttex") {
        D3D12_DESCRIPTOR_RANGE xrange{};
        xrange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        xrange.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER xparams[2]{};
        xparams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        xparams[0].Constants.Num32BitValues = 4;
        xparams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        xparams[1].DescriptorTable.NumDescriptorRanges = 1;
        xparams[1].DescriptorTable.pDescriptorRanges = &xrange;
        D3D12_ROOT_SIGNATURE_DESC xrs{};
        xrs.NumParameters = 2;
        xrs.pParameters = xparams;
        ID3DBlob* xrs_blob = nullptr;
        if (FAILED(D3D12SerializeRootSignature(&xrs, D3D_ROOT_SIGNATURE_VERSION_1, &xrs_blob, &rs_err))) {
            return Fail("D3D12SerializeRootSignature(ttex)", E_FAIL);
        }
        hr = Call(b.dev, kDevCreateRootSignature, uintptr_t{0}, P(xrs_blob->GetBufferPointer()),
                  static_cast<uintptr_t>(xrs_blob->GetBufferSize()), uintptr_t{0}, P(&b.xroot));
        if (hr != 0 || !b.xroot) return Fail("shim CreateRootSignature, ttex (slot 16)", hr);
        std::vector<uint8_t> nvs, nps;
        if (!scarlettx::BuildVictorySyntheticContainer(9, nvs).ok() || !scarlettx::BuildVictorySyntheticContainer(17, nps).ok()) {
            std::printf("[fps] ttex NOT AVAILABLE: this build has no victory-shader bridge, or no textured pixel fixture\n");
            return 5;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = static_cast<ID3D12RootSignature*>(b.xroot);
        d.VS = {nvs.data(), nvs.size()};
        d.PS = {nps.data(), nps.size()};
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = 0xFFFFFFFFu;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        hr = Call(b.dev, kDevCreateGraphicsPipelineState, P(&d), uintptr_t{0}, P(&b.pso_ttex));
        if (hr != 0 || !b.pso_ttex) return Fail("shim CreateGraphicsPipelineState, translated textured (slot 10)", hr);
    }

    // ---- tdispatch: the compute root, the translated pipeline, a buffer and a UAV per descriptor
    double compute_pso_ms = 0;
    if (a.mode == "tdispatch") {
        D3D12_DESCRIPTOR_RANGE urange{};
        urange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        urange.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER cparams[2]{};
        cparams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        cparams[0].Constants.Num32BitValues = 4;
        cparams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        cparams[1].DescriptorTable.NumDescriptorRanges = 1;
        cparams[1].DescriptorTable.pDescriptorRanges = &urange;
        D3D12_ROOT_SIGNATURE_DESC crs{};
        crs.NumParameters = 2;
        crs.pParameters = cparams;
        ID3DBlob* crs_blob = nullptr;
        if (FAILED(D3D12SerializeRootSignature(&crs, D3D_ROOT_SIGNATURE_VERSION_1, &crs_blob, &rs_err))) {
            return Fail("D3D12SerializeRootSignature(tdispatch)", E_FAIL);
        }
        hr = Call(b.dev, kDevCreateRootSignature, uintptr_t{0}, P(crs_blob->GetBufferPointer()),
                  static_cast<uintptr_t>(crs_blob->GetBufferSize()), uintptr_t{0}, P(&b.croot));
        if (hr != 0 || !b.croot) return Fail("shim CreateRootSignature, tdispatch (slot 16)", hr);
        std::vector<uint8_t> kernel;
        if (!scarlettx::BuildVictoryVectorKernelContainer(kDispatchLiteral, kernel).ok()) {
            std::printf("[fps] tdispatch NOT AVAILABLE: this build has no victory-shader bridge\n");
            return 5;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
        cd.pRootSignature = static_cast<ID3D12RootSignature*>(b.croot);
        cd.CS = {kernel.data(), kernel.size()};
        const int64_t t0 = Ticks();
        hr = Call(b.dev, kDevCreateComputePipelineState, P(&cd), uintptr_t{0}, P(&b.pso_compute));
        compute_pso_ms = Us(t0, Ticks()) / 1000.0;
        if (hr != 0 || !b.pso_compute) return Fail("shim CreateComputePipelineState, translated (slot 11)", hr);
        D3D12XBOX_SHADER_TRANSLATION_STATS cs{};
        D3D12XboxGetShaderTranslationStats(&cs);
        if (cs.compute_translated != 1) return Fail("the shim did not translate the native compute program", E_FAIL);
        b.c1 = 0xABCD0000u ^ a.seed;
        b.ubuf_c0.assign(a.descriptors, 0);
        b.ubuf_written.assign(a.descriptors, 0);
        D3D12_RESOURCE_DESC ud{};
        ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ud.Width = kDispatchDwords * 4;
        ud.Height = 1;
        ud.DepthOrArraySize = 1;
        ud.MipLevels = 1;
        ud.SampleDesc.Count = 1;
        ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ud.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = kDispatchDwords;
        view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        for (uint32_t i = 0; i < a.descriptors; ++i) {
            b.ubuf.push_back(b.CreateResource(D3D12_HEAP_TYPE_DEFAULT, ud, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
            Call(b.dev, kDevCreateUnorderedAccessView, P(b.ubuf.back()), uintptr_t{0}, P(&view), b.SrvCpu(i));
        }
    }

    // ---- geometry: one unit quad -------------------------------------------------------------
    const float corners[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    const uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
    b.vb = b.CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, sizeof(corners));
    b.ib = b.CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, sizeof(indices));
    std::memcpy(b.Map(b.vb), corners, sizeof(corners));
    b.Unmap(b.vb);
    std::memcpy(b.Map(b.ib), indices, sizeof(indices));
    b.Unmap(b.ib);
    b.vbv.BufferLocation = static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(Call(b.vb, kResGetGPUVirtualAddress));
    b.vbv.SizeInBytes = sizeof(corners);
    b.vbv.StrideInBytes = 8;
    b.ibv.BufferLocation = static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(Call(b.ib, kResGetGPUVirtualAddress));
    b.ibv.SizeInBytes = sizeof(indices);
    b.ibv.Format = DXGI_FORMAT_R16_UINT;
    if (!b.vbv.BufferLocation || !b.ibv.BufferLocation) return Fail("shim GetGPUVirtualAddress (slot 11)", E_FAIL);

    // ---- plumbing list, slots ----------------------------------------------------------------
    auto make_list = [&](void** alloc, void** list) -> intptr_t {
        intptr_t r = Call(b.dev, kDevCreateCommandAllocator, static_cast<uintptr_t>(D3D12_COMMAND_LIST_TYPE_DIRECT),
                          uintptr_t{0}, P(alloc));
        if (r != 0 || !*alloc) return r ? r : E_FAIL;
        r = Call(b.dev, kDevCreateCommandList, uintptr_t{0}, static_cast<uintptr_t>(D3D12_COMMAND_LIST_TYPE_DIRECT),
                 P(*alloc), uintptr_t{0}, uintptr_t{0}, P(list));
        if (r != 0 || !*list) return r ? r : E_FAIL;
        return Call(*list, kListClose);  // lists are created open
    };
    hr = make_list(&b.plumb_alloc, &b.plumb_list);
    if (hr != 0) return Fail("shim CreateCommandAllocator/CreateCommandList (slots 9, 12)", hr);

    const uint64_t readback_bytes = static_cast<uint64_t>(b.rt_pitch) * b.rt_h;
    b.slots.resize(rt_count);
    for (uint32_t i = 0; i < rt_count; ++i) {
        Slot& s = b.slots[i];
        hr = make_list(&s.alloc, &s.list);
        if (hr != 0) return Fail("shim CreateCommandAllocator/CreateCommandList (slots 9, 12)", hr);
        s.rt = b.CreateTexture(b.rt_w, b.rt_h, true);
        s.rtv = b.rtv_cpu0 + static_cast<uintptr_t>(i) * b.rtv_inc;
        Call(b.dev, kDevCreateRenderTargetView, P(s.rt), uintptr_t{0}, s.rtv);
        s.readback = b.CreateBuffer(D3D12_HEAP_TYPE_READBACK, readback_bytes);
    }

    // ---- textures ----------------------------------------------------------------------------
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    if (a.mode == "draws" || a.mode == "pixel" || a.mode == "tdraws" || a.mode == "tdispatch" || a.mode == "ttex") {
        // 16x16 textures filled once: `textures` with equal contents, then as many with contents
        // of their own for the binding probe. Pixel mode binds one and does not sample it.
        const uint32_t count = a.mode == "draws" ? a.textures * 2 : 1;
        const uint32_t pitch = 256;
        void* staging = b.CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, static_cast<uint64_t>(pitch) * kTexSize * count);
        uint8_t* mapped = b.Map(staging);
        b.PlumbBegin();
        for (uint32_t i = 0; i < count; ++i) {
            const bool probe = i >= a.textures && a.mode == "draws";
            const uint32_t id = probe ? 77 + (i - a.textures) : 7;
            uint8_t* base = mapped + static_cast<size_t>(i) * pitch * kTexSize;
            for (uint32_t y = 0; y < kTexSize; ++y) {
                for (uint32_t x = 0; x < kTexSize; ++x) {
                    const uint32_t v = Hash3(id, x, y);
                    std::memcpy(base + static_cast<size_t>(y) * pitch + x * 4, &v, 4);
                }
            }
            void* tex = b.CreateTexture(kTexSize, kTexSize, false);
            (probe ? b.tex_probe : b.tex_equal).push_back(tex);
            Call(b.dev, kDevCreateShaderResourceView, P(tex), P(&srv), b.SrvCpu(b.MarkerSrvSlot() + i));
            b.Barrier(b.plumb_list, tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = static_cast<ID3D12Resource*>(tex);
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = static_cast<ID3D12Resource*>(staging);
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = static_cast<uint64_t>(i) * pitch * kTexSize;
            src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            src.PlacedFootprint.Footprint.Width = kTexSize;
            src.PlacedFootprint.Footprint.Height = kTexSize;
            src.PlacedFootprint.Footprint.Depth = 1;
            src.PlacedFootprint.Footprint.RowPitch = pitch;
            Call(b.plumb_list, kListCopyTextureRegion, P(&dst), uintptr_t{0}, uintptr_t{0}, uintptr_t{0}, P(&src), uintptr_t{0});
            b.Barrier(b.plumb_list, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        b.Unmap(staging);
        b.PlumbRun();
        if (a.mode == "ttex") {
            // Textures A and B, 8x8, filled through a staging buffer; views in descriptors 1 and 2.
            const uint32_t tpitch = 256;
            void* tstaging = b.CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, static_cast<uint64_t>(tpitch) * 8 * 2);
            uint8_t* tmapped = b.Map(tstaging);
            b.PlumbBegin();
            for (uint32_t which = 0; which < 2; ++which) {
                for (uint32_t j = 0; j < 8; ++j) {
                    for (uint32_t i = 0; i < 8; ++i) {
                        const uint32_t v = TTexTexel(which, i, j);
                        std::memcpy(tmapped + static_cast<size_t>(which) * tpitch * 8 + j * tpitch + i * 4, &v, 4);
                    }
                }
                void* tex = b.CreateTexture(8, 8, false);
                b.ttex_texture[which] = tex;
                Call(b.dev, kDevCreateShaderResourceView, P(tex), P(&srv), b.SrvCpu(1 + which));
                b.Barrier(b.plumb_list, tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = static_cast<ID3D12Resource*>(tex);
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = static_cast<ID3D12Resource*>(tstaging);
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint.Offset = static_cast<uint64_t>(which) * tpitch * 8;
                src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                src.PlacedFootprint.Footprint.Width = 8;
                src.PlacedFootprint.Footprint.Height = 8;
                src.PlacedFootprint.Footprint.Depth = 1;
                src.PlacedFootprint.Footprint.RowPitch = tpitch;
                Call(b.plumb_list, kListCopyTextureRegion, P(&dst), uintptr_t{0}, uintptr_t{0}, uintptr_t{0}, P(&src), uintptr_t{0});
                b.Barrier(b.plumb_list, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            b.Unmap(tstaging);
            b.PlumbRun();
            Call(b.dev, kDevCreateShaderResourceView, P(b.ttex_texture[0]), P(&srv), b.SrvCpu(3));
        }
    } else {
        const uint32_t sz = a.upload_size;
        b.staging_pitch = (sz * 4 + 255u) & ~255u;
        b.staging_bytes = static_cast<uint64_t>(b.staging_pitch) * sz;
        b.payload.resize(kPayloads);
        for (uint32_t p = 0; p < kPayloads; ++p) {
            b.payload[p].resize(static_cast<size_t>(sz) * sz);
            for (uint32_t y = 0; y < sz; ++y) {
                for (uint32_t x = 0; x < sz; ++x) b.payload[p][static_cast<size_t>(y) * sz + x] = Hash3(p + 1000u, x, y);
            }
        }
        for (uint32_t t = 0; t < a.upload_count; ++t) {
            void* tex = b.CreateTexture(sz, sz, false);
            b.tex_stream.push_back(tex);
            Call(b.dev, kDevCreateShaderResourceView, P(tex), P(&srv), b.SrvCpu(t));
        }
        if (b.GuestMem()) {
            XG_TEXTURE2D_DESC xd{};
            xd.Width = sz;
            xd.Height = sz;
            xd.MipLevels = 1;
            xd.ArraySize = 1;
            xd.Format = 28;  // DXGI_FORMAT_R8G8B8A8_UNORM
            xd.SampleDesc.Count = 1;
            xd.BindFlags = 0x8;
            xd.SwizzleMode = XG_SWIZZLE_MODE_4KB_S;
            b.guest_layout = std::make_unique<XG_RESOURCE_LAYOUT>();
            xg::ComputeTexture2DLayout(xd, b.guest_layout.get());
            b.guest_bytes = b.guest_layout->SizeBytes;
            for (uint32_t k = 0; k < kStamps; ++k) {
                uint32_t x, y;
                b.StampPos(k, &x, &y);
                if (!xg::TexelElementOffset(*b.guest_layout, 0, 0, x, y, &b.stamp_offset[k])) {
                    return Fail("xg::TexelElementOffset", E_FAIL);
                }
            }
            b.payload_cooked.resize(kPayloads);
            for (uint32_t p = 0; p < kPayloads; ++p) {
                b.payload_cooked[p].assign(static_cast<size_t>(b.guest_bytes), 0);
                if (!xg::CopyIntoSubresource(*b.guest_layout, b.payload_cooked[p].data(), 0, 0, b.payload[p].data(), sz * 4)) {
                    return Fail("xg::CopyIntoSubresource", E_FAIL);
                }
            }
            for (uint32_t t = 0; t < a.upload_count; ++t) {
                auto* base = reinterpret_cast<uint8_t*>(Call(b.tex_stream[t], kResGetGPUVirtualAddress));
                if (!base) return Fail("shim GetGPUVirtualAddress on a texture (slot 11): no guest memory", E_FAIL);
                b.guest_base.push_back(base);
            }
        }
        for (Slot& s : b.slots) {
            if (b.PerCopy()) {
                hr = make_list(&s.copy_alloc, &s.copy_list);
                if (hr != 0) return Fail("shim CreateCommandAllocator/CreateCommandList (slots 9, 12)", hr);
            }
            if (b.Reuse()) {
                for (uint32_t t = 0; t < a.upload_count; ++t) {
                    s.staging.push_back(b.CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, b.staging_bytes));
                    s.staging_map.push_back(b.Map(s.staging.back()));  // stays mapped
                }
            }
        }
    }

    // --idle-guest-textures N: N more 64x64 textures with guest memory, written once now and
    // never again. They are not drawn. Every flush still has to look at each of them, which is
    // what this option is for measuring.
    double idle_create_ms = 0;
    if (a.idle_guest_textures) {
        const int64_t t0 = Ticks();
        for (uint32_t i = 0; i < a.idle_guest_textures; ++i) {
            void* tex = b.CreateTexture(64, 64, false);
            auto* base = reinterpret_cast<uint32_t*>(Call(tex, kResGetGPUVirtualAddress));
            if (!base) return Fail("shim GetGPUVirtualAddress on an idle texture (slot 11)", E_FAIL);
            base[0] = 0xFF000000u | i;
        }
        b.PlumbBegin();
        b.PlumbRun();  // one submission: the single write of each is uploaded here
        idle_create_ms = Us(t0, Ticks()) / 1000.0;
    }
    if (a.path == "windowed" && a.interval_us) {
        Call(b.dev, kDevSetFrameIntervalX, uintptr_t{0}, static_cast<uintptr_t>(a.interval_us), uintptr_t{2}, uintptr_t{0});
    }
    std::printf("[fps] target=%ux%u display_hz=%lu target_interval_us=%u markers=%d compile_ms=%.3f pso_create_ms=%.3f "
                "vs_bytes=%zu ps_bytes=%zu\n",
                b.rt_w, b.rt_h, static_cast<unsigned long>(display_hz), a.path == "windowed" ? a.interval_us : 0,
                a.markers ? 1 : 0, compile_ms, pso_ms, static_cast<size_t>(vs->GetBufferSize()),
                static_cast<size_t>(ps->GetBufferSize()));
    std::fflush(stdout);

    // ---- one frame of the loop ---------------------------------------------------------------
    uint64_t plane[8] = {};
    auto run_frame = [&](uint32_t frame, bool probe, void* readback, FrameSample& s, Slot* check_slot = nullptr) {
        Slot& slot = check_slot ? *check_slot : b.slots[frame % a.inflight];
        const int64_t t0 = Ticks();
        if (slot.used) b.WaitFor(slot.fence_value);  // recycle: this slot's previous frame must be done
        const int64_t t1 = Ticks();
        if (a.path == "windowed" && a.interval_us) {
            uint64_t token = 0;
            Call(b.dev, kDevWaitFrameEventX, uintptr_t{0}, static_cast<uintptr_t>(INFINITE), uintptr_t{0}, uintptr_t{0},
                 P(&token));
        }
        const int64_t t2 = Ticks();
        b.Frame(slot, frame, probe, readback, s);
        const int64_t t3 = Ticks();
        if (a.path == "windowed") {
            plane[1] = 1;
            plane[2] = reinterpret_cast<uint64_t>(slot.rt);
            s.present_unix_us = UnixMicros();
            const intptr_t phr = Call(b.queue, kQueuePresentX, uintptr_t{1}, P(plane), uintptr_t{0});
            if (phr != 0) {
                Fail("shim ID3D12CommandQueue::PresentX (slot 30)", phr);
                std::exit(2);
            }
        }
        const int64_t t4 = Ticks();
        s.wait_us = Us(t0, t1);
        s.pace_us = Us(t1, t2);
        // Preparation is the frame's CPU time that is not inside a submission (or, on the
        // serialized path, not between the first submission and the fence).
        s.prep_us = Us(t2, t3) - (a.path == "serialized" ? s.serialized_us : s.submit_us);
        s.present_tick = t3;
        s.present_us = Us(t3, t4);
        s.completed_after = b.Completed();
        return t0;
    };

    // ---- prime: every pipeline is created and drawn with before anything is timed ------------
    FrameSample first{};
    const int64_t first_start = run_frame(0, false, nullptr, first);
    b.WaitFor(b.fence_value);
    const double first_frame_ms = Us(first_start, Ticks()) / 1000.0;
    for (uint32_t f = 1; f < a.warmup + 1; ++f) {
        FrameSample s{};
        run_frame(f, false, nullptr, s);
    }
    b.WaitFor(b.fence_value);

    // ---- the timed loop ----------------------------------------------------------------------
    const uint32_t base = a.warmup + 1;
    std::vector<FrameSample> samples(a.frames);
    const uint64_t fence_before = b.fence_value;
    const uint64_t waits_before = b.fence_waits;
    D3D12XBOX_TRANSLATED_DRAW_BIND_STATS cbind0{}, cbind1{};
    D3D12XBOX_SHADER_TRANSLATION_STATS cst0{}, cst1{};
    D3D12XboxGetTranslatedDispatchBindStats(&cbind0);
    D3D12XboxGetShaderTranslationStats(&cst0);
    D3D12XBOX_TRANSLATED_DRAW_BIND_STATS bind0{}, bind1{};
    D3D12XBOX_GRAPHICS_TRANSLATION_STATS gfx0{}, gfx1{};
    D3D12XboxGetTranslatedDrawBindStats(&bind0);
    D3D12XBOX_TRANSLATED_VIEW_CHECK_STATS views0{}, views1{};
    D3D12XboxGetTranslatedViewCheckStats(&views0);
    D3D12XboxGetGraphicsTranslationStats(&gfx0);
    const unsigned long long loop_unix_us = UnixMicros();
    const int64_t loop_start = Ticks();
    for (uint32_t f = 0; f < a.frames; ++f) {
        const int64_t t0 = run_frame(base + f, false, nullptr, samples[f]);
        samples[f].t_start_us = Us(loop_start, t0);
    }
    const int64_t loop_submitted = Ticks();
    b.WaitFor(b.fence_value);  // drain
    const int64_t loop_end = Ticks();
    D3D12XboxGetTranslatedDrawBindStats(&bind1);
    D3D12XboxGetTranslatedViewCheckStats(&views1);
    D3D12XboxGetGraphicsTranslationStats(&gfx1);
    D3D12XboxGetTranslatedDispatchBindStats(&cbind1);
    D3D12XboxGetShaderTranslationStats(&cst1);
    const uint64_t fence_after = b.fence_value;
    const uint64_t completed_after = b.Completed();
    const uint64_t loop_waits = b.fence_waits - waits_before;

    // ---- checks, outside the timed loop ------------------------------------------------------
    uint64_t checked = 0, exact = 0;
    const uint64_t pixels = static_cast<uint64_t>(b.rt_w) * b.rt_h;
    // 1. Further frames through the same code, pipelined, each copied to its own buffer. They come
    //    first: on the windowed path the shim looks for texture-memory writes at the first
    //    submission after a PresentX only, and a read-back submitted in between would use that up.
    uint32_t next = base + a.frames;
    const uint32_t extra = std::min<uint32_t>(a.check_frames, 8);
    for (uint32_t i = 0; i < extra; ++i) {
        Slot& slot = b.slots[a.inflight + i];
        FrameSample s{};
        run_frame(next++, false, slot.readback, s, &slot);
    }
    // 2. Draws mode: a frame with a table change per draw over textures that differ, to show
    //    that the per-draw binding reaches the draw.
    const bool has_probe = a.mode == "draws" || (a.mode == "tdraws" && have_translated);
    if (has_probe) {
        Slot& slot = b.slots[a.inflight + extra];
        FrameSample s{};
        run_frame(next++, true, slot.readback, s, &slot);
    }
    b.WaitFor(b.fence_value);
    for (uint32_t i = 0; i < check_slots; ++i) {
        const Slot& slot = b.slots[a.inflight + i];
        const bool probe = i == extra;
        if (probe && !has_probe) continue;
        exact += b.Compare(slot.readback, slot.last_frame, probe,
                           !probe ? "checked frame"
                                  : a.mode == "draws" ? "binding probe (distinct textures)"
                                                      : "switch probe (translated and desktop pipelines, colour every second draw)");
        checked += pixels;
    }
    // 3. What the timed loop left in each in-flight slot's render target.
    b.PlumbBegin();
    for (uint32_t i = 0; i < a.inflight; ++i) b.RecordReadback(b.plumb_list, b.slots[i].rt, b.slots[i].readback);
    b.PlumbRun();
    for (uint32_t i = 0; i < a.inflight; ++i) {
        exact += b.Compare(b.slots[i].readback, b.slots[i].last_frame, false, "timed-loop tail");
        checked += pixels;
    }
    // 4. tdispatch: every buffer, after the last frame, against the kernel computed on the CPU.
    uint64_t dwords_checked = 0, dwords_exact = 0;
    if (a.mode == "tdispatch") {
        const uint64_t each = kDispatchDwords * 4;
        void* all = b.CreateBuffer(D3D12_HEAP_TYPE_READBACK, each * a.descriptors);
        b.PlumbBegin();
        for (uint32_t i = 0; i < a.descriptors; ++i) {
            b.Barrier(b.plumb_list, b.ubuf[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Call(b.plumb_list, kListCopyBufferRegion, P(all), static_cast<uintptr_t>(each * i), P(b.ubuf[i]), uintptr_t{0},
                 static_cast<uintptr_t>(each));
            b.Barrier(b.plumb_list, b.ubuf[i], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        b.PlumbRun();
        const uint8_t* data = b.Map(all);
        uint32_t buffers_written = 0;
        for (uint32_t i = 0; i < a.descriptors; ++i) {
            buffers_written += b.ubuf_written[i];
            for (uint32_t d = 0; d < kDispatchDwords; ++d) {
                uint32_t got;
                std::memcpy(&got, data + each * i + d * 4, 4);
                const uint32_t want = b.ubuf_written[i] ? DispatchKernel(d, b.ubuf_c0[i], b.c1) : 0u;
                ++dwords_checked;
                dwords_exact += got == want;
            }
        }
        b.Unmap(all);
        std::printf("[fps] check buffers: dwords exact %llu/%llu over %u buffers (%u written by a dispatch, the rest must be zero)\n",
                    static_cast<unsigned long long>(dwords_exact), static_cast<unsigned long long>(dwords_checked),
                    a.descriptors, buffers_written);
        checked += dwords_checked;
        exact += dwords_exact;
    }
    const uint64_t final_signaled = b.fence_value;
    const uint64_t final_completed = b.Completed();
    const uint32_t removed = static_cast<uint32_t>(Call(b.dev, kDevGetDeviceRemovedReason));
    const bool fences_ok = final_completed == final_signaled && completed_after == fence_after && removed == 0;
    std::printf("[fps] fences signaled=%llu completed=%llu (timed loop: %llu..%llu) waits_in_loop=%llu removed_reason=0x%08x\n",
                static_cast<unsigned long long>(final_signaled), static_cast<unsigned long long>(final_completed),
                static_cast<unsigned long long>(fence_before + 1), static_cast<unsigned long long>(fence_after),
                static_cast<unsigned long long>(loop_waits), removed);

    // ---- results -----------------------------------------------------------------------------
    FILE* csv = a.csv.empty() ? nullptr : std::fopen(a.csv.c_str(), "a");
    std::string param = a.mode == "ttex" ? "draws=" + std::to_string(a.draws) : a.mode == "tdispatch" ? "dispatches=" + std::to_string(a.draws) + ";descriptors=" + std::to_string(a.descriptors)
                        : a.mode == "draws" || a.mode == "tdraws" ? "draws=" + std::to_string(a.draws)
                        : a.mode == "pixel" ? std::to_string(b.rt_w) + "x" + std::to_string(b.rt_h)
                                            : std::to_string(a.upload_count) + "x" + std::to_string(a.upload_size);
    if (a.idle_guest_textures) param += ";idle=" + std::to_string(a.idle_guest_textures);
    auto metric = [&](const char* name, double value) {
        std::printf("[bench] %s=%.4f\n", name, value);
        if (csv) {
            std::fprintf(csv, "%s,%u,%s,%s,%s,%s,%u,%s,%.6f\n", a.series.c_str(), a.launch, a.mode.c_str(), a.variant.c_str(),
                         a.path.c_str(), param.c_str(), a.inflight, name, value);
        }
    };
    std::vector<double> interval, prep, submit, wait, present, pace, serialized, present_interval;
    double wait_total = 0, prep_total = 0, submit_total = 0;
    uint64_t draws = 0, binds = 0, submissions = 0, uploads = 0, upload_bytes = 0;
    for (uint32_t f = 0; f < a.frames; ++f) {
        const FrameSample& s = samples[f];
        if (f > 0) interval.push_back((s.t_start_us - samples[f - 1].t_start_us) / 1000.0);
        prep.push_back(s.prep_us / 1000.0);
        submit.push_back(s.submit_us / 1000.0);
        wait.push_back(s.wait_us / 1000.0);
        present.push_back(s.present_us / 1000.0);
        pace.push_back(s.pace_us / 1000.0);
        serialized.push_back(s.serialized_us / 1000.0);
        if (f > 0 && s.present_unix_us) present_interval.push_back(Us(samples[f - 1].present_tick, s.present_tick) / 1000.0);
        wait_total += s.wait_us;
        prep_total += s.prep_us;
        submit_total += s.submit_us;
        draws += s.draws;
        binds += s.table_binds;
        submissions += s.submissions;
        uploads += s.uploads;
        upload_bytes += s.upload_bytes;
    }
    const double total_ms = Us(loop_start, loop_end) / 1000.0;
    const double n = static_cast<double>(a.frames);
    metric("frames", n);
    metric("frames_in_flight", a.inflight);
    metric("total_ms", total_ms);
    metric("drain_ms", Us(loop_submitted, loop_end) / 1000.0);
    if (a.path == "serialized") {
        metric("serialized_submit_to_fence_ms_median", Percentile(serialized, 0.5));
        metric("serialized_submit_to_fence_ms_p95", Percentile(serialized, 0.95));
        metric("serialized_submit_to_fence_ms_p99", Percentile(serialized, 0.99));
        metric("serialized_frames_per_second", n * 1000.0 / total_ms);
    } else {
        metric("completed_fps", n * 1000.0 / total_ms);
    }
    metric("frame_interval_ms_median", Percentile(interval, 0.5));
    metric("frame_interval_ms_p95", Percentile(interval, 0.95));
    metric("frame_interval_ms_p99", Percentile(interval, 0.99));
    metric("cpu_prep_ms_median", Percentile(prep, 0.5));
    metric("cpu_prep_ms_p95", Percentile(prep, 0.95));
    metric("cpu_prep_ms_p99", Percentile(prep, 0.99));
    metric("submit_ms_median", Percentile(submit, 0.5));
    metric("submit_ms_p95", Percentile(submit, 0.95));
    metric("submit_ms_p99", Percentile(submit, 0.99));
    metric("fence_wait_ms_median", Percentile(wait, 0.5));
    metric("fence_wait_ms_p95", Percentile(wait, 0.95));
    metric("fence_wait_ms_p99", Percentile(wait, 0.99));
    metric("fence_wait_ms_total", wait_total / 1000.0 + Us(loop_submitted, loop_end) / 1000.0);
    metric("fence_waits_in_loop", static_cast<double>(loop_waits));
    metric("cpu_prep_ms_total", prep_total / 1000.0);
    metric("submit_ms_total", submit_total / 1000.0);
    metric("draws_per_frame", static_cast<double>(draws) / n);
    metric("table_binds_per_frame", static_cast<double>(binds) / n);
    if (a.mode == "tdispatch") {
        uint64_t csets = 0;
        for (const FrameSample& fs : samples) csets += fs.constant_sets;
        metric("dispatches_per_frame", static_cast<double>(draws) / n);
        metric("constant_sets_per_frame", static_cast<double>(csets) / n);
        metric("translated_dispatches_per_frame", static_cast<double>(cst1.dispatches_translated - cst0.dispatches_translated) / n);
        metric("translated_dispatches_skipped", static_cast<double>(cst1.dispatches_skipped - cst0.dispatches_skipped));
        metric("host_root_binds_per_frame", static_cast<double>(cbind1.host_root_binds - cbind0.host_root_binds) / n);
        metric("user_data_sets_per_frame", static_cast<double>(cbind1.user_data_sets - cbind0.user_data_sets) / n);
        metric("title_root_restores_per_frame", static_cast<double>(cbind1.title_root_restores - cbind0.title_root_restores) / n);
        metric("title_arguments_replayed_per_frame",
               static_cast<double>(cbind1.title_arguments_replayed - cbind0.title_arguments_replayed) / n);
        metric("descriptors", a.descriptors);
        metric("checked_dwords", static_cast<double>(dwords_checked));
        metric("exact_dwords", static_cast<double>(dwords_exact));
        metric("translated_pso_create_ms", compute_pso_ms);
    }
    if (a.mode == "ttex") {
        metric("descriptor_writes_per_frame", static_cast<double>(uploads) / n);
        metric("cpu_prep_ns_per_draw", prep_total * 1000.0 / static_cast<double>(draws ? draws : 1));
    }
    if (a.mode == "tdraws" || a.mode == "ttex") {
        uint64_t csets = 0;
        for (const FrameSample& fs : samples) csets += fs.constant_sets;
        metric("constant_sets_per_frame", static_cast<double>(csets) / n);
        metric("translated_draws_per_frame", static_cast<double>(gfx1.draws_translated - gfx0.draws_translated) / n);
        metric("translated_draws_skipped", static_cast<double>(gfx1.draws_skipped - gfx0.draws_skipped));
        metric("host_root_binds_per_frame", static_cast<double>(bind1.host_root_binds - bind0.host_root_binds) / n);
        metric("user_data_sets_per_frame", static_cast<double>(bind1.user_data_sets - bind0.user_data_sets) / n);
        metric("title_root_restores_per_frame", static_cast<double>(bind1.title_root_restores - bind0.title_root_restores) / n);
        metric("title_arguments_replayed_per_frame",
               static_cast<double>(bind1.title_arguments_replayed - bind0.title_arguments_replayed) / n);
        metric("translated_pso_create_ms", translated_pso_ms);
        // How often the shim looked at the descriptors the draws' tables name.
        metric("bindings_built_per_frame", static_cast<double>(views1.bindings_built - views0.bindings_built) / n);
        metric("views_validated_per_frame", static_cast<double>(views1.views_validated - views0.views_validated) / n);
        metric("slot_rechecks_per_frame", static_cast<double>(views1.slot_rechecks - views0.slot_rechecks) / n);
    }
    metric("submissions_per_frame", static_cast<double>(submissions) / n);
    metric("uploads_per_frame", static_cast<double>(uploads) / n);
    metric("upload_bytes_per_frame", static_cast<double>(upload_bytes) / n);
    if (a.path == "windowed") {
        metric("target_interval_us", a.interval_us);
        metric("display_hz_reported_to_guest", static_cast<double>(display_hz));
        metric("present_call_interval_ms_median", Percentile(present_interval, 0.5));
        metric("present_call_interval_ms_p95", Percentile(present_interval, 0.95));
        metric("present_call_interval_ms_p99", Percentile(present_interval, 0.99));
        metric("present_call_ms_median", Percentile(present, 0.5));
        metric("present_call_ms_p95", Percentile(present, 0.95));
        metric("present_call_ms_p99", Percentile(present, 0.99));
        metric("pace_wait_ms_median", Percentile(pace, 0.5));
        metric("presented_fps", n * 1000.0 / (Us(loop_start, loop_submitted) / 1000.0));
    }
    metric("idle_guest_textures", a.idle_guest_textures);
    metric("idle_guest_textures_create_ms", idle_create_ms);
    metric("first_frame_ms", first_frame_ms);
    metric("pso_create_ms", pso_ms);
    metric("shader_compile_ms", compile_ms);
    metric("checked_pixels", static_cast<double>(checked));
    metric("exact_pixels", static_cast<double>(exact));
    metric("fences_signaled", static_cast<double>(final_signaled));
    metric("fences_completed", static_cast<double>(final_completed));
    if (csv) std::fclose(csv);

    if (!a.frames_csv.empty()) {
        if (FILE* fc = std::fopen(a.frames_csv.c_str(), "a")) {
            for (uint32_t f = 0; f < a.frames; ++f) {
                const FrameSample& s = samples[f];
                std::fprintf(fc, "%s,%u,%s,%s,%s,%s,%u,%u,%u,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%llu,%llu,%u,%u,%u,%u,%llu\n",
                             a.series.c_str(), a.launch, a.mode.c_str(), a.variant.c_str(), a.path.c_str(), param.c_str(),
                             a.inflight, f, base + f, s.t_start_us, s.wait_us, s.pace_us, s.prep_us, s.submit_us,
                             s.present_us, s.serialized_us, s.present_unix_us,
                             static_cast<unsigned long long>(s.completed_after), s.draws, s.table_binds, s.submissions,
                             s.uploads, static_cast<unsigned long long>(s.upload_bytes));
            }
            std::fclose(fc);
        }
    }
    std::printf("[fps] timed loop started unix_us=%llu frames %u..%u\n", loop_unix_us, base, base + a.frames - 1);
    std::fflush(stdout);
    if (a.hold_ms) Sleep(a.hold_ms);

    const bool pass = exact == checked && fences_ok;
    std::printf("[fps] %s checked_pixels=%llu exact_pixels=%llu fences=%s\n", pass ? "PASS" : "FAIL",
                static_cast<unsigned long long>(checked), static_cast<unsigned long long>(exact), fences_ok ? "ok" : "BAD");
    std::fflush(stdout);
    // The window (windowed path) goes away with the queue.
    Call(b.queue, kRelease);
    return pass ? 0 : 1;
}
