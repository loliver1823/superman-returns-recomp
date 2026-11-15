/*
 * present_probe.js - canary lane (iter-467): one present-timestamp instrument for ANY D3D12 / DXGI
 * process (our build, Xenia Canary, the calibration app). OBSERVATION ONLY: the hooks read a clock
 * and write a private buffer; nothing in the target is changed.
 *
 * What it measures: the QueryPerformanceCounter time of every present SUBMISSION the process makes
 * to the kernel graphics driver - the win32u syscall stubs DXGI uses for a present (NtGdiDdDDIPresent,
 * the multi-plane-overlay variants, NtDxgkSubmitPresent[Blt]ToHwQueue, ...). Every IDXGISwapChain::
 * Present that reaches the screen goes through one of them, whoever the app is, so both programs are
 * timed by the same code at the same layer (the PresentMon "between presents" idea, without ETW,
 * which needs an elevated session). tools/perf/canary_compare.py `selftest` checks it against an app
 * that logs its own Present calls.
 *
 * Optional guest hooks for Xenia (init({xenia: true})): Xenia JIT-compiles each guest PPC function
 * once; guest VA G's host code pointer is the u32 at host address G (tools/XENIA_HARNESS.md). Then
 *   0x82112050 D3DDeviceSwap (every guest present: render, movie blit-and-swap, persist re-swap;
 *              docs/HIGH_FRAMERATE.md 2.3) -> event id 100 = one game frame;
 *   0x826D2690 AptLoadByPath -> {t:'apt', path} for 'screens/...' requests (the era markers our
 *              build logs as [APT_SCREEN] / [APT_REQ]; 'screens/hud/main_hud' = the tape anchor).
 *
 * Ring: 1M entries x 16 B {i64 qpc, u32 id, u32 tid}; rpc.exports.read(start, n) hands entries back.
 */
'use strict';

const CAP = 1 << 20;
const ring = Memory.alloc(CAP * 16);
const counter = Memory.alloc(64);
counter.writeS32(0);
const k32 = Process.getModuleByName('kernel32.dll');
const qpcAddr = k32.getExportByName('QueryPerformanceCounter');
const QPC = new NativeFunction(qpcAddr, 'int', ['pointer']);
const QPF = new NativeFunction(k32.getExportByName('QueryPerformanceFrequency'), 'int', ['pointer']);
const qbuf = Memory.alloc(8);
QPF(qbuf);
const QPF_HZ = qbuf.readU64().toNumber();
function qpcNow() { QPC(qbuf); return qbuf.readU64().toString(); }

const cm = new CModule(`
#include <gum/guminterceptor.h>

typedef struct { gint64 t; guint32 id; guint32 tid; } Ev;
extern Ev ring[];
extern volatile gint counter;
extern int QueryPerformanceCounter (gint64 * t);

void
onEnter (GumInvocationContext * ic)
{
  gint64 t;
  guint id;
  gint i;
  Ev * e;
  QueryPerformanceCounter (&t);
  id = GPOINTER_TO_UINT (gum_invocation_context_get_listener_function_data (ic));
  i = g_atomic_int_add (&counter, 1);
  e = &ring[i & (${CAP} - 1)];
  e->t = t;
  e->tid = gum_invocation_context_get_thread_id (ic);
  e->id = id;
}
`, { ring: ring, counter: counter, QueryPerformanceCounter: qpcAddr });

// The kernel present submissions (win32u syscall stubs). Ids 1..N, stable across runs.
const PRESENT_FNS = [
  'NtGdiDdDDIPresent',
  'NtGdiDdDDIPresentMultiPlaneOverlay',
  'NtGdiDdDDIPresentMultiPlaneOverlay2',
  'NtGdiDdDDIPresentMultiPlaneOverlay3',
  'NtGdiDdDDIPresentRedirected',
  'NtDxgkSubmitPresentToHwQueue',
  'NtDxgkSubmitPresentBltToHwQueue',
  'NtGdiDdDDIUnOrderedPresentSwapChain',
];
const hooked = {};
const w32u = Process.getModuleByName('win32u.dll');
PRESENT_FNS.forEach(function (name, k) {
  const id = k + 1;
  let addr = null;
  try { addr = w32u.getExportByName(name); } catch (e) { hooked[name] = { id: id, error: 'no export' }; return; }
  try {
    Interceptor.attach(addr, cm, ptr(id));
    hooked[name] = { id: id, addr: addr.toString() };
  } catch (e) { hooked[name] = { id: id, error: String(e) }; }
});

