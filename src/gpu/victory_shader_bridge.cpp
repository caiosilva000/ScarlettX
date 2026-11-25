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
    out_binding.user_data_dwords = binding.user_data_dwords;
    for (uint32_t i = 0; i < binding.user_data_count && i < kUserDataCapacity; ++i) {
        const auto& u = binding.user_data[i];
        out_binding.user_data.push_back({u.index, u.slot, u.root, u.dword});
    }
    out_binding.root_cbv_count = binding.root_cbv_count;
    out_binding.root_srv_count = binding.root_srv_count;
    out_binding.root_uav_count = binding.root_uav_count;
    out_binding.sampler_count = binding.sampler_count;
    out_binding.static_sampler_count = binding.static_sampler_count;
    out_binding.flags = binding.flags;
    for (int i = 0; i < 3; ++i) out_binding.threads[i] = binding.threads[i];
    return Status::Ok();
#endif
}

}  // namespace scarlettx
