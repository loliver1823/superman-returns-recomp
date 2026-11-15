#!/usr/bin/env python3
"""canary_compare.py - our build vs Xenia Canary, side by side, on the same PC (RELEASE_PLAN phase 5,
criterion C3). Canary lane, iter-467. docs/CANARY_COMPARISON.md has the method, the baseline and
what every number means.

Both programs run the SAME disc image through the SAME scenes, driven to the same points:
  boot (intro logo movies, no presses) -> the title (PRESS START over the 3D city, 10 s) -> START ->
  the front end's last screen (ours: LOAD A GAME? -> A = START NEW GAME; Xenia without a profile:
  DOWN A = continue) -> loading -> the guest requests screens/hud/main_hud (THE ANCHOR) -> one input
  tape played from the anchor on both sides (ours: SR_INPUT_REPLAY; Xenia: a ViGEm virtual pad,
  tools/xenia_pad_tape.py): the opening movie, Metropolis on foot, flight, the pause menu
  (SCENES below; windows verified on the survey pair, docs/CANARY_COMPARISON.md).
and both are timed by ONE instrument: tools/perf/present_probe.js (frida, py -3.13) stamps every
present submission each process makes to the kernel graphics driver (win32u present syscalls), so
"frame time" is the interval between two presents of that process, measured by the same code at the
same layer on both sides (calibrated against an app's own Present log: `selftest`). Game frames
(the guest's own D3DDeviceSwap) are counted too: Xenia by a probe hook on the JIT'd guest function,
ours by SR_PERF_STATS (which also cross-checks the probe's present stamps).

Rules it keeps (archive/v1_orch/LANE_BRIEF.md): it takes the game slot itself (one game process at a
time; one hold per --per-hold runs, so other lanes are not starved), never takes the foreground
(our build: SR_INSTANCE_ID; Xenia: tools/bg_window.py), never reads real input (ours ignores real
pads in automation; Xenia reads every XInput pad, so the run REFUSES when a real pad is connected),
plays no sound (ours: SR_AUDIO_SINK=null; Xenia: --mute=true), never uses exclusive fullscreen.
Fairness: both windows sit behind other windows, where Windows 11 stops honouring a process's timer
resolution; our exe opts itself out (kernel/host_automation.cpp SR_BG_HIGH_QOS), and this tool
applies the same opt-out to Xenia's process (SetProcessInformation, ProcessPowerThrottling).

    python tools/perf/canary_compare.py selftest --out archive/v1_canary/selftest       # the instrument check (no slot)
    python tools/perf/canary_compare.py batch --out-root archive/v1_canary/<tag> \\
        [--configs xenia-1x,xenia-1x-novsync,ours-1x-30,ours-1x-60,ours-1x-unc,xenia-3x,xenia-3x-novsync,ours-3x-60,ours-3x-unc] \\
        [--per-hold 2] [--exe <snapshot exe>] [--owner canary]
    python tools/perf/canary_compare.py run --config ours-1x-60 --out-root <dir> [--slot] [--survey]
    python tools/perf/canary_compare.py report --out-root archive/v1_canary/<tag>    # report.md, readme_table.md, sheets/
    python tools/perf/canary_compare.py tape --out <file.inpr>                        # the route tape
    ... [--route <a1 | tools/perf/routes/<a2 route>.json>]   # the route version (default a1, the 10-09 route;
        an a2 route file: tools/perf/canary_routes.py); report [--route-ref <dir>]: the route-view labels
        (ROUTE-DIVERGED); run --survey [--extra-gates SR_X=V,...]: survey rolls only. Every a2 roll of ours carries
        the a2 log pin (canary_routes.A2_LOG_PIN: SR_LOG=dev), recorded in run.json; --no-a2-log-pin is for two
        pre-registered measured blocks only: the overhead check's reference arm (tools/perf/route_log_cost.py) and
        the unpinned K block (tools/perf/route_repeat.py --profile kperf, Fable ruling 42)
    python tools/perf/canary_compare.py batch --pacing --out-root <dir> [--configs xenia-1x,ours-1x-30,xenia-1x] \\
        --per-hold 3 --exe <snapshot exe>          # display.registers' timing reference (default: the pilot pair)
    python tools/perf/xenia_pacing_cmp.py <dir>/xenia-1x <dir>/ours-1x-30 [--calibration <dir>/xenia-1x_r2] [--pilot]

Run from the repo root that holds "Superman Returns (USA)" (a junction is fine). Python 3.11 with
psutil + vgamepad + PIL + numpy; frida in `py -3.13` (--frida-python). Xenia: the owner's Canary
install and ISO (--xenia, --iso), its config untouched (every difference is a command-line flag).
"""
from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import glob
import hashlib
import json
import os
import re
import shutil
import statistics
import struct
import subprocess
import sys
import threading
import time
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
sys.path.insert(0, HERE)
import canary_routes as routes  # noqa: E402  (the route versions: a1 = ROUTE / SCENES below, a2 route files)

# The Xenia Canary exe and your own disc image: --xenia / --iso, or SR_XENIA_EXE / SR_GAME_ISO
# (as tools/xenia_audio_capture.py); a run refuses without them.
XENIA = os.environ.get("SR_XENIA_EXE", "")
ISO = os.environ.get("SR_GAME_ISO", "")
INSTANCE = "canary"
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

# ------------------------------------------------------------------------------------ configs
# fps: None = the game's own rate (no port feature: the console's 30 Hz render loop + vsync pacing);
# "60" = SR_HIGHFPS=60 (cap 60, vsync since 60 <= the refresh); "uncapped" = SR_HIGHFPS=uncapped.
# Output window: the SAME client size on both sides. Xenia keeps the owner's configured window
# (window_size 1280x720 LOGICAL pixels: on this PC's 4K desktop at 300 % that is a 3840x2039
# client, its menu bar excluded), and ours is given exactly the client size Xenia produced
# ("match": read from a Xenia run in the same --out-root, else derived from the system DPI).
# So 1x and 3x differ only in the internal resolution, as for a player on the owner's 4K TV.
CONFIGS = {
    "xenia-1x": {"side": "xenia", "scale": 1, "vsync": True,
                 "label": "Xenia Canary 1x (owner's config: vsync on)"},
    "xenia-1x-novsync": {"side": "xenia", "scale": 1, "vsync": False,
                         "label": "Xenia Canary 1x, vsync off"},
    "xenia-3x": {"side": "xenia", "scale": 3, "vsync": True,
                 "label": "Xenia Canary 3x = 4K (vsync on)"},
    "xenia-3x-novsync": {"side": "xenia", "scale": 3, "vsync": False,
                         "label": "Xenia Canary 3x = 4K, vsync off"},
    "ours-1x-30": {"side": "ours", "scale": 1, "fps": None,
                   "label": "ours 1x, the game's own 30 fps"},
    "ours-1x-60": {"side": "ours", "scale": 1, "fps": "60",
                   "label": "ours 1x, 60 fps cap (vsync)"},
    "ours-1x-unc": {"side": "ours", "scale": 1, "fps": "uncapped",
                    "label": "ours 1x, uncapped"},
    "ours-3x-30": {"side": "ours", "scale": 3, "fps": None,
                   "label": "ours 3x = 4K, the game's own 30 fps"},
    "ours-3x-60": {"side": "ours", "scale": 3, "fps": "60",
                   "label": "ours 3x = 4K, 60 fps cap (vsync)"},
    "ours-3x-unc": {"side": "ours", "scale": 3, "fps": "uncapped",
                    "label": "ours 3x = 4K, uncapped"},
}
DEFAULT_SET = ["xenia-1x", "xenia-1x-novsync", "ours-1x-30", "ours-1x-60", "ours-1x-unc",
               "xenia-3x", "xenia-3x-novsync", "ours-3x-60", "ours-3x-unc"]
# the README tables: one per internal resolution, Canary's two vsync settings next to ours
README_TABLES = [
    ("1x (1280x720 internal)", ["xenia-1x", "xenia-1x-novsync", "ours-1x-30", "ours-1x-60", "ours-1x-unc"]),
    ("4K (3x = 3840x2160 internal)", ["xenia-3x", "xenia-3x-novsync", "ours-3x-60", "ours-3x-unc"]),
]
SHORT = {"xenia-1x": "Canary, vsync on", "xenia-1x-novsync": "Canary, vsync off", "ours-1x-30": "ours, 30 cap",
         "ours-1x-60": "ours, 60 cap", "ours-1x-unc": "ours, uncapped", "xenia-3x": "Canary, vsync on",
         "xenia-3x-novsync": "Canary, vsync off", "ours-3x-60": "ours, 60 cap", "ours-3x-unc": "ours, uncapped"}

# ------------------------------------------------------------------------------------ the route
# Seconds after the anchor (the guest's main_hud request). One table drives the tape AND the scenes.
ROUTE = {
    "a_tap": 42.0,          # leave the in-engine scene if it waits for input
    "pause1": 50.0,         # START: the pause menu (Xenia harness: gameplay with the HUD after RESUME)
    "resume1": 54.0,        # A = RESUME
    "pan": (58.0, 70.0),    # on foot: slow camera yaw (right stick)
    "walk": (70.0, 80.0),   # on foot: run forward
    "takeoff": 80.0,        # Y held 1.2 s: power launch
    "fly": (83.0, 113.0),   # flight: full stick + RT, yaw pulses every 6 s
    "hover": (113.0, 123.0),
    "pause2": 123.0,        # START: the pause menu, held
    "end": 140.0,
}
# --pacing (native/dreg-xenia: display.registers' timing reference, graded by tools/perf/xenia_pacing_cmp.py): the
# same tape, the pause menu it opens at pause2 held to the anchor + PACING_HOLD_END s, and tools/perf/pacing_probe.py
# on both sides INSTEAD of present_probe.py (one instrument per process, the same guest hooks on both: the guest
# present's entry and the Apt screen requests). No screenshot from pause2 until the probe has stopped (a capture
# perturbs the next ~1 s); then one of the held menu (pause_end), the proof it was still up. Two configs only: the
# pause menu runs at 30 fps on both (docs/CANARY_COMPARISON.md), Canary with the owner's vsync on.
PACING_HOLD_END = 300.0
# --pacing: the probe's present hook must be on by the anchor + this, else the roll ends there (rc 9) and names why
# (dxp0: ours' probe never got past its guest init and the roll held the slot for 5 minutes to record nothing)
PACING_ATTACH_S = 30.0
PACING_CONFIGS = ("xenia-1x", "ours-1x-30")
PACING_PILOT = ["xenia-1x", "ours-1x-30"]           # information only: the instrument check and the tolerances
PACING_GRADED = ["xenia-1x", "ours-1x-30", "xenia-1x"]   # control, target, calibration (one hold)
# Scene windows, verified on the survey pair (archive/v1_canary/survey: a screenshot every 2.5 s on
# both sides, sheet_*.jpg): first_frame = the guest's first D3DDeviceSwap; title = its request of
# screens/bootflow/ScrPressStart; main_hud = its request of screens/hud/main_hud (made AT the NEW
# GAME press: a LOADING screen, then the opening movie +2.5..+30 s, then gameplay with the HUD from
# about +33 s); tape_start = the moment the route tape started on that side (ours: the request
# itself; Xenia: when tools/xenia_pad_tape.py saw the anchor, ~0.5 s later).
SCENES = [
    {"name": "intro", "label": "Intro movies (EA, DC, WB, legal, the game's intro)", "from": ["first_frame", 8.0], "to": ["first_frame", 56.0]},
    {"name": "title", "label": "Title: PRESS START over the 3D city", "from": ["title", 2.0], "to": ["title", 8.5]},
    {"name": "fmv", "label": "Opening movie (FMV)", "from": ["main_hud", 4.0], "to": ["main_hud", 28.0]},
    {"name": "street", "label": "Gameplay start: standing in the street", "from": ["tape_start", 36.0], "to": ["tape_start", 48.5]},
    {"name": "on_foot", "label": "On foot: tutorial overlay, camera pan, run", "from": ["tape_start", 56.0], "to": ["tape_start", 79.5]},
    {"name": "flight", "label": "Flight through the streets (full speed)", "from": ["tape_start", 86.0], "to": ["tape_start", 96.0]},
    {"name": "wall", "label": "Flight mode against a building (close-up wall)", "from": ["tape_start", 98.5], "to": ["tape_start", 122.5]},
    {"name": "pause", "label": "Pause menu (over gameplay)", "from": ["tape_start", 126.0], "to": ["tape_start", 138.0]},
]
LOADS = [
    ("boot_first_frame", "Launch to the first game frame", "launch", "first_frame"),
    ("boot_title", "Launch to the title (PRESS START requested)", "launch", "title"),
    ("newgame_movie_end", "NEW GAME press to the end of the opening movie (the level loads behind it)", "newgame_press", "movie_end"),
]

