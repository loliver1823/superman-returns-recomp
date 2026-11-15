#!/usr/bin/env python3
"""xenia_seed_pass.py - step-4 lane A (xenia-oracle): the Xenia half of the chapter matrix.

For every chapter seed (archive/saves/<seed>, docs/SAVES.md) it cold-boots the owner's STOCK Xenia Canary on the
retail ISO, LOADs the seed through the retail front end (PRESS START -> LOAD A GAME? -> LOAD GAME -> slot 0 ->
the METROPOLIS / FORTRESS carousel's METROPOLIS when the load lands there), then presses NOTHING. From the guest's
own request of screens/hud/main_hud (THE ANCHOR, the same AptLoadByPath event our build logs as [APT_SCREEN]) it
takes PrintWindow shots at anchor +10 / +30 / +60 / +120 s (full client, PNG), a survey shot every 5 s (JPEG,
1280 wide) and records the guest's mixed audio frames (AFR1, tools/xenia_audio_capture.js) for --afr-seconds.

    py -3.11 tools/oracle/xenia_seed_pass.py --out <pass dir> [--seeds 02_ch02_warworld_start,post_warworld]
        [--offsets 10,30,60,120] [--afr-seconds 120] [--hold-size 6] [--gap 150] [--no-slot]

The owner's install is only READ (STEP4_RULES "Xenia"): every Xenia write goes to a private storage root in
<pass>/xenia_storage, which holds COPIES of his main config (passed with --config, so Xenia neither re-saves his
file at start nor on exit), his xconfig.settings and his cache_host (the shader cache: warm, then private), plus
empty cache / cache0 / cache1 / scratch / patches folders. recent.toml lands there too. The content root is
private per seed: lane H's profile tree E03000007EB97E5D (render/lightning archive/v1_lightning/xenia_content,
copied, never written) + the seed's save, header and thumbnail (no .devtool marker, no .backup). sha256 + size +
mtime of his exe, config, recent.toml, xconfig.settings, game configs, cache_host and content are recorded before
and after each invocation (install_before_<tag>.json / install_after_<tag>.json / install_diff.txt); the pass refuses to continue if
his config or exe changes between seeds.

Rules kept: the game slot (tools/game_slot.py, owner xenia-oracle) is held per group of --hold-size seeds, released
with a --gap between groups so other lanes' waiters get in; nothing runs if QUIET_WINDOW exists; Xenia is
launched without activation, pushed to the bottom and guarded (tools/bg_window.py); input is a ViGEm virtual pad
(tools/xenia_pad_tape.py, one process for the pass) and the pass REFUSES when a real XInput pad is connected
(Xenia reads every pad); --mute=true; Xenia is ended with TerminateProcess (a clean close saves cvars).
Flags beyond the owner's config: --async_shader_compilation=false (a pending pipeline drops draws; frames are the
point here, not Xenia's pacing), --discord=false, and the profile signed in at boot.

Needs: py -3.11 with vgamepad (ViGEmBus), psutil, PIL; py -3.13 with frida (--frida-python).

Another machine (lane F, the ROG Ally; tools/ally/README.md): --xenia / --iso name an agent-owned COPY of the same
Xenia build, the roots follow SR_MAIN_ROOT (the save library, and lane H's profile tree in the umbrella mirror),
--owner names the lane in that machine's own game slot, and --pad xpad replaces the ViGEm pad: Xenia runs with
--hid=xinput, is created SUSPENDED, tools/oracle/xenia_xpad.py replaces xinput1_4 inside it before its first
instruction (the originals are never called: the machine's own pads are never read, users 1-3 are not connected),
then the pass resumes it. A real pad there is recorded, not refused (it cannot reach the guest). On a new machine /
user, Xenia's first-run disclaimer MessageBox (HKCU\SOFTWARE\Xenia XEFLAGS bit 0 unset) would wait on the desktop:
the pass refuses to launch until the flag is set (tools/ally/README.md). --present-probe adds Xenia's guest frame
rate; afr_rate (always) is guest audio frames per wall second (187.5 = guest time runs at wall speed).
Outputs per seed (<pass>/<seed>/): run.json, events.jsonl, markers.jsonl, xenia.log, xenia.afr (+ afr.stopped),
shots/hud_+NNN.png, survey/s_+NNN.jpg, fe/*.jpg (the front-end presses), content/ (the private root after the run).
<pass>/summary.json + summary.md: one row per seed (title / anchor times, landing, shots, AFR seconds, verdict).
"""
from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import glob
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.join(TOOLS, "perf"))
import canary_compare as cc  # noqa: E402  (print / window helpers, the real-pad check, Events)
import bg_window  # noqa: E402

XENIA = cc.XENIA            # --xenia / --iso override these (main sets the globals)
ISO = cc.ISO
XENIA_DIR = os.path.dirname(XENIA)
# the main checkout: SR_MAIN_ROOT (another machine: the Ally's umbrella mirror); unset = the main checkout git
# knows for this tree (the parent of git's common directory: _worktrees/dev for every lane)
def _git_main():
    here = os.path.dirname(os.path.abspath(__file__))
    try:
        common = subprocess.run(["git", "-C", here, "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                capture_output=True, text=True, check=True).stdout.strip()
        return os.path.dirname(common)
    except (OSError, subprocess.CalledProcessError):
        return os.path.dirname(os.path.dirname(here))


DEV = os.environ.get("SR_MAIN_ROOT") or _git_main()
SAVES = os.path.join(DEV, "archive", "saves")
PROFILE_TREE = os.path.join(os.path.dirname(DEV), "lightning", "archive", "v1_lightning", "xenia_content",
                            "E03000007EB97E5D")
XUID = "E03000007EB97E5D"
CREATE_SUSPENDED = 0x00000004
QUIET = os.path.join(os.path.dirname(DEV), "native", "archive", "v1_native", "QUIET_WINDOW")  # the umbrella's
OWNER = "xenia-oracle"
SEEDS = ["01_ch01_after_mstobj03", "02_ch02_warworld_start", "post_warworld", "03_ch03_metropolis_after_warworld",
         "04_ch04_dark_metal", "05_ch05_after_bad_news", "06_ch06_after_lexs_first", "07_ch07_robot_chapters",
         "08_ch08_before_metallo", "09_ch09_after_metallo", "10_ch10_before_bizarro", "12_ch12_after_bizarro1",
         "14_ch14_after_prison", "15_ch15", "16_ch16", "17_ch17", "18_ch18_before_twister", "19_ch19_post_game"]
SAVE_REL = os.path.join("454107ED", "00000001", "Superman Returns")
HDR_REL = os.path.join("454107ED", "Headers", "00000001", "Superman Returns.header")


def stamp():
    return time.strftime("%H:%M:%S")


def say(*m):
    print("[%s]" % stamp(), *m, flush=True)


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def tree_state(root, rel_root=None):
    """{relative path: [size, mtime, sha256]} of every file under root."""
    out = {}
    if not os.path.exists(root):
        return out
    if os.path.isfile(root):
        st = os.stat(root)
        return {os.path.basename(root): [st.st_size, round(st.st_mtime, 3), sha(root)]}
    for dp, _dn, fn in os.walk(root):
        for n in fn:
            p = os.path.join(dp, n)
            st = os.stat(p)
            out[os.path.relpath(p, rel_root or root).replace("\\", "/")] = [st.st_size, round(st.st_mtime, 3), sha(p)]
    return out


def install_state(full=True):
    """The owner's Xenia install as the pass may only read it."""
    names = ["xenia_canary.exe", "xenia-canary.config.toml", "recent.toml", "xconfig.settings", "portable.txt"]
    st = {}
    for n in names:
        st.update(tree_state(os.path.join(XENIA_DIR, n), XENIA_DIR))
    if full:
        for d in ("config", "cache_host", "content", "cache", "cache0", "cache1", "patches"):
            st.update(tree_state(os.path.join(XENIA_DIR, d), XENIA_DIR))
        st["<iso size>"] = [os.path.getsize(ISO), round(os.path.getmtime(ISO), 3), "not hashed (7.8 GB)"]
    return st


def diff_state(a, b):
    lines = []
    for k in sorted(set(a) | set(b)):
        if a.get(k) != b.get(k):
            lines.append("%s: %s -> %s" % (k, a.get(k), b.get(k)))
    return lines


# ---------------------------------------------------------------------------------------- capture
def grab(pid):
    """PrintWindow(PW_CLIENTONLY | PW_RENDERFULLCONTENT) of Xenia's game window as a PIL image (never
    activates it; canary_compare.print_window's method, returning the image instead of saving it)."""
    from PIL import Image
    from ctypes import wintypes
    hwnd = cc.game_window(pid)
    if not hwnd:
        return None
    w, h = cc.client_size(hwnd)
    if w <= 0 or h <= 0:
        return None
    u32, g32 = cc._u32, cc._g32
    sdc = u32.GetDC(None)
    mdc = g32.CreateCompatibleDC(sdc)
    bmp = g32.CreateCompatibleBitmap(sdc, w, h)
    old = g32.SelectObject(mdc, bmp)
    ok = u32.PrintWindow(hwnd, mdc, 3)

    class BIH(ctypes.Structure):
        _fields_ = [("biSize", wintypes.DWORD), ("biWidth", ctypes.c_long), ("biHeight", ctypes.c_long),
                    ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                    ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                    ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wintypes.DWORD), ("biClrImportant", wintypes.DWORD)]
    bih = BIH(ctypes.sizeof(BIH), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    g32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bih), 0)
    g32.SelectObject(mdc, old)
    g32.DeleteObject(bmp)
    g32.DeleteDC(mdc)
    u32.ReleaseDC(None, sdc)
    if not ok:
        return None
    return Image.frombuffer("RGB", (w, h), buf.raw, "raw", "BGRX", 0, 1)


