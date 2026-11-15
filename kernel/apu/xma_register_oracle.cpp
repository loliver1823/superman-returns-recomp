// Test-only XMA register oracle, separated from the application's native decoder service.
// Ported register dispatch from Xenia Canary (BSD-3-Clause; docs/THIRD_PARTY.md):
// src/xenia/apu/xma_decoder.cc and kernel/xboxkrnl/xboxkrnl_audio_xma.cc.
// Copyright 2022 Ben Vanik / Xenia Canary. Released under the BSD license.
#if !defined(SR_DIFFTEST)
#error "The XMA register oracle must never be compiled into the application"
#endif

#include "kernel/apu/xma_device.hpp"
#include "kernel/mm_phys.hpp"
#include "app/log.hpp"

#include <cstdlib>

namespace sr::kernel::apu {
namespace {
std::atomic<uint32_t> g_badCtxLogs{0};
bool logN(std::atomic<uint32_t>& c, uint32_t first) {
  const uint32_t n = c.fetch_add(1, std::memory_order_relaxed) + 1;
  return n <= first || (n & (n - 1)) == 0;
}
}  // namespace

std::atomic<int> g_nativeFrontMode{-1};
bool xmaNativeFrontEnabled() {
  const int t = g_nativeFrontMode.load(std::memory_order_relaxed);
  if (t >= 0) return t != 0;
  static const bool v = [] {
    const char* e = std::getenv("SR_XMA_NATIVE_FRONT");
    return !e || e[0] != '0';
  }();   // Test-only native/register A/B; native is the default.
  return v;
}
void xmaNativeFrontSetForTest(int mode) { g_nativeFrontMode.store(mode, std::memory_order_relaxed); }

// xk:78-90 StoreXmaContextIndexedRegister
void XmaDevice::storeIndexedRegister(uint32_t baseRegByteOffset, uint32_t ctxVA) {
  const uint32_t pa = xeGetPhysicalAddress(ctxVA);
  if (pa == UINT32_MAX || pa < arrayPA_ || contextId(ctxVA) < 0) {
    badCtx_.fetch_add(1, std::memory_order_relaxed);
    if (logN(g_badCtxLogs, 8))
      SR_LOG_AUDIO("[XMA] register %04X for ctx %08X (pa %08X): not in the context array", baseRegByteOffset,
                   ctxVA, pa);
    return;
  }
  const uint32_t hw_index = (pa - arrayPA_) / 64;
  const uint32_t reg_num = baseRegByteOffset + (hw_index >> 5) * 4;
  const uint32_t reg_value = 1u << (hw_index & 0x1F);
  writeRegister(reg_num / 4, reg_value);
}

// xd:322-404 WriteRegister (kick / lock / clear groups only; critique B2 bounds). Since iter-467
// the group bodies are kickGroup / lockGroup / clearGroup, shared with the native front end.
void XmaDevice::writeRegister(uint32_t r, uint32_t value) {
  regfileWrites_.fetch_add(1, std::memory_order_relaxed);
  if (r >= kRegContext0Kick && r <= kRegContext0Kick + 9) {
    // Context kick command (XMAEnableContext). The context ID is a bit in the range of the
    // entire context array.
    kickGroup((r - kRegContext0Kick) * 32, value);
  } else if (r >= kRegContext0Lock && r <= kRegContext0Lock + 9) {
    // Context lock command (XMADisableContext).
    lockGroup((r - kRegContext0Lock) * 32, value);
  } else if (r >= kRegContext0Clear && r <= kRegContext0Clear + 9) {
    // Context clear command.
    clearGroup((r - kRegContext0Clear) * 32, value);
  }
  // 0x601 and every other register: ignored (no register file; see header).
}

}  // namespace sr::kernel::apu
