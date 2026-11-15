# Lessons from Other Xbox 360 Recomps — Applicable Fixes

**Date:** 2026-06-12 (post-iter-189)
**Purpose:** Survey of every active Xbox 360 recompilation project, compared against our harness, with concrete fixes/techniques we can apply. Ordered by impact on the current blocker (frontend menu never constructs; file I/O stops after `DATA/TEXT.AST`).

---

## 1. State of the field (June 2026)

| Project | Lineage | Status | Relevance to us |
|---|---|---|---|
| [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) | hedge-dev | **Dormant** — last commit Aug 4 2025 (`ddd128bc`), which is exactly our vendored copy in `reference/XenonRecomp` | No upstream codegen fixes to pull. Our copy IS upstream HEAD. |
| [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) (Sonic Unleashed) | XenonRecomp | Maintained; recent commits are Sonic-specific gameplay fixes (HFR etc.). Our snapshot: Feb 1 2026 | Best reference for: heap/allocator hooks, midasm hooks, function-hook machinery, XAM. **Not** a reference for Nt-level file I/O (see §3). |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) | Xenia kernel + AOT codegen | Active; latest tag **v0.8.0 (2026-05-16)**, nightly `0.8.1.68-dev` (2026-06-28); still self-described "early development, expect breaking public API updates" | Re-checked **2026-07-16 — decision: do not migrate** stands (rationale inline at §6). Now the *only* actively-maintained AOT PPC codegen (XenonRecomp dormant) → keep as a **diff-oracle** for suspected codegen bugs, not a base to switch to. |
| [VivaPinataRecomp](https://github.com/VivaPinataRecomp/VivaPinataRecomp) + TiP-Recomp | ReXGlue | Shipped/playable | Proof that "adopt Xenia's GPU command processor wholesale" works — but only practical because ReXGlue's kernel IS Xenia's. Ours is not. |
| OpenMBU (Marble Blast Ultra) | source-port-like | Shipped | Not a recomp in our sense; not relevant. |

Survey roundup: [The Age of Recomps — RetroLunch, 2026-03-31](https://retrolunch.com/blog/posts/2026-03-31-pc-recomps.html).

---

## 2. Finding #1 — NtReadFile completion contract (ties to the #1 open question)

**The real NT contract:**
`NtReadFile(r3=FileHandle, r4=Event, r5=ApcRoutine, r6=ApcContext, r7=IoStatusBlock, r8=Buffer, r9=Length, r10=ByteOffset)`

**Our gap:** `stub_NtReadFile` ([kernel/stubs.cpp:1107](../kernel/stubs.cpp)) **never signals the Event (r4) and never queues the completion APC (r5/r6)**. It reads synchronously, writes the IOSB, returns success.

**Why it matters here:** this engine is APC-driven — the FM workers are woken via `NtQueueApcThread` (iter-43). If DOGMA's streaming core (FMStreamingCore/FMTaskManager) issues an async read and then sleeps until the completion APC fires, it stalls forever after the first synchronous read. That is exactly the iter-189 symptom: *exactly one file ever loaded (`DATA/TEXT.AST`), menu never constructs because the FE package never streams in.*

**Reference implementation:** Xenia `src/xenia/gpu/../kernel/xboxkrnl/xboxkrnl_io.cc` (local: `reference/xenia/src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc`) — it sets the event and queues the guest APC **even when the read completes synchronously**. UnleashedRecomp is NOT a reference here: its `NtReadFile` is an empty stub ([imports.cpp:609](../reference/UnleashedRecomp/UnleashedRecomp/kernel/imports.cpp)) because Sonic Unleashed does file I/O through the Win32-style `X*` APIs (`file_system.cpp`) instead.

**Next-session action:** instrument `stub_NtReadFile` to log r4 (event) and r5 (APC routine) on every call. If the TEXT.AST read — or any read attempted during the menu pump — passes non-zero values, implement completion signaling per Xenia (signal event via our handle table; queue guest APC to the issuing thread so it fires on the next alertable wait).

---

## 3. Finding #2 — Latent scribbler in stub_NtReadFile's arg heuristic

Our "standard" mapping assumes `r4=IoStatusBlock, r5=Buffer, r6=Length` — but in the real NT convention r4 is an Event **handle** and r5 is an APC **code address** (0x82xxxxxx). The extended-convention detection at [kernel/stubs.cpp:1133](../kernel/stubs.cpp) requires `bufVA==0 && len==0` to engage.

**Failure mode:** a caller using the real convention with non-zero Event/ApcRoutine fails the extended detection (r5 ≠ 0), so we:
- `fread` file data **into the APC routine's code address** (image .text!), and
- write status words at `VA = event handle value`.

Same bug class as the iter-77→189 scribblers. It hasn't fired only because current callers happen to match the heuristics. **Harden regardless of Finding #1:** detect the real convention first (r4 == 0 or small handle-range value, r5 in image code range, r7 pointing at a plausible IOSB).

---

## 4. Finding #3 — Stop fabricating allocator internals; hook the allocator functions (UnleashedRecomp pattern)

**What UR does right** ([kernel/heap.cpp](../reference/UnleashedRecomp/UnleashedRecomp/kernel/heap.cpp)):
- `GUEST_FUNCTION_HOOK(sub_82BD7D30, RtlAllocateHeap)` etc. — hooks the **game's own statically-linked CRT heap functions** at the function-table level.
- Serves every allocation from an o1heap arena **inside guest address space** (user heap at guest 0x20000–0x7FEA0000; physical heap above 0xA0000000).
- Honors the alignment encoded in `XAllocMem` flags (`1 << ((flags >> 24) & 0xF)`); zeroes on flag 0x8 / 0x40000000.

**What we do instead:** fabricate the *internals* of DOGMA's dlmalloc-style heap — bins, top-chunk pointer, fallback critsec — plus a 1MB fake heap at guest VA 0x10000 (`kFallbackHeapBase`, [kernel/guest_context.cpp:2521](../kernel/guest_context.cpp)). Guest allocator code then *walks* our fabricated structures. This is where iter-189's "junk from birth" deques lived (allocs at 0x6xxxx → deque::erase tail-copy scribbler → DEQUE_ERASE_GUARD).

**Applicable fix:** identify DOGMA's malloc/free/realloc entry points in Ghidra and hook them at the function table, serving from a real dedicated guest region. Our ABI is identical to UR's (`void fn(PPCContext& ctx, uint8_t* base)`), so their arg-translation machinery ([kernel/function.h](../reference/UnleashedRecomp/UnleashedRecomp/kernel/function.h)) ports directly. This eliminates the fabricated-heap-walk bug class instead of guarding each symptom.

Note: post-iter-189 self-driven boot means the game initializes its own heap in healthy runs — the fallback engages only in degenerate paths (allocator singleton null). Every engagement mints junk; priority is below Findings 1/2 until the fallback shows up in another corruption chain.

---

## 5. Finding #4 — Midasm hooks instead of hand-editing generated files

XenonRecomp supports `[[midasm_hook]]` in the TOML (struct `RecompilerMidAsmHook`, [recompiler_config.h](../reference/XenonRecomp/XenonRecomp/recompiler_config.h)): inject a call to a named C++ function at an exact guest address, with `registers = [...]` passed in and `ret` / `returnOnTrue` / `returnOnFalse` / `jump_address` semantics. UnleashedRecomp ships dozens (graphics, input, HFR).

**Our problem:** our surgical guest patches are hand edits to *generated* files, which a XenonRecomp regen silently erases — and [guest/config/superman.toml](../guest/config/superman.toml) comments show we expect regens ("should auto-translate on next regen"):
- DEQUE_ERASE_GUARD — edited into `ppc_recomp.46.cpp` (iter-189h)
- iter-77 FMUI pool-worker shutdown restoration (env gate `SR_KEEP_FMUI_POOL_WORKER`) — `ppc_recomp.95.cpp`
- ED68 null-r31 guard (iter-148)

**Applicable fix:** port each to a `[[midasm_hook]]` entry implemented in a new `kernel/guest_patches.cpp`. Example for the deque guard:

```toml
[[midasm_hook]]
name = "DequeEraseGuard"
address = 0x82357EB4          # before the sub_8279F130 copy call
registers = ["r3", "r4", "r5"]
return_on_true = true          # skip the copy when the hook returns true
```

Regen-proof, self-documenting, and reviewable in one place.

---

## 6. Where we are at parity or ahead (no action)

- **GPU.** Two proven roads exist: UR replaces the renderer at the game-API level (never touches PM4); ReXGlue/VivaPinata bolt on Xenia's entire command processor. Our custom PM4 → D3D12 middle path is unproven elsewhere but **working** (60Hz presents, real EDRAM resolves, iter-159/185+). Current blocker is game progression, not GPU. Stay the course; crib individual PM4 packet handlers from Xenia `src/xenia/gpu/command_processor.cc` as real draws start flowing. Escape hatches if the PM4 path walls: UR-style high-level replacement of DOGMA's render layer (Xenia GPU adoption would be costly for us since our kernel isn't Xenia's).
- **Indirect dispatch safety.** Our `PPC_LOOKUP_FUNC_SAFE` (range-checked, iter-189) is *ahead* of both stock XenonRecomp and UR, which crash on garbage function pointers.
- **XAM sign-in.** Our faked signed-in state + one-shot `XN_SYS_SIGNINCHANGED` ([kernel/stubs.cpp:3291](../kernel/stubs.cpp)) matches UR's behavior. UR's persistent notification queue (`XamNotifyEnqueueEvent`) is marginally cleaner; adopt only if a consumer needs repeated notifications.
- **Recompiler config.** Save/restore addresses, switch tables, explicit function boundaries — all set correctly in `superman.toml` (see [reference/XENONRECOMP_CONFIG.md](../reference/XENONRECOMP_CONFIG.md)).
- **ReXGlue migration.** Re-confirmed **2026-07-16: no.** Both reconsider triggers remain **unmet — and are moving away, not toward, met:**
  1. *"ReXGlue API stabilizes + another shipped title"* — the opposite happened. v0.8.0 (May 2026) → nightly (Jun 2026) landed **more** breaking `!` changes (Xenos GPU extracted to a runtime `rexgpu-xenos` plugin; Win32/GTK → SDL3; manifest-based project rework; per-module dispatcher). README still warns of "significant changes and breaking public API updates." No new shipped title beyond VivaPinata/TiP.
  2. *"unavoidable guest SEH"* — not hit. Our XenonRecomp codegen is **instruction-complete for this XEX** (33 dropped instrs hand-implemented, regen pipeline verified — `tools/regen/`), and the live blocker is a game-logic/host-trigger gap (nothing issues the frontend UI-movie load), **not** a codegen limitation a different recompiler would fix.
  - **Why switching is strictly negative for us:** ReXGlue's leverage *is* that its kernel is Xenia's (lets adopters bolt on Xenia's whole GPU command processor — the VivaPinata road). Ours is a bespoke harness (custom PM4→D3D12, hundreds of kernel shims, the natural-path charter architecture, XNotify/critsec/RTAM). Migrating means either discarding that harness for Xenia's, or re-porting every shim onto a still-churning ABI — a multi-month rewrite that touches the blocker **zero**. We already generated a ReXGlue trial from the same XEX (`guest/ppc_rexglue/`, gitignored) and shelved it; nothing since flips the math.
  - **What we *do* keep from ReXGlue:** it's now the only actively-maintained AOT PPC codegen, so use it as a **cross-check oracle** — if a specific function looks miscompiled, regen just that function via ReXGlue and diff against ours (nightly already fixed e.g. "recover function tail after a conditional bcctr"). Consult, don't switch.
  - **"Does the latest ReXGlue drop Xenia?" (checked 2026-07-16, 5-source sweep, high confidence): NO at the layer that matters.** The claim traces to a 2026-07-10 YouTube video ("RexGlue is Changing; No More Xenia") that overstates reality. Actual state: **(kernel) STILL Xenia-derived** — founder interview: *"We inherited Xenia's kernel layer and have heavily modified it"*; `src/kernel/{xboxkrnl,xam,xbdm}` is Xenia's HLE verbatim re-namespaced `rex::`, compiled into `rexruntime`; no from-scratch kernel in any commit/PR. README (July 2026) still: *"ReXGlue is heavily rooted on the foundations of Xenia."* **(GPU) Xenia's Xenos code was EXTRACTED into an optional runtime plugin `rexgpu-xenos` behind an `IGraphicsSystem` ABI** (nightly commit `c22a33e`, **dev-only — NOT in v0.8.0 stable**) — the plugin is still *"ported from Xenia"* verbatim and the core still requires it at runtime. So it's *modularization, not de-Xenia-fication*; both layers remain Xenia-lineage.
  - **The one genuinely interesting bit for us:** the `IGraphicsSystem` plugin boundary + v0.7.3 *"Xenos-Less Support"* (runtime can boot with no GraphicsSystem, for native renderers) means ReXGlue is now architecturally able to *host an external native renderer* — i.e. our PM4→D3D12 could in principle slot in as a `rexgpu-*` plugin. That makes ReXGlue a better **host for our GPU**, not a source of a better GPU — and taking it would still put our renderer on top of a Xenia-lineage kernel, which collides with the clean-native-port charter. Net verdict unchanged: bespoke-primary + ReXGlue-as-reference/interface, never Xenia-kernel-underneath.

---

## 7. Critical sections — how the complete projects do it (for the iter-190 critsec fix)

**Where we are:** iter-190 helpers are landed but not wired ([kernel/stubs.cpp:2924-2958](../kernel/stubs.cpp): `csLegacyFakeSuccessMode`, `csCanonicalKey`, `csOutOfRangeUseLegacy` with 1024-key cap) — the three stubs still use the old inline pretend-success filters and key `getOrCreateCS` on **raw** r3, not the canonical VA. Our model: host-side `std::unordered_map<guestVA, HostCriticalSection>` (shadow state invisible to the guest), `try_lock` + `Sleep(1)` poll, 10s "abandoned" recovery that **destroys a locked `std::mutex`** (UB) and silently corrupts lock state.

**What UR and Xenia both do instead: all lock state lives in the guest struct itself. There is no host map.**

UnleashedRecomp (~45 lines total, [imports.cpp:679-710, 742-750, 1201-1225](../reference/UnleashedRecomp/UnleashedRecomp/kernel/imports.cpp)):
```cpp
void RtlEnterCriticalSection(XRTL_CRITICAL_SECTION* cs) {
    uint32_t thisThread = g_ppcContext->r13.u32;          // guest thread identity
    std::atomic_ref owningThread(cs->OwningThread);
    while (true) {
        uint32_t previousOwner = 0;
        if (owningThread.compare_exchange_weak(previousOwner, thisThread)
            || previousOwner == thisThread) {              // recursion: owner check FIRST
            cs->RecursionCount++;
            return;
        }
        owningThread.wait(previousOwner);                  // futex on the guest field
    }
}
// Leave: --RecursionCount; if 0 { owner.store(0); owner.notify_one(); }
```

Xenia ([xboxkrnl_rtl.cc:352-497](../reference/xenia/src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc)) implements the exact 28-byte XDK layout (`lock_count` @0x10 host-order, `recursion_count` @0x14 BE, `owning_thread` @0x18 BE) with spin-count + event-based waiters. Its comment is the key insight for why guest-struct state is mandatory: *"some games have the critical sections pre-initialized in their embedded data and InitializeCriticalSection will never be called."*

### Why guest-struct state dissolves the iter-190 design problems

| iter-190 problem | host-map solution (in flight) | guest-struct solution |
|---|---|---|
| Cached/uncached alias (0x82… vs 0xC2…) hitting different locks | `csCanonicalKey` aliasing logic | Free — both VAs map to the same host bytes already; the CAS lands on the same cell |
| Garbage storm minting unbounded map entries | 1024-key cap + `unordered_set` | No map → nothing to mint. Keep the range/AV filter for truly unmapped VAs only; add `(r3 & 3)` alignment check |
| Enter/Leave pairing across legacy/real boundary | "keys admitted before cap keep real locking" bookkeeping | Disappears — state is in the bytes; any path that reaches them is paired by construction |
| Dead thread holding locks | `releaseDeadThreadCSes` walks the whole map | Keep a tiny per-thread vector of held CS VAs (push/pop in Enter/Leave); on death, CAS-clear those owners. O(held), no map |
| 10s abandoned recovery | recreate `HostCriticalSection` (UB on locked mutex) | Logged forced owner-clear via CAS — safe, atomic, guest-visible |
| Fabricated "initialized" critsecs (`kFallbackCritSect`, heap+1408, device+13524) | seeded host map entries | With **zero-owner = unlocked** protocol, zeroed memory IS a valid unlocked critsec. Fabricated regions and post-zero-sweep .data self-heal to "unlocked" |
| `Sleep(1)` contention poll (1ms/handoff; suspect in 16s-vs-30min init variance per the iter-190 comment) | unchanged | `WaitOnAddress`/`atomic_ref::wait` on the owner cell (µs handoff), `notify_one` on release. Use timed waits to keep CRITSEC_STALL logging |

### Design decisions for our port

- **Protocol: UR's zero-owner CAS, at XDK field offsets** (owner @0x18, recursion @0x14), fields stored **big-endian** (CAS works on raw bits; encode both compare and exchange values BE) so guest code that *reads* the struct (the cs_timeout_underflow decrementer!) sees plausible values. Prefer this over Xenia's `lock_count = -1` protocol: zero-initialized = unlocked is self-healing in our environment.
- **Escape clause:** if Ghidra ever shows the game *inlining* the critsec fast path (`lwarx/stwcx` on cs+0x10 in game code, kernel called only on contention), switch to Xenia's exact lock_count protocol — guest-struct state is then the *only* correct option, host maps can never interoperate. Worth one decompile of cs_timeout_underflow's target function during implementation.
- **Owner identity: r13 is 0 in our build** ([guest_harness.cpp:692-697](../guest/guest_harness.cpp); [fm_world_ptr_patcher.cpp:9](../kernel/fm_world_ptr_patcher.cpp) documents it), so UR's `r13` recipe needs adapting. Use `getCurrentThreadHandle()` (guarantee nonzero — host helper threads like the audio driver that dispatch guest code need a synthetic id too). Longer term: allocate a real per-thread PCR and set r13 like UR does — note `ppc_recomp.0.cpp:7917/8050` show guest code loading `r13+256` (KeGetCurrentThread on a real PCR); with r13=0 those reads hit fabricated low memory today.
- **Keep our hardening that the complete projects lack** (they crash on garbage pointers; we can't afford to): null check, unmapped-VA filter, spin-monitor instrumentation (`cs_spin_instr`), STALL/ABANDONED logging, `SR_CRITSEC_FAKE_SUCCESS` valve.
- **Spinlocks are currently no-ops** — `KfAcquireSpinLock`/`KeAcquireSpinLockAtRaisedIrql` → `stub_generic_success` ([stubs.cpp:3661-3663](../kernel/stubs.cpp)). UR implements them as guest-field CAS + yield ([imports.cpp:757-775](../reference/UnleashedRecomp/UnleashedRecomp/kernel/imports.cpp)). Every spinlock-protected region in DOGMA currently runs with zero mutual exclusion across our live guest threads — a candidate contributor to residual .data corruption. Fix rides along: same protocol, same identity, ~10 lines.

### Band-aids likely retired by this (retest after)

- `cs_timeout_underflow_fix.cpp` — guest decrements a field in a struct we never maintained; truthful guest fields may end the underflow at the root.
- `crt_file_lock_init_fix.cpp` — lazy-init CS slots become natively valid under zero-owner protocol.
- The 10s force-recreate and `releaseDeadThreadCSes` map walk (replaced per table above).
- Fabricated critsec seeds in `guest_context.cpp` (zeroed = already valid).

---

## 8. Recommended order

0. **§7 critsec rewrite** (already declared next, iter-190): implement guest-struct locking per §7 — it *replaces* rather than completes the in-flight host-map rewiring, and brings the spinlock fix along.
1. **Findings 1+2 together** (one investigation): instrument `stub_NtReadFile` r4/r5 on the next run, during boot AND during the menu pump. If async params appear → implement Xenia-style completion. Harden the arg-convention detection either way. *Directly attacks the menu-construction blocker.*
2. **Finding 4** (midasm hooks): cheap insurance; do before the next planned XenonRecomp regen.
3. **Finding 3** (allocator hooks): structural cleanup; schedule when the 0x10000 fallback heap next appears in a corruption chain.

---

## 9. RecompCore / ModernGekko (GameCube/Wii, July 2026) — techniques worth stealing

**Source:** [reddit r/decomps 2026-07](https://www.reddit.com/r/decomps/comments/1ux4nyk/) + [RecompCore dev-history docs](https://github.com/aharonahdoot/RecompCore/tree/main/docs/development-history) (verbatim agent working docs — an LLM-driven GC/Wii recomp, DolRecomp CPU output hosted inside Dolphin as a new "StaticRecomp" CPU core with interpreter fallback; Super Mario Strikers black-screen → playable in ~3 weeks; since adopted as ModernGekko's engine by the DolRecomp authors). Same working model as ours (human steers, Claude drives, forced knowledge-base read/append per session, checkpoint ledger) — independent validation that the approach ships.

**Strategic note (why they were faster, and why we don't copy it):** their core philosophy is *"don't rebuild the console"* — Dolphin supplies kernel/GPU/audio/timing, recompiled code slots in as a CPU core, anything uncovered falls back to the interpreter. That's the ReXGlue/VivaPinata road (§6) in GC clothing; for us it means Xenia-kernel-underneath, which collides with the clean-native-port charter. The charter-compliant translation is: **use the mature emulator much harder as an *oracle*, never as the chassis.**

### Finding #5 — Lockstep/differential verification of recompiled code (their killer technique)

Every recompiled block re-executed on Dolphin's interpreter, diffed register-by-register and memory-write-by-memory-write → located real DolRecomp codegen bugs (e.g. scalar `lfs` not mirroring the paired-single ps1 lane → later `psq_st` stored stale values), then proved the recompiler clean over a 24.3-billion-dispatch boot→match run. Their maxim: *"the recomp is the implementation under test; its own output cannot establish intended behavior."*

**Our gap:** instruction-*completeness* is proven (33 dropped instrs, `tools/regen/`), instruction-*semantics* are not. A miscompiled CR/XER/carry/FPSCR path would today surface only as distant downstream weirdness — exactly the class we can't currently attribute.
**Cheap first step (no Xenia embedding needed):** per-function differential harness using Ghidra's PCode emulator (`mcp__ghidra__emulate_function` already wired): seed identical register/memory state, run suspect `sub_XXXXXXXX` both ways, diff `PPCContext` + memory writes. Use on-demand when a function is suspected miscompiled (pairs with the ReXGlue diff-oracle, §6). A fuller Xenia-lockstep is possible but is real infrastructure — only justify it if semantic codegen bugs start showing up.

### Finding #6 — Two complementary oracles: run it live, and read the emulator's source

They codified an "evidence hierarchy": (1) real game in emulator at the relevant sequence, (2) emulator debugger watchpoints + hardware source, (3) recomp-side artifacts, (4) decomp/static reasoning — lower levels design experiments, never overrule higher ones. We use the **static** golden dump heavily; the upgrade is the **live** oracle: run Xenia with its debugger at the exact divergence frontier and watch *who writes the predicate at the moment it flips* — e.g. the FE splash/attract state (`DAT_829761E0`+4) and the state-40 movie-completion wait: watchpoint it in a live Xenia session at the moment the menu appears, and the writer names the exact platform event we fail to raise. Complement: read Xenia's source for that subsystem (XMV decode completion, XNotify delivery) *before* theorizing about our own.

### Finding #7 — GPU trace replay decouples renderer work from game-progression walls

Dolphin FifoCI replays recorded CPU→GPU FIFO traffic and diffs pixels, so renderer work never waits on gameplay reaching the scene. **Xenia has the same machinery for Xenos: `--trace_gpu_stream` + xenia-gpu-trace-viewer.** Capture a golden GPU trace of the Superman menu in Xenia, replay the PM4 stream against our PM4→D3D12 backend, diff frames. This serves the approved "menu on screen" direction *today*, independent of the state-40 trigger wall — the render path can be built and regression-tested against golden traces before the game naturally issues those draws.

### Finding #8 — Mechanical anti-spiral rule (their §4a; adopt into our loop)

Their hard rule fixes the failure mode our "3 attempts → stop" only half-covers: a spiraling agent never counts its own spirals, so the trigger must live in the *tool stream*, not self-awareness. The rule: after each observation, hypotheses must be written as table rows — `H<n>: <claim> | KILLS IT: <experiment> | COST: <one rebuild+run / one grep / new tooling>` — and **if any row costs ≤ one rebuild+run, writing further analysis is forbidden; run that killer.** A row without a KILLS-IT experiment is malformed by definition. They replaced ~30% of a session's tokens of blend-algebra derivation with two force-override runs this way. Also worth copying: explicitly record *rejected* hypotheses (with the evidence that killed them) so no future session re-chases them — our RAG does this for fixes, less so for dead theories.

### At parity / validation (no action)

- **Diagnostic forcing** — their "force-override and LOOK" (env-gated, reversible, observe the real system) is precisely our charter's forced-diagnostic model, independently converged on.
- **Knowledge discipline** — forced session-start read + session-end append of a knowledge base, checkpoint ledger, "observe first before theorizing": all already ours (ENGINE_MAP.md / PROGRESS.md / RAG / auto-memory).
- **Layer attribution before touching the layer** — their §0 ("a renderer ticket requires a discriminator artifact first") = our mandatory attribution check.
- **Big-endian float traps, timing-as-one-clock-domain, dispatch-tax-masquerading-as-game-bug** — all lessons we've independently paid for (their VI/AI cadence mismatch ≙ our fab-pump tick-stall rule).

---

## Sources

- [XenonRecomp — hedge-dev](https://github.com/hedge-dev/XenonRecomp)
- [UnleashedRecomp — hedge-dev](https://github.com/hedge-dev/UnleashedRecomp)
- [ReXGlue interview — readonlymemo](https://readonlymemo.com/rexglue-xbox-360-recompilation-interview/)
- [rexglue-sdk](https://github.com/rexglue/rexglue-sdk)
- [VivaPinataRecomp](https://github.com/VivaPinataRecomp/VivaPinataRecomp)
- [The Age of Recomps — RetroLunch](https://retrolunch.com/blog/posts/2026-03-31-pc-recomps.html)
- [How I vibecoded playable GameCube/Wii recompilation — r/decomps](https://www.reddit.com/r/decomps/comments/1ux4nyk/how_i_vibecoded_playable_gamecubewii/)
- [RecompCore](https://github.com/aharonahdoot/RecompCore) / [GXRuntime](https://github.com/aharonahdoot/GXRuntime) / [ModernGekko](https://github.com/ExpansionPak/ModernGekko)
- [RecompCore development-history (agent knowledge base: methodology.md, dolphin-ground-truth.md)](https://github.com/aharonahdoot/RecompCore/tree/main/docs/development-history)
