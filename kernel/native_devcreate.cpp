// boot.device_create (native-a, 2026-10-10; NATIVE_ROADMAP "boot.device_create BUILD PLAN"): the device-create
// producers of the boot PM4 block as host code, under SR_NATIVE_DEVCREATE on the native transport
// (SR_NATIVE_SEGMENTLESS). Default ON since native/devcreate-fix (2026-10-15; NATIVE_ROADMAP "boot.device_create
// BAKE PLAN": the ON-boot backend wait was the installed shader cache's synchronous boot load on the attach path, in
// both arms, gone since render/boot-attach f9c08c2d); =0 keeps the guest bodies and the per-present A/B arm.
// Host code written from the asm of the generated bodies (guest/ppc, 2026-10-10):
//
//   D3DDeviceInitRingBuffer_820FDE90(dev, params)   - the ring, the command buffer, the fence and scratch blocks
//     (the XDK's own allocator and kernel calls, every device field as the asm stores it), the ME_INIT the XDK
//     writes straight into the ring NOT written (the host CP has no micro-engine - its ME_INIT handler is a
//     no-op - and under the native transport nothing parses the ring; the write index stays 0), the eight
//     register writes as run records: SQ_INST_STORE_MANAGEMENT, PG_START/PG_SIZE/WRITEBACK_START/WRITEBACK_SIZE,
//     SCRATCH_ADDR (the scratch block's physical address: the CP's write-backs), SCRATCH_UMSK, WAIT_UNTIL,
//     SQ_EO_RT, with the XDK's kick checks at its own sites.
//   GpuCreateBlockInterruptThreads_82110EB8(dev)    - the per-CPU block-interrupt threads (ExCreateThread, the
//     XDK's blocks in the device), the EVENT_WRITE_SHD that seeds each CPU's interrupt word as a kRkEventShd
//     record.
//
// D3DDeviceResetState_8210E520 and the segment re-emit 820F0048 live in native_flush.cpp (their packet tails are
// Driver items with dry CHECK kinds 8 / 9). Rule 1 (RE-PLAN): each port declines the WHOLE call - the guest body
// runs - before it writes anything: the gate, the transport, the re-init path (a ring already exists), no
// parameters, and a probe of the Entry (opened and abandoned) all decide at the top. The one path a port cannot
// decline at the top is the Entry's open after the allocations (arena pressure, unreachable at boot: the arena
// is empty); it is logged and the guest body runs whole (the four blocks allocated here leak, nothing else).
//
// Proof: tools/perf/native_proof.py --plan devcreate (ledger boot.device_create).

#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "native_devcreate.hpp"
#include "native_flush.hpp"
#include "../app/log.hpp"

extern "C" void __imp__sub_820FDE90(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82110EB8(PPCContext& __restrict ctx, uint8_t* base);

// The gate as a zero-arg helper in this file (scripts/gen_override_manifest.py reads same-file helpers); the
// transport and record-mode conditions are native_flush's devCreateOn().
static bool devCreateGate() {
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_DEVCREATE");
    return !e || e[0] != '0';
  }();
  return on;
}

