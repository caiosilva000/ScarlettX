#include "scarlettx/gpu/victory_shader_bridge.h"

#include <cstring>

namespace {

constexpr size_t kMessageCapacity = 192;

#if defined(SCARLETTX_VICTORY_SHADER_BRIDGE)
extern "C" {
struct SxVictoryShaderInfo {
    int32_t container_status;
    int32_t native_status;
    uint32_t part_count;
    uint32_t has_dxil;
    uint32_t has_xdat;
    uint32_t has_xbop;
    uint32_t native_program_count;
    uint32_t stage_mask;
    uint32_t wave64_mask;
    char message[kMessageCapacity];
};
struct SxVictoryByteBuffer {
    uint8_t* data;
    size_t size;
};
int32_t sx_victory_shader_inspect(const uint8_t*, size_t, SxVictoryShaderInfo*);
int32_t sx_victory_shader_translate(const uint8_t*, size_t, SxVictoryByteBuffer*, SxVictoryShaderInfo*);
void sx_victory_buffer_free(SxVictoryByteBuffer*);

constexpr size_t kUserDataCapacity = 16;
struct SxVictoryUserData {
    uint32_t index;
    uint32_t slot;
    uint32_t root;
    uint32_t dword;
};
struct SxVictoryComputeBinding {
    uint32_t user_data_dwords;
    uint32_t user_data_count;
    SxVictoryUserData user_data[kUserDataCapacity];
    uint32_t root_cbv_count;
    uint32_t root_srv_count;
    uint32_t root_uav_count;
    uint32_t sampler_count;
    uint32_t static_sampler_count;
    uint32_t flags;
    uint32_t threads[3];
};
int32_t sx_victory_synthetic_compute(SxVictoryByteBuffer*, SxVictoryComputeBinding*, SxVictoryShaderInfo*);
int32_t sx_victory_synthetic_container(uint32_t, SxVictoryByteBuffer*);
int32_t sx_victory_synthetic_draw(SxVictoryByteBuffer*, SxVictoryComputeBinding*, SxVictoryByteBuffer*,
                                  SxVictoryComputeBinding*, SxVictoryShaderInfo*);
int32_t sx_victory_shader_translate_compute(const uint8_t*, size_t, SxVictoryByteBuffer*, SxVictoryComputeBinding*,
                                            SxVictoryShaderInfo*);
int32_t sx_victory_shader_translate_stage(const uint8_t*, size_t, uint32_t, SxVictoryByteBuffer*,
                                          SxVictoryComputeBinding*, SxVictoryShaderInfo*);
}

void ConvertBinding(const SxVictoryComputeBinding& binding, scarlettx::VictoryComputeBinding& out) {
    out = scarlettx::VictoryComputeBinding{};
    out.user_data_dwords = binding.user_data_dwords;
    for (uint32_t i = 0; i < binding.user_data_count && i < kUserDataCapacity; ++i) {
        const auto& u = binding.user_data[i];
        out.user_data.push_back({u.index, u.slot, u.root, u.dword});
    }
    out.root_cbv_count = binding.root_cbv_count;
    out.root_srv_count = binding.root_srv_count;
    out.root_uav_count = binding.root_uav_count;
    out.sampler_count = binding.sampler_count;
    out.static_sampler_count = binding.static_sampler_count;
    out.flags = binding.flags;
    for (int i = 0; i < 3; ++i) out.threads[i] = binding.threads[i];
}

scarlettx::VictoryShaderInfo Convert(const SxVictoryShaderInfo& input) {
    scarlettx::VictoryShaderInfo out;
    out.container_status = input.container_status;
    out.native_status = input.native_status;
    out.part_count = input.part_count;
    out.has_dxil = input.has_dxil != 0;
    out.has_xdat = input.has_xdat != 0;
    out.has_xbop = input.has_xbop != 0;
    out.native_program_count = input.native_program_count;
    out.stage_mask = input.stage_mask;
    out.wave64_mask = input.wave64_mask;
    out.message.assign(input.message, strnlen(input.message, kMessageCapacity));
    return out;
}

#endif

}  // namespace

namespace scarlettx {

Status InspectVictoryShader(const std::vector<uint8_t>& bytes, VictoryShaderInfo& out_info) {
#if !defined(SCARLETTX_VICTORY_SHADER_BRIDGE)
    (void)bytes;
    out_info = VictoryShaderInfo{};
    return Status::Error(StatusCode::NotImplemented,
                         "victory-shader bridge was not enabled at configure time");
#else
    if (bytes.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Cannot inspect an empty shader");
    }
    SxVictoryShaderInfo raw{};
    if (sx_victory_shader_inspect(bytes.data(), bytes.size(), &raw) != 0) {
        return Status::Error(StatusCode::ShaderError, "Victory shader bridge rejected its input ABI");
    }
    out_info = Convert(raw);
    return Status::Ok();
#endif
}

Status TranslateVictoryShader(const std::vector<uint8_t>& bytes, VictoryShaderInfo& out_info,
                              std::vector<uint8_t>& out_dxil) {
#if !defined(SCARLETTX_VICTORY_SHADER_BRIDGE)
    (void)bytes;
    out_info = VictoryShaderInfo{};
    out_dxil.clear();
    return Status::Error(StatusCode::NotImplemented,
                         "victory-shader bridge was not enabled at configure time");
#else
    out_dxil.clear();
    if (bytes.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Cannot translate an empty shader");
    }
    SxVictoryShaderInfo raw{};
    SxVictoryByteBuffer buffer{};
    const int32_t result = sx_victory_shader_translate(bytes.data(), bytes.size(), &buffer, &raw);
    out_info = Convert(raw);
    if (result != 0) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Victory shader translation failed: " + out_info.message);
    }
    out_dxil.assign(buffer.data, buffer.data + buffer.size);
    sx_victory_buffer_free(&buffer);
    return Status::Ok();
#endif
}

