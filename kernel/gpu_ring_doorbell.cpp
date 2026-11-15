// iter-372: read the guest's GPU submissions where it actually makes them.
//
// THE PROBLEM THIS SOLVES. Until now the live PM4 scan inferred one command
// window per VdSwap from the delta between successive ring write pointers
// ([prev r3 + 0x100, curr r3)). That is a single contiguous range, so it can
// present at most ONE command segment per frame. iter-371 measured the guest's
// own fence counter (device+10780, bumped 2 per segment terminator) and found:
//
//     boot,  swaps 1-3     : fence 5, 7, 9            -> +2 per swap (1 segment)
//     later, swaps 2760-2910: fence 12797 -> 13691    -> +6 per swap (3 segments)
//
// and the console does TWELVE per frame. The guest closes segments DURING the
// frame; by the time VdSwap runs, device+40 (the cursor) sits BEFORE
// device+13508 (the segment start) because the segment has already rotated and
// is empty. A once-per-VdSwap scan cannot see mid-frame submissions at all.
//
// THE CONSOLE CONTRACT (ENGINE_MAP 6d, pinned by the iter-371 workflow).
// GpuRingUpdateWritePointer_sub_820FD218 writes a PM4_INDIRECT_BUFFER
// descriptor into a 64 KB ring and then rings a doorbell:
//
//   ring dwords   [device+13476]  ring base VA
//                 [device+13480]  dword index mask (0x3FFF = 16K dwords = 64 KB)
//                 [device+10820]  write index, in DWORDS
//   descriptor    0xC0013F00  <IB physical address>  <IB size in dwords>
//   doorbell      store to CP_RB_WPTR at guest VA 0x7FC80714 (the ONLY such
//                 store in the entire XEX)
//
// The command processor consumes descriptors between its read pointer and that
// write pointer. This file is that command processor: it tracks a read index,
// and on every doorbell drains the descriptors the guest just published. That is
// a PLATFORM implementation - the guest decides what to submit and when; we
// implement the consumer the console provided. Nothing here writes guest state
// or calls a guest function the game did not call.
//
// Reading the ring needs no physical-address translation: device+13476 holds a
// VA. The IB targets inside the descriptors are physical, but they point at the
// game's command-buffer allocation in the VIRTUAL range (0x8A40DC80-class),
// which the existing phys->host mapping already resolves correctly. The
// unrelated 0xA-range aliasing gap therefore does not block this path.
//
// SR_PM4_RING=0 disables the drain entirely (A/B).
// SR_PM4_RING_EXEC executes the descriptors — DEFAULT ON since iter-378 (=0
//   keeps the A/B): proven across every 2026-07-26 batch (clean parse, console
//   opcode set, badIB=0, 713KB/run vs the window scan's 40 bytes). The legacy
//   VdSwap window scan stands down automatically when this is on
//   (gfx/null_renderer.cpp), so there is no double-execution.
//
// C1 (docs/RELEASE_COMPOSITION.md 2.2): with the asynchronous CP (sr::kernel::gpuCpActive(), constant true in the
// release configuration) this override only PUBLISHES the write index; the synchronous walk below (the A/B's
// SR_PM4_RING / SR_PM4_RING_EXEC arms) is a fallback component compiled out under SR_ORACLE=0.

#include "guest/ppc/ppc_recomp_shared.h"
#include "sr_oracle.hpp"
#include "gpu_submit.hpp"
#include "guest_context.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include "native_brackets.hpp"
#include "gpu_ringless.hpp"   // iter-467 native-a P2-A

namespace sr {
namespace gfx {
// Defined in gfx/null_renderer.cpp — executes a contiguous PM4 range through
// THE shared engine (the same one the golden-trace replay is verified against).
void executeGuestIndirectBuffer(uint32_t ibPhys, uint32_t ibSizeDwords);
}  // namespace gfx
}  // namespace sr

namespace {

#if SR_ORACLE
bool ringDrainEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_RING");
    return !e || e[0] != '0';
  }();
  return v;
}

bool ringExecEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_RING_EXEC");
    return !e || e[0] != '0';   // iter-378: baked default-ON after K-run proof
  }();
  return v;
}
#endif

// Device pointer, resolved the same way stub_VdSwap does (proven path):
// a global at 0x82000948 holds a pointer to the pointer to the D3D device.
uint32_t deviceVA() {
  constexpr uint32_t kGlobalPtrVA = 0x82000948u;
  const uint32_t p = sr::kernel::readGuestU32BE(kGlobalPtrVA);
  return p ? sr::kernel::readGuestU32BE(p) : 0u;
}

#if SR_ORACLE
std::atomic<uint32_t> g_readIndex{0};      // our CP read pointer, in dwords
std::atomic<bool> g_haveReadIndex{false};
std::atomic<uint64_t> g_descriptors{0};    // IB descriptors consumed
std::atomic<uint64_t> g_doorbells{0};
std::atomic<uint64_t> g_ibDwords{0};       // total IB payload published
std::atomic<uint64_t> g_foreign{0};        // ring dwords that were not a 0x3F IB
#endif

