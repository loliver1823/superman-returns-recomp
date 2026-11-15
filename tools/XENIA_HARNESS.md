# Xenia Harness — Playwright-style control of the working reference emulator

A "snapshot + act + inspect" loop for driving the **stock Xenia Canary** (the build that
reaches the Superman Returns menu) headlessly, so an agent can navigate the working game and
capture guest state for diffing against our recomp. Built iter-236/237 (see
`project_menu_divergence_lead` memory). Inspired by Playwright: *see the screen, act, read state.*

## Pieces

| Capability | Tool | Notes |
|---|---|---|
| **See** | `tools/xenia_shot.ps1` | Screenshots the Xenia window client area with `PrintWindow` (PW_RENDERFULLCONTENT): works while Xenia sits BEHIND other windows, never activates it (iter-464; `-Foreground` = the old restore + foreground + screen BitBlt). Agent `Read`s the PNG. |
| **Act** | `tools/xenia_pad.py` | Persistent **virtual Xbox 360 controller** (vgamepad + ViGEmBus). Send buttons by writing to `tools/pad_cmd.txt`. |
| **Inspect** | `frida-superman` MCP | Attach to xenia_canary; read/scan guest memory; MemoryAccessMonitor for write-watching. |

## Setup (one-time)
- ViGEmBus driver installed + running (`Get-Service ViGEmBus`).
- `pip install vgamepad`.
- Stock menu-reaching Xenia: `<Xenia Canary folder>\xenia_canary.exe`
  with profile XUID `E030000019503791` (`logged_profile_slot_0_xuid` set in its config).

## Run loop
1. Start the pad FIRST so Xenia enumerates it: `python tools/xenia_pad.py` (background).
2. Launch Xenia IN THE BACKGROUND (iter-464: automated runs never take the foreground - the user
   works on this PC): `powershell -File tools\bg_window.ps1 -BgLaunch "<xenia_canary.exe>" "<iso>"`
   (STARTUPINFO SW_SHOWNOACTIVATE, windows parked at the bottom of the z-order, 15 s guard that
   hands the foreground back if Xenia takes it; Python: `tools/bg_window.py`). No
   `--apu=nop/--hid=nop`. Reaches press-start in ~90-100s. The virtual pad needs no focus.
3. See: `powershell tools/xenia_shot.ps1` → Read `tools/xenia.png`.
4. Act: write tokens to `tools/pad_cmd.txt`, e.g. `@START`, `DOWN A`, `B`. (START advances press-start.)
5. Inspect: frida-superman attach; scan/watch (below).

> NB keyboard input also works if `keyboard_mode=1` + `keyboard_user_index=0` in the Xenia config,
> but the user normally plays on a real controller (keyboard mapped off by default), so the virtual
> pad is the reliable path.

## Frida / Xenia guest-memory facts (critical)
- **membase = 0x100000000**; host addr = `0x100000000 + guestVA`. Validate: MZ header at `base+0x82000000`.
- **Guest memory is BIG-ENDIAN.** A guest pointer `0x82086FF0` is bytes **`82 08 6f f0`** (NOT `f0 6f 08 82`
  — that LE order is what OUR recomp uses). Scan/compare accordingly. (This byte-order slip caused a
  false "menu never builds" in iter-234/235.)
- Useful signatures: clip vtable `82 08 6f f0`; AS-symbol vtable `82 08 6e 10`; movie blob magic ASCII
  `41 70 74 20 44 61 74 61` ("Apt Data").
- **Guest call stack at a JIT fault** (Xenia x64 JIT ABI, from xenia source): `rsi`=PPCContext*,
  `rdi`=membase. Validate `rsi` via `[rsi+0xA98]==0x100000000` (virtual_membase). BUT context `r[]`/`lr`
  are STALE mid-function — the live guest regs are in HOST regs. To walk the stack: find a host reg
  holding a value in `[0x70000000,0x72000000]` (= live guest `r1`), then read `membase+r1` and extract
  `0x82xxxxxx` values = the guest return-address chain.
- MemoryAccessMonitor gotchas: first-touch per committed page (re-arm to re-watch); the just-written
  VALUE isn't committed at fault time (can't pattern-filter on it); only watches pages committed at
  enable time; a 32MB region during heavy boot writes can FREEZE/crash Xenia — keep regions small and
  disable promptly.

## ★ Hooking a JIT'd guest function on Xenia (iter-362m — the guest-function oracle)

Xenia JIT-compiles each guest PPC function to host x64 once and caches it. You CAN
`Interceptor.attach` the compiled host code and read the guest PPCContext — no Xenia symbols
needed. This is the decisive oracle for "what does guest fn X return on console" (it caught
the 0x80600006 / sub_824C9740 miscompile: same input state, console returns 0, ours returns 6).

