// Quad reads at triangle edges, synthetic only: a translated pixel program that reads its quad
// neighbours, against a desktop shader and against values calculated on the CPU.
//
// The bridge's quad-read fixture: a vertex program that makes the triangle (3.25, 3.25),
// (59.25, 6.25), (11.25, 60.25) in pixels of a 64x64 target (three diagonal edges that cross
// pixel quads; no pixel centre lies on an edge), and a pixel program that computes w = x * y
// from the pixel position and reads w of other lanes of its quad with DPP quad_perm, as the
// console compiler does for derivatives of computed values:
//
//   red    w(right pixel of the row) - w(left pixel)       ddx_fine(w)   = y
//   green  w(lower pixel of the column) - w(upper pixel)   ddy_fine(w)   = x
//   blue   w of the pixel diagonally opposite in the quad
//   alpha  w of the quad's first (upper left) pixel
//
// In a quad an edge crosses, some of those lanes are outside the triangle. The console runs them
// as helpers; a translation that reads them with an operation the host leaves helpers out of
// gets a wrong value there, along every triangle edge (VIctory 4015a18 is the fix).
//
// Three pictures of the same triangle into an R32G32B32A32_FLOAT target, every pixel read back:
//   desktop     HLSL (vs_5_0, ps_5_0): ddx_fine and ddy_fine of the same w; the two quad reads
//               written out from the pixel's position in its quad
//   translated  the fixture, translated by the bridge
//   cpu         the values above, and the coverage from the triangle's edge functions
// All arithmetic involved is exact in single precision, so the comparison is bit for bit.
//
// Reported: coverage, and the pixels where translated and desktop differ, split into edge pixels
// (a quad neighbour is outside the triangle) and interior pixels, per channel.
//
// Exit codes: 0 translated and desktop agree on every pixel and the coverage is the CPU's,
// 1 they differ, 2 a D3D12 call failed, 3 no device, 4 the fixture did not build or translate,
// 5 the shaders ask for bindings this test does not make.
#include "sx_check.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include "scarlettx/gpu/d3d12_backend.h"
#include "scarlettx/gpu/victory_shader_bridge.h"

