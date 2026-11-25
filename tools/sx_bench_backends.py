#!/usr/bin/env python3
"""Runs one integer kernel on two host backends on this Mac and compares them.

    tools/sx_bench_backends.py --build <build dir inside the bottle> --out <folder>

  crossover   bench_victory_prewarm --workload vector: a synthetic GFX10 container translated by
              the victory-shader bridge to DXIL, dispatched by D3D12Backend under CrossOver
  metal       bench/metal_vector_kernel.swift: the same kernel written in Metal Shading Language,
              on the native Metal device (nothing translated)

Both compute, for i < N:  x = i + c0; x ^= x << 13; x ^= x >> 17; x ^= x << 5; out[i] = (x + c1) ^ k
and both check every element against their own CPU version. For fixed constants each prints a
hash of its output; this script computes the same hash a third time in Python and requires all
three to agree.

Series: `seen` (k = 0, after one discarded launch per backend) and `unseen` (a k no launch has
used: a shader new to every cache; the same k goes to both backends so their outputs compare).
Each launch is a fresh process; the two backends alternate.

This is a comparison of host backends on one kernel. It is not a measure of RDNA2 translation
coverage, and the two programs are not the same code: one is translated console code, the other
hand-written MSL.
"""
import argparse
import csv
import hashlib
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sx_bench_prewarm import EXE, WINE, WINESERVER, pct, sh, win_path  # noqa: E402

C0, C1 = 0x12345678, 0x9E3779B9
M = 0xFFFFFFFF


def cpu_hash(n: int, k: int) -> str:
    """FNV-1a 64 over the little-endian bytes of the kernel's output for the fixed constants."""
    h = 14695981039346656037
    for i in range(n):
        x = (i + C0) & M
        x ^= (x << 13) & M
        x ^= x >> 17
        x ^= (x << 5) & M
        v = ((x + C1) & M) ^ k
        for b in (v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24):
            h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"


