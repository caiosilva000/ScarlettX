# Regression runner (`tools/sx_regress.py`)

One script runs a Windows x64 build tree and reports each check as PASS, FAIL, SKIP or BLOCKED in
`summary.json`. On macOS the build runs in a CrossOver bottle (default `scarlettx-m4`); on Windows it runs
directly (that mode has not been exercised yet).

```sh
# every CTest entry, the offscreen probes with their exact-pixel requirements, and on-screen presentation
tools/sx_regress.py regress --build <build> --inventory <ctest --show-only=json-v1 output> --out <dir> \
    [--sha256-list <file>] [--commit <sha>]

# bounded, traced boot of one package; the package is never written to
tools/sx_regress.py boot-diag --build <build> --package <package dir> --out <dir> \
    [--in-process] [--windowed] [--frames N] [--trace] [--rt-dump] [--raster-dump] [--confirm-user-package] \
    [--allow-pc-target] [--until-window SECONDS]
```

`regress`
- Fixtures are found through `SCARLETTX_TEST_BINARY_DIR`; build-machine paths in the inventory are rewritten.
- Each test has a timeout (default 120 s), its own `.stdout` / `.stderr`, exit code, and the adapters it logged.
- `test_d3d12_offscreen_clear` / `_indexed` pass only with fence completion and 4096 of 4096 exact pixels.
- A GPU test that fails on an unnamed WARP adapter is rerun with `SCARLETTX_PREFER_WARP=0`; both results are
  recorded and the check stays FAIL.
- Presentation is judged from captures of the test window (macOS, needs Screen Recording, `swiftc`, Pillow);
  without those it is BLOCKED, never PASS on API results alone.
- A failing check never stops the others. Exit status is 0 only with no FAIL and no BLOCKED.

`boot-diag`
- Reports the last stage reached (`package_inspected`, `package_drive_mapped`, `image_mapped`, `guest_entered`,
  `d3d12_device_created`, `swap_chain_created`, `first_frame`, `guest_exited_cleanly`), the first missing
  import, the first failing graphics call, the first error line, and any frame captures.
- The boot uses a copy-on-write clone of the package inside `--out` (APFS, same volume); the original is
  compared (size and modification time of every file) before and after. Saves go to `--out/saves`.
- A package that is not one of the build's synthetic packages needs `--confirm-user-package`.
- With `--windowed` the runner captures the largest window owned by the emulator or the guest, whatever its
  title, about once a second, and adds two stages from those captures: `guest_window_visible` and
  `guest_window_content` (at least 1% of the client area differs from the most common colour). A boot that
  times out says which of the three it saw: content, a blank window, or no window.
- A boot whose log shows the guest crashing (`Unhandled page fault` or `Unhandled exception`, recorded as
  `first_guest_fault`) is FAIL whatever else happened, including a window that showed content for the whole
  `--until-window` period. A crash after the runner has already ended the boot is not seen. What the title writes
  on its own screen is not read, so a title that draws an error message still passes the window check.
- `--allow-pc-target` passes the emulator's flag of the same name, for GDK packages built for PC
  (`TargetDeviceFamily="PC"`, for example Microsoft's desktop GDK samples). Such a title draws through the
  desktop `d3d12.dll`, not the `d3d12_xs` shim, so it logs no frames and `--frames` never ends it.
- `--until-window SECONDS` is the end condition for those titles: once the guest window has shown content for
  that long the runner ends the boot and reports PASS. It is BLOCKED without working window capture. A still
  picture cannot prove the guest is not stuck after drawing it; `guest_window.picture_changed` records whether
  the captures differed.

## Building locally on a Mac (`tools/macos_cross_build.sh`)

```sh
tools/macos_cross_build.sh all        # download + verify LLVM-MinGW 20261117 (SHA-256 pinned), build, write the inventory
tools/sx_regress.py regress --build build/macos-win64 \
    --inventory build/macos-win64/ctest-inventory.json --out out/regress
```

This is the quick route for trying a change without a Windows machine. The MSVC build on a Windows
runner remains the reference: the local build uses Clang with the MinGW runtime.

`regress` preflight on CrossOver also checks that the bottle has `xaudio2_9 = native` (BLOCKED with the
command to set it otherwise) and records the bottle's `CX_GRAPHICS_BACKEND` setting in `summary.json`.

## Translated compute (research builds with the Victory bridge)

When the build has `test_victory_compute_e2e` (configured with `-DSCARLETTX_VICTORY_SHADER_BRIDGE=ON`), `regress`
runs it as a GPU check and requires more than exit 0: all five dispatch fences completed without a timeout and
20 of 20 read-back dwords exact. The result's `victory_compute` block names the shader path it exercises, the
adapter and the first failing API if there was one.

Every check also records `exe_sha256`. Without `--commit`, the summary takes the source revision from the
build's own `build-info.txt` (`commit_source` says which), and carries that file as `build_info`: for a bridge
build it holds the Victory gitlink, the bridge's `Cargo.lock` hash and the Rust toolchain.

`test_victory_compute_guest` is the same program on the guest-facing route: a console-shaped container handed to
the shim's device object, dispatched through the shim's command list. As a GPU check it needs the native pipeline
created and counted as translated, seven completed fences, 20 of 20 dwords, five translated dispatches and no
failed check line (`victory_guest` block).
