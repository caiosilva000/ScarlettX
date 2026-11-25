#!/usr/bin/env python3
"""Launches bench_victory_prewarm many times through CrossOver and collects its timings.

    tools/sx_bench_prewarm.py --build <build dir inside the bottle> --out <folder>

Series:
  seen     every launch uses the plain fixture (salt 0), after one discarded launch per workload,
           so the host's on-disk shader caches have seen these shaders. This is the main matrix:
           workloads compute and draw under cold, prewarmed, primed and steady, plus the control.
  unseen   every launch uses a salt no launch has used, so the translated shader's bytes are new
           to every cache: the first-ever cost of a shader, without deleting any cache.
  freshcache  every launch runs a copy of the benchmark under a new name. D3DMetal keeps its shader
           cache per executable name, so the copy starts with an empty one.

Each launch is a fresh process inside the offline sandbox. A fresh process is cold ScarlettX and
D3D12 device state only; whether the host's caches are warm is what the two series separate.
Launches are interleaved round-robin so drift spreads evenly over the conditions.

Outputs in --out: metrics.csv, samples.csv, launches.csv (one row per launch with its exit code),
logs/<n>.stdout|.stderr, environment.json, summary.json and summary.md.
"""
import argparse
import csv
import hashlib
import json
import os
import platform
import statistics
import subprocess
import sys
import time
from pathlib import Path

WINE = "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine"
WINESERVER = "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wineserver"
EXE = "bench_victory_prewarm.exe"
CONDITIONS = ["cold", "prewarmed", "primed", "steady"]
WORKLOADS = ["compute", "draw"]


def sh(cmd, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=30, **kw).stdout.strip()
    except Exception as exc:  # noqa: BLE001 - recorded, not fatal
        return f"unavailable: {exc}"


def win_path(p: Path) -> str:
    return "Z:" + str(p).replace("/", "\\")


def cache_state():
    """Size and file count of the caches CrossOver's wine processes write under the user cache dir."""
    root = Path(sh(["getconf", "DARWIN_USER_CACHE_DIR"]))
    state = {}
    # D3DMetal keeps a shader cache per executable name; the Metal caches of the wine loader
    # process sit beside it.
    for name in (f"d3dm/{EXE}", "com.codeweavers.CrossOver.wineloader", "com.codeweavers.CrossOver"):
        d = root / name
        files = [f for f in d.rglob("*") if f.is_file()] if d.exists() else []
        state[str(d)] = {"exists": d.exists(), "files": len(files), "bytes": sum(f.stat().st_size for f in files)}
    return state


def run_launch(args, n, series, workload, condition, salt, iterations, out, hold_ms=0, exe_name=EXE):
    logs = out / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    exe = "C:\\" + str(Path(args.build).resolve()).split("drive_c/", 1)[1].replace("/", "\\") + "\\" + exe_name
    cmd = ["sandbox-exec", "-f", args.sandbox, WINE, "--bottle", args.bottle, exe,
           "--workload", workload, "--condition", condition, "--series", series, "--launch", str(n),
           "--salt", str(salt), "--iterations", str(iterations),
           "--csv", win_path(out / "metrics.csv"), "--samples", win_path(out / "samples.csv")]
    if hold_ms:
        cmd += ["--hold-ms", str(hold_ms)]
    env = dict(os.environ)
    env.setdefault("WINEDEBUG", "fixme-all")
    start = time.time()
    with open(logs / f"{n:04d}.stdout", "wb") as so, open(logs / f"{n:04d}.stderr", "wb") as se:
        proc = subprocess.Popen(cmd, stdout=so, stderr=se, env=env, cwd=str(Path.home()))
        evidence = None
        if hold_ms:
            # While the process is held open: what graphics code has it mapped?
            time.sleep(max(1.0, hold_ms / 1000 - 1.5))
            pids = sh(["pgrep", "-f", EXE]).split()
            evidence = {"pids": pids, "mapped": {}}
            for pid in pids:
                mapped = sh(["vmmap", pid])
                hits = sorted({line.split("/")[-1].strip() for line in mapped.splitlines()
                               if any(k in line for k in ("D3DMetal", "Metal.framework", "MoltenVK", "libd3dshared",
                                                          "dxvk", "vkd3d"))})
                evidence["mapped"][pid] = hits[:40]
        try:
            code = proc.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            code = "timeout"
    return {"launch": n, "series": series, "workload": workload, "condition": condition, "salt": salt,
            "exit": code, "seconds": round(time.time() - start, 2)}, evidence