void drainRing() {
#if SR_ORACLE
  if (!ringDrainEnabled()) return;
#endif
  const uint32_t dev = deviceVA();
  if (!dev) return;

  const uint32_t ringBaseVA = sr::kernel::readGuestU32BE(dev + 13476);
  const uint32_t mask       = sr::kernel::readGuestU32BE(dev + 13480);
  const uint32_t wptr       = sr::kernel::readGuestU32BE(dev + 10820);
  // The mask is (ring dwords - 1) and must be a power-of-two-minus-one; a
  // 64 KB ring gives 0x3FFF. Reject anything else rather than walk garbage.
  if (!ringBaseVA || !mask || (mask & (mask + 1u)) != 0u || mask > 0xFFFFFu) return;
  if (wptr > mask) return;

  // P2: the async command processor consumes the ring on its own thread
  // (kernel/gpu_command_processor.cpp) — the doorbell only publishes.
  if (sr::kernel::gpuCpActive()) {
    sr::kernel::gpuCpDoorbell(ringBaseVA, mask, wptr);
    return;
  }
#if SR_ORACLE
  const uint64_t n = g_doorbells.fetch_add(1, std::memory_order_relaxed);

  // First doorbell: adopt the guest's write pointer as our starting read
  // pointer. Everything before it was published while we were not looking, and
  // replaying it would execute stale commands.
  if (!g_haveReadIndex.exchange(true, std::memory_order_relaxed)) {
    g_readIndex.store(wptr, std::memory_order_relaxed);
    SR_LOG_KERNEL_IMPORT(
        "[PM4_RING] attach: base=0x%08X dwords=%u wptr=%u (dev=0x%08X)",
        ringBaseVA, mask + 1u, wptr, dev);
    return;
  }

  uint32_t rd = g_readIndex.load(std::memory_order_relaxed);
  uint32_t consumed = 0, descs = 0;
  // Walk [rd, wptr) with wraparound. Bound the walk by the ring size so a bad
  // pointer can never spin here.
  while (rd != wptr && consumed <= mask) {
    const uint32_t hdr = sr::kernel::readGuestU32BE(ringBaseVA + rd * 4u);
    if (hdr == 0xC0013F00u) {
      const uint32_t ibPhys  = sr::kernel::readGuestU32BE(
          ringBaseVA + ((rd + 1u) & mask) * 4u);
      const uint32_t ibDwords = sr::kernel::readGuestU32BE(
          ringBaseVA + ((rd + 2u) & mask) * 4u);
      descs++;
      g_descriptors.fetch_add(1, std::memory_order_relaxed);
      g_ibDwords.fetch_add(ibDwords, std::memory_order_relaxed);
      if (descs <= 2 && n < 64)
        SR_LOG_KERNEL_IMPORT(
            "[PM4_RING] IB desc #%llu: phys=0x%08X size=%u dwords (%u bytes)",
            (unsigned long long)g_descriptors.load(std::memory_order_relaxed),
            ibPhys, ibDwords, ibDwords * 4u);
      if (ringExecEnabled())
        sr::gfx::executeGuestIndirectBuffer(ibPhys, ibDwords);
      rd = (rd + 3u) & mask;
      consumed += 3;
      continue;
    }
    // Not an IB descriptor. The ring should carry nothing else, so count it and
    // step one dword rather than guess at a length.
    if (hdr != 0) g_foreign.fetch_add(1, std::memory_order_relaxed);
    rd = (rd + 1u) & mask;
    consumed++;
  }
  g_readIndex.store(rd, std::memory_order_relaxed);

  // Periodic census: descriptors and payload per doorbell is the number that
  // decides whether the VdSwap window scan was missing submissions.
  if (n < 8 || (n % 512) == 0)
    SR_LOG_KERNEL_IMPORT(
        "[PM4_RING] doorbell #%llu: +%u desc this kick | totals desc=%llu "
        "ibDwords=%llu (%llu KB) foreign=%llu",
        (unsigned long long)n, descs,
        (unsigned long long)g_descriptors.load(std::memory_order_relaxed),
        (unsigned long long)g_ibDwords.load(std::memory_order_relaxed),
        (unsigned long long)(g_ibDwords.load(std::memory_order_relaxed) * 4 / 1024),
        (unsigned long long)g_foreign.load(std::memory_order_relaxed));
#endif  // SR_ORACLE (the synchronous legacy walk)
}

}  // namespace

#if SR_ORACLE
namespace sr::kernel {
void gpuRingDoorbellLine() {
  SR_LOG_KERNEL_IMPORT(
      "[PM4_RING] FINAL doorbells=%llu descriptors=%llu ibDwords=%llu (%llu KB) "
      "foreign=%llu exec=%d",
      (unsigned long long)g_doorbells.load(std::memory_order_relaxed),
      (unsigned long long)g_descriptors.load(std::memory_order_relaxed),
      (unsigned long long)g_ibDwords.load(std::memory_order_relaxed),
      (unsigned long long)(g_ibDwords.load(std::memory_order_relaxed) * 4 / 1024),
      (unsigned long long)g_foreign.load(std::memory_order_relaxed),
      ringExecEnabled() ? 1 : 0);
}
}  // namespace sr::kernel
#endif  // SR_ORACLE

// Called only after the Oracle entry has run the guest producer.
namespace sr::kernel {
void gpuRingLegacyDoorbell() { drainRing(); }
}
