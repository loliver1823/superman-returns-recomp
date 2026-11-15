/*
 * pacing_probe.js - the display.registers timing reference (native/dreg-xenia): ONE guest-pacing instrument for
 * Xenia Canary AND our build. OBSERVATION ONLY: the hooks read guest memory and a clock and write a private buffer;
 * nothing in the target is changed. tools/perf/pacing_probe.py drives it; tools/perf/xenia_pacing_cmp.py grades it.
 *
 * What it records, on both programs, at the same guest point - the ENTRY of the game's present,
 * D3DDeviceSwap_sub_82112050 (every guest present goes through it; docs/ENGINE_MAP.md "Present pacing"):
 *   the QueryPerformanceCounter time, a sequence number, the thread, and the three pacing words of the D3D device
 *   (dev = *(*0x82000948), the words kernel/p2_trace.cpp kind 29 records at VdSwap):
 *     dev+0x3B14 the vblank count (the vblank ISR, source 0), dev+0x3B20 the gate count (82111E38, once per gated
 *     present), dev+0x4E4C the throttle wait snapshot (CPU-speed dependent: information only);
 *   and what the present REQUESTS (dxg1), read, never inferred:
 *     dev+0x33A4 the PresentInterval (RS 376), dev+0x2D3C the word whose bits 23..29 are the late-release pct and
 *     dev+0x3490 the front buffer - the three words GpuPresentEmitCommandBlock_sub_82111EC8 reads inside this present
 *     to build the gate's argument (vblanks << 16 | pct; vblanks = interval <= 1 -> 1, 2 -> 2, 4 -> 3; the gate is
 *     skipped for 0x80000000 IMMEDIATE or a front buffer with no page), and the words our native present gate reads
 *     (kernel/native_flush.cpp presentGatePayload): the same device struct in both programs;
 *     the render loop's period: FGGameRender's GameKernelMgr = *0x8296D8DC, its tick period in ns (i64 at +0x48,
 *     GameKernelMgr_SetTickRate_sub_82720418: 1e9 / rate + 0.5) - the loop that presents once per tick, so a present
 *     waits for max(the interval, the period in vblanks). The CModule reads the period only from a manager the JS
 *     side has seen readable (cfg.rmgr; renderPoll every 250 ms, a {t:'render-mgr'} marker when it changes).
 *   and the gate's RELEASE state (round 2, Astra round 1 P1: what the gate did with the earlier presents' requests -
 *   the release itself happens after this present's entry, in the vblank ISR or in the gate callback): dev+0x3B18
 *   the vblank count at the LAST RELEASE (82111E38 and the vblank ISR 820FDBA0 source 0 store +0x3B18 = +0x3B14 when
 *   they release - in Xenia, and in our host gate kernel/native_vblank.cpp, which mirrors them) and dev+0x3B1C the
 *   countdown (> 0: a release pending; 0: released at a vblank; < 0: released at the callback, the NOW outcome). The
 *   four gate words (+0x3B14, +0x3B20, +0x3B18, +0x3B1C) are read twice; `torn` = they changed between the reads.
 *   AptLoadByPath_sub_826D2690 'screens/...' requests -> {t:'apt', path} (the era markers: main_hud = the tape
 *   anchor, the second screens/misc/ScrPauseMenu = the pause hold the pacing window is cut from).
 *
 * How it finds the guest:
 *   Xenia (init({side:'xenia'})): guest memory at 0x100000000 (VA >= 0xE0000000: +0x1000 host offset); guest VA G's
 *     JIT code pointer is the u32 at host address G once G is compiled (tools/XENIA_HARNESS.md); an uncompiled slot
 *     holds the resolve thunk (the modal slot value, as present_probe.js), so a hook attaches after the first call;
 *     the PPCContext is the register whose +0xA98 holds the membase (present_probe.js findCtx), r[] at +0x28.
 *   ours (init({side:'ours', module})): the exports g_debugGuestBase (the guest base), g_debugDispatchTable /
 *     g_debugDispatchCodeBase (slot i = the host function of guest VA base + 4i - the function every recompiled
 *     caller calls, the override when there is one) and g_debugCtxGprOff (r3's offset in the PPCContext, which is
 *     the first argument, rcx; 0 in our build) with g_debugCtxSize (non-zero once RunGuest has published both).
 *     The present hook is the dispatch slot of 82112050 = kernel/hle_api_observe.cpp's strong-def, which runs the
 *     native swap (native_flush swapLeaf, its only call site) or the SDK body: every guest present enters it.
 * The swap hook is attached only once the device chain reads (cell -> dev, dev's page readable), so the CModule
 * never reads an unmapped guest address.
 *
 * Ring: 256K records x 72 B {i64 qpc, u32 seq, u32 tid, u32 vblanks, u32 gates, u32 wait, u32 dev, u32 interval,
 * u32 pacing, u32 front, u32 rmgr, u64 rperiod, u32 last, u32 countdown, u32 torn, u32 done} (rperiod 0 = the manager
 * not seen readable yet, or none). `done` = the slot's index + 1, stored LAST with a barrier (round 2, Astra round 1
 * P2-3: the counter is taken before the slot is filled, so a reader takes a slot only once its done says it is
 * complete - never by elapsed time). rpc.exports.read(start, n) hands records back.
 */