namespace {

constexpr uint32_t kSize = 64;
constexpr float kClear[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
// The triangle in quarter pixels (the bridge's EDGE_TRIANGLE_QUARTER_PIXELS).
constexpr int kTriangle[3][2] = {{13, 13}, {237, 25}, {45, 241}};
const char* const kChannel[4] = {"ddx", "ddy", "diagonal", "first"};

const char* kDesktopHlsl = R"(
float4 VSMain(uint vid : SV_VertexID) : SV_POSITION {
    float a = float(vid & 1);
    float b = float(vid & 2);
    return float4(-0.8984375f + 1.75f * a + 0.125f * b, 0.8984375f - 0.09375f * a - 0.890625f * b, 0.0f, 1.0f);
}
float4 PSMain(float4 pos : SV_POSITION) : SV_TARGET {
    float w = pos.x * pos.y;
    float px = fmod(floor(pos.x), 2.0f);  // where the pixel is in its quad
    float py = fmod(floor(pos.y), 2.0f);
    float ox = pos.x + 1.0f - 2.0f * px;  // the pixel diagonally opposite
    float oy = pos.y + 1.0f - 2.0f * py;
    return float4(ddx_fine(w), ddy_fine(w), ox * oy, (pos.x - px) * (pos.y - py));
}
)";

struct Gpu {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    UINT64 fence_value = 0;
    ID3D12RootSignature* root = nullptr;
};

int Fail(const char* api, HRESULT hr) {
    std::printf("[victory-quad] FAILED api=%s hr=0x%08x\n", api, static_cast<uint32_t>(hr));
    return 2;
}

// One draw of the triangle with the given shaders. 0 and the whole target, or an exit code.
int Draw(Gpu& gpu, const char* label, const std::vector<uint8_t>& vs, const std::vector<uint8_t>& ps,
         std::vector<float>& out) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = gpu.root;
    pso_desc.VS = {vs.data(), vs.size()};
    pso_desc.PS = {ps.data(), ps.size()};
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = 0xFFFFFFFFu;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pso_desc.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = gpu.device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso));
    std::printf("[victory-quad] %s pipeline hr=0x%08x vs=%zu bytes ps=%zu bytes\n", label, static_cast<uint32_t>(hr),
                vs.size(), ps.size());
    if (FAILED(hr)) return Fail("ID3D12Device::CreateGraphicsPipelineState", hr);

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.NumDescriptors = 1;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    hr = gpu.device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap));
    if (FAILED(hr)) return Fail("ID3D12Device::CreateDescriptorHeap(RTV)", hr);

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rt_desc{};
    rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt_desc.Width = kSize;
    rt_desc.Height = kSize;
    rt_desc.DepthOrArraySize = 1;
    rt_desc.MipLevels = 1;
    rt_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    rt_desc.SampleDesc.Count = 1;
    rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear_val{};
    clear_val.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    std::memcpy(clear_val.Color, kClear, sizeof(kClear));
    ID3D12Resource* target = nullptr;
    hr = gpu.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_val, IID_PPV_ARGS(&target));
    if (FAILED(hr)) return Fail("ID3D12Device::CreateCommittedResource(RT)", hr);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    gpu.device->CreateRenderTargetView(target, nullptr, rtv);

    const UINT row_pitch = (kSize * 16 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const UINT64 readback_size = static_cast<UINT64>(row_pitch) * kSize;
    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb_desc{};
    rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb_desc.Width = readback_size;
    rb_desc.Height = 1;
    rb_desc.DepthOrArraySize = 1;
    rb_desc.MipLevels = 1;
    rb_desc.SampleDesc.Count = 1;
    rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* readback = nullptr;
    hr = gpu.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(hr)) return Fail("ID3D12Device::CreateCommittedResource(Readback)", hr);

    hr = gpu.allocator->Reset();
    if (FAILED(hr)) return Fail("ID3D12CommandAllocator::Reset", hr);
    hr = gpu.list->Reset(gpu.allocator, pso);
    if (FAILED(hr)) return Fail("ID3D12GraphicsCommandList::Reset", hr);
    gpu.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    gpu.list->ClearRenderTargetView(rtv, kClear, 0, nullptr);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(kSize), static_cast<LONG>(kSize)};
    gpu.list->SetGraphicsRootSignature(gpu.root);
    const uint32_t unused = 0;
    gpu.list->SetGraphicsRoot32BitConstants(0, 1, &unused, 0);
    gpu.list->SetGraphicsRoot32BitConstants(1, 1, &unused, 0);
    gpu.list->RSSetViewports(1, &viewport);
    gpu.list->RSSetScissorRects(1, &scissor);
    gpu.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu.list->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = target;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    gpu.list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = target;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kSize;
    dst.PlacedFootprint.Footprint.Height = kSize;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = row_pitch;
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    hr = gpu.list->Close();
    if (FAILED(hr)) return Fail("ID3D12GraphicsCommandList::Close", hr);

    ID3D12CommandList* lists[] = {gpu.list};
    gpu.queue->ExecuteCommandLists(1, lists);
    const UINT64 wanted = ++gpu.fence_value;
    hr = gpu.queue->Signal(gpu.fence, wanted);
    if (FAILED(hr)) return Fail("ID3D12CommandQueue::Signal", hr);
    DWORD wait = 0;
    if (gpu.fence->GetCompletedValue() < wanted) {
        hr = gpu.fence->SetEventOnCompletion(wanted, gpu.event);
        if (FAILED(hr)) return Fail("ID3D12Fence::SetEventOnCompletion", hr);
        wait = WaitForSingleObject(gpu.event, 20000);
    }
    const UINT64 completed = gpu.fence->GetCompletedValue();
    std::printf("[victory-quad] %s fence signaled=%llu completed=%llu wait_result=%lu\n", label,
                static_cast<unsigned long long>(wanted), static_cast<unsigned long long>(completed),
                static_cast<unsigned long>(wait));
    if (completed < wanted) return Fail("ID3D12Fence::GetCompletedValue (fence not reached)", gpu.device->GetDeviceRemovedReason());

    void* mapped = nullptr;
    hr = readback->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped) return Fail("ID3D12Resource::Map(Readback)", FAILED(hr) ? hr : E_POINTER);
    out.resize(static_cast<size_t>(kSize) * kSize * 4);
    for (uint32_t y = 0; y < kSize; ++y) {
        std::memcpy(out.data() + static_cast<size_t>(y) * kSize * 4, static_cast<const uint8_t*>(mapped) + static_cast<size_t>(y) * row_pitch,
                    static_cast<size_t>(kSize) * 16);
    }
    readback->Unmap(0, nullptr);
    readback->Release();
    target->Release();
    rtv_heap->Release();
    pso->Release();
    return 0;
}

