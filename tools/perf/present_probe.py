#!/usr/bin/env python3
"""present_probe.py - canary lane (iter-467): record every present submission of ONE running process
(our build, Xenia Canary, or any D3D12 / DXGI app) with tools/perf/present_probe.js, observation only.

    py -3.13 tools/perf/present_probe.py --out <dir> (--pid N | --name xenia_canary.exe)
             [--xenia] [--anchor-file <path>] [--stop-file <path>] [--wait 120] [--max-seconds 3600]

Writes into <dir>:
  presents.bin   raw records {i64 qpc, u32 id, u32 tid} (id 1..8 = the win32u present syscall named in
                 probe_meta.json "hooked"; id 100 = Xenia's guest D3DDeviceSwap = one game frame)
  markers.jsonl  {"t": "apt", "path", "qpc"} guest screen requests (Xenia: --xenia), attach notes
  probe_meta.json  qpf, the clock pair (qpc <-> unix epoch, taken at attach and at the end), counts,
                 records lost to a full ring (0 expected), the hooks that attached
With --anchor-file the file is written (epoch seconds) the moment the guest requests
'screens/hud/main_hud' (the input-tape anchor tools/xenia_pad_tape.py waits for; our build anchors
SR_INPUT_REPLAY on the same request). The probe stops when the process exits, when --stop-file
appears (a final drain first) or after --max-seconds. Needs the Python that has frida (py -3.13 here).
"""
import argparse
import ctypes
import json
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REC = 16


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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--pid", type=int)
    ap.add_argument("--name")
    ap.add_argument("--xenia", action="store_true", help="also hook the guest swap + Apt screen requests")
    ap.add_argument("--anchor-file")
    ap.add_argument("--stop-file")
    ap.add_argument("--wait", type=float, default=120.0, help="--name: seconds to wait for the process")
    ap.add_argument("--max-seconds", type=float, default=3600.0)
    ap.add_argument("--guest-retry", type=float, default=600.0,
                    help="--xenia: keep retrying the guest hooks this long (the guest image loads later)")
    a = ap.parse_args()
    import frida

    os.makedirs(a.out, exist_ok=True)
    pid = a.pid or (find_pid(a.name, a.wait) if a.name else None)
    if not pid:
        print("present_probe: no process", flush=True)
        return 2
    sess = frida.attach(pid)
    detached = {"why": None}
    sess.on("detached", lambda reason, *_: detached.__setitem__("why", str(reason)))
    script = sess.create_script(open(os.path.join(HERE, "present_probe.js"), encoding="utf-8").read())
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
        if p.get("t") == "apt" and p.get("path") == "screens/hud/main_hud" and a.anchor_file \
                and not anchor_done["v"]:
            anchor_done["v"] = True
            tmp = a.anchor_file + ".tmp"
            with open(tmp, "w") as f:
                f.write("%.6f\n" % time.time())
            os.replace(tmp, a.anchor_file)
            print("anchor: main_hud requested", flush=True)

    script.on("message", on_message)
    script.load()
    q0, e0 = qpc_pair()
    info = script.exports_sync.init({"xenia": False})
    meta = {"pid": pid, "name": a.name, "qpf": info["qpf"], "cap": info["cap"], "hooked": info["hooked"],
            "clock_start": {"qpc": q0, "epoch_ns": e0}, "xenia": a.xenia}
    print("present_probe: attached pid %d, hooks %s" % (
        pid, ", ".join("%s=%s" % (k, "ok" if "addr" in v else v.get("error")) for k, v in info["hooked"].items())),
        flush=True)
    xenia_state = {"done": not a.xenia, "since": time.time(), "err": None}

    bin_f = open(os.path.join(a.out, "presents.bin"), "ab")
    last = 0
    lost = 0
    seen = 0          # the count read on the previous poll: every record below it is >= 250 ms old
    cap = info["cap"]
    t_start = time.time()

    def drain(final=False):
        nonlocal last, lost, seen
        c = script.exports_sync.count()
        # A writer bumps the counter, then fills its slot within ~100 ns; a record counted on the
        # previous poll is complete, one counted just now may not be. A quiet ring (no new count
        # since the last poll) is complete too, so a process that stops presenting loses nothing.
        upto = c if (final or c == seen) else seen
        seen = c
        if upto - last > cap - 4096:
            lost += (upto - last) - (cap - 4096)
            last = upto - (cap - 4096)
        while upto > last:
            n = min(upto - last, 65536)
            data = script.exports_sync.read(last, n)
            bin_f.write(data)
            last += n
        bin_f.flush()

    rc = 0
    try:
        while True:
            if detached["why"]:
                break
            if a.stop_file and os.path.exists(a.stop_file):
                time.sleep(0.2)
                drain(final=True)
                break
            if time.time() - t_start > a.max_seconds:
                drain(final=True)
                break
            if not xenia_state["done"]:
                try:
                    r = script.exports_sync.init({"xenia": True})
                    if "error" in r:
                        xenia_state["err"] = r["error"]
                    else:
                        xenia_state["done"] = True
                        mk.write(json.dumps({"t": "guest-init", "epoch": time.time()}) + "\n")
                except Exception as e:  # noqa: BLE001
                    xenia_state["err"] = str(e)
                if not xenia_state["done"] and time.time() - xenia_state["since"] > a.guest_retry:
                    xenia_state["done"] = True
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
        meta.update({"clock_end": {"qpc": q1, "epoch_ns": e1}, "records": last, "lost": lost,
                     "status_end": status, "detached": detached["why"], "xenia_init_error": xenia_state["err"]})
        with open(os.path.join(a.out, "probe_meta.json"), "w") as f:
            json.dump(meta, f, indent=1)
        bin_f.close()
        mk.close()
        try:
            sess.detach()
        except Exception:  # noqa: BLE001
            pass
    print("present_probe: %d records, %d lost, detached=%s" % (last, lost, detached["why"]), flush=True)
    return rc


def read_records(path):
    """[(qpc, id, tid)] from a presents.bin (any Python)."""
    out = []
    with open(path, "rb") as f:
        d = f.read()
    for i in range(0, len(d) - REC + 1, REC):
        out.append(struct.unpack_from("<qII", d, i))
    return out


if __name__ == "__main__":
    sys.exit(main())
