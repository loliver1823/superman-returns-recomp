/*
 * xenia_audio_capture.js - iter-467 sound lane: the XENIA (oracle) side of the audio A/B.
 * OBSERVATION ONLY: hooks read guest memory and write a file; nothing in the guest or in Xenia is
 * written.
 *
 * Capture point = the same frame our build hands to XAudioSubmitRenderDriverFrame (ENGINE_MAP
 * 7a.1): XAudioMasterVoice_MixAndSubmitFrame_sub_82120990 calls the render client's vtbl+0x18 =
 * 0x8211B680 (bctrl), which does
 *     r11 = r4 ? r4 - 8 : 0;  r4 = *(r11 + 0x1C);  r3 = *(r3 + 0x10);  b XAudioSubmitRenderDriverFrame
 * so at the entry of 0x8211B680 the mixed frame (256 samples x 6 channels planar big-endian
 * float, 6144 bytes) is at BE32(r4 + 0x14). Xenia's indirection-table slot for guest VA G is the
 * u32 at host G (tools/XENIA_HARNESS.md); the hook is attached once the slot holds compiled code.
 *
 * Output (rpc.exports.start(path)): records of
 *     u32 magic 'AFR1' (0x31524641 LE) | u32 seq | u64 t_us (unix epoch microseconds, QPC based)
 *     | 6144 bytes guest frame (raw, big-endian planar)
 * - the SAME format as our build's SR_AUDIO_FRAMES dump (kernel/apu/audio_system.cpp), so
 * tools/audio_oracle_compare.py reads both.
 * Markers (send): {t:'apt', path, t_us} for every AptLoadByPath (0x826D2690) - era boundaries
 * (screens/hud/main_hud = the tape anchor our build also uses) - and {t:'anchor'} on main_hud.
 */
'use strict';

const MEMBASE = ptr('0x100000000');
const RB = 0x28;   // PPCContext r[] base (XENIA_HARNESS.md, measured)
const FRAME_BYTES = 6144;

const k32 = Process.getModuleByName('kernel32.dll');
const QPC = new NativeFunction(k32.getExportByName('QueryPerformanceCounter'), 'int', ['pointer']);
const QPF = new NativeFunction(k32.getExportByName('QueryPerformanceFrequency'), 'int', ['pointer']);
const qbuf = Memory.alloc(8);
QPF(qbuf);
const qpf = qbuf.readU64().toNumber();
QPC(qbuf);
const qpc0 = qbuf.readU64().toNumber();
const epochUs0 = Date.now() * 1000;
function nowUs() {
  QPC(qbuf);
  const d = qbuf.readU64().toNumber() - qpc0;
  return epochUs0 + Math.floor(d * 1e6 / qpf);
}

function be32(va) {
  const u = new Uint8Array(MEMBASE.add(va >>> 0).readByteArray(4));
  return ((u[0] << 24) | (u[1] << 16) | (u[2] << 8) | u[3]) >>> 0;
}
function be8(va) { return MEMBASE.add(va >>> 0).readU8(); }
function plausible(va) { return va >= 0x10000 && va < 0xA0000000; }
function findCtx(cpu) {
  for (const c of [cpu.rsi, cpu.rdi, cpu.rbx, cpu.r12, cpu.r13, cpu.r14, cpu.r15, cpu.rbp]) {
    try { if (c.add(0xA98).readU64().toString() === '4294967296') return c; } catch (e) { }
  }
  return null;
}
function greg(ctx, n) { return ctx.add(RB + 8 * n).readU64().and(0xFFFFFFFF).toNumber() >>> 0; }
function cstr(va, max) {   // one read, not one per byte (the JS lock is shared with the audio hook)
  const u = new Uint8Array(MEMBASE.add(va >>> 0).readByteArray(max));
  let s = '';
  for (let i = 0; i < max && u[i]; i++) s += String.fromCharCode(u[i]);
  return s;
}

let file = null, seq = 0, bad = 0, lastSumUs = 0, sumFrames = 0;
const hdr = new ArrayBuffer(16);
const hdv = new DataView(hdr);