// Whether the centre of pixel (i, j) is inside the triangle: the three edge functions, in
// quarter pixels, all of one sign. None is ever zero (see the fixture).
bool Covered(int i, int j) {
    const int px = 4 * i + 2;
    const int py = 4 * j + 2;
    int positive = 0;
    int negative = 0;
    for (int e = 0; e < 3; ++e) {
        const int* a = kTriangle[e];
        const int* b = kTriangle[(e + 1) % 3];
        const long long v = static_cast<long long>(px - a[0]) * (b[1] - a[1]) - static_cast<long long>(py - a[1]) * (b[0] - a[0]);
        SX_REQUIRE(v != 0);
        if (v > 0) ++positive; else ++negative;
    }
    return positive == 3 || negative == 3;
}

bool InTarget(int i, int j) { return i >= 0 && j >= 0 && i < static_cast<int>(kSize) && j < static_cast<int>(kSize); }

struct Tally {
    uint32_t pixels = 0;        // pixels with any channel different
    uint32_t edge = 0;          // of those, edge pixels
    uint32_t interior = 0;
    uint32_t channel[4] = {};   // values different, per channel
    uint32_t helper_reads = 0;  // values different where the read reaches a lane outside the triangle
    uint32_t other_reads = 0;   // values different elsewhere
};

bool Same(float a, float b) { return std::memcmp(&a, &b, 4) == 0; }

}  // namespace

