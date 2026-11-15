// The 16 legacy XMA* fakes: "playback complete" answers to the title's XMA polling (the output-buffer read and write
// offsets 0 = equal, XMAIsOutputBufferValid 0; XMACreateContext hands out a real zeroed guest allocation per context so the
// game can dereference it). They are the SR_XMA_NEW=0 arm of the audio A/B: the native XMA decoder (kernel/apu/xma_device.cpp,
// default ON) replaces every one of them at registration when its gate is on.
//
// C1 (docs/RELEASE_COMPOSITION.md 2.6; Astra round 1 on release/emu-removal stage 0): a disabled fallback component with a
// unit of its own - CMakeLists.txt EMULATION_SOURCES, out of the release configuration, where kernel/apu/xma_api.hpp's
// xmaNewEnabled() is the constant true and kernel/stubs.cpp's call of registerXmaLegacyExports is compiled out. The
// composition audit fails this unit when a release build links it, and its [XMA_LEGACY] tag (a marker of its manifest
// rule) wherever the fakes are compiled in. Moved out of kernel/stubs.cpp with the fakes' answers unchanged; the generic
// fakes log under [XMA_LEGACY] (capped: the first three calls and every power of two) instead of stubs.cpp's
// SR_STUB_CENSUS census, and one line says when the fakes are the registered exports (the gate off).
#include "kernel/apu/xma_api.hpp"
#include "kernel/export_registry.hpp"
#include "kernel/guest_context.hpp"
#include "app/log.hpp"

#include <atomic>
#include <cstdint>

namespace sr::kernel::apu {
namespace {

constexpr uint32_t kStatusSuccess = 0;
constexpr uint32_t kXMAContextSize = 256u;  // each XMA context is ~256 bytes

// A big-endian store, as every guest write of a kernel stub (kernel/stubs.cpp writeGuest32).
void writeGuest32(uint32_t guestPtr, uint32_t value) {
  uint8_t* host = sr::kernel::guestVAToHost(guestPtr);
  if (host) {
    host[0] = (value >> 24) & 0xFF;
    host[1] = (value >> 16) & 0xFF;
    host[2] = (value >> 8) & 0xFF;
    host[3] = value & 0xFF;
  }
}

// XMACreateContext: r3 = output pointer. All-zero fields signal "idle / complete" to the game's polling code.
uint32_t stub_XMACreateContext(uint32_t r3, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
  if (r3 != 0) {
    uint32_t ctxVA = sr::kernel::guestHeapAlloc(kXMAContextSize);
    if (ctxVA == 0) ctxVA = sr::kernel::guestHeapAllocFallback(kXMAContextSize);
    writeGuest32(r3, ctxVA);
  }
  return kStatusSuccess;
}

// The offset getters answer 0 so read == write (all data consumed); the valid getters answer 0 (no input / output), so
// the polling sees "playback complete".
uint32_t stub_XMAZero(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }

// A generic fake: success, with a capped log line (the first three calls and every power of two).
#define XMA_LEGACY_GENERIC(nm)                                                                             \
  reg.registerExport(nm, +[](uint32_t r3, uint32_t r4, uint32_t r5, uint32_t, uint32_t, uint32_t) -> uint32_t { \
    static std::atomic<uint32_t> s_n{0};                                                                   \
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;                                    \
    if (n <= 3 || (n & (n - 1)) == 0)                                                                      \
      SR_LOG_BOOT(sr::LogSeverity::Info, "[XMA_LEGACY] " nm " #%u lr=%08X r3=%08X r4=%08X r5=%08X", n,     \
                  sr::kernel::getCurrentGuestLR(), r3, r4, r5);                                            \
    return kStatusSuccess;                                                                                 \
  })

}  // namespace

void registerXmaLegacyExports(ExportRegistry& reg) {
  reg.registerExport("XMACreateContext", stub_XMACreateContext);
  XMA_LEGACY_GENERIC("XMADisableContext");
  XMA_LEGACY_GENERIC("XMAEnableContext");
  reg.registerExport("XMAGetOutputBufferReadOffset", stub_XMAZero);
  reg.registerExport("XMAGetOutputBufferWriteOffset", stub_XMAZero);
  XMA_LEGACY_GENERIC("XMAInitializeContext");
  reg.registerExport("XMAIsInputBuffer0Valid", stub_XMAZero);
  reg.registerExport("XMAIsInputBuffer1Valid", stub_XMAZero);
  reg.registerExport("XMAIsOutputBufferValid", stub_XMAZero);
  XMA_LEGACY_GENERIC("XMAReleaseContext");
  XMA_LEGACY_GENERIC("XMASetInputBuffer0");
  XMA_LEGACY_GENERIC("XMASetInputBuffer0Valid");
  XMA_LEGACY_GENERIC("XMASetInputBuffer1");
  XMA_LEGACY_GENERIC("XMASetInputBuffer1Valid");
  XMA_LEGACY_GENERIC("XMASetOutputBufferReadOffset");
  XMA_LEGACY_GENERIC("XMASetOutputBufferValid");
  if (!xmaNewEnabled())
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[XMA_LEGACY] SR_XMA_NEW=0: the 16 legacy XMA fakes answer the title's XMA imports (the native decoder is off)");
}

}  // namespace sr::kernel::apu
