// iter-467 N6 (kernel/ root so gen_override_manifest.py sees it): the audio hardware residue -
// a checkable count of every guest access to the XMA registers, and the native replacement of
// the one access every boot makes.
//
// The static inventory (docs/NATIVE_ROADMAP.md Appendix B.1, scan_mmio.py over all 142
// ppc_recomp.*.cpp, cross-checked against the 13 eieio sites) lists exactly six functions that
// touch the XMA register block 0x7FEA0000 + 4*reg:
//   sub_82116800   lwbrx reg 0x600 (context-array physical base) -> dword_829AC0E8, every boot
//   sub_82119980   Xma_CreateContexts        stwbrx 0x6A0+ (Clear)
//   sub_82119BA8   XmaHw_LockContexts        stwbrx 0x690+ (Lock)
//   sub_82119C28   XmaHw_WaitIdleOrResetDecoder  2 x stwx 0x601 (only on a lock timeout)
//   sub_82119CE8   XmaHw_PollIdleAndSyncShadow   lwbrx 0x606
//   sub_82119E68   XmaHw_WritebackShadowAndKick  stwbrx 0x650+ (Kick)
// The other five XDK XMA HAL functions touch no register (NATIVE_ROADMAP B.2) and are counted the
// same way for N6's "HAL dead: witness = 0" (xma_hal_rest): Xma_AllocContextBlock_sub_82119748,
// Xma_ReleaseContexts_sub_82119A50, Xma_SubmitInputBuffer_sub_82119AC8,
// XmaShadow_ConsumeOutputRing_sub_82119FA8, XmaShadow_ResetSlot_sub_8211A160 (IDA: every code
// caller is an XmaVoice* method or another HAL function).
// The harness has no MMIO trap (0x7FC00000.. is plain RAM), so an access cannot be counted at
// the address; it is counted at the function: each wrapper below is a pass-through that counts
// entries (an upper bound on the accesses: 0 entries = 0 accesses). [AUDIO_NATIVE] prints the
// totals every 30 s from the audio worker (and so carries the whole-run value in its last line),
// together with the XMA device's register-file writes (XmaDevice::regfileWrites: 0 when the
// exports use the native front end, SR_XMA_NATIVE_FRONT).
//
// SR_XMA_ARRAY_NATIVE (default ON since the rolls; =0 keeps the A/B): sub_82116800 stores the
// context array's physical base from the native XMA device (what Xenia's register 0x600 returns:
// XmaDecoder::Setup context_data_first_ptr_ PA; golden sound_gameplay dword_829AC0E8 = 0x1FCAA000
// = Xenia's array) instead of reading the register block (our plain RAM: no device behind it).
// The value feeds only the XDK XMA HAL, dormant in this game (ENGINE_MAP 7a).
#include "guest/ppc/ppc_recomp_shared.h"
#include "kernel/guest_context.hpp"
#include "kernel/apu/xma_device.hpp"
#include "kernel/audio_native_census.hpp"
#include "kernel/sr_oracle.hpp"
#include "app/log.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#if SR_ORACLE
extern "C" void __imp__sub_82116800(PPCContext& __restrict ctx, uint8_t* base);
#endif
// Six MMIO-entry counters are retained diagnostics: never delegate unseen in release.
// They do not implement a register device or alter the original guest operation.
extern "C" void __imp__sub_82119BA8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82119C28(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82119CE8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82119E68(PPCContext& __restrict ctx, uint8_t* base);
#if !SR_ORACLE
extern "C" void __imp__sub_82119980(PPCContext& __restrict ctx, uint8_t* base);
#endif
#if SR_ORACLE
extern "C" void __imp__sub_82119748(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82119A50(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82119AC8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82119FA8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8211A160(PPCContext& __restrict ctx, uint8_t* base);

namespace sr::kernel {
uint32_t xmaHalCreateContextsCalls();   // kernel/xma_hal_observe.cpp
uint32_t xmaHalVoiceCtors();
}
#endif

namespace {

std::atomic<uint64_t> g_reg600Legacy{0}, g_reg600Native{0}, g_halLock{0}, g_halWaitIdle{0},
    g_halPoll{0}, g_halKick{0};
#if !SR_ORACLE
std::atomic<uint64_t> g_halClear{0};
#endif
#if SR_ORACLE
std::atomic<uint64_t> g_halAlloc{0}, g_halRelease{0}, g_halSubmit{0}, g_halConsume{0}, g_halReset{0};
#endif
std::atomic<uint32_t> g_reg600Value{0};

bool arrayNativeOn() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_XMA_ARRAY_NATIVE");
    return !e || e[0] != '0';   // iter-467: baked ON after the rolls; =0 keeps the A/B
  }();
  return v;
#else
  return true;  // Native array publication is mandatory in release.
#endif
}

constexpr uint32_t kXmaArrayBaseCell = 0x829AC0E8u;   // dword_829AC0E8

}  // namespace

// AudioEngine create prologue: reg 0x600 -> dword_829AC0E8, then AudioDeviceCreate_sub_82117018
// (r3 = r5, r4 = r6, r5 = &dword_829AC104).
PPC_FUNC(sub_82116800) {
  if (!arrayNativeOn() || !sr::kernel::apu::xmaNewEnabled() ||
      !sr::kernel::apu::XmaDevice::instance().ensureInit()) {
    g_reg600Legacy.fetch_add(1, std::memory_order_relaxed);
#if SR_ORACLE
    __imp__sub_82116800(ctx, base);
    g_reg600Value.store(sr::kernel::readGuestU32BE(kXmaArrayBaseCell), std::memory_order_relaxed);
    return;
#else
    // Count the refused MMIO boundary, then stop without executing guest hardware access.
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_XMA_BAD] native context-array publication unavailable");
    sr::Log::shutdown();
    std::_Exit(3);
#endif
  }
  const uint32_t pa = sr::kernel::apu::XmaDevice::instance().contextArrayPA();
  g_reg600Native.fetch_add(1, std::memory_order_relaxed);
  g_reg600Value.store(pa, std::memory_order_relaxed);
  // The original, minus the lwbrx: the same registers at the tail call.
  ctx.r3.u64 = ctx.r5.u64;
  ctx.r4.u64 = ctx.r6.u64;
  ctx.r10.s64 = -2103771136;            // lis r10,-32101 (0x829B0000)
  ctx.r5.s64 = -2103771136 + -16124;    // lis r11,-32101; addi r5,r11,-16124 (0x829AC104)
  sr::kernel::writeGuestBE32(kXmaArrayBaseCell, pa);   // stw r11,-16152(r10)
  sub_82117018(ctx, base);
}

