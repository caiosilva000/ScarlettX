#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx {

struct VictoryShaderInfo {
    int32_t container_status = 1;
    int32_t native_status = 1;
    uint32_t part_count = 0;
    bool has_dxil = false;
    bool has_xdat = false;
    bool has_xbop = false;
    uint32_t native_program_count = 0;
    uint32_t stage_mask = 0;
    uint32_t wave64_mask = 0;
    std::string message;
};

// Pinned bridge to third_party/victory-belobug-amd (GPL-3.0-or-later). The
// license and source provenance are documented in THIRD_PARTY.md.
Status InspectVictoryShader(const std::vector<uint8_t>& bytes, VictoryShaderInfo& out_info);
Status TranslateVictoryShader(const std::vector<uint8_t>& bytes, VictoryShaderInfo& out_info,
                              std::vector<uint8_t>& out_dxil);

// One user-data dword a translated shader reads. root: 0 root constants, 1 descriptor table,
// 2 root descriptor, 3 immediate sampler, 4 resource heap, 5 sampler heap, 255 other.
struct VictoryUserData {
    uint32_t index = 0;  // dword in the user-data buffer (b0, space 0)
    uint32_t slot = 0;   // root parameter index in the title's root signature
    uint32_t root = 0;
    uint32_t dword = 0;  // dword within that parameter
};

// What the host binds for a translated compute shader. The DXIL alone is not enough to build a
// pipeline: the root signature and the descriptor heap layout come from here.
struct VictoryComputeBinding {
    uint32_t user_data_dwords = 0;  // size of b0, a multiple of 4
    std::vector<VictoryUserData> user_data;
    uint32_t root_cbv_count = 0;
    uint32_t root_srv_count = 0;
    uint32_t root_uav_count = 0;
    uint32_t sampler_count = 0;
    uint32_t static_sampler_count = 0;
    // bit 0 title memory, 1 memory writes, 2 view addresses, 3 computed heap indices,
    // 4 heap writes, 5 view marks: each needs host tables a plain dispatch does not have.
    uint32_t flags = 0;
    uint32_t threads[3] = {0, 0, 0};
};

// Translates the bridge's built-in synthetic GFX10 compute fixture (hand-assembled, no title
// data): one thread writes ((c0 + c1) ^ c2) + c3 to dword 0 of the raw UAV at heap index 0.
Status TranslateVictorySyntheticCompute(VictoryShaderInfo& out_info, VictoryComputeBinding& out_binding,
                                        std::vector<uint8_t>& out_dxil);

// Translates the bridge's synthetic vertex and pixel programs (hand-assembled, no title data).
// The vertex program makes a triangle covering the viewport from the vertex id; the pixel
// program writes its four root constants (float bits) to render target 0. The bindings are
// per stage, in the same form as a compute shader's.
Status TranslateVictorySyntheticDraw(VictoryShaderInfo& out_info, VictoryComputeBinding& out_vs_binding,
                                     std::vector<uint8_t>& out_vs_dxil, VictoryComputeBinding& out_ps_binding,
                                     std::vector<uint8_t>& out_ps_dxil);

// What a shader container is, for a compute pipeline.
enum class VictoryComputeOutcome {
    Translated,       // console-shaped with a native compute program: out_dxil and out_binding are set
    Desktop,          // no console index: an ordinary host shader, to be used as it is
    NoNativeCompute,  // console-shaped, but it carries no native compute program
    Malformed,        // console-shaped, its native data does not parse
    Refused,          // the translator does not handle this program
    Unavailable,      // the bridge was not enabled at configure time
};
const char* VictoryComputeOutcomeName(VictoryComputeOutcome outcome) noexcept;

// Looks at a shader container as a title passes it to compute pipeline creation and, when it
// carries native code, translates it. The reason for NoNativeCompute, Malformed and Refused is
// in out_info.message.
VictoryComputeOutcome TranslateVictoryCompute(const uint8_t* data, size_t size, VictoryShaderInfo& out_info,
                                              VictoryComputeBinding& out_binding,
                                              std::vector<uint8_t>& out_dxil);

// The same for one stage of a graphics pipeline. NoNativeCompute then means the container is
// console-shaped but carries no native program of that stage.
enum class VictoryStage : uint32_t { Vertex = 0, Pixel = 1, Compute = 2 };
VictoryComputeOutcome TranslateVictoryStage(const uint8_t* data, size_t size, VictoryStage stage,
                                            VictoryShaderInfo& out_info, VictoryComputeBinding& out_binding,
                                            std::vector<uint8_t>& out_dxil);

// Whether the first graphics route can bind this stage: its user data fed from the title's root
// constants only. `why` names the first thing it cannot do.
bool VictoryGraphicsBindingSupported(const VictoryComputeBinding& binding, std::string& why);

// Whether the first host route can bind this shader: user data as root constants at b0 fed from
// the title's root constants and descriptor tables, views taken from the title's heap by index.
// `why` names the first thing it cannot do.
bool VictoryComputeBindingSupported(const VictoryComputeBinding& binding, std::string& why);

// The wave lane range a DXIL container says it expects: MinimumExpectedWaveLaneCount and
// MaximumExpectedWaveLaneCount of the runtime info in its PSV0 part. A shader that does not depend
// on the wave size says 0 and 0xFFFFFFFF. False when the container has no readable PSV0 part.
bool DxilExpectedWaveLanes(const uint8_t* data, size_t size, uint32_t& out_min, uint32_t& out_max) noexcept;

// Whether an adapter that runs waves of adapter_min to adapter_max lanes can run a shader that
// expects expected_min to expected_max, as D3D12 specifies for a declared wave size. `why` says
// what is missing. A shader without a requirement (0 to 0xFFFFFFFF) runs anywhere.
bool WaveLanesSupported(uint32_t expected_min, uint32_t expected_max, bool adapter_wave_ops, uint32_t adapter_min,
                        uint32_t adapter_max, std::string& why);

// Test fixtures: console-shaped containers written by the bridge, no title data.
//   0 the synthetic compute fixture   1 the same, native stream cut short (malformed)
//   2 console index without native code   3 a native program the translator refuses
//   4 to 7 the compute conformance programs
//   9 the draw fixture's vertex program   10 its pixel program, each with its signature parts
//   11 the vertex program, native stream cut short   12 a pixel program that also stores to a
//   descriptor table's UAV (bindings beyond root constants)
//   13 a lane exchange in a wave of 32 (ds_bpermute_b32: the wave reversed, the halves
//   exchanged)   14 the same in a wave of 64
//   15 the quad-read fixture's vertex program (a triangle with three diagonal edges)
//   16 its pixel program (derivatives and quad reads of a computed value, by DPP quad_perm)
Status BuildVictorySyntheticContainer(uint32_t variant, std::vector<uint8_t>& out_bytes);

}  // namespace scarlettx
