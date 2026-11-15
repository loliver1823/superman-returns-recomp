"""bg_window.py - iter-464 bgruns: launch a GUI process (Xenia) WITHOUT taking the foreground.

The user works on this PC while automated runs go; no harness may bring a window to the front
(docs/TOOLKIT.md "Automated runs stay in the background"; our own game does it itself -
kernel/host_automation.hpp). The PowerShell twin is tools/bg_window.ps1.

    import bg_window
    proc = bg_window.popen_background([exe, iso])          # STARTUPINFO SW_SHOWNOACTIVATE
    bg_window.guard_background(proc.pid, proc.sr_prev_foreground)   # daemon thread, for the life
                                                            # of the process: windows to the
                                                            # bottom; the foreground handed back
                                                            # whenever the app takes it without
                                                            # a user gesture
"""
import ctypes
import os
import subprocess
import threading
import time
from ctypes import wintypes

def _load_sdl_audio_env():
    # by file path, so importing bg_window never changes the caller's sys.path
    import importlib.util
    spec = importlib.util.spec_from_file_location("_sr_sdl_audio_env", os.path.join(os.path.dirname(os.path.abspath(__file__)), "sdl_audio_env.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


_sdl_audio_env = _load_sdl_audio_env()

SW_SHOWNOACTIVATE = 4
SWP_NOSIZE, SWP_NOMOVE, SWP_NOACTIVATE, SWP_NOOWNERZORDER = 0x1, 0x2, 0x10, 0x200
HWND_BOTTOM = wintypes.HWND(1)
GW_OWNER = 4

_u32 = ctypes.WinDLL("user32", use_last_error=True)
_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
_u32.EnumWindows.argtypes = [_EnumProc, wintypes.LPARAM]
_u32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
_u32.GetWindowThreadProcessId.restype = wintypes.DWORD
_u32.IsWindowVisible.argtypes = [wintypes.HWND]
_u32.IsWindow.argtypes = [wintypes.HWND]
_u32.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
_u32.GetWindow.restype = wintypes.HWND
_u32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                              ctypes.c_int, ctypes.c_int, wintypes.UINT]
_u32.GetForegroundWindow.restype = wintypes.HWND
_u32.SetForegroundWindow.argtypes = [wintypes.HWND]
_u32.AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.BOOL]
_u32.BringWindowToTop.argtypes = [wintypes.HWND]
_u32.SwitchToThisWindow.argtypes = [wintypes.HWND, wintypes.BOOL]
_u32.GetAsyncKeyState.argtypes = [ctypes.c_int]
_u32.GetAsyncKeyState.restype = ctypes.c_short
_k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
_k32.OpenProcess.restype = wintypes.HANDLE
_k32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
_k32.CloseHandle.argtypes = [wintypes.HANDLE]


def foreground_pid():
    h = _u32.GetForegroundWindow()
    if not h:
        return 0
    pid = wintypes.DWORD(0)
    _u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
    return pid.value


def top_windows(pid):
    """Visible, unowned top-level windows of `pid`."""
    found = []

    def cb(h, _l):
        p = wintypes.DWORD(0)
        _u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and _u32.IsWindowVisible(h) and not _u32.GetWindow(h, GW_OWNER):
            found.append(h)
        return True

    _u32.EnumWindows(_EnumProc(cb), 0)
    return found


def to_bottom(pid):
    n = 0
    for h in top_windows(pid):
        if _u32.SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0,
                             SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOOWNERZORDER):
            n += 1
    return n


def _thread_of(h):
    return _u32.GetWindowThreadProcessId(h, None) if h else 0


