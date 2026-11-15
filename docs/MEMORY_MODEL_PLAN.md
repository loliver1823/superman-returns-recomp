# Guest memory model: move to console semantics (planned, iter-372)

Status: **DESIGNED, NOT LANDED.** Written after iter-372 established the current
model is a simplification that has already produced one real bug and probably
several past ones. Read this before touching `aliasGuestVA`, `physToHost`, or
`MmGetPhysicalAddress` — they are one coupled change, and landing any of them
alone makes things worse (see §4).

## 1. What the console does

One 512 MB physical RAM. Xenia (`src/xenia/gpu/../memory.cc`, and ENGINE_MAP §6e):

- **Virtual heaps** — `0x00000000`, `0x40000000`, `0x7F000000`, `0x80000000`
  (title XEX loads at `0x82000600`), `0x90000000`. Each has its own backing;
  a virtual page is backed by *some* physical page, not an identity-mapped one.
- **Physical heaps** — `0xA0000000` (64 KB pages), `0xC0000000` (16 MB pages),
  `0xE0000000` (4 KB pages). All three are **hardware aliases of the same RAM**:
  the physical offset is `va & 0x1FFFFFFF`, so `0xA0001000`, `0xC0001000` and
  `0xE0001000` are the SAME bytes.

Xenia implements the aliasing literally — one file mapping, `MapViewOfFile`'d at
each physical base — so no arithmetic is involved and no range can be forgotten.

## 2. What we do instead

One flat 4 GB+64 KB `VirtualAlloc` at host `0x2A00000000`
(`guest/guest_harness.cpp:468-472`, 4 GB-aligned so `(uint32_t)base == 0`), with
`guestVAToHost(va) = base + va`. Aliasing is faked with arithmetic on every
access (`kernel/guest_context.hpp` `aliasGuestVA`):

```c
if (va < 0xC0000000u) return va;                  // 0xA0000000-0xBFFFFFFF: UNTOUCHED
if (va >= 0xD0000000u && va < 0xD0100000u) return va;  // fake device region
if (va >= 0xE0000000u) return va - 0x60000000u;   // 0xE -> 0x8
return va - 0x40000000u;                          // 0xC -> 0x8
```

Two deviations, both load-bearing:

1. **`0xA0000000-0xBFFFFFFF` is not folded at all.** Enumeration bug: `0xC` and
   `0xE` are handled, `0xA` was missed. This is the observed defect — the guest
   allocates GPU-visible structures there (`[GPU_RING_ALLOC]` overrides #2/#3/#5/#6:
   ring `0xAEFEED40`, GPU-id block `0xAEFEECE0`) while `physToHost` in
   `gfx/null_renderer.cpp` computes `0x80000000 + (phys & 0x1FFFFFFF)`. The two
   sides land **0x20000000 apart**.
2. **The physical ranges fold into the VIRTUAL heap at `0x8`,** not into a
   separate physical RAM. So a physical allocation and a virtual allocation can
   silently occupy the same host bytes. That is very likely the shared root of the
   recurring overlap family: the two-allocator overlap fix (iter-364), the
   GPU-alloc-vs-game-pool overlap (iter-361), and the scribble arc.

## 3. Target design

1. Reserve the 4 GB region as today.
2. Create a **512 MB pagefile-backed file mapping** = physical RAM.
3. `VirtualFree` the `0xA0000000-0xFFFFFFFF` sub-ranges, then `MapViewOfFileEx`
   that one mapping at `base+0xA0000000`, `base+0xC0000000`, `base+0xE0000000`.
   The three windows are 512 MB each and adjacent, exactly covering the physical
   region — aliasing becomes real.
4. Virtual heaps keep ordinary committed memory.
5. `aliasGuestVA` collapses to identity and leaves the hot path (it currently runs
   on EVERY PPC load/store/SIMD access).
6. `physToHost(phys)` becomes `guestVAToHost(0xA0000000 | (phys & 0x1FFFFFFF))` —
   one definition, used by every consumer.
7. `MmGetPhysicalAddress(va)` returns `va & 0x1FFFFFFF` and is now invertible by (6).

## 4. HAZARDS — why this must land as ONE verified change

- **`0xD0000000-0xD00FFFFF` is a harness invention.** The fake D3D device /
  stubs / bump allocator / framebuffer live there
  (`kernel/guest_context.hpp:104`), and that address sits INSIDE what becomes the
  `0xC` physical window. Mapping that window over it would alias the fake device
  onto physical `0x10000000-0x100FFFFF`. It must be relocated out of the physical
  ranges first — into a virtual heap — or carved out of the view.