# ------------------------------------------------------------------------------------ win32 helpers
_u32 = ctypes.WinDLL("user32", use_last_error=True)
_g32 = ctypes.WinDLL("gdi32", use_last_error=True)
_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
try:
    _u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))   # per-monitor v2: physical pixels
except Exception:  # noqa: BLE001
    pass
_k32.OpenProcess.restype = wintypes.HANDLE
_k32.SetProcessInformation.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
_u32.GetDC.restype = wintypes.HDC
_u32.GetDC.argtypes = [wintypes.HWND]
_u32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
_g32.CreateCompatibleDC.restype = wintypes.HDC
_g32.CreateCompatibleDC.argtypes = [wintypes.HDC]
_g32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
_g32.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
_g32.SelectObject.restype = wintypes.HGDIOBJ
_g32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
_g32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
_g32.DeleteDC.argtypes = [wintypes.HDC]
_g32.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT, ctypes.c_void_p,
                           ctypes.c_void_p, wintypes.UINT]
_u32.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
_u32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]


def qos_opt_out(pid):
    """The background opt-out our exe applies to itself (kernel/host_automation.cpp applyBackgroundQos):
    no EcoQoS, timer resolution honoured while occluded. Applied from outside to Xenia."""
    class PPTS(ctypes.Structure):
        _fields_ = [("Version", wintypes.ULONG), ("ControlMask", wintypes.ULONG), ("StateMask", wintypes.ULONG)]
    h = _k32.OpenProcess(0x0200 | 0x1000, False, pid)
    if not h:
        return "open failed (%d)" % ctypes.get_last_error()
    st = PPTS(1, 0x1 | 0x4, 0)
    ok = _k32.SetProcessInformation(h, 4, ctypes.byref(st), ctypes.sizeof(st))
    err = ctypes.get_last_error()
    _k32.CloseHandle(h)
    return "ok" if ok else "set failed (%d)" % err


def xinput_connected():
    """XInput slots with a pad connected (before our virtual pad exists = real pads)."""
    try:
        x = ctypes.WinDLL("xinput1_4")
    except OSError:
        return []
    buf = ctypes.create_string_buffer(16)
    return [i for i in range(4) if x.XInputGetState(i, buf) == 0]


def game_window(pid):
    import bg_window
    wins = bg_window.top_windows(pid)
    best, area = None, -1
    for h in wins:
        r = wintypes.RECT()
        _u32.GetClientRect(h, ctypes.byref(r))
        a = (r.right - r.left) * (r.bottom - r.top)
        if a > area:
            best, area = h, a
    return best


def client_size(hwnd):
    r = wintypes.RECT()
    _u32.GetClientRect(hwnd, ctypes.byref(r))
    return r.right - r.left, r.bottom - r.top


def print_window(pid, path, max_w=3840):
    """PrintWindow(PW_CLIENTONLY | PW_RENDERFULLCONTENT): the window's own content via DWM, while it
    sits behind other windows; never activates it. Returns (w, h, mean luma) or None."""
    from PIL import Image
    hwnd = game_window(pid)
    if not hwnd:
        return None
    w, h = client_size(hwnd)
    if w <= 0 or h <= 0:
        return None
    sdc = _u32.GetDC(None)
    mdc = _g32.CreateCompatibleDC(sdc)
    bmp = _g32.CreateCompatibleBitmap(sdc, w, h)
    old = _g32.SelectObject(mdc, bmp)
    ok = _u32.PrintWindow(hwnd, mdc, 3)
    class BIH(ctypes.Structure):
        _fields_ = [("biSize", wintypes.DWORD), ("biWidth", ctypes.c_long), ("biHeight", ctypes.c_long),
                    ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                    ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                    ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wintypes.DWORD), ("biClrImportant", wintypes.DWORD)]
    bih = BIH(ctypes.sizeof(BIH), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    lines = _g32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bih), 0)
    _g32.SelectObject(mdc, old)
    _g32.DeleteObject(bmp)
    _g32.DeleteDC(mdc)
    _u32.ReleaseDC(None, sdc)
    if not ok or lines != h:  # PrintWindow failed, or the pixels were not all copied out (no image, not a black one)
        return None
    img = Image.frombuffer("RGB", (w, h), buf.raw, "raw", "BGRX", 0, 1)
    if w > max_w:
        img = img.resize((max_w, int(h * max_w / w)), Image.BILINEAR)
    img.save(path, compress_level=1)
    luma = sum(img.convert("L").resize((64, 36)).getdata()) / (64 * 36)
    return w, h, round(luma, 1)


# ------------------------------------------------------------------------------------ small utils
def now():
    return time.time()


def sha256(path, cap=None):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        n = 0
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
            n += len(b)
            if cap and n >= cap:
                break
    return h.hexdigest()


def find_proc(name, since, exe=None, timeout=120.0):
    import psutil
    end = now() + timeout
    while now() < end:
        for p in psutil.process_iter(["name", "create_time", "exe"]):
            try:
                if (p.info["name"] or "").lower() == name.lower() and p.info["create_time"] >= since - 2:
                    if exe and p.info["exe"] and os.path.normcase(p.info["exe"]) != os.path.normcase(exe):
                        continue
                    return p
            except Exception:  # noqa: BLE001
                continue
        time.sleep(0.25)
    return None


class Events:
    def __init__(self, path):
        self.f = open(path, "a", encoding="utf-8")
        self.lock = threading.Lock()
        self.marks = {}

    def add(self, _ev, t=None, **kw):
        rec = {"t": t if t is not None else now(), "ev": _ev}
        rec.update(kw)
        with self.lock:
            self.f.write(json.dumps(rec) + "\n")
            self.f.flush()
            if _ev.startswith("mark:"):
                self.marks.setdefault(_ev[5:], rec["t"])
        return rec

    def close(self):
        self.f.close()


class Sampler(threading.Thread):
    """Every second: the game process's CPU (cores), private / working-set MB, the rest of the
    machine's busy cores (contention: the owner and other lanes), and the GPU (nvidia-smi, whole
    GPU: one game runs at a time)."""

    def __init__(self, pid, out):
        super().__init__(daemon=True)
        self.pid, self.out, self.stop = pid, out, threading.Event()
        self.smi = None

    def run(self):
        import psutil
        smi = shutil.which("nvidia-smi")
        if smi:
            self.smi = subprocess.Popen([smi, "--query-gpu=timestamp,utilization.gpu,memory.used,clocks.gr,power.draw",
                                         "--format=csv,noheader,nounits", "-lms", "1000"],
                                        stdout=open(os.path.join(os.path.dirname(self.out), "gpu.csv"), "w"),
                                        stderr=subprocess.DEVNULL, creationflags=NO_WINDOW)
        try:
            p = psutil.Process(self.pid)
        except Exception:  # noqa: BLE001
            return
        ncpu = psutil.cpu_count()
        with open(self.out, "w") as f:
            f.write("t,proc_cores,other_cores,private_mb,ws_mb\n")
            last = None
            while not self.stop.is_set():
                try:
                    ct = p.cpu_times()
                    mi = p.memory_info()
                    st = psutil.cpu_times()
                except Exception:  # noqa: BLE001
                    break
                t = now()
                busy = st.user + st.system
                pc = ct.user + ct.system
                if last:
                    dt_ = t - last[0]
                    proc = (pc - last[1]) / dt_
                    allc = (busy - last[2]) / dt_
                    f.write("%.3f,%.2f,%.2f,%.0f,%.0f\n" % (t, proc, max(0.0, min(ncpu, allc) - proc),
                                                            getattr(mi, "private", mi.vms) / 2**20, mi.rss / 2**20))
                    f.flush()
                last = (t, pc, busy)
                self.stop.wait(1.0)
        if self.smi:
            self.smi.terminate()


class Shooter(threading.Thread):
    """Screenshots at (marker, offset) points as the markers arrive."""

    def __init__(self, pid, run_dir, ev, plan):
        super().__init__(daemon=True)
        self.pid, self.dir, self.ev, self.plan = pid, os.path.join(run_dir, "shots"), ev, list(plan)
        self.stop = threading.Event()
        os.makedirs(self.dir, exist_ok=True)

    def run(self):
        done = set()
        while not self.stop.is_set() and len(done) < len(self.plan):
            for i, (name, mark, off) in enumerate(self.plan):
                if i in done or mark not in self.ev.marks:
                    continue
                if now() >= self.ev.marks[mark] + off:
                    done.add(i)
                    path = os.path.join(self.dir, name + ".png")
                    t0 = now()
                    try:
                        r = print_window(self.pid, path)
                    except Exception as e:  # noqa: BLE001
                        r = None
                        self.ev.add("shot-error", name=name, err=str(e))
                    self.ev.add("shot", t=t0, name=name, dur=round(now() - t0, 3), size=r and [r[0], r[1]],
                                luma=r and r[2], ok=bool(r))
            self.stop.wait(0.05)


