// iter-463 audio OBSERVE (kernel/xma_hal_observe.cpp - kernel/ root so gen_override_manifest.py sees it): activation witness for the XDK XAudio XMA HAL (the direct-MMIO path).
//
// The game links the XDK XAudio XMA voice class, whose HAL pokes the XMA registers directly
// (stwbrx to 0x7FEA1xxx: Clear at 0x82119A18, Lock at 0x82119BF8, Kick at 0x82119F74). Those
// stores reach no device in our harness: the native XMA port (kernel/apu/xma_device.cpp) is
// driven through the XMA* kernel exports only. docs/ENGINE_MAP.md section 7a records why that is
// correct for this game: in the Xenia oracle the HAL is DORMANT (no live object carries the XMA
// source-voice vtable 0x820BC978 in either sound golden dump while the PCM voice 0x820BCA90 does;
// XMA contexts 256-319, the ones a HAL voice would take after EA's 256, are never initialised).
//
// This file is the witness that keeps that verdict honest on OUR runs. It wraps the only
// constructor of the XMA source voice (reached from AudioCreateObjectByType_sub_8211B070 for
// voice type 4) and the HAL's context-creation entry, calls the originals unchanged, and logs.
// If [XMA_HAL_LIVE] ever appears, the HAL is live in this game and its three functions must be
// replaced at function level (NATIVE rule: never by emulating the registers).
//
// Pure observe: no guest state is read beyond the arguments, nothing is written.
// SR_XMA_HAL_OBSERVE: log lines (default = SR_XMA_NEW, i.e. OFF with the default exe; =0 / =1
// override). The counters always run.
#include "guest/ppc/ppc_recomp_shared.h"
#include "kernel/guest_context.hpp"
#include "kernel/apu/xma_device.hpp"
#include "app/log.hpp"

#include <atomic>
#include <cstdlib>

extern "C" void __imp__sub_821236E0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82119980(PPCContext& __restrict ctx, uint8_t* base);

namespace {

std::atomic<uint32_t> g_voiceCtors{0};
std::atomic<uint32_t> g_createContexts{0};

bool halObserveLog() {
  static const bool v = [] {
    const char* e = std::getenv("SR_XMA_HAL_OBSERVE");
    if (e && e[0]) return e[0] != '0';
    return sr::kernel::apu::xmaNewEnabled();
  }();
  return v;
}

bool firstOrPow2(uint32_t n) { return n <= 4 || (n & (n - 1)) == 0; }

}  // namespace

// XmaSourceVoiceCtor_sub_821236E0(this, desc, allocator): the XMA source voice constructor.
PPC_FUNC(sub_821236E0) {
  const uint32_t self = ctx.r3.u32, desc = ctx.r4.u32, lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_821236E0(ctx, base);
  const uint32_t n = g_voiceCtors.fetch_add(1, std::memory_order_relaxed) + 1;
  if (halObserveLog() && firstOrPow2(n))
    SR_LOG_AUDIO("[XMA_HAL_LIVE] #%u XAudio XMA source voice constructed this=%08X desc=%08X "
                 "lr=%08X: the XDK XMA HAL is LIVE (its MMIO kicks reach no device)",
                 n, self, desc, lr);
}

// Xma_CreateContexts_sub_82119980(block): XMACreateContext per slot + the Clear register store.
PPC_FUNC(sub_82119980) {
  const uint32_t block = ctx.r3.u32, lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_82119980(ctx, base);
  const uint32_t n = g_createContexts.fetch_add(1, std::memory_order_relaxed) + 1;
  if (halObserveLog() && firstOrPow2(n))
    SR_LOG_AUDIO("[XMA_HAL_LIVE] #%u Xma_CreateContexts block=%08X lr=%08X rc=%08X", n, block, lr,
                 ctx.r3.u32);
}

namespace sr::kernel {
// iter-467 N6: the [AUDIO_NATIVE] census (kernel/audio_native_census.cpp) reads these.
uint32_t xmaHalCreateContextsCalls() { return g_createContexts.load(std::memory_order_relaxed); }
uint32_t xmaHalVoiceCtors() { return g_voiceCtors.load(std::memory_order_relaxed); }
}  // namespace sr::kernel
