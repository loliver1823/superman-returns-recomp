#!/usr/bin/env python3
"""xenia_audio_capture.py - iter-467 sound lane: record the Xenia ORACLE's audio, silently.

Launches the stock oracle Xenia (the owner's canary_experimental build and config: xma_decoder
"new", use_dedicated_xma_thread true) on the retail ISO IN THE BACKGROUND with --mute=true (Xenia
zeroes only its XAudio2 voice volume; the guest's frames are unchanged, so nothing plays through
the owner's speakers and timing is untouched), attaches tools/xenia_audio_capture.js, and records
every mixed 6-channel frame the game submits (the same capture point as our SR_AUDIO_FRAMES dump)
to <out>/xenia.afr. AptLoadByPath markers go to <out>/markers.jsonl; <out>/anchor.txt is written
when the guest requests "screens/hud/main_hud" (tools/xenia_pad_tape.py waits for it).

One game process at a time: hold tools/game_slot.py for the whole run. Drive the pad separately
(start it BEFORE this script so Xenia enumerates it):
    python tools/xenia_pad_tape.py --tape <t.inpr> --anchor-file <out>/anchor.txt --pre "..."
    py -3.13 tools/xenia_audio_capture.py --out archive/v1_audio3/x_<tag> --seconds 900

Then: python tools/audio_oracle_compare.py --oracle <out>/xenia.afr --ours <our .afr> ...
"""
import argparse
import json
import os
import sys
import time

# The Xenia Canary exe and your own disc image: --xenia / --iso, or SR_XENIA_EXE / SR_GAME_ISO.
XENIA = os.environ.get("SR_XENIA_EXE", "")
ISO = os.environ.get("SR_GAME_ISO", "")
HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=float, default=600.0)
    ap.add_argument("--xenia", default=XENIA)
    ap.add_argument("--iso", default=ISO)
    ap.add_argument("--attach-delay", type=float, default=4.0)
    ap.add_argument("--extra", action="append", default=[], help="extra Xenia command-line flag")
    a = ap.parse_args()
    if not a.xenia or not a.iso:
        sys.exit("give --xenia and --iso (or set SR_XENIA_EXE and SR_GAME_ISO)")
    import frida  # noqa: E402
    sys.path.insert(0, HERE)
    import bg_window  # noqa: E402
    os.makedirs(a.out, exist_ok=True)
    out = os.path.abspath(a.out)
    afr = os.path.join(out, "xenia.afr")
    anchor = os.path.join(out, "anchor.txt")
    for p in (afr, anchor):
        if os.path.exists(p):
            raise SystemExit("%s exists: use a fresh --out" % p)
    mk = open(os.path.join(out, "markers.jsonl"), "w")
    args = [a.xenia, "--mute=true", "--log_file=" + os.path.join(out, "xenia.log")] + a.extra + [a.iso]
    t_launch = time.time()
    proc = bg_window.popen_background(args)
    bg_window.guard_background(proc.pid, prev_foreground=proc.sr_prev_foreground)
    mk.write(json.dumps({"t": "launch", "pid": proc.pid, "wall": t_launch, "args": args}) + "\n")
    time.sleep(a.attach_delay)
    sess = frida.attach(proc.pid)
    script = sess.create_script(open(os.path.join(HERE, "xenia_audio_capture.js")).read())

    def on_message(msg, _data):
        if msg.get("type") != "send":
            mk.write(json.dumps({"t": "frida", "msg": msg}) + "\n")
            mk.flush()
            return
        p = msg["payload"]
        mk.write(json.dumps(p) + "\n")
        mk.flush()
        if p.get("t") == "anchor" and not os.path.exists(anchor):
            open(anchor, "w").write(str(time.time()))
            print("anchor: main_hud requested", flush=True)
        elif p.get("t") in ("attached", "attach-error", "started"):
            print(p, flush=True)

    script.on("message", on_message)
    script.load()
    print(script.exports_sync.start(afr), flush=True)
    t0 = time.time()
    try:
        while time.time() - t0 < a.seconds and proc.poll() is None:
            time.sleep(1)
    finally:
        try:
            print("stop:", script.exports_sync.stop(), flush=True)
        except Exception as e:  # noqa: BLE001
            print("stop failed:", e)
        try:
            sess.detach()
        except Exception:  # noqa: BLE001
            pass
        if proc.poll() is None:
            proc.terminate()
        mk.write(json.dumps({"t": "end", "wall": time.time()}) + "\n")
        mk.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
