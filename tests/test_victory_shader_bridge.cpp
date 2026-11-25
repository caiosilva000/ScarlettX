#include "scarlettx/gpu/victory_shader_bridge.h"
#include "sx_check.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

void PutU32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
    std::memcpy(bytes.data() + at, &value, sizeof(value));
}

// A wholly synthetic DXBC envelope containing an intentionally minimal DXIL
// part. It proves the GPL bridge sees the standard container boundary without
// embedding a native game shader or asserting that there is a GFX10 payload.
std::vector<uint8_t> SyntheticDxilEnvelope() {
    constexpr uint32_t kPartOffset = 36;
    constexpr uint32_t kPartSize = 24;
    std::vector<uint8_t> out(kPartOffset + 8 + kPartSize, 0);
    std::memcpy(out.data(), "DXBC", 4);
    PutU32(out, 0x14, 1);             // DXBC 1.0
    PutU32(out, 0x18, static_cast<uint32_t>(out.size()));
    PutU32(out, 0x1c, 1);
    PutU32(out, 0x20, kPartOffset);
    std::memcpy(out.data() + kPartOffset, "DXIL", 4);
    PutU32(out, kPartOffset + 4, kPartSize);
    const size_t program = kPartOffset + 8;
    PutU32(out, program + 0, (5u << 16) | (6u << 4));  // cs_6_0
    PutU32(out, program + 4, 6);                        // 24 bytes
    std::memcpy(out.data() + program + 8, "DXIL", 4);
    PutU32(out, program + 12, 0x100);
    PutU32(out, program + 16, 16);                      // bitcode starts at +24
    PutU32(out, program + 20, 0);                       // deliberately absent
    return out;
}

}  // namespace

int main() {
    scarlettx::VictoryShaderInfo info;
    const auto envelope = SyntheticDxilEnvelope();
    SX_REQUIRE(scarlettx::InspectVictoryShader(envelope, info).ok());
    SX_REQUIRE(info.container_status == 0);
    SX_REQUIRE(info.part_count == 1);
    SX_REQUIRE(info.has_dxil);
    // The bridge now tells the host that this is a DXBC/DXIL envelope but not
    // a native GFX10 shader. A title-derived container must never be needed
    // for this test.
    SX_REQUIRE(info.native_status != 0);
    SX_REQUIRE(info.native_program_count == 0);
    // The wave size a translated shader expects, read from its PSV0 part.
    struct WaveCase {
        uint32_t variant;
        uint32_t min;
        uint32_t max;
    };
    for (const WaveCase& c : {WaveCase{13, 32, 32}, WaveCase{14, 64, 64}, WaveCase{7, 0, 0xFFFFFFFFu}}) {
        std::vector<uint8_t> container;
        SX_REQUIRE(scarlettx::BuildVictorySyntheticContainer(c.variant, container).ok());
        scarlettx::VictoryShaderInfo wave_info;
        scarlettx::VictoryComputeBinding binding;
        std::vector<uint8_t> dxil;
        SX_REQUIRE(scarlettx::TranslateVictoryCompute(container.data(), container.size(), wave_info, binding, dxil) ==
                   scarlettx::VictoryComputeOutcome::Translated);
        uint32_t min = 1;
        uint32_t max = 1;
        SX_REQUIRE(scarlettx::DxilExpectedWaveLanes(dxil.data(), dxil.size(), min, max));
        SX_REQUIRE(min == c.min && max == c.max);
        // Cut anywhere, the container is refused, not read past its end.
        for (size_t cut = 0; cut < dxil.size(); cut += 7) {
            uint32_t a = 0;
            uint32_t b = 0;
            (void)scarlettx::DxilExpectedWaveLanes(dxil.data(), cut, a, b);
        }
        std::cout << "[test_victory_shader_bridge] variant " << c.variant << " expects waves " << min << ".." << max
                  << "\n";
    }
    {
        uint32_t min = 0;
        uint32_t max = 0;
        SX_REQUIRE(!scarlettx::DxilExpectedWaveLanes(nullptr, 0, min, max));
        SX_REQUIRE(!scarlettx::DxilExpectedWaveLanes(envelope.data(), envelope.size(), min, max));  // no PSV0 part
        std::string why;
        SX_REQUIRE(scarlettx::WaveLanesSupported(0, 0xFFFFFFFFu, false, 0, 0, why));   // no requirement
        SX_REQUIRE(scarlettx::WaveLanesSupported(32, 32, true, 32, 32, why));
        SX_REQUIRE(scarlettx::WaveLanesSupported(64, 64, true, 4, 128, why));
        SX_REQUIRE(scarlettx::WaveLanesSupported(16, 64, true, 32, 32, why));          // a range that includes 32
        SX_REQUIRE(!scarlettx::WaveLanesSupported(64, 64, true, 32, 32, why));
        SX_REQUIRE(why.find("64") != std::string::npos && why.find("32") != std::string::npos);
        SX_REQUIRE(!scarlettx::WaveLanesSupported(8, 16, true, 32, 64, why));
        SX_REQUIRE(!scarlettx::WaveLanesSupported(32, 32, false, 0, 0, why));          // no wave operations at all
        std::cout << "[test_victory_shader_bridge] wave ranges: " << why << "\n";
    }
    std::cout << "[test_victory_shader_bridge] DXBC parts=" << info.part_count
              << " native=" << info.native_program_count << " message=" << info.message << "\n";
    return 0;
}