def save_shot(pid, path, ev, name, jpeg_w=0):
    t0 = time.time()
    try:
        img = grab(pid)
    except Exception as e:  # noqa: BLE001
        img = None
        ev.add("shot-error", name=name, err=str(e))
    rec = {"name": name, "ok": img is not None, "dur": None}
    if img is not None:
        if jpeg_w:
            im = img.resize((jpeg_w, int(img.height * jpeg_w / img.width)))
            im.save(path, quality=88)
        else:
            img.save(path, compress_level=1)
        rec["size"] = [img.width, img.height]
        rec["luma"] = round(sum(img.convert("L").resize((64, 36)).getdata()) / (64 * 36), 1)
    rec["dur"] = round(time.time() - t0, 3)
    ev.add("shot", t=t0, path=os.path.relpath(path, os.path.dirname(os.path.dirname(path))), **rec)
    return rec


# ---------------------------------------------------------------------------------------- markers
class MarkTail(threading.Thread):
    """Tails <run>/markers.jsonl (written by xenia_marks.py): every screens/* AptLoadByPath as (epoch s, path)."""

    def __init__(self, path, ev):
        super().__init__(daemon=True)
        self.path, self.ev = path, ev
        self.apt = []          # [(t, path)]
        self.other = []
        self.afr_frames = 0    # the capture script's 5 s 'sum' records: frames written so far
        self.stop = threading.Event()
        self.lock = threading.Lock()

    def run(self):
        pos, buf = 0, ""
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
                    if m.get("t") == "apt":
                        t = m.get("t_us", 0) / 1e6 or m.get("wall")
                        with self.lock:
                            self.apt.append((t, m.get("path")))
                        self.ev.add("apt", t=t, path=m.get("path"))
                    elif m.get("t") == "sum":
                        self.afr_frames = int(m.get("frames", 0))
                    elif m.get("t") in ("attached", "attach-error", "thunk", "cmd", "frida"):
                        self.other.append(m)
            self.stop.wait(0.1)

    def first(self, path, since=0.0):
        with self.lock:
            for t, p in self.apt:
                if p == path and t >= since:
                    return t
        return None


# ---------------------------------------------------------------------------------------- setup
def neutral_tape(path):
    """A one-entry v2 INPR tape (all neutral) for xenia_pad_tape.py: the pass only uses its --cmd-file presses."""
    with open(path, "wb") as f:
        f.write(struct.pack("<IIII", 0x494E5052, 2, 1, 0))
        f.write(struct.pack("<IHH", 0, 1, 0) + struct.pack("<HBBhhhh", 0, 0, 0, 0, 0, 0, 0))
    return path


def make_storage(pass_dir):
    """The private storage root: copies of the owner's main config, xconfig.settings and cache_host."""
    st = os.path.join(pass_dir, "xenia_storage")
    if not os.path.exists(st):
        os.makedirs(st)
        shutil.copy2(os.path.join(XENIA_DIR, "xenia-canary.config.toml"), os.path.join(st, "xenia-canary.config.toml"))
        shutil.copy2(os.path.join(XENIA_DIR, "xconfig.settings"), os.path.join(st, "xconfig.settings"))
        shutil.copytree(os.path.join(XENIA_DIR, "cache_host"), os.path.join(st, "cache_host"))
        for d in ("cache", "cache0", "cache1", "scratch", "patches", "config"):
            os.makedirs(os.path.join(st, d), exist_ok=True)
        with open(os.path.join(pass_dir, "xenia_storage_seeded.json"), "w") as f:
            json.dump({"from": XENIA_DIR, "when": dt.datetime.now().isoformat(timespec="seconds"),
                       "files": tree_state(st)}, f, indent=1)
    return st


def make_content(seed_dir, run_dir):
    """<run>/content/<XUID>/ = lane H's profile tree (FFFE07D1) + the seed's 454107ED save/header/thumbnail."""
    prof = [p for p in os.listdir(seed_dir) if len(p) == 16 and os.path.isdir(os.path.join(seed_dir, p))]
    if len(prof) != 1:
        raise RuntimeError("%s: expected one profile folder, found %s" % (seed_dir, prof))
    src = os.path.join(seed_dir, prof[0])
    content = os.path.join(run_dir, "content")
    dst = os.path.join(content, XUID)
    shutil.copytree(os.path.join(PROFILE_TREE, "FFFE07D1"), os.path.join(dst, "FFFE07D1"))
    for rel in (os.path.join(SAVE_REL, "Superman Returns"), os.path.join(SAVE_REL, "__thumbnail.png"), HDR_REL):
        s = os.path.join(src, rel)
        if not os.path.isfile(s):
            raise RuntimeError("%s missing" % s)
        os.makedirs(os.path.dirname(os.path.join(dst, rel)), exist_ok=True)
        shutil.copy2(s, os.path.join(dst, rel))
    return content, {"seed_profile": prof[0], "save_sha256": sha(os.path.join(src, SAVE_REL, "Superman Returns")),
                     "header_sha256": sha(os.path.join(src, HDR_REL)),
                     "devtool_marker": open(os.path.join(src, "454107ED", "00000001", "Superman Returns.devtool")).read()
                     if os.path.isfile(os.path.join(src, "454107ED", "00000001", "Superman Returns.devtool")) else None}


def pending_reservation():
    """Another lane's live game-slot reservation (tools/game_slot.py read_next), or None."""
    try:
        import game_slot
        r = game_slot.read_next()
    except Exception:  # noqa: BLE001
        return None
    return r.get("owner") if r and r.get("owner") != OWNER else None


def resume_process(pid):
    """NtResumeProcess: start a process created with CREATE_SUSPENDED (--pad xpad, once the pad is in place)."""
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenProcess.restype = wintypes.HANDLE
    k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    nt = ctypes.WinDLL("ntdll")
    nt.NtResumeProcess.argtypes = [wintypes.HANDLE]
    nt.NtResumeProcess.restype = ctypes.c_long
    h = k32.OpenProcess(0x0800, False, pid)          # PROCESS_SUSPEND_RESUME
    if not h:
        return "open failed (%d)" % ctypes.get_last_error()
    st = nt.NtResumeProcess(h)
    k32.CloseHandle(h)
    return "ok" if st == 0 else "NtResumeProcess 0x%08X" % (st & 0xFFFFFFFF)


