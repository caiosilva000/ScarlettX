#!/usr/bin/env python3
"""ScarlettX boot-and-render regression runner.

Runs a Windows x64 ScarlettX build tree and reports every check as PASS, FAIL, SKIP or BLOCKED.
On macOS the build runs in a CrossOver bottle; on Windows it runs directly.

  regress    every entry of a CTest inventory (``ctest --show-only=json-v1``), then the offscreen
             clear and indexed-draw probes with their exact-pixel requirements, then windowed
             presentation checked against captures of the test window.
  boot-diag  one bounded, traced boot of a package the user supplies. The package is never
             written to: the boot uses a copy-on-write clone where the file system allows it,
             and the original is compared before and after. Diagnostics go to --out only.

Examples (macOS, build unpacked inside the bottle's drive_c):

  tools/sx_regress.py regress --build "$BOTTLE/drive_c/sxreg/build" \\
      --inventory ctest-inventory.json --out out/regress
  tools/sx_regress.py boot-diag --build "$BOTTLE/drive_c/sxreg/build" \\
      --package "$BOTTLE/drive_c/sxreg/build/packages/hello_scarlett" --out out/boot-hello

Python 3.9+, standard library only. Pillow is needed for the on-screen presentation check and for
PNG copies of dumped frames; without it those parts report BLOCKED or are left out.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

PASS, FAIL, SKIP, BLOCKED = "PASS", "FAIL", "SKIP", "BLOCKED"
IS_WINDOWS = os.name == "nt"
CROSSOVER_WINE = "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine"
WINDOW_TITLE = "ScarlettX - Xbox"

# Tests that create a D3D12 device; everything else in an inventory counts as CPU/loader.
GPU_TESTS = {
    "test_d3d12_warp", "test_rt_pipeline", "test_shader_pipeline", "test_d3d12_xs_abi", "test_d3d12_guest_texture", "test_d3d12_guest_texture_order", "test_d3d12_guest_texture_inflight",
    "test_d3d12_list_layout", "test_d3d12_frame_pacing", "test_d3d12_present_window", "test_d3d12_present_frames",
    "test_d3d12_offscreen_probe", "test_d3d12_offscreen_clear", "test_d3d12_offscreen_indexed",
    "test_victory_compute_e2e", "test_victory_compute_guest", "test_d3d12_blend_mrt",
    "test_victory_compute_conformance", "test_victory_draw_e2e", "test_victory_draw_guest", "test_victory_draw_reuse", "test_victory_compute_remap",
    "test_victory_wave64_exchange", "test_victory_quad_edge", "test_victory_draw_table",
}
# Translated compute (tests/test_victory_compute_e2e.cpp): five dispatches of four dwords each.
VICTORY_COMPUTE_TEST = "test_victory_compute_e2e"
VICTORY_COMPUTE_DISPATCHES = 5
VICTORY_COMPUTE_DWORDS = 20
# The same program through the shim's guest-facing route (tests/test_victory_compute_guest.cpp):
# five translated dispatches, one dropped control dispatch, one desktop control dispatch.
VICTORY_GUEST_TEST = "test_victory_compute_guest"
VICTORY_GUEST_FENCES = 7
PRESENT_TEST = "test_d3d12_present_frames"
# Frame colours of tests/test_d3d12_present_frames.cpp, left and right half.
PRESENT_COLOURS = {"red": (230, 30, 30), "green": (30, 200, 30), "blue": (30, 60, 230), "yellow": (240, 220, 30)}
PRESENT_FRAMES = {"A": ("red", "green"), "B": ("blue", "yellow")}

ADAPTER_RE = re.compile(r"\[ScarlettX\]\[d3d12\] Adapter: (.*?) \((hardware|WARP/software)\)")
CRASH_RE = re.compile(r"Unhandled page fault|Unhandled exception|Assertion failed|unimplemented function|"
                      r"failed assertion|check failed|FAILED:")


# --------------------------------------------------------------------------------------------
# Host: how to run a Windows executable and how host paths look from inside it
# --------------------------------------------------------------------------------------------
class Host:
    def __init__(self, bottle: str, wine: str):
        self.bottle = bottle
        self.wine = wine
        self.bottle_dir = Path.home() / "Library/Application Support/CrossOver/Bottles" / bottle
        self.blocked = None
        if IS_WINDOWS:
            self.kind = "windows"
        else:
            self.kind = "crossover"
            if not os.path.exists(wine):
                self.blocked = f"CrossOver's wine not found at {wine}"
            elif not self.bottle_dir.is_dir():
                self.blocked = f"CrossOver bottle '{bottle}' not found"

    def win_path(self, host_path) -> str:
        """Path of a host file as the Windows side sees it."""
        p = Path(host_path).resolve()
        if IS_WINDOWS:
            return str(p)
        drive_c = (self.bottle_dir / "drive_c").resolve()
        try:
            return "C:\\" + str(p.relative_to(drive_c)).replace("/", "\\")
        except ValueError:
            return "Z:" + str(p).replace("/", "\\")  # CrossOver bottles map Z: to /

    def command(self, exe_host_path, args):
        if IS_WINDOWS:
            return [str(exe_host_path)] + list(args)
        return [self.wine, "--bottle", self.bottle, "--wait-children", self.win_path(exe_host_path)] + list(args)

    def describe(self) -> dict:
        info = {"kind": self.kind, "os": platform.platform(), "machine": platform.machine()}
        if self.kind == "crossover":
            info["bottle"] = self.bottle
            try:
                out = subprocess.run([self.wine, "--bottle", self.bottle, "--version"], capture_output=True,
                                     text=True, timeout=60)
                info["wine_version"] = (out.stdout or out.stderr).strip().splitlines()[-1]
            except Exception as exc:  # noqa: BLE001 - recorded, not fatal
                info["wine_version"] = f"unknown ({exc})"
            plist = "/Applications/CrossOver.app/Contents/Info.plist"
            try:
                info["crossover_version"] = subprocess.run(
                    ["defaults", "read", plist, "CFBundleShortVersionString"], capture_output=True, text=True,
                    timeout=20).stdout.strip()
            except Exception:  # noqa: BLE001
                pass
            conf = self.bottle_dir / "cxbottle.conf"
            setting = None
            if conf.exists():
                m = re.search(r'^"CX_GRAPHICS_BACKEND"\s*=\s*"([^"]*)"', conf.read_text(errors="replace"), re.M)
                setting = m.group(1) if m else None
            info["bottle_graphics_backend_setting"] = setting or "not set (CrossOver chooses)"
            reg = self.bottle_dir / "user.reg"
            if reg.exists():
                text = reg.read_text(errors="replace")
                m = re.search(r"\[Software\\\\Wine\\\\DllOverrides\][^\[]*", text)
                info["dll_overrides"] = dict(re.findall(r'"([^"]+)"="([^"]*)"', m.group(0))) if m else {}
        return info

    def cleanup_after_timeout(self, exe_name: str):
        if IS_WINDOWS:
            subprocess.run(["taskkill", "/F", "/IM", exe_name], capture_output=True, check=False)
        else:
            for pattern in ("winedbg", exe_name):
                subprocess.run(["pkill", "-f", pattern], capture_output=True, check=False)


def run_logged(host: Host, exe: Path, args, env_extra: dict, cwd: Path, out_dir: Path, name: str, timeout: int,
               while_running=None, stop_when=None) -> dict:
    """Runs one executable with a timeout; stdout and stderr go to <name>.stdout / <name>.stderr."""
    out_dir.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.setdefault("WINEDEBUG", "fixme-all")
    env.update(env_extra)
    cmd = host.command(exe, args)
    so_path, se_path = out_dir / f"{name}.stdout", out_dir / f"{name}.stderr"
    start = time.time()
    with open(so_path, "wb") as so, open(se_path, "wb") as se:
        proc = subprocess.Popen(cmd, cwd=str(cwd), env=env, stdout=so, stderr=se)
        timed_out = False
        stopped = False
        while proc.poll() is None:
            if stop_when and stop_when():
                stopped = True
                proc.kill()
                proc.wait()
                host.cleanup_after_timeout(exe.name)
                break
            if time.time() - start > timeout:
                timed_out = True
                proc.kill()
                proc.wait()
                host.cleanup_after_timeout(exe.name)
                break
            if while_running:
                while_running()
            else:
                time.sleep(0.05)
    stdout = so_path.read_text(errors="replace")
    stderr = se_path.read_text(errors="replace")
    return {
        "command": [exe.name] + list(args),
        "environment": env_extra,
        "exit_code": None if (timed_out or stopped) else proc.returncode,
        "timed_out": timed_out,
        "stopped_by_runner": stopped,
        "seconds": round(time.time() - start, 1),
        "stdout_log": str(so_path),
        "stderr_log": str(se_path),
        "stdout": stdout,
        "stderr": stderr,
    }


def adapters_in(text: str):
    return [{"name": n, "kind": "software" if k.startswith("WARP") else "hardware"} for n, k in ADAPTER_RE.findall(text)]


def first_match(regex, *texts):
    for text in texts:
        for line in text.splitlines():
            if regex.search(line) and "fixme:" not in line:
                return line.strip()[:400]
    return None


def strip_logs(result: dict) -> dict:
    return {k: v for k, v in result.items() if k not in ("stdout", "stderr")}


# --------------------------------------------------------------------------------------------
# Window capture (macOS): the presentation check needs to see the window, not just Present's HRESULT
# --------------------------------------------------------------------------------------------
class WindowCapture:
    def __init__(self, work_dir: Path):
        self.blocked = None
        self.helper = None
        self.shots = []      # (t0_ms, t1_ms, path)
        self.windows = []
        if IS_WINDOWS or sys.platform != "darwin":
            self.blocked = "window capture is implemented for macOS only"
            return
        src = Path(__file__).with_name("sx_winlist.swift")
        helper = work_dir / "sx_winlist"
        if not helper.exists():
            if not shutil.which("swiftc"):
                self.blocked = "swiftc not found (needed to build the window-list helper)"
                return
            built = subprocess.run(["swiftc", "-O", str(src), "-o", str(helper)], capture_output=True, text=True)
            if built.returncode != 0:
                self.blocked = "could not build the window-list helper: " + built.stderr.strip()[:200]
                return
        self.helper = helper
        probe = work_dir / "capture-probe.png"
        subprocess.run(["screencapture", "-x", "-t", "png", str(probe)], capture_output=True, check=False)
        if not probe.exists():
            self.blocked = "screencapture produced no image (grant Screen Recording to the app running this script)"
        else:
            probe.unlink()

    def grab(self, into: Path):
        """One capture of the ScarlettX output window, by window id. Returns quickly when it is not there."""
        into.mkdir(parents=True, exist_ok=True)
        t0 = int(time.time() * 1000)
        listed = subprocess.run([str(self.helper), WINDOW_TITLE], capture_output=True, text=True).stdout.strip()
        if not listed:
            time.sleep(0.1)
            return
        wid, owner, _title, bounds, onscreen = listed.splitlines()[0].split("\t")
        path = into / f"{t0}.png"
        subprocess.run(["screencapture", "-x", "-o", "-l", wid, "-t", "png", str(path)], capture_output=True,
                       check=False)
        if path.exists():
            self.shots.append((t0, int(time.time() * 1000), path))
            self.windows.append({"t_ms": t0, "window_id": int(wid), "owner": owner, "bounds": bounds,
                                 "onscreen": onscreen})
        time.sleep(0.25)


    def grab_guest(self, into: Path, owners, min_interval: float = 1.0):
        """One capture of the largest window owned by the emulator or the guest, whatever its title.

        A title that does not present through the d3d12_xs shim (a PC-target package using the desktop
        d3d12.dll) logs no frames, so its own window is the only sign that it is rendering."""
        now = time.time()
        if now - getattr(self, "_last_guest", 0.0) < min_interval:
            time.sleep(0.05)
            return
        self._last_guest = now
        best = None
        for line in subprocess.run([str(self.helper)], capture_output=True, text=True).stdout.splitlines():
            parts = line.split("\t")
            if len(parts) != 5 or parts[1].lower() not in owners:
                continue
            try:
                w, h = (float(v) for v in parts[3].split(",")[2:])
            except ValueError:
                continue
            if w < 200 or h < 100:      # menu-bar strips and other helper windows
                continue
            if best is None or w * h > best[0]:
                best = (w * h, parts, w, h)
        if best is None:
            return
        _area, (wid, owner, title, bounds, onscreen), w, _h = best
        into.mkdir(parents=True, exist_ok=True)
        t0 = int(now * 1000)
        path = into / f"{t0}.png"
        subprocess.run(["screencapture", "-x", "-o", "-l", wid, "-t", "png", str(path)], capture_output=True,
                       check=False)
        entry = {"t_ms": t0, "window_id": int(wid), "owner": owner, "title": title, "bounds": bounds,
                 "onscreen": onscreen, "captured": path.exists(), "content_fraction": None, "digest": None}
        if path.exists():
            self.shots.append((t0, int(time.time() * 1000), path))
            entry["content_fraction"], entry["digest"] = window_content(path, w)
        self.windows.append(entry)


CONTENT_MIN_FRACTION = 0.01


def window_content(path: Path, window_points_wide: float):
    """(share of client-area pixels that differ from the most common colour, digest of the client area).

    A blank or single-colour window gives 0. The title bar is cut off first. None when Pillow is missing."""
    try:
        from PIL import Image
    except ImportError:
        return None, None
    try:
        img = Image.open(path).convert("RGB")
    except OSError:
        return None, None
    scale = img.width / window_points_wide if window_points_wide else 1.0
    top = min(int(34 * scale), img.height // 4)
    client = img.crop((0, top, img.width, img.height))
    small = client.resize((256, max(1, round(256 * client.height / client.width))), Image.NEAREST)
    buckets = {}
    for r, g, b in small.getdata():
        key = (r >> 4, g >> 4, b >> 4)
        buckets[key] = buckets.get(key, 0) + 1
    total = small.width * small.height
    return round(1 - max(buckets.values()) / total, 4), hashlib.sha256(small.tobytes()).hexdigest()[:16]


def colour_regions(path: Path, colours: dict, tolerance: int = 72):
    """Pixel count and bounding box of each named colour. The tolerance allows for the display's
    colour space: a capture is in Display P3, where sRGB (30,200,30) reads about (95,197,66)."""
    from PIL import Image
    im = Image.open(path).convert("RGB")
    step = 2
    small = im.resize((im.width // step, im.height // step), Image.NEAREST)
    px = small.load()
    found = {}
    for name, (r, g, b) in colours.items():
        x0, y0, x1, y1, n = small.width, small.height, -1, -1, 0
        for y in range(small.height):
            for x in range(small.width):
                p = px[x, y]
                if abs(p[0] - r) < tolerance and abs(p[1] - g) < tolerance and abs(p[2] - b) < tolerance:
                    n += 1
                    x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
        if n:
            found[name] = {"pixels": n * step * step, "box": [x0 * step, y0 * step, (x1 + 1) * step, (y1 + 1) * step]}
    return found, list(im.size)


# --------------------------------------------------------------------------------------------
# regress
# --------------------------------------------------------------------------------------------
def load_inventory(path: Path):
    tests = json.loads(path.read_text(encoding="utf-8-sig"))["tests"]
    build_root = os.path.dirname(tests[0]["command"][0].replace("\\", "/"))
    entries = []
    for t in tests:
        env = {}
        for prop in t.get("properties", []):
            if prop["name"] == "ENVIRONMENT":
                for kv in prop["value"]:
                    k, v = kv.split("=", 1)
                    env[k] = v
        entries.append({"name": t["name"], "exe": os.path.basename(t["command"][0].replace("\\", "/")),
                        "args": t["command"][1:], "env": env})
    return entries, build_root


def check_offscreen(stdout: str, mode: str) -> dict:
    """The exact-pixel requirements of one offscreen probe mode, read from its output."""
    fence = re.search(rf"\[offscreen:{mode}\] fence signaled=(\d+) completed=(\d+) wait_result=(\d+) index_count=(\d+)",
                      stdout)
    pixels = re.search(rf"\[offscreen:{mode}\] pixels exact (\d+)/(\d+)", stdout)
    backend = re.search(r'\[offscreen\] backend=(\S+) adapter="([^"]*)" vendor=(\S+) device=(\S+) warp=(\S+)', stdout)
    failed = re.search(rf"\[offscreen:{mode}\] FAILED api=(.*?) hr=(0x[0-9a-f]+)", stdout)
    info = {
        "fence_signaled": int(fence.group(1)) if fence else None,
        "fence_completed": int(fence.group(2)) if fence else None,
        "wait_result": int(fence.group(3)) if fence else None,
        "index_count": int(fence.group(4)) if fence else None,
        "pixels_exact": int(pixels.group(1)) if pixels else None,
        "pixels_total": int(pixels.group(2)) if pixels else None,
        "backend": backend.group(1) if backend else None,
        "adapter": backend.group(2) if backend else None,
        "vendor": backend.group(3) if backend else None,
        "device": backend.group(4) if backend else None,
        "warp": backend.group(5) if backend else None,
    }
    if failed:
        info["first_failing_api"] = {"api": failed.group(1), "hresult": failed.group(2)}
    info["requirements_met"] = bool(
        fence and pixels and info["fence_completed"] >= info["fence_signaled"] >= 1 and info["wait_result"] == 0
        and info["pixels_exact"] == info["pixels_total"] == 4096)
    return info


def check_victory_compute(stdout: str) -> dict:
    """The requirements of the translated-compute probe, read from its output: every dispatch's
    fence completed without a timeout and every read-back dword exact."""
    fences = [(int(s), int(c), int(w)) for s, c, w in
              re.findall(r"\[victory-compute\] fence signaled=(\d+) completed=(\d+) wait_result=(\d+)", stdout)]
    dwords = re.search(r"\[victory-compute\] dwords exact (\d+)/(\d+)", stdout)
    translated = re.search(r"\[victory-compute\] translated: dxil=(\d+) bytes", stdout)
    backend = re.search(r'\[victory-compute\] backend=(\S+) adapter="([^"]*)" vendor=(\S+) device=(\S+) warp=(\S+)',
                        stdout)
    failed = re.search(r"\[victory-compute\] FAILED api=(.*?) hr=(0x[0-9a-f]+)", stdout)
    info = {
        "shader_path": "synthetic GFX10 compute -> victory-shader bridge -> DXIL -> D3D12Backend::RunComputeProbe",
        "dxil_bytes": int(translated.group(1)) if translated else None,
        "fences_completed": sum(1 for s, c, w in fences if c >= s >= 1 and w == 0),
        "fences_expected": VICTORY_COMPUTE_DISPATCHES,
        "dwords_exact": int(dwords.group(1)) if dwords else None,
        "dwords_total": int(dwords.group(2)) if dwords else None,
        "backend": backend.group(1) if backend else None,
        "adapter": backend.group(2) if backend else None,
        "vendor": backend.group(3) if backend else None,
        "device": backend.group(4) if backend else None,
        "warp": backend.group(5) if backend else None,
    }
    if failed:
        info["first_failing_api"] = {"api": failed.group(1), "hresult": failed.group(2)}
    info["requirements_met"] = bool(
        translated and dwords and len(fences) == VICTORY_COMPUTE_DISPATCHES
        and info["fences_completed"] == VICTORY_COMPUTE_DISPATCHES
        and info["dwords_exact"] == info["dwords_total"] == VICTORY_COMPUTE_DWORDS)
    return info


def check_victory_guest(stdout: str) -> dict:
    """The requirements of the guest-route compute test, read from its output."""
    fences = [(int(s), int(c), int(w)) for s, c, w in
              re.findall(r"\[victory-guest\] fence signaled=(\d+) completed=(\d+) wait_result=(\d+)", stdout)]
    dwords = re.search(r"\[victory-guest\] dwords exact (\d+)/(\d+), translated dispatches=\+(\d+)", stdout)
    created = re.search(r"\[victory-guest\] shim CreateComputePipelineState\(native container\) hr=(0x[0-9a-f]+) "
                        r"translated=\+(\d+)", stdout)
    failed = re.search(r"\[victory-guest\] FAILED api=(.*?) hr=(0x[0-9a-f]+)", stdout)
    info = {
        "shader_path": ("synthetic console-shaped container -> d3d12_xs CreateComputePipelineState (device object) "
                        "-> victory-shader bridge -> DXIL -> shim command list dispatch"),
        "native_pipeline_hresult": created.group(1) if created else None,
        "pipelines_translated": int(created.group(2)) if created else None,
        "fences_completed": sum(1 for s_, c, w in fences if c >= s_ >= 1 and w == 0),
        "fences_expected": VICTORY_GUEST_FENCES,
        "dwords_exact": int(dwords.group(1)) if dwords else None,
        "dwords_total": int(dwords.group(2)) if dwords else None,
        "translated_dispatches": int(dwords.group(3)) if dwords else None,
        "checks_ok": len(re.findall(r"\[victory-guest\] ok: ", stdout)),
        "checks_failed": len(re.findall(r"\[victory-guest\] CHECK FAILED: ", stdout)),
        "final_line": (re.search(r"\[victory-guest\] (PASS|FAIL) \(\d+ failed checks\)", stdout) or [None])[0],
    }
    if failed:
        info["first_failing_api"] = {"api": failed.group(1), "hresult": failed.group(2)}
    info["requirements_met"] = bool(
        created and dwords and info["native_pipeline_hresult"] == "0x00000000" and info["pipelines_translated"] == 1
        and len(fences) == VICTORY_GUEST_FENCES and info["fences_completed"] == VICTORY_GUEST_FENCES
        and info["dwords_exact"] == info["dwords_total"] == VICTORY_COMPUTE_DWORDS
        and info["translated_dispatches"] == VICTORY_COMPUTE_DISPATCHES and info["checks_failed"] == 0
        and info["final_line"] == "[victory-guest] PASS (0 failed checks)")
    return info


def check_victory_conformance(stdout: str) -> dict:
    """Per-case verdicts of the compute conformance test and the adapter's wave capability."""
    caps = re.search(r"\[victory-conf\] adapter wave capability: query=(0x[0-9a-f]+) wave_ops=(\S+) lanes_min=(\d+) "
                     r"lanes_max=(\d+)", stdout)
    cases = {}
    for name, dispatches, lanes, exact, total, verdict in re.findall(
            r"\[victory-conf\] case=(\S+) dispatches=(\d+) groups=\S+ threads_per_group=(\d+) dwords exact (\d+)/(\d+) "
            r"verdict=(\S+)", stdout):
        cases[name] = {"verdict": verdict, "dispatches": int(dispatches), "threads_per_group": int(lanes),
                       "dwords_exact": int(exact), "dwords_total": int(total)}
    for name, verdict, why in re.findall(r"\[victory-conf\] case=(\S+) verdict=(SKIP|FAIL) \((.*?)\)\n", stdout):
        cases[name] = {"verdict": verdict, "reason": why}
    for name, text in re.findall(r"\[victory-conf\] case=(\S+) wave: (.*)", stdout):
        cases.setdefault(name, {})["wave"] = text
    fences = [(int(s_), int(c), int(w)) for s_, c, w in
              re.findall(r"\[victory-conf\] fence signaled=(\d+) completed=(\d+) wait_result=(\d+)", stdout)]
    info = {
        "shader_path": ("synthetic console-shaped containers -> d3d12_xs CreateComputePipelineState (device object) "
                        "-> victory-shader bridge -> DXIL -> shim command list dispatch"),
        "wave_ops": caps.group(2) if caps else None,
        "wave_lanes_min": int(caps.group(3)) if caps else None,
        "wave_lanes_max": int(caps.group(4)) if caps else None,
        "cases": cases,
        "skipped": sorted(n for n, c in cases.items() if c.get("verdict") == "SKIP"),
        "fences": len(fences),
        "fences_completed": sum(1 for s_, c, w in fences if c >= s_ >= 1 and w == 0),
        "final_line": (re.search(r"\[victory-conf\] (PASS|FAIL) \(\d+ failed\)", stdout) or [None])[0],
    }
    required = ("groups", "lanes32")
    info["requirements_met"] = bool(
        caps and all(cases.get(n, {}).get("verdict") == "PASS" for n in required)
        and all(c.get("verdict") in ("PASS", "SKIP") for c in cases.values()) and len(cases) == 4
        and all(c.get("dwords_exact") == c.get("dwords_total") for c in cases.values() if c.get("verdict") == "PASS")
        and info["fences"] >= 1 and info["fences_completed"] == info["fences"]
        and info["final_line"] == "[victory-conf] PASS (0 failed)")
    return info


