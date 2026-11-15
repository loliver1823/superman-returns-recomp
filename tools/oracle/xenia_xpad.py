#!/usr/bin/env python3
"""xenia_xpad.py - step-4 lane F (rog-oracle): the in-process virtual pad for xenia_seed_pass.py --pad xpad.

Runs under the Python that has frida (`py -3.13`, or SR_PYTHON on the Ally). Attaches to a Xenia process that the
pass created SUSPENDED and loads tools/oracle/xenia_xpad.js, which replaces xinput1_4.dll's entry points (the
original functions are never called: no real pad is read, no real motor driven; users 1-3 are not connected). Only
then does it write --ready-file; the pass resumes Xenia after seeing it, so the guest never runs without the hook.
Xenia must run with --hid=xinput (the SDL and "any" drivers would read the real pads themselves).

Presses and tapes use the same syntax as tools/xenia_pad_tape.py (ViGEm, the rig's pad):
  --cmd-file    tokens written to this file are pressed (A, @START = a long press, DOWN+A ...; deleted once read)
  --tape/--anchor-file  a v2 .inpr tape played from the moment --anchor-file appears (the main_hud request)

    py -3.13 tools/oracle/xenia_xpad.py --pid <suspended xenia pid> --ready-file <f> --cmd-file <f> --log <f>
        [--tape <t.inpr> --anchor-file <run>/anchor.txt] [--quit-file <f>]

Exits when Xenia exits, on --quit-file, or after --max-seconds. --log gets one line per press / tape change plus
the hook record and, at exit, the per-function call counts (the isolation witness: GetState user 0 > 0, and every
call answered by the replacement).
"""
import argparse
import json
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
JS = os.path.join(HERE, "xenia_xpad.js")
NAMES = {"UP": 0x0001, "DOWN": 0x0002, "LEFT": 0x0004, "RIGHT": 0x0008, "START": 0x0010, "BACK": 0x0020,
         "LS": 0x0040, "RS": 0x0080, "LB": 0x0100, "RB": 0x0200, "A": 0x1000, "B": 0x2000, "X": 0x4000, "Y": 0x8000}


