# macOS ARM64 first host build

Build on Apple Silicon with Xcode command-line tools and CMake >=3.20:
```sh
cmake -S . -B build -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```

Verified locally on Darwin arm64, AppleClang 21, CMake 4.4.3: native Mach-O arm64 CLI and static core built; 3/3 tests passed (game_config, kernel_hle, rt_pipeline).

## Dependency audit
The portable core uses C++20 and the standard library; no downloaded third-party dependencies are required. Existing POSIX aligned allocation compiles on macOS. Windows uses the Windows SDK: Direct3D12, DXGI, D3DCompiler, OLE32 and UUID, plus Win32 loader/window/input APIs. MinGW additionally uses its thread runtime. Windows guest shims and PE synthetic games are excluded on macOS because ARM64 dylibs and Mach-O executables cannot replace x86-64 PE DLLs/games.

The initial full build failed on unconditional windows.h in xg_xs_shim.cpp and Windows D3D12 types/performance counters in d3d12_xs_shim.cpp. The Apple build selects the portable host targets before declaring those Windows-dependent targets. The existing Windows target definitions remain intact.

## Scope and limitations
This is a working native host build, not a playable Xbox emulator. Package inspection still requires a real package executable. Inspecting the source-only hello_scarlett fixture correctly reports its missing .exe. The diag command returns an error stating D3D12Backend requires Windows. No Metal renderer or x86-64 guest translation exists in this change. Windows builds were not run locally; compatibility is preserved structurally but unverified here. PE guest execution, audio/input shims and Windows integration tests are not counted as passing macOS tests.
