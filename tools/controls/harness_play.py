#!/usr/bin/env python3
"""iter-464 controls lane: drive a running game through the HARNESS INPUT CHANNEL with the
PLAYER keymap and the channel's MOUSE block (kernel/host_automation.hpp layout v1 + the
controls extension, kernel/pc_controls.hpp). The window stays in the background; no focus,
no SendInput, the desktop mouse never moves.

    python tools/controls/harness_play.py --instance run1 --log superman_returns_run1.log \
        --script tools/controls/scripts/<name>.txt [--out archive/v1_controls/<tag>/play.jsonl]

Script language (one command per line, '#' comments):
    keymap player|legacy          # channel flags bit0 (player keymap) - legacy = sr_drive's map
    await <regex> [timeout_s]     # wait for a NEW log line matching (from the script's start)
    wait <ms>
    down <key> / up <key>         # SDL key name as in input.ini ("W", "Left Alt", "Space", "Return")
    tap <key> [hold_ms]           # down, hold (default 120 ms), up
    pulse <key> <hold_ms> <gap_ms> <n>
    mouse <dx> <dy> [over_ms]     # relative counts, spread evenly over over_ms (default 0)
    button <left|right|middle> <down|up>
    click <left|right|middle> [hold_ms]
    wheel <n>                     # + = up
    tap_until <key> <hold_ms> <gap_ms> <max> <regex>   # tap until a NEW log line matches
    await_ctx <n> [timeout_s]     # wait until the read-back context (pc_controls Context) == n
    hold_ctx <key> <n> [max_hold_ms] [tries] [settle_ms]   # hold <key> until the read-back context
                                  # == n (1 menu, 2 gameplay), release; unconfirmed after the settle:
                                  # press again, up to tries (default 4000 / 3 / 3000). Never confirmed =
                                  # FATAL (a context-changing tap must not be lost: the flicker gate's 2 s
                                  # SIM_FREEZE holds swallowed 150 ms Escape taps and left the pause menu up)
    mark dump                     # iter-467: set the channel's DUMP MARK (flags bit 2): with
                                  # SR_PRESENT_DUMP_FROM_MARK=1 the every-present dump window opens at
                                  # the next present (the flicker gate's span: tools/perf/flicker_play.txt)
    sample <label>                # record the game's read-back pad state (0x320) into --out
    watch <ms> <label>            # sample every poll-count change for <ms>
    press <key> <hold_ms> <label> # down, `watch` for the hold (what the key mapped to), up
    expect <regex> [timeout_s]    # like await, but a miss is recorded, not fatal
    tap_try <key> <hold_ms> <gap_ms> <max> <regex>   # tap_until, but a miss is not fatal
    ifmiss <command>              # run <command> only if the last expect / tap_try missed
    ifmatch <regex> <command>     # run <command> only if the last expect / tap_try HIT and its line matches
                                  # <regex> (no spaces in it) - a branch on WHICH alternative an expect saw
                                  # (flicker_gate --from-save: the HUD or the METROPOLIS / FORTRESS menu)
    releaseall
    cursor <feed line>            # iter-466 controls2: append a line to the mouse-cursor feed
                                  # (--mouse-feed = the game's SR_MOUSE_FEED): "move x y" (stage
                                  # 1280x720), "down left", "up left", "wheel n" (kernel/mouse_ui.hpp)
    cclick <x> <y> [left|right|middle]   # cursor: move there, then a click
    chover <index> / cclickitem <index> [left|right]   # cursor to the centre of the menu item with
                                  # that game index, from the newest [MOUSE_UI] slot lines
                                  # (needs SR_MOUSE_UI_LOG=1); cclickitem clicks it
    vplug [x360|ds4] / vunplug    # a ViGEm Xbox 360 pad (default) or DualShock 4 (vgamepad; the game needs
                                  # SR_HARNESS_VPAD=1); on a ds4: vpress A/B/X/Y/START/BACK/LB/RB (no D-pad)
    vpress <A|B|X|Y|START|BACK|UP|DOWN|LEFT|RIGHT|LB|RB> <hold_ms> [label]   # watch while held
    vtrigger <l|r> <hold_ms> [label]
    vstick <l|r> <x> <y> <hold_ms> [label]   # XInput units, +y up
Every step is appended to --out as JSON (the read-back block included) for assertions.
"""
import argparse
import json
import math
import mmap
import os
import re
import struct
import sys
import time