'use strict';

const CAP = 1 << 18;
const REC = 72;
const ring = Memory.alloc(CAP * REC);
const counter = Memory.alloc(64);
counter.writeS32(0);
const cfg = Memory.alloc(16);   // {u8 *base; u32 xenia; u32 rmgr (the render manager the JS side saw readable)}
const CFG_RMGR = Process.pointerSize + 4;
const k32 = Process.getModuleByName('kernel32.dll');
const qpcAddr = k32.getExportByName('QueryPerformanceCounter');
const QPC = new NativeFunction(qpcAddr, 'int', ['pointer']);
const QPF = new NativeFunction(k32.getExportByName('QueryPerformanceFrequency'), 'int', ['pointer']);
const qbuf = Memory.alloc(8);
QPF(qbuf);
const QPF_HZ = qbuf.readU64().toNumber();
function qpcNow() { QPC(qbuf); return qbuf.readU64().toString(); }

const GUEST_SWAP = 0x82112050;
const GUEST_APT = 0x826D2690;
const VD_DEVICE_CELL = 0x82000948;
const DEV_VBLANKS = 0x3B14, DEV_GATES = 0x3B20, DEV_WAIT = 0x4E4C;
const DEV_INTERVAL = 0x33A4, DEV_PACING = 0x2D3C, DEV_FRONT = 0x3490;
const DEV_LAST = 0x3B18, DEV_COUNTDOWN = 0x3B1C;
const RENDER_CELL = 0x8296D8DC, MGR_RATE = 0x3C, MGR_PERIOD = 0x48;