def window_texts(pid):
    """Every top-level window of pid with its class, title and its child controls' texts (a stalled Xenia's
    MessageBox reads here: class #32770, the message in a Static child). Read only: WM_GETTEXT via GetWindowTextW."""
    from ctypes import wintypes
    u32 = ctypes.WinDLL("user32", use_last_error=True)
    proc_t = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    u32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    u32.EnumWindows.argtypes = [proc_t, wintypes.LPARAM]
    u32.EnumChildWindows.argtypes = [wintypes.HWND, proc_t, wintypes.LPARAM]
    u32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    u32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    u32.IsWindowVisible.argtypes = [wintypes.HWND]

    def info(h):
        cls = ctypes.create_unicode_buffer(256)
        txt = ctypes.create_unicode_buffer(4096)
        u32.GetClassNameW(h, cls, 256)
        u32.GetWindowTextW(h, txt, 4096)
        return cls.value, txt.value
    out = []

    def top(h, _l):
        p = wintypes.DWORD(0)
        u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid:
            c, t = info(h)
            kids = []

            def child(hc, _l2):
                cc, tc = info(hc)
                if tc:
                    kids.append([cc, tc])
                return True
            u32.EnumChildWindows(h, proc_t(child), 0)
            out.append({"class": c, "title": t, "visible": bool(u32.IsWindowVisible(h)), "children": kids})
        return True
    u32.EnumWindows(proc_t(top), 0)
    return out


def disclaimer_acknowledged():
    """Xenia shows a modal first-run DISCLAIMER MessageBox (emulator.cc) unless HKCU\\SOFTWARE\\Xenia XEFLAGS has bit 0
    (EmulatorFlagDisclaimerAcknowledged); it waits for a click on the desktop (ROG rx1-rx3). Read only."""
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"SOFTWARE\Xenia") as k:
            v, _t = winreg.QueryValueEx(k, "XEFLAGS")
        return bool(int(v) & 1)
    except OSError:
        return False


def xinput_caps_connected():
    """XInput slots with a pad connected, by XInputGetCapabilities (connection only: no input state is read)."""
    try:
        x = ctypes.WinDLL("xinput1_4")
    except OSError:
        return []
    buf = ctypes.create_string_buffer(20)
    return [i for i in range(4) if x.XInputGetCapabilities(i, 0, buf) == 0]


OWNER_GAME_DIRS = ("\\steamapps\\common\\", "\\xboxgames\\", "\\epic games\\", "\\gog galaxy\\games\\")
OWNER_EMUS = ("retroarch", "dolphin", "yuzu", "ryujinx", "rpcs3", "pcsx2", "cemu", "ppsspp", "duckstation", "citra",
              "melonds", "xemu", "vita3k", "shadps4", "suyu", "sudachi", "xenia")


def owner_busy(own_root):
    """--owner-guard (a machine the owner also plays on, e.g. the Ally): his games / emulators running now, i.e.
    any process whose exe lies under a game library folder, or an emulator (Xenia included) outside own_root (the
    agent-owned tree). Plus the foreground window's process, for the record."""
    import psutil
    hits = []
    for p in psutil.process_iter(["name", "exe"]):
        nm, exe = (p.info.get("name") or "").lower(), (p.info.get("exe") or "").lower()
        if exe.startswith(own_root.lower()):
            continue
        if any(d in exe for d in OWNER_GAME_DIRS) or nm.startswith(OWNER_EMUS):
            hits.append("%s pid %d" % (p.info.get("name"), p.pid))
    fg = bg_window.foreground_pid()
    try:
        fg_name = psutil.Process(fg).name() if fg else None
    except Exception:  # noqa: BLE001
        fg_name = "?"
    return hits, fg_name


def metro_summary(path, anchor):
    """metro_watch.py's changes as [{s after the anchor, engaged, flows added / removed}] (M1: event starts / names)."""
    out = []
    if not os.path.exists(path):
        return out
    prev = None
    for line in open(path, encoding="utf-8", errors="replace"):
        try:
            m = json.loads(line)
        except ValueError:
            continue
        c = m.get("change")
        if not c:
            continue
        flows = c.get("flows") or []
        pf = (prev or {}).get("flows") or []
        out.append({"s": round(m["t"] - anchor, 1) if anchor else None, "engaged": c.get("engaged"),
                    "added": [f for f in flows if f not in pf], "removed": [f for f in pf if f not in flows]})
        prev = c
    return out


def start_metro_watch(a, pid, rd, side, module="SupermanReturns.exe"):
    """tools/oracle/metro_watch.py on pid (its own frida session, read only) -> <rd>/metro.jsonl."""
    return subprocess.Popen(a.frida_python + [os.path.join(HERE, "metro_watch.py"), "--pid", str(pid), "--side", side,
                                              "--module", module, "--out", os.path.join(rd, "metro.jsonl"),
                                              "--quit-file", os.path.join(rd, "metro.quit"), "--max-seconds", "7200"],
                            stdout=open(os.path.join(rd, "metro_stdout.txt"), "w"), stderr=subprocess.STDOUT,
                            creationflags=cc.NO_WINDOW)


def stop_metro_watch(rd, mw):
    open(os.path.join(rd, "metro.quit"), "w").close()
    try:
        mw.wait(15)
    except subprocess.TimeoutExpired:
        mw.kill()


def probe_fps(rd, t0, t1, win=10.0):
    """--present-probe: Xenia's guest frame rate (D3DDeviceSwap records, id 100) and host presents per second over
    [t0, t1) epoch s, plus per-window guest rates (tools/perf/present_probe.py output, read by canary_compare)."""
    pres, guest, info = cc.load_probe(rd)
    span = max(t1 - t0, 1e-6)

    def rate(ts, a, b):
        return round(sum(1 for t in ts if a <= t < b) / (b - a), 2)
    wins = []
    t = t0
    while t + win <= t1 + 1e-6:
        wins.append(rate(guest, t, t + win))
        t += win
    gaps = [b - a for a, b in zip(guest, guest[1:]) if t0 <= a < t1]
    return {"window_s": [round(t0, 3), round(t1, 3)], "guest_swaps_per_s": round(
        sum(1 for x in guest if t0 <= x < t1) / span, 2), "host_presents_per_s": round(
        sum(1 for x in pres if t0 <= x < t1) / span, 2), "guest_per_%ds" % int(win): wins,
        "guest_gap_ms_max": round(max(gaps) * 1000, 1) if gaps else None, "probe": info}


def afr_rate(path, win=10.0):
    """Guest audio frames per wall second in an AFR (record = 'AFR1' | seq | t_us | 6144 B): 187.5 = real time."""
    if not os.path.exists(path):
        return None
    ts = []
    with open(path, "rb") as f:
        while True:
            h = f.read(16)
            if len(h) < 16:
                break
            ts.append(struct.unpack_from("<Q", h, 8)[0] / 1e6)
            f.seek(6144, 1)
    if len(ts) < 2:
        return {"frames": len(ts)}
    wins = []
    t = ts[0]
    i = 0
    while t + win <= ts[-1]:
        j = i
        while j < len(ts) and ts[j] < t + win:
            j += 1
        wins.append(round((j - i) / win, 1))
        i, t = j, t + win
    return {"frames": len(ts), "wall_s": round(ts[-1] - ts[0], 2),
            "frames_per_s": round((len(ts) - 1) / (ts[-1] - ts[0]), 2), "per_%ds" % int(win): wins,
            "guest_s_per_wall_s": round((len(ts) - 1) / 187.5 / (ts[-1] - ts[0]), 3)}


def slot(verb, note=""):
    cmd = [sys.executable, os.path.join(TOOLS, "game_slot.py"), verb, "--owner", OWNER]
    if verb == "acquire":
        cmd += ["--poll", "30", "--note", note or "step4 xenia seed pass"]
    say("slot", verb)
    return subprocess.run(cmd, cwd=REPO).returncode


