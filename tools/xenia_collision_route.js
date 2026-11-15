/*
 * xenia_collision_route.js - iter-463 collision fixed-route test, the XENIA (oracle) side of
 * archive/v1_invest/collision/prep/collision_route_watch.cpp. OBSERVATION ONLY: every hook reads
 * guest memory and sends a message; nothing in the guest or in Xenia is written.
 *
 * Hooks JIT'd guest functions through Xenia's indirection table (tools/XENIA_HARNESS.md: the slot
 * for guest VA G is the u32 at host address G; compiled code lives at >= 0xA0000000). A function
 * is attached once its slot holds compiled code (polled every 250 ms).
 *   0x826D2690 AptLoadByPath(path r3)       -> {t:'anchor'} on "screens/hud/main_hud" (the same
 *                                              anchor our input tapes use, kernel/fe_flow_watch.cpp)
 *   0x821C0C58 MoveTowardIdealPos(PRXY r3, entity r4, dt f1) -> per-tick {t:'tick'} near the
 *              target, 1 Hz {t:'sec'} summaries, {t:'teleport'} (entity pos at entry != last out)
 *              + {t:'path'} every tick while speed > 20 m/s (and the first slow tick after), with
 *              the manifold / overlap-list bodies, and {t:'body'} at a body's first sighting
 *              (the same evidence as kernel/collision_watch.cpp [COLL_PATH]/[COLL_BODY];
 *              tools/collision_route_analyze.py --xenia reads it)
 *   0x821AA218 actor SetPosition, 0x821E3CF8 actor SetTransform (pos = r4+16), 0x821AE1C8
 *   SetPhysRepMode, 0x821AE388 Deactivate, 0x821AE218 SetFilterInfo -> {t:'event'} for Superman.
 *              SETPOS from INSIDE MoveTowardIdealPos (the mover's own write-back, 30/s) is
 *              skipped (verifier correction 5): a per-thread depth counter is raised in the
 *              mover's onEnter and lowered in its onLeave.
 *   0x82596508 hkWorld::addEntity(world r3, entity r4), 0x825967C0 hkWorld::removeEntity(result
 *              r3, world r4, entity r5) -> {t:'world'} for every layer-7 body and any body within
 *              300 m of Superman (lock = world+0x8C; a locked world queues the op)
 * Superman = the actor (vt 0x82009798) whose FSM handler name (actor+0x84 -> +0x24) starts
 * "SUPERMAN". Target body = the hkRigidBody whose transform translation (body+0x1C -> +48) is
 * within 0.5 m of TARGET. Field contracts: see collision_route_watch.cpp.
 * Parameters (rpc.exports.configure): target [x,y,z], aabb [[lo],[hi]], radius.
 */
'use strict';

const MEMBASE = ptr('0x100000000');
let TARGET = [-2281.447, 1176.876, -365.4114];           // Havok_Box01 (golden 0x022CBE40)
let AABB = [[-2301.17, 1171.95, -375.39], [-2262.25, 1374.14, -354.79]];
let RADIUS = 40.0;
const RB = 0x28;   // PPCContext r[] base (XENIA_HARNESS.md, measured)

function be32(va) {
  const b = MEMBASE.add(va >>> 0).readByteArray(4);
  const u = new Uint8Array(b);
  return ((u[0] << 24) | (u[1] << 16) | (u[2] << 8) | u[3]) >>> 0;
}
function be8(va) { return MEMBASE.add(va >>> 0).readU8(); }
function bef(va) {
  const dv = new DataView(new ArrayBuffer(4));
  dv.setUint32(0, be32(va));
  return dv.getFloat32(0);
}
function vec3(va) { return [bef(va), bef(va + 4), bef(va + 8)]; }
function plausible(va) { return va >= 0x10000 && va < 0xA0000000; }
function findCtx(cpu) {
  for (const c of [cpu.rsi, cpu.rdi, cpu.rbx, cpu.r12, cpu.r13, cpu.r14, cpu.r15, cpu.rbp]) {
    try { if (c.add(0xA98).readU64().toString() === '4294967296') return c; } catch (e) { }
  }
  return null;
}
function greg(ctx, n) { return ctx.add(RB + 8 * n).readU64().and(0xFFFFFFFF).toNumber() >>> 0; }
function cstr(va, max) {
  let s = '';
  for (let i = 0; i < max; i++) { const c = be8(va + i); if (!c) break; s += String.fromCharCode(c); }
  return s;
}