Status TranslateVictorySyntheticCompute(VictoryShaderInfo& out_info, VictoryComputeBinding& out_binding,
                                        std::vector<uint8_t>& out_dxil) {
    out_info = VictoryShaderInfo{};
    out_binding = VictoryComputeBinding{};
    out_dxil.clear();
#if !defined(SCARLETTX_VICTORY_SHADER_BRIDGE)
    return Status::Error(StatusCode::NotImplemented,
                         "victory-shader bridge was not enabled at configure time");
#else
    SxVictoryShaderInfo raw{};
    SxVictoryByteBuffer buffer{};
    SxVictoryComputeBinding binding{};
    const int32_t result = sx_victory_synthetic_compute(&buffer, &binding, &raw);
    out_info = Convert(raw);
    if (result != 0) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Victory synthetic compute translation failed: " + out_info.message);
    }
    out_dxil.assign(buffer.data, buffer.data + buffer.size);
    sx_victory_buffer_free(&buffer);
    ConvertBinding(binding, out_binding);
    return Status::Ok();
#endif
}

Status TranslateVictorySyntheticDraw(VictoryShaderInfo& out_info, VictoryComputeBinding& out_vs_binding,
                                     std::vector<uint8_t>& out_vs_dxil, VictoryComputeBinding& out_ps_binding,
                                     std::vector<uint8_t>& out_ps_dxil) {
    out_info = VictoryShaderInfo{};
    out_vs_binding = VictoryComputeBinding{};
    out_ps_binding = VictoryComputeBinding{};
    out_vs_dxil.clear();
    out_ps_dxil.clear();
#if !defined(SCARLETTX_VICTORY_SHADER_BRIDGE)
    return Status::Error(StatusCode::NotImplemented,
                         "victory-shader bridge was not enabled at configure time");
#else
    SxVictoryShaderInfo raw{};
    SxVictoryByteBuffer vs{};
    SxVictoryByteBuffer ps{};
    SxVictoryComputeBinding vs_binding{};
    SxVictoryComputeBinding ps_binding{};
    const int32_t result = sx_victory_synthetic_draw(&vs, &vs_binding, &ps, &ps_binding, &raw);
    out_info = Convert(raw);
    if (result != 0) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Victory synthetic draw translation failed: " + out_info.message);
    }
    out_vs_dxil.assign(vs.data, vs.data + vs.size);
    out_ps_dxil.assign(ps.data, ps.data + ps.size);
    sx_victory_buffer_free(&vs);
    sx_victory_buffer_free(&ps);
    ConvertBinding(vs_binding, out_vs_binding);
    ConvertBinding(ps_binding, out_ps_binding);
    return Status::Ok();
#endif
}

const char* VictoryComputeOutcomeName(VictoryComputeOutcome outcome) noexcept {
    switch (outcome) {
        case VictoryComputeOutcome::Translated: return "translated";
        case VictoryComputeOutcome::Desktop: return "desktop";
        case VictoryComputeOutcome::NoNativeCompute: return "no-native-compute";
        case VictoryComputeOutcome::Malformed: return "malformed";
        case VictoryComputeOutcome::Refused: return "refused";
        case VictoryComputeOutcome::Unavailable: return "unavailable";
    }
    return "?";
}

VictoryComputeOutcome TranslateVictoryCompute(const uint8_t* data, size_t size, VictoryShaderInfo& out_info,
                                              VictoryComputeBinding& out_binding,
                                              std::vector<uint8_t>& out_dxil) {
    out_info = VictoryShaderInfo{};
    out_binding = VictoryComputeBinding{};
    out_dxil.clear();
#if !defined(SCARLETTX_VICTORY_SHADER_BRIDGE)
    (void)data;
    (void)size;
    return VictoryComputeOutcome::Unavailable;
#else
    if (!data || size == 0) return VictoryComputeOutcome::Desktop;
    SxVictoryShaderInfo raw{};
    SxVictoryByteBuffer buffer{};
    SxVictoryComputeBinding binding{};
    const int32_t result = sx_victory_shader_translate_compute(data, size, &buffer, &binding, &raw);
    out_info = Convert(raw);
    switch (result) {
        case 0:
            out_dxil.assign(buffer.data, buffer.data + buffer.size);
            sx_victory_buffer_free(&buffer);
            ConvertBinding(binding, out_binding);
            return VictoryComputeOutcome::Translated;
        case 1: return VictoryComputeOutcome::Desktop;
        case 2: return VictoryComputeOutcome::NoNativeCompute;
        case 3: return VictoryComputeOutcome::Malformed;
        default: return VictoryComputeOutcome::Refused;
    }
#endif
}

