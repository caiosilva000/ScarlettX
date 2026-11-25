#!/usr/bin/env python3
"""Launches bench_sx_fps through CrossOver and collects its numbers (docs/fps-benchmark.md).

    tools/sx_bench_fps.py --build <build dir inside the bottle> --out <folder> --suite offscreen
    tools/sx_bench_fps.py --build <dir> --out <folder> --suite windowed
    tools/sx_bench_fps.py --build <A> --build-b <B> --out <folder> --suite ab

Every launch is a fresh process inside the offline sandbox. Machine state drifts over minutes by
several percent, so things that are compared are never run in separate blocks: the variants of a
comparison (or the two builds of an A/B) are launched back to back, and the order rotates from one
repetition to the next.

Suites:
  offscreen  uncapped throughput with a fixed number of frames in flight, per stress mode:
             draws (fixed against pertable at each draw count), pixel (light against heavy at each
             resolution), upload (percopy against each one-change variant and the guest-memory
             route), and a short serialized series that is labelled as such.
  windowed   PresentX with a target interval, and once without: present-call intervals from the
             benchmark, and window captures taken while it runs, decoded for the frame number the
             benchmark draws along the bottom edge.
  ab         two builds on the same cells, interleaved launch by launch.

Before each launch the driver looks for another build or test on the machine and waits for it; a
launch that started while one was running is marked in launches.csv.

Outputs in --out: metrics.csv, frames.csv (every timed frame), launches.csv, logs/<n>.stdout|.stderr,
environment.json, summary.json, summary.md, and captures/ with captures.csv for the windowed suite.
This driver never stops the wineserver and never deletes a cache.
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
import threading
import time
from pathlib import Path

WINE = "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine"
EXE = "bench_sx_fps.exe"
BUSY_PATTERN = r"ninja|cargo|clang|test_.*\.exe|bench_victory"
METRICS_HEADER = "series,launch,mode,variant,path,param,inflight,metric,value\n"
FRAMES_HEADER = ("series,launch,mode,variant,path,param,inflight,frame,gframe,t_start_us,wait_us,pace_us,prep_us,"
                 "submit_us,present_us,serialized_us,present_unix_us,completed_after,draws,table_binds,submissions,"
                 "uploads,upload_bytes\n")
KEY_METRICS = ["completed_fps", "serialized_frames_per_second", "serialized_submit_to_fence_ms_median",
               "frame_interval_ms_median", "frame_interval_ms_p95", "frame_interval_ms_p99", "cpu_prep_ms_median",
               "cpu_prep_ms_p95", "submit_ms_median", "submit_ms_p95", "fence_wait_ms_median", "fence_wait_ms_total",
               "draws_per_frame", "table_binds_per_frame", "submissions_per_frame", "uploads_per_frame",
               "upload_bytes_per_frame", "present_call_interval_ms_median", "present_call_interval_ms_p95",
               "present_call_interval_ms_p99", "present_call_ms_median", "presented_fps"]


def sh(cmd, timeout=30):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout).stdout.strip()
    except Exception as exc:  # noqa: BLE001 - recorded, not fatal
        return f"unavailable: {exc}"


def win_path(p: Path) -> str:
    return "Z:" + str(p).replace("/", "\\")


def exe_in_bottle(build: Path) -> str:
    return "C:\\" + str(build).split("drive_c/", 1)[1].replace("/", "\\") + "\\" + EXE


def others_running():
    """Builds and tests of other sessions on this machine (this driver's own launches excluded)."""
    out = sh(["pgrep", "-fl", BUSY_PATTERN])
    return [line for line in out.splitlines() if line and EXE not in line and "sx_bench_fps" not in line
            and not line.startswith("unavailable")]


def wait_until_quiet(limit_s):
    start = time.time()
    busy = others_running()
    while busy and time.time() - start < limit_s:
        time.sleep(3)
        busy = others_running()
    return busy, round(time.time() - start, 1)


def cache_state():
    root = Path(sh(["getconf", "DARWIN_USER_CACHE_DIR"]))
    d = root / "d3dm" / EXE
    files = [f for f in d.rglob("*") if f.is_file()] if d.exists() else []
    return {str(d): {"exists": d.exists(), "files": len(files), "bytes": sum(f.stat().st_size for f in files)}}


def build_record(build: Path):
    info = {}
    f = build / "build-info.txt"
    for line in f.read_text().splitlines() if f.exists() else []:
        k, sep, v = line.partition("=")
        if sep:
            info.setdefault(k, v)
    return {"build": str(build), "build_info": info,
            "exe_sha256": hashlib.sha256((build / EXE).read_bytes()).hexdigest(),
            "d3d12_xs_dll_sha256": hashlib.sha256((build / "d3d12_xs.dll").read_bytes()).hexdigest()}


class Runner:
    def __init__(self, args, out: Path):
        self.args = args
        self.out = out
        self.logs = out / "logs"
        self.logs.mkdir(parents=True, exist_ok=True)
        self.n = 0
        self.launches = []
        (out / "metrics.csv").write_text(METRICS_HEADER)
        (out / "frames.csv").write_text(FRAMES_HEADER)

    def launch(self, build: Path, series, cell, side="", hold_ms=0, capture=None, extra_env=None, quiet=True):
        """One process. `cell` is a dict of bench arguments (mode, variant, path, ...)."""
        self.n += 1
        n = self.n
        busy, waited = wait_until_quiet(self.args.busy_wait) if quiet else ([], 0)
        cmd = ["sandbox-exec", "-f", self.args.sandbox, WINE, "--bottle", self.args.bottle, exe_in_bottle(build),
               "--series", series, "--launch", str(n), "--csv", win_path(self.out / "metrics.csv"),
               "--frames-csv", win_path(self.out / "frames.csv")]
        for k, v in cell.items():
            cmd += [f"--{k}", str(v)]
        if hold_ms:
            cmd += ["--hold-ms", str(hold_ms)]
        if cell.get("path") == "windowed":
            # The window needs the display awake for the length of the launch.
            cmd = ["caffeinate", "-d", "-u"] + cmd
        env = dict(os.environ)
        env.setdefault("WINEDEBUG", "fixme-all")
        env.update(extra_env or {})
        start = time.time()
        route = None
        with open(self.logs / f"{n:04d}.stdout", "wb") as so, open(self.logs / f"{n:04d}.stderr", "wb") as se:
            proc = subprocess.Popen(cmd, stdout=so, stderr=se, env=env, cwd=str(Path.home()))
            stop = threading.Event()
            worker = None
            if capture is not None:
                worker = threading.Thread(target=capture, args=(n, proc, stop))
                worker.start()
            if hold_ms:
                # While the process is held open after its run: which graphics code is mapped in it?
                deadline = time.time() + self.args.timeout
                while time.time() < deadline and b"timed loop started" not in (self.logs / f"{n:04d}.stdout").read_bytes():
                    time.sleep(0.2)
                route = {}
                for pid in sh(["pgrep", "-f", EXE]).split():
                    mapped = sh(["vmmap", pid])
                    hits = sorted({line.split()[-1] for line in mapped.splitlines()
                                   if any(k in line for k in ("D3DMetal", "d3d12.dll", "Metal.framework/Versions/A/Metal",
                                                              "libmetalirconverter", "MoltenVK", "dxvk", "vkd3d",
                                                              "d3d12_xs.dll"))})
                    if hits:
                        route[pid] = hits
            try:
                code = proc.wait(timeout=self.args.timeout)
            except subprocess.TimeoutExpired:
                proc.kill()
                code = "timeout"
            stop.set()
            if worker:
                worker.join()
        row = {"launch": n, "series": series, "side": side, "mode": cell["mode"], "variant": cell["variant"],
               "path": cell.get("path", "offscreen"),
               "args": " ".join(f"--{k} {v}" for k, v in cell.items()), "exit": code,
               "seconds": round(time.time() - start, 2), "waited_for_quiet_s": waited,
               "others_running_at_start": len(busy), "others_running_at_end": len(others_running()),
               "load1": round(os.getloadavg()[0], 2)}
        self.launches.append(row)
        print(f"{n:4d} {side:1s} {series:10s} {cell['mode']:6s} {cell['variant']:13s} {row['path']:10s} "
              f"exit={code} shared={'yes' if row['others_running_at_start'] or row['others_running_at_end'] else 'no'}",
              flush=True)
        return row, route

    def group(self, build, series, cells, repetitions, side=""):
        """The cells of one comparison, back to back, the order rotating with the repetition."""
        for r in range(repetitions):
            order = cells[r % len(cells):] + cells[:r % len(cells)]
            if (r // len(cells)) % 2:
                order = order[::-1]
            for cell in order:
                self.launch(build, series, cell, side)

    def finish(self, env):
        with open(self.out / "launches.csv", "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(self.launches[0].keys()))
            w.writeheader()
            w.writerows(self.launches)
        env["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
        env["thermal_after"] = sh(["pmset", "-g", "therm"])
        env["power_after"] = sh(["pmset", "-g", "batt"])
        env["caches_after"] = cache_state()
        env["launches"] = len(self.launches)
        env["launches_with_other_work_running"] = sum(
            1 for r in self.launches if r["others_running_at_start"] or r["others_running_at_end"])
        (self.out / "environment.json").write_text(json.dumps(env, indent=1))
        return summarize(self.out, self.launches)


def pct(values, p):
    v = sorted(values)
    if not v:
        return None
    rank = p * (len(v) - 1)
    lo = int(rank)
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (rank - lo)


def summarize(out: Path, launches):
    """Per cell and metric: the median over launches with its spread, and per comparison the paired
    differences of each candidate against the group's first cell."""
    ok = {r["launch"] for r in launches if r["exit"] == 0}
    by_launch = {}
    cells = {}
    for r in csv.DictReader(open(out / "metrics.csv")):
        launch = int(r["launch"])
        if launch not in ok or r["series"].endswith("discard"):
            continue
        key = (r["series"], r["mode"], r["path"], r["param"], r["variant"])
        cells.setdefault(key, {}).setdefault(r["metric"], []).append(float(r["value"]))
        by_launch.setdefault(launch, {"key": key})[r["metric"]] = float(r["value"])
    rows = []
    for key, metrics in sorted(cells.items()):
        row = {"series": key[0], "mode": key[1], "path": key[2], "param": key[3], "variant": key[4],
               "launches": len(next(iter(metrics.values())))}
        for m in KEY_METRICS:
            if m in metrics:
                v = metrics[m]
                row[m] = {"median": statistics.median(v), "min": min(v), "max": max(v)}
        row["checked_pixels"] = sum(metrics.get("checked_pixels", []))
        row["exact_pixels"] = sum(metrics.get("exact_pixels", []))
        row["fences_signaled"] = sum(metrics.get("fences_signaled", []))
        row["fences_completed"] = sum(metrics.get("fences_completed", []))
        rows.append(row)
    # Pooled per-frame percentiles from the raw frames (all launches of a cell together).
    pooled = {}
    last = {}
    for r in csv.DictReader(open(out / "frames.csv")):
        launch = int(r["launch"])
        if launch not in ok or r["series"].endswith("discard"):
            continue
        key = (r["series"], r["mode"], r["path"], r["param"], r["variant"])
        p = pooled.setdefault(key, {"interval": [], "prep": [], "submit": [], "wait": []})
        t = float(r["t_start_us"])
        if launch in last and int(r["frame"]) > 0:
            p["interval"].append((t - last[launch]) / 1000.0)
        last[launch] = t
        p["prep"].append(float(r["prep_us"]) / 1000.0)
        p["submit"].append(float(r["submit_us"]) / 1000.0)
        p["wait"].append(float(r["wait_us"]) / 1000.0)
    for row in rows:
        p = pooled.get((row["series"], row["mode"], row["path"], row["param"], row["variant"]))
        if p:
            row["pooled_frames"] = len(p["prep"])
            for name, v in p.items():
                row[f"pooled_{name}_ms"] = {"median": pct(v, 0.5), "p95": pct(v, 0.95), "p99": pct(v, 0.99)}
    failed = [r for r in launches if r["exit"] != 0]
    summary = {"launches": len(launches), "failed_launches": failed, "cells": rows}
    (out / "summary.json").write_text(json.dumps(summary, indent=1))

    def cell(row, m, digits=1):
        if m not in row:
            return ""
        v = row[m]
        return f"{v['median']:.{digits}f} ({v['min']:.{digits}f}..{v['max']:.{digits}f})"

    lines = ["# bench_sx_fps summary", "",
             f"{len(launches)} launches, {len(failed)} failed. Median over launches (min..max). Wall-clock times.", "",
             "| series | mode | path | param | variant | n | completed fps | frame ms p50 / p95 / p99 (pooled) | "
             "CPU prep ms | submit ms | fence wait ms | draws | binds | submits | uploads | upload bytes | exact / checked |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for row in rows:
        fps = cell(row, "completed_fps") or ("serialized: " + cell(row, "serialized_frames_per_second"))
        pi = row.get("pooled_interval_ms")
        frame = f"{pi['median']:.3f} / {pi['p95']:.3f} / {pi['p99']:.3f}" if pi and pi["median"] is not None else ""
        lines.append(
            f"| {row['series']} | {row['mode']} | {row['path']} | {row['param']} | {row['variant']} | {row['launches']} | "
            f"{fps} | {frame} | {cell(row, 'cpu_prep_ms_median', 3)} | {cell(row, 'submit_ms_median', 3)} | "
            f"{cell(row, 'fence_wait_ms_median', 3)} | {row.get('draws_per_frame', {}).get('median', '')} | "
            f"{row.get('table_binds_per_frame', {}).get('median', '')} | "
            f"{row.get('submissions_per_frame', {}).get('median', '')} | "
            f"{row.get('uploads_per_frame', {}).get('median', '')} | "
            f"{row.get('upload_bytes_per_frame', {}).get('median', '')} | "
            f"{int(row['exact_pixels'])} / {int(row['checked_pixels'])} |")
    (out / "summary.md").write_text("\n".join(lines) + "\n")
    return summary


# ---------------------------------------------------------------------------------------------
# Windowed suite: captures of the window, decoded for the frame number drawn along the bottom.
# ---------------------------------------------------------------------------------------------

def classify(rgb):
    r, g, b = rgb[:3]
    if r > 200 and g > 200 and b > 200:
        return "white"
    if r < 60 and g < 60 and b < 60:
        return "black"
    if r > 180 and g < 110 and b > 180:
        return "magenta"
    if r < 140 and g > 180 and b > 180:
        return "cyan"
    if r > 180 and g < 110 and b < 110:
        return "red"
    if r < 160 and g > 180 and b < 140:
        return "green"
    return "other"


def decode_capture(path, rt_w, rt_h):
    """The frame number in a capture of the window, or None. The client area is the bottom 16:9 part
    of the image; the markers are squares of rt_w/32 along its bottom edge."""
    from PIL import Image
    img = Image.open(path).convert("RGB")
    w, h = img.size
    client_h = w * 9 / 16
    block_h = (rt_w / 32) / rt_h * client_h
    y = int(h - block_h / 2)
    colours = [classify(img.getpixel((int((k + 0.5) * w / 32), y))) for k in range(19)]
    if colours[17] != "red" or colours[18] != "green" or colours[16] not in ("magenta", "cyan"):
        return None, colours
    frame = 0
    for k in range(16):
        if colours[k] == "white":
            frame |= 1 << k
        elif colours[k] != "black":
            return None, colours
    if (frame & 1) != (1 if colours[16] == "magenta" else 0):
        return None, colours
    return frame, colours


def make_capturer(out: Path, winlist, shots):
    cap_dir = out / "captures"
    cap_dir.mkdir(exist_ok=True)

    def capture(n, proc, stop):
        while proc.poll() is None and not stop.is_set():
            listing = sh([winlist, "ScarlettX - Xbox"]).splitlines()
            listing = [line for line in listing if line and not line.startswith("unavailable")]
            if not listing:
                time.sleep(0.05)
                continue
            wid = listing[0].split("\t")[0]
            t0 = time.time()
            path = cap_dir / f"{n:04d}-{int(t0 * 1000)}.png"
            subprocess.run(["screencapture", "-x", "-o", "-l", wid, "-t", "png", str(path)], check=False,
                           capture_output=True)
            t1 = time.time()
            if path.exists():
                shots.append({"launch": n, "t0_unix_us": int(t0 * 1e6), "t1_unix_us": int(t1 * 1e6), "path": path})
            time.sleep(0.02)
    return capture


def analyse_captures(out: Path, shots, keep):
    """Matches every capture with the frames presented around it. Keeps `keep` image files per launch."""
    presents = {}
    target = {}
    for r in csv.DictReader(open(out / "frames.csv")):
        if int(r["present_unix_us"]):
            presents.setdefault(int(r["launch"]), []).append((int(r["present_unix_us"]), int(r["gframe"])))
    for log in sorted((out / "logs").glob("*.stdout")):
        for line in log.read_text(errors="replace").splitlines():
            if line.startswith("[fps] target="):
                w, h = line.split("target=")[1].split()[0].split("x")
                target[int(log.stem)] = (int(w), int(h))
    rows = []
    kept = {}
    for s in shots:
        n = s["launch"]
        if n not in target:
            continue
        frame, colours = decode_capture(s["path"], *target[n])
        before = [g for t, g in presents.get(n, []) if t <= s["t0_unix_us"]]
        during = [g for t, g in presents.get(n, []) if t <= s["t1_unix_us"]]
        rows.append({"launch": n, "t0_unix_us": s["t0_unix_us"], "t1_unix_us": s["t1_unix_us"],
                     "decoded_frame": "" if frame is None else frame,
                     "last_presented_before_capture": before[-1] if before else "",
                     "last_presented_by_capture_end": during[-1] if during else "",
                     "in_timed_loop": bool(before) and bool(presents.get(n)) and s["t1_unix_us"] <= presents[n][-1][0],
                     "file": s["path"].name if kept.get(n, 0) < keep else ""})
        if kept.get(n, 0) < keep:
            kept[n] = kept.get(n, 0) + 1
        else:
            s["path"].unlink()
    if rows:
        with open(out / "captures.csv", "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
    per_launch = {}
    for n in sorted({r["launch"] for r in rows}):
        mine = [r for r in rows if r["launch"] == n]
        decoded = [r for r in mine if r["decoded_frame"] != ""]
        timed = [r for r in decoded if r["in_timed_loop"]]
        frames = [r["decoded_frame"] for r in decoded]
        rec = {"captures": len(mine), "decoded": len(decoded), "distinct_frames": len(set(frames)),
               "never_went_back": all(b >= a for a, b in zip(frames, frames[1:]))}
        if len(timed) >= 2:
            span_s = (timed[-1]["t0_unix_us"] - timed[0]["t0_unix_us"]) / 1e6
            rec["frames_advanced_on_screen"] = timed[-1]["decoded_frame"] - timed[0]["decoded_frame"]
            rec["seconds_between_first_and_last_capture"] = round(span_s, 3)
            rec["on_screen_frames_per_second"] = round(rec["frames_advanced_on_screen"] / span_s, 2) if span_s > 0 else None
            # A capture takes tens to hundreds of milliseconds. Its picture is consistent with the
            # benchmark's log when the frame it shows was the newest presented at some point between
            # the capture's start and its end.
            rec["captures_in_timed_loop"] = len(timed)
            rec["consistent_with_present_log"] = sum(
                1 for r in timed if r["last_presented_before_capture"] - 1 <= r["decoded_frame"] <= r["last_presented_by_capture_end"])
            rec["frames_past_capture_start"] = sorted(
                r["decoded_frame"] - r["last_presented_before_capture"] for r in timed)
        per_launch[n] = rec
    return per_launch


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", required=True, help="build directory (inside the bottle's drive_c)")
    ap.add_argument("--build-b", help="suite ab: the candidate build")
    ap.add_argument("--out", required=True)
    ap.add_argument("--suite", required=True, choices=["offscreen", "windowed", "ab"])
    ap.add_argument("--bottle", default="scarlettx-m4")
    ap.add_argument("--sandbox", default=str(Path.home() / "m4-feasibility/retail-preflight/offline.sb"))
    ap.add_argument("--repetitions", type=int, default=6, help="launches per cell")
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--warmup", type=int, default=30)
    ap.add_argument("--inflight", type=int, default=3)
    ap.add_argument("--seed", default="0x5CA41E77")
    ap.add_argument("--rounds", type=int, default=1024, help="xorshift rounds of the heavy pixel shader")
    ap.add_argument("--draw-counts", nargs="+", type=int, default=[250, 1000, 4000, 16000])
    ap.add_argument("--resolutions", nargs="+", default=["1280x720", "1920x1080"])
    ap.add_argument("--upload-count", type=int, default=16)
    ap.add_argument("--upload-size", type=int, default=256)
    ap.add_argument("--interval-us", type=int, default=16667)
    ap.add_argument("--modes", nargs="+", default=["draws", "pixel", "upload"], choices=["draws", "pixel", "upload"])
    ap.add_argument("--ab-cells", nargs="+", default=["upload:guestmem", "upload:batched-reuse", "draws:fixed", "pixel:heavy"],
                    help="suite ab: mode:variant cells to run on both builds")
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--busy-wait", type=int, default=600, help="seconds to wait for other builds or tests to finish")
    ap.add_argument("--winlist", default=str(Path.home() / "m4-feasibility/winlist"),
                    help="built tools/sx_winlist.swift (windowed suite)")
    ap.add_argument("--keep-captures", type=int, default=4, help="capture images to keep per launch")
    args = ap.parse_args()

    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    builds = {"A": Path(args.build).resolve()}
    if args.suite == "ab":
        if not args.build_b:
            print("BLOCKED: --suite ab needs --build-b")
            return 2
        builds["B"] = Path(args.build_b).resolve()
    for build in builds.values():
        if not (build / EXE).exists():
            print(f"BLOCKED: {build / EXE} is missing (build with SCARLETTX_BENCHMARKS=ON)")
            return 2

    common = {"frames": args.frames, "warmup": args.warmup, "inflight": args.inflight, "seed": args.seed}

    def cell(mode, variant, **kw):
        c = {"mode": mode, "variant": variant, **common}
        if mode == "pixel":
            c["rounds"] = args.rounds
        if mode == "upload":
            c["upload-count"] = args.upload_count
            c["upload-size"] = args.upload_size
        c.update(kw)
        return c

    def size(res):
        w, h = res.split("x")
        return {"width": int(w), "height": int(h)}

    env = {"started": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "suite": args.suite, "machine": platform.machine(),
           "macos": platform.mac_ver()[0], "cpu": sh(["sysctl", "-n", "machdep.cpu.brand_string"]),
           "crossover": sh(["defaults", "read", "/Applications/CrossOver.app/Contents/Info", "CFBundleShortVersionString"]),
           "bottle": args.bottle, "power": sh(["pmset", "-g", "batt"]), "thermal": sh(["pmset", "-g", "therm"]),
           "low_power_mode": sh(["sh", "-c", "pmset -g | grep -i lowpowermode"]),
           "displays": sh(["system_profiler", "SPDisplaysDataType"], timeout=60),
           "builds": {k: build_record(v) for k, v in builds.items()}, "arguments": vars(args),
           "held_constant": {**common, "rounds": args.rounds, "upload": f"{args.upload_count}x{args.upload_size}"},
           "caches_before": cache_state(), "others_running_at_start": others_running(),
           "note": "the wineserver was left alone; nothing was deleted; all launches inside the offline sandbox"}
    run = Runner(args, out)
    a = builds["A"]

    # The route, from a process held open after its run, and one discarded launch per pipeline so the
    # host's on-disk cache (one per executable name) has seen every shader before anything is compared.
    _, route = run.launch(a, "discard", cell("draws", "fixed", draws=1000, frames=60), hold_ms=5000)
    env["route_mapped_in_process"] = route
    stdout0 = (run.logs / "0001.stdout").read_text(errors="replace")
    env["adapter"] = next((line for line in stdout0.splitlines() if line.startswith("[fps] adapter")), "not reported")
    for build in builds.values():
        run.launch(build, "discard", cell("pixel", "light", frames=60))
        run.launch(build, "discard", cell("pixel", "heavy", frames=60))
        run.launch(build, "discard", cell("upload", "batched-reuse", frames=60))
        run.launch(build, "discard", cell("draws", "pertable", draws=1000, frames=60))

    capture_report = None
    if args.suite == "offscreen":
        if "draws" in args.modes:
            for count in args.draw_counts:
                run.group(a, "offscreen", [cell("draws", "fixed", draws=count), cell("draws", "pertable", draws=count)],
                          args.repetitions)
        if "pixel" in args.modes:
            for res in args.resolutions:
                run.group(a, "offscreen", [cell("pixel", "light", **size(res)), cell("pixel", "heavy", **size(res))],
                          args.repetitions)
        if "upload" in args.modes:
            variants = ["percopy", "batched", "reuse", "batched-reuse", "guestmem"]
            run.group(a, "offscreen", [cell("upload", v) for v in variants], args.repetitions)
        # Serialized: one frame at a time, each waited for. Not a frame rate; labelled as such.
        serial = []
        if "draws" in args.modes:
            serial.append(cell("draws", "fixed", draws=4000, path="serialized"))
        if "pixel" in args.modes:
            serial.append(cell("pixel", "heavy", path="serialized", **size(args.resolutions[-1])))
        if "upload" in args.modes:
            serial.append(cell("upload", "batched-reuse", path="serialized"))
        if serial:
            run.group(a, "serialized", serial, max(2, args.repetitions // 2))
    elif args.suite == "windowed":
        shots = []
        capturer = make_capturer(out, args.winlist, shots)
        paced = {"path": "windowed", "interval-us": args.interval_us}
        free = {"path": "windowed", "interval-us": 0}
        cells = []
        for res in args.resolutions:  # the window's client size stays 1280x720 for every one of these
            cells += [cell("pixel", "light", **size(res), **paced), cell("pixel", "heavy", **size(res), **paced)]
        cells += [cell("draws", "fixed", draws=4000, **paced), cell("upload", "batched-reuse", **paced),
                  cell("upload", "guestmem", **paced)]
        for r in range(args.repetitions):
            order = cells[r % len(cells):] + cells[:r % len(cells)]
            for c in order:
                run.launch(a, "windowed-paced", c, capture=capturer)
        uncapped = [cell("pixel", "light", **size(args.resolutions[-1]), **free),
                    cell("pixel", "heavy", **size(args.resolutions[-1]), **free)]
        for r in range(max(2, args.repetitions // 2)):
            for c in (uncapped if r % 2 == 0 else uncapped[::-1]):
                run.launch(a, "windowed-uncapped", c, capture=capturer)
        capture_report = analyse_captures(out, shots, args.keep_captures)
    else:
        cells = []
        for spec in args.ab_cells:
            mode, variant = spec.split(":")
            kw = {"draws": 4000} if mode == "draws" else (size(args.resolutions[-1]) if mode == "pixel" else {})
            cells.append(cell(mode, variant, **kw))
        flip = False
        for _ in range(args.repetitions):
            for c in cells:
                for side in (("B", "A") if flip else ("A", "B")):
                    run.launch(builds[side], f"{side}-ab", c, side)
                flip = not flip

    summary = run.finish(env)
    if capture_report is not None:
        summary["captures"] = capture_report
        (out / "summary.json").write_text(json.dumps(summary, indent=1))
    if args.suite == "ab":
        # Pair the launches up again: the k-th A and the k-th B of a cell were run back to back.
        per = {}
        ok = {r["launch"] for r in run.launches if r["exit"] == 0}
        for r in csv.DictReader(open(out / "metrics.csv")):
            if int(r["launch"]) not in ok or "discard" in r["series"]:
                continue
            side = r["series"].split("-", 1)[0]
            per.setdefault((r["mode"], r["variant"], r["param"], r["metric"]), {"A": [], "B": []})[side].append(float(r["value"]))
        ab = []
        for (mode, variant, param, metric), v in sorted(per.items()):
            k = min(len(v["A"]), len(v["B"]))
            if k == 0 or metric not in KEY_METRICS:
                continue
            diffs = [v["B"][i] - v["A"][i] for i in range(k)]
            ma = statistics.median(v["A"])
            ab.append({"mode": mode, "variant": variant, "param": param, "metric": metric, "pairs": k,
                       "median_A": ma, "median_B": statistics.median(v["B"]),
                       "min_A": min(v["A"]), "max_A": max(v["A"]), "min_B": min(v["B"]), "max_B": max(v["B"]),
                       "median_paired_difference": statistics.median(diffs),
                       "percent": (statistics.median(diffs) / ma * 100.0) if ma else None,
                       "B_higher_in_pairs": sum(d > 0 for d in diffs), "B_lower_in_pairs": sum(d < 0 for d in diffs)})
        (out / "ab.json").write_text(json.dumps(ab, indent=1))
    failed = summary["failed_launches"]
    print(f"{len(run.launches)} launches, {len(failed)} failed -> {out / 'summary.md'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