def cache_state(names):
    root = Path(sh(["getconf", "DARWIN_USER_CACHE_DIR"]))
    state = {}
    for name in names:
        d = root / name
        files = [f for f in d.rglob("*") if f.is_file()] if d.exists() else []
        state[name] = {"exists": d.exists(), "files": len(files), "bytes": sum(f.stat().st_size for f in files)}
    return state


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--bottle", default="scarlettx-m4")
    ap.add_argument("--sandbox", default=str(Path.home() / "m4-feasibility/retail-preflight/offline.sb"))
    ap.add_argument("--launches", type=int, default=10)
    ap.add_argument("--unseen-launches", type=int, default=8)
    ap.add_argument("--iterations", type=int, default=200)
    ap.add_argument("--elements", type=int, default=1 << 20)
    ap.add_argument("--timeout", type=int, default=120)
    args = ap.parse_args()

    out = Path(args.out).resolve()
    (out / "logs").mkdir(parents=True, exist_ok=True)
    (out / "bin").mkdir(exist_ok=True)
    build = Path(args.build).resolve()
    exe = build / EXE
    if not exe.exists():
        print(f"BLOCKED: {exe} is missing")
        return 2
    source = Path(__file__).resolve().parent.parent / "bench" / "metal_vector_kernel.swift"
    native = out / "bin" / "metal_vector_kernel"
    built = subprocess.run(["xcrun", "swiftc", "-O", str(source), "-o", str(native)], capture_output=True, text=True)
    if built.returncode != 0:
        print("BLOCKED: the native Metal program did not compile\n" + built.stderr[-2000:])
        return 2

    (out / "metrics.csv").write_text("series,launch,workload,shader_path,condition,salt,shader_fnv,metric,value\n")
    (out / "samples.csv").write_text("series,launch,workload,condition,salt,iteration,record_ms,submit_to_fence_ms,gpu_ms\n")
    caches = [f"d3dm/{EXE}", "metal_vector_kernel", "com.apple.metal"]
    env = {
        "started": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "machine": platform.machine(), "macos": platform.mac_ver()[0],
        "cpu": sh(["sysctl", "-n", "machdep.cpu.brand_string"]),
        "crossover": sh(["defaults", "read", "/Applications/CrossOver.app/Contents/Info", "CFBundleShortVersionString"]),
        "swift": sh(["xcrun", "swiftc", "--version"]).splitlines()[0],
        "bottle": args.bottle, "power": sh(["pmset", "-g", "batt"]), "thermal": sh(["pmset", "-g", "therm"]),
        "elements": args.elements, "iterations": args.iterations,
        "crossover_exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
        "metal_exe_sha256": hashlib.sha256(native.read_bytes()).hexdigest(),
        "metal_source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "caches_before": cache_state(caches),
    }
    subprocess.run([WINESERVER, "--bottle", args.bottle, "-k"], capture_output=True)
    time.sleep(2)

    exe_win = "C:\\" + str(build).split("drive_c/", 1)[1].replace("/", "\\") + "\\" + EXE
    launches = []
    n = 0

    def go(backend, series, k, hold_ms=0):
        nonlocal n
        n += 1
        if backend == "crossover":
            cmd = ["sandbox-exec", "-f", args.sandbox, WINE, "--bottle", args.bottle, exe_win, "--workload", "vector",
                   "--condition", "cold", "--series", f"{series}-crossover", "--launch", str(n), "--salt", str(k),
                   "--iterations", str(args.iterations), "--elements", str(args.elements),
                   "--csv", win_path(out / "metrics.csv"), "--samples", win_path(out / "samples.csv")]
            if hold_ms:
                cmd += ["--hold-ms", str(hold_ms)]
        else:
            cmd = ["sandbox-exec", "-f", args.sandbox, str(native), "--series", f"{series}-metal", "--launch", str(n),
                   "--k", str(k), "--iterations", str(args.iterations), "--elements", str(args.elements),
                   "--csv", str(out / "metrics.csv"), "--samples", str(out / "samples.csv")]
        envp = dict(os.environ)
        envp.setdefault("WINEDEBUG", "fixme-all")
        start = time.time()
        evidence = None
        with open(out / "logs" / f"{n:04d}.stdout", "wb") as so, open(out / "logs" / f"{n:04d}.stderr", "wb") as se:
            proc = subprocess.Popen(cmd, stdout=so, stderr=se, env=envp, cwd=str(Path.home()))
            if hold_ms:
                time.sleep(max(1.0, hold_ms / 1000 - 2.0))
                evidence = {}
                for pid in sh(["pgrep", "-f", EXE]).split():
                    mapped = sh(["vmmap", pid])
                    hits = sorted({line.split()[-1] for line in mapped.splitlines()
                                   if re.search(r"D3DMetal|apple_gptk.*d3d12|metalirconverter|Metal\.framework/Versions/A/Metal$", line)})
                    evidence[pid] = [h.replace("/Applications/CrossOver.app/Contents/SharedSupport/CrossOver", "<CX>") for h in hits]
            try:
                code = proc.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                proc.kill()
                code = "timeout"
        text = (out / "logs" / f"{n:04d}.stdout").read_text(errors="replace")
        digest = re.search(r"canonical_output_fnv=([0-9a-f]{16})", text)
        device = re.search(r'(?:adapter|device)="([^"]*)"', text)
        row = {"launch": n, "backend": backend, "series": series, "k": k, "exit": code,
               "seconds": round(time.time() - start, 2), "output_fnv": digest.group(1) if digest else None,
               "device": device.group(1) if device else None}
        launches.append(row)
        print(f"{n:4d} {backend:10s} {series:8s} k={k:<11d} exit={code} fnv={row['output_fnv']} {row['seconds']}s", flush=True)
        return evidence

    env["route_evidence_crossover"] = go("crossover", "discard", 0, hold_ms=6000)
    go("metal", "discard", 0)
    for _ in range(args.launches):
        go("crossover", "seen", 0)
        go("metal", "seen", 0)
    env["caches_after_seen"] = cache_state(caches)
    k = (int(time.time()) & 0x3FFFFFFF) | 0x40000000
    for _ in range(args.unseen_launches):
        k += 1
        go("crossover", "unseen", k)
        go("metal", "unseen", k)
    env["caches_after_unseen"] = cache_state(caches)
    env["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    env["thermal_after"] = sh(["pmset", "-g", "therm"])

    # Output equality: every launch against the Python reference for its k.
    reference = {kk: cpu_hash(args.elements, kk) for kk in sorted({r["k"] for r in launches})}
    for r in launches:
        r["reference_fnv"] = reference[r["k"]]
        r["output_equals_reference"] = r["output_fnv"] == reference[r["k"]]
    with open(out / "launches.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(launches[0].keys()))
        w.writeheader()
        w.writerows(launches)
    (out / "environment.json").write_text(json.dumps(env, indent=1))

    rows = list(csv.DictReader(open(out / "metrics.csv")))
    groups = {}
    for r in rows:
        if r["series"].startswith("discard"):
            continue
        groups.setdefault((r["series"], r["metric"]), []).append(float(r["value"]))
    summary = [{"series": s, "metric": m, "launches": len(v), "median": statistics.median(v), "min": min(v),
                "max": max(v), "p95": pct(v, 0.95)} for (s, m), v in sorted(groups.items())]
    failed = [r for r in launches if r["exit"] != 0]
    unequal = [r for r in launches if not r["output_equals_reference"]]
    result = {"launches": len(launches), "failed_launches": failed, "outputs_not_equal_to_reference": unequal,
              "reference_fnv_by_k": {str(kk): v for kk, v in reference.items()},
              "devices": sorted({(r["backend"], r["device"]) for r in launches}), "summary": summary}
    (out / "summary.json").write_text(json.dumps(result, indent=1))
    wanted = ["translate_ms", "pipeline_create_ms", "first_submission_ms", "cold_compile_ms", "warm_dispatch_median_ms",
              "warm_dispatch_p95_ms", "warm_dispatch_min_ms", "warm_gpu_median_ms"]
    lines = ["| series | metric | launches | median ms | min | max |", "|---|---|---|---|---|---|"]
    for s in summary:
        if s["metric"] in wanted:
            lines.append(f"| {s['series']} | {s['metric']} | {s['launches']} | {s['median']:.3f} | {s['min']:.3f} | {s['max']:.3f} |")
    (out / "summary.md").write_text("\n".join(lines) + "\n")
    print(f"{len(launches)} launches, {len(failed)} failed, {len(unequal)} outputs differ from the reference "
          f"-> {out / 'summary.json'}")
    return 1 if failed or unequal else 0


if __name__ == "__main__":
    sys.exit(main())
