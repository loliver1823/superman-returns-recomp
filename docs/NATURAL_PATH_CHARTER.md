# The Natural-Path Charter

**Established 2026-07-13.** The goal of this project is a **clean native PC port** of Superman
Returns: the game's own recompiled code running on a correctly-implemented host platform, with
**zero forcing**. This document is the constitution for how we work. It exists because the
project accumulated a layer of "forcing" band-aids that faked progress and made honest
measurement impossible. Never again.

---

## 1. The one rule

> **Never make a decision the game's own code should make. Only fix the platform underneath it
> so the game makes that decision itself.**

The dividing question for every change:

> **Is the guest doing this itself and I'm making it *possible* — or am I doing it *for* the guest?**

If you are doing it *for* the guest, it is forcing, and it is banned as a default.

---

## 2. Platform fix vs. forcing (with real examples from this project)

A recompilation ALWAYS needs a host layer — implementing what the Xbox 360 provided and the PC
doesn't is the port itself, not cheating. The line is between **implementing the machine** and
**making the game's decisions**.

| ✅ PLATFORM FIX — allowed | ⛔ FORCING — banned as default |
|---|---|
| Implement the 33 instructions the recompiler dropped | Kernel thread calls `sub_822A8C68` to advance the splash |
| Host critical-section locking (Xbox CS primitives) | Write `state=4` into the FE state machine by hand |
| TLS / guest-memory bridging across host threads | Mint a fake EB80 dispatcher + vtable because the real one is null |
| GPU PM4 command buffers → D3D12 | Substitute movie names into the real queue |
| Missing kernel imports (`Nt*`, `Xam*`, `Ke*`) | Bump `present`/`defExec`/`aptCtx` to make a metric move |
| **`NtAllocateVirtualMemory` honoring the requested base** (killed the year-old scribble-corruption family at its root — **THE model**) | Drive the FE tick / executor / screen pump from a kernel thread |

**The two paradigms live side-by-side in this project's history.** The scribble bug: we found
the root (allocator ignored the requested address), fixed that one platform bug, and the game
*stopped corrupting itself naturally*. The fabricator: instead of finding why the dispatcher
globals were null, we minted fake ones and drove them by hand. **Always do the former.**

### 2a. Player-action direct calls (USER DECISION 2026-10-06)

"Any PC controller bindings should be a remapper, it should call the functions directly. That goes for
the cheats as well, always call functions directly, no virtual presses or mappings." So a port feature
that carries out an explicit player input may call the game's own function for that action, instead of
synthesising virtual pad presses:

- **Allowed:** a bound key calls the game's own action function (the one its pad path would reach) with
  the same arguments; a CHEATS row calls the cheat's own effect function after the game's own condition
  check; the mouse UI calls the menu's own select/focus functions.
- **Required:** the same function and arguments as the game's path, the game's own conditions honoured,
  an OVERRIDES.json entry (`port-feature`), off on natural rolls (port features off), and a test that the
  direct call and the game's own path produce the same result.
- **Still forcing:** any call the player did not ask for - progression, objectives, state the game would
  not reach from that input. Test harnesses (sr_drive, tapes) keep injecting pad state.

---

## 3. Enforcement (structural — so it cannot rot again)

The previous cleanup plan (`BANDAID_BURNDOWN.md`) failed for one reason: retirement was
**optional and defaulted to keep-everything** (`SR_T2_KEEP=ALL`), and one band-aid (the
fabricator) wasn't even under the kill-switch. Structure fixes that:

1. **One master gate: `SR_ALLOW_FORCING`, default OFF.** Every forcing path checks it. No
   forcing may be ungated. The default build forces NOTHING.
2. **Forcing is a diagnostic, not a crutch.** It may be switched on only as a *named, opt-in*
   probe to reveal the platform gap it hides. Lifecycle for every band-aid:
   **force (diagnostic) → diagnose the gap → root-fix the platform → confirm natural → DELETE the forcing.**
3. **Attribution is mandatory.** Never report a metric as progress without proving which code
   caused it. Tag every result **natural** or **forced (diagnostic)**. A forced result is
   investigation, never progress.