// The XDK XMA HAL's register functions: pass-through entry counters (observe only).
PPC_FUNC(sub_82119BA8) {
  g_halLock.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82119BA8(ctx, base);
}
PPC_FUNC(sub_82119C28) {
  g_halWaitIdle.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82119C28(ctx, base);
}
PPC_FUNC(sub_82119CE8) {
  g_halPoll.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82119CE8(ctx, base);
}
PPC_FUNC(sub_82119E68) {
  g_halKick.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82119E68(ctx, base);
}

#if !SR_ORACLE
// Oracle's existing xma_hal_observe.cpp owns this wrapper there. Retain only the
// register-entry witness in release, before the unchanged body can fault.
PPC_FUNC(sub_82119980) {
  g_halClear.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82119980(ctx, base);
}
#endif

#if SR_ORACLE
// The HAL's register-free functions: pass-through entry counters (observe only).
PPC_FUNC(sub_82119748) {
  g_halAlloc.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82119748(ctx, base);
}
PPC_FUNC(sub_82119A50) {
  g_halRelease.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82119A50(ctx, base);
}
PPC_FUNC(sub_82119AC8) {
  g_halSubmit.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82119AC8(ctx, base);
}
PPC_FUNC(sub_82119FA8) {
  g_halConsume.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82119FA8(ctx, base);
}
PPC_FUNC(sub_8211A160) {
  g_halReset.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_8211A160(ctx, base);
}