def check_victory_wave(stdout: str) -> dict:
    """The lane exchange test: each case's class, and what the adapter and the shaders say about waves."""
    caps = re.search(r"\[victory-wave\] adapter wave capability: query=(0x[0-9a-f]+) wave_ops=(\S+) lanes_min=(\d+) "
                     r"lanes_max=(\d+)", stdout)
    cases = {}
    for name, lanes, lo, hi, reports, cls in re.findall(
            r"\[victory-wave\] case=(\S+) lanes=(\d+) adapter_range=(\d+)\.\.(\d+) host_reports_wave=(\S+) "
            r"class=(\S+)", stdout):
        cases[name] = {"class": cls, "lanes": int(lanes), "host_reports_wave": reports == "yes"}
    for name, exact, total, b_ok, b_total in re.findall(
            r"\[victory-wave\] case=(\S+) dispatches=\d+ threads_per_group=\d+ dwords exact (\d+)/(\d+) "
            r"boundary (\d+)/(\d+)", stdout):
        cases.setdefault(name, {}).update(dwords_exact=int(exact), dwords_total=int(total),
                                          boundary_exact=int(b_ok), boundary_total=int(b_total))
    for name, text in re.findall(r"\[victory-wave\] case=(\S+) translated: (.*)", stdout):
        cases.setdefault(name, {})["translated"] = text
    for name, verdict in re.findall(
            r"\[victory-wave\] case=(\S+) wave size out of the adapter's range: (NOT refused|refused) by the shim",
            stdout):
        cases.setdefault(name, {})["out_of_range_refused_by_shim"] = verdict == "refused"
    fences = [(int(s_), int(c), int(w)) for s_, c, w in
              re.findall(r"\[victory-wave\] fence signaled=(\d+) completed=(\d+) wait_result=(\d+)", stdout)]
    info = {
        "shader_path": ("synthetic console-shaped containers -> d3d12_xs CreateComputePipelineState (device object) "
                        "-> victory-shader bridge -> DXIL -> shim command list dispatch"),
        "wave_ops": caps.group(2) if caps else None,
        "wave_lanes_min": int(caps.group(3)) if caps else None,
        "wave_lanes_max": int(caps.group(4)) if caps else None,
        "cases": cases,
        "fences": len(fences),
        "fences_completed": sum(1 for s_, c, w in fences if c >= s_ >= 1 and w == 0),
        "final_line": (re.search(r"\[victory-wave\] (PASS|FAIL) \(\d+ failed\)", stdout) or [None])[0],
    }
    # Exact or cleanly refused; exact as a real wave wherever the adapter reports that wave size.
    def acceptable(c):
        if c.get("class") == "native_exact":
            return c.get("host_reports_wave") and c.get("dwords_exact") == c.get("dwords_total")
        if c.get("class") == "emulated_exact":
            return not c.get("host_reports_wave") and c.get("dwords_exact") == c.get("dwords_total")
        return (c.get("class") == "unsupported" and not c.get("host_reports_wave")
                and c.get("out_of_range_refused_by_shim", True))
    info["requirements_met"] = bool(
        caps and set(cases) == {"exchange32", "exchange64"} and all(acceptable(c) for c in cases.values())
        and info["fences_completed"] == info["fences"]
        and info["final_line"] == "[victory-wave] PASS (0 failed)")
    return info