let supActor = 0, supPrxy = 0, targetBody = 0, tick = 0, lastOut = null;
let supPos = null, supSpd = 0, lastFast = false;
const moverDepth = {};           // threadId -> nesting depth inside MoveTowardIdealPos
const seenBodies = new Set();    // 'addr@x,y,z' already sent as {t:'body'}
// f1 (dt): Xenia PPCContext is r[32] (measured at 0x28), ctr, lr, msr, f[32] -> f1 at 0x148.
// DERIVED from reference/xenia-canary ppc_context.h, not measured: out-of-range reads fall back
// to the game's fixed 1/30 s step.
const F1 = 0x148;
let sec = null;
function resetSec() {
  sec = { calls: 0, integrate: 0, inactive: 0, skip: 0, keyframed: 0, teleports: 0, near: 0,
          targetContact: 0, inside: 0, maxDepth: -1e9, maxTeleport: 0, maxStep: 0, t0: Date.now() };
}
resetSec();

const LOCO = { 0x8200EDF4: 'LOCO', 0x8200ED88: 'FLY', 0x8200ED28: 'ANIM', 0x8200EDBC: 'GRND',
               0x8200ED58: 'CPTR', 0x8200F030: 'WHLD', 0x82008D50: 'NONE' };

function isSuperman(ent) {
  if (ent === supActor) return true;
  if (supActor || !plausible(ent) || be32(ent) !== 0x82009798) return false;
  const h = be32(ent + 0x84); if (!plausible(h)) return false;
  const s = be32(h + 0x24); if (!plausible(s)) return false;
  if (cstr(s, 8) !== 'SUPERMAN') return false;
  supActor = ent; supPrxy = be32(ent + 0x70);
  send({ t: 'superman', ms: Date.now(), actor: '0x' + ent.toString(16), prxy: '0x' + supPrxy.toString(16) });
  return true;
}
function isTarget(body) {
  if (!plausible(body)) return false;
  if (body === targetBody) return true;
  const xf = be32(body + 0x1C); if (!plausible(xf)) return false;
  const p = vec3(xf + 48);
  for (let i = 0; i < 3; i++) if (Math.abs(p[i] - TARGET[i]) > 0.5) return false;
  if (!targetBody) { targetBody = body; send({ t: 'target', body: '0x' + body.toString(16) }); }
  return true;
}
function bodyInfo(body) {
  const xf = plausible(body) ? be32(body + 0x1C) : 0;
  const pos = plausible(xf) ? vec3(xf + 0x30) : [NaN, NaN, NaN];
  const layer = plausible(body) ? (be32(body + 0x30) & 0x1F) : -1;
  const shape = plausible(body) ? be32(body + 0x14) : 0;
  return { pos: pos, layer: layer, shapevt: '0x' + (plausible(shape) ? be32(shape) : 0).toString(16) };
}
function bodyList(arr, n, stride, off, show, src) {
  const out = [];
  for (let i = 0; i < n && i < show; i++) {
    const coll = be32(arr + stride * i + off);
    const body = coll ? (coll - 0x14) >>> 0 : 0;
    out.push('0x' + body.toString(16));
    if (!body) continue;
    const b = bodyInfo(body), key = body + '@' + b.pos.map(x => x.toFixed(2)).join(',');
    if (!seenBodies.has(key)) {
      seenBodies.add(key);
      send({ t: 'body', body: '0x' + body.toString(16), src: src, pos: b.pos, layer: b.layer, shapevt: b.shapevt, tick: tick });
    }
  }
  return out;
}
function pathTick(prxy, pin, pout, v, spd, path) {
  const proxy = be32(prxy + 4), ph = plausible(proxy) ? be32(proxy + 0x30) : 0;
  let manN = plausible(proxy) ? be32(proxy + 0x78) : 0, ovN = plausible(ph) ? be32(ph + 0x124) : 0;
  if (manN > 64) manN = 0;
  if (ovN > 4096) ovN = 0;
  const man = manN ? bodyList(be32(proxy + 0x74), manN, 48, 40, 8, 'man') : [];
  const ov = ovN ? bodyList(be32(ph + 0x120), ovN, 4, 0, 12, 'ov') : [];
  send(Object.assign({ t: 'path', ms: Date.now(), tick: tick, path: path, L: plausible(ph) ? (be32(ph + 0x30) & 0x1F) : -1,
                       in: pin, out: pout, v: v, spd: spd, manN: manN, man: man, ovN: ovN, ov: ov }, locoInfo()));
}
function worldEvent(ev, st, world, lock, body, b) {
  const known = supActor && supPos;
  const d = known ? Math.hypot(b.pos[0] - supPos[0], b.pos[1] - supPos[1], b.pos[2] - supPos[2]) : NaN;
  if (b.layer !== 7 && !(known && d <= 300)) return;
  send({ t: 'world', ev: ev, st: st, ms: Date.now(), tick: tick, body: '0x' + body.toString(16),
         world: '0x' + world.toString(16), lock: lock, layer: b.layer, shapevt: b.shapevt, pos: b.pos,
         sup: supPos, spd: supSpd, dist: isNaN(d) ? null : d });
}
function depthIn(p) { let d = 1e9; for (let i = 0; i < 3; i++) d = Math.min(d, p[i] - AABB[0][i], AABB[1][i] - p[i]); return d; }
function distTo(p) {
  let s = 0;
  for (let i = 0; i < 3; i++) { const e = Math.max(AABB[0][i] - p[i], 0, p[i] - AABB[1][i]); s += e * e; }
  return Math.sqrt(s);
}
function locoInfo() {
  const slot = supActor ? be32(supActor + 0x60) : 0;
  const comp = plausible(slot) ? be32(slot + 4) : 0;
  const vt = plausible(comp) ? be32(comp) : 0;
  return { loco: LOCO[vt] || ('0x' + vt.toString(16)), def: plausible(comp) ? be32(comp + 0x60) : null,
           sub: plausible(comp) ? be32(comp + 0x90) : null };
}
function summary() {
  if (Date.now() - sec.t0 < 1000) return;
  const p = supPrxy;
  const proxy = plausible(p) ? be32(p + 4) : 0, ph = plausible(proxy) ? be32(proxy + 0x30) : 0;
  const li = locoInfo();
  send(Object.assign({ t: 'sec', ms: Date.now(), b0E: p ? be8(p + 0x0E) : null, b0F: p ? be8(p + 0x0F) : null,
    sup: p ? be8(p + 0x7C) : null, red: p ? be32(p + 0x84) : null,
    fi: plausible(ph) ? '0x' + be32(ph + 0x30).toString(16) : null }, li, sec));
  resetSec();
}
function routeTick(prxy, ent, pin, pout, path, b0E, b0F) {
  if (distTo(pout) > RADIUS && distTo(pin) > RADIUS) return;
  sec.near++;
  const proxy = be32(prxy + 4), ph = plausible(proxy) ? be32(proxy + 0x30) : 0;
  let man = 0, manT = 0, ovN = 0, ovT = 0;
  if (plausible(proxy)) {
    const arr = be32(proxy + 0x74); man = be32(proxy + 0x78);
    if (man > 64 || !plausible(arr)) man = 0;
    for (let i = 0; i < man; i++) if (isTarget(be32(arr + 48 * i + 40) - 0x14)) manT++;
  }
  if (plausible(ph)) {
    const arr = be32(ph + 0x120); ovN = be32(ph + 0x124);
    if (ovN > 4096 || !plausible(arr)) ovN = 0;
    for (let i = 0; i < ovN; i++) if (isTarget(be32(arr + 4 * i) - 0x14)) ovT = 1;
  }
  const depth = depthIn(pout);
  sec.maxDepth = Math.max(sec.maxDepth, depth);
  if (manT) sec.targetContact++;
  if (depth > 1.5) sec.inside++;
  const v = plausible(proxy) ? vec3(proxy + 0x10) : [0, 0, 0];
  send(Object.assign({ t: 'tick', ms: Date.now(), tick: tick, path: path, b0E: b0E, b0F: b0F, in: pin, out: pout,
                       v: v, man: man, manT: manT, ovT: ovT, ovN: ovN, depth: depth }, locoInfo()));
  if (depth > 1.5 && manT === 0) send({ t: 'inside', tick: tick, depth: depth, path: path, pos: pout, ovT: ovT });
}