def give_back(prev):
    """Hand the foreground back to `prev`. Attaching this thread's input to the current foreground
    thread (and prev's) lifts the foreground lock for the call; a window still mid-activation can
    override one attempt, so it retries, ending with SwitchToThisWindow. Synthesises no input."""
    if not prev or not _u32.IsWindow(prev):
        return False
    me = _k32.GetCurrentThreadId()
    for attempt in range(6):
        fg = _u32.GetForegroundWindow()
        if fg == prev:
            return True
        fg_t, prev_t = _thread_of(fg), _thread_of(prev)
        a1 = bool(fg_t and fg_t != me and _u32.AttachThreadInput(me, fg_t, True))
        a2 = bool(prev_t and prev_t not in (me, fg_t) and _u32.AttachThreadInput(me, prev_t, True))
        try:
            _u32.BringWindowToTop(prev)
            _u32.SetForegroundWindow(prev)
            if attempt >= 3 and _u32.GetForegroundWindow() != prev:
                _u32.SwitchToThisWindow(prev, True)
        finally:
            if a2:
                _u32.AttachThreadInput(me, prev_t, False)
            if a1:
                _u32.AttachThreadInput(me, fg_t, False)
        time.sleep(0.015)
    return _u32.GetForegroundWindow() == prev


def gesture_down():
    """A user gesture that can legitimately activate a window: a mouse button, Alt or a Win key."""
    return any(_u32.GetAsyncKeyState(vk) & 0x8000 for vk in (0x01, 0x02, 0x04, 0x12, 0x5B, 0x5C))


def popen_background(args, **kw):
    """subprocess.Popen with STARTUPINFO wShowWindow=SW_SHOWNOACTIVATE. The window that held the
    foreground at launch rides along as proc.sr_prev_foreground (for guard_background)."""
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = SW_SHOWNOACTIVATE
    # lane sdl3-audio: the SDL2 AND the SDL3 spelling of the audio driver / disk-file variables (tools/sdl_audio_env.py)
    kw["env"] = _sdl_audio_env.mirror_env(dict(kw["env"] if kw.get("env") is not None else os.environ))
    prev = _u32.GetForegroundWindow()
    proc = subprocess.Popen(args, startupinfo=si, **kw)
    proc.sr_prev_foreground = prev
    return proc


def guard_background(pid, prev_foreground=None, appear_timeout=30.0, guard_seconds=None, wait=False):
    """Push `pid`'s windows to the bottom of the z-order as soon as they appear, then - for the life
    of the process, or `guard_seconds` - hand the foreground back to the window that last held it
    whenever `pid` takes it WITHOUT a user gesture (mouse button / Alt / Win down in the last
    0.6 s), and push its windows down again. A user who clicks or Alt+Tabs to it keeps it.
    Runs on a daemon thread unless wait=True. Returns the thread (or the stats dict when waiting)."""
    stats = {"moved": 0, "gave_back": 0}

    def alive():
        h = _k32.OpenProcess(0x1000, False, pid)        # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return False
        code = wintypes.DWORD(0)
        ok = _k32.GetExitCodeProcess(h, ctypes.byref(code))
        _k32.CloseHandle(h)
        return bool(ok) and code.value == 259          # STILL_ACTIVE

    def run():
        end = time.time() + appear_timeout
        while time.time() < end:
            stats["moved"] = to_bottom(pid)
            if stats["moved"]:
                break
            time.sleep(0.05)
        last_other = prev_foreground
        if not last_other and foreground_pid() != pid:
            last_other = _u32.GetForegroundWindow()
        last_gesture, user_owned, handled = 0.0, False, False
        end = time.time() + guard_seconds if guard_seconds else float("inf")
        while time.time() < end and alive():
            if gesture_down():
                last_gesture = time.time()
            if foreground_pid() != pid:
                fg = _u32.GetForegroundWindow()
                if fg:
                    last_other = fg
                user_owned, handled = False, False
            elif not user_owned and not handled:
                if time.time() - last_gesture < 0.6:
                    user_owned = True
                else:
                    if give_back(last_other):
                        stats["gave_back"] += 1
                    to_bottom(pid)
                    handled = True
            time.sleep(0.05)

    if wait:
        run()
        return stats
    t = threading.Thread(target=run, name="bg_window_guard", daemon=True)
    t.stats = stats
    t.start()
    return t
