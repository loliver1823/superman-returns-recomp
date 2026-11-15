"""Persistent virtual Xbox 360 controller for driving Xenia headlessly.

Part of the Xenia harness (see tools/XENIA_HARNESS.md). Requires:
  - ViGEmBus driver installed + running
  - pip install vgamepad

Run it in the background BEFORE launching Xenia so Xenia enumerates the pad:
  python tools/xenia_pad.py   (keep running)

Send input by writing space-separated button tokens to pad_cmd.txt (next to this
script). The process presses them in order, then clears the file.
Buttons: A B X Y START BACK UP DOWN LEFT RIGHT LB RB LS RS (LS/RS = stick clicks)
Prefix a token with '@' for a longer hold, e.g. '@START'. Join buttons with '+' to
press them together, e.g. '@LS+RS'.

Xenia's Superman Returns front-end mapping: START = advance press-start;
A = select; B = back; DPAD = navigate.

iter-464 controls lane - held state, sticks, triggers and waits (for gameplay
sequences such as "hold B and flick the left stick"):
  HOLD:B / HOLD:B+LB   press and KEEP held (until REL)
  REL:B / REL:ALL      release (ALL also centres both sticks and zeroes the triggers)
  LX=-1 LY=0.5 RX=0 RY=1   set a stick axis (-1..1, +Y = up); stays until changed
  LT=1 RT=0            set a trigger (0..1)
  WAIT:250             sleep 250 ms (the state is held meanwhile)
Every state token takes effect at once (one gp.update()); the plain button tokens
above keep their old press-release timing.
"""
import vgamepad as vg
import time, os

HERE = os.path.dirname(os.path.abspath(__file__))
CMD = os.path.join(HERE, "pad_cmd.txt")
LOG = os.path.join(HERE, "pad_log.txt")

gp = vg.VX360Gamepad()
B = vg.XUSB_BUTTON
BTN = {
    'A': B.XUSB_GAMEPAD_A, 'B': B.XUSB_GAMEPAD_B, 'X': B.XUSB_GAMEPAD_X, 'Y': B.XUSB_GAMEPAD_Y,
    'START': B.XUSB_GAMEPAD_START, 'BACK': B.XUSB_GAMEPAD_BACK,
    'UP': B.XUSB_GAMEPAD_DPAD_UP, 'DOWN': B.XUSB_GAMEPAD_DPAD_DOWN,
    'LEFT': B.XUSB_GAMEPAD_DPAD_LEFT, 'RIGHT': B.XUSB_GAMEPAD_DPAD_RIGHT,
    'LB': B.XUSB_GAMEPAD_LEFT_SHOULDER, 'RB': B.XUSB_GAMEPAD_RIGHT_SHOULDER,
    'LS': B.XUSB_GAMEPAD_LEFT_THUMB, 'RS': B.XUSB_GAMEPAD_RIGHT_THUMB,
}

def log(m):
    # iter-464: epoch-ms stamp, so a timeline (tools/controls/move_probe.py) can align with it
    with open(LOG, "a") as f:
        f.write("%d %s\n" % (int(time.time() * 1000), m))

gp.update()
log("pad: virtual X360 controller created")

AXES = {'LX': 0.0, 'LY': 0.0, 'RX': 0.0, 'RY': 0.0}
HELD = set()


def apply_sticks():
    gp.left_joystick_float(x_value_float=AXES['LX'], y_value_float=AXES['LY'])
    gp.right_joystick_float(x_value_float=AXES['RX'], y_value_float=AXES['RY'])


def state_token(tok):
    """iter-464: the held-state tokens. Returns True when `tok` was one of them."""
    up = tok.upper()
    if up.startswith('WAIT:'):
        time.sleep(max(0, int(up[5:])) / 1000.0)
        return True
    if up.startswith('HOLD:') or up.startswith('REL:'):
        press = up.startswith('HOLD:')
        names = up.split(':', 1)[1].split('+')
        if not press and names == ['ALL']:
            for n in list(HELD):
                gp.release_button(BTN[n])
            HELD.clear()
            for k in AXES:
                AXES[k] = 0.0
            apply_sticks()
            gp.left_trigger_float(value_float=0.0)
            gp.right_trigger_float(value_float=0.0)
        else:
            for n in names:
                if n not in BTN:
                    continue
                if press:
                    gp.press_button(BTN[n]); HELD.add(n)
                else:
                    gp.release_button(BTN[n]); HELD.discard(n)
        gp.update()
        log(up)
        return True
    if '=' in up:
        k, v = up.split('=', 1)
        val = max(-1.0, min(1.0, float(v)))
        if k in AXES:
            AXES[k] = val
            apply_sticks()
        elif k == 'LT':
            gp.left_trigger_float(value_float=max(0.0, val))
        elif k == 'RT':
            gp.right_trigger_float(value_float=max(0.0, val))
        else:
            return False
        gp.update()
        log(up)
        return True
    return False


while True:
    try:
        if os.path.exists(CMD):
            txt = open(CMD).read().strip()
            os.remove(CMD)
            for tok in txt.split():
                if state_token(tok):
                    continue
                hold = 0.45 if tok.startswith('@') else 0.12
                names = [n for n in tok.lstrip('@').upper().split('+') if n in BTN]
                if names:
                    for n in names:
                        gp.press_button(BTN[n])
                    gp.update(); time.sleep(hold)
                    for n in names:
                        gp.release_button(BTN[n])
                    gp.update(); time.sleep(0.45)
                    log("pressed " + "+".join(names))
    except Exception as e:
        log("err: " + str(e))
    time.sleep(0.05)