const hooks = {
  0x8211B680: {
    onEnter() {
      if (!file) return;
      const ctx = findCtx(this.context);
      if (!ctx) { bad++; return; }
      const r4 = greg(ctx, 4);
      const base = r4 ? ((r4 - 8) >>> 0) : 0;
      let samples = 0;
      try { samples = be32(base + 0x1C); } catch (e) { bad++; return; }
      if (!plausible(samples) && !(samples >= 0xA0000000 && samples < 0xFFFF0000)) { bad++; return; }
      let data;
      try { data = MEMBASE.add(samples >>> 0).readByteArray(FRAME_BYTES); } catch (e) { bad++; return; }
      const t = nowUs();
      hdv.setUint32(0, 0x31524641, true);
      hdv.setUint32(4, seq >>> 0, true);
      hdv.setUint32(8, t % 4294967296, true);
      hdv.setUint32(12, Math.floor(t / 4294967296), true);
      file.write(hdr);
      file.write(data);
      seq++; sumFrames++;
      if (t - lastSumUs >= 5000000) {
        // no flush here: this runs on Xenia's audio worker (a flush stalled it up to ~0.5 s in x2)
        if (lastSumUs) send({ t: 'sum', t_us: t, frames: seq, bad: bad, rate: sumFrames * 1e6 / (t - lastSumUs) });
        lastSumUs = t; sumFrames = 0;
      }
    }
  },
  0x826D2690: {
    onEnter() {
      const ctx = findCtx(this.context); if (!ctx) return;
      const p = greg(ctx, 3);
      if (!plausible(p)) return;
      const s = cstr(p, 96);
      if (s.indexOf('screens/') !== 0) return;   // era markers only (keep the shared JS lock short)
      send({ t: 'apt', path: s, t_us: nowUs(), seq: seq });
      if (s === 'screens/hud/main_hud') send({ t: 'anchor', t_us: nowUs(), seq: seq });
    }
  },
};

// The slot value of a guest function that is not compiled yet: the shared resolve thunk. In the
// oracle build it sits INSIDE the generated-code region (0xA0000220 on 2026-10-07), so the old
// "host >= 0xA0000000 = compiled" test attached the hook to the thunk itself (every unresolved
// call). Take the most common slot value over the image's code range = the thunk.
let THUNK = null;
function findThunk() {
  const counts = {};
  for (let va = 0x82100000; va < 0x82800000; va += 0x2000) {
    let v = 0;
    try { v = ptr(va).readU32() >>> 0; } catch (e) { continue; }
    counts[v] = (counts[v] || 0) + 1;
  }
  let best = null, n = 0;
  for (const k of Object.keys(counts)) if (counts[k] > n) { n = counts[k]; best = parseInt(k, 10); }
  if (n >= 100) { THUNK = best; send({ t: 'thunk', host: '0x' + best.toString(16), n: n }); }
}
const attached = {};
function poll() {
  if (THUNK === null) { findThunk(); if (THUNK === null) return; }
  for (const k of Object.keys(hooks)) {
    if (attached[k]) continue;
    const va = parseInt(k, 10);
    let host = 0;
    try { host = ptr(va).readU32() >>> 0; } catch (e) { continue; }
    if (host < 0xA0000000 || host === THUNK) continue;   // not compiled yet (resolve thunk)
    try {
      Interceptor.attach(ptr(host), hooks[k]);
      attached[k] = true;
      send({ t: 'attached', guest: '0x' + va.toString(16), host: '0x' + host.toString(16), t_us: nowUs() });
    } catch (e) { send({ t: 'attach-error', guest: '0x' + va.toString(16), err: String(e) }); attached[k] = true; }
  }
}
const pollTimer = setInterval(function () {
  poll();
  if (Object.keys(attached).length === Object.keys(hooks).length) clearInterval(pollTimer);
}, 100);

rpc.exports = {
  start(path) { file = new File(path, 'wb'); send({ t: 'started', path: path, t_us: nowUs() }); poll(); return 'ok'; },
  stop() { if (file) { file.flush(); file.close(); file = null; } return { frames: seq, bad: bad }; },
  stats() { return { frames: seq, bad: bad, attached: Object.keys(attached) }; },
};