# ---------------------------------------------------------------------------------------- a menu route
def run_route(route, proc, rd, ev, press, tail):
    """--route: steps run on the LoadComplete carousel INSTEAD of A = METROPOLIS (e.g. the retail credits route).
    Steps (comma separated): press=<pad tokens, ';' between tokens>, wait=<screen path substring>[:timeout s]
    (a NEW screens/* AptLoadByPath after the last press), sleep=<s>, shot=<name> (full PNG in route/),
    dense=<name>:<s> (a dense JPEG series in route/<name>/). A missed wait is recorded and the route goes on."""
    out = []
    os.makedirs(os.path.join(rd, "route"), exist_ok=True)
    t_last = time.time()
    for step in [x.strip() for x in route.split(",") if x.strip()]:
        k, _, v = step.partition("=")
        rec = {"step": step, "t": time.time()}
        if k == "press":
            t_last = press(" ".join(v.split(";")))
            time.sleep(0.8)
        elif k == "wait":
            pat, _, to = v.partition(":")
            end = time.time() + float(to or 15)
            hit = None
            while time.time() < end and proc.poll() is None and not hit:
                with tail.lock:
                    hit = next(((t, p) for t, p in tail.apt if t >= t_last - 0.05 and pat in p), None)
                time.sleep(0.1)
            rec.update({"ok": bool(hit), "hit": hit})
        elif k == "seek":
            # seek=<target substr>:<move token>:<back token>:<tries>: A on the current icon; a new screen that is not
            # the target -> a shot of it, <back>, <move>, again (the icon menus' default selection is not static)
            target, mv, back, tries = (v.split(":") + ["RIGHT", "B", "4"])[:4]
            seen = []
            hit = None
            for i in range(int(tries)):
                t_p = press("A")
                end = time.time() + 8.0
                new = None
                while time.time() < end and proc.poll() is None and not new:
                    with tail.lock:
                        new = next(((t, p) for t, p in tail.apt if t >= t_p - 0.05 and "/hud/" not in p), None)
                    time.sleep(0.1)
                if not new:
                    seen.append(None)
                    break
                seen.append(new[1])
                time.sleep(2.5)
                nm = "seek_%s_%d_%s" % (target, i, new[1].split("/")[-1])
                save_shot(proc.pid, os.path.join(rd, "route", nm + ".png"), ev, nm)
                if target in new[1]:
                    hit = new
                    break
                press(back)
                time.sleep(2.5)
                press(mv)
                time.sleep(1.2)
            t_last = time.time()
            rec.update({"ok": bool(hit), "hit": hit, "seen": seen})
        elif k == "gate":
            # gate=<name>[:timeout s]: a shot route/<name>.png, then wait for route/<name>.go (pad tokens, ';' between)
            # written by whoever looks at the shot - for a menu whose default selection decides an irreversible press
            # The answer: tokens with an A = press them and go on; "-" = stop the route here; anything else (e.g. LEFT)
            # = press them, then the gate RE-RUNS (a new shot, a new .go) - lane A's credits route 2026-10-18
            nm, _, to = v.partition(":")
            go = os.path.join(rd, "route", nm + ".go")
            end = time.time() + float(to or 300)
            answers = []
            stop = False
            for k_try in range(10):
                shot_name = nm if k_try == 0 else "%s_%d" % (nm, k_try)
                rec.update(save_shot(proc.pid, os.path.join(rd, "route", shot_name + ".png"), ev, shot_name))
                while time.time() < end and proc.poll() is None and not os.path.exists(go):
                    time.sleep(0.5)
                if not os.path.exists(go):
                    rec.update({"ok": False, "err": "gate timeout"})
                    stop = True
                    break
                toks = open(go).read().strip()
                os.replace(go, go + ".%d" % k_try)          # consumed: the next round waits for a fresh answer
                answers.append(toks)
                if not toks or toks == "-":
                    rec.update({"ok": False, "err": "stopped at the gate"})
                    stop = True
                    break
                t_last = press(" ".join(toks.split(";")))
                time.sleep(0.8)
                if "A" in [t.lstrip("@").upper() for t in toks.split(";")]:
                    break
                time.sleep(2.0)                              # let the menu settle before the next shot
            else:
                rec.update({"ok": False, "err": "gate: no A after 10 rounds"})
                stop = True
            rec["go"] = answers
            if stop:
                ev.add("route", **{kk: vv for kk, vv in rec.items() if kk != "t"})
                out.append(rec)
                break
        elif k == "sleep":
            time.sleep(float(v))
        elif k == "shot":
            rec.update(save_shot(proc.pid, os.path.join(rd, "route", v + ".png"), ev, v))
        elif k == "dense":
            nm, _, secs = v.partition(":")
            dd = os.path.join(rd, "route", nm)
            os.makedirs(dd, exist_ok=True)
            t0 = time.time()
            n = 0
            while time.time() < t0 + float(secs or 5) and proc.poll() is None:
                tt = time.time()
                img = grab(proc.pid)
                if img is not None:
                    img.save(os.path.join(dd, "d_%+08d.jpg" % int((tt - t0) * 1000)), quality=92)
                    n += 1
            rec["frames"] = n
        else:
            rec.update({"ok": False, "err": "unknown step"})
        ev.add("route", **{kk: vv for kk, vv in rec.items() if kk != "t"})
        out.append(rec)
    return out


# ---------------------------------------------------------------------------------------- one seed
def guard_owner(a, name):
    """--owner-guard: refuse (SystemExit: the pass stops, the slot is released) while the owner runs a game."""
    if not a.owner_guard:
        return None
    busy, fg = owner_busy(a.owner_guard)
    say("owner guard: busy %s, foreground %s" % (busy, fg))
    if busy:
        raise SystemExit("the owner has something running (%s) - stopping before %s" % (busy, name))
    return {"busy": busy, "foreground": fg, "t": time.time()}