#endif

namespace sr::kernel {

AudioNativeCensus audioNativeCensus() {
  AudioNativeCensus c;
  c.reg600_lwbrx = g_reg600Legacy.load(std::memory_order_relaxed);
#if SR_ORACLE
  c.hal_clear = xmaHalCreateContextsCalls();
#else
  c.hal_clear = g_halClear.load(std::memory_order_relaxed);
#endif
  c.hal_lock = g_halLock.load(std::memory_order_relaxed);
  c.hal_waitidle = g_halWaitIdle.load(std::memory_order_relaxed);
  c.hal_poll = g_halPoll.load(std::memory_order_relaxed);
  c.hal_kick = g_halKick.load(std::memory_order_relaxed);
  c.guest_mmio_fns = c.reg600_lwbrx + c.hal_clear + c.hal_lock + c.hal_waitidle + c.hal_poll + c.hal_kick;
  c.xma_regfile_writes = apu::XmaDevice::instance().regfileWrites();
#if SR_ORACLE
  c.xma_hal_voices = xmaHalVoiceCtors();
  c.xma_hal_rest = g_halAlloc.load(std::memory_order_relaxed) + g_halRelease.load(std::memory_order_relaxed) +
                   g_halSubmit.load(std::memory_order_relaxed) + g_halConsume.load(std::memory_order_relaxed) +
                   g_halReset.load(std::memory_order_relaxed);
#endif
  c.reg600_native = g_reg600Native.load(std::memory_order_relaxed);
  c.reg600_value = g_reg600Value.load(std::memory_order_relaxed);
  return c;
}

// One [AUDIO_NATIVE] line (formatted here, logged by the caller); the keys are the struct's fields.
int audioNativeCensusLine(char* out, size_t n) {
  const AudioNativeCensus c = audioNativeCensus();
#if !SR_ORACLE
  // Register-free HAL counters are not retained; do not print invented zeroes.
  return std::snprintf(out, n,
      "guest_mmio_fns=%llu (reg600_lwbrx=%llu hal_clear=%llu hal_lock=%llu hal_waitidle=%llu "
      "hal_poll=%llu hal_kick=%llu) xma_regfile_writes=%llu reg600_native=%llu scope=mmio",
      (unsigned long long)c.guest_mmio_fns, (unsigned long long)c.reg600_lwbrx,
      (unsigned long long)c.hal_clear, (unsigned long long)c.hal_lock,
      (unsigned long long)c.hal_waitidle, (unsigned long long)c.hal_poll,
      (unsigned long long)c.hal_kick, (unsigned long long)c.xma_regfile_writes,
      (unsigned long long)c.reg600_native);
#else
  return std::snprintf(
      out, n,
      "guest_mmio_fns=%llu (reg600_lwbrx=%llu hal_clear=%llu hal_lock=%llu hal_waitidle=%llu "
      "hal_poll=%llu hal_kick=%llu) xma_regfile_writes=%llu reg600=%s:%08X xma_hal_voices=%llu "
      "xma_hal_rest=%llu",
      (unsigned long long)c.guest_mmio_fns, (unsigned long long)c.reg600_lwbrx,
      (unsigned long long)c.hal_clear, (unsigned long long)c.hal_lock,
      (unsigned long long)c.hal_waitidle, (unsigned long long)c.hal_poll,
      (unsigned long long)c.hal_kick, (unsigned long long)c.xma_regfile_writes,
      c.reg600_native ? "native" : (c.reg600_lwbrx ? "lwbrx" : "not-run"), c.reg600_value,
      (unsigned long long)c.xma_hal_voices, (unsigned long long)c.xma_hal_rest);
#endif
}

}  // namespace sr::kernel
