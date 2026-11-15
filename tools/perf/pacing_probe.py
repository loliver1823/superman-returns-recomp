#!/usr/bin/env python3
"""pacing_probe.py - record the guest's present pacing of ONE running process, Xenia Canary or our build, with
tools/perf/pacing_probe.js (observation only): the display.registers timing reference (native/dreg-xenia).

    py -3.13 tools/perf/pacing_probe.py --out <dir> --side xenia|ours (--pid N | --name <image.exe>)
             [--module SupermanReturns.exe] [--anchor-file <path>] [--stop-file <path>] [--wait 120]
             [--max-seconds 3600] [--guest-retry 900]

Writes into <dir>:
  pacing.bin       one 72-byte record per guest present (the entry of D3DDeviceSwap_sub_82112050):
                   {i64 qpc, u32 seq, u32 tid, u32 vblanks, u32 gates, u32 wait, u32 dev, u32 interval, u32 pacing,
                   u32 front, u32 rmgr, u64 rperiod, u32 last, u32 countdown, u32 torn, u32 done} - the device's
                   vblank count (dev+0x3B14), gate count (dev+0x3B20) and throttle wait snapshot (dev+0x4E4C); what
                   the present requests: the PresentInterval (dev+0x33A4), the word holding the late-release pct
                   (dev+0x2D3C, bits 23..29), the front buffer (dev+0x3490) - the words 82111EC8 and our native
                   present gate read - and the render loop's manager (*0x8296D8DC) with its tick period in ns (+0x48;
                   0 until the probe has seen the manager readable); the gate's release state: the vblank count at the
                   last release (dev+0x3B18) and the countdown (dev+0x3B1C), read twice with the two words above
                   (torn = they changed between the reads); done = the slot's index + 1, the writer's last store (a
                   slot is drained only once complete). load() also reads dxg1 / dxg2's 56-byte records and dxp0 /
                   dxp1's 32-byte ones (FORMATS: no release state / no request).
  markers.jsonl    {"t": "apt", "path", "qpc", "epoch"} the guest's 'screens/...' requests (both sides),
                   {"t": "render-mgr", "mgr", "period_ns", "rate"} each render manager the probe saw, attach notes
  probe_meta.json  qpf, the clock pair (qpc <-> unix epoch at attach and at the end), records, records lost to a
                   full ring (0 expected), slots never completed (incomplete, 0 expected), why the probe stopped
                   (stop: stop-file / max-seconds / detached / error), the hooks, the device chain
With --anchor-file the file is written (epoch seconds) the moment the guest requests 'screens/hud/main_hud' (the
anchor tools/xenia_pad_tape.py waits for, as tools/perf/present_probe.py writes it). The probe stops when the
process exits, when --stop-file appears (a final drain first) or after --max-seconds. Needs the Python that has
frida (py -3.13 here); load() and read_records() below need only the standard library.
Until the guest init succeeds, each new reason is printed and written to markers.jsonl ("guest-init-error"); exit 3
= the init never succeeded (no hook, no record), 4 = pacing_probe.js's record size is not this reader's, else 0.

    python tools/perf/pacing_probe.py --check-exe <SupermanReturns.exe>

(standard library, no frida, no process) reads the exe's PE export table: every export --side ours reads must be
there (exit 0), else the missing ones are named (exit 1) - the offline pre-flight of a pilot's exe.
"""
import argparse
import ctypes
import json
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REC = 72
FMT = "<qIIIIIIIIIIQIIII"
FIELDS = ("qpc", "seq", "tid", "vblanks", "gates", "wait", "dev", "interval", "pacing", "front", "rmgr", "rperiod",
          "last", "countdown", "torn", "done")
DONE_OFF = 68   # the `done` word's offset in a record: the slot's index + 1 once it is complete
# every record format a capture may hold (probe_meta.json "format"): the current one, dxg1 / dxg2's (the request, no
# release state, no completion word) and dxp0 / dxp1's (no request)
FORMATS = {FMT: FIELDS,
           "<qIIIIIIIIIIQ": ("qpc", "seq", "tid", "vblanks", "gates", "wait", "dev", "interval", "pacing", "front",
                             "rmgr", "rperiod"),
           "<qIIIIII": ("qpc", "seq", "tid", "vblanks", "gates", "wait", "dev")}
