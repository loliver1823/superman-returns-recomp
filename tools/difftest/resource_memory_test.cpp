#include "gfx/resource_memory.hpp"
#include "kernel/native_resource_writes.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <vector>

#include "kernel/sr_oracle.hpp"
#include "gfx/volume_gates.hpp"
#if SR_ORACLE
// This standalone core test links resource_memory.cpp without the guest-side reader observer
// (kernel/resource_reader_observe.cpp, which needs the guest, the log and the quit unit); its
// coherency pre-hook observes only and is inert here.
namespace sr::kernel::reader_observe {
void preCoherency(uint32_t, uint32_t, uint32_t) noexcept {}
}  // namespace sr::kernel::reader_observe
#endif

namespace R = sr::gfx::memres;
namespace {
unsigned passed = 0, failed = 0;
void check(bool ok, const char* description) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", description);
  ok ? ++passed : ++failed;
}
}

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--gate") == 0) {
    std::printf("resource_mode=%d\n", R::resourceMode() ? 1 : 0);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--generation-exhaustion") == 0) {
    R::Registry registry;
    registry.reset(UINT32_MAX);
    registry.noteCpuWrite(0x1000u, 1u, R::Src::kReadFile);
    std::fprintf(stderr, "GENERATION_EXHAUSTION_CONTROL_ESCAPED\n");
    return 1;
  }
  if (argc != 1) return 2;
  {
    // Step-4 lane C round 2, P2-1 (SR_TEX3D_V2): Astra's counterexample - a 1x1x64 k_8_8_8_8 volume (tiled, pitch
    // 32) with a mip page and mip_max_level 6. The volume loader reads levels 1-6 (the longest axis is the depth);
    // the cube-layout spans clamp by width / height and gave no mip range. With the gate off this checks the old
    // answer (one span), so running the test both ways shows the mutant and the fix.
    const uint32_t f[6] = {2u | (1u << 22) | (1u << 31), 6u | (0x100u << 12), 63u << 22, 0u, 6u << 6,
                           (2u << 9) | (0x200u << 12)};
    R::ResourceSpan spans[2];
    const uint32_t n = R::resourceTextureSpans(f, spans);
    const bool v2 = sr::gfx::volume::tex3dV2On();
    std::printf("volume spans (SR_TEX3D_V2=%d): %u span(s) base 0x%08X+%u mip 0x%08X+%u\n", v2 ? 1 : 0, n,
                spans[0].phys, spans[0].len, n > 1 ? spans[1].phys : 0u, n > 1 ? spans[1].len : 0u);
    check(v2 ? (n == 2 && spans[1].phys == 0x200000u && spans[1].len > 0u) : n == 1,
          v2 ? "a 1x1x64 volume with mip_max 6 captures its mip range (P2-1)"
             : "gate off: the 1x1x64 volume's mip range is missing (the P2-1 counterexample, old behaviour)");
    // The 64^3 tiled k_8_8_8_8 noise volume of gp1 / gp2: the base to its EXTENT (1,050,752 bytes), not the
    // 1,048,576-byte slice stride the base-only reader used.
    const uint32_t g[6] = {2u | (2u << 22) | (1u << 31), 6u | (0x103A6u << 12), 63u | (63u << 11) | (63u << 22),
                           0u, 0u, 2u << 9};
    const uint32_t m = R::resourceTextureSpans(g, spans);
    check(m == 1 && spans[0].len >= (v2 ? 1050752u : 1048576u),
          v2 ? "the 64^3 volume's base span reaches its data extent" : "gate off: the 64^3 base span (old)");
  }
  R::ResourceMemory memory;
  {
    R::Registry registry;
    registry.reset(UINT32_MAX - 1u);
    registry.noteCpuWrite(0x1000u, 1u, R::Src::kReadFile);
    check(registry.clock() == UINT32_MAX && registry.cpuGen(1) == UINT32_MAX,
          "the final generation remains ordered instead of wrapping");
    uint32_t generation = 123u;
    check(!registry.tryReserveGeneration(generation) && generation == 123u &&
          registry.clock() == UINT32_MAX && registry.cpuGen(1) == UINT32_MAX &&
          registry.gpuGen(1) == 0u && registry.uploadedGen(1) == 0u,
          "exhausted reservation changes neither its output nor any ownership state");
  }
  constexpr uint32_t page = 120, phys = page << R::kPageLog2;
  auto ticket = memory.prepare(page);
  check(ticket.decision == R::Decision::kUploadFirst, "a new page requires its first copy");
  check(memory.prepare(page).decision == R::Decision::kUploadFirst,
        "preparing or abandoning a copy never marks its bytes current");
  check(memory.commit(ticket), "a successful copy can commit its ticket");
  check(memory.prepare(page).decision == R::Decision::kKeep, "committed unchanged bytes are reused");
  check(!memory.commit(memory.prepare(R::kPages)), "invalid pages cannot commit");

  memory.noteCoherency(0x03000000u, phys + 4095, 2);
  check(memory.prepare(page).decision == R::Decision::kUploadNoted &&
        memory.prepare(page + 1).decision == R::Decision::kUploadNoted,
        "a straddling coherency record invalidates both pages");
  ticket = memory.prepare(page);
  memory.noteFileRead(0x80000000u + phys, 4);
  memory.commit(ticket);
  check(memory.prepare(page).decision == R::Decision::kUploadNoted,
        "a write arriving during a copy stays pending after its old ticket commits");
  memory.commit(memory.prepare(page));

  memory.noteGpu(phys, 4096);
  check(memory.prepare(page).decision == R::Decision::kKeepGpu,
        "a resolve keeps its GPU-owned bytes instead of copying stale RAM");
  memory.noteCoherency(0x03000100u, phys, 4096);
  memory.noteCoherency(0x00010000u, phys, 4096);
  check(memory.prepare(page).decision == R::Decision::kKeepGpu,
        "resolve and shader-sync coherency records do not invent CPU writes");
  memory.noteCpu(phys, 4, R::Src::kInval);
  check(memory.prepare(page).decision == R::Decision::kUploadNoted,
        "a newer CPU write owns the bytes and must be captured again");

  const uint32_t deferredResolve = memory.reserveGpu();
  memory.noteCpu(phys, 4, R::Src::kInval);
  memory.noteGpuAt(phys, 4096, deferredResolve);
  check(memory.cpuGeneration(page) > memory.gpuGeneration(page) &&
        memory.prepare(page).decision == R::Decision::kUploadNoted,
        "a late-executed resolve cannot eclipse the CPU write produced after it");
  const uint32_t nextResolve = memory.reserveGpu();
  memory.noteGpuAt(phys, 4096, nextResolve);
  memory.noteGpuAt(phys, 4096, deferredResolve);
  check(memory.gpuGeneration(page) == nextResolve && memory.prepare(page).decision == R::Decision::kKeepGpu,
        "publishing an older resolve never replaces a newer GPU generation");
  check(memory.noteGpuAt(phys, 4096, UINT32_MAX) == 0 && memory.noteGpuAt(phys, 4096, 0) == 0,
        "unreserved GPU generations are refused");

  const uint32_t olderResolve = memory.reserveGpu();
  memory.noteCpu(phys, 4096u, R::Src::kReadFile);
  memory.commit(memory.prepare(page));  // destination preservation captured the newer CPU generation
  memory.noteGpuAt(phys, 4096u, olderResolve);
  check(memory.prepare(page).decision == R::Decision::kUploadNoted,
        "an older resolve invalidates newer CPU bytes captured before it overwrites them");
  memory.commit(memory.prepare(page));  // next draw's CPU copy is after that resolve
  memory.noteGpuAt(phys, 4096u, olderResolve);  // execution republishes the same producer token
  check(memory.prepare(page).decision == R::Decision::kKeep,
        "render publication of the same resolve cannot invalidate its following CPU copy");

  // Two draws reuse one inline ring page before a whole-buffer coherency note.
  // Keep each admitted capture until its draw consumes the queued copy.
  const uint32_t ringPage = page + 2u;
  uint32_t ringBytes = 0x11111111u;
  memory.noteInlineWrite(0xA0000000u + (ringPage << R::kPageLog2), 4096u);
  check(memory.sourceWindow(ringPage, 8) == 0xAu,
        "completed inline write names the actual guest write window");
  auto firstDraw = memory.prepare(ringPage);
  const uint32_t firstCopy = ringBytes;
  memory.commit(firstDraw);
  ringBytes = 0x22222222u;
  memory.noteInlineWrite(0xA0000000u + (ringPage << R::kPageLog2), 4096u);
  auto secondDraw = memory.prepare(ringPage);
  const uint32_t secondCopy = ringBytes;
  memory.commit(secondDraw);
  check(R::isUpload(firstDraw.decision) && R::isUpload(secondDraw.decision) && secondDraw.cpu > firstDraw.cpu,
        "completed inline writes refresh their shared page without a whole-buffer note");
  check(firstCopy == 0x11111111u && secondCopy == 0x22222222u,
        "ordered ring captures preserve both draws");
  const auto inlineResolve = memory.reserveGpu();
  const auto beforePreserve = memory.cpuGeneration(ringPage);
  auto preservation = memory.prepare(ringPage);
  check(preservation.cpu == beforePreserve &&
        memory.generation() == inlineResolve,
        "inline preservation captures bytes without creating a post-producer CPU write");
  memory.commit(preservation);
  memory.noteGpuAt(ringPage << R::kPageLog2, 64u, inlineResolve);
  check(memory.prepare(ringPage).decision == R::Decision::kKeepGpu,
        "a later GPU producer keeps its resolved bytes despite earlier inline use");
  const auto beforeNewInline = memory.reserveGpu();
  memory.noteInlineWrite(0xA0000000u + (ringPage << R::kPageLog2), 4096u);
  const auto newerInline = memory.prepare(ringPage);
  memory.commit(newerInline);
  memory.noteGpuAt(ringPage << R::kPageLog2, 64u, beforeNewInline);
  check(memory.sourceWindow(ringPage, 8) == 0xAu &&
        R::isUpload(memory.prepare(ringPage).decision),
        "an older GPU token cannot eclipse a newer completed inline write");
  memory.noteFileRead(0x80000000u + (ringPage << R::kPageLog2), 4096u);
  memory.commit(memory.prepare(ringPage));
  check(memory.sourceWindow(ringPage, 8) == 8u && memory.prepare(ringPage).decision == R::Decision::kKeep,
        "completed file reuse retires stale inline window and refresh policy without a new grant");
  memory.noteInlineWrite(0xA0000000u + (ringPage << R::kPageLog2), 4096u);
  memory.noteGrant(0x80000000u + (ringPage << R::kPageLog2), 4096u);
  memory.commit(memory.prepare(ringPage));
  check(memory.prepare(ringPage).decision == R::Decision::kKeep && memory.sourceWindow(ringPage, 0) == 8u,
        "reallocation expires inline refresh policy and window provenance");

  memory.noteGrant(0xA0000000u + phys, 4096);
  check(memory.sourceWindow(page, 0) == 0xAu, "the remembered grant supplies an ambiguous page");
  check(memory.sourceWindow(page, 8) == 8u, "a live owner takes precedence over the previous grant");
  check(memory.sourceWindow(page + 50, 0) == 8u, "a page with no grant uses the explicit 0x8 default");
  check(R::resourcePhysOfVa(0xE0000000u) == 0x1000u &&
        R::resourcePhysOfVa(0x80000000u + phys) == phys,
        "physical translation preserves the XDK high-window displacement");
  const uint32_t before = memory.cpuGeneration(page);
  memory.noteFileRead(phys, 4096);
  check(memory.cpuGeneration(page) == before, "virtual-heap reads are not physical GPU writes");

  std::vector<uint32_t> dirty;
  memory.takeDirtyBlocks(dirty);
  check(dirty.size() == 1 && dirty[0] == page >> R::ResourceMemory::kDirtyBlockLog2,
        "dirty blocks deduplicate multiple CPU writes");
  memory.takeDirtyBlocks(dirty);
  check(dirty.empty(), "consuming dirty blocks does not repeat old invalidations");
  memory.noteCpu(0x1FFFFFFFu, 2, R::Src::kOther);
  memory.takeDirtyBlocks(dirty);
  check(dirty.size() == 1 && dirty[0] == 2047u, "a final-byte write stays within the physical table");
  check(memory.noteCpu(0x20000000u, 4, R::Src::kOther) == 0,
        "out-of-range physical writes are refused");

  {
    R::ResourceMemory aliases;
    std::atomic<bool> finished{false}, mismatch{false};
    aliases.noteInlineWrite(0xA0001000u, 4096);  // generation2 -> A, generation3 -> 8, alternating
    std::thread writer([&] {
      for (uint32_t i = 0; i < 4000u; ++i) {
        aliases.noteFileRead(0x80001000u, 4096);
        aliases.noteInlineWrite(0xA0001000u, 4096);
      }
      finished.store(true, std::memory_order_release);
    });
    do {
      const auto capture = aliases.prepare(1, 8);
      if (capture.window != ((capture.cpu & 1u) ? 8u : 0xAu)) mismatch.store(true);
    } while (!finished.load(std::memory_order_acquire));
    writer.join();
    check(!mismatch.load(), "concurrent alias changes keep each ticket's generation and source window coherent");
    const auto oldAlias = aliases.prepare(1, 8);
    aliases.noteFileRead(0x80001000u, 4096);
    aliases.commit(oldAlias);
    check(oldAlias.window == 0xAu && aliases.prepare(1, 0xA).window == 8u &&
          R::isUpload(aliases.prepare(1).decision),
          "late alias changes cannot rewrite a ticket and remain pending after its old copy");
  }

  memory.commit(memory.prepare(page));
  ticket = memory.prepare(page);
  std::vector<std::thread> writers;
  for (unsigned i = 0; i < 4; ++i)
    writers.emplace_back([&] {
      for (unsigned j = 0; j < 1000; ++j) memory.noteCpu(phys, 4, R::Src::kReadFile);
    });
  for (auto& writer : writers) writer.join();
  check(memory.cpuGeneration(page) >= before + 4000u &&
        memory.prepare(page).decision == R::Decision::kUploadNoted,
        "concurrent notes never move a page generation backwards");
  memory.takeDirtyBlocks(dirty);
  check(dirty.size() == 1, "concurrent notes retain the pending dirty block");

  {
    // perf parity: the per-64-page-block change sequence behind the request memo
    {
      R::Registry blocks;
      const uint64_t since = blocks.sequence();
      check(blocks.unchangedSince(0x100u, 0x13Fu, since), "a block range nobody noted is unchanged");
      blocks.noteCpuWrite(0x800u << R::kPageLog2, 4096u, R::Src::kOther);  // block 0x20
      check(blocks.unchangedSince(0x100u, 0x13Fu, since) && !blocks.unchangedSince(0x800u, 0x800u, since),
            "a note changes only its own block");
      blocks.noteCpuWrite(0x13Fu << R::kPageLog2, 4096u, R::Src::kOther);  // block 4, the range's last page
      check(!blocks.unchangedSince(0x100u, 0x100u, since) && blocks.unchangedSince(0x100u, 0x13Fu, blocks.sequence()),
            "a note on any page of a block changes the block for every page of it; a later read sees it settled");
      const uint64_t before = blocks.sequence();
      blocks.noteGpuWriteAt(0x200u << R::kPageLog2, 64u, blocks.reserveGeneration());
      blocks.forgetUpload(0x300u);
      check(!blocks.unchangedSince(0x200u, 0x200u, before) && !blocks.unchangedSince(0x300u, 0x300u, before) &&
                blocks.unchangedSince(0x100u, 0x13Fu, before),
            "a GPU publication and an upload reset change their blocks too");
    }
    // mem-native: exact CPU-write patches (the D3D header fence stamps). A patch names dwords, reserves no
    // generation, leaves the page's decision unchanged and is taken once.
    R::ResourceMemory patches;
    constexpr uint32_t pp = 0x1711Eu, headerVa = 0xB711E2B4u;  // a Warworld IB header's fence (mrc_fetch4 WRITER #5)
    patches.commit(patches.prepare(pp));
    const uint32_t clock = patches.generation();
    const uint64_t sequence = patches.sequence();
    const auto cpu = patches.cpuGeneration(pp);
    patches.notePatch(headerVa, 4u);
    check(patches.generation() == clock && patches.sequence() == sequence && patches.cpuGeneration(pp) == cpu &&
          patches.prepare(pp).decision == R::Decision::kKeep,
          "a patch reserves no generation and leaves the page's own decision a keep");
    check(patches.patchPending(pp) && patches.patchPendingRange(pp - 3u, pp) && patches.patchPendingRange(pp, pp + 70u) &&
          !patches.patchPendingRange(pp + 1u, pp + 70u) && !patches.patchPendingRange(0u, pp - 1u),
          "the pending bit names exactly the patched page");
    {
      // perf parity round 2: the arming count moves when a page's pending bit is set from clear, not on a second
      // stamp of a page already pending
      R::ResourceMemory arming;
      const uint64_t a0 = arming.patchArmed();
      arming.notePatch(headerVa, 4u);
      arming.notePatch(headerVa + 0x10u, 4u);
      const uint64_t a1 = arming.patchArmed();
      R::ResourceMemory::Patch drained;
      arming.takePatch(pp, drained);
      arming.notePatch(headerVa, 4u);
      check(a1 == a0 + 1u && arming.patchArmed() == a1 + 1u,
            "the arming count moves once per pending page, again after its take");
    }
    R::ResourceMemory::Patch taken;
    check(patches.takePatch(pp, taken) && taken.page == pp &&
          taken.dwords[1][(0x2B4u >> 2) >> 6] == uint64_t{1} << ((0x2B4u >> 2) & 63u),
          "the taken patch names the one stamped dword, in the window it was written through (0xA)");
    bool others = false;
    for (uint32_t i = 0; i < 16u; ++i)
      others = others || taken.dwords[0][i] || (i != (0x2B4u >> 2) >> 6 && taken.dwords[1][i]);
    check(!others && !patches.patchPending(pp) && !patches.takePatch(pp, taken),
          "a patch is taken once and names no other dword");
    patches.notePatch(0x80000FFEu, 4u);  // unaligned, crossing a page: both dwords, both pages, window 0x8
    R::ResourceMemory::Patch a, b;
    check(patches.takePatch(0u, a) && patches.takePatch(1u, b) && !a.dwords[1][15] &&
          (a.dwords[0][15] >> 63) == 1u && (b.dwords[0][0] & 1u) == 1u,
          "a write straddling a page boundary patches the whole dwords on both pages (window 0x8)");
    patches.notePatch(0x80002010u, 4u);  // dword 4 through 0x8 ...
    patches.notePatch(0xA0002020u, 4u);  // ... dword 8 through 0xA, the same physical page
    R::ResourceMemory::Patch mixed;
    check(patches.takePatch(2u, mixed) && mixed.dwords[0][0] == (uint64_t{1} << 4) &&
          mixed.dwords[1][0] == (uint64_t{1} << 8),
          "writes through both windows of one page keep each dword's own window (Astra round 1, finding 1)");
    patches.notePatch(0x40001000u, 4u);
    check(patches.patchStats().refused == 1u && !patches.patchPendingRange(0u, R::kPages - 1u),
          "a virtual-heap write is refused: not GPU-addressable through the arena");
    patches.noteGpu(pp << R::kPageLog2, 64u);
    patches.notePatch(headerVa, 4u);
    check(patches.prepare(pp).decision == R::Decision::kKeepGpu && patches.patchPending(pp),
          "a patch on a GPU-written page leaves the GPU's ownership of its other bytes");
    // Astra round 2, finding 1: a GPU write heard after a patch supersedes the dwords it covers (both windows), the
    // rest of the patch stays; a patch noted after the GPU write is the newer CPU write and stays whole.
    patches.notePatch(0x80000000u + (pp << R::kPageLog2) + 0x2B8u, 4u);  // dword 0xAE through 0x8 beside 0xAD via 0xA
    patches.notePatch(0x80000000u + (pp << R::kPageLog2) + 0x400u, 4u);  // dword 0x100, outside the GPU write below
    const uint64_t supersededBefore = patches.patchStats().superseded;
    patches.noteGpuAt((pp << R::kPageLog2) + 0x2B5u, 6u, patches.reserveGpu());  // touches dwords 0xAD and 0xAE
    check(patches.patchStats().superseded - supersededBefore == 2u,
          "a GPU write supersedes the two patched dwords it touches (one per window)");
    R::ResourceMemory::Patch kept;
    check(patches.takePatch(pp, kept) && kept.dwords[0][0x100u >> 6] == uint64_t{1} << (0x100u & 63u) &&
          !kept.dwords[0][0xAEu >> 6] && !kept.dwords[1][0xADu >> 6],
          "after the GPU write only the untouched dword stays patched: no older CPU bytes over the GPU's result");
    patches.notePatch(headerVa, 4u);  // a stamp after the GPU write: the CPU's newer bytes
    R::ResourceMemory::Patch later;
    check(patches.takePatch(pp, later) && later.dwords[1][0xADu >> 6] == uint64_t{1} << (0xADu & 63u),
          "a patch noted after the GPU write is kept whole");
    std::atomic<bool> done{false};
    uint64_t seen = 0;
    std::thread stamper([&] {
      for (uint32_t i = 0; i < 20000u; ++i) patches.notePatch(0xB7120008u + 16u * (i & 255u), 4u);
      done.store(true, std::memory_order_release);
    });
    for (;;) {
      const bool last = done.load(std::memory_order_acquire);
      R::ResourceMemory::Patch p;
      if (patches.takePatch(0x17120u, p)) seen |= p.any() ? 1u : 0u;
      if (last) break;
    }
    stamper.join();
    R::ResourceMemory::Patch rest;
    patches.takePatch(0x17120u, rest);
    check(seen == 1u && !patches.patchPending(0x17120u),
          "concurrent stamps and takes lose no pending patch (the final take drains it)");
  }
  {
    // step 6 G8 (SR_PATCH_LEAN): a note of one whole dword whose bit and page are both still pending writes nothing
    // (the pending take copies it); any other state takes the full path. Dword 4 of page 0x17130, through 0xA
    // (0xB7130010) and through 0x8 (0x97130010, the same physical dword).
    constexpr uint32_t lp = 0x17130u, viaA = 0xB7130010u, via8 = 0x97130010u;
    const uint64_t d4 = uint64_t{1} << 4, d5 = uint64_t{1} << 5;
    R::ResourceMemory lean;
    lean.setPatchLean(true);
    lean.notePatch(viaA, 4u);
    const auto s0 = lean.patchStats();
    lean.notePatch(viaA, 4u);  // a shader bind's second stamp: the dword and its page still pending
    const auto s1 = lean.patchStats();
    R::ResourceMemory::Patch t1;
    check(s1.fast == s0.fast + 1u && s1.notes == s0.notes + 1u && s1.dwords == s0.dwords + 1u &&
              lean.takePatch(lp, t1) && t1.dwords[1][0] == d4 && !t1.dwords[0][0] && !lean.patchPending(lp),
          "G8: a second note of a still-pending dword takes the lean path, is counted, and is taken once");
    lean.notePatch(viaA, 4u);  // after the take: the page is no longer pending
    const auto s2 = lean.patchStats();
    R::ResourceMemory::Patch t2;
    check(s2.fast == s1.fast && lean.patchPending(lp) && lean.takePatch(lp, t2) && t2.dwords[1][0] == d4,
          "G8: a note after the take takes the full path and re-arms the page");
    lean.notePatch(viaA, 4u);
    lean.notePatch(viaA + 4u, 4u);  // the page pending, but a DIFFERENT dword
    const auto s3 = lean.patchStats();
    R::ResourceMemory::Patch t3;
    check(s3.fast == s2.fast && lean.takePatch(lp, t3) && t3.dwords[1][0] == (d4 | d5),
          "G8: a pending page with another dword set takes the full path for the new dword");
    lean.notePatch(via8, 4u);  // the dword through 0x8 ...
    lean.notePatch(viaA, 4u);  // ... then through 0xA: the other window's bit is not this note's
    const auto s4 = lean.patchStats();
    R::ResourceMemory::Patch t4;
    check(s4.fast == s3.fast && lean.takePatch(lp, t4) && t4.dwords[0][0] == d4 && t4.dwords[1][0] == d4,
          "G8: a dword pending through one window takes the full path when noted through the other");
    R::ResourceMemory off;
    off.setPatchLean(false);
    off.notePatch(viaA, 4u);
    off.notePatch(viaA, 4u);
    R::ResourceMemory::Patch t5;
    check(off.patchStats().fast == 0u && off.patchStats().notes == 2u && off.takePatch(lp, t5) && t5.dwords[1][0] == d4,
          "G8: SR_PATCH_LEAN=0 takes the full path every time (the control)");
    // the launch switch itself (the job runs this test with SR_PATCH_LEAN unset and =0): an arena left at its default
    // follows the variable, read once
    const char* leanVar = std::getenv("SR_PATCH_LEAN");
    const bool leanWanted = !leanVar || leanVar[0] != '0';
    R::ResourceMemory dflt;
    dflt.notePatch(viaA, 4u);
    dflt.notePatch(viaA, 4u);
    check(R::patchLeanOn() == leanWanted && dflt.patchStats().fast == (leanWanted ? 1u : 0u),
          "G8: an arena at its default follows SR_PATCH_LEAN");
    // a writer storing a value then noting it, a taker taking then reading: the last value written is the one a take
    // copies after the writer stops (the lean path's fence; on x86 a missing fence shows only rarely - the review owns it)
    R::ResourceMemory race;
    race.setPatchLean(true);
    std::atomic<uint32_t> guestWord{0};
    std::atomic<bool> stop{false};
    uint32_t copied = 0;
    std::thread writer([&] {
      for (uint32_t i = 1; i <= 50000u; ++i) {
        guestWord.store(i, std::memory_order_relaxed);
        race.notePatch(viaA, 4u);
      }
      stop.store(true, std::memory_order_release);
    });
    for (;;) {
      const bool last = stop.load(std::memory_order_acquire);
      R::ResourceMemory::Patch p;
      if (race.takePatch(lp, p)) copied = guestWord.load(std::memory_order_relaxed);
      if (last) break;
    }
    writer.join();
    R::ResourceMemory::Patch tail;
    if (race.takePatch(lp, tail)) copied = guestWord.load(std::memory_order_relaxed);
    check(copied == 50000u && !race.patchPending(lp) && race.patchStats().fast > 0u,
          "G8: concurrent notes and takes: the last value written is the last one copied (lean path exercised)");
  }
  {
    // step 6 G8c: the fast-path statistic is a relaxed load + store (no locked RMW). 20 noting threads - more than the
    // 16 shards, so some share one - over 8 dwords of one page, and one taker. No patch is lost: each dword's final
    // value is the last one a take copied. The statistics stay bounded: a shared shard may lose fast counts, never
    // gain any.
    constexpr uint32_t kThreads = 20, kIters = 20000, kDwords = 8, sp = 0x17140u, spVa = 0xB7140000u;
    R::ResourceMemory stress;
    stress.setPatchLean(true);
    std::atomic<uint32_t> words[kDwords] = {};
    std::atomic<uint32_t> running{kThreads}, ready{0};
    std::atomic<bool> go{false};   // the start gate: every noter starts together, so shared shards overlap in time
    uint32_t copiedWords[kDwords] = {};
    std::vector<std::thread> noters;
    for (uint32_t t = 0; t < kThreads; ++t)
      noters.emplace_back([&, t] {
        ready.fetch_add(1, std::memory_order_acq_rel);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (uint32_t i = 1; i <= kIters; ++i) {
          const uint32_t d = (t + i) % kDwords;
          words[d].store(t * kIters + i, std::memory_order_relaxed);
          stress.notePatch(spVa + 4u * d, 4u);
        }
        running.fetch_sub(1, std::memory_order_release);
      });
    const auto takeCopy = [&] {
      R::ResourceMemory::Patch p;
      if (!stress.takePatch(sp, p)) return;
      for (uint32_t d = 0; d < kDwords; ++d)
        if (p.dwords[1][0] >> d & 1u) copiedWords[d] = words[d].load(std::memory_order_relaxed);
    };
    while (ready.load(std::memory_order_acquire) != kThreads) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (;;) {
      const bool last = running.load(std::memory_order_acquire) == 0u;
      takeCopy();
      if (last) break;
    }
    for (auto& n : noters) n.join();
    takeCopy();
    bool same = true;
    for (uint32_t d = 0; d < kDwords; ++d) same = same && copiedWords[d] == words[d].load(std::memory_order_relaxed);
    const auto s = stress.patchStats();
    check(same && !stress.patchPending(sp),
          "G8c: 20 noting threads and a taker: each dword's final value is the last one copied (no lost patch)");
    check(s.fast > 0u && s.fast <= s.notes && s.notes <= uint64_t(kThreads) * kIters && s.dwords <= s.notes,
          "G8c: under shard collisions the statistics stay bounded (fast counts may be lost, never invented)");
  }

  R::ResourceSpan spans[2];
  check(R::resourceTextureSpans(nullptr, spans) == 0, "a missing texture descriptor supplies no invented extent");
  uint32_t fetch[6] = {};
  check(R::resourceTextureSpans(fetch, spans) == 0, "an invalid texture descriptor supplies no invented extent");
  fetch[0] = 2u | (1u << 22);  // 32-texel linear pitch
  fetch[1] = 6u | (0x5000u << 12);  // RGBA8
  fetch[2] = 31u | (31u << 13);
  fetch[4] = 5u << 6;
  fetch[5] = (0x5100u << 12) | (1u << 9);
  check(R::resourceTextureSpans(fetch, spans) == 2 && spans[0].len == 4096u &&
        spans[1].len == 40960u,
        "unpacked mips include all five aligned level strides");
  memory.noteTextureUnlock(fetch);
  check(memory.cpuGeneration(0x5109u) != 0 && memory.cpuGeneration(0x510Au) == 0,
        "Unlock publishes the final mip page and leaves its neighbour untouched");
  fetch[4] = 0;
  fetch[5] = 3u << 9;
  check(R::resourceTextureSpans(fetch, spans) == 1 && spans[0].len == 24576u,
        "a cube includes all six face strides");
  fetch[1] = (0x5000u << 12) | 32u;
  check(R::resourceTextureSpans(fetch, spans) == 1 && spans[0].len == 49152u,
        "RGBA16 float captures all eight bytes of every cube texel");
  fetch[1] = (0x5000u << 12) | 6u;
  fetch[5] = 2u << 9;
  fetch[2] = 3u | (3u << 11) | (2u << 22);  // 4 x 4 x 3, four-slice storage alignment
  check(R::resourceTextureSpans(fetch, spans) == 1 && spans[0].len == 16384u,
        "a volume includes its aligned four-slice storage");
  fetch[5] = 1u << 9;
  fetch[2] = 31u | (31u << 13);
  memory.noteTextureUnlock(fetch, true);
  memory.commit(memory.prepare(0x5000u));
  memory.beginFrame();
  check(memory.prepare(0x5000u).decision == R::Decision::kUploadNoted,
        "a registered movie plane refreshes at the next frame without an invented Lock");
  memory.commit(memory.prepare(0x5000u));
  check(memory.prepare(0x5000u).decision == R::Decision::kKeep,
        "a registered movie plane is copied only once within that frame");
  memory.noteGrant(0x85000000u, 4096u);
  memory.commit(memory.prepare(0x5000u));
  memory.beginFrame();
  check(memory.prepare(0x5000u).decision == R::Decision::kKeep,
        "reallocation expires the previous movie-plane registration");
  memory.noteTextureUnlock(fetch, true);
  memory.noteTextureUnlock(fetch, false);
  memory.commit(memory.prepare(0x5000u));
  memory.beginFrame();
  check(memory.prepare(0x5000u).decision == R::Decision::kKeep,
        "a replacement non-movie Unlock expires the movie-plane registration");
  _putenv_s("SR_MEM_RESOURCES", "1");
  auto& boundary = R::resourceMemory();
  constexpr uint32_t inlineVa = 0xA0100000u, inlinePage = 0x100u, device = 0x81000000u;
  const auto beforeBegin = boundary.generation();
  sr::kernel::resourceInlineBegin(device, inlineVa, 64);
  check(boundary.generation() == beforeBegin && boundary.cpuGeneration(inlinePage) == 0,
        "real Begin latch never announces its unwritten allocation");
  sr::kernel::resourceInlineEnd(device);
  check(boundary.cpuGeneration(inlinePage) > beforeBegin && boundary.sourceWindow(inlinePage, 8) == 0xAu,
        "matching End publishes completed bytes and their actual write window");
  boundary.commit(boundary.prepare(inlinePage));
  const auto queuedResolve = boundary.reserveGpu();
  sr::kernel::resourceInlineBegin(device, inlineVa + 128u, 64);
  check(boundary.prepare(inlinePage).decision == R::Decision::kKeep && boundary.generation() == queuedResolve,
        "a later unwritten allocation sharing the page cannot change an earlier draw's ownership");
  boundary.noteGpuAt(0x100000u, 64, queuedResolve);
  check(boundary.prepare(inlinePage).decision == R::Decision::kKeepGpu,
        "a queued resolve remains newer than an earlier completed inline write");
  sr::kernel::resourceInlineEnd(device);
  check(boundary.prepare(inlinePage).decision == R::Decision::kUploadNoted,
        "the later allocation owns CPU bytes only when its End arrives");
  auto rejects = [&](auto&& fn, const char* name) {
    bool caught = false;
    try { fn(); } catch (const std::logic_error&) { caught = true; }
    check(caught, name);
  };
  rejects([&] { sr::kernel::resourceInlineEnd(device); }, "unmatched End is rejected");
  rejects([&] { sr::kernel::resourceInlineBegin(device, inlineVa, 0x100000000ull); },
          "overflowing inline extent is rejected without arming a latch");
  {
    sr::kernel::ResourceInlineUpScope outer(device);
    sr::kernel::resourceInlineBegin(device, inlineVa, 64);
    {
      sr::kernel::ResourceInlineUpScope inner(device + 4);
      sr::kernel::resourceInlineBegin(device + 4, inlineVa + 4096, 64);
      sr::kernel::resourceInlineMemcpyComplete(inlineVa + 4096, 64);
      inner.finish();
    }
    sr::kernel::resourceInlineMemcpyComplete(inlineVa, 64);
    outer.finish();
    rejects([&] { sr::kernel::resourceInlineMemcpyComplete(inlineVa, 64); }, "duplicate scoped copy is rejected");
  }
  check(boundary.cpuGeneration(inlinePage + 1) != 0, "nested UP scopes retain their own exact allocation");
  rejects([&] {
    sr::kernel::ResourceInlineUpScope scope(device);
    sr::kernel::resourceInlineBegin(device, inlineVa, 64);
    sr::kernel::resourceInlineMemcpyComplete(inlineVa + 4, 64);
  }, "mismatched scoped copy is rejected and its exceptional scope unwinds");
  rejects([&] {
    sr::kernel::ResourceInlineUpScope scope(device);
    sr::kernel::resourceInlineBegin(device, inlineVa, 64);
    scope.finish();
  }, "UP return without its completed copy is rejected");
  {
    sr::kernel::ResourceInlineUpScope scope(device);
    sr::kernel::resourceInlineBegin(device, 0, 64);
    scope.finish();
  }
  sr::kernel::resourceInlineBegin(device, inlineVa, 64);
  sr::kernel::resourceInlineEnd(device);
  check(boundary.cpuGeneration(inlinePage) != 0, "failed UP allocation and exceptional scopes leave no stale latch");
  std::printf("resource_memory_test: %u passed, %u failed\n", passed, failed);
  return failed ? 1 : 0;
}
