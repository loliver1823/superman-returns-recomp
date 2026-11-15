#!/usr/bin/env python3
"""xenia_marks.py - step-4 lane A (xenia-oracle): the frida half of tools/oracle/xenia_seed_pass.py.

Runs under the Python that has frida (here `py -3.13`). Attaches to a RUNNING Xenia (the pass launched it in
the background) and loads tools/xenia_audio_capture.js UNCHANGED (the sound lane's capture script: every mixed
6-channel frame the guest submits -> an AFR1 file, and every guest AptLoadByPath of a screens/* path -> a
marker). Unlike tools/xenia_audio_capture.py it does not launch Xenia and it does not record from the boot: the
pass starts and stops the AFR through command files, so the AFR covers exactly the window it asks for.

    py -3.13 tools/oracle/xenia_marks.py --pid <xenia pid> --dir <run dir>

<dir>/markers.jsonl   every message of the script, one JSON per line ({t:'apt', path, t_us, seq}, {t:'anchor'}, ...)
                      plus {t:'cmd', ...} lines for the commands below; t_us = unix epoch us (QPC based, in Xenia)
<dir>/anchor.txt      written (epoch seconds) on the first screens/hud/main_hud request
<dir>/afr.start       (the pass writes it) -> rpc start(<dir>/xenia.afr); renamed afr.start.done
<dir>/afr.stop        (the pass writes it) -> rpc stop(); the result goes to afr.stopped (JSON)
<dir>/marks.quit      (the pass writes it) -> detach and exit (also exits when Xenia exits)
OBSERVATION ONLY: the script reads guest memory and writes a file; nothing in the guest or in Xenia is written.
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
JS = os.path.join(os.path.dirname(HERE), "xenia_audio_capture.js")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--dir", required=True)
    ap.add_argument("--max-seconds", type=float, default=1800.0)
    a = ap.parse_args()
    import frida  # noqa: E402
    d = os.path.abspath(a.dir)
    os.makedirs(d, exist_ok=True)
    afr = os.path.join(d, "xenia.afr")
    anchor = os.path.join(d, "anchor.txt")
    mk = open(os.path.join(d, "markers.jsonl"), "a", encoding="utf-8")
    import threading
    lock = threading.Lock()   # frida's message thread and this loop both write markers.jsonl

    def note(rec):
        rec.setdefault("wall", time.time())
        with lock:
            mk.write(json.dumps(rec) + "\n")
            mk.flush()

    sess = frida.attach(a.pid)
    note({"t": "cmd", "what": "attached", "pid": a.pid, "js": JS})
    script = sess.create_script(open(JS, encoding="utf-8").read())
    detached = []
    sess.on("detached", lambda reason, *_: detached.append(str(reason)))

    def on_message(msg, _data):
        if msg.get("type") != "send":
            note({"t": "frida", "msg": msg})
            return
        p = dict(msg["payload"])
        note(p)
        if p.get("t") == "anchor" and not os.path.exists(anchor):
            with open(anchor, "w") as f:
                f.write(str(p.get("t_us", 0) / 1e6))

    script.on("message", on_message)
    script.load()
    t0 = time.time()
    started = stopped = False
    try:
        while time.time() - t0 < a.max_seconds and not detached:
            if not started and os.path.exists(os.path.join(d, "afr.start")):
                r = script.exports_sync.start(afr)
                started = True
                os.replace(os.path.join(d, "afr.start"), os.path.join(d, "afr.start.done"))
                note({"t": "cmd", "what": "afr-start", "result": r})
            if started and not stopped and os.path.exists(os.path.join(d, "afr.stop")):
                r = script.exports_sync.stop()
                stopped = True
                with open(os.path.join(d, "afr.stopped"), "w") as f:
                    json.dump(r, f)
                note({"t": "cmd", "what": "afr-stop", "result": r})
            if os.path.exists(os.path.join(d, "marks.quit")):
                break
            time.sleep(0.05)
    finally:
        if started and not stopped and not detached:
            try:
                r = script.exports_sync.stop()
                with open(os.path.join(d, "afr.stopped"), "w") as f:
                    json.dump(r, f)
                note({"t": "cmd", "what": "afr-stop (exit)", "result": r})
            except Exception as e:  # noqa: BLE001
                note({"t": "cmd", "what": "afr-stop failed", "err": str(e)})
        try:
            sess.detach()
        except Exception:  # noqa: BLE001
            pass
        note({"t": "cmd", "what": "end", "detached": detached})
        mk.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
