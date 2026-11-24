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
    [--in-process] [--windowed] [--frames N] [--trace] [--rt-dump] [--raster-dump] [--confirm-user-package]
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