COMPLETE_WAIT_S = 1.0   # the final drain waits this long for a slot whose writer has not published it yet
ANCHOR_PATH = "screens/hud/main_hud"
# every export pacing_probe.js initOurs reads (kernel/guest_context.cpp, guest/guest_harness.cpp)
OURS_EXPORTS = ("g_debugGuestBase", "g_debugDispatchTable", "g_debugDispatchCodeBase", "g_debugDispatchTableEntries",
                "g_debugCtxGprOff", "g_debugCtxSize")


def pe_exports(path):
    """The names in a PE file's export directory (standard library: the headers and the export table only)."""
    with open(path, "rb") as f:
        def at(off, n):
            f.seek(off)
            return f.read(n)
        if at(0, 2) != b"MZ":
            raise ValueError("%s: not a PE file" % path)
        pe = struct.unpack("<I", at(0x3C, 4))[0]
        if at(pe, 4) != b"PE\0\0":
            raise ValueError("%s: no PE signature" % path)
        nsec, = struct.unpack("<H", at(pe + 6, 2))
        optsz, = struct.unpack("<H", at(pe + 20, 2))
        opt = pe + 24
        magic, = struct.unpack("<H", at(opt, 2))
        exp_rva, _exp_size = struct.unpack("<II", at(opt + (112 if magic == 0x20B else 96), 8))
        if not exp_rva:
            return []
        secs = []
        for i in range(nsec):
            vsize, va, rawsize, rawptr = struct.unpack("<IIII", at(opt + optsz + 40 * i + 8, 16))
            secs.append((va, max(vsize, rawsize), rawptr))

        def off(rva):
            for va, size, raw in secs:
                if va <= rva < va + size:
                    return raw + rva - va
            raise ValueError("%s: RVA 0x%x outside every section" % (path, rva))
        d = at(off(exp_rva), 40)
        n_names, = struct.unpack("<I", d[24:28])
        names_rva, = struct.unpack("<I", d[32:36])
        out = []
        for p in struct.unpack("<%dI" % n_names, at(off(names_rva), 4 * n_names)):
            out.append(at(off(p), 256).split(b"\0", 1)[0].decode("ascii", "replace"))
        return out


def check_exe(path):
    """{present, missing}: the exports --side ours needs, in the exe's export table."""
    have = set(pe_exports(path))
    return {"exe": path, "present": [n for n in OURS_EXPORTS if n in have],
            "missing": [n for n in OURS_EXPORTS if n not in have]}