**Indirection table (the guest-VA -> host-code map).** `code_cache_base.h`: the table is
`AllocFixed` at host `0x80000000` == `kIndirectionTableBase`, so **guest VA G's slot is at host
address G directly**. Each slot is a `uint32_t` host-code pointer into the generated-code region
`[0xA0000000, 0xB0000000)` (`kGeneratedCodeExecuteBase=0xA0000000`).
```js
const hostCode = ptr(0x824C9740).readU32() >>> 0;   // compiled iff != THUNK (see below)
```
The function must have been called once to be compiled (slot != the default resolve-thunk value).
⚠ **The resolve thunk itself lives INSIDE the generated-code region** in the oracle build
(0xA0000220, seen iter-463 and iter-467), so `hostCode >= 0xA0000000` does NOT mean compiled: a
hook attached on that test sits on the thunk and fires for every unresolved call with garbage
registers. Take the thunk as the most common slot value over the code range (e.g. every 0x2000 in
0x82100000..0x82800000) and attach only when `hostCode !== THUNK` (tools/xenia_audio_capture.js
`findThunk`, tools/devtools/frida_skip.js, tools/xenia_collision_route.js).
Attach ~9s after launch to catch the intro-FMV decoder creates (they fire ~10-13s); the whole
intro's decoders tear down by press-start, so it's per-FMV-init, not per-frame.

**PPCContext ABI at the hook.** `GetContextReg()=rsi`, `GetMembaseReg()=rdi` (x64_emitter.cc).
At `onEnter`, find the context robustly by scanning host regs for `[reg+0xA98]==0x100000000`
(virtual_membase) — rsi is right but the trampoline can shift it.
⚠️ **r[] is NOT at struct-offset 0x20** (the header comment lies — cr0..cr7/fpscr precede it).
Auto-detect r[] base: scan ctx offsets for guest r1 (a stack ptr, `[0x40000000,0x80000000)`,
typically `0x70xxxxxx`); **r[] base = that offset - 8**. Measured base = **0x28**, so
**r0@0x28, r1@0x30, r3@0x40** (i.e. `rN` at `ctx + 0x28 + N*8`). Guest fields are BIG-ENDIAN.
```js
function findCtx(cpu){ for(const c of [cpu.rsi,cpu.rdi,cpu.rbx,cpu.r12,cpu.r13,cpu.r14,cpu.r15,cpu.rbp])
  { try{ if(c.add(0xA98).readU64().toString()==='4294967296') return c; }catch(e){} } return null; }
const RB=0x28; // r[] base; r3 = ctx.add(RB+0x18)
Interceptor.attach(ptr(ptr(0x824C9740).readU32()>>>0), {
  onEnter(){ const ctx=findCtx(this.context); this.ctx=ctx;
    const a1 = ctx.add(RB+0x18).readU64().and(0xFFFFFFFF).toNumber()>>>0;  // guest r3
    // read a1 fields: be32(membase + a1 + idx*4)
  },
  onLeave(){ const rc = this.ctx.add(RB+0x18).readU64().and(0xFFFFFFFF).toNumber()>>>0; } // guest r3 = return
});
```
Guest regs at a function boundary ARE synced to the PPCContext (caller sets r3 there before the
call; the epilogue writes the return r3 back) — so read them from `ctx`, not host regs. Cores are
freed/reused after init, so read the codec-core fields AT CALL TIME in the hook, never post-hoc.
Pad must be launched BEFORE Xenia to be enumerated.

**ISO (updated iter-463):** the user's oracle build `canary_experimental@0bd090dbe` (the exe above,
config `xma_decoder = "new"`, `use_dedicated_xma_thread = true`) runs the FULL sound ISO
`<your disc images>\Superman Returns (USA).iso` past press start into
gameplay with the HUD, no crash, 0 error lines in its log (2 runs, 2026-09-30; evidence in
`archive/v1_audio/oracle/`). The old advice to use `SupermanReturns NO SOUND.iso` dates from the
older build and decoder, which crashed past press start. Launch with
`--log_file=<archive path>` so the user's own `xenia.log` is left alone.
Flow with no profile: PRESS START (~115 s after launch) -> "Select a gamer profile" -> `DOWN A`
(continue without profile) -> opening cinematic -> an orbiting in-engine Superman scene, which
is already gameplay with the idle camera and no HUD. `A` does not leave it (iter-463 live run 1):
`START` opens the pause menu and `A` (Resume) shows Metropolis gameplay with the HUD. There is no separate main menu on this path
(NEW GAME is implicit), and idling on press start or in gameplay drops into attract/idle cameras.