MAGIC = 0x49485253
OFF_MAGIC, OFF_VER, OFF_PID, OFF_POLLS, OFF_LASTMASK, OFF_FLAGS = 0x00, 0x04, 0x08, 0x0C, 0x10, 0x14
OFF_KEYS = 0x40
OFF_MOUSE = 0x300          # i32 dxTotal, i32 dyTotal, i32 wheelTotal, u32 buttons
OFF_READBACK = 0x320       # see kernel/pc_controls.cpp writeReadback
FLAG_DUMP_MARK = 4         # kernel/host_automation.hpp kHarnessFlagDumpMark
MAP_BYTES = 4096

# SDL scancodes for the names the scripts use (SDL_scancode.h).
SC = {
    'A': 4, 'B': 5, 'C': 6, 'D': 7, 'E': 8, 'F': 9, 'G': 10, 'H': 11, 'I': 12, 'J': 13,
    'K': 14, 'L': 15, 'M': 16, 'N': 17, 'O': 18, 'P': 19, 'Q': 20, 'R': 21, 'S': 22,
    'T': 23, 'U': 24, 'V': 25, 'W': 26, 'X': 27, 'Y': 28, 'Z': 29,
    '1': 30, '2': 31, '3': 32, '4': 33, '5': 34, '6': 35, '7': 36, '8': 37, '9': 38, '0': 39,
    'RETURN': 40, 'ENTER': 40, 'ESCAPE': 41, 'BACKSPACE': 42, 'TAB': 43, 'SPACE': 44,
    'RIGHT': 79, 'LEFT': 80, 'DOWN': 81, 'UP': 82,
    'LEFT CTRL': 224, 'LEFT SHIFT': 225, 'LEFT ALT': 226,
    'RIGHT CTRL': 228, 'RIGHT SHIFT': 229, 'RIGHT ALT': 230, 'F9': 66,
    'F6': 63, 'F7': 64, 'F8': 65, 'F10': 67, 'F11': 68,   # iter-465: dev-tool hotkeys (F10 skip mission, F11 skip chapter)
}
BTN = {'left': 1, 'right': 2, 'middle': 4, 'x1': 8, 'x2': 16}


CHANNEL_TIMEOUT_S = 60   # the default wait for the game to stamp its channel (--channel-timeout overrides it)


def open_channel(instance, timeout_s=CHANNEL_TIMEOUT_S):
    """The game's harness input channel once a game stamped it, waiting up to timeout_s. A caller that starts the tape
    before its driver launches the game (perf_run: harness_play first, then sr_drive's stage pass, the previous game's
    sweep and the launch) passes the roll's own timeout: a fixed 60 s let zpk2's keep_off tape die (2026-10-16:
    sr_drive launched the game 142 s after the tape started) while the roll played on untapped."""
    if not math.isfinite(timeout_s) or timeout_s <= 0:
        raise ValueError('channel timeout must be finite and positive')
    sid = re.sub(r'[^A-Za-z0-9_.-]', '_', instance or 'default')[:64]
    name = 'Local\\SR_HARNESS_INPUT_' + sid
    deadline = time.monotonic() + timeout_s
    while True:
        m = mmap.mmap(-1, MAP_BYTES, tagname=name)
        magic, ver, pid = struct.unpack_from('<III', m, 0)
        if magic == MAGIC and ver == 1 and pid != 0:
            return m, pid
        m.close()
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            sys.exit('harness_play: channel %s not stamped by a game within %gs' % (name, timeout_s))
        time.sleep(min(0.5, remaining))