int main(int argc, char** argv) {
    bool prefer_warp = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--warp") prefer_warp = true;
        else {
            std::fprintf(stderr, "usage: test_victory_quad_edge [--warp]\n");
            return 64;
        }
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // The fixture, translated.
    std::vector<uint8_t> shaders[2];
    const scarlettx::VictoryStage stages[2] = {scarlettx::VictoryStage::Vertex, scarlettx::VictoryStage::Pixel};
    for (int s = 0; s < 2; ++s) {
        std::vector<uint8_t> container;
        if (!scarlettx::BuildVictorySyntheticContainer(15 + s, container).ok()) {
            std::printf("[victory-quad] the fixture could not be built (variant %d)\n", 15 + s);
            return 4;
        }
        scarlettx::VictoryShaderInfo info;
        scarlettx::VictoryComputeBinding binding;
        const auto outcome =
            scarlettx::TranslateVictoryStage(container.data(), container.size(), stages[s], info, binding, shaders[s]);
        std::printf("[victory-quad] %s program: container=%zu bytes outcome=%s dxil=%zu bytes user_data_dwords=%u%s%s\n",
                    s == 0 ? "vertex" : "pixel", container.size(), scarlettx::VictoryComputeOutcomeName(outcome),
                    shaders[s].size(), binding.user_data_dwords, info.message.empty() ? "" : " message=",
                    info.message.c_str());
        if (outcome != scarlettx::VictoryComputeOutcome::Translated) return 4;
        if (binding.user_data_dwords || binding.root_cbv_count || binding.root_srv_count || binding.root_uav_count ||
            binding.sampler_count || binding.static_sampler_count || binding.flags) {
            std::printf("[victory-quad] FAIL: the %s program asks for bindings\n", s == 0 ? "vertex" : "pixel");
            return 5;
        }
    }

    scarlettx::D3D12Backend backend;
    const auto init = backend.Initialize(prefer_warp, false);
    if (!init.ok()) {
        std::printf("[victory-quad] no device: %s\n", init.message.c_str());
        return 3;
    }
    const auto& adapter = backend.AdapterInfo();
    std::printf("[victory-quad] adapter=%s vendor=0x%04x device=0x%04x warp=%d\n", adapter.description.c_str(),
                adapter.vendor_id, adapter.device_id, adapter.is_warp ? 1 : 0);

    Gpu gpu;
    gpu.device = static_cast<ID3D12Device*>(backend.NativeDevice());
    SX_REQUIRE(gpu.device != nullptr);
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT hr = gpu.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&gpu.queue));
    if (FAILED(hr)) return Fail("ID3D12Device::CreateCommandQueue", hr);
    hr = gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator));
    if (FAILED(hr)) return Fail("ID3D12Device::CreateCommandAllocator", hr);
    hr = gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator, nullptr, IID_PPV_ARGS(&gpu.list));
    if (FAILED(hr)) return Fail("ID3D12Device::CreateCommandList", hr);
    gpu.list->Close();
    hr = gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence));
    if (FAILED(hr)) return Fail("ID3D12Device::CreateFence", hr);
    gpu.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // One root signature for both pipelines: a dword of root constants per stage at b0, which
    // neither the translated shaders nor the desktop shaders read.
    D3D12_ROOT_PARAMETER params[2]{};
    const D3D12_SHADER_VISIBILITY visibility[2] = {D3D12_SHADER_VISIBILITY_VERTEX, D3D12_SHADER_VISIBILITY_PIXEL};
    for (int i = 0; i < 2; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[i].Constants.Num32BitValues = 1;
        params[i].ShaderVisibility = visibility[i];
    }
    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 2;
    rs_desc.pParameters = params;
    rs_desc.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                                                            D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                                                            D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);
    ID3DBlob* sig = nullptr;
    ID3DBlob* err = nullptr;
    hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) return Fail("D3D12SerializeRootSignature", hr);
    hr = gpu.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&gpu.root));
    if (FAILED(hr)) return Fail("ID3D12Device::CreateRootSignature", hr);

    // The desktop reference.
    std::vector<uint8_t> desktop[2];
    const char* const entries[2] = {"VSMain", "PSMain"};
    const char* const targets[2] = {"vs_5_0", "ps_5_0"};
    for (int s = 0; s < 2; ++s) {
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        hr = D3DCompile(kDesktopHlsl, std::strlen(kDesktopHlsl), "quad_edge.hlsl", nullptr, nullptr, entries[s], targets[s],
                        D3DCOMPILE_IEEE_STRICTNESS, 0, &code, &errors);
        if (FAILED(hr)) {
            if (errors) std::printf("[victory-quad] %s\n", static_cast<const char*>(errors->GetBufferPointer()));
            return Fail(s == 0 ? "D3DCompile(VSMain)" : "D3DCompile(PSMain)", hr);
        }
        const auto* bytes = static_cast<const uint8_t*>(code->GetBufferPointer());
        desktop[s].assign(bytes, bytes + code->GetBufferSize());
        code->Release();
    }

    std::vector<float> ref;
    std::vector<float> got;
    if (const int rc = Draw(gpu, "desktop", desktop[0], desktop[1], ref)) return rc;
    if (const int rc = Draw(gpu, "translated", shaders[0], shaders[1], got)) return rc;

    // The CPU's picture.
    std::vector<uint8_t> covered(static_cast<size_t>(kSize) * kSize);
    std::vector<float> cpu(static_cast<size_t>(kSize) * kSize * 4);
    uint32_t covered_count = 0;
    for (int j = 0; j < static_cast<int>(kSize); ++j) {
        for (int i = 0; i < static_cast<int>(kSize); ++i) {
            const size_t at = static_cast<size_t>(j) * kSize + i;
            covered[at] = Covered(i, j) ? 1 : 0;
            float* p = &cpu[at * 4];
            if (!covered[at]) {
                std::memcpy(p, kClear, sizeof(kClear));
                continue;
            }
            ++covered_count;
            const float x = static_cast<float>(i) + 0.5f;
            const float y = static_cast<float>(j) + 0.5f;
            const float left = static_cast<float>(i & ~1) + 0.5f;
            const float top = static_cast<float>(j & ~1) + 0.5f;
            p[0] = (left + 1.0f) * y - left * y;
            p[1] = x * (top + 1.0f) - x * top;
            p[2] = (x + ((i & 1) ? -1.0f : 1.0f)) * (y + ((j & 1) ? -1.0f : 1.0f));
            p[3] = left * top;
        }
    }
    auto is_covered = [&](int i, int j) { return InTarget(i, j) && covered[static_cast<size_t>(j) * kSize + i]; };

    // Coverage as drawn: a pixel the triangle did not touch still holds the clear value.
    auto drawn_coverage = [&](const std::vector<float>& image, uint32_t& drawn, uint32_t& wrong) {
        drawn = 0;
        wrong = 0;
        for (size_t at = 0; at < covered.size(); ++at) {
            const bool is_drawn = std::memcmp(&image[at * 4], kClear, sizeof(kClear)) != 0;
            if (is_drawn) ++drawn;
            if (is_drawn != (covered[at] != 0)) ++wrong;
        }
    };
    uint32_t ref_drawn = 0, ref_wrong = 0, got_drawn = 0, got_wrong = 0;
    drawn_coverage(ref, ref_drawn, ref_wrong);
    drawn_coverage(got, got_drawn, got_wrong);
    std::printf("[victory-quad] coverage: cpu=%u desktop=%u (differs at %u) translated=%u (differs at %u) of %u pixels\n",
                covered_count, ref_drawn, ref_wrong, got_drawn, got_wrong, kSize * kSize);

    // Edge pixels, and which reads reach a lane outside the triangle.
    uint32_t edge_pixels = 0;
    uint32_t helper_reads_total = 0;
    uint32_t helper_reads_channel[4] = {};
    auto outside = [&](int i, int j, bool reads[4]) {
        reads[0] = !is_covered(i ^ 1, j);
        reads[1] = !is_covered(i, j ^ 1);
        reads[2] = !is_covered(i ^ 1, j ^ 1);
        reads[3] = !is_covered(i & ~1, j & ~1);
        return reads[0] || reads[1] || reads[2] || reads[3];
    };
    for (int j = 0; j < static_cast<int>(kSize); ++j) {
        for (int i = 0; i < static_cast<int>(kSize); ++i) {
            if (!is_covered(i, j)) continue;
            bool reads[4];
            if (outside(i, j, reads)) ++edge_pixels;
            for (int c = 0; c < 4; ++c) {
                if (reads[c]) {
                    ++helper_reads_total;
                    ++helper_reads_channel[c];
                }
            }
        }
    }
    std::printf("[victory-quad] pixels: covered=%u edge=%u interior=%u; reads of a lane outside the triangle: %u "
                "(ddx %u, ddy %u, diagonal %u, first %u)\n",
                covered_count, edge_pixels, covered_count - edge_pixels, helper_reads_total, helper_reads_channel[0],
                helper_reads_channel[1], helper_reads_channel[2], helper_reads_channel[3]);

    auto compare = [&](const char* name, const std::vector<float>& a, const char* a_name, const std::vector<float>& b,
                       const char* b_name) {
        Tally t;
        int shown = 0;
        for (int j = 0; j < static_cast<int>(kSize); ++j) {
            for (int i = 0; i < static_cast<int>(kSize); ++i) {
                if (!is_covered(i, j)) continue;
                const size_t at = (static_cast<size_t>(j) * kSize + i) * 4;
                bool reads[4];
                const bool edge = outside(i, j, reads);
                bool any = false;
                for (int c = 0; c < 4; ++c) {
                    if (Same(a[at + c], b[at + c])) continue;
                    any = true;
                    ++t.channel[c];
                    if (reads[c]) ++t.helper_reads; else ++t.other_reads;
                    if (shown < 6) {
                        ++shown;
                        std::printf("[victory-quad] %s pixel (%d,%d) quad lane %d %s: %s=%g %s=%g cpu=%g (%s)\n", name, i, j,
                                    (i & 1) | ((j & 1) << 1), kChannel[c], a_name, static_cast<double>(a[at + c]), b_name,
                                    static_cast<double>(b[at + c]), static_cast<double>(cpu[at + c]),
                                    reads[c] ? "reads a lane outside the triangle" : "reads lanes inside the triangle");
                    }
                }
                if (any) {
                    ++t.pixels;
                    if (edge) ++t.edge; else ++t.interior;
                }
            }
        }
        std::printf("[victory-quad] %s: differing_pixels=%u edge=%u interior=%u values ddx=%u ddy=%u diagonal=%u first=%u "
                    "helper_reads_wrong=%u/%u other_reads_wrong=%u\n",
                    name, t.pixels, t.edge, t.interior, t.channel[0], t.channel[1], t.channel[2], t.channel[3],
                    t.helper_reads, helper_reads_total, t.other_reads);
        return t;
    };
    const Tally desktop_cpu = compare("desktop_vs_cpu", ref, "desktop", cpu, "cpu");
    const Tally translated_desktop = compare("translated_vs_desktop", got, "translated", ref, "desktop");
    const Tally translated_cpu = compare("translated_vs_cpu", got, "translated", cpu, "cpu");
    (void)desktop_cpu;
    (void)translated_cpu;

    const bool pass = ref_wrong == 0 && got_wrong == 0 && translated_desktop.pixels == 0;
    std::printf("[victory-quad] %s (edge pixels differing: %u of %u)\n", pass ? "PASS" : "FAIL", translated_desktop.edge,
                edge_pixels);
    return pass ? 0 : 1;
}