const hooks = {
  0x826D2690: {
    onEnter() {
      const ctx = findCtx(this.context); if (!ctx) return;
      const p = greg(ctx, 3);
      if (plausible(p) && cstr(p, 32) === 'screens/hud/main_hud') send({ t: 'anchor', ms: Date.now() });
    }
  },
  0x821C0C58: {
    onEnter() {
      this.tid = this.threadId;
      moverDepth[this.tid] = (moverDepth[this.tid] || 0) + 1;
      const ctx = findCtx(this.context); this.ok = false; if (!ctx) return;
      const prxy = greg(ctx, 3), ent = greg(ctx, 4);
      if (!isSuperman(ent)) return;
      this.ok = true; this.prxy = prxy; this.ent = ent;
      this.pin = vec3(ent + 0x20); this.b0E = be8(prxy + 0x0E); this.b0F = be8(prxy + 0x0F);
      let dt = 1 / 30;
      try { const f = ctx.add(F1).readDouble(); if (f > 0 && f < 0.5) dt = f; } catch (e) { }
      this.dt = dt;
    },
    onLeave() {
      moverDepth[this.tid] = Math.max(0, (moverDepth[this.tid] || 1) - 1);
      if (!this.ok) return;
      tick++; sec.calls++;
      const pout = vec3(this.ent + 0x20);
      const proxy = be32(this.prxy + 4), v = plausible(proxy) ? vec3(proxy + 0x10) : [0, 0, 0];
      const step = Math.hypot(pout[0] - this.pin[0], pout[1] - this.pin[1], pout[2] - this.pin[2]);
      let spd = Math.hypot(v[0], v[1], v[2]);
      if (step / this.dt > spd) spd = step / this.dt;
      supPos = pout; supSpd = spd;
      const fast = spd > 20, active = (this.b0F & 4) !== 0;
      const pth = !active ? 'inactive' : (be32(this.prxy + 0x88) === 1 ? 'off-camera-skip'
                                          : ((this.b0E >> 4) === 1 ? 'keyframed' : 'integrate'));
      if (fast || lastFast) pathTick(this.prxy, this.pin, pout, v, spd, pth);
      lastFast = fast;
      if (!active) { sec.inactive++; routeTick(this.prxy, this.ent, this.pin, pout, 'inactive', this.b0E, this.b0F); lastOut = pout; summary(); return; }
      const skipped = be32(this.prxy + 0x88) === 1, keyed = (this.b0E >> 4) === 1;
      if (skipped) sec.skip++;
      if (keyed) sec.keyframed++;
      if (!skipped && !keyed) sec.integrate++;
      if (lastOut) {
        const d = Math.hypot(this.pin[0] - lastOut[0], this.pin[1] - lastOut[1], this.pin[2] - lastOut[2]);
        if (d > 0.01) { sec.teleports++; sec.maxTeleport = Math.max(sec.maxTeleport, d);
                        send({ t: 'teleport', tick: tick, d: d, from: lastOut, to: this.pin }); }
      }
      sec.maxStep = Math.max(sec.maxStep, Math.hypot(pout[0] - this.pin[0], pout[1] - this.pin[1], pout[2] - this.pin[2]));
      routeTick(this.prxy, this.ent, this.pin, pout, skipped ? 'off-camera-skip' : keyed ? 'keyframed' : 'integrate', this.b0E, this.b0F);
      lastOut = pout;
      summary();
    }
  },
};
function eventHook(name, who) {
  return {
    onEnter() {
      const ctx = findCtx(this.context); if (!ctx) return;
      const r3 = greg(ctx, 3);
      const mine = who === 'actor' ? (r3 === supActor && supActor) : (r3 === supPrxy && supPrxy);
      if (!mine) return;
      if (name === 'SETPOS' && (moverDepth[this.threadId] || 0) > 0) return;   // the mover's own write-back
      // the guest LR is not read (its PPCContext offset is unmeasured); the host return address
      // lands in the caller's JIT code, which the driver can map back via the indirection table
      send({ t: 'event', ev: name, ms: Date.now(), r4: '0x' + greg(ctx, 4).toString(16), r5: '0x' + greg(ctx, 5).toString(16),
             tick: tick, pos: name === 'SETPOS' ? vec3(greg(ctx, 4)) : (name === 'SETXFORM' ? vec3(greg(ctx, 4) + 16) : undefined),
             hostRet: this.returnAddress.toString() });
    }
  };
}
hooks[0x821AA218] = eventHook('SETPOS', 'actor');
hooks[0x821E3CF8] = eventHook('SETXFORM', 'actor');
hooks[0x82596508] = {
  onEnter() {
    const ctx = findCtx(this.context); this.ok = false; if (!ctx) return;
    this.ok = true; this.world = greg(ctx, 3); this.body = greg(ctx, 4);
    this.lock = plausible(this.world) ? be32(this.world + 0x8C) : 0;
  },
  onLeave() {
    if (!this.ok) return;
    worldEvent('ADD', this.lock ? 'queued' : 'added', this.world, this.lock, this.body, bodyInfo(this.body));
  }
};
hooks[0x825967C0] = {
  onEnter() {   // the remove can release the body: read it before the guest runs
    const ctx = findCtx(this.context); if (!ctx) return;
    const world = greg(ctx, 4), body = greg(ctx, 5), lock = plausible(world) ? be32(world + 0x8C) : 0;
    worldEvent('REMOVE', lock ? 'queued' : 'removed', world, lock, body, bodyInfo(body));
  }
};
hooks[0x821AE1C8] = eventHook('SET_PHYSREP_MODE', 'prxy');
hooks[0x821AE388] = eventHook('DEACTIVATE', 'prxy');
hooks[0x821AE218] = eventHook('SET_FILTERINFO', 'prxy');

// iter-467: the slot value of a not-yet-compiled guest function is the shared resolve thunk,
// which in the oracle build sits INSIDE the generated-code region (0xA0000220 on 2026-10-07):
// the old "host >= 0xA0000000 = compiled" test attached every hook to the thunk itself (every
// unresolved call). The most common slot value over the image's code range is the thunk
// (same rule as tools/xenia_audio_capture.js and tools/devtools/frida_skip.js).
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
      send({ t: 'attached', guest: '0x' + va.toString(16), host: '0x' + host.toString(16), ms: Date.now() });
    } catch (e) { send({ t: 'attach-error', guest: '0x' + va.toString(16), err: String(e) }); attached[k] = true; }
  }
}
setInterval(poll, 250);
poll();

rpc.exports = {
  configure(target, aabb, radius) { TARGET = target; AABB = aabb; RADIUS = radius; return 'ok'; },
};
