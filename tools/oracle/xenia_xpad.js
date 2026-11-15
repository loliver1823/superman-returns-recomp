/*
 * xenia_xpad.js - step-4 lane F (rog-oracle): an in-process virtual pad for an agent-owned Xenia launched with
 * --hid=xinput, for machines whose own controller must never reach the guest (the ROG Ally's built-in pad is an
 * XInput device, and Xenia reads every pad). Loaded by tools/oracle/xenia_xpad.py into a Xenia process created
 * SUSPENDED, before its first instruction runs.
 *
 * It REPLACES xinput1_4.dll's XInputGetCapabilities / XInputGetState / XInputGetStateEx (ordinal 100, used while
 * guide_button=true) / XInputGetKeystroke / XInputSetState / XInputEnable with C functions (a CModule). The original
 * functions are NEVER called, so no real pad is ever read and no real motor is ever driven:
 *   user 0   = the virtual pad: connected wired-gamepad caps, the state set by rpc set(), packet number bumped on
 *              every change (as XInput does); SetState (rumble) accepted and dropped; keystrokes ERROR_EMPTY;
 *   users 1-3 = ERROR_DEVICE_NOT_CONNECTED.
 * The replacements run no JavaScript: Xenia's threads never take the frida JS lock (the hot path is the guest's
 * XamInputGetState every frame). The state lives in one shared block; rpc set() writes it, rpc stats() reads the
 * per-function / per-user call counters.
 * If xinput1_4.dll is not loaded yet (a suspended process has only ntdll), this script loads it (Module.load) and
 * replaces it at once; Xenia's LoadLibraryW later gets the same, already replaced module. (No module observer: its
 * JS callback runs under the loader lock, and with Xenia's window creation loading DLLs while the other frida
 * sessions attached, that deadlocked the UI thread - ROG pilot rx1 2026-10-18, no "Setup:" log line in 360 s.)
 */
'use strict';

// block layout: +0 packet u32 | +4 buttons u16 | +6 lt u8 | +7 rt u8 | +8 lx | +10 ly | +12 rx | +14 ry (s16)
//               +16 counters u32[6 functions][5 users: 0..3, other]
const FNS = ['GetState', 'GetStateEx', 'GetCapabilities', 'GetKeystroke', 'SetState', 'Enable'];
const st = Memory.alloc(256);
st.writeU32(1);

const cm = new CModule(`
#include <stdint.h>
extern unsigned char st[];

static void bump(int fn, uint32_t user) {
  uint32_t *c = (uint32_t *) (st + 16);
  c[fn * 5 + (user < 4 ? (int) user : 4)]++;
}

static void pad(unsigned char *p) {
  int i;
  for (i = 0; i < 12; i++)
    p[i] = st[4 + i];
}

uint32_t x_getstate(uint32_t user, unsigned char *out) {
  bump(0, user);
  if (user != 0)
    return 1167;
  *(uint32_t *) out = *(uint32_t *) st;
  pad(out + 4);
  return 0;
}

uint32_t x_getstateex(uint32_t user, unsigned char *out) {
  bump(1, user);
  if (user != 0)
    return 1167;
  *(uint32_t *) out = *(uint32_t *) st;
  pad(out + 4);
  return 0;
}

uint32_t x_getcaps(uint32_t user, uint32_t flags, unsigned char *out) {
  int i;
  bump(2, user);
  if (user != 0)
    return 1167;
  out[0] = 1;            /* Type XINPUT_DEVTYPE_GAMEPAD */
  out[1] = 1;            /* SubType XINPUT_DEVSUBTYPE_GAMEPAD */
  out[2] = 0;            /* Flags */
  out[3] = 0;
  out[4] = 0xFF;         /* Gamepad capability masks of a wired 360 pad: buttons 0xF3FF */
  out[5] = 0xF3;
  out[6] = 0xFF;         /* triggers */
  out[7] = 0xFF;
  for (i = 8; i < 16; i += 2) {
    out[i] = 0xC0;       /* thumbs -64 (0xFFC0) */
    out[i + 1] = 0xFF;
  }
  out[16] = 0xFF;        /* vibration 0x00FF / 0x00FF */
  out[17] = 0;
  out[18] = 0xFF;
  out[19] = 0;
  return 0;
}

uint32_t x_getkeystroke(uint32_t user, uint32_t reserved, void *out) {
  bump(3, user);
  return (user == 0 || user == 0xFF) ? 4306 : 1167;
}

uint32_t x_setstate(uint32_t user, void *vibration) {
  bump(4, user);
  return user == 0 ? 0 : 1167;
}

void x_enable(int on) {
  bump(5, 0);
}
`, { st: st });

