# difftest — L6 differential CPU-test (per-function correctness)

Runs a single guest PowerPC function on identical inputs in (a) our recomp and (b) a trusted
reference, then diffs the register/memory deltas. Catches endianness / pointer / vtable / sign
bugs **systematically** instead of only when they crash. Full rationale + staging in
[`docs/DIFFTEST_DESIGN.md`](../../docs/DIFFTEST_DESIGN.md).

## Layout

```
scripts/difftest/
  scan_candidates.py   # find pure-leaf test targets in the recomp source
  golden_capture.py    # build the reference (golden) deltas  <-- the working first half
  cases/               # input cases:  <fn>.cases.json
  golden/              # captured goldens: <fn>.golden.json   (created by golden_capture)
  README.md
```

## What's DONE (works today, stdlib-only)

- **Candidate selection** — `scan_candidates.py` lists Tier-0 pure-arith leaf functions
  (no calls, no memory, no spills) from `guest/ppc/ppc_recomp.*.cpp`. ~300 found.
  ```sh
  python scan_candidates.py --limit 40              # Tier 0 table
  python scan_candidates.py --allow-mem --json      # Tier 1 (memory-touching) as JSON
  ```
- **Golden capture** — `golden_capture.py` produces the reference deltas in the canonical format.
  Three worked-example functions are wired up under `cases/`:
  | VA | computes | exercises |
  |---|---|---|
  | `sub_822949D8` | `1 if r5==r4 else 0` (cntlzw+rotate-mask) | shift/rotate, cntlzw-of-0 |
  | `sub_826690D0` | `align_up(r3*8,32)+r3*8` (size round) | rlwinm masks, alignment |
  | `sub_82670774` | `r3*r4*3` (index scale) | mullw 32→64, mask, overflow |

  ```sh
  # (a) emit the exact emulate_function payloads to run via the Ghidra MCP tool:
  python golden_capture.py --cases cases/sub_82670774.cases.json --fn sub_82670774 --emit

  # (b) ingest the saved responses into golden/sub_82670774.golden.json:
  python golden_capture.py --cases cases/sub_82670774.cases.json --fn sub_82670774 \
         --ingest responses/sub_82670774.responses.json

  # (c) no oracle needed: pin the hand-computed 'expect' fields as the golden
  #     (valid for the simple Tier-0 canaries):
  python golden_capture.py --cases cases/sub_82670774.cases.json --fn sub_82670774 --hand
  ```

### Why emit/ingest and not a live MCP call

A plain script can't invoke an MCP tool itself, **and** the Ghidra `emulate_function` endpoint
currently fails on this PowerPC program with `"Undefined register: ESP"` (a server-side bug,
iter-215 — verified independent of our inputs). So golden_capture is oracle-agnostic: it emits the
exact payloads for an agent to run, ingests the responses, or pins hand-derived goldens. The day the
emulate endpoint is fixed (or a Xenia single-step oracle lands, see TOOLING_PLAN §L9-A), step (a)+(b)
become fully automatic with no format change.

## What's DONE (the recomp-side execution half — built)

- **`difftest_main` entrypoint** (`tools/difftest/difftest_main.cpp`) — a standalone executable
  that allocates flat guest memory, calls `sr::kernel::setGuestMemory`, zeroes a `PPCContext`,
  seeds the input GPRs, calls `sub_XXXX(ctx, base)` directly, and dumps the register deltas in the
  canonical §4 format (`"oracle":"recomp"`). Built by the `difftest` CMake target, which links the
  SAME recomp static lib (`SupermanReturnsGuest`) and kernel objects the game uses, with the SAME
  `PPC_CONFIG_*`/include set, so the `PPCContext` layout is bit-identical. Tier-0 leaves are called
  by symbol (no dispatch table). Build + run:
  ```sh
  cmake --build build --target difftest
  ./build/difftest.exe --self-test                                  # built-in 3-fn sanity check
  ./build/difftest.exe --cases scripts/difftest/cases/sub_82670774.cases.json   # JSON array of deltas
  ./build/difftest.exe 0x82670774 r3=0x7 r4=0x5                      # ad-hoc single case
  ```
- **`compare.py`** — runs `difftest` on a case file and masked-diffs its deltas against
  `golden/<fn>.golden.json` (gates on the ABI output regs r3/r4; cr/xer/scratch advisory; mem
  Tier 1). Exit 0 = match, 1 = divergence.
  ```sh
  python compare.py --all                          # every golden/*.golden.json
  python compare.py --fn sub_82670774              # one function (cases/ + golden/ auto)
  python compare.py --fn sub_82670774 --difftest ../../build/difftest.exe
  ```
  Stage-A status: all 3 canary functions (14 cases) PASS against the `--hand` goldens.

## What's PENDING

- **Tier 1 memory seeding/diff** — `difftest_main` already allocates the flat guest image and
  has the `mem` field plumbed through the format; the remaining work is seeding big-endian bytes
  at input VAs + capturing the written-byte set, plus the matching oracle memory input. This is
  where the endianness/alignment payoff is (§6, §7 Stage D).
- **Live Ghidra-emulate oracle** — still blocked on the server-side `ESP` bug (see below); the
  `--hand` goldens stand in for the canary set today.

## Golden format

See `docs/DIFFTEST_DESIGN.md` §4. One object per `(function, input-case)`:
inputs (`regs`/`mem`) → `deltas` (changed output `regs` + written `mem`), tagged with the `oracle`.
The recomp side emits the **same** shape with `"oracle": "recomp"`; `compare.py` diffs the two.
