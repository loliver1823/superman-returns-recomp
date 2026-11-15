#!/usr/bin/env python3
"""pacing_probe_selftest.py - the pacing probe's offline instrument check: tools/perf/pacing_probe.py against a FAKE
guest, one per side (this script in --host mode: a Python process laid out as each program is - Xenia's guest memory
at 0x100000000 with VA >= 0xE0000000 at +0x1000 and its JIT indirection table at the guest code VAs, or our build's
guest base, dispatch table and context offsets - presenting at 30 Hz through a hooked "D3DDeviceSwap" over a 60 Hz
vblank thread, and making the era's Apt requests through a hooked "AptLoadByPath"). No game, no Xenia, no GPU; run it
with the Python that has frida (Windows x64).

    python tools/perf/pacing_probe_selftest.py --out <dir> [--seconds 20] [--sides xenia,ours]

Checks, per side: both hooks attach (Xenia: the resolve thunk found, the device chain read through the +0x1000
offset; ours: the context block starts unpublished, the probe reports "g_debugCtxSize is 0" and retries, and once the
host publishes it - our PPCContext's real layout, r3 at offset 0 - inits with r3_off 0 and the swap slot's host
function); every present is recorded (the host's own count, sequence numbers contiguous, nothing lost); each record's
gate count and throttle wait are exactly the host's (BE reads through the CModule); the device is the one laid out;
the request (dxg1) is exactly the host's on every record - the PresentInterval (dev+0x33A4, 1), the pct word
(dev+0x2D3C, changing per present), the front buffer (dev+0x3490, changing per present), the render manager
(*0x8296D8DC, Xenia's past 0xE0000000) and its period (+0x48, 33,333,334 ns: the i64's word order checked), with a
render-mgr marker (period, rate 30); the Apt requests arrive in order and main_hud writes the anchor file;
xenia_pacing_cmp.window() cuts the window from the second pause request with a 60 Hz cadence (+-2%), 1 gate and about
2 vblanks per present, every pair graded against T = max(N 1, R 2) = 2. Across the sides: the cadences match and the
request is the same (xenia_pacing_cmp's cadence_match, requested_match).
Exit 0 = every check passes.
"""
import argparse
import ctypes
import json
import os
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

MEMBASE = 0x100000000
CODE = 0xA0000000
THUNK = CODE + 0xF00
CODE_BASE, ENTRIES = 0x82000000, 0x200000
CELL, CELL_TARGET, STRINGS = 0x82000948, 0x82010000, 0x82020000
DEV = {"xenia": 0xE0100000, "ours": 0x40100000}   # Xenia's past 0xE0000000: the +0x1000 host offset is exercised
SWAP, APT = 0x82112050, 0x826D2690
DEV_VBL, DEV_GATES, DEV_WAIT = 0x3B14, 0x3B20, 0x4E4C
DEV_INTERVAL, DEV_PACING, DEV_FRONT = 0x33A4, 0x2D3C, 0x3490
DEV_LAST, DEV_COUNTDOWN = 0x3B18, 0x3B1C
RENDER_CELL, MGR_RATE, MGR_PERIOD = 0x8296D8DC, 0x3C, 0x48
RMGR_OFF, PERIOD30, PCT = 0x8000, 33333334, 8   # the render manager inside the device's 64 KB (Xenia: past 0xE0000000)
GATE0 = 700


def pacing_word(k):
    return (PCT << 23) | 0xC0400000 | (k & 0xFFFF)


def front_word(k):
    return 0x1F000000 + ((k & 0xFF) << 12)
# r3's offset in the guest context: Xenia's PPCContext r[] at +0x28; ours = XenonRecomp's PPCContext, r3 FIRST
# (the PDB of exe 0360f8ed9ed7: r3 0, r0 8, r1 16, r2 24, r4..r31 from 32, sizeof 2688). dxp0 read a zero r3
# offset as "not filled yet" for the whole run; the old fake used Xenia's layout for both sides and never saw it.
OURS_GPR_ORDER = [3, 0, 1, 2] + list(range(4, 32))
OURS_GPR_OFF = [8 * OURS_GPR_ORDER.index(i) for i in range(32)]
OURS_CTX_SIZE = 2688
R3_OFF = {"xenia": 0x28 + 8 * 3, "ours": OURS_GPR_OFF[3]}
PAUSE = "screens/misc/ScrPauseMenu"
TIMELINE = [(1.0, "screens/hud/main_hud"), (2.0, PAUSE), (4.0, PAUSE)]