// ---------------------------------------------------------------- Xenia guest hooks (optional)
const MEMBASE = ptr('0x100000000');
const RB = 0x28;   // PPCContext r[] base (XENIA_HARNESS.md, measured)
const GUEST_SWAP = 0x82112050;
const GUEST_APT = 0x826D2690;
function findCtx(cpu) {
  for (const c of [cpu.rsi, cpu.rdi, cpu.rbx, cpu.r12, cpu.r13, cpu.r14, cpu.r15, cpu.rbp]) {
    try { if (c.add(0xA98).readU64().toString() === '4294967296') return c; } catch (e) { }
  }
  return null;
}
function greg(ctx, n) { return ctx.add(RB + 8 * n).readU64().and(0xFFFFFFFF).toNumber() >>> 0; }
function cstr(va, max) {
  const u = new Uint8Array(MEMBASE.add(va >>> 0).readByteArray(max));
  let s = '';
  for (let i = 0; i < max && u[i]; i++) s += String.fromCharCode(u[i]);
  return s;
}
let THUNK = null;
function findThunk() {   // the shared resolve thunk = the most common slot value over the code range
  const counts = {};
  for (let va = 0x82100000; va < 0x82800000; va += 0x2000) {
    let v = 0;
    try { v = ptr(va).readU32() >>> 0; } catch (e) { continue; }
    counts[v] = (counts[v] || 0) + 1;
  }
  let best = null, n = 0;
  for (const k of Object.keys(counts)) if (counts[k] > n) { n = counts[k]; best = parseInt(k, 10); }
  if (n >= 100) { THUNK = best; send({ t: 'thunk', host: '0x' + best.toString(16), n: n, qpc: qpcNow() }); }
}
// 0x82476760: the movie pipeline's deleting destructor (virtual), called when a movie ends; our
// build logs the same call as [MPDTOR] (kernel/cs_timeout_underflow_fix.cpp). The intro movies
// call it at boot, so it is compiled (and hooked) before the opening movie ends - a hook can only
// attach AFTER a function's first call, which rules out functions first called at the moment of
// interest (e.g. the FE show-screen action 0x82280760, first called when the HUD comes up).
const GUEST_MOVIE_DTOR = 0x82476760;
const guestHooks = {};
guestHooks[GUEST_SWAP] = { kind: 'cm', id: 100 };
guestHooks[GUEST_MOVIE_DTOR] = {
  kind: 'js', cb: { onEnter() { send({ t: 'movie_end', qpc: qpcNow() }); } }
};
guestHooks[GUEST_APT] = {
  kind: 'js', cb: {
    onEnter() {
      const ctx = findCtx(this.context); if (!ctx) return;
      const p = greg(ctx, 3);
      if (p < 0x10000 || p >= 0xA0000000) return;
      let s = '';
      try { s = cstr(p, 96); } catch (e) { return; }
      if (s.indexOf('screens/') !== 0) return;
      send({ t: 'apt', path: s, qpc: qpcNow() });
    }
  }
};
const guestAttached = {};
let guestTimer = null;
function guestPoll() {
  if (THUNK === null) { findThunk(); if (THUNK === null) return; }
  for (const k of Object.keys(guestHooks)) {
    if (guestAttached[k]) continue;
    const va = parseInt(k, 10);
    let host = 0;
    try { host = ptr(va).readU32() >>> 0; } catch (e) { continue; }
    if (host < 0xA0000000 || host === THUNK) continue;   // not compiled yet
    const h = guestHooks[k];
    try {
      if (h.kind === 'cm') Interceptor.attach(ptr(host), cm, ptr(h.id));
      else Interceptor.attach(ptr(host), h.cb);
      guestAttached[k] = { host: '0x' + host.toString(16), qpc: qpcNow() };
      send({ t: 'attached', guest: '0x' + va.toString(16), host: '0x' + host.toString(16), qpc: qpcNow() });
    } catch (e) {
      guestAttached[k] = { error: String(e) };
      send({ t: 'attach-error', guest: '0x' + va.toString(16), err: String(e) });
    }
  }
  if (Object.keys(guestAttached).length === Object.keys(guestHooks).length && guestTimer !== null) {
    clearInterval(guestTimer);
    guestTimer = null;
  }
}

rpc.exports = {
  init(opts) {
    if (opts && opts.xenia) {
      let ok = false;
      try { ok = MEMBASE.add(0x82000000).readU16() === 0x5A4D; } catch (e) { }
      if (!ok) return { error: 'no guest image at membase+0x82000000 (not Xenia, or not booted yet)' };
      guestTimer = setInterval(guestPoll, 50);
      guestPoll();
    }
    return { qpf: QPF_HZ, cap: CAP, hooked: hooked, qpc: qpcNow() };
  },
  count() { return counter.readS32(); },
  qpc() { return qpcNow(); },
  // n entries from index `start` (wrapping), raw {i64 qpc, u32 id, u32 tid}
  read(start, n) {
    const s = start & (CAP - 1);
    if (s + n <= CAP) return ring.add(s * 16).readByteArray(n * 16);
    const a = new Uint8Array(ring.add(s * 16).readByteArray((CAP - s) * 16));
    const b = new Uint8Array(ring.readByteArray((n - (CAP - s)) * 16));
    const out = new Uint8Array(n * 16);
    out.set(a, 0);
    out.set(b, a.length);
    return out.buffer;
  },
  status() { return { count: counter.readS32(), hooked: hooked, guest: guestAttached, thunk: THUNK }; },
};
