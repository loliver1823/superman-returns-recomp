#!/usr/bin/env python3
"""
Frida MCP server — AI-controlled dynamic instrumentation for the Superman Returns harness.

Uses only: frida (already installed), Python stdlib. No pyuv dependency.

Start as MCP stdio server:
    python tools/frida_mcp.py

Usage (Claude will call these tools):
  - frida_list_processes    — find the running harness PID
  - frida_attach            — attach to the process
  - frida_watchpoint        — set hardware watchpoint on a guest VA
  - frida_clear_watchpoints — clear all watchpoints
  - frida_read_memory       — read guest memory at a VA
  - frida_eval              — run arbitrary Frida JS in the attached process
  - frida_detach            — detach from process
"""

import json
import sys
import threading
import io

try:
    import frida
    FRIDA_AVAILABLE = True
except ImportError:
    FRIDA_AVAILABLE = False

# ── Frida session state ──────────────────────────────────────────────────────

_session = None          # frida.Session
_script = None           # frida.Script (persistent watchpoint handler)
_watchpoint_hits = []    # collected hit records
_lock = threading.Lock()
_attached_pid = None

WATCHPOINT_JS = """
// Persistent watchpoint handler script.
// Called from Python via exports.setWatchpoint / exports.clearWatchpoints.

var _watchpoints = {};
var _nextWpSlot = 0;             // hardware watchpoint slot id (0..3); reused round-robin
var _wpHandlerInstalled = false; // install the exception handler exactly once
var _rearmTimer = null;          // periodic re-arm so writes from NEW threads are caught
var _armedThreadKeys = {};       // "threadId:slot" -> true (skip already-armed)
var _g_debugGuestBase = null;

// HW watchpoints live in per-thread debug registers, so a thread spawned AFTER arming
// (boot spawns threads continuously) would not trip the watchpoint. Re-arm every active
// watchpoint on every current thread periodically so any-thread writes are caught.
function rearmAllWatchpoints() {
    var threads = Process.enumerateThreads();
    for (var va in _watchpoints) {
        var wp = _watchpoints[va];
        for (var i = 0; i < threads.length; i++) {
            var key = threads[i].id + ':' + wp.slot;
            if (_armedThreadKeys[key]) continue;
            try {
                threads[i].setHardwareWatchpoint(wp.slot, wp.hostAddr, wp.size, wp.mode);
                _armedThreadKeys[key] = true;
            } catch(e) {}
        }
    }
}

function getGuestBase() {
    if (_g_debugGuestBase !== null) return _g_debugGuestBase;
    try {
        // frida 17 removed Module.findExportByName; use findGlobalExportByName,
        // fall back to the legacy API for older frida.
        var sym = null;
        if (typeof Module.findGlobalExportByName === 'function') {
            sym = Module.findGlobalExportByName('g_debugGuestBase');
        } else if (typeof Module.findExportByName === 'function') {
            sym = Module.findExportByName(null, 'g_debugGuestBase');
        }
        if (sym) {
            // frida 17 removed Memory.readPointer; use the NativePointer method.
            _g_debugGuestBase = sym.readPointer();
        }
    } catch(e) {}
    if (_g_debugGuestBase === null) {
        // Xenia fallback (XENIA_HARNESS.md): xenia_canary maps the guest at a fixed
        // host membase 0x100000000. Validate via the XEX image's MZ header at
        // membase+0x82000000 before trusting it, so reads/watchpoints work when the
        // attached process is Xenia rather than our harness.
        try {
            var xeniaBase = ptr('0x100000000');
            var mz = new Uint8Array(xeniaBase.add(0x82000000).readByteArray(2));
            if (mz[0] === 0x4D && mz[1] === 0x5A) _g_debugGuestBase = xeniaBase;
        } catch(e) {}
    }
    return _g_debugGuestBase;
}

// ── PPC-aware debug layer (iter-214) ────────────────────────────────────────
// Reads the harness's exported live-PPCContext pointer + register offsets and the
// inverse dispatch table, so we can report TRUE guest registers and a guest PPC
// call stack (sub_XXXX frames) instead of host x86 frames.
var _dbg = null;
function _sym(name) {
    try {
        if (typeof Module.findGlobalExportByName === 'function')
            return Module.findGlobalExportByName(name);
        if (typeof Module.findExportByName === 'function')
            return Module.findExportByName(null, name);
    } catch(e) {}
    return null;
}
function dbg() {
    if (_dbg) return _dbg;
    var ctxSym = _sym('g_debugMainCtx');
    var gprSym = _sym('g_debugCtxGprOff');
    var lrSym  = _sym('g_debugCtxLrOff');
    var ctrSym = _sym('g_debugCtxCtrOff');
    var dtSym  = _sym('g_debugDispatchTable');
    var dteSym = _sym('g_debugDispatchTableEntries');
    var dcbSym = _sym('g_debugDispatchCodeBase');
    if (!ctxSym || !gprSym || !dtSym || !dcbSym) return null;   // old build, no PPC exports
    _dbg = {
        ctxSym: ctxSym,                                  // &(void* g_debugMainCtx)
        gprSym: gprSym,                                  // &g_debugCtxGprOff[0]
        lrOff:  lrSym  ? lrSym.readU32()  : 0,
        ctrOff: ctrSym ? ctrSym.readU32() : 0,
        dtSym:  dtSym,                                   // &(void** g_debugDispatchTable)
        dtEntries: dteSym ? dteSym.readU64().toNumber() : 0,
        codeBase:  dcbSym ? dcbSym.readU32() : 0,
    };
    return _dbg;
}
function gprOff(d, n) { return d.gprSym.add(n * 4).readU32(); }
// Map a guest code VA to "sub_<entry>[+off]" by walking the inverse dispatch table
// back from the VA's slot to the nearest populated (function-entry) slot.
function symbolize(d, va) {
    if (va < d.codeBase) return '0x' + va.toString(16);
    var idx = (va - d.codeBase) >>> 2;
    if (idx >= d.dtEntries) return '0x' + va.toString(16);
    var table = d.dtSym.readPointer();                   // host base of the dispatch table
    if (table.isNull()) return '0x' + va.toString(16);
    var scan = 0;
    while (idx > 0 && scan < 16384) {
        if (!table.add(idx * Process.pointerSize).readPointer().isNull()) break;
        idx--; scan++;
    }
    var entryVA = d.codeBase + idx * 4;
    var s = 'sub_' + entryVA.toString(16).toUpperCase();
    if (va !== entryVA) s += '+0x' + (va - entryVA).toString(16);
    return s;
}
// Big-endian 32-bit read from guest memory (guest memory IS big-endian).
function rdBE32(va) {
    var b = getGuestBase();
    if (!b) return 0;
    var u = new Uint8Array(b.add(ptr(va).and(ptr('0xFFFFFFFF'))).readByteArray(4));
    return ((u[0] << 24) | (u[1] << 16) | (u[2] << 8) | u[3]) >>> 0;
}
// Walk the PPC AIX back-chain from (lr, r1) → array of symbolized guest frames.
// PPC prologue saves LR at [SP_in - 8]; back chain at [r1]. Stacks grow down, so
// each back-chain pointer must ascend. At kernel-import/leaf boundaries [r1] may be
// 0 (r1 sits below a standard frame), so if [r1] isn't a valid back-chain we scan a
// small window above r1 for the first plausible one (ascending + saved LR is code).
function guestStack(d, lr, sp, maxFrames) {
    var hi = d.codeBase + d.dtEntries * 4;
    function okSP(v)   { return v >= 0x10000 && v < 0xC0000000; }
    function okCode(v) { return v >= d.codeBase && v < hi; }
    // Always emit the exact VA next to the (best-effort) symbol: the dispatch-table
    // symbolizer lands on the nearest registered entry, which can be a finer boundary
    // than the source naming — feed the 0x... VAs to va_to_source for authoritative names.
    function frame(va) { return symbolize(d, va >>> 0) + ' (0x' + (va >>> 0).toString(16) + ')'; }
    var frames = [frame(lr)];
    var bc = rdBE32(sp);
    if (!(okSP(bc) && bc > sp && okCode(rdBE32(bc - 8)))) {
        bc = 0;
        for (var off = 0; off <= 0x60; off += 4) {
            var cand = rdBE32(sp + off);
            if (okSP(cand) && cand > (sp + off) && okCode(rdBE32(cand - 8))) { bc = cand; break; }
        }
    }
    var max = maxFrames || 32;
    for (var i = 0; i < max && bc; i++) {
        if (!okSP(bc)) break;
        var savedLR = rdBE32(bc - 8) >>> 0;
        if (okCode(savedLR)) frames.push(frame(savedLR));
        var next = rdBE32(bc);
        if (next <= bc) break;          // back chain must ascend
        bc = next;
    }
    return frames;
}

rpc.exports = {
    guestBase: function() {
        var b = getGuestBase();
        return b ? b.toString() : null;
    },

    guestToHost: function(guestVA) {
        var b = getGuestBase();
        if (!b) return null;
        // full zero-extended 32-bit VA (PPC_GUEST_OFFSET); base = host addr of guest VA 0
        var offset = ptr(guestVA).and(ptr('0xFFFFFFFF'));
        return b.add(offset).toString();
    },

    setWatchpoint: function(guestVA, sizeBytes, mode) {
        // mode: 'r', 'w', 'rw'
        var b = getGuestBase();
        if (!b) return 'ERROR: g_debugGuestBase not found — is harness running?';
        // full zero-extended 32-bit VA (PPC_GUEST_OFFSET); base = host addr of guest VA 0
        var offset = ptr(guestVA).and(ptr('0xFFFFFFFF'));
        var hostAddr = b.add(offset);

        // Remove existing watchpoint on same address
        if (_watchpoints[guestVA]) {
            try { _watchpoints[guestVA].remove(); } catch(e) {}
        }

        if (Process.enumerateThreads().length === 0) return 'ERROR: no threads found';

        // Frida API: Thread.setHardwareWatchpoint(id, address, size, conditions). The FIRST
        // arg is a hardware slot id (unsigned int 0..3) — omitting it was the "expected an
        // unsigned integer" bug. Register the watchpoint, then rearmAllWatchpoints() arms
        // the slot on every thread; a timer keeps arming threads spawned later.
        var slot = _nextWpSlot % 4;
        _nextWpSlot += 1;
        _watchpoints[guestVA] = {
            slot: slot, hostAddr: hostAddr, size: sizeBytes, mode: mode,
            remove: function() {
                Process.enumerateThreads().forEach(function(t) {
                    try { t.unsetHardwareWatchpoint(slot); } catch(e) {}
                });
                for (var k in _armedThreadKeys) {
                    if (k.split(':')[1] === String(slot)) delete _armedThreadKeys[k];
                }
            }
        };
        rearmAllWatchpoints();
        var setOn = Object.keys(_armedThreadKeys).filter(function(k) {
            return k.split(':')[1] === String(slot);
        });
        if (setOn.length === 0) {
            delete _watchpoints[guestVA];
            return 'ERROR setting watchpoint: no thread accepted slot ' + slot;
        }
        // Keep re-arming so threads spawned after now are also covered.
        if (_rearmTimer === null) _rearmTimer = setInterval(rearmAllWatchpoints, 200);

        // Install the exception handler exactly once. It MUST return false for anything
        // that is not our watchpoint, otherwise it would swallow the recomp's real
        // crashes (single global handler shared by all watchpoints).
        if (_wpHandlerInstalled) return 'Watchpoint set on ' + setOn.length +
            ' threads (slot ' + slot + '): guest ' + guestVA + ' → host ' + hostAddr.toString();
        _wpHandlerInstalled = true;
        Process.setExceptionHandler(function(details) {
            // Only handle hardware-watchpoint traps; let everything else propagate.
            if (details.type !== 'breakpoint' && details.type !== 'single-step') return false;
            var ctx = details.context;
            // iter-365o: walk the FAULTING thread's guest stack via
            // g_debugThreadRegistry (same machinery as guestThreadBacktrace) — the
            // old main-thread walk mislabeled every cross-thread writer. Falls back
            // to the main thread only when the writer is unregistered (host thread).
            var wtid = Process.getCurrentThreadId();
            var gstack = null;
            var src = 'main';
            try {
                var reg = _sym('g_debugThreadRegistry');
                var d = dbg();
                if (reg && d) {
                    for (var i = 0; i < 64; i++) {
                        var slot = reg.add(i * 64);
                        if (slot.readU32() !== wtid) continue;
                        var wctx = slot.add(8).readPointer();
                        if (wctx.isNull()) break;
                        var lrOff = slot.add(16).readU32(), r1Off = slot.add(20).readU32();
                        gstack = guestStack(d, wctx.add(lrOff).readU32() >>> 0,
                                            r1Off ? wctx.add(r1Off).readU32() >>> 0 : 0, 24);
                        src = 'writer tid ' + wtid;
                        break;
                    }
                }
            } catch(e) {}
            if (!gstack) {
                try {
                    var d2 = dbg();
                    if (d2) {
                        var c = d2.ctxSym.readPointer();
                        if (!c.isNull()) {
                            gstack = guestStack(d2, c.add(d2.lrOff).readU32() >>> 0,
                                                c.add(gprOff(d2, 1)).readU32() >>> 0, 24);
                            src = 'MAIN(fallback) writer tid ' + wtid;
                        }
                    }
                } catch(e) {}
            }
            // Handler is shared by all watchpoints; report which ones are active so the
            // caller can disambiguate (single-watchpoint case is unambiguous anyway).
            send({
                type: 'watchpoint_hit',
                guestVA: Object.keys(_watchpoints).join(','),
                pc: (ctx.pc ? ctx.pc.toString() : '?') + ' [' + src + ']',
                sp: ctx.sp ? ctx.sp.toString() : '?',
                guestStack: gstack,
                backtrace: Thread.backtrace(ctx, Backtracer.ACCURATE)
                    .map(function(a) { return a.toString(); })
            });
            return true; // handled — resume execution
        });

        return 'Watchpoint set on ' + setOn.length + ' threads (slot ' + slot +
            '): guest ' + guestVA + ' → host ' + hostAddr.toString();
    },

    clearWatchpoints: function() {
        if (_rearmTimer !== null) { clearInterval(_rearmTimer); _rearmTimer = null; }
        for (var va in _watchpoints) {
            try { _watchpoints[va].remove(); } catch(e) {}
        }
        _watchpoints = {};
        _armedThreadKeys = {};
        return 'All watchpoints cleared';
    },

    readMemory: function(guestVA, size) {
        var b = getGuestBase();
        if (!b) return null;
        // full zero-extended 32-bit VA (PPC_GUEST_OFFSET); base = host addr of guest VA 0
        var offset = ptr(guestVA).and(ptr('0xFFFFFFFF'));
        var hostAddr = b.add(offset);
        try {
            // frida 17: NativePointer.readByteArray (Memory.readByteArray removed).
            var bytes = hostAddr.readByteArray(size);
            return Array.from(new Uint8Array(bytes)).map(function(b) {
                return ('0' + b.toString(16)).slice(-2);
            }).join(' ');
        } catch(e) {
            return 'ERROR: ' + e.message;
        }
    },

    // PPC-aware: true guest registers of the main thread (host-native, no bswap).
    guestRegs: function() {
        var d = dbg();
        if (!d) return 'ERROR: PPC debug exports not found (rebuild harness with iter-214 changes?)';
        var ctx = d.ctxSym.readPointer();
        if (ctx.isNull()) return 'ERROR: g_debugMainCtx is null (guest not running yet)';
        var out = {};
        for (var n = 0; n < 32; n++)
            out['r' + n] = '0x' + ctx.add(gprOff(d, n)).readU32().toString(16);
        out['lr']  = '0x' + ctx.add(d.lrOff).readU32().toString(16);
        out['ctr'] = '0x' + ctx.add(d.ctrOff).readU32().toString(16);
        return JSON.stringify(out);
    },

    // PPC-aware: guest call stack of the main thread (sub_XXXX frames).
    guestBacktrace: function(maxFrames) {
        var d = dbg();
        if (!d) return 'ERROR: PPC debug exports not found (rebuild harness with iter-214 changes?)';
        var ctx = d.ctxSym.readPointer();
        if (ctx.isNull()) return 'ERROR: g_debugMainCtx is null (guest not running yet)';
        var lr = ctx.add(d.lrOff).readU32() >>> 0;
        var sp = ctx.add(gprOff(d, 1)).readU32() >>> 0;   // r1 = guest SP
        return guestStack(d, lr, sp, maxFrames).join('\\n');
    },

    // iter-295g: enumerate ALL guest-running threads via the harness's
    // g_debugThreadRegistry export (64 slots x 64B: tid u32, seq u32, ctx ptr@8,
    // lrOff/r1Off/r3Off/ctrOff u32 @16..28, name[32] @32). Live lr symbolized.
    // Closes the "PPC layer only tracks the main thread" gap.
    guestThreads: function() {
        var reg = _sym('g_debugThreadRegistry');
        if (!reg) return 'ERROR: g_debugThreadRegistry not found (rebuild harness with iter-295g changes?)';
        var d = dbg();
        var out = [];
        for (var i = 0; i < 64; i++) {
            var slot = reg.add(i * 64);
            var tid = slot.readU32();
            if (!tid) continue;
            var ctx = slot.add(8).readPointer();
            if (ctx.isNull()) continue;
            var lrOff = slot.add(16).readU32(), r1Off = slot.add(20).readU32();
            var r3Off = slot.add(24).readU32(), ctrOff = slot.add(28).readU32();
            var name = '-';
            try { name = slot.add(32).readUtf8String(31) || '-'; } catch(e) {}
            var lr = lrOff ? ctx.add(lrOff).readU32() >>> 0 : 0;
            var e = { slot: i, tid: tid, name: name,
                      lr: '0x' + lr.toString(16),
                      r1: '0x' + (r1Off ? ctx.add(r1Off).readU32() >>> 0 : 0).toString(16),
                      r3: '0x' + (r3Off ? ctx.add(r3Off).readU32() >>> 0 : 0).toString(16),
                      ctr: '0x' + (ctrOff ? ctx.add(ctrOff).readU32() >>> 0 : 0).toString(16) };
            if (d && lr) e.where = symbolize(d, lr);
            out.push(e);
        }
        return JSON.stringify(out);
    },

    // iter-295g: guest backtrace of ANY registered thread (by slot index or tid).
    guestThreadBacktrace: function(slotOrTid, maxFrames) {
        var reg = _sym('g_debugThreadRegistry');
        if (!reg) return 'ERROR: g_debugThreadRegistry not found (rebuild harness with iter-295g changes?)';
        var d = dbg();
        if (!d) return 'ERROR: PPC debug exports not found';
        for (var i = 0; i < 64; i++) {
            var slot = reg.add(i * 64);
            var tid = slot.readU32();
            if (!tid) continue;
            if (i !== slotOrTid && tid !== slotOrTid) continue;
            var ctx = slot.add(8).readPointer();
            if (ctx.isNull()) return 'ERROR: slot ctx is null';
            var lrOff = slot.add(16).readU32(), r1Off = slot.add(20).readU32();
            var lr = ctx.add(lrOff).readU32() >>> 0;
            var sp = r1Off ? ctx.add(r1Off).readU32() >>> 0 : 0;
            return guestStack(d, lr, sp, maxFrames).join('\\n');
        }
        return 'ERROR: no registered thread with slot/tid ' + slotOrTid;
    },

    eval: function(code) {
        try {
            return String(eval(code));
        } catch(e) {
            return 'ERROR: ' + e.message;
        }
    }
};
"""