def check_victory_draw_table(stdout: str) -> dict:
    """The descriptor-table draw test: frames exact, dropped draws, objects released in flight."""
    total = re.search(r"\[victory-draw-table\] pixels exact (\d+)/(\d+) over (\d+) frames", stdout)
    fences = [(int(s_), int(c), int(w)) for s_, c, w in
              re.findall(r"\[victory-draw-table\] fence signaled=(\d+) completed=(\d+) wait_result=(\d+)", stdout)]
    in_flight = re.findall(r"\[victory-draw-table\] (10\w) .*?: Release returned \d+, list still in flight afterwards: (\w+)",
                           stdout)
    info = {
        "shader_path": ("synthetic console-shaped vertex and pixel containers -> d3d12_xs CreateGraphicsPipelineState "
                        "(device object) -> victory-shader bridge -> DXIL -> shim command list draws on the shim's queue"),
        "pixels_exact": int(total.group(1)) if total else 0,
        "pixels_total": int(total.group(2)) if total else 0,
        "frames": int(total.group(3)) if total else 0,
        "checks_ok": len(re.findall(r"\[victory-draw-table\] ok: ", stdout)),
        "checks_failed": len(re.findall(r"\[victory-draw-table\] CHECK FAILED", stdout)),
        "released_in_flight": {name: answer == "yes" for name, answer in in_flight},
        "fences": len(fences),
        "fences_completed": sum(1 for s_, c, w in fences if c >= s_ >= 1 and w == 0),
        "final_line": (re.search(r"\[victory-draw-table\] (PASS|FAIL) \(\d+ failed\)", stdout) or [None])[0],
    }
    info["requirements_met"] = bool(
        total and info["frames"] == 11 and info["pixels_exact"] == info["pixels_total"] == 11 * 4096
        and info["checks_failed"] == 0 and len(in_flight) == 4 and all(info["released_in_flight"].values())
        and info["fences"] >= 11 and info["fences_completed"] == info["fences"]
        and info["final_line"] == "[victory-draw-table] PASS (0 failed)")
    return info


