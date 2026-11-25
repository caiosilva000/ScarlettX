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

}  // namespace scarlettx