def run_seed(a, name, pass_dir, storage, pad_cmd):
    guard = guard_owner(a, name)
    seed_dir = os.path.join(a.saves, name)
    rd = os.path.join(pass_dir, name)
    if os.path.exists(rd) and os.listdir(rd):
        k = 2
        while os.path.exists("%s_r%d" % (rd, k)):
            k += 1
        rd = "%s_r%d" % (rd, k)
    for sub in ("shots", "survey", "fe"):
        os.makedirs(os.path.join(rd, sub), exist_ok=True)
    ev = cc.Events(os.path.join(rd, "events.jsonl"))
    meta = {"seed": name, "seed_dir": seed_dir, "tool": "tools/oracle/xenia_seed_pass.py", "offsets": a.offsets,
            "afr_seconds": a.afr_seconds, "xenia": XENIA, "iso": ISO}
    res = {"seed": name, "dir": rd, "verdict": "FAIL", "reason": ""}
    if a.boot_only:
        # a cold boot with NO save: the profile tree only (XC1: intro movies, title, attract; no input)
        content = os.path.join(rd, "content")
        shutil.copytree(os.path.join(PROFILE_TREE, "FFFE07D1"), os.path.join(content, XUID, "FFFE07D1"))
        meta["save"] = None
    else:
        content, meta["save"] = make_content(seed_dir, rd)
    meta["content_before"] = tree_state(content)
    flags = ["--config=" + os.path.join(storage, "xenia-canary.config.toml"), "--storage_root=" + storage,
             "--cache_root=" + os.path.join(storage, "cache_host"), "--content_root=" + content,
             "--log_file=" + os.path.join(rd, "xenia.log"), "--mute=true", "--discord=false", "--fullscreen=false",
             "--logged_profile_slot_0_xuid=" + XUID, "--async_shader_compilation=false",
             "--draw_resolution_scale_x=1", "--draw_resolution_scale_y=1", "--vsync=true", "--framerate_limit=0"]
    if a.pad == "xpad":
        flags.append("--hid=xinput")   # the only driver the in-process pad replaces (any = SDL + keyboard)
    flags += a.extra_flags.split()     # e.g. the Ally's --window_size_x=853 --window_size_y=480 (a 1280x720 client)
    meta["flags"] = flags
    meta["pad"] = a.pad
    meta["machine"] = os.environ.get("COMPUTERNAME", "")
    meta["owner_guard"] = guard

    def press(tokens):
        with open(pad_cmd + ".tmp", "w") as f:
            f.write(tokens)
        os.replace(pad_cmd + ".tmp", pad_cmd)
        ev.add("press", tokens=tokens)
        return time.time()

    proc = marks = probe = mwatch = None
    tail = None
    seed_pad = None
    xpad = None
    try:
        if a.tape and a.pad != "xpad":
            # a tape after the anchor: this seed's own pad (xenia_pad_tape.py anchored on THIS run's anchor.txt), started
            # BEFORE Xenia so Xenia enumerates it; the front-end presses still go through the --cmd-file
            seed_pad = subprocess.Popen([sys.executable, os.path.join(TOOLS, "xenia_pad_tape.py"), "--tape",
                                         os.path.abspath(a.tape), "--anchor-file", os.path.join(rd, "anchor.txt"),
                                         "--cmd-file", pad_cmd, "--log", os.path.join(rd, "pad.log"), "--max-wait", "900"],
                                        stdout=open(os.path.join(rd, "pad_stdout.txt"), "w"), stderr=subprocess.STDOUT,
                                        creationflags=cc.NO_WINDOW)
            meta["tape"] = os.path.abspath(a.tape)
            meta["tape_sha256"] = sha(a.tape)
            time.sleep(3.0)
            if seed_pad.poll() is not None:
                res["reason"] = "the seed's pad exited at once (rc %s)" % seed_pad.returncode
                return res
        t_launch = time.time()
        if a.pad == "xpad":
            # early: created suspended, the in-process pad replaces xinput1_4 before Xenia's first instruction, then
            # resume. late: a normal background launch, the pad attached at once; isolation then holds only if the
            # replacement came before Xenia's own xinput driver loaded the DLL (the hook reports "loaded by xpad"),
            # else Xenia is killed. (ROG rx1 / rx2: the early mode's Xenia stopped in a MessageBoxW in OnInitialize.)
            if os.path.exists(pad_cmd):
                os.remove(pad_cmd)
            early = a.xpad_attach == "early"
            proc = bg_window.popen_background([XENIA] + flags + [ISO], cwd=XENIA_DIR,
                                              creationflags=CREATE_SUSPENDED if early else 0)
            ev.add("created-suspended" if early else "launched", pid=proc.pid, xpad_attach=a.xpad_attach)
            ready = os.path.join(rd, "xpad.ready")
            xcmd = a.frida_python + [os.path.join(HERE, "xenia_xpad.py"), "--pid", str(proc.pid), "--ready-file", ready,
                                     "--cmd-file", pad_cmd, "--log", os.path.join(rd, "xpad.log"),
                                     "--quit-file", os.path.join(rd, "xpad.quit"),
                                     "--dump-file", os.path.join(rd, "xpad_threads")]
            if a.tape:
                xcmd += ["--tape", os.path.abspath(a.tape), "--anchor-file", os.path.join(rd, "anchor.txt")]
                meta["tape"] = os.path.abspath(a.tape)
                meta["tape_sha256"] = sha(a.tape)
            xpad = subprocess.Popen(xcmd, stdout=open(os.path.join(rd, "xpad_stdout.txt"), "w"),
                                    stderr=subprocess.STDOUT, creationflags=cc.NO_WINDOW)
            end = time.time() + 60.0
            while time.time() < end and not os.path.exists(ready) and xpad.poll() is None:
                time.sleep(0.05)
            if not os.path.exists(ready):
                res["reason"] = "the in-process pad was never ready (xenia_xpad.py rc %s); Xenia never ran" % xpad.poll()
                return res
            meta["xpad_ready"] = json.load(open(ready))
            meta["xpad_ready_s"] = round(time.time() - t_launch, 2)
            hk = None
            for line in open(os.path.join(rd, "xpad.log"), encoding="utf-8", errors="replace"):
                if '"what": "hooked"' in line:
                    hk = json.loads(line.split(" js ", 1)[1])
            meta["xpad_hook"] = hk
            if not early:
                # the isolation proof of the late mode: xinput1_4 was NOT in Xenia before the replacement (so neither
                # its driver nor the ImGui gamepad poll could have read a pad), and its log shows no HID setup yet
                try:
                    xl = open(os.path.join(rd, "xenia.log"), encoding="utf-8", errors="replace").read()
                except OSError:
                    xl = ""
                meta["xpad_late_proof"] = {"how": (hk or {}).get("how"), "log_had_hid_setup": "Initializing HID" in xl}
                if not hk or hk.get("how") != "loaded by xpad" or "Initializing HID" in xl:
                    res["reason"] = "late attach came after Xenia loaded xinput1_4 (%s): isolation not proven, killed" \
                        % meta["xpad_late_proof"]
                    return res
                ev.add("xpad-late-ok", ready_s=meta["xpad_ready_s"])
            else:
                meta["resume"] = resume_process(proc.pid)
                if meta["resume"] != "ok":
                    res["reason"] = "resume failed: %s" % meta["resume"]
                    return res
                t_launch = time.time()   # the guest's clock starts here, like a normal launch
                ev.add("resumed", pid=proc.pid, xpad_ready_s=meta["xpad_ready_s"])

            def xpad_watchdog(p=proc, h=xpad):
                # the replacement lives only while the helper's session does: if the helper dies with Xenia still
                # running, Xenia goes at once (its guest must never read the machine's real pad)
                while p.poll() is None:
                    if h.poll() is not None and not os.path.exists(os.path.join(rd, "xpad.quit")):
                        p.kill()
                        ev.add("xpad-died-killed-xenia", rc=h.returncode)
                        return
                    time.sleep(0.1)
            threading.Thread(target=xpad_watchdog, daemon=True).start()
        else:
            proc = bg_window.popen_background([XENIA] + flags + [ISO], cwd=XENIA_DIR)
        guard = bg_window.guard_background(proc.pid, prev_foreground=proc.sr_prev_foreground)
        ev.add("mark:launch", t=t_launch, pid=proc.pid)
        time.sleep(1.0)
        meta["qos_opt_out"] = cc.qos_opt_out(proc.pid)
        time.sleep(3.0)
        marks = subprocess.Popen(a.frida_python + [os.path.join(HERE, "xenia_marks.py"), "--pid", str(proc.pid),
                                                   "--dir", rd, "--max-seconds", "1500"],
                                 stdout=open(os.path.join(rd, "marks_stdout.txt"), "w"), stderr=subprocess.STDOUT,
                                 creationflags=cc.NO_WINDOW)
        tail = MarkTail(os.path.join(rd, "markers.jsonl"), ev)
        tail.start()
        if a.present_probe:
            # tools/perf/present_probe.py --xenia (observation only): every guest D3DDeviceSwap + host present
            import types
            probe = cc.start_probe(types.SimpleNamespace(frida_python=a.frida_python, pacing=False), proc.pid, rd, True)
        if a.metro_watch:
            mwatch = start_metro_watch(a, proc.pid, rd, "xenia")

        def wait_for(path, since, timeout):
            end = time.time() + timeout
            while time.time() < end and proc.poll() is None:
                t = tail.first(path, since)
                if t:
                    return t
                time.sleep(0.2)
            return None

        def sleep_until(t):
            while time.time() < t and proc.poll() is None:
                time.sleep(0.05)

        # 0. Xenia's own setup reached HID (its log), else fail fast: the ROG pilot rx1's UI thread stalled before
        #    "Setup: Initializing Memory..." and the title wait burned 360 s
        if a.setup_timeout > 0:
            xlog = os.path.join(rd, "xenia.log")
            t_end = time.time() + a.setup_timeout
            t_setup = None
            while time.time() < t_end and proc.poll() is None and t_setup is None:
                try:
                    with open(xlog, encoding="utf-8", errors="replace") as f:
                        if "Setup: Initializing HID" in f.read():
                            t_setup = time.time()
                except OSError:
                    pass
                time.sleep(0.5)
            if t_setup is None:
                res["reason"] = "Xenia's setup never reached HID in %d s (Xenia %s)" % (
                    a.setup_timeout, "exited %s" % proc.poll() if proc.poll() is not None else "running: stalled")
                if proc.poll() is None:
                    # a stalled Xenia's own words: its windows (ROG rx2: a MessageBoxW in OnInitialize)
                    try:
                        res["stall_windows"] = window_texts(proc.pid)
                    except Exception as e:  # noqa: BLE001
                        res["stall_windows"] = "error %s" % e
                    ev.add("stall-windows", windows=res["stall_windows"])
                if xpad is not None and proc.poll() is None:
                    # where it stalled: every thread's stack (xpad_threads.json), before Xenia is killed
                    open(os.path.join(rd, "xpad_threads"), "w").close()
                    t_end = time.time() + 15.0
                    while time.time() < t_end and not os.path.exists(os.path.join(rd, "xpad_threads.json")):
                        time.sleep(0.2)
                return res
            res["setup_hid_s"] = round(t_setup - t_launch, 1)
        # 1. the title: nothing is pressed until the guest requests PRESS START
        t_title = wait_for("screens/bootflow/ScrPressStart", 0, a.title_timeout)
        if not t_title:
            res["reason"] = "no ScrPressStart request in %d s (Xenia %s)" % (
                a.title_timeout, "exited %s" % proc.poll() if proc.poll() is not None else "running")
            return res
        res["title_s"] = round(t_title - t_launch, 1)
        sleep_until(t_title + 5.0)
        save_shot(proc.pid, os.path.join(rd, "fe", "01_title.jpg"), ev, "fe_title", 960)
        if a.boot_only:
            # no input at all: the title and its attract loop until --boot-only s after the launch
            n = 0
            while time.time() < t_launch + a.boot_only and proc.poll() is None:
                sleep_until(min(t_launch + a.boot_only, time.time() + 30.0))
                n += 1
                save_shot(proc.pid, os.path.join(rd, "survey", "boot_%03d.jpg" % n), ev, "boot_%03d" % n, 1280)
            res["xenia_alive_at_end"] = proc.poll() is None
            res["boot_seconds"] = round(time.time() - t_launch, 1)
            res["verdict"] = "PASS" if res["xenia_alive_at_end"] else "FAIL"
            if res["verdict"] == "FAIL":
                res["reason"] = "Xenia exited during the boot-only window"
            return res
        # 2. START -> LOAD A GAME? (a ScrGenPop request after the press)
        t_pop = None
        for attempt in range(4):
            tp = press("@START")
            t_pop = wait_for("screens/misc/ScrGenPop", tp, 12.0)
            if t_pop:
                break
            ev.add("retry", step="start", attempt=attempt + 1)
        if not t_pop:
            save_shot(proc.pid, os.path.join(rd, "fe", "02_no_popup.jpg"), ev, "fe_no_popup", 960)
            res["reason"] = "no ScrGenPop (LOAD A GAME?) after 4 START presses"
            return res
        sleep_until(t_pop + 3.0)
        save_shot(proc.pid, os.path.join(rd, "fe", "02_popup.jpg"), ev, "fe_popup", 960)
        press("DOWN")
        time.sleep(1.5)
        save_shot(proc.pid, os.path.join(rd, "fe", "03_load_game.jpg"), ev, "fe_down", 960)
        press("A")
        # 3. LOADING CONTENT, then the slot list (slot 0 focused: every seed uses slot 0 only)
        time.sleep(a.slots_dwell)
        save_shot(proc.pid, os.path.join(rd, "fe", "04_slots.jpg"), ev, "fe_slots", 960)
        t_slot = press("A")
        res["slot_press_s"] = round(t_slot - t_launch, 1)
        # 4. where the load lands: the METROPOLIS / FORTRESS carousel (A = METROPOLIS) or the HUD itself
        t_hud = None
        landing = None
        carousel_presses = 0
        slot_retries = 0
        end = time.time() + a.load_timeout
        last_press = t_slot
        while time.time() < end and proc.poll() is None:
            t_hud = tail.first("screens/hud/main_hud", t_slot)
            if t_hud:
                landing = landing or "hud"
                break
            t_car = tail.first("screens/frontend/ScrMainMenu", t_slot)
            if t_car and a.route:
                landing = "carousel"
                sleep_until(t_car + 3.0)
                save_shot(proc.pid, os.path.join(rd, "fe", "05_carousel.jpg"), ev, "fe_carousel", 960)
                res["route"] = run_route(a.route, proc, rd, ev, press, tail)
                res["verdict"] = "PASS" if all(st.get("ok", True) for st in res["route"]) else "FAIL"
                res["reason"] = "" if res["verdict"] == "PASS" else "a route wait missed: %s" % [
                    st for st in res["route"] if not st.get("ok", True)]
                return res
            if t_car:
                landing = "carousel"
                if carousel_presses < 3 and time.time() >= max(t_car + 3.0, last_press + 15.0 if carousel_presses else 0):
                    if carousel_presses == 0:
                        save_shot(proc.pid, os.path.join(rd, "fe", "05_carousel.jpg"), ev, "fe_carousel", 960)
                    last_press = press("A")
                    carousel_presses += 1
            elif slot_retries < 2 and time.time() > last_press + 40.0:
                save_shot(proc.pid, os.path.join(rd, "fe", "05_no_landing_%d.jpg" % slot_retries), ev, "fe_no_landing", 960)
                last_press = press("A")
                slot_retries += 1
                ev.add("retry", step="slot", attempt=slot_retries)
            time.sleep(0.2)
        res["landing"] = landing
        res["carousel_presses"] = carousel_presses
        res["slot_retries"] = slot_retries
        if not t_hud:
            save_shot(proc.pid, os.path.join(rd, "fe", "06_no_hud.jpg"), ev, "fe_no_hud", 960)
            res["reason"] = "no main_hud request after the slot press (landing %s)" % landing
            return res
        ev.add("mark:anchor", t=t_hud, src="AptLoadByPath screens/hud/main_hud")
        res["anchor_s"] = round(t_hud - t_launch, 1)
        res["anchor_epoch"] = t_hud
        # 5. from the anchor: AFR, the matched shots, the survey; NO input
        open(os.path.join(rd, "afr.start"), "w").close()
        ev.add("afr-start-requested")
        # the timeline after the anchor: the matched shots, the 5 s survey and the dense windows (--dense
        # "from:len,..." or --dense-from/--dense-seconds), run in start order; a shot inside a dense window is
        # still taken (as a full PNG) when the window ends
        windows = list(a.dense_windows)
        plan = [("hud_%+04d" % o, t_hud + o, "shot") for o in a.offsets]
        plan += [("s_%+04d" % s, t_hud + s, "survey") for s in range(-5, int(a.afr_seconds) + 6, a.survey_every)]
        plan += [("dense_%+05.1f" % f, t_hud + f, "dense") for f, _l in windows]
        plan.sort(key=lambda x: x[1])
        shots = {}
        n_dense = 0
        for nm, t, kind in plan:
            sleep_until(t)
            if proc.poll() is not None:
                break
            if kind == "survey":
                save_shot(proc.pid, os.path.join(rd, "survey", nm + ".jpg"), ev, nm, 1280)
            elif kind == "shot":
                shots[nm] = save_shot(proc.pid, os.path.join(rd, "shots", nm + ".png"), ev, nm)
            else:
                # a dense PrintWindow series (as fast as it goes, ~10-30/s at 1280x720): transients, motion, effects
                f = float(nm.split("_", 1)[1])
                length = dict(windows)[f]
                dd = os.path.join(rd, "dense")
                os.makedirs(dd, exist_ok=True)
                k = 0
                while time.time() < t_hud + f + length and proc.poll() is None:
                    tt = time.time()
                    img = grab(proc.pid)
                    if img is not None:
                        img.save(os.path.join(dd, "d_%+08d.jpg" % int((tt - t_hud) * 1000)), quality=92)
                        k += 1
                n_dense += k
                ev.add("dense", start=f, seconds=length, frames=k)
        res["dense_frames"] = n_dense
        # Xenia's guest submits fewer than 187.5 frames/s in places (the pilot: 88-187/s; the capture hook's JS lock
        # and synchronous shader compiles slow its audio worker), so the AFR runs until it holds --afr-seconds of
        # FRAMES (+2 s), capped at 1.5x the wall time
        target = int((a.afr_seconds + 2.0) * 187.5)
        sleep_until(t_hud + a.afr_seconds + 3.0)
        while (tail.afr_frames < target and time.time() < t_hud + a.afr_seconds * 1.5 and proc.poll() is None):
            time.sleep(0.25)
        open(os.path.join(rd, "afr.stop"), "w").close()
        ev.add("afr-stop-requested")
        end = time.time() + 20
        while time.time() < end and not os.path.exists(os.path.join(rd, "afr.stopped")):
            time.sleep(0.2)
        res["shots"] = {k: v.get("ok") for k, v in shots.items()}
        afr = os.path.join(rd, "xenia.afr")
        res["afr_seconds"] = round(os.path.getsize(afr) / 6160 / 187.5, 1) if os.path.exists(afr) else 0.0
        try:
            res["afr_stopped"] = json.load(open(os.path.join(rd, "afr.stopped")))
        except (OSError, ValueError):
            res["afr_stopped"] = None
        res["xenia_alive_at_end"] = proc.poll() is None
        ok_shots = all(res["shots"].get("hud_%+04d" % o) for o in a.offsets)
        if ok_shots and res["afr_seconds"] >= a.afr_seconds - 1 and res["xenia_alive_at_end"]:
            res["verdict"] = "PASS"
        else:
            res["reason"] = "shots %s, AFR %.1f s, Xenia alive %s" % (res["shots"], res["afr_seconds"],
                                                                    res["xenia_alive_at_end"])
        return res
    finally:
        if marks is not None:
            open(os.path.join(rd, "marks.quit"), "w").close()
            try:
                marks.wait(20)
            except subprocess.TimeoutExpired:
                marks.kill()
        if tail is not None:
            tail.stop.set()
        if mwatch is not None:
            stop_metro_watch(rd, mwatch)
            res["metro_events"] = metro_summary(os.path.join(rd, "metro.jsonl"), res.get("anchor_epoch"))
        if probe is not None:
            cc.stop_probe(rd, probe)
            if res.get("anchor_epoch"):
                try:
                    res["fps"] = probe_fps(rd, res["anchor_epoch"], res["anchor_epoch"] + a.afr_seconds)
                    res["fps_boot_to_anchor"] = probe_fps(rd, t_launch, res["anchor_epoch"])
                except Exception as e:  # noqa: BLE001
                    res["fps"] = {"error": str(e)}
        if res.get("anchor_epoch"):
            try:
                res["afr_rate"] = afr_rate(os.path.join(rd, "xenia.afr"))
            except Exception as e:  # noqa: BLE001
                res["afr_rate"] = {"error": str(e)}
        if xpad is not None:
            # its final call counts while Xenia still runs; the helper stays attached (a detach would revert the
            # replacement) until Xenia is killed below
            open(os.path.join(rd, "xpad.quit"), "w").close()
            end = time.time() + 10.0
            while time.time() < end and xpad.poll() is None and not os.path.exists(os.path.join(rd, "xpad.quit.done")):
                time.sleep(0.05)
        if proc is not None:
            try:
                proc.kill()        # TerminateProcess: a clean close would save cvars (lane K)
                proc.wait(30)
            except Exception:  # noqa: BLE001
                pass
        if xpad is not None:
            try:
                xpad.wait(15)         # it exits once Xenia is gone
            except subprocess.TimeoutExpired:
                pass
        for helper in (seed_pad, xpad):
            if helper is not None:
                try:
                    helper.kill()     # by its own handle (PID); never by name
                except Exception:  # noqa: BLE001
                    pass
        time.sleep(2.0)
        meta["content_after"] = tree_state(content)
        meta["content_diff"] = diff_state(meta["content_before"], meta["content_after"])
        meta["result"] = res
        meta["marks"] = ev.marks
        with open(os.path.join(rd, "run.json"), "w") as f:
            json.dump(meta, f, indent=1)
        ev.close()


