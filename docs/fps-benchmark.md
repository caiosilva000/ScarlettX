# Synthetic frame-rate benchmark (opt-in)

`bench_sx_fps` renders synthetic frames through the guest-facing `d3d12_xs` objects (the shim's device, command
allocator, command list, queue, fence, resources and descriptor heaps, called by vtable slot, as a title calls
them) and reports frame rates and where the time of a frame goes. `tools/sx_bench_fps.py` launches it many times
through CrossOver. It is not a test and is not built by default:

```sh
SCARLETTX_BENCHMARKS=ON SCARLETTX_BUILD_DIR="$BOTTLE/drive_c/sxfps/build" tools/macos_cross_build.sh build
tools/sx_bench_fps.py --build "$BOTTLE/drive_c/sxfps/build" --out out/fps-offscreen --suite offscreen
tools/sx_bench_fps.py --build "$BOTTLE/drive_c/sxfps/build" --out out/fps-windowed --suite windowed
tools/sx_bench_fps.py --build <A> --build-b <B> --out out/fps-ab --suite ab
```

The scene and the frame loop start from `tests/synthetic_games/scarlett_arena`: a floor, beacons that move on a
scripted path and pulse on every fourth frame, and per frame "wait for the frame, record, submit, present".

## Shader source

Desktop bytecode only: HLSL through `D3DCompile`, pipelines through the shim's `CreateGraphicsPipelineState`.
`--shader-source translated` is reserved for native RDNA2 graphics through the shim and exits 5 until the shim can
create such a pipeline; the place for it is marked in `bench/bench_sx_fps.cpp`. A translated draw that goes to
`D3D12Backend` directly (`bench_victory_prewarm --workload draw`) is a backend-only run and must be labelled so.

## Stress modes (one mode and one variant per process)

| Mode | What is fixed | Variants |
|---|---|---|
| `draws` | `--draws` indexed 4x4-pixel quads per frame, a cheap textured shader, 1280x720 | `fixed`: one descriptor table per frame. `pertable`: `SetGraphicsRootDescriptorTable` before every draw, cycling over 16 textures with equal contents, so the picture is the same |
| `pixel` | floor + 24 beacons (25 draws), `--width` x `--height` | `light`: one integer hash per pixel. `heavy`: the same hash and `--rounds` (1024) xorshift rounds |
| `upload` | 16 textures of 256x256 RGBA8 rewritten every frame (4,194,304 bytes), then drawn 1:1 | `percopy` (baseline): a new staging buffer per texture and one `ExecuteCommandLists` per copy. `batched`: all copies in the frame's command list. `reuse`: staging kept per in-flight slot and reused after that slot's fence completes. `batched-reuse`: both. `guestmem`: texels written through the address `GetGPUVirtualAddress` returns for the texture (the console route; the shim notices the writes and uploads at submission) |

Each process logs the draws, descriptor-table changes, submissions, uploads and upload bytes it actually issued
per frame.

`guestmem` is the variant that exercises ScarlettX's own code most: the shim write-watches the texture's memory,
converts it from the console layout and uploads it at submission. It is where this benchmark found its first
optimization: a texture written in two submissions running is no longer re-armed page by page (see
`FlushGuestTextureWrites` in `src/shims/d3d12_xs_shim.cpp`). The other four variants go through the command
list's `CopyTextureRegion` and measure the host route with a thin shim on top.

## Paths

- `offscreen`: uncapped throughput with `--inflight` (3) frames in flight. The loop waits only to recycle a slot
  whose previous frame's fence has not completed, and to drain the queue at the end. The headline is
  **completed frames per second**: frames divided by the time from the first frame's start to the drain.
- `serialized`: one frame at a time, each waited for. Reports submission-to-fence per frame. This is not a frame
  rate and is always labelled `serialized`.
- `windowed`: the same loop with `WaitFrameEventX` (after `SetFrameIntervalX`, unless `--interval-us 0`) and
  `PresentX` per frame. The window's client area is 1280x720 whatever the internal resolution: the shim's swap
  chain takes the size of the presented texture and stretches. The benchmark reports **present-call intervals**
  (when `PresentX` was called). It cannot see when a frame reached the display: the swap chain belongs to the
  shim and the route offers no display timestamps. Instead it draws the frame number along the bottom edge
  (16 squares, white = 1, then a square that alternates magenta and cyan, then red and green), and the driver
  captures the window while the benchmark runs and decodes which frame was on screen at each capture.

## What is timed, and what is not

Per frame (raw rows in `frames.csv`, microseconds): `wait_us` (recycling wait on the fence), `pace_us`
(`WaitFrameEventX`), `prep_us` (CPU preparation: resets, generated texels, recording, `Close`), `submit_us`
(`ExecuteCommandLists` and `Signal`), `present_us` (`PresentX`), and the counts. `metrics.csv` has per-launch
medians, p95 and p99; the driver's summary pools the frames of all launches of a cell.

All times are wall-clock. The host returns no usable GPU timestamps, so there are no GPU-side durations.

Before timing, every pipeline is created and drawn with: warm-up frames run the same code as timed frames.
One discarded launch per pipeline comes first, so the host's on-disk shader cache has seen every shader.

## Correctness (outside the timed loop; a wrong result is a non-zero exit)

- `--check-frames` (2) more frames run through the same code straight after the timed loop, pipelined, in slots
  of their own, each copied to a read-back buffer, and are compared, pixel for pixel, with a picture computed on
  the CPU for that frame.
- Then the render target of every in-flight slot is read back and compared the same way with the frame that
  slot last held: the last frames of the timed loop itself. In `upload` every frame's textures carry 16 texels that belong to that
  frame alone, so a frame drawn with an earlier update does not compare equal.
- `draws` adds a frame with a table change per draw over 16 textures that differ, to show that a per-draw
  binding reaches its draw.
- Every fence value signalled must have completed, and the device must not be removed.

The checked frames come before any other submission on purpose. On the windowed path the shim looks for writes
to texture memory (the `guestmem` route) once per presented frame, at the first `ExecuteCommandLists` after a
`PresentX`. A title that submits one command list, then writes texture memory, then submits another in the same
frame gets the old texels in that second submission and the new ones a frame later. An earlier revision of this
benchmark submitted a read-back in between and saw exactly that (19,456 of 1,048,576 pixels exact in the frame
that followed).

Exit codes: 0 pass, 1 a read-back was not exact or a fence did not complete, 2 a call failed, 3 no device,
5 shader source not available, 64 usage.

## Comparisons

Variants of one comparison, or the two builds of an A/B, are launched back to back and the order rotates.
The driver checks for other builds or tests on the machine before each launch, waits for them, and marks
launches that ran while one was active. It does not stop the wineserver and does not delete caches.