class Player:
    def __init__(self, m, log_path, out):
        self.m = m
        self.log_path = log_path
        self.log_pos = os.path.getsize(log_path) if log_path and os.path.exists(log_path) else 0
        self.out = open(out, 'a') if out else None
        self.dx = self.dy = self.wheel = 0
        self.buttons = 0
        self.t0 = time.time()

    def rec(self, **kw):
        kw['t'] = round(time.time() - self.t0, 3)
        kw['epoch_ms'] = int(time.time() * 1000)
        kw['readback'] = self.readback()
        line = json.dumps(kw)
        print(line, flush=True)
        if self.out:
            self.out.write(line + '\n')
            self.out.flush()

    def readback(self):
        polls = struct.unpack_from('<I', self.m, OFF_POLLS)[0]
        (ver, ctx, dodge, moves, buttons, lt, rt, lx, ly, rx, ry, seq, mlookPolls) = struct.unpack_from(
            '<BBBBHBBhhhhII', self.m, OFF_READBACK)
        return dict(polls=polls, rbver=ver, ctx=ctx, dodge=dodge, moves=moves,
                    buttons='0x%04X' % buttons, lt=lt, rt=rt, lx=lx, ly=ly, rx=rx, ry=ry,
                    seq=seq, mlook=mlookPolls)

    def key(self, name, down):
        sc = SC.get(name.upper())
        if sc is None:
            sys.exit('harness_play: unknown key %r' % name)
        self.m[OFF_KEYS + sc] = 1 if down else 0

    def flags(self, player):
        f = struct.unpack_from('<I', self.m, OFF_FLAGS)[0]
        f = (f | 1) if player else (f & ~1)
        f |= 2   # mouse block present
        struct.pack_into('<I', self.m, OFF_FLAGS, f)

    def push_mouse(self):
        struct.pack_into('<iiiI', self.m, OFF_MOUSE, self.dx, self.dy, self.wheel, self.buttons)

    def mouse(self, dx, dy, over_ms):
        steps = max(1, int(over_ms / 8)) if over_ms > 0 else 1
        sx = sy = 0
        for i in range(1, steps + 1):
            tx, ty = round(dx * i / steps), round(dy * i / steps)
            self.dx += tx - sx
            self.dy += ty - sy
            sx, sy = tx, ty
            self.push_mouse()
            if steps > 1:
                time.sleep(over_ms / 1000.0 / steps)

    def await_log(self, pattern, timeout_s):
        rx = re.compile(pattern)
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if self.log_path and os.path.exists(self.log_path):
                with open(self.log_path, 'rb') as f:
                    f.seek(self.log_pos)
                    chunk = f.read()
                nl = chunk.rfind(b'\n')
                if nl >= 0:
                    # consume up to the MATCHING line only: a later line in the same chunk is
                    # still there for the next await / expect
                    pos = 0
                    for raw in chunk[:nl + 1].split(b'\n')[:-1]:
                        pos += len(raw) + 1
                        ln = raw.decode('utf-8', 'replace').rstrip('\r')
                        if rx.search(ln):
                            self.log_pos += pos
                            return ln
                    self.log_pos += nl + 1
            time.sleep(0.25)
        return None

    def release_all(self):
        for i in range(512):
            self.m[OFF_KEYS + i] = 0
        self.buttons = 0
        self.push_mouse()


MOUSE_SLOT = re.compile(r'\[MOUSE_UI\]\s+slot \d+ index=(-?\d+) kind=\d+ en=1 rect=\(([-\d.]+),([-\d.]+)\)-\(([-\d.]+),([-\d.]+)\)')


def _item_rect(self, idx):
    """The newest enabled rect of menu item `idx` from the run log's [MOUSE_UI] slot lines (the log
    grows by thousands of lines a second: read it backwards in growing windows)."""
    if not self.log_path or not os.path.exists(self.log_path):
        return None
    size = os.path.getsize(self.log_path)
    win = 1 << 20
    while True:
        lo = max(0, size - win)
        with open(self.log_path, 'rb') as f:
            f.seek(lo)
            tail = f.read(size - lo).decode('utf-8', 'replace')
        # only the newest snapshot's slot lines describe what is on screen now
        snap = tail.rfind('[MOUSE_UI] snapshot #')
        best = None
        if snap >= 0:
            for m in MOUSE_SLOT.finditer(tail, snap):
                if int(m.group(1)) == idx:
                    best = tuple(float(m.group(k)) for k in (2, 3, 4, 5))
                    break
        if snap >= 0 or lo == 0 or win >= (256 << 20):
            return best
        win *= 4