**The 0xE0000000 view is offset by 0x1000.** Xenia's vE0000000 heap has
`host_address_offset = 0x1000` on Windows (memory.cc), so guest VA `X >= 0xE0000000` lives at
host `0x100000000 + X + 0x1000`, not `+ X`. Reading `membase + X` there returns the bytes of
`X - 0x1000`. `tools/golden.py` applies this since iter-463.

**XMA context array (audio oracle).** On this build and ISO, `XmaDecoder::context_data_first_ptr_`
= **`0xFFCA9000`** (last = `0xFFCADFFF`, 320 x 64 B; PA via Xenia's GetPhysicalAddress =
`0x1FCAA000`). Find it on any run by scanning host (non-guest) memory for the little-endian pair
`first, first+0x4FFF` (the two adjacent `uint32` members). The EA DECEAXMA path uses the XMA*
imports; hook their JIT'd thunks through the indirection table at the import-thunk VAs
`0x828EEA4C` (XMADisableContext) ... `0x828EEB3C` (XMACreateContext), 16 B apart in the import
order of the log's import table.

**Full-memory dump (the golden method, scriptable).** Detach Frida first, then:
`cdb.exe -pv -p <pid> -c ".dump /ma /o <path>.DMP; qd"` (Windows Kits x64 debugger, noninvasive,
~3 min, ~13.5 GB; Xenia keeps running). The dumps are read with `tools/golden.py --dump NAME`:
`nosound` (the original `data/xenia_canary.DMP`), `sound_menu`, `sound_gameplay`.

## Watchpoint-at-the-flip (the live-oracle move; TOOLKIT §1.7, HARD RULE in CLAUDE.md)
The golden dump shows the healthy END state; the live watchpoint names **who writes a predicate
and when**. Use it whenever our boot parks on a state the working reference flips (e.g. the SM
state `DAT_829761E0+4`, an FSM gate, a movie-complete flag):
1. `frida_attach target=xenia_canary` — `tools/frida_mcp.py` auto-falls-back to Xenia's membase
   `0x100000000` when the harness export is absent (validated via MZ at `+0x82000000`).
2. `frida_watchpoint guest_va=0x829761E4 size=4 mode=w` (hardware WP, re-armed on new threads).
3. Drive the game to the transition (pad tokens / just wait for the intro→menu flip).
4. `frida_get_hits` → host backtrace lands in Xenia JIT code; recover the guest context per the
   "Guest call stack at a JIT fault" recipe below, and read the surrounding guest memory
   (`frida_read_memory`) at the hit moment.
Caution: hits pause nothing — sample fast state via `frida_eval` in the same session. For
watching many addresses, prefer MemoryAccessMonitor (gotchas below) over burning the 4 HW slots.

## Pipeline found (working menu) — all guest fns our recomp HAS but doesn't drive
- TICK (per-frame, reads+writes parsed blobs): `sub_826BE8C0`, `sub_826ACD18`.
- CONTENT-FETCH chain (the gap our build skips; big contiguous copy into the Apt heap):
  `sub_8279A3C8` <- ... <- `sub_821491A0` (per-frame async-load PUMP) <- `sub_8271ED68` <- `sub_827206B8`.
- RESOURCE worker `sub_8271ED68` dispatches the type-specific load via `resource[+8] -> vtable[0x14]`
  = the `.apt` PARSER for a movie resource (the final hop to pin).

## Next
Catch a guaranteed-fresh `.apt` movie parse (boot-time menu build, or first visit to an unvisited
screen) with a small armed region, or resolve a live movie-resource's `vtable[0x14]` → parser guest
addr → host-drive the fetch + parse in our recomp so DEF_EXEC/aptCount go nonzero.

## The trace-dump ORACLE (iter-359f — the render ground truth)

One command renders a captured .xtr to the pixel-perfect golden frame (headless):

```
reference/xenia-canary/build/bin/Windows/Release/xenia-gpu-d3d12-trace-dump.exe \
  --async_shader_compilation=false data/gpu_traces/menu_pressstart_454107ED_9415.xtr
```

Output PNG lands in CWD (or --trace_dump_path=DIR). Golden committed at
data/gpu_traces/menu_pressstart_oracle.png. DIFF EVERY RENDER ITERATION against it.
The build needs XENIA_BUILD_MISC=ON plus the three headless fixes in
tools/xenia-trace/xenia_trace_tools_headless.patch (null imgui drawer, virtual
ExecutePacket for trace playback, presenter-without-window). Note: the trace's tag-8
EDRAM snapshot is init-only — per-pass EDRAM diffing needs the interactive
trace-viewer, not the dump tool.