def shot_plan(survey, pacing=False, scenes=None, end=None):
    plan = []
    scenes = SCENES if scenes is None else scenes
    end = ROUTE["end"] if end is None else end
    for s in scenes:
        # 1.2 s past the window: a capture perturbs the next ~1 s (survey pair), and the live marks
        # know the guest's main_hud request, not the tape start (Xenia's tape starts ~0.55 s later)
        mk = "main_hud" if s["to"][0] == "tape_start" else s["to"][0]
        if pacing and mk == "main_hud" and s["to"][1] + 1.2 >= ROUTE["pause2"]:
            continue   # --pacing: nothing inside the pause hold (pause_end is taken after the probe stops)
        plan.append((s["name"], mk, s["to"][1] + 1.2))
    if survey:
        for k in range(-2, int(end / 2.5) + 2):
            plan.append(("survey_%+06.1f" % (k * 2.5), "main_hud", k * 2.5))
        for k in range(0, 50):
            plan.append(("boot_%03d" % (k * 4), "first_frame", k * 4.0))
        for k in range(0, 6):
            plan.append(("title_%02d" % (k * 2), "title", k * 2.0))
    return plan


# ------------------------------------------------------------------------------------ the tape
def build_tape(path):
    from gen_collision_route_tape import entry, MAGIC, VERSION, STEP_MS
    A, Y, START = 0x1000, 0x8000, 0x0010
    R = ROUTE

    def inside(ms, a, b):
        return a * 1000 <= ms < b * 1000

    ents = []
    for ms in range(0, int(R["end"] * 1000), STEP_MS):
        b = rt = lx = ly = rx = 0
        if inside(ms, R["a_tap"], R["a_tap"] + 0.22) or inside(ms, R["resume1"], R["resume1"] + 0.22):
            b |= A
        if inside(ms, R["pause1"], R["pause1"] + 0.45) or inside(ms, R["pause2"], R["pause2"] + 0.45):
            b |= START
        if inside(ms, *R["pan"]):
            rx = 12000
        if inside(ms, *R["walk"]):
            ly = 26000
        if inside(ms, R["takeoff"], R["takeoff"] + 1.2):
            b |= Y
        if inside(ms, *R["fly"]):
            ly, rt = 32767, 255
            k = (ms - int(R["fly"][0] * 1000)) // 6000
            if (ms - int(R["fly"][0] * 1000)) % 6000 < 1000 and k > 0:
                rx = 16000 if k % 2 else -16000
        ents.append(entry(ms, b, 0, rt, lx, ly, rx, 0))
    with open(path, "wb") as f:
        f.write(struct.pack("<IIII", MAGIC, VERSION, len(ents), 0))
        f.write(b"".join(ents))
    return path


# ------------------------------------------------------------------------------------ live markers
LOG_TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]")
OURS_MARKS = [
    ("title", re.compile(r"\[APT_SCREEN\] path='screens/bootflow/ScrPressStart'")),
    ("popup", re.compile(r"\[APT_SCREEN\] path='screens/misc/ScrGenPop'")),
    ("main_hud", re.compile(r"\[APT_SCREEN\] path='screens/hud/main_hud'")),
]


def log_epoch(line):
    m = LOG_TS.match(line)
    if not m:
        return None
    return dt.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp()


class OursLogWatch(threading.Thread):
    def __init__(self, log, perf_dir, ev, t_launch):
        super().__init__(daemon=True)
        self.log, self.perf, self.ev, self.t0 = log, perf_dir, ev, t_launch
        self.stop = threading.Event()

    def run(self):
        pos, buf, seen = 0, b"", set()
        first = False
        while not self.stop.is_set():
            if not first:
                fc = os.path.join(self.perf, "frames.csv")
                meta = os.path.join(self.perf, "meta.txt")
                try:
                    if os.path.exists(fc) and os.path.exists(meta):
                        m = dict(l.strip().split("=", 1) for l in open(meta) if "=" in l and not l.startswith("env "))
                        for row in open(fc):
                            if row.startswith("S,"):
                                us = int(row.strip().split(",")[2])
                                self.ev.add("mark:first_frame", t=int(m["epoch_ms_t0"]) / 1000.0 + us / 1e6,
                                            src="frames.csv (live, ms-accurate)")
                                first = True
                                break
                except Exception:  # noqa: BLE001
                    pass
            try:
                if os.path.exists(self.log) and os.path.getmtime(self.log) >= self.t0 - 5:
                    with open(self.log, "rb") as f:
                        f.seek(pos)
                        chunk = f.read()
                        pos += len(chunk)
                    buf += chunk
                    lines = buf.split(b"\n")
                    buf = lines.pop()
                    for raw in lines:
                        line = raw.decode("utf-8", "replace")
                        for name, rx in OURS_MARKS:
                            if name not in seen and rx.search(line):
                                t = log_epoch(line)
                                if t:
                                    seen.add(name)
                                    self.ev.add("mark:" + name, t=t, src="game log")
            except OSError:
                pass
            self.stop.wait(0.2)


class XeniaMarkWatch(threading.Thread):
    def __init__(self, markers, ev):
        super().__init__(daemon=True)
        self.path, self.ev = markers, ev
        self.stop = threading.Event()
        self.qmap = None

    def run(self):
        pos, buf, seen = 0, "", set()
        while not self.stop.is_set():
            if os.path.exists(self.path):
                with open(self.path, encoding="utf-8") as f:
                    f.seek(pos)
                    chunk = f.read()
                    pos = f.tell()
                buf += chunk
                lines = buf.split("\n")
                buf = lines.pop()
                for line in lines:
                    try:
                        m = json.loads(line)
                    except ValueError:
                        continue
                    t = m.get("epoch")
                    if m.get("t") == "attached" and m.get("guest") == "0x82112050" and "first_frame" not in seen:
                        seen.add("first_frame")
                        self.ev.add("mark:first_frame", t=t, src="probe: guest D3DDeviceSwap compiled + hooked")
                    if m.get("t") == "apt":
                        p = m.get("path")
                        for name, want in (("title", "screens/bootflow/ScrPressStart"),
                                           ("main_hud", "screens/hud/main_hud")):
                            if p == want and name not in seen:
                                seen.add(name)
                                self.ev.add("mark:" + name, t=t, src="probe: AptLoadByPath")
            self.stop.wait(0.2)


# ------------------------------------------------------------------------------------ one run
def probe_dir(a, run_dir):
    return os.path.join(run_dir, "pacing" if getattr(a, "pacing", False) else "probe")


def pacing_hook(a, run_dir):
    """--pacing: (attached, why) - is the probe's D3DDeviceSwap hook on (its markers.jsonl), and if not, the last
    guest-init / attach error it reported."""
    attached, why = False, "no guest-init or attach note from the probe"
    try:
        for line in open(os.path.join(probe_dir(a, run_dir), "markers.jsonl"), encoding="utf-8"):
            try:
                m = json.loads(line)
            except ValueError:
                continue
            if m.get("t") == "attached" and m.get("guest") == "0x82112050":
                attached = True
            elif m.get("t") in ("guest-init-error", "attach-error"):
                why = m.get("err")
            elif m.get("t") == "guest-init":
                why = "guest init ok, the swap hook not attached (the device chain unreadable?)"
    except OSError as e:
        why = "no markers.jsonl (%s)" % e
    return attached, (None if attached else why)


