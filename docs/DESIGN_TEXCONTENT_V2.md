# DESIGN — Texture Content v2: retiring the permanent capture dedupe (iter-456g mandate)

Status: **THIRD ADJUDICATION APPLIED (2026-08-30) — the ledger's RE-PLAN
VERDICT (sha dcc4e938db019bc5) supersedes §7 and mandates the corrections
marked ⛔CORRECTED below; its 8-slice EXECUTION PLAN is controlling. Class A
and Class B remain independently approved behind default-OFF gates; Class C is
CONDITIONAL on a proven load-bearing direct-written version. Bake bar and
retirement restrictions intact.**

Prior status: **REVIEWED — APPROVED WITH REQUIRED AMENDMENTS** (second director verdict
2026-08-30, recorded verbatim in the ledger §"DESIGN REVIEW VERDICT", sha
b44-prefix in ledger; execution ONLY per its 6-slice EXECUTION PLAN — the
amendments SUPERSEDE §3-§5 where they conflict: per-version provenance/epoch
model replaces the per-key taxonomy; resolve ownership is a versioned state
machine, not a key exemption; the Class-C ranked signal is the COHER_*_HOST
triplet at WAIT_REG_MEM, not PM4_INVALIDATE_STATE; REFRESH_FROM gates
PUBLICATION only. Original text below retained for history.) (first verdict
2026-08-30, recorded verbatim in `docs/CLAIMS_LEDGER.md` §"VERDICT iter-456
family", sha b737759ec2a1a191). Nothing in this document authorizes a default
change; every mechanism ships behind an opt-in `SR_*` gate until the §6
evidence bar is carried in full.

## 1. Problem

⛔CORRECTED (third adjudication): the defect is structurally wrong and proven
load-bearing **for the glyph atlas**. The whiten-zero replay proves the
all-zero set affects the FIXTURE'S LOOK — it does NOT prove that stale content
causes the flat/facade divergence. The original wording ("proven load-bearing
by the iter-456e/f arc") is struck.

The HLE renderer's texture-content acquisition has one structural defect:
**bind-time capture is
once-per-tuple with permanent dedupe** (`s_texCaptured.insert`, kernel/
hle_api_observe.cpp), so any texture whose guest memory is written AFTER its
first bind renders its first-capture content forever. The `[TEXUP_ZERO]`
census names 29 all-zero captured uploads on the title fixture
(`archive/iter456_title_fresh.srht`); the `SR_TEXSLOT_WHITE=zero` same-frame
replay A/B (director-re-run) proves the class gates the backdrop's appearance.
The glyph-atlas white-boxes bug was the same class (fixed for k_8 by the
iter-455n unlock-snapshot + iter-456 registration; every other format still
rides the permanently-deduped path).

## 2. The reference model (the design's north star)

`reference/UnleashedRecomp/UnleashedRecomp/gpu/video.cpp`:

- `LockTextureRect` (2168): host-owned staging (`mappedMemory`, host-chosen
  pitch), pointer handed to the guest.
- `UnlockTextureRect`/`ProcUnlockTextureRect` (2180/2190): on EVERY unlock,
  memcpy the WHOLE slice into the upload allocator and `copyTextureRegion` to
  the GPU texture. **Unconditional. No hash. No dedupe. No format gate.**
- Because the reference REPLACES D3D9, every texture fill in its title goes
  through Lock/Unlock — side-channel writes cannot exist. That is why
  "unconditional unlock upload" is a COMPLETE policy there.

## 3. Why it is not complete here (delegation deltas + measured taxonomy)

We DELEGATE to the guest's own D3D9 (ENGINE_MAP §6g.2): guest owns the pixels,
pitch is guest-fixed, and — the key delta — **content can enter texture memory
without any D3D9 call**. Measured on the preserved roll-5 live log (240 s,
~2,000 unlocks): the unlock population is movie planes + atlas (k_8, already
served) + exactly 8 non-k_8 unlocks (the six TGA UI textures fmt 0x18280186
and two fmt 0x1A200154). **None of the ten all-zero 256²/512² DXT1 world
textures ever unlocks** — they are filled directly (file stream + guest-CPU
decompress) — and the fmt-36 luminance chain + 0B5BE000 + 0B8DF000 + 0BF36000
are resolve destinations (S6_RESOLVE dests). Hence three content classes, and
three mechanisms:

| Class | Content producer | Members (measured so far) | Mechanism |
|---|---|---|---|
| A — unlock-written | guest Lock/Unlock | movie planes, atlas (k_8, served today); TGA six, 0x1A200154 pair; any future | **A: unconditional unlock publish, all supported formats** |
| B — resolve-written | GPU resolve (host executes it) | 0B5BE000, 0BB6D000/6F/71/73/77, 0B8DF000, 0BF36000, +resolve dests | **B: host resolve ownership; exempt from guest capture** |
| C — direct-written | file stream + guest CPU writes, no D3D9 call | the ten DXT1 world keys; cloud sprite 0BC22000 (creator TBD) | **C: an event-driven dirty signal (candidates §5.3); polling barred** |

Per-key classification is MEASURED, never assumed: the evidence plan includes
an instrumented roll joining every zero key to its content-arrival event
(unlock VA / resolve dest / file-read+CPU-write range).

## 4. Design

### 4.1 Class A — unconditional unlock publish (the reference rule, widened)

Today `unlockSnapshotWanted` admits k_8 only (tex_unlock_rule.hpp). Widen the
unlock publish to EVERY format `hleUploadHostTextureFmt` supports
(6/50/18/19/20/49/24/36/2/29/31), keeping the k_8 path byte-identical:

- Source span: from the texture header the hook already holds — the 24-byte
  fetch constant at hdr+28 carries base/pitch/tiled/endian; TexRec carries
  fmt/w/h. Span math = the SAME block/tiled span helper the bind-time capture
  uses (single shared function; the two sites must not fork — iter-449's
  "tiled span exceeds rows*pitch" lesson applies to both).
- Publish RAW guest bytes; the backend upload already does untile/endian per
  format (proven correct offline in iter-456d on BC1/BC2/BC3 real payloads).
- Unconditional per unlock, no dedupe (reference rule). The existing
  latch-consume discipline (one unlock per lock) bounds the rate at the
  guest's own write rate.
- Failure honesty: every refusal keeps landing in exactly one named
  `[TEXUNLOCK_REFUSE]` counter; `NotEligibleFormat` disappears as a silent
  class (only truly-unsupported formats remain, and they log).

### 4.2 Class B — resolve ownership exemption

A key that is a resolve destination (`s_resolveOwned`) must NEVER be
guest-captured: the authoritative content is the host-side resolve product
(already bound via the fromResolve path), and the guest-memory copy is only
the writeback for guest CPU reads. Bind-time capture and any future dirty
mechanism skip resolve-owned keys. This deletes the shadow/luminance members
of the zero census by construction rather than by refresh.

### 4.3 Class C — the dirty signal (the open design question)

The verdict bars baked hash-at-bind polling. Candidates, in preference order,
each with a selection experiment BEFORE any implementation:

1. **PM4 cache-invalidate ranges (the console's own contract).** On 360 the
   guest must invalidate the GPU's caches after CPU-writing texture memory;
   those invalidations traverse the command stream our CP service already
   parses. IF the title emits ranged invalidates covering the streamed
   textures, keying re-capture on them is exactly "the guest tells us what
   changed" — zero polling, zero forcing. Experiment: census the live PM4
   service for invalidate packets during the streaming window; join ranges
   against the ten DXT keys' pixel pages. (Xenia's handling is the oracle for
   packet semantics.)
2. **Host file-I/O dirty pages.** kernel/file_io.cpp executes every stream
   read — the host KNOWS which guest pages it wrote. A page-generation map
   (write-gen per 4 KB page, ~512 KB) + per-key captured-gen enables
   re-capture exactly when a later file read touched the key's range. Limit:
   guest-CPU decompression writes (file bytes land compressed elsewhere, CPU
   writes the texture) are invisible — the experiment must measure what
   fraction of the ten keys' content arrives via direct file DMA vs CPU
   decompress before trusting this signal.
3. **Bounded re-capture windows** (e.g. re-hash a key only while its create
   era is younger than N frames) — LAST resort, and only if 1 and 2 both
   fail the measurement; it is still polling and would need its own
   director sign-off.

### 4.4 Retirements

When A+B+C carry the bar: the permanent `s_texCaptured` dedupe is retired for
covered classes; the `SR_HLE_TEX_REFRESH` diagnostic (iter-450UH) is retired
per charter rule 3 (force → diagnose → root-fix → delete); the iter-455n
"non-k_8 stays on bind-time capture" scope note in ENGINE_MAP/MIGRATION is
superseded.

## 5. Evidence plan — mapped to the verdict's 8-point bar

1. **Unconditional upload for observable Unlocks** → widened-gate replay A/B
   on the fresh fixture + a k_8 byte-identity control (gate-off path
   byte-identical for movie planes/atlas), plus [TEXUNLOCK_REFUSE]
   conservation.
2. **Principled dirty signal, no hash-at-bind polling** → the §4.3 selection
   experiments, each a census with a KILLS-IT join, before ANY implementation.
3. **No per-key/menu-state forcing** → audit_forcing.py clean; all mechanism
   keys arrive from guest events or host-own writes; zero literals.
4. **Alias/tiling/endian/format/mip/volume/cube/descriptor/fence/capacity** →
   the shared span helper unit-tested against the offline decoder's proven
   transforms (road/asphalt/concrete payloads as golden vectors); volume &
   cube behavior unchanged this slice (named non-goal); descriptor lifetime
   via the existing retireResource path; capacity via arena counters in the
   A/B rolls.
5. ⛔CORRECTED: this item is **NOT satisfied**. The planned tagged replay and
   the zero→nonzero join were never completed; the within-roll arm answered the
   FACADE hypothesis negatively, which is a different thing. Original text:
   **first item: the dual-version fixture.**
   Record a fixture during a refresh-armed roll so it contains v0 (zero) AND
   vN (filled) uploads for the same keys; the exhaustive
   initial-zero → changed-nonzero → republished → sampled-draw join is then
   OFFLINE-computable from trace records (tools/srht_census.py extension),
   closing the causality bar the verdict set — no cross-roll comparison.
6. **Sequential K=1 whole-run validation** → ≥3 sequential rolls per config,
   verdicts only from recorded runs.
7. **Both captures, all eras, vs the Xenia oracle** → PrintWindow +
   SR_PRESENT_DUMP reconciled per the capture rule, movie/title/dialog/
   terminal eras, `menu_pressstart_oracle.png` comparison for the title.
8. **Performance & conservation** → feFrames/present rates vs baseline across
   the sequential rolls (the refresh roll's half-rate feFrames flag is the
   cautionary precedent); upload-bytes accounting proving unchanged textures
   generate zero traffic under A (no unlock ⇒ no publish) and bounded
   traffic under C (events only).

## 6. Non-goals (this slice)

Mip-chain generation/upload (the minification aliasing stays), volume/cube
swizzle (named gap, separate slice), exposure/tonemap correctness beyond what
class B restores, Superman model/geometry defects, the movie chroma bands.

## 7. Sequencing ⛔SUPERSEDED by the ledger RE-PLAN VERDICT's 8-slice plan
(historical text retained below)

1. Dual-version fixture + offline join tool (evidence 5) — also the
   causality-bar closer for iter-456f.
2. §4.3 dirty-signal selection censuses (PM4 invalidate first).
3. Class A widening behind `SR_HLE_UNLOCK_UPLOAD_ALL` (opt-in), replay-first.
4. Class B exemption behind the same review.
5. Class C per the selected signal, own gate.
6. Full bar run → director verdict → bake or iterate.

## 8. Evidence item 5 — first execution notes (same day)

- `tools/srht_upload_join.py` landed: exhaustive per-key upload-version/bind
  join over every trace record (no sampling). VALIDATED against
  `archive/iter456_title_fresh.srht`: independently reproduces the exact
  29-key all-zero census (752 upload keys = 723 nonzero + 29 zero-only) and
  adds bind counts (0B5BE000 bound 331x with zero content, atlas 24x, cloud
  sprite 12x).
- Dual-version fixture attempt 1 (`archive/iter456_dualver.srht`, refresh
  armed, ARM_GEOM=1000, 300 presents, census-valid): the mechanics WORK —
  7 NONZERO-CHANGED keys show multi-version uploads in-trace (the movie-plane
  republishes) — but the early window closed before the dynamic set's content
  arrived (14 ZERO-ONLY, no ZERO->NONZERO). WINDOW LESSON: a full FE-era
  window costs ~5-10 MB/present in vertex payloads (multi-GB for the ~1000
  presents between FE start and facade arrival) — the whole-window fixture
  shape is NOT viable for this join.
- Selected next shape (the verdict's "within-roll toggle" arm, sharpened):
  `SR_HLE_TEX_REFRESH_KEY=<hex>` already exists (iter-450UH) — refresh exactly
  ONE key in a live roll. If exactly that key's scene element transforms while
  every other stale element persists, attribution is per-key causal WITHIN one
  roll, with the key-filtered [S6_TEXREFRESH] log exhaustive by construction
  ("under a key filter every event is printed"). Repeat per facade key of
  interest; reconcile both captures per the capture rule.

## 9. Surgical-experiment findings (same day, cont.)

- **Streamed-texture keys are PER-ROLL heap addresses.** The surgical roll
  filtered on 01EAE000 (a fixture-roll world-DXT key); that address never
  materialized — the roll's own zero census names the same CLASS at different
  addresses (01E18000, 01DAC000, 01ED4000, 01D6E000, 0108C000, 01F9E000,
  01000000, 0067A000, 01F60000, 007A6000, 01FDC000). Two design consequences:
  (1) §4.3's dirty signal MUST be event-driven — any address-keyed
  configuration (filters, lists, caches keyed on prior-roll addresses) is
  wrong by construction; (2) the class C taxonomy refines into the per-roll
  streaming pool (0x000..-0x01F.. range, addresses vary) vs stable early
  allocations (the 0x0B..-0x0D.. dynamic set — 0BA64000/0BC22000 recur at
  identical addresses across rolls, and 0BC22000 captured ALL-ZERO again).
- **Filter-off refresh cost datum:** with the key filter active, 3,382,210
  revisits short-circuited before any hashing and feFrames ran at full rate
  (4,577/240 s) — vs the unfiltered refresh roll's half rate. The hashing
  load, not the revisit plumbing, is the plausible FE-tick cost; a production
  mechanism that hashes nothing (event-driven uploads) inherits none of it.

## 10. ZNZ-join roll — THE MODEL CORRECTION (same day, cont.)

The exhaustive zero->nonzero instrument ([TEXREFRESH_ZNZ], kernel change
iter-456h-2) returned **zeroFill=0 across a full refresh-armed roll that
nonetheless recovered the facades** (archive/iter456_znz_join, p4200 dump:
brick facades + lit windows; census keys=843 probed=6508 changed=140
published=140/102.75MB rejected=0). Three revisions follow:

1. **The facade driver is NONZERO->NONZERO refresh, not zero late-fill.** The
   facade textures are captured at first bind with EARLY nonzero content and
   the guest keeps writing them (streaming); permanent dedupe froze v0. The
   design's class C mechanism target is therefore "content evolves after
   first bind" generally — the all-zero census was the DISCOVERY instrument
   for the dedupe defect, not the definition of the affected set.
2. **The all-zero class did not late-fill in any monitored roll** (this roll
   zeroFill=0; the 0BC22000 surgical: 56 probes unchanged). Its members are
   either genuinely-zero-at-era (director's caution, now evidenced) or fill
   in eras not yet run. The whiten-zero replay result stands (those keys'
   emptiness gates the LOOK), but "stale" is unproven per zero key.
3. **Confound to retire before any causal claim: FE tick rate.** Both
   facade-showing rolls ran feFrames~2000/240s; the flat control ran 4500.
   H-alt: backdrop content state depends on FE progression/timing, not on
   refresh. The within-roll arm decides it.

**Next evidence steps (both small, named):**
- Exhaustive FIRST-republish-per-key log (newVer==1; bounded by distinct
  republished keys, <=windowKeys~190/roll) — names the facade keys and their
  formats without sampling.
- `SR_HLE_TEX_REFRESH_FROM=<present#>`: arm the refresh mid-roll. Frames
  before N = stale control, after N = refreshed, SAME roll — the within-roll
  A/B the director's bar requires, and it kills H-alt (an FE-rate effect
  would not switch at N).

## 11. EXECUTION under the design-review verdict (slices 1-2)

The director's 6-slice EXECUTION PLAN (ledger §"DESIGN REVIEW VERDICT") is the
controlling document; §3-§5 above are superseded where they conflict (per-
version provenance model; versioned resolve ownership; COHER_*_HOST as the
class-C first candidate; FROM gates publication only).

### Slice 1 — evidence join repaired + exhaustive first-republish (DONE, tooling)

`tools/srht_upload_join.py` rewritten to the verdict's bar:
- **Refuses** malformed traces: magic, version (v5 only), record count, payload
  total, presents count, per-record bounds, trailing bytes, and the stored
  XXH3-64 of every record.
- ⚠ **Recorder-contract finding**: a payloadless record stores hash **0**, not
  `xxh3("")` — `TraceRecord rec{}` value-initializes and the hash is set only
  inside the payload branches. The tool's first run REFUSED the (valid) title
  fixture on 27,403 records because it hashed the empty string; verified
  empirically (27,403 payloadless records, all storing 0) and the tool now
  validates BOTH halves of the contract, with a self-test that rejects a
  payloadless record carrying a nonzero hash.
- **11 adversarial self-tests, ALL PASS** (`--selftest`): zero->nonzero with
  draw-in-force after the fill, nonzero->changed, bind-without-draw, bind
  overwritten before draw, upload-after-bind, draw-type multiplicity,
  resolve-after-upload ordering, plus the five structural refusals.
- **Sampler state is tracked through the stream**, so the tool reports
  DRAW-IN-FORCE (draws executed while the key was bound) — not bind counts as
  a proxy. Shader-proven sampling is reported as NOT ESTABLISHED offline.
- Acceptance re-run: the fresh fixture still reports exactly
  **752 = 723 nonzero + 29 zero-only**, hashes verified.
- Double-life reproduced independently: `0B5BE000` upload rec463 then 40
  resolve records (rec952..52412); `0BF36000` nonzero upload rec475 then 12
  (rec9507..51169) — upload-then-resolve ordering, exactly as the verdict said.
  **`0B5BE000` is in force at 1,953 draws carrying zero content.**

Kernel: `SR_HLE_TEX_REFRESH_FIRST=1` (default OFF) logs `[TEXREFRESH_FIRST]`
once per key at its first successful republish, carrying both digests, both
nonzero counts, fmt/dims/pitch/tiled/endian, alias window + flip, span vs
tight footprint vs tail, seed and publish present ordinals, and the outcome.
No cap (bounded by distinct republished keys, ~190/roll measured), and the
census now prints `firstPub=` so completeness is checkable.

### Slice 2 — within-roll causality gate (implemented, evidence pending)

`SR_HLE_TEX_REFRESH_FROM=<present ordinal>` (default 0 = publish from boot),
with the verdict's amended semantics: **probing and hashing run from boot on
both sides of N; only PUBLICATION is gated.** A suppressed change does not
consume the stored digest, so it is re-detected and published at/after N
rather than lost, and suppressions are counted (`preN=` in the census) so
"changes existed before N" is provable rather than inferred from silence.
Because the hashing workload is identical either side of N, an FE-rate step at
N cannot be produced by the diagnostic — which is what makes this arm capable
of killing the FE-rate alternative the §10 correction raised.

## 12. Slice 2 RESULT — the within-roll arm refuted the facade attribution

Ledger `iter-456k`. Pre-boundary frame (0 publications) and post-boundary frame
(31 publications, 46.93 MB) are **pixel-identical**; the facades are already on
screen 120 presents BEFORE the boundary with publication suppressed. The arm
itself behaved perfectly (preN=139 detected-and-suppressed, all publishes ≥ N,
conservation closed, no present-rate step: 20.41/s → 19.74/s), which is what
makes the negative trustworthy.

**Consequences for this design:**
- §5 evidence item 5 is SATISFIED in method and ANSWERED against the
  hypothesis it was built to test. The live facade attribution is withdrawn.
- **Class C's premise needs re-founding.** The verdict's slice-3 census demands
  "all relevant direct-written nonzero→nonzero facade versions covered before
  their first draw-in-force" — but this roll shows the republished versions in
  the title era are visually inert (deltas of ±0.3% nonzero on BCn textures,
  i.e. the streaming system's own trickle, not content arrival). Running a
  COHER census to cover events that demonstrably change nothing would be
  measuring the wrong population.
- **The dedupe defect itself is NOT withdrawn** — it is proven by the atlas
  (captured empty, frozen, fixed, text now renders) and by `0B5BE000` in force
  at 1,953 draws with zero content. What is withdrawn is the claim that
  republication is what put facades on screen.
- **The open question is now sharper and different:** why does one roll's
  backdrop show facades while another's shows flat untextured planes, when
  neither depends on republication? Candidates: (a) first-bind-capture timing
  race per key (the atlas class, but zeroFill=0 says it is rare); (b) the flat
  rolls differ in something else entirely — missing draws, different shader
  permutation, resolve-product state, or FE progression reaching a different
  scene state. This must be answered BEFORE class-C signal selection, because
  it decides whether class C addresses the backdrop at all.

**Proposed next step (for director review, not yet executed):** a same-fixture
deterministic comparison — record two fixtures from rolls that visibly differ
(flat vs facades) and diff their upload/bind/draw joins with
`tools/srht_upload_join.py`. That names the difference structurally (which keys
carry which content, which draws are in force) without any live timing
attribution. It is the cheapest question-answering move and needs no new gate.

## 13. ⛔ MANDATORY CORRECTIONS from the third adjudication (ledger sha dcc4e938db019bc5)

These strike text in §10-§12 above. The struck claims were mine and were not
supported by the evidence; the surviving facts are listed beside each.

- **§10 — STRIKE** "the facade driver is NONZERO→NONZERO refresh", "permanent
  dedupe froze facade v0", and "the roll recovered facades". RETAINED as
  measured facts only: `zeroFill=0`; nonzero→nonzero changes occurred; that
  roll displayed facades. FE/timing remains OPEN.
- **§11 — Slice 2 is "adjudicated negative"**, not "evidence pending".
- **§12 — RETAIN** the pre-publication facade and the 121 byte-identical
  frames. **STRIKE** the inference that the observed deltas are "the streaming
  system's own trickle" (unproven). **REPLACE** "same-fixture deterministic
  comparison" with **"matched cross-roll structural discovery, followed by a
  same-trace causal replay"** — two DOGMA-generated fixtures are correlational
  no matter how deterministically each one replays.
- **§3-§4 — state explicitly: NO facade-driving Class-C member is currently
  established.** The per-version provenance amendments stand.

### The four ranked hypotheses for the flat/facade divergence (director's own)

    H1: sampled texture content/version differs | KILLS IT: aligned
        descriptor/content/draw-in-force tuples are identical | COST: two offline joins
    H2: draw, shader, constants, or render state differs | KILLS IT: aligned
        per-present draw contracts are identical | COST: one offline census pair
    H3: resolve product/authority differs | KILLS IT: successful resolve ordering
        and replayed products are equivalent | COST: two replays + resolve-success evidence
    H4: upstream FE/scene progression emits a different stream | KILLS IT: the
        complete aligned command streams match despite different pixels | COST: one structural pair diff

Any discovered difference is a RANKED CANDIDATE only; causality then requires a
derived same-trace replay A/B differing solely in that candidate (slice 4).

### Controlling execution order (from the verdict)

1. Slice 1 — evidence tooling: **ACCEPTED/DONE**, no further work this arc.
2. Slice 2 — publication-boundary arm: **CLOSED NEGATIVE**. Do NOT spend another
   roll on the tagged accept/drop replay for a transition that did not occur;
   do NOT delete the diagnostic yet.
3. **Bank and structurally compare a natural FLAT/FACADE pair — NO behavior
   gates** (refresh, Class A, Class B, dirty events, forcing all OFF).
   Observer-only: tape + `SR_HLE_TRACE_REC` + `SR_HLE_TRACE_FRAMES=16` +
   `SR_HLE_TRACE_HOTKEY=1` + `SR_PRESENT_DUMP=1` + concurrent PrintWindow.
   Both classes must be agreed by BOTH capture paths before the F9 arm; both
   fixtures must verify; each must replay back to its own source class
   (sequentially, exact output hashes) or the recorder omitted the cause.
   Compare on canonical descriptor/epoch identities — never raw keys. Report
   CORRELATIONS only, each with its KILLS-IT. Three failed acquisition
   attempts ⇒ stop and report; never manufacture the pair with refresh.
4. Causalize the top delta offline via derived control/treatment traces from
   ONE source fixture (replay-only; no new live gate). Three failed candidate
   families ⇒ write up and stop.
5. Class A behind `SR_HLE_UNLOCK_UPLOAD_ALL=1` (independent).
6. Class B v2 behind `SR_HLE_RESOLVE_OWNERSHIP_V2=1` (independent).
7. Class C census — CONDITIONAL on slice 4 proving a load-bearing
   direct-written version. COHER first; all-writer page generations second;
   file-I/O only for proven direct destinations. Polling/bounded windows remain
   unauthorized.
8. STOP POINT — Class C production, `s_texCaptured` retirement, diagnostic
   deletion, default changes and baking all remain barred.

⚠ Director's standing instruction for slice 3: **bank the flat/facade baseline
fixtures BEFORE implementing Class A or B**, even though those gates are OFF.

## 14. Slice 3 acquisition — THREE ATTEMPTS, NO PAIR: stop-and-report (director rule)

Per the RE-PLAN VERDICT slice 3: "If three sequential acquisition attempts fail
to produce both classes, stop and report; do not manufacture a pair with
refresh." Three attempts are spent. **No FLAT/FACADE fixture pair was banked.**
Reporting rather than continuing.

Tooling built (`tools/arm_on_class.py`): watches the live `SR_PRESENT_DUMP`
stream, classifies the backdrop objectively, and arms the F9 recorder itself,
so both rolls of a pair are armed by the SAME criterion instead of an eye.
Classifier = mean absolute horizontal luminance gradient over the left/right
quarter-columns (backdrop sample, excludes UI and letterbox). Calibrated on
banked frames: FACADE 8.4-9.9, FLAT 0.00-0.29, intro MOVIE ~3.5.

Attempt log (each one a real defect found and fixed — the tool is now sound):
1. **r1 — arm never fired.** `SetForegroundWindow`+`keybd_event` from a
   background console does not reach SDL; the log's unconditional
   `[TRACE_HOTKEY] WAITING for F9` made the failure visible rather than silent.
   FIX: `PostMessage(WM_KEYDOWN/WM_KEYUP)` straight to the window queue —
   verified working in r3 (`HOTKEY HIT` → `ARMED` → fixture written).
   Also mis-armed at present 30 because the intro movie (~3.5) cleared the
   naive 3.0 threshold. FIX: threshold 6.0 + `--min-present` era gate.
2. **r2 — FLAT for its entire title era** (metric 0.000 at every sampled
   present p3000..p4110) but the watcher was waiting for `facade`, so nothing
   was banked. FIX: `--want any` arms on whichever class appears and reports
   it, so a DOGMA roll is never wasted.
3. **r3 — armed successfully but on a BLACK frame** (0.3% non-black): the
   gradient metric cannot distinguish a flat backdrop from a dark screen, both
   ~0. Fixture `archive/iter456_pair/r3.srht` (51 MB) is therefore a dark-era
   capture, not a FLAT-backdrop capture. FIX: `--min-nonblack` (default 0.35)
   rejects unclassifiable dark frames; re-verified to separate all three cases.

### ⚠ CORRELATION OBSERVED (candidate only — explicitly NOT a causal claim)

Across every roll in this arc: **facades appeared in 4/4 rolls that had
`SR_HLE_TEX_REFRESH=1`, and 0/3+ rolls without it.** Slice 2 already proved
PUBLICATION is not the agent (facades present with publication suppressed; 121
byte-identical frames across 31 publishes). Combined, those two facts point at
the refresh **probe** path — its per-bind hashing cost and the whole-roll
progression change it causes — i.e. exactly the "whole-roll hashing/FE
progression" alternative the director retained when striking my "FE-rate is
DEAD" wording.

    H-probe-timing: refresh PROBING (not publishing) shifts when bind-capture
      happens relative to the guest's streaming writes, so captures land after
      content arrives and freeze good content instead of partial content.
      | KILLS IT: a no-refresh roll slowed by an unrelated mechanism that
        touches NO capture logic must show facades; if it stays flat under an
        equivalent slowdown, timing is refuted and the probe path itself is
        implicated.
      | COST: one roll (partially spent: r3 used per-present GPU readback as
        the slowdown and stayed flat/dark through the sampled window — NOT yet
        a fair test, because the readback slows the RENDER thread while
        probing slows the GUEST BIND path; the loci differ).
      | STATUS: OPEN. This is a correlation over rolls with differing gates,
        which is the same class of evidence the director already ruled
        non-causal once. It must not be reported as a finding.

**Recommendation for the next director review:** the natural-pair acquisition
may be unobtainable as specified if facades genuinely require refresh — in
which case slice 3's premise needs the same re-founding slice 4 was given.
Options to put to the director: (a) accept a matched pair that differs ONLY in
`SR_HLE_TEX_REFRESH` and treat the gate itself as the candidate under test;
(b) test H-probe-timing with a slowdown injected on the guest bind path;
(c) park the flat/facade question and proceed with slices 5-6 (Class A and
Class B v2), which the verdict already ruled stand independently.

### Slice-3 addendum — the r3 aggregate SHARPENS the correlation (still not causal)

r3 finished after the stop-and-report above. Its aggregate: **feFrames 6146 —
the HIGHEST FE-tick count of any roll in this arc** (per-present GPU readback
slows the RENDER thread, and demonstrably did not slow the guest's FE
progression at all). Ranked with the others:

| roll | refresh | feFrames / 240-280 s | backdrop class |
|---|---|---|---|
| r3 (heavy readback) | OFF | **6146** | FLAT |
| r4 control | OFF | 4511 | FLAT |
| pair r2 | OFF | (title era sampled) | FLAT |
| slice2 FROM=3500 | ON (probe both sides) | 2801 | FACADE |
| slice1 FIRST | ON | 2152 | FACADE |
| znz join | ON | 2034 | FACADE |
| refresh_live2 | ON | 2000 | FACADE |

Fast rolls are flat; slow rolls have facades, with no overlap so far. That is
consistent with H-probe-timing AND with any other whole-roll progression
effect, and it remains **cross-roll evidence with a gate difference** — the
exact class the director already ruled non-causal once. It is recorded to make
the candidate falsifiable, not to support it: a fair test must slow the GUEST
BIND path with refresh OFF (r3 slowed the render thread and moved the FE tick
the wrong way, so it tested nothing). Evidence pruned to a 1-in-50 sample plus
the armed frames (13.5 GiB freed; `archive/` is evidence ground, not a dump).

## 15. ⛔ MANDATORY §14 CORRECTIONS (fourth adjudication, ledger sha 3cc275b1554e8343)

The slice-3 stop was ACCEPTED; §14's wording was OVERSTATED. Corrections, each
striking text of mine above:

- **STRIKE "the tool is now sound."** `tools/arm_on_class.py` carries TWO real
  defects the director found by reading it: (1) files are added to `seen`
  BEFORE a successful decode, so a frame that fails mid-write is skipped
  permanently rather than retried; (2) `--want any` increments the stability
  run even when consecutive classifications DIFFER, so "stable" does not mean
  what it says. The watcher remains useful ACQUISITION TOOLING after repair,
  **is not accepted evidence, and must gain adversarial tests before reuse.**
- **The `<6` class means NON-FACADE / LOW-DETAIL, not proven title-era FLAT.**
  `--min-present` is a present-ordinal gate, not a scene-identity gate, and the
  classifier reads GPU readback only. Wherever title-scene identity was not
  established on BOTH capture paths, the class is **NO-FACADE/LOW-DETAIL**.
- **REPLACE** "r2 was FLAT for its entire title era" with: *the sampled GPU
  interval p3000–p4110 was uniform/non-facade* — the inspected frames were
  byte-identical grey fields and do not independently establish the title scene.
- **REPLACE** "across every roll in the arc" with *among the seven classified
  rolls listed*.
- **Raw `feFrames` must be normalized by actual measured roll duration** before
  any "fast/slow" language; 240 s and 280 s rolls are not comparable raw.
- **STRIKE** "demonstrably did not slow guest FE progression at all" — the
  aggregate moved upward; zero slowdown was never proven.
- **One slowed roll cannot refute timing under DOGMA**; it can reject only that
  particular slowdown emulation.

**The defensible statement, replacing §14's correlation paragraph:** *the listed
rolls show a perfect refresh-gate/backdrop-class correlation, but the samples
are cross-roll, gate-confounded, incompletely scene-classified evidence and
establish neither probe timing nor causality.*

Additional facts to preserve: `archive/iter456_pair/r3.srht` is structurally
VALID (v5, 6,993 records, 50,703,328 payload bytes, 16 presents) and is a
**dark-era fixture, not a FLAT fixture**.

### Continuation ranking (director)

1. **(c) slices 5–6, beginning with Class A** — independently proven structural
   defects, deterministic bars, no facade attribution required.
2. (b) the fair H-probe-timing test — PARKED. A valid version needs a
   default-OFF, refresh-mutually-exclusive **shadow-scan** gate at the exact
   captured-texture bind-revisit locus, reproducing the same eligibility,
   throttle cadence, alias selection, bytes scanned and scan workload while
   discarding all results and mutating no state, with workload conservation and
   multiple sequential K=1 rolls. **Not an arbitrary sleep.**
3. (a) the gate-differing pair — REJECTED: it violates the natural-pair
   contract and merely repeats the known correlation; `SR_HLE_TEX_REFRESH`
   changes probe work, timing, bookkeeping and potentially publication, so it
   is not a smallest isolated candidate.

## 16. SLICE 5 — Class A, under execution (director's NEXT INSTRUCTION)

Gate `SR_HLE_UNLOCK_UPLOAD_ALL=1`, default OFF. Refresh, Class B, dirty-event
and forcing gates stay OFF. Order fixed by the instruction:
1. Repair/extract the shared checked layout/footprint helper (bind capture AND
   Unlock, computing the maximum byte ACTUALLY touched by the tiled decode).
2. Run the full R2 adversarial/golden suite.
3. Replay `archive/iter456_title_fresh.srht` gate OFF → byte-identical baseline.
4. Replay gate ON → structural/hash validity, qualifying-Unlock coverage,
   complete refusal/resource conservation.
5. STOP IMMEDIATELY on any test failure, gate-OFF difference, neighbour-tail
   access, unsupported/mip admission, conservation mismatch, or resource/fence
   hazard.
6. If the offline bars pass: exactly three sequential K=1 gate-ON validations
   with GPU readback AND PrintWindow across all reached eras.
7. Then STOP and submit the Class-A evidence packet to the director. Do NOT
   begin Class B, change defaults, bake, or claim a facade fix.

## 17. Slice 5 REWORK — the trace instrument was blind, and the ownership predicate was inverted

Two defects found in one pass, both of which made earlier slice-5 evidence weaker
than it looked. Recording them here because each is a trap the next slice can
walk into again.

### 17.1 The instrument was blind to the class under test

Executing the fifth adjudication's Q4 (a no-op keep control and a
bit-26-drop derivative from one gate-ON fixture) produced a derivative
**byte-identical to its control**. The drop removed nothing: the banked source
contained **zero** unlock-origin uploads. Censused, not assumed — 32,230
records, 965 TextureUploads, `bit26=0`.

Three linked causes, all in the trace path:

1. **Recorder, in-window.** The `TextureUpload` payload arm called `texRead()`
   unconditionally. A Class-A handle carries `kClassAHandleFlag`, which
   `texRead` correctly refuses — so a widened publish was written PAYLOADLESS
   and silently de-fanged (`bytes=0`, `arenaOffset=kArenaFull`). The record
   still looked well-formed.
2. **Recorder, preamble.** The shadow insert is guarded by
   `if (texRead(...))`, so a Class-A upload was never shadowed and could never
   travel in the preamble.
3. **Replay.** Payloads were re-staged into the shared texture pool via
   `texWrite`, but the executor routes bit-26 records to `classARead` /
   `classARelease`. A replayed widened upload would have been dropped, counted a
   badHandle, and leaked the shared slot.

**The general lesson.** A fixture that carries a command but not its pixels
"proves" a regression that exists only in the fixture — the exact failure the
S6i payload arm was originally added to prevent, reappearing one pool later.
Whenever a new allocator is introduced on a path the recorder serializes, the
recorder is part of that path.

**The fix that generalizes:** `uploadIsClassA()` / `uploadFlagAgrees()` are named
ONCE in `hle_video.hpp` and used by the executor, the recorder and the replay
staging. No site answers "which pool holds these bytes" privately again. Routing
is on the HANDLE (what the allocator issued, so it cannot read the wrong arena);
bit 26 is provenance carried alongside, and a disagreement is logged as a defect
rather than silently resolved.

**And the arm that makes the class reachable:** `SR_HLE_TRACE_ARM_UNLOCKN=<n>`
(default OFF). The texkey arm needs a physical address known in advance, but the
class under test is defined by a FLAG, and physical keys move between rolls.
This arms on the fact itself — the Nth bit-26 upload has ENTERED THE SHADOW — so
the preamble necessarily contains the records the controls are about.

Re-run on a fixture armed that way: 22 bit-26 uploads, **all 22 carrying
payload**, 0 payloadless.

### 17.2 The ownership predicate was REVERSED against its own contract

Found by the director (sixth adjudication) at `gfx/hle_video.cpp:206`, and
confirmed at the program point before any change. The contract block said:

> same key + same signature -> owned; a stale bind capture is refused.
> same key + DIFFERENT signature -> recreation; ownership does not carry over.

`unlockOwnedForOther()` returned true for a **different** signature, and the
upload arm admitted capture when that predicate was false. So it admitted
exactly the same-epoch stale capture Class A exists to block, and refused the
recreation it must admit. The inline comment at the use site had drifted to
describe the inverted behavior, so the file disagreed with itself.

**Why 54 passing tests missed it.** R2-5 recomputed a LOCAL signature and
asserted equality/inequality against itself. It never drove the production
decision. *A test that only compares duplicate formulas cannot catch an inverted
consumer.* This is the single most transferable lesson in this section.

**The repair** is `gfx/tex_ownership.hpp`: one state machine, one production
instance, and the tests drive that instance rather than a copy.

- The verdict is a REASON, not a bool — `AdmitNoOwner`, `AdmitRecreated`,
  `AdmitStaleGeneration`, `RefuseSameEpoch`. A bare bool is how the inversion
  hid: both sides of it looked plausible at the call site.
- A **generation** counter, raised by `noteObjectEvent`, covers the case a
  descriptor comparison structurally cannot see: recreation at the same address
  with the SAME descriptor. Without it, a new object silently inherits a dead
  object's ownership.
- Keys are normalized INSIDE the machine (0x8/0xA alias fold), so a caller
  cannot forget to.
- Bit 26 is masked OUT of the signature: provenance, not descriptor — so a
  widened publish and a bind capture of the same surface compare equal, which is
  the comparison the refusal depends on.
- The legacy k_8 route claims through the same machine. One ownership model.
- The vestigial `s_unlockOwned` set is deleted: written at two sites, read at
  none.

Tests: 83/83 (was 54). **Mutation-validated** — restoring the inversion drops
the suite to 63/83 with 8 named failures across R3-1..R3-5.

### 17.3 What the Q4 null actually says

With a fixture that contains the class, removing all 22 unlock-origin uploads
changed **no pixel** in 16 frames. That null is explained, not merely observed:
the join shows all 22 keys have `binds=0` in that window, while other keys in
the same window have 9-137 binds — so the join is not blind. Class A published
real, nonzero pixels for 22 textures nothing sampled there, and the textures
that WERE sampled arrived `[ZERO-ONLY]`.

The director ruled that window **non-exercising** (it sits at the terminal-black
transition, not the malformed backdrop era) and that the next source must be
chosen by the independently defined target era — the backdrop visibly present on
both capture paths — never by requiring a bit-26 bind, which would condition
acquisition on the mechanism under test.

The bound `[ZERO-ONLY]` population is a logged Class-C/zero-stale lead. It is
**not** authorized inside slice 5.