# x64: inner (hooked) = push rbp; mov rbp,rsp; push rbx; push rsi; xor eax,eax; nop5; nop5; pop rsi; pop rbx; pop rbp;
# ret - wrapper = push rsi; mov rsi,rcx; sub rsp,0x20; mov rax,inner; call rax; add rsp,0x20; pop rsi; ret (the inner
# function sees the context in rcx (ours) and rsi (Xenia's register scan))
INNER = bytes.fromhex("554889E5535631C00F1F4400000F1F4400005E5B5DC3")


def wrapper(target):
    return bytes.fromhex("564889CE4883EC2048B8") + struct.pack("<Q", target) + bytes.fromhex("FFD04883C4205EC3")


# ------------------------------------------------------------------------------------------------ the fake host
def host(a):
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.VirtualAlloc.restype = ctypes.c_void_p
    k32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]

    def valloc(addr, size, prot=0x04):
        p = k32.VirtualAlloc(ctypes.c_void_p(addr) if addr else None, size, 0x3000, prot)
        if not p or (addr and p != addr):
            raise SystemExit("VirtualAlloc 0x%x failed (%d)" % (addr, ctypes.get_last_error()))
        return p

    side, d = a.host, a.dir
    xenia = side == "xenia"
    dev = DEV[side]

    def host_of(va):
        return MEMBASE + va + (0x1000 if xenia and va >= 0xE0000000 else 0)

    def wbe(va, v):   # one aligned 32-bit store of the big-endian value
        ctypes.c_uint32.from_address(host_of(va)).value = int.from_bytes(struct.pack(">I", v & 0xFFFFFFFF), "little")

    def rbe(va):
        return struct.unpack(">I", ctypes.c_uint32.from_address(host_of(va)).value.to_bytes(4, "little"))[0]

    valloc(MEMBASE + CODE_BASE, 0x30000)
    ctypes.memmove(MEMBASE + CODE_BASE, b"MZ", 2)
    wbe(CELL, CELL_TARGET)
    wbe(CELL_TARGET, dev)
    valloc(MEMBASE + (dev & ~0xFFFF), 0x10000)
    wbe(dev + DEV_VBL, 5000)
    wbe(dev + DEV_GATES, GATE0)
    wbe(dev + DEV_LAST, 5000)
    wbe(dev + DEV_COUNTDOWN, 0)
    # the request: the golden PresentInterval 1, the pct word, a front buffer; the render loop's manager at 30 Hz
    wbe(dev + DEV_INTERVAL, 1)
    wbe(dev + DEV_PACING, pacing_word(0))
    wbe(dev + DEV_FRONT, front_word(0))
    rmgr = dev + RMGR_OFF
    wbe(rmgr + MGR_RATE, struct.unpack(">I", struct.pack(">f", 30.0))[0])
    wbe(rmgr + MGR_PERIOD, 0)
    wbe(rmgr + MGR_PERIOD + 4, PERIOD30)
    valloc(MEMBASE + (RENDER_CELL & ~0xFFFF), 0x10000)
    wbe(RENDER_CELL, rmgr)
    valloc(CODE, 0x10000, 0x40)
    ctypes.memmove(CODE, INNER, len(INNER))
    ctypes.memmove(CODE + 0x40, INNER, len(INNER))
    ctypes.memmove(THUNK, b"\xC3", 1)
    w_swap, w_apt = wrapper(CODE), wrapper(CODE + 0x40)
    ctypes.memmove(CODE + 0x100, w_swap, len(w_swap))
    ctypes.memmove(CODE + 0x140, w_apt, len(w_apt))
    keep = []
    symbols = None
    if xenia:
        # the JIT indirection table at the guest code VAs: every slot the resolve thunk, two compiled
        valloc(CODE_BASE, 0x800000)
        ctypes.memmove(CODE_BASE, struct.pack("<I", THUNK) * (0x800000 // 4), 0x800000)
        ctypes.c_uint32.from_address(SWAP).value = CODE
        ctypes.c_uint32.from_address(APT).value = CODE + 0x40
    else:
        table = valloc(0, ENTRIES * 8)
        ctypes.c_uint64.from_address(table + (SWAP - CODE_BASE) // 4 * 8).value = CODE
        ctypes.c_uint64.from_address(table + (APT - CODE_BASE) // 4 * 8).value = CODE + 0x40
        # the context block starts UNPUBLISHED (zeros, as before RunGuest); it is published when the driver has
        # seen the probe report that state - size first, then the offsets, RunGuest's order
        v = {"g_debugGuestBase": ctypes.c_uint64(MEMBASE), "g_debugDispatchTable": ctypes.c_uint64(table),
             "g_debugDispatchCodeBase": ctypes.c_uint32(CODE_BASE), "g_debugDispatchTableEntries": ctypes.c_uint64(ENTRIES),
             "g_debugCtxGprOff": (ctypes.c_uint32 * 32)(), "g_debugCtxSize": ctypes.c_uint32(0)}
        keep.append(v)
        symbols = {k: hex(ctypes.addressof(x)) for k, x in v.items()}
        json.dump(symbols, open(os.path.join(d, "symbols.json"), "w"))

        def publish():
            while not os.path.exists(os.path.join(d, "publish")):
                time.sleep(0.05)
            v["g_debugCtxSize"].value = OURS_CTX_SIZE
            for i in range(32):
                v["g_debugCtxGprOff"][i] = OURS_GPR_OFF[i]
        threading.Thread(target=publish, daemon=True).start()
    ctx = ctypes.create_string_buffer(0x1000)
    cbase = ctypes.addressof(ctx)
    ctypes.c_uint64.from_address(cbase + 0xA98).value = MEMBASE   # Xenia's context: +0xA98 = the membase
    fn = ctypes.CFUNCTYPE(ctypes.c_int64, ctypes.c_void_p)
    swap, apt = fn(CODE + 0x100), fn(CODE + 0x140)
    off = 0
    strings = {}
    for _t, path in TIMELINE:
        if path not in strings:
            strings[path] = STRINGS + off
            ctypes.memmove(host_of(STRINGS + off), path.encode() + b"\0", len(path) + 1)
            off += 0x100
    json.dump({"pid": os.getpid()}, open(os.path.join(d, "ready.json"), "w"))
    while not os.path.exists(os.path.join(d, "go")):
        time.sleep(0.05)

    stop = threading.Event()
    t0 = time.perf_counter()
    # the gate (82111E38 + the vblank ISR's source 0, as both programs run it): one lock for the vblank thread and the
    # present loop, held across each swap - so the state the probe reads inside the swap is exactly the one recorded
    gate = {"count": 5000, "last": 5000, "cd": 0, "gates": GATE0}
    lock = threading.Lock()
    snaps = []

    def s32(x):
        x &= 0xFFFFFFFF
        return x - (1 << 32) if x >= (1 << 31) else x

    def vblanks():
        nxt = t0
        while not stop.is_set():
            nxt += 1.0 / 60.0
            dt = nxt - time.perf_counter()
            if dt > 0:
                time.sleep(dt)
            with lock:   # the ISR: count + 1; a positive countdown - 1, released at 0 (last = count)
                gate["count"] += 1
                wbe(dev + DEV_VBL, gate["count"])
                c = s32(gate["cd"])
                if c > 0:
                    c -= 1
                    gate["cd"] = c
                    wbe(dev + DEV_COUNTDOWN, c)
                    if c == 0:
                        gate["last"] = gate["count"]
                        wbe(dev + DEV_LAST, gate["last"])
    th = threading.Thread(target=vblanks, daemon=True)
    th.start()
    k, nxt, apts = 0, t0, list(TIMELINE)
    done_apts = []
    while time.perf_counter() - t0 < a.seconds:
        while apts and time.perf_counter() - t0 >= apts[0][0]:
            _t, path = apts.pop(0)
            ctypes.c_uint64.from_address(cbase + R3_OFF[side]).value = strings[path]
            apt(cbase)
            done_apts.append(path)
        nxt += 1.0 / 30.0
        dt = nxt - time.perf_counter()
        if dt > 0:
            time.sleep(dt)
        with lock:
            wbe(dev + DEV_WAIT, 1000 + k % 13)
            wbe(dev + DEV_PACING, pacing_word(k))
            wbe(dev + DEV_FRONT, front_word(k))
            snaps.append([gate["count"], gate["gates"], gate["last"], gate["cd"] & 0xFFFFFFFF])
            swap(cbase)
            # this present's gate callback (N = 1; today's line % is 100, so WAIT or NEXT, never NOW)
            remaining = s32(gate["last"] + 1 - gate["count"])
            gate["gates"] += 1
            wbe(dev + DEV_GATES, gate["gates"])
            gate["cd"] = remaining if remaining > 0 else 1
            wbe(dev + DEV_COUNTDOWN, gate["cd"])
        k += 1
    stop.set()
    th.join(1.0)
    json.dump({"presents": k, "apts": done_apts, "dev": dev, "vblanks_last": rbe(dev + DEV_VBL), "snaps": snaps},
              open(os.path.join(d, "done.json"), "w"))
    while not os.path.exists(os.path.join(d, "quit")):
        time.sleep(0.05)
    return 0


# ------------------------------------------------------------------------------------------------ the driver
def wait_for(cond, timeout, what):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.1)
    raise AssertionError("timed out waiting for " + what)


def markers_of(path):
    out = []
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
    return out


def one_side(side, out, seconds):
    import pacing_probe as pp
    import xenia_pacing_cmp as xc
    d = os.path.join(out, side)
    os.makedirs(d, exist_ok=True)
    for f in ("go", "quit", "stop", "publish", "ready.json", "done.json", "anchor.txt"):
        if os.path.exists(os.path.join(d, f)):
            os.remove(os.path.join(d, f))
    hp = subprocess.Popen([sys.executable, os.path.abspath(__file__), "--host", side, "--dir", d, "--seconds",
                           str(seconds)], stdout=open(os.path.join(d, "host.txt"), "w"), stderr=subprocess.STDOUT)
    probe = None
    try:
        wait_for(lambda: os.path.exists(os.path.join(d, "ready.json")) or hp.poll() is not None, 60, "the host")
        assert hp.poll() is None, open(os.path.join(d, "host.txt")).read()[-800:]
        cmd = [sys.executable, os.path.join(HERE, "pacing_probe.py"), "--out", os.path.join(d, "pacing"), "--pid",
               str(hp.pid), "--side", side, "--stop-file", os.path.join(d, "stop"), "--anchor-file",
               os.path.join(d, "anchor.txt"), "--guest-retry", "60"]
        if side == "ours":
            cmd += ["--symbols", os.path.join(d, "symbols.json")]
        probe = subprocess.Popen(cmd, stdout=open(os.path.join(d, "probe.txt"), "w"), stderr=subprocess.STDOUT)
        mk = os.path.join(d, "pacing", "markers.jsonl")
        if side == "ours":   # the probe must report the unpublished context, keep retrying, then init
            wait_for(lambda: any(m.get("t") == "guest-init-error" for m in markers_of(mk)) or probe.poll() is not None,
                     60, "the probe to report the unpublished context (ours)")
            assert probe.poll() is None, open(os.path.join(d, "probe.txt")).read()[-800:]
            open(os.path.join(d, "publish"), "w").close()

        def attached():
            got = {m.get("guest") for m in markers_of(mk) if m.get("t") == "attached"}
            return {hex(SWAP), hex(APT)} <= got or probe.poll() is not None
        wait_for(attached, 90, "both hooks to attach (%s)" % side)
        assert probe.poll() is None, open(os.path.join(d, "probe.txt")).read()[-800:]
        open(os.path.join(d, "go"), "w").close()
        wait_for(lambda: os.path.exists(os.path.join(d, "done.json")), seconds + 60, "the host's presents")
        time.sleep(0.5)
        open(os.path.join(d, "stop"), "w").close()   # the probe's final drain, while the host is still alive
        probe.wait(60)
    finally:
        open(os.path.join(d, "quit"), "w").close()
        try:
            hp.wait(30)
        except subprocess.TimeoutExpired:
            hp.kill()
        if probe is not None and probe.poll() is None:
            probe.kill()
    done = json.load(open(os.path.join(d, "done.json")))
    recs, markers, meta = pp.load(os.path.join(d, "pacing"))
    n = done["presents"]
    assert meta.get("lost") == 0 and len(recs) == n, ("records", len(recs), "presents", n, meta.get("lost"))
    assert [r["seq"] for r in recs] == list(range(n)), "sequence numbers not contiguous"
    snaps = done["snaps"]
    bad = [k for k, r in enumerate(recs) if [r["vblanks"], r["gates"], r["last"], r["countdown"]] != snaps[k]
           or r["wait"] != 1000 + k % 13 or r["dev"] != DEV[side] or r["torn"] != 0 or r["done"] != k + 1]
    assert not bad, ("records unlike the host's", bad[:5], recs[bad[0]] if bad else None, snaps[bad[0]] if bad else None)
    assert meta.get("stop") == "stop-file" and meta.get("incomplete") == 0, (meta.get("stop"), meta.get("incomplete"))
    # the request, exactly the host's on every record (the render manager seen readable before the first present)
    rmgr = DEV[side] + RMGR_OFF
    bad = [k for k, r in enumerate(recs) if r["interval"] != 1 or r["pacing"] != pacing_word(k)
           or r["front"] != front_word(k) or r["rmgr"] != rmgr or r["rperiod"] != PERIOD30]
    assert not bad, ("requests unlike the host's", bad[:5], recs[bad[0]] if bad else None)
    rm = [m for m in markers if m.get("t") == "render-mgr"]
    assert len(rm) == 1 and rm[0]["mgr"] == hex(rmgr) and rm[0]["period_ns"] == PERIOD30 and rm[0]["rate"] == 30.0, rm
    assert meta.get("format") == pp.FMT and meta.get("rec") == pp.REC, (meta.get("format"), meta.get("rec"))
    att = [m for m in markers if m.get("t") == "attached" and m.get("guest") == hex(SWAP)]
    assert att and att[0].get("dev") == hex(DEV[side]), att
    if side == "xenia":
        assert any(m.get("t") == "thunk" and m.get("host") == hex(THUNK) for m in markers), "no resolve thunk"
    else:
        errs = [m for m in markers if m.get("t") == "guest-init-error"]
        inits = [m for m in markers if m.get("t") == "guest-init"]
        assert errs and "g_debugCtxSize is 0" in errs[0]["err"], ("the unpublished context not reported", errs[:2])
        assert len(inits) == 1 and inits[0]["init"]["r3_off"] == 0 and inits[0]["epoch"] > errs[-1]["epoch"] \
            and inits[0]["init"]["swap_host"] == hex(CODE), ("init: r3 at offset 0, the swap slot's host", inits)
    paths = [m["path"] for m in markers if m.get("t") == "apt"]
    assert paths == [p for _t, p in TIMELINE] == done["apts"], paths
    assert os.path.exists(os.path.join(d, "anchor.txt")), "no anchor file"
    w = xc.window(recs, markers, meta, source=d)
    assert "error" not in w, w.get("error")
    assert abs(w["cadence_hz"] - 60.0) <= 1.2, w["cadence_hz"]
    assert w["gates_per_present"]["mean"] == 1.0 and abs(w["vblanks_per_present"]["mean"] - 2.0) <= 0.1, \
        (w["gates_per_present"], w["vblanks_per_present"])
    assert w["pairs"] >= int((seconds - 4.0 - 3.0 - 1.0) * 30 * 0.9), w["pairs"]
    rq = w["requested"]
    assert rq["fields"] and rq["pairs_graded"] == w["pairs"] and rq["nr"] == [[1, 2]], rq
    assert rq["t_pairs"] == {"2": w["pairs"]} and rq["pct"] == {str(PCT): w["pairs"] + 1}, rq
    # the release (round 2): the fake gate's releases read back - none early, none in its request's own vblank
    rl = w["release"]
    assert rl["fields"] and rl["torn"] == 0 and rl["conflicts"] == 0 and rl["observed"] >= w["pairs"] - 3, rl
    assert rl["early"] == 0 and rl["same_vblank"] == 0 and rl["now"] == 0, rl
    return w


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out")
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--sides", default="xenia,ours")
    ap.add_argument("--host", choices=("xenia", "ours"), help=argparse.SUPPRESS)
    ap.add_argument("--dir", help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.host:
        return host(a)
    if not a.out:
        ap.error("--out is required")
    import xenia_pacing_cmp as xc
    fails, wins = 0, {}
    for side in [s.strip() for s in a.sides.split(",") if s.strip()]:
        try:
            wins[side] = one_side(side, a.out, a.seconds)
            print("PASS %s: %d pairs, cadence %.3f Hz, vblanks/present %.3f, gates/present %.3f" % (
                side, wins[side]["pairs"], wins[side]["cadence_hz"], wins[side]["vblanks_per_present"]["mean"],
                wins[side]["gates_per_present"]["mean"]), flush=True)
        except AssertionError as e:
            print("FAIL %s: %s" % (side, str(e)[:800]), flush=True)
            fails += 1
    if "xenia" in wins and "ours" in wins:
        r = xc.compare(wins["xenia"], wins["ours"])
        ok = all(r["states"][k] == "PASS" for k in ("cadence_match", "ours_one_gate_per_present", "requested_match",
                                                     "xenia_requested_read", "ours_requested_read",
                                                     "xenia_release_not_early", "ours_release_not_early",
                                                     "xenia_release_at_vblank", "ours_release_at_vblank"))
        print("%s across: cadence %s, states %s" % ("PASS" if ok else "FAIL", r["stats"]["cadence_hz"],
                                                   {k: v for k, v in r["states"].items() if v != "PASS"}))
        fails += 0 if ok else 1
    print("%d failed" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