const cm = new CModule(`
#include <gum/guminterceptor.h>

typedef struct { gint64 t; guint32 seq; guint32 tid; guint32 vbl; guint32 gates; guint32 wait; guint32 dev;
                 guint32 ival; guint32 pacing; guint32 front; guint32 rmgr; guint64 rperiod;
                 guint32 last; guint32 countdown; guint32 torn; volatile guint32 done; } Rec;
typedef struct { guint8 * base; guint32 xenia; volatile guint32 rmgr; } Cfg;
extern Rec ring[];
extern volatile gint counter;
extern Cfg cfg;
extern int QueryPerformanceCounter (gint64 * t);

static guint32
be32 (guint32 va)
{
  guint8 * p = cfg.base + va;
  guint32 v;
  if (cfg.xenia && va >= 0xE0000000u)
    p += 0x1000;
  v = *(volatile guint32 *) p;   /* one aligned load: never torn against the ISR's store */
  return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

void
onEnter (GumInvocationContext * ic)
{
  gint64 t;
  gint i;
  Rec * r;
  guint32 cell, dev, rm;
  QueryPerformanceCounter (&t);
  i = g_atomic_int_add (&counter, 1);
  r = &ring[i & (${CAP} - 1)];
  r->t = t;
  r->seq = (guint32) i;
  r->tid = gum_invocation_context_get_thread_id (ic);
  cell = be32 (${VD_DEVICE_CELL}u);
  dev = cell ? be32 (cell) : 0;
  r->dev = dev;
  r->vbl = r->gates = r->last = r->countdown = 0;
  r->torn = 0;
  if (dev)
  {
    /* the gate's four words, twice: torn = the vblank ISR or the gate callback stored between the two reads */
    guint32 v1 = be32 (dev + ${DEV_VBLANKS}u), g1 = be32 (dev + ${DEV_GATES}u);
    guint32 l1 = be32 (dev + ${DEV_LAST}u), c1 = be32 (dev + ${DEV_COUNTDOWN}u);
    guint32 v2 = be32 (dev + ${DEV_VBLANKS}u), g2 = be32 (dev + ${DEV_GATES}u);
    guint32 l2 = be32 (dev + ${DEV_LAST}u), c2 = be32 (dev + ${DEV_COUNTDOWN}u);
    r->vbl = v2;
    r->gates = g2;
    r->last = l2;
    r->countdown = c2;
    r->torn = (v1 != v2 || g1 != g2 || l1 != l2 || c1 != c2) ? 1u : 0u;
  }
  r->wait = dev ? be32 (dev + ${DEV_WAIT}u) : 0;
  r->ival = dev ? be32 (dev + ${DEV_INTERVAL}u) : 0;
  r->pacing = dev ? be32 (dev + ${DEV_PACING}u) : 0;
  r->front = dev ? be32 (dev + ${DEV_FRONT}u) : 0;
  rm = be32 (${RENDER_CELL}u);   /* the image's data section: always mapped */
  r->rmgr = rm;
  /* only a manager the JS side has seen readable: a new one reads 0 until renderPoll has checked it */
  r->rperiod = (rm != 0 && rm == cfg.rmgr)
      ? (((guint64) be32 (rm + ${MGR_PERIOD}u)) << 32) | be32 (rm + ${MGR_PERIOD}u + 4u) : 0;
  /* the slot is complete: published last, with a full barrier (a reader takes slot k only when done == k + 1). The
     CModule runtime has g_atomic_int_add, not _set: one locked add moves done from its old value to i + 1 (each slot
     has one writer at a time - its index's) */
  g_atomic_int_add ((volatile gint *) &r->done, (gint) ((guint32) (i + 1) - r->done));
}
`, { ring: ring, counter: counter, cfg: cfg, QueryPerformanceCounter: qpcAddr });

let SIDE = null, MEMBASE = null, THUNK = null;
let TABLE = null, CODE_BASE = 0, ENTRIES = 0, R3_OFF = null;
const attached = {};
let timer = null;

function hostAddr(va) {
  va = va >>> 0;
  const p = MEMBASE.add(va);
  return (SIDE === 'xenia' && va >= 0xE0000000) ? p.add(0x1000) : p;
}
function readable(p, n) {
  const r = Process.findRangeByAddress(p);
  return r !== null && r.protection.charAt(0) === 'r' && p.add(n).compare(r.base.add(r.size)) <= 0;
}
function be32(va) {
  const p = hostAddr(va);
  if (!readable(p, 4)) return null;
  const b = new Uint8Array(p.readByteArray(4));
  return ((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]) >>> 0;
}
function cstr(va, max) {
  const u = new Uint8Array(hostAddr(va).readByteArray(max));
  let s = '';
  for (let i = 0; i < max && u[i]; i++) s += String.fromCharCode(u[i]);
  return s;
}
// the device chain, read by JS before the swap hook goes on: null until it reads
function deviceChain() {
  const cell = be32(VD_DEVICE_CELL);
  if (!cell) return null;
  const dev = be32(cell);
  if (!dev) return null;
  for (const off of [DEV_PACING, DEV_INTERVAL, DEV_FRONT, DEV_VBLANKS, DEV_LAST, DEV_COUNTDOWN, DEV_GATES, DEV_WAIT]) {
    if (be32(dev + off) === null) return null;
  }
  return { cell: cell, dev: dev };
}

// the render loop's manager: the CModule reads its period only once this has seen the manager readable
let renderMgr = 0, renderTimer = null;
const f32 = new DataView(new ArrayBuffer(4));
function renderPoll() {
  let rm = null;
  try { rm = be32(RENDER_CELL); } catch (e) { return; }
  if (rm === null || rm === renderMgr) return;
  if (rm !== 0 && !readable(hostAddr(rm + MGR_RATE), MGR_PERIOD + 8 - MGR_RATE)) return;   // retried next poll
  let period = null, rate = null;
  if (rm !== 0) {
    period = be32(rm + MGR_PERIOD) * 4294967296 + be32(rm + MGR_PERIOD + 4);
    f32.setUint32(0, be32(rm + MGR_RATE));
    rate = f32.getFloat32(0);
  }
  cfg.add(CFG_RMGR).writeU32(rm);
  renderMgr = rm;
  send({ t: 'render-mgr', mgr: '0x' + rm.toString(16), period_ns: period, rate: rate, qpc: qpcNow() });
}