namespace sr::native::devcreate {
namespace {

uint32_t rd(uint32_t va) { return sr::kernel::readGuestU32BE(va); }
void wr(uint32_t va, uint32_t v) { sr::kernel::writeGuestBE32(va, v); }
void wr8(uint32_t va, uint8_t v) {
  if (uint8_t* p = sr::kernel::guestVAToHost(va)) *p = v;
}
// The XDK's inline VA -> physical conversion (the 0xE0000000 window carries its 4 KB offset).
uint32_t physOf(uint32_t va) { return (((va >> 20) + 0x200u) & 0x1000u) + (va & 0x1FFFFFFFu); }
uint32_t log2u(uint32_t v) {  // 31 - cntlzw(v); 0xFFFFFFFF for 0, as the asm computes it
  unsigned long i = 0;
  return v && _BitScanReverse(&i, v) ? uint32_t(i) : 0xFFFFFFFFu;
}
void setArgs(PPCContext& ctx, uint64_t r3, uint64_t r4 = 0, uint64_t r5 = 0, uint64_t r6 = 0, uint64_t r7 = 0,
             uint64_t r8 = 0, uint64_t r9 = 0) {
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  ctx.r7.u64 = r7;
  ctx.r8.u64 = r8;
  ctx.r9.u64 = r9;
}
// A guest helper or a kernel import with its register arguments; the result is r3.
#define SR_DC_CALL(fn, ...) (setArgs(ctx, __VA_ARGS__), fn(ctx, base), ctx.r3.u32)

void be4(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

// A probe of the Entry: can an entry open on this device right now? Nothing is written either way.
bool entryWouldOpen(PPCContext& ctx, uint8_t* base, uint32_t dev) {
  sr::native::flush::Entry probe(ctx, base, dev);
  if (!probe.open()) return false;
  probe.abandon();
  return true;
}

std::atomic<uint32_t> g_ring{0}, g_ringDecl{0}, g_ringLate{0}, g_irq{0}, g_irqDecl{0}, g_irqLate{0};

// D3DDeviceInitRingBuffer_820FDE90(dev r3, params r4): true = handled (r3 set as the asm returns it).
bool initRingBuffer(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32, params = ctx.r4.u32;
  // The re-init path (a ring, a command buffer or the blocks exist: the asm's teardown) and the no-parameter
  // call stay the guest's; so does a device no entry can open on.
  if (rd(dev + 10780) || rd(dev + 13484) || rd(dev + 13416) || rd(dev + 13420) || rd(dev + 10768) ||
      rd(dev + 10772) || !params || !entryWouldOpen(ctx, base, dev)) {
    g_ringDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  // The asm's teardown on a fresh device: the fields cleared, the identifier address cleared, four frees of 0.
  wr(dev + 13416, 0);
  wr(dev + 13420, 0);
  wr(dev + 40, 0);
  wr(dev + 44, 0);
  SR_DC_CALL(__imp__VdSetSystemCommandBufferGpuIdentifierAddress, 0);
  SR_DC_CALL(sub_82167F98, 0, 0xB1800000u);
  SR_DC_CALL(sub_82167F98, 0, 0xB1800000u);
  SR_DC_CALL(sub_82167F98, 0, 0xB1800000u);
  SR_DC_CALL(sub_82167F98, 0, 0xB1800000u);
  // The parameters with the asm's defaults: a 32 KB ring, 2 MB of command buffer in 32 segments.
  uint32_t ringSize = rd(params + 4), ringVA = rd(params + 8), cmdSize = rd(params + 12), cmdVA = rd(params + 16),
           segs = rd(params + 20);
  if (!ringSize) ringSize = 0x8000u;
  if (!cmdSize) cmdSize = 0x200000u;
  if (!segs) segs = 32u;
  const uint32_t segSize = cmdSize / segs;
  if (!ringVA) ringVA = SR_DC_CALL(sub_82167F90, ringSize, 0xB5800000u);
  wr(dev + 13416, ringVA);
  if (!cmdVA) cmdVA = SR_DC_CALL(sub_82167F90, cmdSize, cmdSize < 0x80000u ? 0xB7800000u : 0x87800000u);
  wr(dev + 13420, cmdVA);
  if (!ringVA || !cmdVA) {
    ctx.r3.u64 = 0x8007000Eu;
    g_ring.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  const uint32_t fb = SR_DC_CALL(sub_82167F90, 96u, 0xA5800000u);  // the fence block (fb): S, progress, id, rptr
  wr(dev + 10768, fb);
  if (!fb) {
    ctx.r3.u64 = 0x8007000Eu;
    g_ring.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  const uint32_t pc = SR_DC_CALL(sub_82167F90, 32u, 0x95800000u);  // the scratch block (pc): the CP's write-backs
  wr(dev + 10772, pc);
  if (!pc) {
    ctx.r3.u64 = 0x8007000Eu;
    g_ring.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  SR_DC_CALL(sub_8279F290, fb, 0, 96u);
  SR_DC_CALL(sub_8279F290, pc, 0, 32u);
  const uint32_t ringLog2 = log2u(ringSize);
  const uint32_t ringPhys = SR_DC_CALL(__imp__MmGetPhysicalAddress, ringVA);
  SR_DC_CALL(__imp__VdInitializeRingBuffer, ringPhys, ringLog2 - 3u);
  uint32_t blockLog2 = log2u((ringSize >> 9) & 0x7FFFFFu);
  if (blockLog2 > 19u) blockLog2 = 19u;
  SR_DC_CALL(__imp__VdEnableRingBufferRPtrWriteBack, physOf(fb + 60u), blockLog2);
  // The device's transport fields, as the asm stores them.
  const uint32_t cmdDw = cmdSize >> 2, ringDw = ringSize >> 2;
  const uint32_t segEnd = cmdVA + (segSize & ~3u), sentinel = cmdVA + cmdDw * 4u - 4u;
  wr(sentinel, 0x0BADF00Du);
  wr(dev + 13516, cmdDw);
  wr(dev + 13476, ringVA);
  wr(dev + 13484, cmdVA);
  wr(dev + 13504, cmdVA);
  wr(dev + 13488, sentinel);
  wr(dev + 13508, cmdVA);
  wr(dev + 13492, segSize);
  wr(dev + 13480, ringDw - 1u);
  wr(fb + 60u, 0);
  wr(dev + 10820, 0);
  wr(dev + 40, cmdVA - 4u);
  wr(dev + 44, segEnd);
  wr(dev + 48, segEnd - 160u);
  wr(dev + 13496, 0);
  if (!rd(dev + 10780)) wr(dev + 10780, 3u);
  wr(fb, rd(dev + 10780) - 2u);
  wr(fb + 4u, (rd(dev + 13500) & 3u) | rd(dev + 40));
  SR_DC_CALL(__imp__VdSetSystemCommandBufferGpuIdentifierAddress, fb + 8u);
  // ME_INIT (19 dwords into the ring, after GpuRingWaitReadPtrWrap and KiApcNormalRoutineNop(buf, 19) - a generic
  // no-op stub here): not written, see the file comment. Then the eight register writes as records.
  sr::native::flush::Entry e(ctx, base, dev);
  if (!e.open()) {  // unreachable (the probe opened on this device moments ago): the guest body, whole
    g_ringLate.fetch_add(1, std::memory_order_relaxed);
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[NATIVE_DEVCREATE] ring init: the entry declined after the allocations; the guest body runs whole");
    ctx.r3.u64 = dev;
    ctx.r4.u64 = params;
    __imp__sub_820FDE90(ctx, base);
    return true;
  }
  uint8_t v[16];
  e.kick();
  be4(v, 0x00010800u);
  e.run(0x0D02u, 1u, v);  // SQ_INST_STORE_MANAGEMENT
  be4(v, 0xC0100000u);
  be4(v + 4, 0x07F00000u);
  be4(v + 8, 0xC0000000u);
  be4(v + 12, 0x00100000u);
  e.run(0x0A02u, 4u, v);  // PG_START, PG_SIZE, WRITEBACK_START, WRITEBACK_SIZE
  be4(v, physOf(pc));
  e.run(0x01DDu, 1u, v);  // SCRATCH_ADDR
  be4(v, 0x00020033u);
  e.run(0x01DCu, 1u, v);  // SCRATCH_UMSK
  e.kick();
  be4(v, 0x00020000u);
  e.run(0x05C8u, 1u, v);  // WAIT_UNTIL
  be4(v, 0u);
  e.run(0x0D04u, 1u, v);  // SQ_EO_RT
  e.close();
  ctx.r3.u64 = 0;
  g_ring.fetch_add(1, std::memory_order_relaxed);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_DEVCREATE] ring init native: dev=%08X ring=%08X/%u cmd=%08X/%u seg=%u fb=%08X pc=%08X (ME_INIT not written)",
              dev, ringVA, ringSize, cmdVA, cmdSize, segSize, fb, pc);
  return true;
}

// GpuCreateBlockInterruptThreads_82110EB8(dev r3): true = handled (r3 = 1, or 0 when a thread failed, as the asm).
bool createBlockInterruptThreads(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  const uint32_t flags = rd(dev + 20416);
  if ((flags & 0x100u) || !entryWouldOpen(ctx, base, dev)) {  // no threads wanted (the asm returns 1 at once)
    g_irqDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  wr(dev + 10816, rd(dev + 13476));
  wr(dev + 10824, rd(dev + 13480));
  wr(dev + 10872, 0);
  wr(dev + 10908, 0xC0000000u);
  wr(dev + 10924, 0x7F100000u);
  wr8(dev + 10876, 0);
  wr(dev + 10880, 0);
  wr(dev + 10884, dev + 10884);  // the list head {flink, blink} = itself
  wr(dev + 10888, dev + 10884);
  uint32_t win = 0;
  for (uint32_t cpu = 0, page = 0x7F000000u, blk = dev + 11236; page < 0x7F000300u; cpu++, page += 0x80u, blk += 56u) {
    if (!(flags & (0x01000000u << cpu))) continue;
    wr(dev + 11192, cpu);
    wr(blk - 36u, cpu);
    wr(blk - 40u, dev + 10812);
    const uint32_t winBase = rd(dev + 10924) + win;
    win += 0x8000u;
    wr(blk - 24u, winBase);
    wr(blk - 16u, (page + 0x41000000u) | 2u);
    wr(blk - 12u, page);
    wr(blk - 32u, rd(blk - 24u));
    wr(blk - 20u, rd(dev + 10924) + win);
    // EVENT_WRITE_SHD {3, the CPU's interrupt word, the window base (its low 2 bits from blk-28)}
    sr::native::flush::Entry e(ctx, base, dev);
    if (!e.open()) {  // unreachable (the probe): the guest body, whole - it redoes this CPU's fields and continues
      g_irqLate.fetch_add(1, std::memory_order_relaxed);
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[NATIVE_DEVCREATE] interrupt threads: the entry declined mid-way; the guest body runs whole");
      ctx.r3.u64 = dev;
      __imp__sub_82110EB8(ctx, base);
      return true;
    }
    e.kick();
    e.eventShd(3u, rd(blk - 16u), (rd(blk - 28u) & 3u) | (rd(blk - 32u) & ~3u));
    e.close();
    wr8(blk - 8u, 0);
    wr(blk - 4u, 0);
    wr(blk, blk);
    wr(blk + 4u, blk);
    const uint32_t r = SR_DC_CALL(__imp__ExCreateThread, blk + 8u, 0x8000u, blk + 12u, 0, 0x82110A30u, blk - 40u,
                                  0x80u | ((1u << cpu) << 24));
    if (int32_t(r) < 0) {
      ctx.r3.u64 = 0;
      g_irq.fetch_add(1, std::memory_order_relaxed);
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[NATIVE_DEVCREATE] interrupt thread for cpu %u: ExCreateThread %08X", cpu, r);
      return true;
    }
    SR_DC_CALL(__imp__KeSetBasePriorityThread, rd(blk + 8u), 17u);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_DEVCREATE] interrupt thread native: cpu %u word=%08X window=%08X thread=%08X",
                cpu, rd(blk - 16u), winBase, rd(blk + 8u));
  }
  ctx.r3.u64 = 1;
  g_irq.fetch_add(1, std::memory_order_relaxed);
  return true;
}

}  // namespace

void finalLine(const char* why) {
  if (!sr::native::flush::devCreateOn()) return;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_DEVCREATE] %s ringInit=%u (decl %u, late %u) irqThreads=%u (decl %u, late %u) resetState=%llu segmentBegin=%llu (decl %llu)",
              why, g_ring.load(), g_ringDecl.load(), g_ringLate.load(), g_irq.load(), g_irqDecl.load(), g_irqLate.load(),
              (unsigned long long)sr::native::flush::devResetCalls(), (unsigned long long)sr::native::flush::segBeginCalls(),
              (unsigned long long)sr::native::flush::devCreateDecl());
}

}  // namespace sr::native::devcreate

PPC_FUNC(sub_820FDE90) {
  if (devCreateGate() && sr::native::flush::devCreateOn() && sr::native::devcreate::initRingBuffer(ctx, base)) return;
  __imp__sub_820FDE90(ctx, base);
}
PPC_FUNC(sub_82110EB8) {
  if (devCreateGate() && sr::native::flush::devCreateOn() && sr::native::devcreate::createBlockInterruptThreads(ctx, base)) return;
  __imp__sub_82110EB8(ctx, base);
}