def check_victory_quad(stdout: str) -> dict:
    """The quad-read test at triangle edges: coverage, and where translated and desktop differ."""
    cov = re.search(r"\[victory-quad\] coverage: cpu=(\d+) desktop=(\d+) \(differs at (\d+)\) translated=(\d+) "
                    r"\(differs at (\d+)\) of (\d+) pixels", stdout)
    px = re.search(r"\[victory-quad\] pixels: covered=(\d+) edge=(\d+) interior=(\d+); reads of a lane outside the "
                   r"triangle: (\d+)", stdout)
    compares = {}
    for name, pixels, edge, interior, ddx, ddy, diag, first, hw, ht, ow in re.findall(
            r"\[victory-quad\] (\w+): differing_pixels=(\d+) edge=(\d+) interior=(\d+) values ddx=(\d+) ddy=(\d+) "
            r"diagonal=(\d+) first=(\d+) helper_reads_wrong=(\d+)/(\d+) other_reads_wrong=(\d+)", stdout):
        compares[name] = {"differing_pixels": int(pixels), "edge": int(edge), "interior": int(interior),
                          "values": {"ddx": int(ddx), "ddy": int(ddy), "diagonal": int(diag), "first": int(first)},
                          "helper_reads_wrong": int(hw), "helper_reads": int(ht), "other_reads_wrong": int(ow)}
    fences = [(int(s_), int(c), int(w)) for s_, c, w in
              re.findall(r"\[victory-quad\] \w+ fence signaled=(\d+) completed=(\d+) wait_result=(\d+)", stdout)]
    adapter = re.search(r"\[victory-quad\] adapter=(.*?) vendor=", stdout)
    info = {
        "shader_path": ("synthetic console-shaped vertex and pixel containers -> victory-shader bridge -> DXIL -> "
                        "host D3D12 draw; reference: HLSL through D3DCompile"),
        "adapter": adapter.group(1) if adapter else None,
        "covered_cpu": int(cov.group(1)) if cov else None,
        "coverage_differs_desktop": int(cov.group(3)) if cov else None,
        "coverage_differs_translated": int(cov.group(5)) if cov else None,
        "edge_pixels": int(px.group(2)) if px else None,
        "interior_pixels": int(px.group(3)) if px else None,
        "helper_reads": int(px.group(4)) if px else None,
        "compares": compares,
        "fences": len(fences),
        "fences_completed": sum(1 for s_, c, w in fences if c >= s_ >= 1 and w == 0),
        "final_line": (re.search(r"\[victory-quad\] (PASS|FAIL) \(edge pixels differing: \d+ of \d+\)", stdout)
                       or [None])[0],
    }
    td = compares.get("translated_vs_desktop", {})
    info["requirements_met"] = bool(
        cov and px and info["coverage_differs_desktop"] == 0 and info["coverage_differs_translated"] == 0
        and info["edge_pixels"] and info["helper_reads"] and td and td["differing_pixels"] == 0
        and info["fences"] == 2 and info["fences_completed"] == 2
        and (info["final_line"] or "").startswith("[victory-quad] PASS"))
    return info


def check_victory_draw(stdout: str) -> dict:
    """The requirements of the translated draw test: four draws, every pixel exact."""
    fences = [(int(s_), int(c), int(w)) for s_, c, w in
              re.findall(r"\[victory-draw\] fence signaled=(\d+) completed=(\d+) wait_result=(\d+)", stdout)]
    pixels = re.search(r"\[victory-draw\] pixels exact (\d+)/(\d+)", stdout)
    backend = re.search(r'\[victory-draw\] backend=(\S+) adapter="([^"]*)" vendor=(\S+) device=(\S+) warp=(\S+)', stdout)
    failed = re.search(r"\[victory-draw\] FAILED api=(.*?) hr=(0x[0-9a-f]+)", stdout)
    info = {
        "shader_path": ("synthetic GFX10 vertex and pixel programs -> victory-shader bridge -> DXIL -> "
                        "D3D12Backend::RunDrawProbe (backend path, not the shim's guest route)"),
        "fences": len(fences),
        "fences_completed": sum(1 for s_, c, w in fences if c >= s_ >= 1 and w == 0),
        "pixels_exact": int(pixels.group(1)) if pixels else None,
        "pixels_total": int(pixels.group(2)) if pixels else None,
        "adapter": backend.group(2) if backend else None,
        "warp": backend.group(5) if backend else None,
    }
    if failed:
        info["first_failing_api"] = {"api": failed.group(1), "hresult": failed.group(2)}
    info["requirements_met"] = bool(pixels and info["fences"] == 4 and info["fences_completed"] == 4
                                    and info["pixels_exact"] == info["pixels_total"] == 16384)
    return info


def check_victory_draw_guest(stdout: str) -> dict:
    """The requirements of the guest-route draw test: the native pipeline created and counted as
    translated, four translated draws with every pixel exact, six completed fences (four draws,
    the dropped control draw and the desktop control), and no failed check."""
    fences = [(int(s_), int(c), int(w)) for s_, c, w in
              re.findall(r"\[victory-draw-guest\] fence signaled=(\d+) completed=(\d+) wait_result=(\d+)", stdout)]
    pixels = re.search(r"\[victory-draw-guest\] pixels exact (\d+)/(\d+), translated draws=\+(\d+)", stdout)
    created = re.search(r"\[victory-draw-guest\] shim CreateGraphicsPipelineState\(native containers\) hr=(0x[0-9a-f]+) "
                        r"translated=\+(\d+)", stdout)
    desktop = re.search(r"\[victory-draw-guest\] desktop control: drawn half exact (\d+)/(\d+), untouched half exact "
                        r"(\d+)/(\d+)", stdout)
    failed = re.search(r"\[victory-draw-guest\] FAILED api=(.*?) hr=(0x[0-9a-f]+)", stdout)
    info = {
        "shader_path": ("synthetic console-shaped vertex and pixel containers -> d3d12_xs CreateGraphicsPipelineState "
                        "(device object) -> victory-shader bridge -> DXIL -> shim command list draw"),
        "native_pipeline_hresult": created.group(1) if created else None,
        "pipelines_translated": int(created.group(2)) if created else None,
        "fences": len(fences),
        "fences_completed": sum(1 for s_, c, w in fences if c >= s_ >= 1 and w == 0),
        "pixels_exact": int(pixels.group(1)) if pixels else None,
        "pixels_total": int(pixels.group(2)) if pixels else None,
        "translated_draws": int(pixels.group(3)) if pixels else None,
        "desktop_control_exact": (int(desktop.group(1)) + int(desktop.group(3))) if desktop else None,
        "checks_ok": len(re.findall(r"\[victory-draw-guest\] ok: ", stdout)),
        "checks_failed": len(re.findall(r"\[victory-draw-guest\] CHECK FAILED: ", stdout)),
        "final_line": (re.search(r"\[victory-draw-guest\] (PASS|FAIL) \(\d+ failed checks\)", stdout) or [None])[0],
    }
    if failed:
        info["first_failing_api"] = {"api": failed.group(1), "hresult": failed.group(2)}
    info["requirements_met"] = bool(
        created and pixels and info["native_pipeline_hresult"] == "0x00000000" and info["pipelines_translated"] == 1
        and info["fences"] == 6 and info["fences_completed"] == 6
        and info["pixels_exact"] == info["pixels_total"] == 16384 and info["translated_draws"] == 4
        and info["desktop_control_exact"] == 4096 and info["checks_failed"] == 0
        and info["final_line"] == "[victory-draw-guest] PASS (0 failed checks)")
    return info