4. **One true number.** `docs/NATURAL_BASELINE.md` is the sole progress metric: how far the
   natural (forcing-off) game reaches. It advances only when the game genuinely does more itself.
5. **Host launches are forcing surface too (iter-463).** A host thread, timer, VEH or atexit
   callback that writes guest memory, calls guest code, allocates from the guest heap or
   signals a guest-visible object does something the guest did not ask for, however it is
   gated inside. The override registry never saw this class. Two such threads ran on every
   natural roll: `cfgSoundOffThread` zeroed the Sound config flag, and the MODGLOB poller
   armed over the manager globals. Meanwhile `audit_forcing.py` printed "no UNGATED".
   `scripts/audit_host_threads.py` now runs inside `audit_forcing.py` and fails on any such
   launch unless one of three things holds. Either the **launch itself** sits behind
   `forcingAllowed()` (a gate inside the thread body does not count, because a default run
   should start nothing), or the launch is registered in `docs/HOST_THREADS.json` "allow" as
   `platform` or `diagnostic` with a reason, or it is a **declared band-aid** in
   `docs/HOST_THREADS.json` "declared": a reason it cannot go yet, a retirement condition and
   a pointer to its `docs/BANDAIDS.md` row are all required, and the audit prints every
   declared entry on every run so it can never go quiet. A declared entry means the natural
   baseline is "natural except that one" and every baseline entry must say so. At the iter-463
   close exactly one is declared: `cfgSoundOffThread` (cfg-sound-off, the game is silent),
   waiting on the audio stage (per-thread KPCR + real Ke/Kf spinlocks). Platform means the console provides the
   behaviour: the CP, the vblank ISR, the XAudio callback, the KeTimeStampBundle clock,
   XNotify deliveries. Forcing is never allowlisted. Open forcing verdicts sit in
   `open_forcing`, and the audit fails until each one is gated or deleted.

---

## 4. For the human: how to check you're NOT being gaslit

You never have to trust a progress report again. The system is built so you can verify it
yourself, without believing anyone:

1. **Run the default build.** With `SR_ALLOW_FORCING` off (the default), the game forces
   nothing. Whatever you see on screen or in the log is 100% the game's own behavior. Nothing is
   driven by hand, nothing is faked.
2. **Read `docs/NATURAL_BASELINE.md`.** It states, with a date and commit hash, the furthest the
   natural game has reached. That number can only go up when the game genuinely does more. If a
   session claims progress, this file must show it — and you can reproduce it.
3. **Demand attribution.** Any progress claim must say **"natural"** and cite the baseline. If a
   result is tagged **"forced"** or **"diagnostic,"** it is NOT progress — it is a probe into a
   gap. Reports that conflate the two are the exact failure this charter exists to prevent.
4. **The smell test:** if a claim can't be reproduced by you running the default build, treat it
   as unproven until it can.

---

## 5. For the agent: the working loop under this charter

1. Take the natural baseline as the reference (forcing off). Where the game stops IS the frontier.
2. Attack the FIRST natural gap. Ask: *what mechanism should do this, and why doesn't it?*
3. If you must, turn ON one named forcing diagnostic to see what the game does differently — this
   reveals the missing mechanism. Report it as **forced/diagnostic**, never as progress.
4. Find the platform gap or real bug behind it. Root-fix the platform.
5. Confirm the game now does it naturally (baseline advances). **Delete the forcing band-aid.**
6. Update `docs/NATURAL_BASELINE.md` with the new honest frontier + commit.
7. Never re-enable a deleted forcing to "get past" something. If the natural path stops, that stop
   is the work — name it, don't paper it.

---

## 6. Migration from the forced era (in progress)

- [ ] Land the `SR_ALLOW_FORCING` master gate over every forcing path (incl. the fabricator).
- [ ] Take the true baseline (all forcing off) → record in `NATURAL_BASELINE.md`.
- [ ] Convert `BANDAID_BURNDOWN.md` entries: each becomes a "diagnostic → root-fix → delete" item.
- [ ] Re-audit `BANDAIDS.md` class-B "keep" list against §2 — confirm each is a true platform fix.
- [ ] Delete forcing band-aids as their natural roots land; never on a schedule, always on proof.