# ── MCP tool definitions ─────────────────────────────────────────────────────

MCP_TOOLS = [
    {
        "name": "frida_list_processes",
        "description": "List running processes matching a name filter. Use to find the Superman Returns harness PID.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "filter": {"type": "string", "description": "Name substring to filter (default: 'Superman')"}
            }
        }
    },
    {
        "name": "frida_attach",
        "description": "Attach to the running harness by PID or process name.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "target": {"type": "string", "description": "PID (integer) or process name substring"}
            },
            "required": ["target"]
        }
    },
    {
        "name": "frida_guest_base",
        "description": "Read the g_debugGuestBase symbol from the attached process. Returns the host address of guest VA 0x00000000.",
        "inputSchema": {"type": "object", "properties": {}}
    },
    {
        "name": "frida_watchpoint",
        "description": "Set a hardware watchpoint on a guest virtual address. Fires on read/write and reports caller backtrace.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "guest_va": {"type": "string", "description": "Guest VA hex string, e.g. '0x8200094C'"},
                "size": {"type": "integer", "description": "Watchpoint size in bytes (1/2/4/8, default 4)"},
                "mode": {"type": "string", "description": "'r', 'w', or 'rw' (default 'w')"}
            },
            "required": ["guest_va"]
        }
    },
    {
        "name": "frida_get_hits",
        "description": "Return all watchpoint hit records collected since last call.",
        "inputSchema": {"type": "object", "properties": {}}
    },
    {
        "name": "frida_clear_watchpoints",
        "description": "Remove all active hardware watchpoints.",
        "inputSchema": {"type": "object", "properties": {}}
    },
    {
        "name": "frida_read_memory",
        "description": "Read guest memory at a VA and return hex bytes.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "guest_va": {"type": "string"},
                "size": {"type": "integer", "description": "Bytes to read (default 16)"}
            },
            "required": ["guest_va"]
        }
    },
    {
        "name": "frida_guest_regs",
        "description": "PPC-aware: read the MAIN guest thread's true PowerPC registers (r0-r31, lr, ctr) from the live PPCContext. Values are guest-native (already correct, no bswap needed). Racy snapshot.",
        "inputSchema": {"type": "object", "properties": {}}
    },
    {
        "name": "frida_guest_backtrace",
        "description": "PPC-aware: reconstruct the MAIN guest thread's PowerPC call stack as sub_XXXX frames by walking the guest back-chain (lr + r1), symbolized via the inverse dispatch table. Use this instead of host x86 backtraces.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "max_frames": {"type": "integer", "description": "Max stack frames (default 32)"}
            }
        }
    },
    {
        "name": "frida_threads",
        "description": "PPC-aware: enumerate ALL guest-running threads (main + FM workers + host driver threads like FE_TICK) from the harness's g_debugThreadRegistry — tid, name, live lr (symbolized), r1, r3, ctr per thread. Use to find which thread is stuck where.",
        "inputSchema": {"type": "object", "properties": {}}
    },
    {
        "name": "frida_thread_backtrace",
        "description": "PPC-aware: guest call stack of ANY registered thread (pick slot or tid from frida_threads). Same back-chain walk as frida_guest_backtrace but not limited to the main thread.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "slot_or_tid": {"type": "integer", "description": "Registry slot index or host thread id (from frida_threads)"},
                "max_frames": {"type": "integer", "description": "Max stack frames (default 32)"}
            },
            "required": ["slot_or_tid"]
        }
    },
    {
        "name": "frida_eval",
        "description": "Evaluate arbitrary Frida JavaScript in the attached process context.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "code": {"type": "string"}
            },
            "required": ["code"]
        }
    },
    {
        "name": "frida_detach",
        "description": "Detach from the current process.",
        "inputSchema": {"type": "object", "properties": {}}
    }
]

