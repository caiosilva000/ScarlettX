#include "sx_check.h"
#include <iostream>

#include "scarlettx/loader/game_config.h"

int main() {
    const std::string scarlett_xml = R"(
<?xml version="1.0" encoding="utf-8"?>
<Game configVersion="1">
  <Identity Name="TestStudio.ScarlettGame" Publisher="CN=Test" Version="1.2.3.4" />
  <TitleId>74B7FFFC</TitleId>
  <ShellVisuals DefaultDisplayName="Test Scarlett Title" />
  <ExecutableList>
    <Executable Name="Game.exe" Id="Game" TargetDeviceFamilyForXboxOne="XboxGen9" />
  </ExecutableList>
</Game>
)";

    scarlettx::GameConfig cfg;
    auto st = scarlettx::ParseGameConfigXml(scarlett_xml, cfg);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(cfg.IsNativeScarlett());
    SX_REQUIRE(cfg.title_id == "74B7FFFC");
    SX_REQUIRE(cfg.identity_name == "TestStudio.ScarlettGame");
    SX_REQUIRE(cfg.PrimaryExecutable() != nullptr);
    SX_REQUIRE(cfg.PrimaryExecutable()->name == "Game.exe");

    const std::string formatted_xml = R"(
<Game configVersion = '1'>
  <Identity Name = 'TestStudio.FormattedGame' />
  <ShellVisuals DefaultDisplayName = 'Arcade > Racing &amp; &quot;Rally&quot; &apos;98 &lt;Demo&gt; &amp;lt;' />
  <ExecutableList>
    <Executable OverrideDisplayName="Alternate Title" Alias='Name="Wrong.exe"'
                Name
                = 'Formatted.exe' TargetDeviceFamily = "Scarlett" />
  </ExecutableList>
</Game>
)";

    scarlettx::GameConfig formatted_cfg;
    st = scarlettx::ParseGameConfigXml(formatted_xml, formatted_cfg);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(formatted_cfg.config_version == "1");
    SX_REQUIRE(formatted_cfg.identity_name == "TestStudio.FormattedGame");
    SX_REQUIRE(formatted_cfg.display_name == "Arcade > Racing & \"Rally\" '98 <Demo> &lt;");
    SX_REQUIRE(formatted_cfg.IsNativeScarlett());
    SX_REQUIRE(formatted_cfg.PrimaryExecutable() != nullptr);
    SX_REQUIRE(formatted_cfg.PrimaryExecutable()->name == "Formatted.exe");

    const std::string gen9_aware_xml = R"(
<Game configVersion="0">
  <Identity Name="TestStudio.OldGame" Publisher="CN=Test" Version="1.0.0.0" />
  <ExecutableList>
    <Executable Name="OldGame.exe" TargetDeviceFamilyForXboxOne="XboxOneGen9Aware" />
  </ExecutableList>
</Game>
)";
    scarlettx::GameConfig old_cfg;
    st = scarlettx::ParseGameConfigXml(gen9_aware_xml, old_cfg);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(!old_cfg.IsNativeScarlett());
    SX_REQUIRE(old_cfg.console_target == scarlettx::ConsoleTarget::XboxOneGen9Aware);

    const std::string pc_xml = R"(
<Game configVersion="1">
  <Identity Name="TestStudio.DesktopGame" Publisher="CN=Test" Version="1.0.0.0" />
  <ExecutableList>
    <Executable Name="DesktopGame.exe" Alias="DesktopGame.exe" TargetDeviceFamily="PC" Id="Game" />
  </ExecutableList>
</Game>
)";
    scarlettx::GameConfig pc_cfg;
    st = scarlettx::ParseGameConfigXml(pc_xml, pc_cfg);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(pc_cfg.IsPcDesktop());
    SX_REQUIRE(!pc_cfg.IsNativeScarlett());
    SX_REQUIRE(!cfg.IsPcDesktop());
    SX_REQUIRE(pc_cfg.PrimaryExecutable() != nullptr);
    SX_REQUIRE(pc_cfg.PrimaryExecutable()->name == "DesktopGame.exe");

    // A config with no TargetDeviceFamily parses with an unknown target ...
    const std::string untargeted_xml = R"(
<Game configVersion="1">
  <Identity Name="TestStudio.Untargeted" Publisher="CN=Test" Version="1.0.0.0" />
  <ExecutableList>
    <Executable Name="Untargeted.exe" Id="Game" />
  </ExecutableList>
</Game>
)";
    scarlettx::GameConfig untargeted_cfg;
    st = scarlettx::ParseGameConfigXml(untargeted_xml, untargeted_cfg);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(untargeted_cfg.console_target == scarlettx::ConsoleTarget::Unknown);
    SX_REQUIRE(!untargeted_cfg.target_inferred);
    SX_REQUIRE(untargeted_cfg.PrimaryExecutable() != nullptr);

    // ... and the executable's imports say which platform it was built for.
    using scarlettx::ConsoleTarget;
    using scarlettx::InferConsoleTargetFromImports;
    SX_REQUIRE(InferConsoleTargetFromImports({"KERNEL32.dll", "d3d12_xs.dll", "xg_xs.dll"}) ==
               ConsoleTarget::XboxGen9Scarlett);
    SX_REQUIRE(InferConsoleTargetFromImports({"d3d12_x.dll", "KERNEL32.dll"}) == ConsoleTarget::XboxOne);
    SX_REQUIRE(InferConsoleTargetFromImports({"D3D12.dll", "dxgi.dll", "USER32.dll"}) == ConsoleTarget::PcDesktop);
    SX_REQUIRE(InferConsoleTargetFromImports({"d3d12.dll", "D3D12_XS.DLL"}) == ConsoleTarget::XboxGen9Scarlett);
    SX_REQUIRE(InferConsoleTargetFromImports({"KERNEL32.dll", "xgameruntime.dll"}) == ConsoleTarget::Unknown);
    SX_REQUIRE(InferConsoleTargetFromImports({}) == ConsoleTarget::Unknown);

    scarlettx::GameConfig bad_cfg;
    st = scarlettx::ParseGameConfigXml("<NotAGame/>", bad_cfg);
    SX_REQUIRE(!st.ok());

    std::cout << "[test_game_config] PASSED\n";
    return 0;
}