def qpc_pair():
    """(qpc ticks, unix epoch ns) taken back to back; QPC is one clock for every process."""
    k = ctypes.windll.kernel32
    v = ctypes.c_int64()
    best = None
    for _ in range(5):
        k.QueryPerformanceCounter(ctypes.byref(v))
        a = v.value
        e = time.time_ns()
        k.QueryPerformanceCounter(ctypes.byref(v))
        b = v.value
        if best is None or b - a < best[0]:
            best = (b - a, (a + b) // 2, e)
    return best[1], best[2]


def find_pid(name, wait):
    import subprocess
    end = time.time() + wait
    while time.time() < end:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq %s" % name, "/FO", "CSV", "/NH"],
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = [p.strip('"') for p in line.split('","')]
            if len(parts) > 1 and parts[0].lower() == name.lower():
                return int(parts[1])
        time.sleep(0.25)
    return None


def read_records(path, fmt=FMT):
    """[dict(FORMATS[fmt] fields)] from a pacing.bin (any Python); fmt = the capture's record format."""
    if fmt not in FORMATS:
        raise ValueError("%s: unknown record format %r (known: %s)" % (path, fmt, ", ".join(FORMATS)))
    fields, size = FORMATS[fmt], struct.calcsize(fmt)
    out = []
    with open(path, "rb") as f:
        d = f.read()
    for i in range(0, len(d) - size + 1, size):
        out.append(dict(zip(fields, struct.unpack_from(fmt, d, i))))
    return out


def complete_slots(data, first):
    """For raw ring bytes holding the slots first, first + 1, ...: whether each is complete (its `done` word = its
    index + 1, the writer's last store) - the reader's test, never elapsed time."""
    out = []
    for k in range(len(data) // REC):
        done, = struct.unpack_from("<I", data, k * REC + DONE_OFF)
        out.append(done == ((first + k + 1) & 0xFFFFFFFF))
    return out


def load(probe_dir):
    """(records, markers, meta) of one capture directory (any Python). The record format is the one probe_meta.json
    names (a capture from before it was recorded: the current one)."""
    meta = json.load(open(os.path.join(probe_dir, "probe_meta.json"), encoding="utf-8"))
    recs = read_records(os.path.join(probe_dir, "pacing.bin"), meta.get("format") or FMT)
    markers = []
    mp = os.path.join(probe_dir, "markers.jsonl")
    if os.path.exists(mp):
        for line in open(mp, encoding="utf-8"):
            try:
                markers.append(json.loads(line))
            except ValueError:
                continue
    return recs, markers, meta


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check-exe", help="read the exe's export table for the --side ours exports, then exit")
    ap.add_argument("--out")
    ap.add_argument("--side", choices=("xenia", "ours"))
    ap.add_argument("--pid", type=int)
    ap.add_argument("--name")
    ap.add_argument("--module", default="SupermanReturns.exe", help="--side ours: the module with the debug exports")
    ap.add_argument("--anchor-file")
    ap.add_argument("--stop-file")
    ap.add_argument("--wait", type=float, default=120.0, help="--name: seconds to wait for the process")
    ap.add_argument("--max-seconds", type=float, default=3600.0)
    ap.add_argument("--guest-retry", type=float, default=900.0,
                    help="keep retrying the guest init this long (the guest comes up after the process)")
    ap.add_argument("--symbols", help="TEST ONLY (pacing_probe_selftest.py): a JSON file of the --side ours "
                                      "variables' addresses, in place of the module's exports")
    a = ap.parse_args()
    if a.check_exe:
        r = check_exe(a.check_exe)
        print("pacing_probe --check-exe %s: %d of %d exports present%s" % (
            a.check_exe, len(r["present"]), len(OURS_EXPORTS), "; MISSING " + ", ".join(r["missing"]) if r["missing"]
            else ""), flush=True)
        return 1 if r["missing"] else 0
    if not a.out or not a.side:
        ap.error("--out and --side are required (or --check-exe)")
    symbols = json.load(open(a.symbols, encoding="utf-8")) if a.symbols else None
    import frida

    os.makedirs(a.out, exist_ok=True)
    pid = a.pid or (find_pid(a.name, a.wait) if a.name else None)
    if not pid:
        print("pacing_probe: no process", flush=True)
        return 2
    sess = frida.attach(pid)
    detached = {"why": None}
    sess.on("detached", lambda reason, *_: detached.__setitem__("why", str(reason)))
    script = sess.create_script(open(os.path.join(HERE, "pacing_probe.js"), encoding="utf-8").read())
    mk = open(os.path.join(a.out, "markers.jsonl"), "a", encoding="utf-8")
    anchor_done = {"v": False}

    def on_message(msg, _data):
        if msg.get("type") != "send":
            mk.write(json.dumps({"t": "frida", "msg": msg}) + "\n")
            mk.flush()
            return
        p = msg["payload"]
        p["epoch"] = time.time()
        mk.write(json.dumps(p) + "\n")
        mk.flush()
        if p.get("t") == "apt" and p.get("path") == ANCHOR_PATH and a.anchor_file and not anchor_done["v"]:
            anchor_done["v"] = True
            tmp = a.anchor_file + ".tmp"
            with open(tmp, "w") as f:
                f.write("%.6f\n" % time.time())
            os.replace(tmp, a.anchor_file)
            print("anchor: main_hud requested", flush=True)

    script.on("message", on_message)
    script.load()
    q0, e0 = qpc_pair()
    info = script.exports_sync.info()
    if info["rec"] != REC:
        print("pacing_probe: pacing_probe.js writes %d-byte records, this reader expects %d" % (info["rec"], REC),
              flush=True)
        sess.detach()
        return 4
    meta = {"pid": pid, "name": a.name, "side": a.side, "module": a.module if a.side == "ours" else None,
            "qpf": info["qpf"], "cap": info["cap"], "rec": info["rec"], "fields": FIELDS, "format": FMT,
            "clock_start": {"qpc": q0, "epoch_ns": e0}, "tool": "tools/perf/pacing_probe.py"}
    print("pacing_probe: attached pid %d (%s)" % (pid, a.side), flush=True)
    guest = {"done": False, "since": time.time(), "err": None, "init": None}

    bin_f = open(os.path.join(a.out, "pacing.bin"), "ab")
    last = 0
    lost = 0
    incomplete = 0    # slots the final drain gave up on (their writer never published them): skipped, a seq gap
    cap = info["cap"]
    t_start = time.time()
    stop = {"why": None}

    def drain(final=False):
        # Round 2 (Astra round 1 P2-3): a writer takes its slot from the counter, THEN fills it and publishes `done`
        # last; a slot is written out only once complete. Non-final: up to the first incomplete slot (the rest next
        # poll). Final: each incomplete slot is waited for up to COMPLETE_WAIT_S, then skipped and counted.
        nonlocal last, lost, incomplete
        c = script.exports_sync.count()
        if c - last > cap - 4096:
            lost += (c - last) - (cap - 4096)
            last = c - (cap - 4096)
        deadline = time.time() + COMPLETE_WAIT_S
        while c > last:
            n = min(c - last, 16384)
            data = script.exports_sync.read(last, n)
            ok = complete_slots(data, last)
            k = ok.index(False) if False in ok else len(ok)
            bin_f.write(data[:k * REC])
            last += k
            if k == len(ok):
                continue
            if not final:
                break
            if time.time() < deadline:
                time.sleep(0.01)
                continue
            last += 1   # given up: the writer never published this slot
            incomplete += 1
            deadline = time.time() + COMPLETE_WAIT_S
        bin_f.flush()

    try:
        while True:
            if detached["why"]:
                stop["why"] = "detached"
                break
            if a.stop_file and os.path.exists(a.stop_file):
                time.sleep(0.2)
                drain(final=True)
                stop["why"] = "stop-file"
                break
            if time.time() - t_start > a.max_seconds:
                drain(final=True)
                stop["why"] = "max-seconds"
                break
            if not guest["done"]:
                try:
                    r = script.exports_sync.init({"side": a.side, "module": a.module, "symbols": symbols})
                    err = r.get("error")
                except Exception as e:  # noqa: BLE001
                    r, err = None, str(e)
                if err is None:
                    guest.update({"done": True, "init": r, "err": None})
                    mk.write(json.dumps({"t": "guest-init", "init": r, "epoch": time.time()}) + "\n")
                    mk.flush()
                    print("pacing_probe: guest init ok after %.1f s: %s" % (time.time() - guest["since"], json.dumps(r)),
                          flush=True)
                elif err != guest["err"]:   # each new reason once, never silent (dxp0 retried one for 6 minutes)
                    guest["err"] = err
                    mk.write(json.dumps({"t": "guest-init-error", "err": err, "epoch": time.time()}) + "\n")
                    mk.flush()
                    print("pacing_probe: guest init not ready: %s" % err, flush=True)
                if not guest["done"] and time.time() - guest["since"] > a.guest_retry:
                    guest["done"] = True
                    print("pacing_probe: guest init GAVE UP after %.0f s: %s" % (a.guest_retry, guest["err"]),
                          flush=True)
            try:
                drain()
            except Exception as e:  # noqa: BLE001 - the process is going away
                if detached["why"]:
                    break
                mk.write(json.dumps({"t": "drain-error", "err": str(e), "epoch": time.time()}) + "\n")
            time.sleep(0.25)
    finally:
        try:
            status = script.exports_sync.status()
        except Exception:  # noqa: BLE001
            status = None
        q1, e1 = qpc_pair()
        # stop: why the probe ended - "stop-file" (the runner's end of the hold, after a final drain), "max-seconds",
        # "detached" (the process went away or frida lost it), or "error" (an exception out of the loop)
        meta.update({"clock_end": {"qpc": q1, "epoch_ns": e1}, "records": last, "lost": lost,
                     "incomplete": incomplete, "stop": stop["why"] or "error", "status_end": status,
                     "detached": detached["why"], "guest_init": guest["init"], "guest_init_error": guest["err"]})
        with open(os.path.join(a.out, "probe_meta.json"), "w") as f:
            json.dump(meta, f, indent=1)
        bin_f.close()
        mk.close()
        try:
            sess.detach()
        except Exception:  # noqa: BLE001
            pass
    print("pacing_probe: %d records, %d lost, detached=%s%s" % (
        last, lost, detached["why"], "" if guest["init"] else "; the guest init NEVER succeeded: %s" % guest["err"]),
        flush=True)
    return 0 if guest["init"] else 3


if __name__ == "__main__":
    sys.exit(main())