# ── Tool implementations ─────────────────────────────────────────────────────

def tool_list_processes(args):
    if not FRIDA_AVAILABLE:
        return "frida not installed"
    f = args.get("filter", "Superman").lower()
    procs = frida.get_local_device().enumerate_processes()
    matched = [p for p in procs if f in p.name.lower()]
    if not matched:
        return f"No processes matching '{f}'"
    return "\n".join(f"PID {p.pid}: {p.name}" for p in matched)


def tool_attach(args):
    global _session, _script, _attached_pid
    if not FRIDA_AVAILABLE:
        return "frida not installed"

    target = args["target"]
    try:
        pid = int(target)
    except ValueError:
        # Find by name
        procs = frida.get_local_device().enumerate_processes()
        found = [p for p in procs if target.lower() in p.name.lower()]
        if not found:
            return f"No process matching '{target}'"
        pid = found[0].pid

    if _session:
        try:
            _session.detach()
        except Exception:
            pass

    _session = frida.attach(pid)
    _attached_pid = pid
    _script = _session.create_script(WATCHPOINT_JS)

    def on_message(msg, data):
        if msg.get("type") == "send" and msg.get("payload", {}).get("type") == "watchpoint_hit":
            with _lock:
                _watchpoint_hits.append(msg["payload"])

    _script.on("message", on_message)
    _script.load()
    return f"Attached to PID {pid}. Script loaded."