def load_tape(path):
    """v2 INPR (tools/xenia_pad_tape.py load_tape): [(ms, buttons, lt, rt, lx, ly, rx, ry)] of the active entries."""
    d = open(path, "rb").read()
    mag, ver, _n, _ = struct.unpack_from("<IIII", d, 0)
    if mag != 0x494E5052 or ver != 2:
        raise SystemExit("%s: not a v2 INPR tape" % path)
    out = []
    for i in range(16, len(d) - 19, 20):
        ms, aa, _p, b, lt, rt, lx, ly, rx, ry = struct.unpack_from("<IHHHBBhhhh", d, i)
        if aa:
            out.append((ms, b, lt, rt, lx, ly, rx, ry))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--ready-file", required=True)
    ap.add_argument("--cmd-file", default=None)
    ap.add_argument("--log", required=True)
    ap.add_argument("--tape", default=None)
    ap.add_argument("--anchor-file", default=None)
    ap.add_argument("--quit-file", default=None)
    ap.add_argument("--dump-file", default=None, help="when this file appears: every thread's stack -> <file>.json")
    ap.add_argument("--max-seconds", type=float, default=3600.0)
    a = ap.parse_args()
    import frida  # noqa: E402
    logf = open(a.log, "a", encoding="utf-8")

    def log(m):
        logf.write("%.3f %s\n" % (time.time(), m))
        logf.flush()

    tape = load_tape(a.tape) if a.tape else []
    sess = frida.attach(a.pid)
    detached = []
    sess.on("detached", lambda reason, *_: detached.append(str(reason)))
    script = sess.create_script(open(JS, encoding="utf-8").read())
    hooked = []

    def on_message(msg, _data):
        if msg.get("type") == "send":
            p = msg["payload"]
            log("js " + json.dumps(p))
            if p.get("what") == "hooked":
                hooked.append(p)
        else:
            log("js-error " + json.dumps(msg))

    script.on("message", on_message)
    script.load()
    st = script.exports_sync.stats()
    log("loaded; stats %s" % json.dumps(st))
    with open(a.ready_file + ".tmp", "w") as f:
        json.dump({"pid": a.pid, "t": time.time(), "hooked_at_ready": st.get("hooked"), "js": JS}, f)
    os.replace(a.ready_file + ".tmp", a.ready_file)

    def setp(b, lt=0, rt=0, lx=0, ly=0, rx=0, ry=0):
        return script.exports_sync.set(b, lt, rt, lx, ly, rx, ry)

    t0 = time.time()
    t_stats = t0
    ta = None
    ti = 0
    last = None
    try:
        while not detached and time.time() - t0 < a.max_seconds:
            if a.quit_file and os.path.exists(a.quit_file):
                # final counts while the process still runs, then STAY attached: detaching reverts the replacement,
                # and a still-running guest would then read the machine's real pad. The pass kills Xenia after
                # <quit>.done; the session then ends with the process.
                try:
                    log("final stats %s" % json.dumps(script.exports_sync.stats()))
                except Exception as e:  # noqa: BLE001
                    log("final stats unavailable (%s)" % e)
                open(a.quit_file + ".done", "w").close()
                end = time.time() + 60.0
                while not detached and time.time() < end:
                    time.sleep(0.05)
                log("after quit: detached %s" % detached)
                break
            if a.dump_file and os.path.exists(a.dump_file):
                try:
                    os.remove(a.dump_file)
                    th = script.exports_sync.threads()
                    with open(a.dump_file + ".json", "w") as f:
                        json.dump(th, f, indent=1)
                    log("thread dump: %d threads -> %s.json" % (len(th), a.dump_file))
                except Exception as e:  # noqa: BLE001
                    log("thread dump failed (%s)" % e)
            if time.time() - t_stats >= 30.0:
                t_stats = time.time()
                try:
                    log("stats %s" % json.dumps(script.exports_sync.stats()))
                except Exception as e:  # noqa: BLE001
                    log("stats unavailable (%s)" % e)
            if a.cmd_file and os.path.exists(a.cmd_file):
                try:
                    txt = open(a.cmd_file).read().strip()
                    os.remove(a.cmd_file)
                except OSError:
                    txt = ""
                for tok in txt.split():
                    names = [n for n in tok.lstrip("@").upper().split("+") if n in NAMES]
                    if not names:
                        continue
                    setp(sum(NAMES[n] for n in names))
                    time.sleep(0.45 if tok.startswith("@") else 0.12)
                    setp(0)
                    time.sleep(0.45)
                    log("cmd %s at %.1f s; counts %s" % ("+".join(names), time.time() - t0,
                                                         json.dumps(script.exports_sync.stats().get("counts"))))
            if tape and ta is None and a.anchor_file and os.path.exists(a.anchor_file):
                ta = time.time()
                log("anchor seen (%.1f s after start) - playing the tape (%d entries)" % (ta - t0, len(tape)))
            while ta is not None and ti < len(tape) and time.time() >= ta + tape[ti][0] / 1000.0:
                stt = tape[ti]
                if stt[1:] != last:
                    setp(*stt[1:])
                    log("t=%.2f btn=%04X LT=%d RT=%d L=(%d,%d) R=(%d,%d)" % ((stt[0] / 1000.0,) + stt[1:]))
                    last = stt[1:]
                ti += 1
                if ti == len(tape):
                    setp(0)
                    log("tape done; neutral")
            time.sleep(0.01)
    finally:
        if not detached:
            try:
                log("exit stats %s" % json.dumps(script.exports_sync.stats()))
            except Exception as e:  # noqa: BLE001
                log("exit stats unavailable (%s)" % e)
        try:
            sess.detach()
        except Exception:  # noqa: BLE001
            pass
        log("end; detached %s" % detached)
        logf.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
