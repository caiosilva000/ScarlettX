#!/usr/bin/env python3
"""A/B of two builds of bench_victory_prewarm, interleaved launch by launch.

    tools/sx_bench_ab.py --a <build A> --b <build B> --out <folder>

Machine state drifts over minutes by as much as the differences between two translator versions,
so the two builds are never run in separate blocks here: every (workload, condition, cache state)
is launched on A and on B back to back, the order alternating from pair to pair. The untranslated
control workload runs on both builds too; it is the same code in both, so its A/B difference is
the noise floor of the comparison.

Series: `seen` (salt 0, after discarded launches on both builds) and `unseen` (a salt no launch
has used; the two builds translate it to different bytes, so it is new to the host cache for
both). Outputs: metrics.csv and samples.csv with the series prefixed `A-` or `B-`, launches.csv,
environment.json, and ab.json with, per cell and metric, the two medians and the paired
differences.
"""
import argparse
import copy
import csv
import hashlib
import json
import platform
import statistics
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sx_bench_prewarm import EXE, WINESERVER, cache_state, run_launch, sh  # noqa: E402

CONDITIONS = ["cold", "prewarmed", "primed", "steady"]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a", required=True, help="build directory of A (the baseline)")
    ap.add_argument("--b", required=True, help="build directory of B (the candidate)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--bottle", default="scarlettx-m4")
    ap.add_argument("--sandbox", default=str(Path.home() / "m4-feasibility/retail-preflight/offline.sb"))
    ap.add_argument("--workloads", nargs="+", default=["draw", "guest"])
    ap.add_argument("--pairs", type=int, default=10)
    ap.add_argument("--unseen-pairs", type=int, default=8)
    ap.add_argument("--iterations", type=int, default=200)
    ap.add_argument("--control-iterations", type=int, default=30)
    ap.add_argument("--timeout", type=int, default=120)
    args = ap.parse_args()

    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    builds = {"A": Path(args.a).resolve(), "B": Path(args.b).resolve()}
    for name, build in builds.items():
        if not (build / EXE).exists():
            print(f"BLOCKED: {build / EXE} is missing")
            return 2
    (out / "metrics.csv").write_text("series,launch,workload,shader_path,condition,salt,shader_fnv,metric,value\n")
    (out / "samples.csv").write_text("series,launch,workload,condition,salt,iteration,record_ms,submit_to_fence_ms,gpu_ms\n")

    def info(build):
        d = {}
        f = build / "build-info.txt"
        for line in f.read_text().splitlines() if f.exists() else []:
            k, sep, v = line.partition("=")
            if sep:
                d[k] = v
        return {"build": str(build), "build_info": d, "exe_sha256": hashlib.sha256((build / EXE).read_bytes()).hexdigest()}

    env = {"started": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "machine": platform.machine(), "macos": platform.mac_ver()[0],
           "cpu": sh(["sysctl", "-n", "machdep.cpu.brand_string"]),
           "crossover": sh(["defaults", "read", "/Applications/CrossOver.app/Contents/Info", "CFBundleShortVersionString"]),
           "bottle": args.bottle, "power": sh(["pmset", "-g", "batt"]), "thermal": sh(["pmset", "-g", "therm"]),
           "A": info(builds["A"]), "B": info(builds["B"]), "pairs": args.pairs, "unseen_pairs": args.unseen_pairs,
           "iterations": args.iterations, "caches_before": cache_state(),
           "note": "both builds run under the same executable name, so they share one D3DMetal cache directory"}
    subprocess.run([WINESERVER, "--bottle", args.bottle, "-k"], capture_output=True)
    time.sleep(2)

    side_args = {}
    for name, build in builds.items():
        a = copy.copy(args)
        a.build = str(build)
        side_args[name] = a
    launches = []
    n = 0
    flip = False

    def pair(series, workload, condition, salt, iterations):
        nonlocal n, flip
        order = ("B", "A") if flip else ("A", "B")
        flip = not flip
        for side in order:
            n += 1
            row, _ = run_launch(side_args[side], n, f"{side}-{series}", workload, condition, salt, iterations, out)
            row["side"] = side
            launches.append(row)
            print(f"{n:4d} {side} {series:8s} {workload:8s} {condition:10s} salt={salt:<11d} exit={row['exit']}", flush=True)

    for workload in args.workloads:
        pair("discard", workload, "cold", 0, 10)
    pair("discard", "control", "cold", 0, 3)
    for _ in range(args.pairs):
        for workload in args.workloads:
            for condition in CONDITIONS:
                pair("seen", workload, condition, 0, args.iterations)
        pair("seen", "control", "cold", 0, args.control_iterations)
    env["caches_after_seen"] = cache_state()
    salt = (int(time.time()) & 0x3FFFFFFF) | 0x40000000
    for _ in range(args.unseen_pairs):
        for workload in args.workloads:
            for condition in ("cold", "prewarmed", "primed"):
                salt += 1
                pair("unseen", workload, condition, salt, args.iterations)
    env["caches_after_unseen"] = cache_state()
    env["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    env["thermal_after"] = sh(["pmset", "-g", "therm"])
    with open(out / "launches.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(launches[0].keys()))
        w.writeheader()
        w.writerows(launches)
    (out / "environment.json").write_text(json.dumps(env, indent=1))

    # Pair the launches up again: the k-th A and the k-th B of a cell were run back to back.
    cells = {}
    for r in csv.DictReader(open(out / "metrics.csv")):
        side, series = r["series"].split("-", 1)
        if series == "discard":
            continue
        cells.setdefault((series, r["workload"], r["condition"], r["metric"]), {"A": [], "B": []})[side].append(float(r["value"]))
    ab = []
    for (series, workload, condition, metric), v in sorted(cells.items()):
        a, b = v["A"], v["B"]
        k = min(len(a), len(b))
        if k == 0:
            continue
        diffs = [b[i] - a[i] for i in range(k)]
        ma = statistics.median(a)
        ab.append({"series": series, "workload": workload, "condition": condition, "metric": metric, "pairs": k,
                   "median_A": ma, "median_B": statistics.median(b), "median_paired_difference": statistics.median(diffs),
                   "percent": (statistics.median(diffs) / ma * 100.0) if ma else None,
                   "B_higher_in_pairs": sum(d > 0 for d in diffs), "B_lower_in_pairs": sum(d < 0 for d in diffs),
                   "min_difference": min(diffs), "max_difference": max(diffs)})
    failed = [r for r in launches if r["exit"] != 0]
    (out / "ab.json").write_text(json.dumps({"launches": len(launches), "failed_launches": failed, "cells": ab}, indent=1))
    print(f"{len(launches)} launches, {len(failed)} failed -> {out / 'ab.json'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