def tool_guest_base(_args):
    if not _script:
        return "Not attached. Call frida_attach first."
    result = _script.exports_sync.guest_base()
    return f"g_debugGuestBase = {result}" if result else "g_debugGuestBase is null (harness not started?)"


def tool_watchpoint(args):
    if not _script:
        return "Not attached."
    va_str = args["guest_va"]
    va = int(va_str, 16)
    size = args.get("size", 4)
    mode = args.get("mode", "w")
    return _script.exports_sync.set_watchpoint(va, size, mode)


def tool_get_hits(_args):
    with _lock:
        hits = list(_watchpoint_hits)
        _watchpoint_hits.clear()
    if not hits:
        return "No watchpoint hits since last check."
    lines = []
    for h in hits:
        gstack = h.get("guestStack")
        # guestVA arrives from the JS side as str (hex or decimal) or int —
        # iter-365o: ':X' on a str raised and made every harvest fail.
        gva = h.get("guestVA", 0)
        if isinstance(gva, str):
            try:
                gva = int(gva, 0)
            except ValueError:
                gva = 0
        block = f"Guest 0x{gva:X} hit | host PC={h.get('pc')}"
        if gstack:
            block += "\n  guest stack (main thread):\n    " + "\n    ".join(gstack)
        bt = "\n  ".join(h.get("backtrace", []))
        block += f"\n  host backtrace:\n  {bt}"
        lines.append(block)
    return "\n\n".join(lines)


