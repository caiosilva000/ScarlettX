# Prewarm benchmark (opt-in)

Does preparing a shader and its pipeline ahead of time shorten the first use? `bench_victory_prewarm` measures
it for synthetic fixtures; `tools/sx_bench_prewarm.py` launches it many times through CrossOver.

Build it with the bridge and the benchmark option on (neither is on by default):

```sh
SCARLETTX_BENCHMARKS=ON SCARLETTX_VICTORY_SHADER_BRIDGE=ON SCARLETTX_BUILD_DIR="$BOTTLE/drive_c/sxbench/build" \
    tools/macos_cross_build.sh build
tools/sx_bench_prewarm.py --build "$BOTTLE/drive_c/sxbench/build" --out out/prewarm
```

## Workloads, each labelled with the shader path it exercises

| Workload | Path |
|---|---|
| `compute` | synthetic GFX10 compute -> victory-shader bridge -> DXIL -> `D3D12Backend` probe session |
| `draw` | synthetic GFX10 vertex and pixel programs -> bridge -> DXIL -> `D3D12Backend` probe session |
| `control` | no translation: the existing offscreen indexed draw (HLSL through d3dcompiler, pipeline built inside every call) |

`compute` and `draw` are the backend path, not the d3d12_xs shim's guest-facing route.

## Conditions (one per process)

| Condition | Before the timed first use | The first use |
|---|---|---|
| `cold` | nothing | translate, build the pipeline, submit |
| `prewarmed` | translate, build the pipeline | submit |
| `primed` | translate, build the pipeline, one throwaway submission | submit |
| `steady` | translate, build the pipeline, three warm-up submissions | none timed |

Every launch then runs the steady loop: `--iterations` submissions with the pipeline in place, nothing read back
inside the loop, and one validating read-back after it.

## Metrics (wall-clock milliseconds)

`translate_ms`, `root_signature_ms`, `pipeline_create_ms`, `resources_ms`; `first_submission_ms` (recording the
command list, `ExecuteCommandLists`, until the fence completes); `first_use_latency_ms` (what a caller waits for at
first use: the submission, plus translation and pipeline creation when cold); `preparation_ms` (what was spent
ahead of time); `total_to_first_result_ms` (everything up to the first completed result, preparation included, so
a stall moved earlier still shows); `steady_submission_median_ms` and `_p95_ms`; `translate_repeat_ms` and
`pipeline_recreate_ms` (the same shader and pipeline a second time in the process).

`gpu_ms` comes from timestamp queries around the work and is reported only when the host returns usable
timestamps. These are offscreen submission timings; frame pacing needs the windowed presentation probe.

## Cache conditions

A fresh process is cold ScarlettX and D3D12 device state only. The host keeps shader caches on disk (D3DMetal:
one per executable name under the user cache directory, `d3dm/<exe>/shaders.cache`), so the driver runs three
series and never deletes a cache:

- `seen`: the plain fixtures, after one discarded launch per workload.
- `unseen`: a salt in the fixture that no launch has used, so the translated shader is new to every cache.
- `freshcache`: a copy of the benchmark under a new name, which starts with an empty D3DMetal cache.

The driver records the cache directories' size before and after each series, the adapter, and what graphics
libraries the first launch has mapped.

Correctness is required in every launch: completed fences, exact dwords or pixels, exit 0.

## Host-backend comparison (`tools/sx_bench_backends.py`)

One integer kernel on two host backends on the same Mac, same numbers:

```text
element i:  x = i + c0;  x ^= x << 13;  x ^= x >> 17;  x ^= x << 5;  out[i] = (x + c1) ^ k
```

- `crossover`: `bench_victory_prewarm --workload vector`, the kernel as a synthetic GFX10 container, translated by
  the bridge to DXIL and dispatched by `D3D12Backend` under CrossOver.
- `metal`: `bench/metal_vector_kernel.swift`, the kernel written in Metal Shading Language on the native Metal
  device. Nothing is translated.

Both use 32 threads per group and N / 32 groups, check every element against their own CPU version, and print a
hash of the output for fixed constants. The driver computes that hash a third time in Python and requires all
three to agree. It reports the cold path (front end or translation, pipeline creation, first dispatch) and the
warm dispatch (each dispatch submitted and waited for) separately.

This compares host backends. It is not a measure of RDNA2 translation coverage, and the two programs are not the
same code. `k` is a literal in the shader, so the `unseen` series gives both backends a shader new to their caches.