- **Switching `0xC`/`0xE` from `-> 0x8` to `-> physical RAM` RELOCATES existing
  state.** Everything the guest has been reading and writing through `0xC`/`0xE`
  today lands in the `0x8` range; after the change it lands in the `0xA` window.
  Any state established before the switch moves out from under its readers.
- **`MmGetPhysicalAddress` is COUPLED and must not be fixed alone.** It is
  currently a generic stub returning 0, and `stub_VdInitializeRingBuffer`
  (`kernel/stubs.cpp:2633-2643`) depends on that: it rejects `r3` and falls
  through to `g_gpuRingAllocVA`, which is how the ring base is correct today and
  how iter-372's doorbell consumer works. Making it return a real physical
  address BEFORE (6) is in place feeds the guest a value our own translation
  cannot invert — a regression, not a fix.
- **`kPhysTop` must stay < `0xC0000000`** (`guest_context.hpp:43`) — an existing
  constraint that assumes the current fold.

## 5. Verification plan (do not land without all of it)

1. Golden-trace replay parity: `packets=63054 draws=3501 regWrites=1756473
   vsLoads=512 psLoads=263 CLEAN-EOF` + identical pixel histogram.
2. `python scripts/audit_forcing.py` clean.
3. K=4 natural boot, no-env: FULL-INIT rolls, no new crash class, `[ALLOC_COORD]`
   and `[HEAP_RESV]` unchanged in character.
4. **A deep roll** (`clipCtorReal>0` / `defExec>0`). As of iter-371 **no deep roll
   exists on any of the 27 logs on disk** — this is the blocker for validating a
   foundation change, and is the reason iter-372 stopped short of landing it.
5. Regression check on the overlap family: iter-364 `SR_DLMALLOC_FENCE` effects
   census (33/33) and iter-361 deep-Apt rate must not move.

## 5b. iter-427 ATTEMPT (2026-07-30): view aliasing IMPLEMENTED, A/B NEGATIVE — allocator surgery is the missing prerequisite

`SR_PHYS_UNIFY=1` (guest_harness.cpp `allocGuestMemoryUnified`) lands §3 mechanically:
placeholder-reserve the 4 GB (VirtualAlloc2), 512 MB anonymous section, MapViewOfFile3
views — **amended to map the `0x8` window as a view too**, because live evidence showed
the guest mixing `0x8`/`0xA`-range VAs for the same physical bytes (the vdecl-patched
movie VS is written through one window and ring-copied through the other; under the flat
model that copy reads zeros → `[PM4_REJECT] VERTEX 27/30dw all-zero` → the movie draw
loses its UV-carrying shader → the flat quad).

**Result: 3/3 shallow rolls, giant spin-storms (224M-516M), movie never starts.**
Mechanism (thread sampler): guest STACKS live at `0xAE`-range VAs while the heap hands
out `0x8E`-range VAs. Under the `0x8` view, `0x8E846940` and `0xAE846940` are the same
section page (identical mod-512MB offset) — and the heap/phys dividing-window check is
**VA-based**, so both get granted → real data collision. This is iter-373's fold
regression re-derived with views: **any model that makes `0x8` alias physical RAM
requires the allocators to be OFFSET-disjoint mod 512 MB, not merely VA-disjoint.**

Prerequisites for landing unify for real:
1. Physical allocator constrained to 512 MB with FREE support (the game's 10×128 MB
   early allocs cannot live simultaneously — audit MmFreePhysicalMemory traffic).
2. Heap and phys allocations offset-disjoint mod 512 MB (the current VA window
   [0x82AE0000..0xBF000000) spans 966 MB, so mod-512 collisions are guaranteed).
3. Then `MmGetPhysicalAddress = va & 0x1FFFFFFF` becomes safe and invertible.

The gate + mapping infrastructure stays in-tree (default OFF). Interim: the D3D9-HLE
boundary sidesteps the ring's corrupted view for rendering (container shaders hash
intact at the hook; device shadow carries the full fetch file at dev+1024).

## 6. Expected payoff

- Kills the phys/virtual overlap bug class structurally instead of per-incident.
- Removes a branch from the hottest path in the emulator.
- Makes the Xenia oracle directly transferable — reasoning about memory stops
  needing a mental translation layer.