VictoryComputeOutcome TranslateVictoryStage(const uint8_t* data, size_t size, VictoryStage stage,
                                            VictoryShaderInfo& out_info, VictoryComputeBinding& out_binding,
                                            std::vector<uint8_t>& out_dxil) {
    out_info = VictoryShaderInfo{};
    out_binding = VictoryComputeBinding{};
    out_dxil.clear();
#if !defined(SCARLETTX_VICTORY_SHADER_BRIDGE)
    (void)data;
    (void)size;
    (void)stage;
    return VictoryComputeOutcome::Unavailable;
#else
    if (!data || size == 0) return VictoryComputeOutcome::Desktop;
    SxVictoryShaderInfo raw{};
    SxVictoryByteBuffer buffer{};
    SxVictoryComputeBinding binding{};
    const int32_t result =
        sx_victory_shader_translate_stage(data, size, static_cast<uint32_t>(stage), &buffer, &binding, &raw);
    out_info = Convert(raw);
    switch (result) {
        case 0:
            out_dxil.assign(buffer.data, buffer.data + buffer.size);
            sx_victory_buffer_free(&buffer);
            ConvertBinding(binding, out_binding);
            return VictoryComputeOutcome::Translated;
        case 1: return VictoryComputeOutcome::Desktop;
        case 2: return VictoryComputeOutcome::NoNativeCompute;
        case 3: return VictoryComputeOutcome::Malformed;
        default: return VictoryComputeOutcome::Refused;
    }
#endif
}

bool VictoryGraphicsBindingSupported(const VictoryComputeBinding& binding, std::string& why) {
    why.clear();
    if (binding.user_data_dwords > 32) {
        // Two stages share a 64-dword root signature.
        why = "user data of " + std::to_string(binding.user_data_dwords) + " dwords does not fit a stage's root constants";
    } else if (binding.root_cbv_count) {
        why = "root constant buffers are not bound yet";
    } else if (binding.root_srv_count || binding.root_uav_count) {
        why = "root buffer views are not bound yet";
    } else if (binding.sampler_count || binding.static_sampler_count) {
        why = "samplers are not bound yet";
    } else if (binding.flags) {
        why = "the shader needs title-memory or heap tables (flags " + std::to_string(binding.flags) + ")";
    } else {
        for (const auto& u : binding.user_data) {
            if (u.index >= binding.user_data_dwords) {
                why = "a user-data entry lies outside the user data";
            } else if (u.root == 1) {
                why = "descriptor tables are not mapped for graphics pipelines yet (root parameter " +
                      std::to_string(u.slot) + ")";
            } else if (u.root != 0) {
                why = "user data from root type " + std::to_string(u.root) + " (only root constants are mapped)";
            }
            if (!why.empty()) break;
        }
    }
    return why.empty();
}

bool VictoryComputeBindingSupported(const VictoryComputeBinding& binding, std::string& why) {
    why.clear();
    // A root signature holds 64 dwords; the user data is its only parameter here.
    if (binding.user_data_dwords == 0 || binding.user_data_dwords > 64) {
        why = "user data of " + std::to_string(binding.user_data_dwords) + " dwords does not fit root constants";
    } else if (binding.root_cbv_count) {
        why = "root constant buffers are not bound yet";
    } else if (binding.root_srv_count || binding.root_uav_count) {
        why = "root buffer views are not bound yet";
    } else if (binding.sampler_count || binding.static_sampler_count) {
        why = "samplers are not bound yet";
    } else if (binding.flags) {
        why = "the shader needs title-memory or heap tables (flags " + std::to_string(binding.flags) + ")";
    } else {
        for (const auto& u : binding.user_data) {
            if (u.index >= binding.user_data_dwords) {
                why = "a user-data entry lies outside the user data";
            } else if (u.root != 0 && u.root != 1) {
                why = "user data from root type " + std::to_string(u.root) +
                      " (only root constants and descriptor tables are mapped)";
            } else if (u.root == 1 && u.dword != 0) {
                why = "a descriptor table entry other than its first dword";
            }
            if (!why.empty()) break;
        }
    }
    return why.empty();
}

Status BuildVictorySyntheticContainer(uint32_t variant, std::vector<uint8_t>& out_bytes) {
    out_bytes.clear();
#if !defined(SCARLETTX_VICTORY_SHADER_BRIDGE)
    (void)variant;
    return Status::Error(StatusCode::NotImplemented,
                         "victory-shader bridge was not enabled at configure time");
#else
    SxVictoryByteBuffer buffer{};
    if (sx_victory_synthetic_container(variant, &buffer) != 0) {
        return Status::Error(StatusCode::InvalidArgument, "Unknown synthetic container variant");
    }
    out_bytes.assign(buffer.data, buffer.data + buffer.size);
    sx_victory_buffer_free(&buffer);
    return Status::Ok();
#endif
}

}  // namespace scarlettx
