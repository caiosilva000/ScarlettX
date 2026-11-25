#!/usr/bin/env python3
"""Measures what a shader or pipeline cache could save, by shader size.

    tools/sx_bench_cache.py --build <build dir inside the bottle> --out <folder>

For each size (--rounds of the vector kernel) it launches `bench_victory_prewarm --workload cache`
in fresh processes, alternating two cache states:
  seen     one literal per size, after a discarded launch: the host's on-disk cache has this shader
  unseen   a literal no launch has used: a shader new to every cache
Each launch reports the translator's first call in the process, this shader's translation (and a
repeat), pipeline creation the first time and again, whether the host hands out pipeline blobs,
and validates the shader's output. Nothing is deleted.
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

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sx_bench_prewarm import EXE, WINE, WINESERVER, cache_state, pct, sh, win_path  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--bottle", default="scarlettx-m4")
    ap.add_argument("--sandbox", default=str(Path.home() / "m4-feasibility/retail-preflight/offline.sb"))
    ap.add_argument("--rounds", type=int, nargs="+", default=[1, 32, 256, 1024])
    ap.add_argument("--launches", type=int, default=6)
    ap.add_argument("--timeout", type=int, default=120)
    args = ap.parse_args()

    out = Path(args.out).resolve()
    (out / "logs").mkdir(parents=True, exist_ok=True)
    build = Path(args.build).resolve()
    exe = build / EXE
    if not exe.exists():
        print(f"BLOCKED: {exe} is missing")
        return 2
    (out / "metrics.csv").write_text("series,launch,workload,shader_path,condition,salt,shader_fnv,metric,value\n")
    env = {
        "started": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "machine": platform.machine(), "macos": platform.mac_ver()[0],
        "cpu": sh(["sysctl", "-n", "machdep.cpu.brand_string"]),
        "crossover": sh(["defaults", "read", "/Applications/CrossOver.app/Contents/Info", "CFBundleShortVersionString"]),
        "bottle": args.bottle, "power": sh(["pmset", "-g", "batt"]), "thermal": sh(["pmset", "-g", "therm"]),
        "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "rounds": args.rounds, "launches": args.launches,
        "caches_before": cache_state(),
    }
    subprocess.run([WINESERVER, "--bottle", args.bottle, "-k"], capture_output=True)
    time.sleep(2)
    exe_win = "C:\\" + str(build).split("drive_c/", 1)[1].replace("/", "\\") + "\\" + EXE
    launches = []
    n = 0

    def go(series, rounds, k, extra=()):
        nonlocal n
        n += 1
        cmd = ["sandbox-exec", "-f", args.sandbox, WINE, "--bottle", args.bottle, exe_win, "--workload", "cache",
               "--condition", "cold", "--series", f"{series}-r{rounds}", "--launch", str(n), "--salt", str(k),
               "--rounds", str(rounds), "--csv", win_path(out / "metrics.csv"), *extra]
        envp = dict(os.environ)
        envp.setdefault("WINEDEBUG", "fixme-all")
        start = time.time()
        with open(out / "logs" / f"{n:04d}.stdout", "wb") as so, open(out / "logs" / f"{n:04d}.stderr", "wb") as se:
            try:
                code = subprocess.run(cmd, stdout=so, stderr=se, env=envp, cwd=str(Path.home()),
                                      timeout=args.timeout).returncode
            except subprocess.TimeoutExpired:
                code = "timeout"
        launches.append({"launch": n, "series": series, "rounds": rounds, "k": k, "exit": code,
                         "seconds": round(time.time() - start, 2)})
        print(f"{n:4d} {series:8s} rounds={rounds:<5d} k={k:<11d} exit={code} {launches[-1]['seconds']}s", flush=True)

    fresh = (int(time.time()) & 0x3FFFFFFF) | 0x40000000
    for rounds in args.rounds:
        go("discard", rounds, 5000 + rounds)  # the seen literal for this size
    for _ in range(args.launches):
        for rounds in args.rounds:
            go("seen", rounds, 5000 + rounds)
            fresh += 1
            go("unseen", rounds, fresh)
    # Pipeline blobs with a damaged copy offered back, where the host hands any out.
    go("blobcheck", args.rounds[-1], 5000 + args.rounds[-1], extra=("--corrupt-blob",))
    env["caches_after"] = cache_state()
    env["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    env["thermal_after"] = sh(["pmset", "-g", "therm"])

    with open(out / "launches.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(launches[0].keys()))
        w.writeheader()
        w.writerows(launches)
    (out / "environment.json").write_text(json.dumps(env, indent=1))
    groups = {}
    for r in csv.DictReader(open(out / "metrics.csv")):
        if r["series"].startswith(("discard", "blobcheck")):
            continue
        groups.setdefault((r["series"], r["metric"]), []).append(float(r["value"]))
    summary = [{"series": s, "metric": m, "launches": len(v), "median": statistics.median(v), "min": min(v),
                "max": max(v), "p95": pct(v, 0.95)} for (s, m), v in sorted(groups.items())]
    failed = [r for r in launches if r["exit"] != 0]
    (out / "summary.json").write_text(json.dumps({"launches": len(launches), "failed_launches": failed,
                                                  "summary": summary}, indent=1))
    print(f"{len(launches)} launches, {len(failed)} failed -> {out / 'summary.json'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