def read_build_info(build: Path) -> dict:
    """build-info.txt as tools/macos_cross_build.sh writes it: `key=value` lines."""
    info = {}
    try:
        for line in (build / "build-info.txt").read_text().splitlines():
            key, sep, value = line.partition("=")
            if sep:
                info[key.strip()] = value.strip()
    except OSError:
        pass
    return info


def cmd_regress(args) -> int:
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    build = Path(args.build).resolve()
    host = Host(args.bottle, args.wine)
    # The source revision: --commit, else what the build recorded about itself.
    build_info = read_build_info(build)
    summary = {"tool": "sx_regress regress", "started": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "build": str(build),
               "commit": args.commit or build_info.get("commit"),
               "commit_source": "--commit" if args.commit else ("build-info.txt" if build_info.get("commit") else None),
               "build_info": build_info, "host": None, "graphics": {}, "checks": []}
    checks = summary["checks"]

    def add(name, category, status, **details):
        checks.append({"name": name, "category": category, "status": status, **details})
        print(f"{status:8} {category:13} {name}" + (f"  ({details['note']})" if details.get("note") else ""), flush=True)

    def finish() -> int:
        counts = {s: sum(1 for c in checks if c["status"] == s) for s in (PASS, FAIL, SKIP, BLOCKED)}
        summary["counts"] = counts
        summary["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
        (out / "summary.json").write_text(json.dumps(summary, indent=1))
        lines = ["| Check | Category | Status | Exit | Note |", "|---|---|---|---|---|"]
        for c in checks:
            lines.append(f"| `{c['name']}` | {c['category']} | {c['status']} | {c.get('exit_code', '')} | {c.get('note', '')} |")
        (out / "summary.md").write_text("\n".join(lines) + "\n")
        print(f"\n{counts}  ->  {out / 'summary.json'}")
        return 0 if counts[FAIL] == 0 and counts[BLOCKED] == 0 else 1

    if host.blocked:
        add("host", "preflight", BLOCKED, note=host.blocked)
        return finish()
    summary["host"] = host.describe()
    if not build.is_dir():
        add("build", "preflight", BLOCKED, note=f"build folder not found: {build}")
        return finish()

    if args.sha256_list:
        want = {}
        for line in Path(args.sha256_list).read_text(encoding="utf-8-sig").splitlines():
            if "  " in line:
                digest, rel = line.strip().split("  ", 1)
                rel = rel.replace("\\", "/")
                want[rel.split("dist/", 1)[-1].lstrip("./")] = digest
        bad = [rel for rel, digest in want.items()
               if not (build / rel).exists() or hashlib.sha256((build / rel).read_bytes()).hexdigest() != digest]
        add("build-checksums", "preflight", PASS if not bad else FAIL, files=len(want), mismatched=bad[:20],
            note=f"{len(want) - len(bad)} of {len(want)} files match")

    # ScarlettX ships its own xaudio2_9.dll. Wine has a built-in one that wins unless the bottle says
    # otherwise, and it lacks the shim's exports (the audio tests then stop in "unimplemented function").
    if host.kind == "crossover" and (build / "xaudio2_9.dll").exists():
        override = summary["host"].get("dll_overrides", {}).get("xaudio2_9", "")
        if override.replace(" ", "").split(",")[0] == "native":
            add("bottle-xaudio2_9-override", "preflight", PASS, note=f"xaudio2_9 = {override}")
        else:
            add("bottle-xaudio2_9-override", "preflight", BLOCKED,
                note=f"xaudio2_9 override is '{override or 'not set'}'; the audio tests will fail. Fix: "
                     f"\"{host.wine}\" --bottle {host.bottle} reg add 'HKCU\\Software\\Wine\\DllOverrides' "
                     f"/v xaudio2_9 /t REG_SZ /d native /f")

    entries, inventory_root = load_inventory(Path(args.inventory))
    fixture_env = {"SCARLETTX_TEST_BINARY_DIR": host.win_path(build)}
    win_build = host.win_path(build)

    def rewrite(arg: str) -> str:
        norm = arg.replace("\\", "/")
        if norm.startswith(inventory_root):
            return win_build + norm[len(inventory_root):].replace("/", "\\")
        return arg

    # Adapter and backend, from the emulator's own diagnostic command.
    diag_exe = build / "scarlettx.exe"
    if diag_exe.exists():
        r = run_logged(host, diag_exe, ["diag"], fixture_env, build, out / "logs", "scarlettx_diag", args.timeout)
        m = re.search(r"\[diag\] Adapter: (.*) \(WARP=(\w+), Device5=(\w+), DXR Tier=(\d+)\)", r["stdout"])
        summary["graphics"] = {"backend": "D3D12", "route": ("CrossOver, bottle graphics backend setting: " + summary["host"].get("bottle_graphics_backend_setting", "?"))
                               if host.kind == "crossover" else "native",
                               "adapter": m.group(1) if m else None, "warp": m.group(2) if m else None,
                               "device5": m.group(3) if m else None, "dxr_tier": m.group(4) if m else None,
                               "note": "scarlettx diag asks for WARP first and reports the adapter it ended on"}
        add("scarlettx diag", "preflight", PASS if r["exit_code"] == 0 and m else FAIL, exit_code=r["exit_code"],
            **{k: r[k] for k in ("stdout_log", "stderr_log")}, note=(m.group(1) if m else first_match(CRASH_RE, r["stderr"])))

    for entry in entries:
        name, exe = entry["name"], build / entry["exe"]
        if args.only and name not in args.only:
            continue
        category = "gpu" if name in GPU_TESTS else ("boot" if name.startswith("test_boot_") else "cpu-loader")
        if name == PRESENT_TEST:
            category = "presentation"
        if not exe.exists():
            add(name, category, SKIP, note=f"{entry['exe']} is not in this build")
            continue
        env = dict(fixture_env, **entry["env"])
        argv = [rewrite(a) for a in entry["args"]]
        r = run_logged(host, exe, argv, env, build, out / "logs", name, args.timeout)
        status = PASS if r["exit_code"] == 0 else FAIL
        details = strip_logs(r)
        details["adapters"] = adapters_in(r["stderr"])
        note = None
        if status == FAIL:
            note = "timed out" if r["timed_out"] else first_match(CRASH_RE, r["stderr"], r["stdout"])
            # A WARP request the host cannot meet: record the hardware-adapter result next to the failure.
            if category in ("gpu", "presentation") and any(a["name"] == "" for a in details["adapters"]):
                r2 = run_logged(host, exe, argv, dict(env, SCARLETTX_PREFER_WARP="0"), build, out / "logs",
                                name + ".prefer-hardware", args.timeout)
                details["with_SCARLETTX_PREFER_WARP_0"] = dict(strip_logs(r2), status=PASS if r2["exit_code"] == 0 else FAIL,
                                                               adapters=adapters_in(r2["stderr"]),
                                                               first_error=None if r2["exit_code"] == 0 else
                                                               ("timed out" if r2["timed_out"] else
                                                                first_match(CRASH_RE, r2["stderr"], r2["stdout"])))
                details["finding"] = "warp-request-has-no-fallback"
                note = (f"WARP adapter unusable; with SCARLETTX_PREFER_WARP=0: "
                        f"{details['with_SCARLETTX_PREFER_WARP_0']['status']}")
        if name == "test_inproc_loader":
            m = re.search(r"\[test_inproc_loader\] PASSED \((.*?)\)", r["stdout"])
            details["final_line"] = m.group(0) if m else None
            if status == PASS and not m:
                status, note = FAIL, "exit 0 but the PASSED line is missing"
            elif m:
                note = m.group(1)
        if name in ("test_d3d12_offscreen_clear", "test_d3d12_offscreen_indexed"):
            mode = name.rsplit("_", 1)[1]
            probe = check_offscreen(r["stdout"], mode)
            details["offscreen"] = probe
            if status == PASS and not probe["requirements_met"]:
                status, note = FAIL, "exit 0 but fence or exact-pixel requirement not met"
            elif status == PASS:
                note = (f"{probe['pixels_exact']}/{probe['pixels_total']} exact, fence {probe['fence_completed']}/"
                        f"{probe['fence_signaled']}, adapter {probe['adapter']}")
        if name == VICTORY_COMPUTE_TEST:
            probe = check_victory_compute(r["stdout"])
            details["victory_compute"] = probe
            if status == PASS and not probe["requirements_met"]:
                status, note = FAIL, "exit 0 but a fence or the exact-dword requirement was not met"
            elif status == PASS:
                note = (f"{probe['dwords_exact']}/{probe['dwords_total']} dwords exact, fences "
                        f"{probe['fences_completed']}/{probe['fences_expected']}, adapter {probe['adapter']}")
        if name == VICTORY_GUEST_TEST:
            probe = check_victory_guest(r["stdout"])
            details["victory_guest"] = probe
            if status == PASS and not probe["requirements_met"]:
                status, note = FAIL, "exit 0 but a guest-route requirement was not met"
            elif status == PASS:
                note = (f"{probe['dwords_exact']}/{probe['dwords_total']} dwords exact through the shim, fences "
                        f"{probe['fences_completed']}/{probe['fences_expected']}, {probe['checks_ok']} checks")
        if name == "test_victory_compute_conformance":
            probe = check_victory_conformance(r["stdout"])
            details["victory_conformance"] = probe
            if status == PASS and not probe["requirements_met"]:
                status, note = FAIL, "exit 0 but a conformance requirement was not met"
            elif status == PASS:
                verdicts = ", ".join(f"{n} {c.get('verdict')}" for n, c in probe["cases"].items())
                note = (f"{verdicts}; adapter waves {probe['wave_lanes_min']}..{probe['wave_lanes_max']}"
                        + (f"; SKIPPED: {', '.join(probe['skipped'])}" if probe["skipped"] else ""))
        if name == "test_victory_draw_table":
            probe = check_victory_draw_table(r["stdout"])
            details["victory_draw_table"] = probe
            if status == PASS and not probe["requirements_met"]:
                status, note = FAIL, "exit 0 but a descriptor-table draw requirement was not met"
            elif status == PASS:
                note = (f"{probe['pixels_exact']}/{probe['pixels_total']} pixels exact over {probe['frames']} frames, fences "
                        f"{probe['fences_completed']}/{probe['fences']}, {probe['checks_ok']} checks, "
                        f"{sum(probe['released_in_flight'].values())} objects released in flight")
        if name == "test_victory_quad_edge":
            probe = check_victory_quad(r["stdout"])
            details["victory_quad"] = probe
            td = probe["compares"].get("translated_vs_desktop")
            if status == PASS and not probe["requirements_met"]:
                status, note = FAIL, "exit 0 but a quad-read requirement was not met"
            elif td:
                note = (f"translated vs desktop: {td['edge']} of {probe['edge_pixels']} edge pixels and "
                        f"{td['interior']} of {probe['interior_pixels']} interior pixels differ; "
                        f"{td['helper_reads_wrong']} of {td['helper_reads']} reads of a lane outside the triangle wrong")
        if name == "test_victory_wave64_exchange":
            probe = check_victory_wave(r["stdout"])
            details["victory_wave"] = probe
            classes = ", ".join(f"{n} {c.get('class')}" for n, c in probe["cases"].items())
            if status == PASS and not probe["requirements_met"]:
                status, note = FAIL, "exit 0 but a lane exchange requirement was not met"
            elif classes:
                note = f"{classes}; adapter waves {probe['wave_lanes_min']}..{probe['wave_lanes_max']}"
        if name == "test_victory_draw_e2e":
            probe = check_victory_draw(r["stdout"])
            details["victory_draw"] = probe
            if status == PASS and not probe["requirements_met"]:
                status, note = FAIL, "exit 0 but a fence or the exact-pixel requirement was not met"
            elif status == PASS:
                note = (f"{probe['pixels_exact']}/{probe['pixels_total']} pixels exact, fences "
                        f"{probe['fences_completed']}/{probe['fences']}, adapter {probe['adapter']}")
        if name == "test_victory_draw_guest":
            probe = check_victory_draw_guest(r["stdout"])
            details["victory_draw_guest"] = probe
            if status == PASS and not probe["requirements_met"]:
                status, note = FAIL, "exit 0 but a guest-route draw requirement was not met"
            elif status == PASS:
                note = (f"{probe['pixels_exact']}/{probe['pixels_total']} pixels exact through the shim, fences "
                        f"{probe['fences_completed']}/{probe['fences']}, {probe['checks_ok']} checks")
        # The hash of what ran, so a result can be tied to one artifact.
        details["exe_sha256"] = hashlib.sha256(exe.read_bytes()).hexdigest()
        add(name, category, status, note=note, **details)

    # Presentation: the API results above, plus what the window showed.
    present_exe = build / f"{PRESENT_TEST}.exe"
    if not args.skip_present and (not args.only or PRESENT_TEST in args.only):
        if not present_exe.exists():
            add("presentation-on-screen", "presentation", SKIP, note=f"{PRESENT_TEST}.exe is not in this build")
        else:
            cap = WindowCapture(out)
            try:
                import PIL  # noqa: F401
            except ImportError:
                cap.blocked = cap.blocked or "Pillow is not installed (needed to read the captures)"
            if cap.blocked:
                add("presentation-on-screen", "presentation", BLOCKED, note=cap.blocked)
            else:
                pdir = out / "presentation"
                env = dict(fixture_env, SCARLETTX_REAL_TITLE="1", SCARLETTX_PRESENT_HOLD_MS=str(args.hold_ms))
                r = run_logged(host, present_exe, [], env, build, pdir, PRESENT_TEST, args.timeout + 60,
                               while_running=lambda: cap.grab(pdir / "captures"))
                if r["exit_code"] != 0 and any(a["name"] == "" for a in adapters_in(r["stderr"])):
                    env["SCARLETTX_PREFER_WARP"] = "0"
                    cap.shots.clear()
                    r = run_logged(host, present_exe, [], env, build, pdir, PRESENT_TEST, args.timeout + 60,
                                   while_running=lambda: cap.grab(pdir / "captures"))
                report = analyse_presentation(r["stdout"], cap, pdir)
                ok = r["exit_code"] == 0 and report["overall"] == PASS
                add("presentation-on-screen", "presentation", PASS if ok else FAIL, exit_code=r["exit_code"],
                    environment=env, adapters=adapters_in(r["stderr"]), stdout_log=r["stdout_log"],
                    stderr_log=r["stderr_log"], capture_report=report,
                    note=f"{sum(1 for s in report['stages'] if s['verdict'] == PASS)} of 4 stages seen in the window, "
                         f"resize ratio {report.get('resize_ratio')}, {len(cap.shots)} captures")
    return finish()


def analyse_presentation(stdout: str, cap: WindowCapture, pdir: Path) -> dict:
    stages = []
    for m in re.finditer(r"stage=(\S+) frame=(\w) presentx_hr=(0x[0-9a-f]+) t_ms=(\d+) client=(\d+)x(\d+)", stdout):
        end = re.search(rf"stage={re.escape(m.group(1))} end t_ms=(\d+)", stdout)
        stages.append({"stage": m.group(1), "frame": m.group(2), "presentx_hr": m.group(3), "start_ms": int(m.group(4)),
                       "end_ms": int(end.group(1)) if end else None, "client": [int(m.group(5)), int(m.group(6))]})
    report = {"captures": len(cap.shots), "method": "screencapture -l <window id>: the window alone, as the window "
              "server composites it", "window": cap.windows[:1] + cap.windows[-1:], "stages": stages}
    from PIL import Image
    for s in stages:
        inside = [x for x in cap.shots if s["end_ms"] and x[0] >= s["start_ms"] + 300 and x[1] <= s["end_ms"]]
        if not inside:
            s["verdict"] = FAIL
            s["reason"] = "no capture inside this stage"
            continue
        _t0, _t1, path = inside[len(inside) // 2]
        keep = pdir / f"{s['stage']}.png"
        Image.open(path).save(keep)
        found, size = colour_regions(path, PRESENT_COLOURS)
        left, right = PRESENT_FRAMES[s["frame"]]
        s.update(capture=str(keep), capture_size=size, found=found)
        ok = left in found and right in found
        if ok:
            lb, rb = found[left]["box"], found[right]["box"]
            s["area_px"] = [rb[2] - lb[0], max(lb[3], rb[3]) - min(lb[1], rb[1])]
            ok = (lb[0] < rb[0] and abs(lb[2] - rb[0]) <= 8 and abs(lb[1] - rb[1]) <= 8 and abs(lb[3] - rb[3]) <= 8
                  and found[left]["pixels"] > 5000 and found[right]["pixels"] > 5000)
            stale = {c: found[c]["pixels"] for c in PRESENT_COLOURS if c not in (left, right)
                     and c in found and found[c]["pixels"] > 2000}
            if stale:
                s["stale_colours"] = stale
                ok = False
        s["verdict"] = PASS if ok else FAIL
    by = {s["stage"]: s for s in stages}
    if all(k in by and "area_px" in by[k] for k in ("2-frame-B", "3-frame-A-resized")):
        a, b = by["2-frame-B"]["area_px"], by["3-frame-A-resized"]["area_px"]
        report["resize_ratio"] = [round(b[0] / a[0], 3), round(b[1] / a[1], 3)]
    closed = "window closed with the queue" in stdout
    report["clean_close"] = closed
    report["overall"] = PASS if len(stages) == 4 and all(s["verdict"] == PASS for s in stages) and closed and \
        report.get("resize_ratio") == [0.5, 0.5] else FAIL
    # Make a small animation of every capture and drop the raw ones.
    if cap.shots:
        frames = []
        for _t0, _t1, path in cap.shots:
            im = Image.open(path).convert("RGB")
            canvas = Image.new("RGB", (640, 374), (20, 20, 20))
            canvas.paste(im.resize((max(1, im.width // 4), max(1, im.height // 4))), (0, 0))
            frames.append(canvas)
        frames[0].save(pdir / "presentation.gif", save_all=True, append_images=frames[1:], duration=400, loop=0)
        report["animation"] = str(pdir / "presentation.gif")
        shutil.rmtree(pdir / "captures", ignore_errors=True)
    (pdir / "capture-report.json").write_text(json.dumps(report, indent=1))
    return report

# A crash of the guest itself, as the host reports it. Narrower than CRASH_RE on purpose: a
# failed check inside a test or an unimplemented import is diagnosis, this decides the verdict.
GUEST_FAULT_RE = re.compile(r"Unhandled page fault[^\n]*|Unhandled exception[^\n]*")


# --------------------------------------------------------------------------------------------
# boot-diag
# --------------------------------------------------------------------------------------------
BOOT_STAGES = [
    ("package_inspected", None),
    ("package_drive_mapped", re.compile(r"\[ScarlettX\]\[vfs\] Mapped virtual G:")),
    ("image_mapped", re.compile(r"\[ScarlettX\]\[loader\] Mapped ")),
    ("guest_entered", re.compile(r"\[ScarlettX\]\[loader\] Entering native PE entry point|^\[(?!ScarlettX|boot|inspect)[\w ]+\] ", re.M)),
    ("d3d12_device_created", re.compile(r"\[ScarlettX\]\[d3d12\] Adapter: .+ \((hardware|WARP/software)\)")),
    ("swap_chain_created", re.compile(r"\[ScarlettX\]\[d3d12_xs\] SwapChain created")),
    ("first_frame", re.compile(r"\bFrame \d+\b|\[ScarlettX\]\[d3d12_xs\] Dumped frame to")),
    ("guest_window_visible", None),     # from window captures, not from the log
    ("guest_window_content", None),
    ("guest_exited_cleanly", re.compile(r"\[boot\] Guest exited cleanly\.")),
]
GRAPHICS_FAIL_RE = re.compile(
    r"(?i)(d3d12|dxgi|present|swapchain|swap chain|metal|mtl|shader|pipeline|dxil|dxbc).*"
    r"(failed|returned 0x(?!0+\b)[0-9a-f]+|hr=0x8[0-9a-f]{7}|unimplemented|not implemented|unsupported|assertion|invalid)"
    r"|(failed assertion|check failed).*(d3d12|texture|MTL)")
STATIC_STUB_RE = re.compile(r"^\s*\[stub #(\d+)\] (.+?) \(target=(.*?)\)", re.M)
STUB_HIT_RE = re.compile(r"\[ScarlettX\]\[STUB\] (\S+) \(hit #1\)")
WINE_UNIMPL_RE = re.compile(r"unimplemented function (\S+)")
LOADER_ERR_RE = re.compile(r"(?i)\[(boot|inspect)\] Error: .*|could not load|cannot find|not found.*\.dll|error 126|"
                           r"Library .* not found|import .* (unresolved|missing)")


def content_span(windows):
    """Seconds between the first and the latest capture that showed content; None before the first one."""
    shown = [w["t_ms"] for w in windows if (w.get("content_fraction") or 0) >= CONTENT_MIN_FRACTION]
    return round((shown[-1] - shown[0]) / 1000, 1) if shown else None


def summarise_guest_window(windows, boot_seconds) -> dict:
    captured = [w for w in windows if w.get("captured")]
    shown = [w for w in captured if (w.get("content_fraction") or 0) >= CONTENT_MIN_FRACTION]
    last = captured[-1] if captured else (windows[-1] if windows else None)
    return {
        "captures": len(captured),
        "seen_without_capture": len(windows) - len(captured),
        "title": last["title"] if last else None,
        "owner": last["owner"] if last else None,
        "bounds": last["bounds"] if last else None,
        "content_fraction": shown[-1]["content_fraction"] if shown else (last or {}).get("content_fraction"),
        "content_seconds": content_span(windows),
        "picture_changed": len({w["digest"] for w in shown}) > 1 if shown else None,
        "note": "content = share of client-area pixels differing from the most common colour, at least "
                f"{CONTENT_MIN_FRACTION:.0%}. A still picture cannot prove the guest is not stuck after drawing it; "
                "picture_changed says whether captures differed.",
        "boot_seconds": boot_seconds,
    }


def manifest(root: Path) -> dict:
    """Size and modification time of every file under a folder, to prove it was not written to."""
    entries = {}
    for dirpath, _dirs, files in os.walk(root):
        for f in files:
            p = Path(dirpath) / f
            try:
                st = p.stat()
                entries[str(p.relative_to(root))] = [st.st_size, st.st_mtime_ns]
            except OSError as exc:
                entries[str(p.relative_to(root))] = ["unreadable", str(exc)]
    return entries


def cmd_boot_diag(args) -> int:
    out = Path(args.out).resolve()
    build = Path(args.build).resolve()
    package = Path(args.package).resolve()
    host = Host(args.bottle, args.wine)
    diag = {"tool": "sx_regress boot-diag", "started": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "build": str(build),
            "commit": args.commit, "package": str(package), "status": None}

    def finish(status, reason=None) -> int:
        diag["status"], diag["reason"] = status, reason
        diag["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
        out.mkdir(parents=True, exist_ok=True)
        (out / "boot-diag.json").write_text(json.dumps(diag, indent=1))
        print(json.dumps({k: diag.get(k) for k in ("status", "reason", "last_stage", "first_missing_import",
                                                    "first_failing_graphics_call", "first_error", "first_guest_fault",
                                                    "frame_captures",
                                                    "package_unchanged")}, indent=1))
        print(f"-> {out / 'boot-diag.json'}")
        return 0 if status == PASS else 1

    if host.blocked:
        return finish(BLOCKED, host.blocked)
    if not (build / "scarlettx.exe").exists():
        return finish(BLOCKED, f"scarlettx.exe not found in {build}")
    if not package.is_dir():
        return finish(BLOCKED, f"package folder not found: {package}")
    try:
        out.relative_to(package)
        return finish(BLOCKED, "--out must not be inside the package")
    except ValueError:
        pass
    # A package that did not come with the build is the user's own title: it needs an explicit go.
    try:
        package.relative_to(build / "packages")
        synthetic = True
    except ValueError:
        synthetic = False
    diag["package_kind"] = "synthetic (from the build tree)" if synthetic else "user-supplied"
    if not synthetic and not args.confirm_user_package:
        return finish(BLOCKED, "this package is not one of the build's synthetic packages; pass --confirm-user-package "
                               "to boot a title you own")
    out.mkdir(parents=True, exist_ok=True)
    diag["host"] = host.describe()

    before = manifest(package)
    diag["package_files"] = len(before)
    diag["package_bytes"] = sum(v[0] for v in before.values() if isinstance(v[0], int))

    # Boot from a copy-on-write clone when possible, so nothing can write to the user's files.
    boot_root, view = package, None
    if args.clone != "never" and sys.platform == "darwin":
        view = out / "package-view"
        if view.exists():
            shutil.rmtree(view)
        same_volume = os.stat(package).st_dev == os.stat(out).st_dev
        if same_volume:
            cloned = subprocess.run(["cp", "-c", "-R", str(package), str(view)], capture_output=True, text=True)
            if cloned.returncode == 0:
                boot_root = view
                diag["package_access"] = "APFS copy-on-write clone in --out (the original is never opened by the guest)"
            else:
                view = None
                diag["package_access"] = "direct (clone failed: " + cloned.stderr.strip()[:120] + ")"
        else:
            view = None
            diag["package_access"] = "direct (--out is on another volume, so no clone was made)"
        if boot_root is package and args.clone == "require":
            return finish(BLOCKED, "a clone was required (--clone require) but could not be made: " + diag["package_access"])
    else:
        diag["package_access"] = "direct"

    logs = out / "logs"
    env = {"SCARLETTX_SAVE_ROOT": host.win_path(out / "saves"),
           "SCARLETTX_DUMP_FRAME": host.win_path(out / "frames" / "frame"),
           "SCARLETTX_DUMP_FRAME_AT": args.dump_frames_at}
    (out / "saves").mkdir(exist_ok=True)
    (out / "frames").mkdir(exist_ok=True)
    exe = build / "scarlettx.exe"
    win_pkg = host.win_path(boot_root)

    # Stage 1: static inspection (manifest, executable, imports).
    r = run_logged(host, exe, ["inspect", win_pkg], env, build, logs, "inspect", args.timeout)
    inspect = {"exit_code": r["exit_code"], "stdout_log": r["stdout_log"], "stderr_log": r["stderr_log"]}
    for key, pattern in (("title", r"^Title: (.*)$"), ("identity", r"^Identity: (.*)$"),
                         ("scarlett_native", r"^Scarlett Native: (.*)$"), ("executable", r"^Executable: (.*)$")):
        m = re.search(pattern, r["stdout"], re.M)
        inspect[key] = m.group(1).strip() if m else None
    inspect["imports"] = [{"dll": m.group(1), "hle": bool(m.group(2)), "symbols": int(m.group(3))} for m in
                          re.finditer(r"^\s+- (\S+)( \[HLE -> [^\]]*\])? \((\d+) symbols\)", r["stdout"], re.M)]
    diag["inspect"] = inspect
    reached = []
    all_out, all_err = r["stdout"], r["stderr"]
    if r["exit_code"] == 0:
        reached.append("package_inspected")
        # Stage 2: the bounded boot.
        boot_args = ["boot", win_pkg, "--frames", str(args.frames)]
        boot_args += ["--warp"] if args.warp else ["--gpu"]
        if args.in_process:
            boot_args.append("--in-process")
        if not args.windowed:
            boot_args.append("--headless")
        if args.trace:
            boot_args.append("--trace")
        if args.stats:
            boot_args.append("--stats")
        if args.rt_dump:
            boot_args += ["--rt-dump", host.win_path(out / "frames" / "rt_dump.ppm")]
        if args.raster_dump:
            boot_args += ["--raster-dump", host.win_path(out / "frames" / "raster_dump.ppm")]
        if args.allow_pc_target:
            boot_args.append("--allow-pc-target")
        boot_args += args.extra_arg
        cap = None
        if args.windowed:
            cap = WindowCapture(out)
            if cap.blocked:
                diag["window_capture"] = "BLOCKED: " + cap.blocked
                cap = None
        owners = {exe.name.lower()}
        if inspect["executable"]:
            owners.add(re.split(r"[\\/]", inspect["executable"].split(" (PE64")[0])[-1].lower())

        def content_held() -> bool:
            held = content_span(cap.windows)
            return held is not None and held >= args.until_window

        if args.until_window is not None and not cap:
            if view and not args.keep_view:
                shutil.rmtree(view, ignore_errors=True)
            return finish(BLOCKED, "--until-window needs --windowed and working window capture: "
                          + str(diag.get("window_capture", "--windowed was not given")))
        # A sleeping display makes every window capture fail, so keep it awake while the boot runs.
        awake = None
        if cap and shutil.which("caffeinate"):
            awake = subprocess.Popen(["caffeinate", "-d", "-u"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            b = run_logged(host, exe, boot_args, env, build, logs, "boot", args.timeout,
                           while_running=(lambda: cap.grab_guest(out / "window-captures", owners)) if cap else None,
                           stop_when=content_held if (cap and args.until_window is not None) else None)
        finally:
            if awake:
                awake.terminate()
        diag["boot"] = strip_logs(b)
        all_out += b["stdout"]
        all_err += b["stderr"]
        combined = b["stdout"] + "\n" + b["stderr"]
        for stage, regex in BOOT_STAGES[1:]:
            if regex and regex.search(combined):
                reached.append(stage)
        if cap:
            diag["guest_window"] = summarise_guest_window(cap.windows, b["seconds"])
            if diag["guest_window"]["captures"]:
                reached.append("guest_window_visible")
            if diag["guest_window"]["content_seconds"] is not None:
                reached.append("guest_window_content")
        diag["adapters"] = adapters_in(b["stderr"])
        diag["window_captures"] = [str(p) for _a, _b, p in cap.shots] if cap else []
    diag["stages_reached"] = reached
    order = [s for s, _ in BOOT_STAGES]
    diag["last_stage"] = max(reached, key=order.index) if reached else "none"

    # Missing imports: left as stubs when the image was mapped, then the first one the guest called.
    static_stubs = [{"stub": int(m.group(1)), "import": m.group(2), "target": m.group(3)}
                    for m in STATIC_STUB_RE.finditer(all_out)]
    hit = STUB_HIT_RE.search(all_out)
    unimpl = WINE_UNIMPL_RE.search(all_err)
    diag["imports_left_as_stubs"] = static_stubs
    diag["first_missing_import"] = None
    if hit:
        diag["first_missing_import"] = {"import": hit.group(1), "how": "first auto-stub the guest called"}
    elif unimpl:
        diag["first_missing_import"] = {"import": unimpl.group(1), "how": "host DLL has no implementation"}
    elif static_stubs:
        diag["first_missing_import"] = {"import": static_stubs[0]["import"], "how": "left as a stub at map time, not called"}
    diag["first_failing_graphics_call"] = first_match(GRAPHICS_FAIL_RE, all_err, all_out)
    diag["first_error"] = first_match(re.compile(CRASH_RE.pattern + "|" + LOADER_ERR_RE.pattern), all_err, all_out)
    diag["first_guest_fault"] = first_match(GUEST_FAULT_RE, all_err, all_out)

    # Frames: what the shim dumped, what the emulator was asked to dump, and window captures.
    frames = sorted(str(p) for p in (out / "frames").glob("*") if p.is_file())
    try:
        from PIL import Image
        for f in list(frames):
            if f.lower().endswith((".bmp", ".ppm")):
                png = f.rsplit(".", 1)[0] + ".png"
                Image.open(f).save(png)
                frames.append(png)
    except ImportError:
        pass
    diag["frame_captures"] = frames + diag.get("window_captures", [])

    after = manifest(package)
    changed = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k))
    diag["package_unchanged"] = not changed
    diag["package_changes"] = changed[:50]
    if view and not args.keep_view:
        view_after = manifest(view)
        diag["writes_inside_the_clone"] = sorted(
            k for k in set(before) | set(view_after)
            if (before.get(k) or [None])[0] != (view_after.get(k) or [None])[0] or k not in before)[:50]
        shutil.rmtree(view, ignore_errors=True)

    if changed:
        return finish(FAIL, "the package was modified during the run")
    boot = diag.get("boot")
    if not boot:
        return finish(FAIL, "inspection failed: " + str(diag["first_error"]))
    gw = diag.get("guest_window")
    fault = diag["first_guest_fault"]
    if fault:
        # A picture on screen does not make a crashed guest a pass.
        if gw and gw["content_seconds"] is not None:
            seen = f"its window '{gw['title']}' had shown rendered content for {gw['content_seconds']} s"
        else:
            seen = "no rendered window was seen"
        how = ("the runner then ended the boot" if boot.get("stopped_by_runner") else
               "the boot then timed out" if boot["timed_out"] else f"the boot then exited with {boot['exit_code']}")
        return finish(FAIL, f"the guest crashed ({fault.strip()[:160]}); {seen}; {how} "
                            f"(last stage: {diag['last_stage']})")
    if boot.get("stopped_by_runner"):
        return finish(PASS, f"guest window '{gw['title']}' showed rendered content for {gw['content_seconds']} s; "
                            f"the runner ended the boot (--until-window {args.until_window:g})")
    if boot["timed_out"]:
        if gw is None:
            seen = "window capture was off, so a hang and a running guest cannot be told apart"
        elif gw["content_seconds"] is not None:
            seen = (f"a guest window '{gw['title']}' with rendered content was up for {gw['content_seconds']} s, "
                    "so the guest was running, not stopped before its first picture")
        elif gw["captures"]:
            seen = f"a guest window '{gw['title']}' was up but stayed blank"
        elif gw["seen_without_capture"]:
            return finish(BLOCKED, f"boot did not end within {args.timeout} s (last stage: {diag['last_stage']}); "
                                   f"a guest window '{gw['title']}' was listed but could not be captured "
                                   "(display asleep or screen locked?), so rendering could not be judged")
        else:
            seen = "no guest window appeared"
        return finish(FAIL, f"boot did not end within {args.timeout} s (last stage: {diag['last_stage']}); {seen}")
    if boot["exit_code"] != 0:
        return finish(FAIL, f"boot exited with {boot['exit_code']} (last stage: {diag['last_stage']})")
    return finish(PASS, f"boot ended cleanly after --frames {args.frames}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--build", required=True, help="build tree with the EXEs, shim DLLs and packages/")
        p.add_argument("--out", required=True, help="folder for logs, captures and the JSON summary")
        p.add_argument("--bottle", default="scarlettx-m4", help="CrossOver bottle (macOS)")
        p.add_argument("--wine", default=CROSSOVER_WINE, help="CrossOver's wine launcher (macOS)")
        p.add_argument("--timeout", type=int, default=120, help="seconds per test or boot")
        p.add_argument("--commit", default=None, help="source commit of the build, recorded in the summary")

    r = sub.add_parser("regress", help="CTest inventory + offscreen probes + presentation")
    common(r)
    r.add_argument("--inventory", required=True, help="output of ctest --show-only=json-v1 for this build")
    r.add_argument("--sha256-list", help="'<sha256>  <relative path>' lines to verify the build against")
    r.add_argument("--only", type=lambda s: set(s.split(",")), help="comma-separated test names")
    r.add_argument("--hold-ms", type=int, default=4000, help="how long each presentation stage is held")
    r.add_argument("--skip-present", action="store_true", help="leave out the on-screen presentation check")
    r.set_defaults(func=cmd_regress)

    b = sub.add_parser("boot-diag", help="bounded, traced boot of one package")
    common(b)
    b.add_argument("--package", required=True, help="package folder (never written to)")
    b.add_argument("--frames", type=int, default=3, help="frames after which the emulator stops the guest")
    b.add_argument("--in-process", action="store_true", help="map the title into the emulator (real titles)")
    b.add_argument("--windowed", action="store_true", help="show the output window and capture it")
    b.add_argument("--warp", action="store_true", help="ask for the software rasterizer instead of --gpu")
    b.add_argument("--trace", action="store_true", help="log shim calls (large)")
    b.add_argument("--stats", action="store_true")
    b.add_argument("--rt-dump", action="store_true", help="ask the emulator for a ray-traced frame dump in --out/frames")
    b.add_argument("--raster-dump", action="store_true", help="ask the emulator for a raster frame dump in --out/frames")
    b.add_argument("--allow-pc-target", action="store_true",
                   help="also boot a GDK package built for PC (it bypasses the d3d12_xs shim: use --until-window)")
    b.add_argument("--until-window", type=float, default=None, metavar="SECONDS",
                   help="end the boot as PASS once the guest's own window has shown rendered content this long "
                        "(for titles that log no frames; needs --windowed)")
    b.add_argument("--extra-arg", action="append", default=[], help="extra scarlettx boot argument (repeatable)")
    b.add_argument("--dump-frames-at", default="1,2,5,30,90", help="PresentX frame numbers the shim dumps")
    b.add_argument("--clone", choices=["auto", "require", "never"], default="auto",
                   help="boot from a copy-on-write clone of the package (macOS/APFS)")
    b.add_argument("--keep-view", action="store_true", help="keep the clone in --out after the run")
    b.add_argument("--confirm-user-package", action="store_true",
                   help="required for any package that is not one of the build's synthetic packages")
    b.set_defaults(func=cmd_boot_diag)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