def start_probe(a, pid, run_dir, xenia, anchor=None):
    if getattr(a, "pacing", False):
        cmd = a.frida_python + [os.path.join(HERE, "pacing_probe.py"), "--out", probe_dir(a, run_dir), "--pid",
                                str(pid), "--side", "xenia" if xenia else "ours", "--module", os.path.basename(a.exe),
                                "--stop-file", os.path.join(run_dir, "probe.stop"), "--max-seconds", "1800"]
        if anchor:
            cmd += ["--anchor-file", anchor]
        return subprocess.Popen(cmd, stdout=open(os.path.join(run_dir, "probe_stdout.txt"), "w"),
                                stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
    pdir = os.path.join(run_dir, "probe")
    cmd = a.frida_python + [os.path.join(HERE, "present_probe.py"), "--out", pdir, "--pid", str(pid),
                            "--stop-file", os.path.join(run_dir, "probe.stop"), "--max-seconds", "1800"]
    if xenia:
        cmd += ["--xenia"]
    if anchor:
        cmd += ["--anchor-file", anchor]
    return subprocess.Popen(cmd, stdout=open(os.path.join(run_dir, "probe_stdout.txt"), "w"),
                            stderr=subprocess.STDOUT, creationflags=NO_WINDOW)


def stop_probe(run_dir, probe, timeout=20):
    open(os.path.join(run_dir, "probe.stop"), "w").close()
    try:
        probe.wait(timeout)
    except subprocess.TimeoutExpired:
        probe.kill()


def hold_end(a):
    """seconds after the anchor at which the roll ends: the route's end, or --pacing's pause hold."""
    return PACING_HOLD_END if getattr(a, "pacing", False) else routes.end_of(getattr(a, "route_def", None))


def pacing_end(a, run_dir, probe, pid, ev):
    """--pacing, at the end of the hold: stop the probe (its final drain), THEN the one screenshot of the held menu."""
    stop_probe(run_dir, probe)
    ev.add("pacing-probe-stopped")
    path = os.path.join(run_dir, "shots", "pause_end.png")
    t0 = now()
    try:
        r = print_window(pid, path)
    except Exception as e:  # noqa: BLE001
        r = None
        ev.add("shot-error", name="pause_end", err=str(e))
    ev.add("shot", t=t0, name="pause_end", dur=round(now() - t0, 3), size=r and [r[0], r[1]], luma=r and r[2],
           ok=bool(r))


def host_info():
    info = {"when": dt.datetime.now().isoformat(timespec="seconds")}
    smi = shutil.which("nvidia-smi")
    if smi:
        try:
            info["gpu"] = subprocess.run([smi, "--query-gpu=name,driver_version", "--format=csv,noheader"],
                                         capture_output=True, text=True, timeout=20).stdout.strip()
        except Exception:  # noqa: BLE001
            pass
    try:
        import psutil
        info["ram_gb"] = round(psutil.virtual_memory().total / 2**30, 1)
        info["ram_free_gb"] = round(psutil.virtual_memory().available / 2**30, 1)
        info["logical_cpus"] = psutil.cpu_count()
    except Exception:  # noqa: BLE001
        pass
    try:
        info["cpu"] = subprocess.run(["powershell", "-NoProfile", "-Command",
                                      "(Get-CimInstance Win32_Processor).Name"], capture_output=True, text=True,
                                     timeout=30, creationflags=NO_WINDOW).stdout.strip()
        info["display"] = subprocess.run(["powershell", "-NoProfile", "-Command",
                                          "Get-CimInstance Win32_VideoController | ForEach-Object { '{0}x{1} @ {2} Hz' -f "
                                          "$_.CurrentHorizontalResolution, $_.CurrentVerticalResolution, "
                                          "$_.CurrentRefreshRate }"], capture_output=True, text=True, timeout=30,
                                         creationflags=NO_WINDOW).stdout.strip()
    except Exception:  # noqa: BLE001
        pass
    return info


def run_one(a, name, out_root):
    cfg = CONFIGS[name]
    if getattr(a, "pacing", False) and (name not in PACING_CONFIGS or a.survey):
        raise SystemExit("--pacing runs %s only, without --survey" % ", ".join(PACING_CONFIGS))
    route = getattr(a, "route_def", None)
    rname = routes.name_of(route)
    tape = os.path.join(out_root, "canary_route.inpr" if rname == routes.A1 else "canary_route_%s.inpr" % rname)
    want = routes.tape_bytes(route)
    if os.path.exists(tape):
        if sha256(tape) != routes.sha256_bytes(want):
            raise SystemExit("%s holds a different tape than route %s builds (sha256 %s, the route's %s): an edited "
                             "route file, or another revision's out-root - use a fresh --out-root"
                             % (tape, rname, sha256(tape), routes.sha256_bytes(want)))
    else:
        os.makedirs(out_root, exist_ok=True)
        with open(tape, "wb") as f:
            f.write(want)
    run_dir = os.path.join(out_root, name)
    if os.path.exists(run_dir) and os.listdir(run_dir):
        k = 2
        while os.path.exists("%s_r%d" % (run_dir, k)):
            k += 1
        run_dir = "%s_r%d" % (run_dir, k)
    os.makedirs(run_dir)
    ev = Events(os.path.join(run_dir, "events.jsonl"))
    meta = {"config": name, "cfg": cfg, "tool": "tools/perf/canary_compare.py",
            "route": ROUTE if rname == routes.A1 else {k: v for k, v in route.items() if not k.startswith("_")},
            "route_name": rname, "route_file": None if rname == routes.A1 else route["_file"],
            "route_file_sha256": None if rname == routes.A1 else route["_sha256"],
            "scenes": routes.scenes_of(route), "extra_gates": getattr(a, "extra_gates_map", {}) or None,
            # the a2 log pin (canary_routes.A2_LOG_PIN): ours' a2 rolls only; log_pin_off = --no-a2-log-pin
            "log_pin": (getattr(a, "log_pin", {}) or None) if cfg["side"] == "ours" else None,
            "log_pin_off": bool(getattr(a, "no_a2_log_pin", False)) if cfg["side"] == "ours" else None,
            "tape": tape, "tape_sha256": sha256(tape), "host": host_info(), "survey": a.survey}
    if getattr(a, "pacing", False):
        meta["pacing"] = {"hold_end": PACING_HOLD_END, "probe": "tools/perf/pacing_probe.py", "dir": "pacing",
                          "graded_by": "tools/perf/xenia_pacing_cmp.py"}
    print("[%s] run %s -> %s" % (time.strftime("%H:%M:%S"), name, run_dir), flush=True)
    try:
        if cfg["side"] == "ours":
            rc = run_ours(a, cfg, run_dir, tape, ev, meta)
        else:
            rc = run_xenia(a, cfg, run_dir, tape, ev, meta)
    finally:
        meta["marks"] = ev.marks
        with open(os.path.join(run_dir, "run.json"), "w") as f:
            json.dump(meta, f, indent=1)
        ev.close()
    print("[%s] run %s done rc=%s marks=%s" % (time.strftime("%H:%M:%S"), name, rc, sorted(ev.marks)), flush=True)
    return rc


# the harness's own roots (tools/ally-runner): read by the roll tools, never by the game - kept in the child env
HARNESS_ENV = ("SR_MAIN_ROOT", "SR_PYTHON", "SR_SLOT_MIN_FREE_GB")


def child_env(gates, base=None):
    """(env, removed): the environment ours' roll runs with (native/dreg-xenia round 2, Astra round 1 P2-6) - the
    parent's minus every inherited SR_* variable (an SR_NATIVE_VBLANK=1, a high-FPS or diagnostic setting in the shell
    that started the batch must not reach a capture recorded as the baked default; Windows names are case-blind),
    the harness roots kept, plus the roll's own gates. removed = the inherited SR_* names dropped."""
    base = dict(os.environ if base is None else base)
    keep = {k.upper() for k in HARNESS_ENV}
    removed = sorted(k for k in base if k.upper().startswith("SR_") and k.upper() not in keep)
    env = {k: v for k, v in base.items() if k not in removed}
    env.update(gates)
    return env, removed


# --extra-gates may never set these: the harness's own inputs, and (2026-10-22 audio safety) the audio sink / device
# and real input (every roll of ours stays SR_AUDIO_SINK=null, no real pad)
EXTRA_GATES_REFUSED = ("SR_INPUT_REPLAY", "SR_INSTANCE_ID", "SR_PERF_DIR", "SR_AUDIO_SINK", "SR_AUDIO_DEVICE",
                       "SR_HARNESS_REAL_INPUT")
_GATE_NAME = re.compile(r"SR_[A-Z0-9_]+")


def parse_extra_gates(text):
    """--extra-gates NAME=VALUE,... -> {NAME: VALUE}. Names are canonical (upper-cased, SR_ + [A-Z0-9_]: Windows
    environment names are case-blind, so SR_Log and SR_LOG are one variable; Astra pin r1 #1); a refused name or the
    same name twice is an error."""
    out = {}
    for item in text.split(","):
        k, sep, v = item.strip().partition("=")
        k = k.strip().upper()
        if not sep or not _GATE_NAME.fullmatch(k) or k in EXTRA_GATES_REFUSED:
            raise SystemExit("--extra-gates: %r is not an allowed SR_NAME=VALUE" % item)
        if k in out:
            raise SystemExit("--extra-gates: %s given twice (names are case-blind)" % k)
        out[k] = v
    return out


def apply_log_pin(a):
    """a.log_pin = the gates an a2 run / batch adds to ours' side: the a2 log pin (canary_routes.A2_LOG_PIN), or {}
    for a1, for a command without a route, or with --no-a2-log-pin (a measured roll of a pre-registered block: the
    overhead check's reference arm, tools/perf/route_log_cost.py, or the unpinned K block, route_repeat --profile
    kperf, Fable ruling 42). Refused: --no-a2-log-pin on a1 or with --survey (the opt-out is for measured rolls) or
    with an --extra-gates value for a pinned name; on a pinned a2 roll, an --extra-gates value contradicting the pin."""
    a.log_pin = {}
    if not hasattr(a, "route_def"):
        return a.log_pin
    off = bool(getattr(a, "no_a2_log_pin", False))
    extra = getattr(a, "extra_gates_map", {}) or {}
    if a.route_def is None:
        if off:
            raise SystemExit("--no-a2-log-pin is for a2 routes only (a1 rolls never carry the pin)")
        return a.log_pin
    if off:
        if getattr(a, "survey", False):
            raise SystemExit("--no-a2-log-pin is for measured rolls (the overhead check's reference arm, the kperf "
                             "block), not --survey")
        named = sorted(k for k in extra if k in routes.A2_LOG_PIN)
        if named:
            raise SystemExit("--no-a2-log-pin with --extra-gates %s: an unpinned roll runs without the pinned gates"
                             % named)
        return a.log_pin
    clash = {k: v for k, v in extra.items() if k in routes.A2_LOG_PIN and v != routes.A2_LOG_PIN[k]}
    if clash:
        raise SystemExit("--extra-gates %s contradicts the a2 log pin %s (every a2 roll of ours carries it)"
                         % (clash, routes.A2_LOG_PIN))
    a.log_pin = dict(routes.A2_LOG_PIN)
    return a.log_pin


def run_ours(a, cfg, run_dir, tape, ev, meta):
    root = os.path.abspath(a.root)
    exe = os.path.abspath(a.exe)
    meta.update({"exe": exe, "exe_sha256": sha256(exe), "root": root})
    perf = os.path.join(run_dir, "perf")
    gates = {
        "SR_INSTANCE_ID": INSTANCE, "SR_LOG": "player", "SR_MODS": "0", "SR_AUDIO_SINK": "null",
        "SR_FRAME_DUMP": "0",   # the timed BMP dumps of automated rolls (players never have them)
        "SR_PERF_STATS": "1", "SR_PERF_DIR": perf, "SR_INPUT_REPLAY": tape,
        "SR_RES_SCALE": str(cfg["scale"]), "SR_WINDOW_SIZE": "%dx%d" % matched_window(out_root_of(run_dir)),
        "SR_SHADER_RUNTIME_DIR": os.path.abspath(os.path.join(out_root_of(run_dir), "..", "_shader_runtime_cache")),
    }
    if cfg.get("fps"):
        gates["SR_HIGHFPS"] = cfg["fps"]
    gates.update(getattr(a, "log_pin", {}) or {})  # the a2 log pin (canary_routes.A2_LOG_PIN): every a2 roll of ours
    gates.update(getattr(a, "extra_gates_map", {}) or {})  # --extra-gates: survey rolls only (refused otherwise)
    rt_cache = gates["SR_SHADER_RUNTIME_DIR"]
    meta["shader_runtime_cache_files_before"] = len(glob.glob(os.path.join(rt_cache, "**", "*"), recursive=True))
    meta["gates"] = gates
    log = os.path.join(root, "superman_returns_%s.log" % INSTANCE)
    if os.path.exists(log):
        os.remove(log)   # our own instance's log from an earlier run (the live watcher must not read it)
    # --pacing: the same steps with a route dwell that outlasts the pause hold (the dwell is a ceiling)
    scenario = "canary_ours_pacing.json" if getattr(a, "pacing", False) else "canary_ours.json"
    meta["scenario"] = scenario
    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", os.path.join(root, "tools", "sr_drive.ps1"),
           "-Scenario", os.path.join(TOOLS, "scenarios", scenario), "-Out", os.path.join(run_dir, "drive"),
           "-TimeoutSeconds", "900", "-ExePath", exe, "-Gates", ",".join("%s=%s" % kv for kv in gates.items())]
    env, removed = child_env(gates)
    meta["env_explicit"] = True
    meta["env_sr"] = {k: v for k, v in env.items() if k.upper().startswith("SR_")}
    meta["env_removed"] = removed
    if removed:
        print("[%s] ours' child environment: inherited %s removed" % (time.strftime("%H:%M:%S"), ", ".join(removed)),
              flush=True)
    t_start = now()
    ev.add("start", cmd=cmd, env_removed=removed)
    drv = subprocess.Popen(cmd, cwd=root, env=env, stdout=open(os.path.join(run_dir, "drive_stdout.txt"), "w"),
                           stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
    p = find_proc("SupermanReturns.exe", t_start, exe, timeout=180)
    if not p:
        ev.add("error", what="game process never appeared")
        drv.wait()
        return 3
    ev.add("mark:launch", t=p.create_time(), pid=p.pid, src="process create time")
    probe = start_probe(a, p.pid, run_dir, xenia=False)
    samp = Sampler(p.pid, os.path.join(run_dir, "proc.csv"))
    samp.start()
    watch = OursLogWatch(log, perf, ev, t_start)
    watch.start()
    pacing = getattr(a, "pacing", False)
    shoot = Shooter(p.pid, run_dir, ev, shot_plan(a.survey, pacing, meta["scenes"], hold_end(a)))
    shoot.start()
    size_logged = False
    rc = None
    hook = None   # --pacing: pacing_hook() once the anchor + PACING_ATTACH_S has passed
    while rc is None:
        rc = drv.poll()
        if not size_logged and "first_frame" in ev.marks:
            h = game_window(p.pid)
            if h:
                meta["client_size"] = client_size(h)
                size_logged = True
        if pacing and hook is None and "main_hud" in ev.marks and now() > ev.marks["main_hud"] + PACING_ATTACH_S:
            hook = pacing_hook(a, run_dir)
            ev.add("pacing-hook", attached=hook[0], why=hook[1])
            if not hook[0]:
                break
        if "main_hud" in ev.marks and now() > ev.marks["main_hud"] + hold_end(a) + (0 if pacing else 3):
            ev.add("route-done")
            if pacing and drv.poll() is None:
                pacing_end(a, run_dir, probe, p.pid, ev)
                meta["pacing"]["held"] = True
            break
        time.sleep(0.25)
    stop_probe(run_dir, probe)
    # The route is over: end the roll now (sr_drive's last dwell is a ceiling, not a duration).
    if drv.poll() is None:
        try:
            p.terminate()
        except Exception:  # noqa: BLE001
            pass
        try:
            drv.wait(120)
        except subprocess.TimeoutExpired:
            drv.kill()
    for t in (shoot, watch, samp):
        t.stop.set()
    meta["drive_rc"] = drv.returncode
    meta["shader_runtime_cache_files_after"] = len(glob.glob(os.path.join(rt_cache, "**", "*"), recursive=True))
    logs = glob.glob(os.path.join(run_dir, "drive", "attempt*", "run.log"))
    if not logs and os.path.exists(log):
        shutil.copy2(log, os.path.join(run_dir, "game.log"))
    if pacing and hook is not None and not hook[0]:
        meta["pacing"]["no_hook"] = hook[1]
        return 9   # the probe's present hook never came on: nothing to grade, the roll ended early
    if pacing and not meta["pacing"].get("held"):
        return 8   # the game or the drive ended before the pause hold did
    return 0 if "main_hud" in ev.marks else 4


def matched_window(out_root):
    """The client size Xenia's window had in this --out-root (its run.json), else the size its
    1280x720-logical window gets at the system DPI (menu bar: 40.33 logical px, measured)."""
    for p in sorted(glob.glob(os.path.join(out_root, "xenia*", "run.json"))):
        try:
            cs = json.load(open(p)).get("client_size")
        except (OSError, ValueError):
            cs = None
        if cs and cs[0] > 0 and cs[1] > 0:
            return int(cs[0]), int(cs[1])
    try:
        sc = _u32.GetDpiForSystem() / 96.0
    except Exception:  # noqa: BLE001
        sc = 1.0
    return int(round(1280 * sc)), int(round(720 * sc)) - int(round(40.33 * sc))


def out_root_of(run_dir):
    return os.path.dirname(os.path.abspath(run_dir))


def run_xenia(a, cfg, run_dir, tape, ev, meta):
    real = xinput_connected()
    meta["xinput_before_pad"] = real
    if real and not a.allow_real_pad:
        ev.add("error", what="real XInput pad(s) connected at slots %s: Xenia reads every pad - refusing" % real)
        print("REFUSED: a real XInput pad is connected (slots %s); Xenia would read it. --allow-real-pad to override"
              % real, flush=True)
        return 5
    if not a.xenia or not a.iso:
        sys.exit("canary_compare: give --xenia and --iso (or set SR_XENIA_EXE and SR_GAME_ISO)")
    xenia = os.path.abspath(a.xenia)
    meta.update({"xenia": xenia, "xenia_sha256": sha256(xenia), "iso": a.iso,
                 "iso_size": os.path.getsize(a.iso), "iso_head_sha256_64MB": sha256(a.iso, cap=64 << 20)})
    content = os.path.join(run_dir, "content")
    os.makedirs(content, exist_ok=True)
    anchor = os.path.join(run_dir, "anchor.txt")
    cmdf = os.path.join(run_dir, "pad_cmd.txt")
    padlog = os.path.join(run_dir, "pad.log")
    pad = subprocess.Popen(a.pad_python + [os.path.join(TOOLS, "xenia_pad_tape.py"), "--tape", tape,
                                           "--anchor-file", anchor, "--cmd-file", cmdf, "--log", padlog,
                                           "--max-wait", "900"],
                           stdout=open(os.path.join(run_dir, "pad_stdout.txt"), "w"), stderr=subprocess.STDOUT,
                           creationflags=NO_WINDOW)
    time.sleep(3.0)   # the pad must exist before Xenia enumerates input
    flags = ["--mute=true", "--discord=false", "--fullscreen=false", "--log_file=" + os.path.join(run_dir, "xenia.log"),
             "--content_root=" + content, "--draw_resolution_scale_x=%d" % cfg["scale"],
             "--draw_resolution_scale_y=%d" % cfg["scale"], "--vsync=%s" % ("true" if cfg["vsync"] else "false"),
             "--framerate_limit=0"]
    meta["xenia_flags"] = flags
    import bg_window
    t_start = now()
    proc = bg_window.popen_background([xenia] + flags + [a.iso])
    bg_window.guard_background(proc.pid, prev_foreground=proc.sr_prev_foreground)
    ev.add("mark:launch", t=t_start, pid=proc.pid, src="Popen")
    try:
        import psutil
        ct = psutil.Process(proc.pid).create_time()
        ev.add("launch-create-time", t=ct)
        ev.marks["launch"] = ct
    except Exception:  # noqa: BLE001
        pass
    time.sleep(1.0)
    meta["qos_opt_out"] = qos_opt_out(proc.pid)
    probe = start_probe(a, proc.pid, run_dir, xenia=True, anchor=anchor)
    samp = Sampler(proc.pid, os.path.join(run_dir, "proc.csv"))
    samp.start()
    watch = XeniaMarkWatch(os.path.join(probe_dir(a, run_dir), "markers.jsonl"), ev)
    watch.start()
    pacing = getattr(a, "pacing", False)
    shoot = Shooter(proc.pid, run_dir, ev, shot_plan(a.survey, pacing, meta["scenes"], hold_end(a)))
    shoot.start()

    def press(tokens):
        with open(cmdf + ".tmp", "w") as f:
            f.write(tokens)
        os.replace(cmdf + ".tmp", cmdf)
        ev.add("press", tokens=tokens)

    rc = 0
    try:
        # The front end, pressed where our scenario presses: nothing until the title is requested.
        end = now() + 420
        while "title" not in ev.marks and now() < end and proc.poll() is None:
            time.sleep(0.25)
        if "title" not in ev.marks:
            ev.add("error", what="no title request")
            return 6
        while now() < ev.marks["title"] + 10.0:
            time.sleep(0.1)
        for attempt in range(4):
            press("@START")
            t_p = now()
            while now() < t_p + 8.0:
                time.sleep(0.1)
            press("DOWN A")
            t_p = now()
            while "main_hud" not in ev.marks and now() < t_p + 30.0 and proc.poll() is None:
                time.sleep(0.2)
            if "main_hud" in ev.marks:
                break
            ev.add("retry-frontend", attempt=attempt + 1)
        if "main_hud" not in ev.marks:
            ev.add("error", what="no main_hud request")
            rc = 7
        else:
            hook = None   # --pacing: pacing_hook() once the anchor + PACING_ATTACH_S has passed
            while now() < ev.marks["main_hud"] + hold_end(a) + (0 if pacing else 3) and proc.poll() is None:
                if "client_size" not in meta:
                    h = game_window(proc.pid)
                    if h:
                        meta["client_size"] = client_size(h)
                if pacing and hook is None and now() > ev.marks["main_hud"] + PACING_ATTACH_S:
                    hook = pacing_hook(a, run_dir)
                    ev.add("pacing-hook", attached=hook[0], why=hook[1])
                    if not hook[0]:
                        break
                time.sleep(0.25)
            if pacing and hook is not None and not hook[0]:
                meta["pacing"]["no_hook"] = hook[1]
                rc = 9   # the probe's present hook never came on: nothing to grade, the roll ended early
            else:
                ev.add("route-done")
            if pacing and rc != 9:
                if proc.poll() is None:
                    pacing_end(a, run_dir, probe, proc.pid, ev)
                    meta["pacing"]["held"] = True
                else:
                    rc = 8   # Xenia ended before the pause hold did
    finally:
        stop_probe(run_dir, probe)
        for t in (shoot, watch, samp):
            t.stop.set()
        try:
            proc.kill()
            proc.wait(30)
        except Exception:  # noqa: BLE001
            pass
        try:
            pad.kill()
        except Exception:  # noqa: BLE001
            pass
        # the press times from the pad log (the time a token's press began)
        try:
            for line in open(padlog):
                parts = line.split()
                if len(parts) >= 3 and parts[1] in ("cmd", "pre"):
                    ev.add("pad", t=float(parts[0]), line=line.strip())
        except OSError:
            pass
    return rc


# ------------------------------------------------------------------------------------ batch
def slot(a, verb):
    cmd = [sys.executable, os.path.join(TOOLS, "game_slot.py"), verb, "--owner", a.owner]
    if verb == "acquire":
        cmd += ["--poll", "30", "--note", "canary compare"]
    return subprocess.run(cmd, cwd=a.root).returncode


def cmd_batch(a):
    configs = a.configs or ",".join(PACING_PILOT if a.pacing else DEFAULT_SET)
    names = [n.strip() for n in configs.split(",") if n.strip()]
    for n in names:
        if n not in CONFIGS:
            raise SystemExit("unknown config %s (have: %s)" % (n, ", ".join(CONFIGS)))
        if a.pacing and (n not in PACING_CONFIGS or a.survey):
            raise SystemExit("--pacing runs %s only, without --survey" % ", ".join(PACING_CONFIGS))
    os.makedirs(a.out_root, exist_ok=True)
    groups = [names[i:i + a.per_hold] for i in range(0, len(names), a.per_hold)]
    for g in groups:
        if a.slot and slot(a, "acquire") != 0:
            raise SystemExit("could not take the game slot")
        hb_stop = threading.Event()

        def hb():
            while not hb_stop.wait(300):
                slot(a, "heartbeat")
        threading.Thread(target=hb, daemon=True).start()
        try:
            for n in g:
                run_one(a, n, a.out_root)
                time.sleep(3)
        finally:
            hb_stop.set()
            if a.slot:
                slot(a, "release")
        if a.gap and g is not groups[-1]:
            print("[%s] slot released; waiting %d s so other lanes can take it" % (time.strftime("%H:%M:%S"), a.gap),
                  flush=True)
            time.sleep(a.gap)
    return 0


def cmd_run(a):
    if a.config not in CONFIGS:
        raise SystemExit("unknown config")
    if a.pacing and (a.config not in PACING_CONFIGS or a.survey):
        raise SystemExit("--pacing runs %s only, without --survey" % ", ".join(PACING_CONFIGS))
    os.makedirs(a.out_root, exist_ok=True)
    if a.slot and slot(a, "acquire") != 0:
        raise SystemExit("could not take the game slot")
    try:
        return run_one(a, a.config, a.out_root)
    finally:
        if a.slot:
            slot(a, "release")


# ------------------------------------------------------------------------------------ analysis
def pct(v, q):
    if not v:
        return None
    s = sorted(v)
    return s[min(len(s) - 1, int(q * (len(s) - 1) + 0.5))]


def rolling_hitches(ts, ratio, win=120, warm=30):
    """tools/perf/hitch_report.py's definition: an interval > ratio x the median of the previous 120."""
    import collections
    out, ring = [], collections.deque(maxlen=win)
    for x, y in zip(ts, ts[1:]):
        ms = (y - x) * 1000.0
        if len(ring) >= warm:
            if ms > ratio * statistics.median(ring):
                out.append((y, ms))
        ring.append(ms)
    return out


def load_probe(run_dir):
    pdir = os.path.join(run_dir, "probe")
    meta = json.load(open(os.path.join(pdir, "probe_meta.json")))
    q0, e0 = meta["clock_start"]["qpc"], meta["clock_start"]["epoch_ns"] / 1e9
    f = float(meta["qpf"])
    pres, guest, ids = [], [], {}
    d = open(os.path.join(pdir, "presents.bin"), "rb").read()
    for i in range(0, len(d) - 15, 16):
        q, idn, _tid = struct.unpack_from("<qII", d, i)
        t = e0 + (q - q0) / f
        if idn == 100:
            guest.append(t)
        elif 1 <= idn <= 99:
            pres.append(t)
            ids[idn] = ids.get(idn, 0) + 1
    pres.sort()
    guest.sort()
    q1, e1 = meta["clock_end"]["qpc"], meta["clock_end"]["epoch_ns"] / 1e9
    drift_ms = ((e1 - e0) - (q1 - q0) / f) * 1000.0
    return pres, guest, {"ids": ids, "lost": meta.get("lost"), "clock_drift_ms": round(drift_ms, 2),
                         "records": meta.get("records")}


def load_ours_frames(run_dir, probe_presents):
    """frames.csv (SR_PERF_STATS) guest swaps + host presents on the epoch timeline, the offset refined
    by matching its presents to the probe's (the calibration of the probe on the real target)."""
    perf = os.path.join(run_dir, "perf")
    fc = os.path.join(perf, "frames.csv")
    if not os.path.exists(fc):
        return [], {"error": "no frames.csv"}
    m = {}
    for line in open(os.path.join(perf, "meta.txt"), encoding="utf-8", errors="replace"):
        if "=" in line and not line.startswith("env "):
            k, v = line.strip().split("=", 1)
            m[k] = v
    base = int(m["epoch_ms_t0"]) / 1000.0
    swaps, pres = [], []
    for row in open(fc):
        p = row.strip().split(",")
        if len(p) < 3 or p[0] not in ("S", "P"):
            continue
        (swaps if p[0] == "S" else pres).append(base + int(p[2]) / 1e6)
    import bisect
    cal = {"perf_presents": len(pres), "probe_presents": len(probe_presents)}
    if pres and probe_presents:
        # coarse: the ms-accurate epoch base; fine: the median nearest-neighbour delta
        deltas = []
        for t in pres[:: max(1, len(pres) // 4000)]:
            i = bisect.bisect_left(probe_presents, t)
            best = None
            for j in (i - 1, i):
                if 0 <= j < len(probe_presents):
                    d = probe_presents[j] - t
                    if best is None or abs(d) < abs(best):
                        best = d
            if best is not None and abs(best) < 0.02:
                deltas.append(best)
        if deltas:
            off = statistics.median(deltas)
            swaps = [t + off for t in swaps]
            pres2 = [t + off for t in pres]
            # how many perf presents have a probe stamp within 2 ms (inside the probe's window)
            lo, hi = probe_presents[0], probe_presents[-1]
            inwin = [t for t in pres2 if lo <= t <= hi]
            hit = 0
            res = []
            for t in inwin:
                i = bisect.bisect_left(probe_presents, t)
                d = min(abs(probe_presents[j] - t) for j in (i - 1, i) if 0 <= j < len(probe_presents))
                res.append(d)
                hit += d < 0.002
            pw = [t for t in probe_presents if pres2[0] <= t <= pres2[-1]]
            cal.update({"offset_ms": round(off * 1000, 3), "matched_within_2ms": hit, "perf_in_window": len(inwin),
                        "probe_in_window": len(pw), "residual_p50_us": round(pct(res, 0.5) * 1e6, 1),
                        "residual_p99_us": round(pct(res, 0.99) * 1e6, 1)})
    return swaps, cal


def series_stats(ts, t0, t1):
    s = [t for t in ts if t0 <= t <= t1]
    if len(s) < 3:
        return {"n": len(s)}
    iv = [(b - a) * 1000.0 for a, b in zip(s, s[1:])]
    h15 = rolling_hitches(s, 1.5)
    h2 = rolling_hitches(s, 2.0)
    h4 = rolling_hitches(s, 4.0)
    span = s[-1] - s[0]
    return {"n": len(s), "fps": round((len(s) - 1) / span, 2) if span > 0 else None,
            # unrounded, for graders that compare unrounded (route_repeat, route_log_cost; Astra pin r1 #4)
            "fps_raw": (len(s) - 1) / span if span > 0 else None,
            "p50": round(pct(iv, 0.5), 2), "p95": round(pct(iv, 0.95), 2), "p99": round(pct(iv, 0.99), 2),
            "max": round(max(iv), 1), "std": round(statistics.pstdev(iv), 2),
            "over_33": sum(x > 33.4 for x in iv), "over_50": sum(x > 50.0 for x in iv),
            "over_100": sum(x > 100.0 for x in iv),
            # an absolute stall count comparable across frame-rate caps (the relative hitch counts
            # below are config-relative: at 0.3 ms frames a 0.5 ms interval is a "1.5x hitch")
            "stall50_per_min": round(sum(x > 50.0 for x in iv) * 60.0 / span, 2) if span > 0 else None,
            "hitch15": len(h15), "hitch2": len(h2), "hitch4": len(h4),
            "low1pct_fps": round(1000.0 / pct(iv, 0.99), 1) if pct(iv, 0.99) else None}


def read_csv_rows(path):
    if not os.path.exists(path):
        return []
    rows = []
    with open(path) as f:
        hdr = f.readline().strip().split(",")
        for line in f:
            p = line.strip().split(",")
            if len(p) == len(hdr):
                try:
                    rows.append({k: float(v) for k, v in zip(hdr, p)})
                except ValueError:
                    pass
    return rows


def gpu_rows(run_dir):
    out = []
    p = os.path.join(run_dir, "gpu.csv")
    if not os.path.exists(p):
        return out
    for line in open(p):
        parts = [x.strip() for x in line.split(",")]
        if len(parts) < 5:
            continue
        try:
            t = dt.datetime.strptime(parts[0], "%Y/%m/%d %H:%M:%S.%f").timestamp()
            out.append({"t": t, "util": float(parts[1]), "mem_mb": float(parts[2]), "clk": float(parts[3])})
        except ValueError:
            continue
    return out


def mean_in(rows, key, t0, t1):
    v = [r[key] for r in rows if t0 <= r["t"] <= t1]
    return round(sum(v) / len(v), 2) if v else None


def marks_for(run_dir, meta, probe_guest):
    marks = dict(meta.get("marks", {}))
    evs = [json.loads(l) for l in open(os.path.join(run_dir, "events.jsonl"), encoding="utf-8")]
    if meta["cfg"]["side"] == "xenia":
        # tools/xenia_pad_tape.py logs a token AFTER press (0.12 s, '@' 0.45 s) + release + 0.45 s
        pad = []
        try:
            for line in open(os.path.join(run_dir, "pad.log"), encoding="utf-8", errors="replace"):
                p = line.split()
                if len(p) >= 3:
                    pad.append((float(p[0]), line.strip()))
        except (OSError, ValueError):
            pass
        presses = [t - 0.57 for t, l in pad if " cmd A " in " %s " % l]
        if "main_hud" in marks:
            before = [t for t in presses if t <= marks["main_hud"] + 0.05]
            if before:
                marks["newgame_press"] = before[-1]
        # the tape's own start: the pad tool sees the anchor file only after its last front-end
        # token, so Xenia's route starts later than the guest's request (0.54 s in the pilot)
        seen = [t for t, l in pad if "anchor seen" in l]
        if seen:
            marks["tape_start"] = seen[0]
        if probe_guest and "first_frame" in marks:
            marks["first_frame"] = min(marks["first_frame"], probe_guest[0])
        try:
            for line in open(os.path.join(run_dir, "probe", "markers.jsonl"), encoding="utf-8"):
                m = json.loads(line)
                if m.get("t") == "movie_end" and "main_hud" in marks and m["epoch"] > marks["main_hud"]:
                    marks["movie_end"] = m["epoch"]
                    break
        except (OSError, ValueError):
            pass
    else:
        logs = glob.glob(os.path.join(run_dir, "drive", "attempt*", "run.log")) or             glob.glob(os.path.join(run_dir, "game.log"))
        if logs and "main_hud" in marks:
            with open(logs[-1], encoding="utf-8", errors="replace") as f:
                for line in f:
                    if "[MPDTOR] #" in line and "movie_end" not in marks:
                        t = log_epoch(line)
                        if t and t > marks["main_hud"]:
                            marks["movie_end"] = t
                    # ours only (Xenia cannot hook a function first called at that moment): the FE
                    # flow's first show-screen action after the movie = the HUD up, in control
                    if "[FE_SHOWSCREEN] #" in line and "in_control" not in marks:
                        t = log_epoch(line)
                        if t and t > marks["main_hud"]:
                            marks["in_control"] = t
                    if "movie_end" in marks and "in_control" in marks:
                        break
        # SR_INPUT_REPLAY anchors in the guest's own main_hud request (kernel/fe_flow_watch.cpp)
        if "main_hud" in marks:
            marks["tape_start"] = marks["main_hud"]
        steps = glob.glob(os.path.join(run_dir, "drive", "attempt*", "steps.log"))
        if steps:
            day = dt.datetime.fromtimestamp(marks.get("launch", time.time())).date()
            last = None
            for line in open(steps[-1], encoding="ascii", errors="replace"):
                if "step 'start-new-game': pulse" in line and "/" in line.split("pulse", 1)[1][:6]:
                    m = re.match(r"\[(\d\d:\d\d:\d\d\.\d{3})\]", line)
                    if m:
                        t = dt.datetime.combine(day, dt.datetime.strptime(m.group(1), "%H:%M:%S.%f").time()).timestamp()
                        if marks.get("launch") and t < marks["launch"] - 60:
                            t += 86400
                        last = t
                if "step 'start-new-game'" in line and "DELIVERED" in line and last:
                    marks["newgame_press"] = last
                    break
            if "newgame_press" not in marks and last:
                marks["newgame_press"] = last
    return marks


def analyse_run(run_dir):
    meta = json.load(open(os.path.join(run_dir, "run.json")))
    pres, guest, pinfo = load_probe(run_dir)
    cal = None
    if meta["cfg"]["side"] == "ours":
        guest, cal = load_ours_frames(run_dir, pres)
    marks = marks_for(run_dir, meta, guest)
    proc = read_csv_rows(os.path.join(run_dir, "proc.csv"))
    gpu = gpu_rows(run_dir)
    evs = [json.loads(l) for l in open(os.path.join(run_dir, "events.jsonl"), encoding="utf-8")]
    shots = {e["name"]: e for e in evs if e["ev"] == "shot"}
    res = {"run": os.path.basename(run_dir), "config": meta["config"], "label": meta["cfg"]["label"],
           "side": meta["cfg"]["side"], "client_size": meta.get("client_size"), "probe": pinfo,
           "calibration": cal, "marks": marks, "scenes": {}, "loads": {}, "shots": {},
           "qos": meta.get("qos_opt_out"), "gates": meta.get("gates"), "xenia_flags": meta.get("xenia_flags"),
           "route_name": meta.get("route_name", routes.A1), "scenes_def": meta.get("scenes") or SCENES,
           "route_file_sha256": meta.get("route_file_sha256"), "tape_sha256": meta.get("tape_sha256"),
           "survey": meta.get("survey"), "extra_gates": meta.get("extra_gates"), "log_pin": meta.get("log_pin"),
           "log_pin_off": meta.get("log_pin_off"), "run_dir": os.path.realpath(run_dir)}
    for key, _label, m0, m1 in LOADS:
        if m0 in marks and m1 in marks:
            res["loads"][key] = round(marks[m1] - marks[m0], 2)
    for s in meta.get("scenes") or SCENES:
        if s["from"][0] not in marks or s["to"][0] not in marks:
            continue
        t0 = marks[s["from"][0]] + s["from"][1]
        t1 = marks[s["to"][0]] + s["to"][1]
        if t1 - t0 < 1.0:
            continue
        st = {"t0": t0, "t1": t1, "dur": round(t1 - t0, 2),
              "present": series_stats(pres, t0, t1), "game": series_stats(guest, t0, t1),
              "proc_cores": mean_in(proc, "proc_cores", t0, t1), "other_cores": mean_in(proc, "other_cores", t0, t1),
              "private_mb": mean_in(proc, "private_mb", t0, t1), "gpu_util": mean_in(gpu, "util", t0, t1),
              "shots_inside": [n for n, e in shots.items() if t0 <= e["t"] <= t1]}
        res["scenes"][s["name"]] = st
        if s["name"] in shots:
            res["shots"][s["name"]] = os.path.join(run_dir, "shots", s["name"] + ".png")
    return res


def fmt_fps(st, key="present"):
    x = st.get(key, {}) if st else {}
    if not x or x.get("n", 0) < 3:
        return "-"
    return "%.0f fps, p99 %.1f ms" % (x["fps"], x["p99"])


def side_by_side(paths, labels, out, h=360):
    from PIL import Image, ImageDraw
    ims = []
    for p in paths:
        if p and os.path.exists(p):
            im = Image.open(p).convert("RGB")
            ims.append(im.resize((int(im.width * h / im.height), h), Image.BILINEAR))
        else:
            ims.append(Image.new("RGB", (int(h * 16 / 9), h), (40, 40, 40)))
    W = sum(i.width for i in ims)
    sheet = Image.new("RGB", (W, h + 22), (0, 0, 0))
    x = 0
    d = ImageDraw.Draw(sheet)
    for im, lab in zip(ims, labels):
        sheet.paste(im, (x, 22))
        d.text((x + 6, 4), lab, fill=(255, 255, 255))
        x += im.width
    sheet.save(out, quality=88)


def crop_pair(left, right, out, w=1280, h=720):
    from PIL import Image
    ims = []
    for p in (left, right):
        im = Image.open(p).convert("RGB")
        x0, y0 = max(0, (im.width - w) // 2), max(0, (im.height - h) // 2)
        ims.append(im.crop((x0, y0, x0 + w, y0 + h)))
    sheet = Image.new("RGB", (w * 2 + 8, h), (0, 0, 0))
    sheet.paste(ims[0], (0, 0))
    sheet.paste(ims[1], (w + 8, 0))
    sheet.save(out, compress_level=6)


def route_views(results, ref_dir):
    """The A3 detector: each run's flight / wall / pause shot against the route's frozen reference shot
    (<ref_dir>/<scene>.png). Labels only: {run: {scene: {on_view, distance, dluma}}}."""
    out = {}
    for r in results:
        row = {}
        for scene in routes.POST_SCENES:
            shot, ref = r["shots"].get(scene), os.path.join(ref_dir, scene + ".png")
            if shot and os.path.isfile(shot) and os.path.isfile(ref):
                ok, d, dl = routes.on_view(shot, ref, scene)
                row[scene] = {"on_view": ok, "distance": d, "dluma": dl}
        out[r["run"]] = row
    return out


def route_revision(r):
    """A run's route revision: (route name, route file sha256, tape sha256, scenes). A legacy run (before route
    versions: no route_name in run.json) is a1 with a1's SCENES; its tape sha256 is whatever it recorded."""
    scenes = json.dumps(r.get("scenes_def") or SCENES, sort_keys=True)
    return (r.get("route_name") or routes.A1, r.get("route_file_sha256"), r.get("tape_sha256"),
            hashlib.sha256(scenes.encode("utf-8")).hexdigest()[:12])


def route_revisions(results):
    return {route_revision(r) for r in results}


def cmd_report(a):
    root = a.out_root
    runs = sorted(d for d in glob.glob(os.path.join(root, "*")) if os.path.exists(os.path.join(d, "run.json"))
                  and os.path.exists(os.path.join(d, "probe", "probe_meta.json")))
    results = []
    for d in runs:
        try:
            results.append(analyse_run(d))
        except Exception as e:  # noqa: BLE001
            print("skip %s: %s" % (d, e))
    revs = route_revisions(results)
    if len(revs) > 1:
        raise SystemExit("the runs under %s use different route revisions (route, route file sha256, tape sha256, "
                         "scenes): %s - report one route revision per --out-root" % (root, sorted(revs, key=repr)))
    SCENES = results[0]["scenes_def"] if results else globals()["SCENES"]  # noqa: N806 (this report's route)
    views = route_views(results, a.route_ref) if getattr(a, "route_ref", None) else {}
    for r in results:
        r["route_views"] = views.get(r["run"])
    json.dump(results, open(os.path.join(root, "report.json"), "w"), indent=1)
    order = [r for n in list(CONFIGS) for r in results if r["config"] == n]
    lines = ["# Canary comparison: %s" % os.path.basename(os.path.abspath(root)), "",
             "Generated by `python tools/perf/canary_compare.py report --out-root %s`." % root.replace("\\", "/"), "",
             "Present = the interval between two present submissions of the process (tools/perf/present_probe.js, "
             "both sides); game = the guest's own D3DDeviceSwap (one game frame). fps = (presents - 1) / span; "
             "p50/p95/p99/max = present-interval percentiles in ms; hitches = intervals > 1.5x / 2x / 4x the median "
             "of the previous 120 (tools/perf/hitch_report.py).", ""]
    lines += ["## Runs", "", "| run | config | window (client px) | probe presents (ids) | lost | calibration (ours: SR_PERF_STATS presents vs probe) | Xenia QoS opt-out |",
              "|---|---|---|---|---|---|---|"]
    for r in order:
        cal = r.get("calibration") or {}
        cs = ("%s/%s within 2 ms, residual p50 %s us p99 %s us" % (cal.get("matched_within_2ms"), cal.get("perf_in_window"),
              cal.get("residual_p50_us"), cal.get("residual_p99_us"))) if cal.get("perf_in_window") else "-"
        lines.append("| %s | %s | %s | %s %s | %s | %s | %s |" % (r["run"], r["label"], r.get("client_size"),
                     r["probe"]["records"], r["probe"]["ids"], r["probe"]["lost"], cs, r.get("qos") or "-"))
    lines += ["", "## Load times (s)", "", "| run | " + " | ".join(l[1] for l in LOADS) + " |",
              "|---|" + "---|" * len(LOADS)]
    for r in order:
        lines.append("| %s | %s |" % (r["run"], " | ".join(str(r["loads"].get(l[0], "-")) for l in LOADS)))
    for s in SCENES:
        lines += ["", "## %s" % s["label"], "",
                  "| run | dur s | present fps | p50 | p95 | p99 | max | std | >33.4 | >50 | hitch 1.5x/2x/4x | game fps | game p99 | cores | other cores | GPU % |",
                  "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
        for r in order:
            st = r["scenes"].get(s["name"])
            rv = (r.get("route_views") or {}).get(s["name"])
            tag = " (ROUTE-DIVERGED)" if rv and not rv["on_view"] else ""
            if not st:
                lines.append("| %s%s | - |" % (r["run"], tag))
                continue
            p, g = st["present"], st["game"]
            if p.get("n", 0) < 3:
                lines.append("| %s | %s | (no presents) |" % (r["run"], st["dur"]))
                continue
            lines.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s/%s/%s | %s | %s | %s | %s | %s |" % (
                r["run"] + tag, st["dur"], p["fps"], p["p50"], p["p95"], p["p99"], p["max"], p["std"], p["over_33"],
                p["over_50"], p["hitch15"], p["hitch2"], p["hitch4"], g.get("fps", "-"), g.get("p99", "-"),
                st["proc_cores"], st["other_cores"], st["gpu_util"]))
    if views:
        lines += ["", "## Route views (canary_routes: luma within %s of the reference shot's and the 64x36 thumbnail "
                  "distance <= tau; a diverged scene is labelled in its table above, never dropped or changed)"
                  % routes.LUMA_BAND, "", "| run | " + " | ".join(routes.POST_SCENES) + " |",
                  "|---|" + "---|" * len(routes.POST_SCENES)]
        for r in order:
            rv = r.get("route_views") or {}
            lines.append("| %s | %s |" % (r["run"], " | ".join(
                ("on view" if rv[n]["on_view"] else "DIVERGED") + " (d %s, dl %s)" % (rv[n]["distance"], rv[n]["dluma"])
                if n in rv else "-" for n in routes.POST_SCENES)))
    # side-by-side sheets: every config's shot of each scene
    sheets = os.path.join(root, "sheets")
    os.makedirs(sheets, exist_ok=True)
    lines += ["", "## Screenshots", ""]
    for s in SCENES:
        have = [r for r in order if s["name"] in r["shots"]]
        if not have:
            continue
        out = os.path.join(sheets, s["name"] + ".jpg")
        side_by_side([r["shots"][s["name"]] for r in have], [r["run"] for r in have], out)
        lines.append("- %s: `%s`" % (s["label"], out.replace("\\", "/")))
        # the 4K pair at 1:1: the centre 1280x720 of Xenia 3x and ours 3x (the last run of each
        # that has this shot), so detail is compared without any rescale
        x3 = [r for r in have if r["config"] == "xenia-3x"]
        o3 = [r for r in have if r["config"] == "ours-3x-60"]
        if x3 and o3:
            crop = os.path.join(sheets, s["name"] + "_4k_crop.png")
            crop_pair(x3[-1]["shots"][s["name"]], o3[-1]["shots"][s["name"]], crop)
            lines.append("  - 1:1 centre crop, Xenia 3x | ours 3x: `%s`" % crop.replace("\\", "/"))
    open(os.path.join(root, "report.md"), "w", encoding="utf-8").write("\n".join(lines) + "\n")
    # the README tables (one cell per scene x config: present fps and p99), one per resolution;
    # one column per config: its first run (repeats, <config>_r2..., stay in report.md)
    t = []
    for title, names in README_TABLES:
        cols = []
        for n in names:
            runs = sorted((r for r in order if r["config"] == n), key=lambda r: r["run"])
            if runs:
                cols.append(runs[0])
        if not cols:
            continue
        t += ["**%s**" % title, "", "| Scene | " + " | ".join(SHORT.get(r["config"], r["config"]) for r in cols) + " |",
              "|---|" + "---|" * len(cols)]
        for s in SCENES:
            t.append("| %s | %s |" % (s["label"], " | ".join(fmt_fps(r["scenes"].get(s["name"])) for r in cols)))
        for key, label, _m0, _m1 in LOADS:
            t.append("| %s | %s |" % (label, " | ".join(("%s s" % r["loads"][key]) if key in r["loads"] else "-"
                                                      for r in cols)))
        t.append("")
    open(os.path.join(root, "readme_table.md"), "w", encoding="utf-8").write("\n".join(t) + "\n")
    print("\n".join(lines))
    print("\nREADME table: %s" % os.path.join(root, "readme_table.md"))
    return 0


# ------------------------------------------------------------------------------------ selftest
def cmd_selftest(a):
    """The instrument check (no game slot): the calibration app presents a known number of frames,
    logging its own QPC around every Present; the probe must see every one, inside that window."""
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, "present_selftest.exe")
    if not os.path.exists(exe):
        clang = shutil.which("clang++") or r"C:\Program Files\LLVM\bin\clang++.exe"
        r = subprocess.run([clang, "-O2", "-std=c++17", os.path.join(HERE, "present_selftest.cpp"), "-o", exe,
                            "-ld3d12", "-ldxgi", "-luser32", "-lwinmm"], capture_output=True, text=True)
        if r.returncode:
            print(r.stdout + r.stderr)
            return 2
    ok = True
    for tag, frames, sync, sleep_ms in (("unc", 2000, 0, 0), ("sleep3", 600, 0, 3), ("vsync", 300, 1, 0)):
        csvp = os.path.join(out, "st_%s.csv" % tag)
        pdir = os.path.join(out, "probe_%s" % tag)
        for p in (csvp,):
            if os.path.exists(p):
                os.remove(p)
        if os.path.isdir(pdir):
            for f in os.listdir(pdir):
                os.remove(os.path.join(pdir, f))
        app = subprocess.Popen([exe, csvp, str(frames), str(sync), str(sleep_ms), "3000"], stdout=subprocess.PIPE,
                               text=True, creationflags=NO_WINDOW)
        app.stdout.readline()
        if tag == "sleep3":
            qos_opt_out(app.pid)      # the same opt-out the comparison applies to Xenia
        pr = subprocess.run(a.frida_python + [os.path.join(HERE, "present_probe.py"), "--out", pdir, "--pid",
                                              str(app.pid), "--max-seconds", "120"], capture_output=True, text=True)
        app.wait(60)
        recs = []
        d = open(os.path.join(pdir, "presents.bin"), "rb").read()
        for i in range(0, len(d) - 15, 16):
            recs.append(struct.unpack_from("<qII", d, i))
        rows = [l.strip().split(",") for l in open(csvp).read().splitlines()[1:]]
        before = [int(r[1]) for r in rows]
        after = [int(r[2]) for r in rows]
        stamps = sorted(r[0] for r in recs)
        j = inside = 0
        for t in stamps:
            while j < len(before) and after[j] < t:
                j += 1
            if j < len(before) and before[j] <= t <= after[j]:
                inside += 1
        good = len(stamps) == frames and inside == frames
        ok &= good
        iv = [(b - x) / 1e4 for x, b in zip(stamps, stamps[1:])]
        print("%-7s app %d presents, probe %d, inside the app's Present window %d, ids %s, interval p50 %.3f ms -> %s"
              % (tag, frames, len(stamps), inside, sorted({r[1] for r in recs}), pct(iv, 0.5) or 0,
                 "PASS" if good else "FAIL"), flush=True)
        if pr.returncode:
            print(pr.stdout + pr.stderr)
    print("selftest %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# ------------------------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--out-root", required=True)
        p.add_argument("--root", default=REPO, help="the repo root that holds 'Superman Returns (USA)'")
        p.add_argument("--exe", default=os.path.join(REPO, "build", "SupermanReturns.exe"))
        p.add_argument("--xenia", default=XENIA)
        p.add_argument("--iso", default=ISO)
        p.add_argument("--owner", default="canary")
        p.add_argument("--no-slot", dest="slot", action="store_false")
        p.add_argument("--survey", action="store_true", help="dense screenshots (timeline survey; perturbs timing)")
        p.add_argument("--allow-real-pad", action="store_true")
        p.add_argument("--frida-python", default="py -3.13")
        p.add_argument("--pad-python", default=sys.executable)
        p.add_argument("--route", default=routes.A1,
                       help="a1 (the 10-09 route, the default) or an a2 route file (tools/perf/canary_routes.py)")
        p.add_argument("--extra-gates", default="",
                       help="NAME=VALUE,... added to OUR side's gates - survey rolls only (with --survey)")
        p.add_argument("--no-a2-log-pin", action="store_true",
                       help="an a2 roll of ours WITHOUT the a2 log pin (SR_LOG=dev), measured rolls only: the "
                            "pre-registered overhead check's reference arm (tools/perf/route_log_cost.py) or the "
                            "unpinned K block (route_repeat.py --profile kperf, Fable ruling 42); recorded in "
                            "run.json")
        p.add_argument("--pacing", action="store_true",
                       help="display.registers' timing reference: the pause hold to the anchor + %d s, "
                            "tools/perf/pacing_probe.py instead of present_probe.py (%s only)"
                            % (PACING_HOLD_END, ", ".join(PACING_CONFIGS)))

    b = sub.add_parser("batch")
    common(b)
    b.add_argument("--configs", default=None,
                   help="default: %s; with --pacing: %s (the pilot; the graded set: %s --per-hold 3)"
                        % (",".join(DEFAULT_SET), ",".join(PACING_PILOT), ",".join(PACING_GRADED)))
    b.add_argument("--per-hold", type=int, default=2, help="runs per game-slot hold")
    b.add_argument("--gap", type=int, default=120, help="s between holds (other lanes queue for the slot)")
    r = sub.add_parser("run")
    common(r)
    r.add_argument("--config", required=True)
    rp = sub.add_parser("report")
    rp.add_argument("--out-root", required=True)
    rp.add_argument("--route-ref", default=None, help="a folder with the route's reference flight / wall / pause shots")
    s = sub.add_parser("selftest")
    s.add_argument("--out", default=os.path.join(REPO, "archive", "v1_canary", "selftest"))
    s.add_argument("--frida-python", default="py -3.13")
    t = sub.add_parser("tape")
    t.add_argument("--out", required=True)
    t.add_argument("--route", default=routes.A1)
    a = ap.parse_args()
    if hasattr(a, "frida_python") and isinstance(a.frida_python, str):
        a.frida_python = a.frida_python.split()
    if hasattr(a, "pad_python") and isinstance(a.pad_python, str):
        a.pad_python = a.pad_python.split()
    if hasattr(a, "route"):
        a.route_def = None if a.route == routes.A1 else routes.load_route(a.route)
    if a.cmd == "tape":
        print(a.out, routes.build_route_tape(a.out, getattr(a, "route_def", None)))
        return 0
    if getattr(a, "pacing", False) and getattr(a, "route_def", None) is not None:
        raise SystemExit("--pacing runs the a1 route only")
    a.extra_gates_map = {}
    if getattr(a, "extra_gates", ""):
        if not a.survey:
            raise SystemExit("--extra-gates is for --survey rolls only (a measured roll keeps the harness's gates)")
        a.extra_gates_map = parse_extra_gates(a.extra_gates)
    apply_log_pin(a)
    if a.cmd == "selftest":
        return cmd_selftest(a)
    if a.cmd == "report":
        return cmd_report(a)
    if not os.path.isdir(os.path.join(a.root, "Superman Returns (USA)")):
        raise SystemExit("no 'Superman Returns (USA)' under --root %s" % a.root)
    return cmd_batch(a) if a.cmd == "batch" else cmd_run(a)


if __name__ == "__main__":
    sys.exit(main())