let hooked = null;

function hook(m) {
  const ex = (n) => m.findExportByName(n);
  // ordinal 100 (XInputGetStateEx) has no name: resolve it the way Xenia does, GetProcAddress(module, (LPCSTR)100)
  const k32 = Process.getModuleByName('kernel32.dll');
  const gpa = new NativeFunction(k32.getExportByName('GetProcAddress'), 'pointer', ['pointer', 'pointer']);
  const done = [];
  const rep = (addr, fn, name) => {
    if (addr && !addr.isNull()) {
      Interceptor.replace(addr, fn);
      done.push(name);
    }
  };
  rep(ex('XInputGetCapabilities'), cm.x_getcaps, 'XInputGetCapabilities');
  rep(ex('XInputGetState'), cm.x_getstate, 'XInputGetState');
  rep(gpa(m.base, ptr(100)), cm.x_getstateex, 'XInputGetStateEx(100)');
  rep(ex('XInputGetKeystroke'), cm.x_getkeystroke, 'XInputGetKeystroke');
  rep(ex('XInputSetState'), cm.x_setstate, 'XInputSetState');
  rep(ex('XInputEnable'), cm.x_enable, 'XInputEnable');
  Interceptor.flush();
  hooked = { module: m.path, base: m.base.toString(), replaced: done };
}

let mod = Process.findModuleByName('xinput1_4.dll');
const how = mod ? 'already loaded' : 'loaded by xpad';
if (!mod)
  mod = Module.load('xinput1_4.dll');
hook(mod);
send({ t: 'xpad', what: 'hooked', how: how, module: hooked.module, replaced: hooked.replaced });

function counts() {
  const out = {};
  for (let f = 0; f < FNS.length; f++) {
    for (let u = 0; u < 5; u++) {
      const n = st.add(16 + 4 * (f * 5 + u)).readU32();
      if (n)
        out[FNS[f] + ':' + (u < 4 ? u : 'other')] = n;
    }
  }
  return out;
}

rpc.exports = {
  set(b, lt, rt, lx, ly, rx, ry) {
    const cur = [st.add(4).readU16(), st.add(6).readU8(), st.add(7).readU8(), st.add(8).readS16(),
                 st.add(10).readS16(), st.add(12).readS16(), st.add(14).readS16()];
    const nb = [b & 0xFFFF, lt & 0xFF, rt & 0xFF, lx, ly, rx, ry];
    if (cur.some((v, i) => v !== nb[i])) {
      // the state first, then the packet number (a reader that sees the new packet sees the new state)
      st.add(4).writeU16(nb[0]);
      st.add(6).writeU8(nb[1]);
      st.add(7).writeU8(nb[2]);
      st.add(8).writeS16(nb[3]);
      st.add(10).writeS16(nb[4]);
      st.add(12).writeS16(nb[5]);
      st.add(14).writeS16(nb[6]);
      st.writeU32((st.readU32() + 1) >>> 0);
    }
    return st.readU32();
  },
  stats() {
    return { hooked: hooked, counts: counts(), packet: st.readU32() };
  },
  threads() {
    // a stall diagnostic: every thread's pc + a fuzzy backtrace as module+offset (no dbghelp: nothing that could
    // wait on the loader lock a stalled thread may hold)
    const where = (a) => {
      const m = Process.findModuleByAddress(a);
      return m ? m.name + '+0x' + a.sub(m.base).toString(16) : a.toString();
    };
    return Process.enumerateThreads().map((t) => {
      let bt;
      try {
        bt = Thread.backtrace(t.context, Backtracer.FUZZY).slice(0, 20).map(where);
      } catch (e) {
        bt = ['error ' + e];
      }
      return { id: t.id, state: t.state, pc: where(t.context.pc), bt: bt };
    });
  }
};
