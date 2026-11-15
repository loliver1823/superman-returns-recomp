#!/usr/bin/env python3
"""res_window.py - resize a running game window the way a user's drag does, without taking the
foreground (iter-466 res4k: the live resize / refit check of docs/RESOLUTION.md).

    python tools/res_window.py --pid <pid> --size 1600x900 [--client]   # outer size, or the client area
    python tools/res_window.py --pid <pid> --steps 1920x1080,1366x768,2560x1080 --dwell 6
    python tools/res_window.py --pid <pid> --info                        # print window + client rects

SetWindowPos(HWND_BOTTOM, SWP_NOACTIVATE | SWP_NOOWNERZORDER): the window gets WM_SIZE (SDL's
resize event -> the swapchain resize -> the present refits the frame) and stays at the bottom of
the z-order, never activated (TOOLKIT 1.8a background rule). Only the PID you started.
"""
import argparse
import ctypes
import sys
import time
from ctypes import wintypes

u32 = ctypes.WinDLL("user32", use_last_error=True)
# Physical pixels, like the game (per-monitor DPI aware): a DPI-unaware caller reads and sets
# LOGICAL sizes - on the 300% 4K desktop a 1600x900 client read as 533x300 and a 2560x1440 request
# became 7680x4320, clamped to the screen (3844x2097) - 2026-10-06.
try:
    u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # PER_MONITOR_AWARE_V2
except (AttributeError, OSError):
    pass
HWND_BOTTOM = wintypes.HWND(1)
SWP_NOMOVE, SWP_NOACTIVATE, SWP_NOOWNERZORDER = 0x0002, 0x0010, 0x0200
GWL_STYLE, GWL_EXSTYLE = -16, -20
u32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                             ctypes.c_int, ctypes.c_uint]
u32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
u32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
u32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
u32.GetWindowLongW.argtypes = [wintypes.HWND, ctypes.c_int]
u32.AdjustWindowRectEx.argtypes = [ctypes.POINTER(wintypes.RECT), wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


def class_of(h):
    buf = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(h, buf, 256)
    return buf.value


def windows_of(pid):
    """The process's visible top-level windows, the game's SDL window first (class SDL_app), then
    by client area, largest first: the process also owns small visible helper windows, and the
    first one EnumWindows returns is not the swapchain's window."""
    out = []

    def cb(h, _l):
        p = wintypes.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u32.IsWindowVisible(h):
            out.append(h)
        return True

    u32.EnumWindows(WNDENUMPROC(cb), 0)

    def key(h):
        _w, (cw, ch) = rects(h)
        return (class_of(h) != "SDL_app", -(cw * ch))

    return sorted(out, key=key)


def rects(h):
    w, c = wintypes.RECT(), wintypes.RECT()
    u32.GetWindowRect(h, ctypes.byref(w))
    u32.GetClientRect(h, ctypes.byref(c))
    return (w.left, w.top, w.right - w.left, w.bottom - w.top), (c.right, c.bottom)


def resize(h, cw, ch, client):
    if client:  # the outer size whose client area is cw x ch
        r = wintypes.RECT(0, 0, cw, ch)
        u32.AdjustWindowRectEx(ctypes.byref(r), u32.GetWindowLongW(h, GWL_STYLE), False,
                               u32.GetWindowLongW(h, GWL_EXSTYLE))
        cw, ch = r.right - r.left, r.bottom - r.top
    ok = u32.SetWindowPos(h, HWND_BOTTOM, 0, 0, cw, ch, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOOWNERZORDER)
    return bool(ok)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--size")
    ap.add_argument("--steps", help="comma list of WxH, one every --dwell seconds")
    ap.add_argument("--dwell", type=float, default=6.0)
    ap.add_argument("--client", action="store_true", help="sizes are the client area (the swapchain)")
    ap.add_argument("--info", action="store_true")
    a = ap.parse_args()
    hs = windows_of(a.pid)
    if not hs:
        print("res_window: no visible window for pid %d" % a.pid)
        return 2
    h = hs[0]
    sizes = [a.size] if a.size else (a.steps.split(",") if a.steps else [])
    for i, s in enumerate(sizes):
        w, hh = (int(v) for v in s.lower().split("x"))
        ok = resize(h, w, hh, a.client)
        time.sleep(0.5)
        print("%s resize %s -> ok=%d window=%s client=%s" % (time.strftime("%H:%M:%S"), s, ok, *rects(h)),
              flush=True)
        if i + 1 < len(sizes):
            time.sleep(a.dwell)
    if a.info or not sizes:
        for x in hs:
            print("%s hwnd=%#x class=%s window=%s client=%s" % ("*" if x == h else " ", x, class_of(x), *rects(x)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