def pct(values, p):
    v = sorted(values)
    if not v:
        return None
    rank = p * (len(v) - 1)
    lo = int(rank)
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (rank - lo)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--bottle", default="scarlettx-m4")
    ap.add_argument("--sandbox", default=str(Path.home() / "m4-feasibility/retail-preflight/offline.sb"))
    ap.add_argument("--launches", type=int, default=10, help="launches per workload and condition, seen series")
    ap.add_argument("--unseen-launches", type=int, default=6)
    ap.add_argument("--fresh-cache-launches", type=int, default=3,
                    help="launches per workload and condition of a copy with an empty D3DMetal cache")
    ap.add_argument("--iterations", type=int, default=200, help="steady submissions per launch")
    ap.add_argument("--control-iterations", type=int, default=30)
    ap.add_argument("--timeout", type=int, default=120)
    args = ap.parse_args()

    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    build = Path(args.build).resolve()
    exe = build / EXE
    if not exe.exists():
        print(f"BLOCKED: {exe} is missing (build with SCARLETTX_BENCHMARKS=ON SCARLETTX_VICTORY_SHADER_BRIDGE=ON)")
        return 2
    (out / "metrics.csv").write_text("series,launch,workload,shader_path,condition,salt,shader_fnv,metric,value\n")
    (out / "samples.csv").write_text("series,launch,workload,condition,salt,iteration,record_ms,submit_to_fence_ms,gpu_ms\n")

    build_info = {}
    for line in (build / "build-info.txt").read_text().splitlines() if (build / "build-info.txt").exists() else []:
        k, sep, v = line.partition("=")
        if sep:
            build_info[k] = v
    env = {
        "started": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "machine": platform.machine(), "macos": platform.mac_ver()[0],
        "cpu": sh(["sysctl", "-n", "machdep.cpu.brand_string"]),
        "crossover": sh(["defaults", "read", "/Applications/CrossOver.app/Contents/Info", "CFBundleShortVersionString"]),
        "bottle": args.bottle, "sandbox_profile": args.sandbox,
        "power": sh(["pmset", "-g", "batt"]), "thermal": sh(["pmset", "-g", "therm"]),
        "build": str(build), "build_info": build_info,
        "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
        "launches": args.launches, "unseen_launches": args.unseen_launches, "iterations": args.iterations,
        "wineserver": "restarted once before the first launch, then left running",
        "caches_before": cache_state(),
    }
    subprocess.run([WINESERVER, "--bottle", args.bottle, "-k"], capture_output=True)
    time.sleep(2)

    launches = []
    n = 0

    def go(series, workload, condition, salt, iterations, hold_ms=0, exe_name=EXE):
        nonlocal n
        n += 1
        row, evidence = run_launch(args, n, series, workload, condition, salt, iterations, out, hold_ms, exe_name)
        launches.append(row)
        print(f"{n:4d} {series:8s} {workload:8s} {condition:10s} salt={salt:<11d} exit={row['exit']} {row['seconds']}s",
              flush=True)
        return evidence

    # Discarded launches: bring wine up and let the host's caches see the plain fixtures. The
    # first one is held open so the process's mapped graphics libraries can be recorded.
    env["route_evidence"] = go("discard", "compute", "cold", 0, 10, hold_ms=6000)
    go("discard", "draw", "cold", 0, 10)
    go("discard", "control", "cold", 0, 3)
    env["caches_after_discard"] = cache_state()

    # Main matrix, round-robin.
    for _ in range(args.launches):
        for workload in WORKLOADS:
            for condition in CONDITIONS:
                go("seen", workload, condition, 0, args.iterations)
        go("seen", "control", "cold", 0, args.control_iterations)
    env["caches_after_seen"] = cache_state()

    # Shaders no cache has seen: a salt from the clock, then counting up.
    salt = (int(time.time()) & 0x3FFFFFFF) | 0x40000000
    for _ in range(args.unseen_launches):
        for workload in WORKLOADS:
            for condition in ("cold", "prewarmed", "primed"):
                salt += 1
                go("unseen", workload, condition, salt, args.iterations)
    env["caches_after_unseen"] = cache_state()

    # An empty host cache without deleting one: D3DMetal's cache is per executable name, so a copy
    # of the benchmark under a name never used before starts with none.
    fresh = []
    for i in range(args.fresh_cache_launches):
        for workload in WORKLOADS:
            for condition in ("cold", "primed"):
                salt += 1
                name = f"bench_fresh_{salt:x}.exe"
                (build / name).write_bytes(exe.read_bytes())
                fresh.append(name)
                go("freshcache", workload, condition, 0, args.iterations, exe_name=name)
    env["fresh_cache_executables"] = fresh
    env["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    env["thermal_after"] = sh(["pmset", "-g", "therm"])

    with open(out / "launches.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(launches[0].keys()))
        w.writeheader()
        w.writerows(launches)
    (out / "environment.json").write_text(json.dumps(env, indent=1))

    # Summary: per series, workload, condition and metric, the median and spread across launches.
    rows = list(csv.DictReader(open(out / "metrics.csv")))
    groups = {}
    for r in rows:
        if r["series"] == "discard":
            continue
        groups.setdefault((r["series"], r["workload"], r["condition"], r["metric"]), []).append(float(r["value"]))
    summary = []
    for (series, workload, condition, metric), values in sorted(groups.items()):
        summary.append({"series": series, "workload": workload, "condition": condition, "metric": metric,
                        "launches": len(values), "median": statistics.median(values), "min": min(values),
                        "max": max(values), "p95": pct(values, 0.95)})
    failed = [r for r in launches if r["exit"] != 0]
    result = {"launches": len(launches), "failed_launches": failed, "summary": summary}
    (out / "summary.json").write_text(json.dumps(result, indent=1))

    wanted = ["translate_ms", "pipeline_create_ms", "first_submission_ms", "first_use_latency_ms", "preparation_ms",
              "total_to_first_result_ms", "steady_submission_median_ms", "steady_submission_p95_ms",
              "translate_repeat_ms", "pipeline_recreate_ms", "control_first_call_ms",
              "control_repeat_call_median_ms", "control_repeat_call_p95_ms"]
    lines = ["| series | workload | condition | metric | launches | median ms | min | max |", "|---|---|---|---|---|---|---|---|"]
    for s in summary:
        if s["metric"] in wanted:
            lines.append(f"| {s['series']} | {s['workload']} | {s['condition']} | {s['metric']} | {s['launches']} | "
                         f"{s['median']:.3f} | {s['min']:.3f} | {s['max']:.3f} |")
    (out / "summary.md").write_text("\n".join(lines) + "\n")
    print(f"{len(launches)} launches, {len(failed)} failed -> {out / 'summary.json'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