// ---- Xenia: the JIT indirection table
function findThunk() {   // the resolve thunk = the most common slot value over the code range (present_probe.js)
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
function xeniaCtx(cpu) {
  for (const c of [cpu.rsi, cpu.rdi, cpu.rbx, cpu.r12, cpu.r13, cpu.r14, cpu.r15, cpu.rbp]) {
    try { if (c.add(0xA98).readU64().toString() === '4294967296') return c; } catch (e) { }
  }
  return null;
}

// the host code of a guest function, or null while it is not there yet
function hostFn(va) {
  if (SIDE === 'xenia') {
    if (THUNK === null) { findThunk(); if (THUNK === null) return null; }
    let h = 0;
    try { h = ptr(va).readU32() >>> 0; } catch (e) { return null; }
    if (h < 0xA0000000 || h >= 0xB0000000 || h === THUNK) return null;   // not compiled yet
    return ptr(h);
  }
  const slot = (va - CODE_BASE) / 4;
  if (slot < 0 || slot >= ENTRIES) return null;
  const p = TABLE.add(slot * Process.pointerSize).readPointer();
  return p.isNull() ? null : p;
}
// guest r3 at a hooked function's entry
function r3Of(ic) {
  if (SIDE === 'xenia') {
    const ctx = xeniaCtx(ic.context);
    return ctx === null ? null : (ctx.add(0x28 + 8 * 3).readU64().and(0xFFFFFFFF).toNumber() >>> 0);
  }
  return ic.context.rcx.add(R3_OFF).readU64().and(0xFFFFFFFF).toNumber() >>> 0;
}

const aptCb = {
  onEnter() {
    let p = null;
    try { p = r3Of(this); } catch (e) { return; }
    if (p === null || p < 0x10000 || p >= 0xA0000000) return;
    let s = '';
    try { s = cstr(p, 96); } catch (e) { return; }
    if (s.indexOf('screens/') !== 0) return;
    send({ t: 'apt', path: s, qpc: qpcNow() });
  }
};

function poll() {
  if (!attached[GUEST_APT]) {
    const h = hostFn(GUEST_APT);
    if (h !== null) {
      try {
        Interceptor.attach(h, aptCb);
        attached[GUEST_APT] = { host: h.toString(), qpc: qpcNow() };
        send({ t: 'attached', guest: '0x' + GUEST_APT.toString(16), host: h.toString(), qpc: qpcNow() });
      } catch (e) {
        attached[GUEST_APT] = { error: String(e) };
        send({ t: 'attach-error', guest: '0x' + GUEST_APT.toString(16), err: String(e) });
      }
    }
  }
  if (!attached[GUEST_SWAP]) {
    const h = hostFn(GUEST_SWAP);
    const chain = h === null ? null : deviceChain();
    if (h !== null && chain !== null) {
      try {
        Interceptor.attach(h, cm);
        attached[GUEST_SWAP] = { host: h.toString(), qpc: qpcNow(), cell: '0x' + chain.cell.toString(16),
                                 dev: '0x' + chain.dev.toString(16) };
        send({ t: 'attached', guest: '0x' + GUEST_SWAP.toString(16), host: h.toString(), qpc: qpcNow(),
               cell: '0x' + chain.cell.toString(16), dev: '0x' + chain.dev.toString(16) });
      } catch (e) {
        attached[GUEST_SWAP] = { error: String(e) };
        send({ t: 'attach-error', guest: '0x' + GUEST_SWAP.toString(16), err: String(e) });
      }
    }
  }
  if (attached[GUEST_APT] && attached[GUEST_SWAP] && timer !== null) {
    clearInterval(timer);
    timer = null;
  }
}

function initOurs(module, symbols) {
  // symbols: TEST ONLY (tools/perf/pacing_probe_selftest.py's fake host) - the variables' addresses by name
  const mod = symbols ? null : Process.getModuleByName(module);
  const sym = function (n) {
    const p = symbols ? (symbols[n] ? ptr(symbols[n]) : null) : mod.findExportByName(n);
    if (p === null) throw new Error(n + ' export not found');
    return p;
  };
  const base = sym('g_debugGuestBase').readPointer();
  const table = sym('g_debugDispatchTable').readPointer();
  if (base.isNull()) return { error: 'g_debugGuestBase is null (guest memory not up yet)' };
  if (table.isNull()) return { error: 'g_debugDispatchTable is null (dispatch table not built yet)' };
  // RunGuest (guest/guest_harness.cpp) publishes g_debugCtxSize and g_debugCtxGprOff[] together. The size is the
  // "filled" test, never r3's own offset: XenonRecomp's PPCContext puts r3 FIRST (offset 0 - the PDB of exe
  // 0360f8ed9ed7: r3 0, r0 8, r1 16, sizeof 2688), so a zero r3 offset is the filled value (dxp0's NO_DATA).
  const size = sym('g_debugCtxSize').readU32();
  if (size === 0) return { error: 'g_debugCtxSize is 0 (RunGuest has not published the context offsets yet)' };
  const r3 = sym('g_debugCtxGprOff').add(4 * 3).readU32();
  if (r3 + 8 > size) return { error: 'g_debugCtxGprOff[3] = ' + r3 + ' lies outside the context (size ' + size + ')' };
  MEMBASE = base;
  TABLE = table;
  CODE_BASE = sym('g_debugDispatchCodeBase').readU32() >>> 0;
  ENTRIES = sym('g_debugDispatchTableEntries').readU64().toNumber();
  R3_OFF = r3;
  return null;
}

rpc.exports = {
  // {side: 'xenia'} | {side: 'ours', module: 'SupermanReturns.exe'}; returns {error} until the guest is up
  init(opts) {
    if (SIDE !== null) return { ok: true, again: true };
    const side = opts && opts.side;
    if (side === 'xenia') {
      MEMBASE = ptr('0x100000000');
    } else if (side === 'ours') {
      let err = null;
      try { err = initOurs(opts.module || 'SupermanReturns.exe', opts.symbols || null); } catch (e) { err = { error: String(e) }; }
      if (err !== null) return err;
    } else {
      return { error: 'side must be xenia or ours' };
    }
    SIDE = side;
    let ok = false;
    try { ok = readable(hostAddr(0x82000000), 2) && hostAddr(0x82000000).readU16() === 0x5A4D; } catch (e) { }
    if (!ok) {
      SIDE = null;
      return { error: 'no guest image at the guest base + 0x82000000 (not booted yet)' };
    }
    cfg.writePointer(MEMBASE);
    cfg.add(Process.pointerSize).writeU32(side === 'xenia' ? 1 : 0);
    cfg.add(CFG_RMGR).writeU32(0);
    renderPoll();   // before the swap hook can attach
    renderTimer = setInterval(renderPoll, 250);
    timer = setInterval(poll, 50);
    poll();
    const swapHost = side === 'ours' ? hostFn(GUEST_SWAP) : null;   // ours: the strong-def's address (no thunk)
    return { ok: true, side: side, membase: MEMBASE.toString(), qpf: QPF_HZ, cap: CAP, rec: REC, qpc: qpcNow(),
             code_base: CODE_BASE, entries: ENTRIES, r3_off: R3_OFF,
             swap_host: swapHost === null ? null : swapHost.toString() };
  },
  count() { return counter.readS32(); },
  qpc() { return qpcNow(); },
  info() { return { qpf: QPF_HZ, cap: CAP, rec: REC }; },
  // n records from index `start` (wrapping), raw
  read(start, n) {
    const s = start & (CAP - 1);
    if (s + n <= CAP) return ring.add(s * REC).readByteArray(n * REC);
    const a = new Uint8Array(ring.add(s * REC).readByteArray((CAP - s) * REC));
    const b = new Uint8Array(ring.readByteArray((n - (CAP - s)) * REC));
    const out = new Uint8Array(n * REC);
    out.set(a, 0);
    out.set(b, a.length);
    return out.buffer;
  },
  status() {
    let chain = null;
    try { chain = SIDE === null ? null : deviceChain(); } catch (e) { }
    return { count: counter.readS32(), side: SIDE, attached: attached, thunk: THUNK,
             chain: chain === null ? null : { cell: '0x' + chain.cell.toString(16), dev: '0x' + chain.dev.toString(16) },
             render_mgr: '0x' + renderMgr.toString(16) };
  },
};