def tool_clear_watchpoints(_args):
    if not _script:
        return "Not attached."
    return _script.exports_sync.clear_watchpoints()


def tool_read_memory(args):
    if not _script:
        return "Not attached."
    va = int(args["guest_va"], 16)
    size = args.get("size", 16)
    return _script.exports_sync.read_memory(va, size)


def tool_guest_regs(_args):
    if not _script:
        return "Not attached."
    result = _script.exports_sync.guest_regs()
    if result and result.startswith("{"):
        try:
            regs = json.loads(result)
            return "  ".join(f"{k}={v}" for k, v in regs.items())
        except Exception:
            pass
    return result


def tool_guest_backtrace(args):
    if not _script:
        return "Not attached."
    return _script.exports_sync.guest_backtrace(args.get("max_frames", 32))


def tool_threads(_args):
    if not _script:
        return "Not attached."
    result = _script.exports_sync.guest_threads()
    if result and result.startswith("["):
        try:
            rows = json.loads(result)
            if not rows:
                return "No registered guest threads (registry empty)."
            lines = []
            for r in rows:
                where = f"  {r['where']}" if r.get("where") else ""
                lines.append(
                    f"slot {r['slot']:2d}  tid={r['tid']:<6d} name={r['name']:<16s} "
                    f"lr={r['lr']:<10s} r1={r['r1']:<10s} r3={r['r3']:<10s} ctr={r['ctr']}{where}")
            return "\n".join(lines)
        except Exception:
            pass
    return result