def _watch(self, ms, label):
    end = time.time() + int(ms) / 1000.0
    last = None
    while time.time() < end:
        rb = self.readback()
        if rb['polls'] != last:
            last = rb['polls']
            self.rec(step='watch ' + label)
        time.sleep(0.004)


def _vpad(self, cmd, parts, line):
    import vgamepad as vg   # lazy: only vpad scripts need it
    X = vg.XUSB_BUTTON
    names = {'A': X.XUSB_GAMEPAD_A, 'B': X.XUSB_GAMEPAD_B, 'X': X.XUSB_GAMEPAD_X, 'Y': X.XUSB_GAMEPAD_Y,
             'START': X.XUSB_GAMEPAD_START, 'BACK': X.XUSB_GAMEPAD_BACK,
             'UP': X.XUSB_GAMEPAD_DPAD_UP, 'DOWN': X.XUSB_GAMEPAD_DPAD_DOWN,
             'LEFT': X.XUSB_GAMEPAD_DPAD_LEFT, 'RIGHT': X.XUSB_GAMEPAD_DPAD_RIGHT,
             'LB': X.XUSB_GAMEPAD_LEFT_SHOULDER, 'RB': X.XUSB_GAMEPAD_RIGHT_SHOULDER}
    if cmd == 'vplug':
        # `vplug` / `vplug x360`: a ViGEm Xbox 360 pad; `vplug ds4` (step 4 item 2f, 2026-10-18): a ViGEm DualShock 4,
        # whose triggers are their own HID axes (the 360 pad's share one, which SDL's RAWINPUT driver without XInput
        # correlation - SR_HARNESS_VPAD=1 - reads as 0)
        kind = parts[0].lower() if parts else 'x360'
        if kind not in ('x360', 'ds4'):
            sys.exit('harness_play: vplug [x360|ds4]: ' + line)
        self.pad = vg.VDS4Gamepad() if kind == 'ds4' else vg.VX360Gamepad()
        self.pad_kind = kind
        self.pad.update()
        self.rec(step=line)
        return
    if cmd == 'vunplug':
        if getattr(self, 'pad', None):
            del self.pad
            self.pad = None
        self.rec(step=line)
        return
    pad = getattr(self, 'pad', None)
    if pad is None:
        sys.exit('harness_play: vplug first')
    if getattr(self, 'pad_kind', 'x360') == 'ds4':
        D, DS = vg.DS4_BUTTONS, vg.DS4_SPECIAL_BUTTONS
        names = {'A': D.DS4_BUTTON_CROSS, 'B': D.DS4_BUTTON_CIRCLE, 'X': D.DS4_BUTTON_SQUARE,
                 'Y': D.DS4_BUTTON_TRIANGLE, 'START': D.DS4_BUTTON_OPTIONS, 'BACK': D.DS4_BUTTON_SHARE,
                 'LB': D.DS4_BUTTON_SHOULDER_LEFT, 'RB': D.DS4_BUTTON_SHOULDER_RIGHT}
        if cmd == 'vpress' and parts and parts[0].upper() in ('UP', 'DOWN', 'LEFT', 'RIGHT'):
            sys.exit('harness_play: vpress on a ds4 pad: the D-pad is not mapped here (%s)' % line)
    if cmd == 'vpress':
        hold, label = int(parts[1]), (parts[2] if len(parts) > 2 else parts[0])
        pad.press_button(button=names[parts[0].upper()]); pad.update()
        self.watch(hold, label)
        pad.release_button(button=names[parts[0].upper()]); pad.update()
    elif cmd == 'vtrigger':
        hold, label = int(parts[1]), (parts[2] if len(parts) > 2 else 'trigger')
        (pad.left_trigger if parts[0] == 'l' else pad.right_trigger)(value=255); pad.update()
        self.watch(hold, label)
        (pad.left_trigger if parts[0] == 'l' else pad.right_trigger)(value=0); pad.update()
    elif cmd == 'vstick':
        x, y, hold = int(parts[1]), int(parts[2]), int(parts[3])
        label = parts[4] if len(parts) > 4 else 'stick'
        if getattr(self, 'pad_kind', 'x360') == 'ds4':
            # the DS4 target's axes are 0..255 (128 = centre): vgamepad's float form, from XInput units (+y up)
            ff = pad.left_joystick_float if parts[0] == 'l' else pad.right_joystick_float

            def f(x_value, y_value):
                ff(x_value_float=max(-1.0, min(1.0, x_value / 32767.0)),
                   y_value_float=max(-1.0, min(1.0, y_value / 32767.0)))
        else:
            f = pad.left_joystick if parts[0] == 'l' else pad.right_joystick
        # a ramp, as a thumb moves: SDL sends no SDL_CONTROLLERAXISMOTION for an axis's FIRST change
        # after the pad opens (its initial-value settling), and player 1 changes device on that
        # event (host_input.cpp) - an instant full deflection never made the pad player 1
        f(x_value=x // 2, y_value=y // 2); pad.update()
        time.sleep(0.06)
        f(x_value=x, y_value=y); pad.update()
        self.watch(hold, label)
        f(x_value=0, y_value=0); pad.update()
    self.rec(step=line)


def hold_ctx(p, rest):
    """`hold_ctx <key> <ctx> [max_hold_ms] [tries] [settle_ms]` -> (confirmed, presses). The key is held until
    the read-back context is <ctx> (or max_hold_ms), then released; a press the read-back has not confirmed
    after settle_ms more is pressed again. Holding through the change is safe: pc_controls' context latch
    ignores an input still held when the context changes (Esc = START opens the pause menu, where Esc = B).
    The read-back only moves when the game polls its pad, so a press held across a sim hold (SR_DIAG_SIM_FREEZE,
    2 s) lands when the sim resumes - a fixed 150 ms tap inside the hold never reached the game."""
    toks = rest.split()
    nums = []
    while toks and toks[-1].isdigit() and len(nums) < 4:
        nums.insert(0, int(toks.pop()))
    if not toks or not nums:
        sys.exit('harness_play: hold_ctx <key> <ctx> [max_hold_ms] [tries] [settle_ms]: ' + rest)
    name, want = ' '.join(toks), nums[0]
    max_hold = nums[1] if len(nums) > 1 else 4000
    tries = nums[2] if len(nums) > 2 else 3
    settle = nums[3] if len(nums) > 3 else 3000

    def until(ms, step_s):
        end = time.time() + ms / 1000.0
        while time.time() < end and p.readback()['ctx'] != want:
            time.sleep(step_s)
        return p.readback()['ctx'] == want

    if p.readback()['ctx'] == want:
        return True, 0
    for k in range(1, tries + 1):
        p.key(name, True)
        try:
            held = until(max_hold, 0.01)
        finally:
            p.key(name, False)
        if held or until(settle, 0.05):
            return True, k
        p.rec(step='hold_ctx %s: press %d of %d not confirmed (context %d, want %d)'
              % (name, k, tries, p.readback()['ctx'], want))
    return False, tries


def run(p, lines):
    missed = False
    last_hit = None   # the line the last expect / tap_try matched (ifmatch)
    for raw in lines:
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        if line.lower().startswith('ifmiss '):
            if not missed:
                continue
            line = line[7:].strip()
        if line.lower().startswith('ifmatch '):
            rx, _, rest_cmd = line[8:].strip().partition(' ')
            if not (last_hit and re.search(rx, last_hit)):
                continue
            line = rest_cmd.strip()
        if line.lower().startswith('expect '):
            m = re.match(r'(.*?)(?:\s+(\d+))?$', line[7:].strip())
            hit = p.await_log(m.group(1), int(m.group(2) or 30))
            missed = hit is None
            last_hit = hit
            p.rec(step=line, hit=hit)
            continue
        # key names may contain spaces ("Left Alt"): quote-free parse by command shape
        cmd, _, rest = line.partition(' ')
        cmd = cmd.lower()
        if cmd == 'keymap':
            p.flags(rest.strip().lower() == 'player')
            p.rec(step=line)
        elif cmd == 'await':
            m = re.match(r'(.*?)(?:\s+(\d+))?$', rest.strip())
            hit = p.await_log(m.group(1), int(m.group(2) or 300))
            p.rec(step=line, hit=hit)
            if hit is None:
                sys.exit('harness_play: await timed out: ' + line)
        elif cmd == 'wait':
            time.sleep(int(rest) / 1000.0)
        elif cmd in ('down', 'up'):
            p.key(rest.strip(), cmd == 'down')
            p.rec(step=line)
        elif cmd == 'tap':
            parts = rest.strip().rsplit(' ', 1)
            hold = 120
            if len(parts) == 2 and parts[1].isdigit():
                name, hold = parts[0], int(parts[1])
            else:
                name = rest.strip()
            p.key(name, True); time.sleep(hold / 1000.0); p.key(name, False)
            p.rec(step=line)
        elif cmd == 'pulse':
            parts = rest.strip().rsplit(' ', 3)
            name, hold, gap, n = parts[0], int(parts[1]), int(parts[2]), int(parts[3])
            for _ in range(n):
                p.key(name, True); time.sleep(hold / 1000.0); p.key(name, False); time.sleep(gap / 1000.0)
            p.rec(step=line)
        elif cmd == 'mouse':
            a = rest.split()
            p.mouse(int(a[0]), int(a[1]), int(a[2]) if len(a) > 2 else 0)
            p.rec(step=line)
        elif cmd == 'button':
            b, st = rest.split()
            p.buttons = (p.buttons | BTN[b]) if st == 'down' else (p.buttons & ~BTN[b])
            p.push_mouse()
            p.rec(step=line)
        elif cmd == 'click':
            a = rest.split()
            hold = int(a[1]) if len(a) > 1 else 120
            p.buttons |= BTN[a[0]]; p.push_mouse(); time.sleep(hold / 1000.0)
            p.buttons &= ~BTN[a[0]]; p.push_mouse()
            p.rec(step=line)
        elif cmd == 'wheel':
            p.wheel += int(rest); p.push_mouse()
            p.rec(step=line)
        elif cmd in ('tap_until', 'tap_try'):
            m = re.match(r'(.+?) (\d+) (\d+) (\d+) (.+)$', rest.strip())
            name, hold, gap, mx, rxs = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), m.group(5)
            hit = None
            for k in range(mx):
                p.key(name, True); time.sleep(hold / 1000.0); p.key(name, False)
                hit = p.await_log(rxs, gap / 1000.0)
                if hit:
                    break
            p.rec(step=line, hit=hit)
            missed = hit is None
            last_hit = hit
            if hit is None and cmd == 'tap_until':
                sys.exit('harness_play: tap_until gave up: ' + line)
        elif cmd == 'await_ctx':
            a = rest.split()
            want, deadline = int(a[0]), time.time() + (int(a[1]) if len(a) > 1 else 300)
            while time.time() < deadline and p.readback()['ctx'] != want:
                time.sleep(0.1)
            p.rec(step=line)
            if p.readback()['ctx'] != want:
                sys.exit('harness_play: await_ctx timed out: ' + line)
        elif cmd == 'mark':
            if rest.strip().lower() != 'dump':
                sys.exit('harness_play: unknown mark: ' + line)
            f = struct.unpack_from('<I', p.m, OFF_FLAGS)[0]
            struct.pack_into('<I', p.m, OFF_FLAGS, f | FLAG_DUMP_MARK)
            p.rec(step=line)
        elif cmd == 'hold_ctx':
            confirmed, tries_used = hold_ctx(p, rest)
            p.rec(step=line, confirmed=confirmed, tries=tries_used)
            if not confirmed:
                sys.exit('harness_play: hold_ctx never confirmed the context change: ' + line)
        elif cmd == 'sample':
            p.rec(step=line)
        elif cmd == 'watch':
            ms, label = rest.split(' ', 1)
            end = time.time() + int(ms) / 1000.0
            last = None
            while time.time() < end:
                rb = p.readback()
                if rb['polls'] != last:
                    last = rb['polls']
                    p.rec(step='watch ' + label)
                time.sleep(0.004)
        elif cmd == 'press':
            name, hold, label = re.match(r'(.+?) (\d+) (\S+)$', rest.strip()).groups()
            p.key(name, True)
            p.rec(step=line + ' (down)')
            end = time.time() + int(hold) / 1000.0
            last = None
            while time.time() < end:
                rb = p.readback()
                if rb['polls'] != last:
                    last = rb['polls']
                    p.rec(step='press ' + label)
                time.sleep(0.004)
            p.key(name, False)
            p.rec(step=line + ' (up)')
        elif cmd in ('chover', 'cclickitem'):
            parts = rest.split()
            idx = int(parts[0])
            btn = parts[1] if len(parts) > 1 else 'left'
            rect = p.item_rect(idx)
            if not rect:
                p.rec(step=line, hit=None)
                missed = True
                continue
            cx, cy = (rect[0] + rect[2]) / 2.0, (rect[1] + rect[3]) / 2.0
            feed_lines = ['move %.1f %.1f' % (cx - 4, cy), 'move %.1f %.1f' % (cx, cy)]
            if cmd == 'cclickitem':
                feed_lines += ['down ' + btn, 'up ' + btn]
            for fl in feed_lines:
                with open(p.feed, 'a') as ff:
                    ff.write(fl + chr(10))
                time.sleep(0.15)
            missed = False
            p.rec(step=line, rect=rect)
        elif cmd in ('vplug', 'vunplug', 'vpress', 'vtrigger', 'vstick'):
            parts = rest.split()
            p.vpad(cmd, parts, line)
        elif cmd in ('cursor', 'cclick'):
            if not p.feed:
                sys.exit('harness_play: cursor commands need --mouse-feed')
            if cmd == 'cursor':
                feed_lines = [rest.strip()]
            else:
                parts = rest.split()
                btn = parts[2] if len(parts) > 2 else 'left'
                feed_lines = ['move %s %s' % (parts[0], parts[1]), 'down ' + btn, 'up ' + btn]
            for fl in feed_lines:
                with open(p.feed, 'a') as ff:
                    ff.write(fl + chr(10))
                time.sleep(0.12)
            p.rec(step=line)
        elif cmd == 'releaseall':
            p.release_all()
            p.rec(step=line)
        else:
            sys.exit('harness_play: unknown command: ' + line)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--instance', required=True)
    ap.add_argument('--log', default='')
    ap.add_argument('--script', required=True)
    ap.add_argument('--out', default='')
    ap.add_argument('--mouse-feed', default='', help="the game's SR_MOUSE_FEED file (cursor commands)")
    ap.add_argument('--channel-timeout', type=float, default=CHANNEL_TIMEOUT_S,
                    help="seconds to wait for a game to stamp the input channel (default %(default)s; perf_run passes "
                         "the roll's own timeout: the tape starts before the driver launches the game)")
    a = ap.parse_args()
    if not math.isfinite(a.channel_timeout) or a.channel_timeout <= 0:
        ap.error('--channel-timeout must be finite and positive')
    m, pid = open_channel(a.instance, a.channel_timeout)
    print('harness_play: channel open, game pid %d' % pid, flush=True)
    p = Player(m, a.log, a.out)
    p.feed = a.mouse_feed
    p.log_path = a.log
    p.pad = None
    Player.item_rect = _item_rect
    Player.vpad = _vpad
    Player.watch = _watch
    try:
        with open(a.script) as f:
            run(p, f.readlines())
    finally:
        try:
            p.release_all()
        finally:
            if p.out:
                p.out.close()
            m.close()


if __name__ == '__main__':
    main()