# ---------------------------------------------------------------------------------------- the pass
def main():
    global XENIA, ISO, XENIA_DIR, OWNER
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seeds", default=",".join(SEEDS))
    ap.add_argument("--saves", default=SAVES)
    ap.add_argument("--offsets", default="10,30,60,120")
    ap.add_argument("--afr-seconds", type=float, default=120.0)
    ap.add_argument("--survey-every", type=int, default=5)
    ap.add_argument("--slots-dwell", type=float, default=12.0)
    ap.add_argument("--title-timeout", type=float, default=360.0)
    ap.add_argument("--load-timeout", type=float, default=240.0)
    ap.add_argument("--hold-size", type=int, default=6)
    ap.add_argument("--gap", type=float, default=150.0)
    ap.add_argument("--no-slot", action="store_true")
    ap.add_argument("--frida-python", default=os.environ.get("SR_PYTHON") or "py -3.13",
                    help="the Python with frida (default: SR_PYTHON when set, else py -3.13)")
    ap.add_argument("--xenia", default=XENIA, help="xenia_canary.exe (default: the owner's install on the rig)")
    ap.add_argument("--iso", default=ISO)
    ap.add_argument("--owner", default=OWNER, help="the game-slot owner name (the lane)")
    ap.add_argument("--xpad-attach", choices=("early", "late"), default="early",
                    help="--pad xpad: early = created suspended, the pad attached before Xenia runs (the ROG pilot rx5 "
                         "PASS); late = a normal launch, the pad attached at once, Xenia killed unless the replacement "
                         "loaded xinput1_4 itself (on the Ally Xenia reaches HID setup in 2-4 s: late lost, rx4)")
    ap.add_argument("--setup-timeout", type=float, default=120.0,
                    help="fail a seed fast when Xenia's log shows no 'Setup: Initializing HID' in this many s (0 = off)")
    ap.add_argument("--owner-guard", default="", metavar="AGENT_ROOT",
                    help="before each seed refuse if the owner runs a game / emulator outside AGENT_ROOT (the Ally)")
    ap.add_argument("--boot-only", type=float, default=0.0, metavar="SECONDS",
                    help="a cold boot with no save and NO input for this many s after the launch (seed name free, "
                         "e.g. 'boot'): title, attract; no load")
    ap.add_argument("--extra-flags", default="", help="more Xenia flags, space separated (never the owner's config)")
    ap.add_argument("--metro-watch", action="store_true",
                    help="also run tools/oracle/metro_watch.py: metro (mini) event starts / names per seed (M1)")
    ap.add_argument("--present-probe", action="store_true",
                    help="also run tools/perf/present_probe.py --xenia: Xenia's guest frame rate per seed (res fps)")
    ap.add_argument("--pad", choices=("vigem", "xpad"), default="vigem",
                    help="vigem = tools/xenia_pad_tape.py (the rig); xpad = the in-process pad (tools/oracle/xenia_xpad.py)")
    ap.add_argument("--route", default="", help="steps on the carousel instead of METROPOLIS (run_route)")
    ap.add_argument("--tape", default="", help="a v2 .inpr tape played from each seed's anchor (a per-seed pad)")
    ap.add_argument("--dense-seconds", type=float, default=0.0, help="dense PrintWindow series length after the anchor")
    ap.add_argument("--dense-from", type=float, default=-1.0, help="dense series start, s after the anchor")
    ap.add_argument("--dense", default="", help="dense windows 'from:len,from:len' (s after the anchor)")
    a = ap.parse_args()
    XENIA, ISO, OWNER = os.path.abspath(a.xenia), os.path.abspath(a.iso), a.owner
    XENIA_DIR = os.path.dirname(XENIA)
    for p in (XENIA, ISO):
        if not os.path.isfile(p):
            raise SystemExit("missing %s" % p)
    a.offsets = [int(x) for x in a.offsets.split(",")]
    a.frida_python = a.frida_python.split() if " " in a.frida_python.strip() and not os.path.isfile(a.frida_python) \
        else [a.frida_python]
    a.dense_windows = [(float(w.split(":")[0]), float(w.split(":")[1])) for w in a.dense.split(",") if w.strip()]
    if a.dense_seconds > 0:
        a.dense_windows.append((a.dense_from, a.dense_seconds))
    seeds = [s.strip() for s in a.seeds.split(",") if s.strip()]
    for s in seeds:
        if not a.boot_only and not os.path.isdir(os.path.join(a.saves, s)):
            raise SystemExit("no seed %s under %s" % (s, a.saves))
    pass_dir = os.path.abspath(a.out)
    os.makedirs(pass_dir, exist_ok=True)
    if not disclaimer_acknowledged():
        raise SystemExit("REFUSED: Xenia's first-run disclaimer is not acknowledged for this user (HKCU\\SOFTWARE\\Xenia "
                         "XEFLAGS bit 0): Xenia would open a modal MessageBox on the desktop and wait. On an agent "
                         "machine set it once: reg add HKCU\\SOFTWARE\\Xenia /v XEFLAGS /t REG_QWORD /d 1 /f")
    if a.pad == "xpad":
        real = xinput_caps_connected()
        with open(os.path.join(pass_dir, "pads_%s.json" % time.strftime("%m%d_%H%M%S")), "w") as f:
            json.dump({"real_xinput_slots_connected": real, "pad": "xpad",
                       "note": "masked: Xenia runs --hid=xinput with xinput1_4 replaced before its first instruction"}, f)
        say("real XInput pad(s) at slots %s: masked by the in-process pad (never read)" % real)
    else:
        real = cc.xinput_connected()
        if real:
            raise SystemExit("REFUSED: real XInput pad(s) connected at slots %s - Xenia would read them" % real)
    lane_h_before = tree_state(PROFILE_TREE)
    say("hashing the owner's install (read only)")
    before = install_state(full=True)
    tag = time.strftime("%m%d_%H%M%S")   # one before / after pair per invocation of the pass
    with open(os.path.join(pass_dir, "install_before_%s.json" % tag), "w") as f:
        json.dump(before, f, indent=1)
    storage = make_storage(pass_dir)
    pad_cmd = os.path.join(pass_dir, "pad_cmd.txt")
    pad_quit = os.path.join(pass_dir, "pad.quit")
    for stale in (pad_quit, pad_cmd):   # a previous pass's quit file would end the new pad at once (no presses)
        if os.path.exists(stale):
            os.remove(stale)
    tape = neutral_tape(os.path.join(pass_dir, "neutral.inpr"))
    pad = None if (a.tape or a.pad == "xpad") else subprocess.Popen([sys.executable, os.path.join(TOOLS, "xenia_pad_tape.py"), "--tape", tape,
                            "--anchor-file", pad_quit, "--cmd-file", pad_cmd, "--log", os.path.join(pass_dir, "pad.log"),
                            "--max-wait", "40000"], stdout=open(os.path.join(pass_dir, "pad_stdout.txt"), "a"),
                           stderr=subprocess.STDOUT, creationflags=cc.NO_WINDOW)
    time.sleep(3.0)
    if pad is not None and pad.poll() is not None:
        raise SystemExit("the virtual pad (xenia_pad_tape.py) exited at once (rc %s): see pad_stdout.txt" % pad.returncode)
    results = []
    summary_path = os.path.join(pass_dir, "summary.json")
    if os.path.exists(summary_path):
        results = json.load(open(summary_path)).get("results", [])
    quick0 = install_state(full=False)
    groups = [seeds[i:i + a.hold_size] for i in range(0, len(seeds), a.hold_size)]
    try:
        for gi, g in enumerate(groups):
            while os.path.exists(QUIET):
                say("QUIET_WINDOW exists - waiting")
                time.sleep(60)
            if not a.no_slot and slot("acquire", "step4 xenia seed pass group %d/%d" % (gi + 1, len(groups))) != 0:
                raise SystemExit("could not take the game slot")
            hb_stop = threading.Event()

            def hb():
                while not hb_stop.wait(300):
                    subprocess.run([sys.executable, os.path.join(TOOLS, "game_slot.py"), "heartbeat", "--owner", OWNER],
                                   cwd=REPO, capture_output=True)
            threading.Thread(target=hb, daemon=True).start()
            try:
                for gk, name in enumerate(g):
                    if os.path.exists(QUIET):
                        say("QUIET_WINDOW appeared - stopping the group")
                        break
                    if gk and pending_reservation():
                        say("reservation pending for %s - ending the hold early (the rest goes to the next group)"
                            % pending_reservation())
                        groups.insert(gi + 1, g[gk:])
                        break
                    if pad is not None and pad.poll() is not None:
                        raise SystemExit("the virtual pad exited (rc %s) - stopping before %s" % (pad.returncode, name))
                    say("seed", name)
                    r = run_seed(a, name, pass_dir, storage, pad_cmd)
                    q = install_state(full=False)
                    r["install_quick_diff"] = diff_state(quick0, q)
                    results = [x for x in results if x.get("seed") != name] + [r]
                    with open(summary_path, "w") as f:
                        json.dump({"results": results}, f, indent=1)
                    say("seed", name, r["verdict"], r.get("reason", ""), "anchor %s s" % r.get("anchor_s"),
                        "landing", r.get("landing"), "AFR %s s" % r.get("afr_seconds"))
                    if r["install_quick_diff"]:
                        raise SystemExit("the owner's install CHANGED: %s" % r["install_quick_diff"])
                    time.sleep(3.0)
            finally:
                hb_stop.set()
                if not a.no_slot:
                    slot("release")
            if gi < len(groups) - 1 and a.gap:
                say("slot released; %d s gap for other lanes" % a.gap)
                time.sleep(a.gap)
    finally:
        open(pad_quit, "w").close()
        time.sleep(1.0)
        try:
            if pad is not None:
                pad.kill()
        except Exception:  # noqa: BLE001
            pass
        after = install_state(full=True)
        with open(os.path.join(pass_dir, "install_after_%s.json" % tag), "w") as f:
            json.dump(after, f, indent=1)
        d = diff_state(before, after)
        lane_h_after = tree_state(PROFILE_TREE)
        with open(os.path.join(pass_dir, "install_diff.txt"), "a") as f:
            f.write("[%s] owner's install, before vs after this invocation: %s\n" % (tag, "IDENTICAL" if not d else "CHANGED"))
            f.write("\n".join(d) + ("\n" if d else ""))
            f.write("lane H's profile tree: %s\n" % ("IDENTICAL" if lane_h_before == lane_h_after else "CHANGED"))
        say("install", "IDENTICAL" if not d else "CHANGED: %s" % d[:5])
        rows = ["| seed | verdict | title s | anchor s | landing | shots | AFR s | reason |", "|---|---|---|---|---|---|---|---|"]
        for r in results:
            rows.append("| %s | %s | %s | %s | %s | %s | %s | %s |" % (
                r["seed"], r["verdict"], r.get("title_s"), r.get("anchor_s"), r.get("landing"),
                sum(1 for v in (r.get("shots") or {}).values() if v), r.get("afr_seconds"), r.get("reason", "")))
        with open(os.path.join(pass_dir, "summary.md"), "w") as f:
            f.write("\n".join(rows) + "\n")
    return 0 if results and all(r["verdict"] == "PASS" for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