def tool_thread_backtrace(args):
    if not _script:
        return "Not attached."
    return _script.exports_sync.guest_thread_backtrace(
        args["slot_or_tid"], args.get("max_frames", 32))


def tool_eval(args):
    if not _script:
        return "Not attached."
    return _script.exports_sync.eval(args["code"])


def tool_detach(_args):
    global _session, _script, _attached_pid
    if _session:
        try:
            _session.detach()
        except Exception:
            pass
    _session = _script = _attached_pid = None
    return "Detached."


TOOL_HANDLERS = {
    "frida_list_processes": tool_list_processes,
    "frida_attach": tool_attach,
    "frida_guest_base": tool_guest_base,
    "frida_watchpoint": tool_watchpoint,
    "frida_get_hits": tool_get_hits,
    "frida_clear_watchpoints": tool_clear_watchpoints,
    "frida_read_memory": tool_read_memory,
    "frida_guest_regs": tool_guest_regs,
    "frida_guest_backtrace": tool_guest_backtrace,
    "frida_threads": tool_threads,
    "frida_thread_backtrace": tool_thread_backtrace,
    "frida_eval": tool_eval,
    "frida_detach": tool_detach,
}

# ── MCP stdio server ─────────────────────────────────────────────────────────

def mcp_server():
    stdin = io.TextIOWrapper(sys.stdin.buffer, encoding="utf-8")
    stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")

    def send(obj):
        stdout.write(json.dumps(obj) + "\n")
        stdout.flush()

    def handle(req):
        method = req.get("method", "")
        rid = req.get("id")

        if method == "initialize":
            return {"jsonrpc": "2.0", "id": rid, "result": {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "frida-superman", "version": "1.0"}
            }}

        if method == "tools/list":
            return {"jsonrpc": "2.0", "id": rid, "result": {"tools": MCP_TOOLS}}

        if method == "tools/call":
            name = req["params"]["name"]
            args = req["params"].get("arguments", {})
            handler = TOOL_HANDLERS.get(name)
            if not handler:
                text = f"Unknown tool: {name}"
            else:
                try:
                    text = str(handler(args))
                except Exception as e:
                    text = f"Error: {e}"
            return {"jsonrpc": "2.0", "id": rid, "result": {
                "content": [{"type": "text", "text": text}]
            }}

        if method == "notifications/initialized":
            return None

        return {"jsonrpc": "2.0", "id": rid,
                "error": {"code": -32601, "message": f"Unknown method: {method}"}}

    for line in stdin:
        line = line.strip()
        if not line:
            continue
        try:
            resp = handle(json.loads(line))
            if resp is not None:
                send(resp)
        except Exception as e:
            send({"jsonrpc": "2.0", "id": None,
                  "error": {"code": -32700, "message": str(e)}})


if __name__ == "__main__":
    mcp_server()
