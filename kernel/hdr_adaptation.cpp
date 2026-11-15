#include "hdr_adaptation.hpp"

#include "guest_context.hpp"
#include "highfps.hpp"
#include "../gfx/shader_cache.hpp"
#include "../app/log.hpp"
#include "guest/ppc/ppc_recomp_shared.h"

#include <windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace sr::kernel::hdr {
namespace {
thread_local TickSnapshot tick;
thread_local EffectSnapshot effect;
std::atomic<uint64_t> corrected{0}, invalidTick{0};
// G3 (step 6): how the metadata reads were served (readStats)
std::atomic<uint64_t> directReads{0}, osReads{0}, unreadableReads{0}, directFaults{0};
std::atomic<int> directForTest{-1};   // setDirectReadsForTest: -1 = the env gate, 0 = the OS copy, 1 = direct

// G3 (step 6, lane C, 2026-10-24): SR_HDR_DIRECT_READ, default ON - the metadata reads below copy guest memory directly
// once the fault-free predicate has proved the range readable; =0 keeps the OS copy (ReadProcessMemory), the A/B.
bool directReadsOn() noexcept {
  const int forced = directForTest.load(std::memory_order_relaxed);
  if (forced >= 0) return forced != 0;
  static const bool on = [] {
    const char* value = std::getenv("SR_HDR_DIRECT_READ");
    const bool direct = !value || value[0] != '0';
    SR_LOG_BOOT(sr::LogSeverity::Info, "[HDR_ADAPT_DT] metadata reads: %s (SR_HDR_DIRECT_READ)",
                direct ? "direct copies of proved-readable ranges" : "OS copies (ReadProcessMemory)");
    return direct;
  }();
  return on;
}

bool hdrAdaptationOn() noexcept {
  // X2 BAKE (step 4, lane D, 2026-10-18; docs/HIGH_FRAMERATE.md "Eye-adaptation rate"): default ON. With a frame-rate
  // feature on, c0.x = authored x 30 x render dt, so the history step keeps the console's 30-evaluations-per-second
  // response at any render rate (measured on frozen scenes, set x2set5); at the game's own 30 Hz nothing changes
  // (RenderTickScope is a no-op with HFR off). =0 keeps the A/B.
  static const bool on = [] {
    const char* value = std::getenv("SR_HDR_ADAPT_DT");
    return !value || value[0] != '0';
  }();
  return on;
}

// The freeze oracle's eye-adaptation hold (2026-10-13, DIAGNOSTIC: only with
// SR_DIAG_SIM_FREEZE, never in a player's run). The adaptation steps the exposure
// history once per RENDERED frame (out = hist + (cur - hist) * (1 - exp2(-0.87 * c0.x)),
// ENGINE_MAP 6g.8d), so it keeps converging while the sim is held and frozen presents
// drift by 1-4 levels over the whole frame - more at lower frame rates (the 74ef828f
// baseline at 40 fps fails the freeze the same way). Like the shader time 0x29, the
// oracle holds it: c0.x = 0 (no step, out = hist) while the hold lasts.
// SR_DIAG_SIM_FREEZE_ADAPT=0 lets it run (the A/B).
bool freezeAdaptHoldOn() noexcept {
  static const bool on = [] {
    // Off unless the freeze oracle itself is on (SR_DIAG_SIM_FREEZE, default OFF).
    const char* freeze = std::getenv("SR_DIAG_SIM_FREEZE");
    if (!freeze || !*freeze || freeze[0] == '0' || !highfps::simFreezeOn()) return false;
    const char* value = std::getenv("SR_DIAG_SIM_FREEZE_ADAPT");
    const bool hold = !value || value[0] != '0';
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SIM_FREEZE] eye adaptation %s during holds (SR_DIAG_SIM_FREEZE_ADAPT)",
                hold ? "HELD" : "running");
    return hold;
  }();
  return on;
}
std::atomic<uint64_t> held{0};

uint32_t be32(const uint8_t* p) noexcept {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | p[3];
}

// Normalize the complete range before comparing aliases. Reservation bounds
// alone do not establish commitment: read() below proves the range readable before
// it copies (or, with SR_HDR_DIRECT_READ=0, lets the OS copy fail).
uint8_t* range(uint8_t* base, uint32_t va, uint32_t bytes) noexcept {
  if (!base || base != getGuestBase() || !va || !bytes ||
      uint64_t(va) + bytes > 0x100000000ull) return nullptr;
  const uint32_t first = aliasGuestVA(va), last = aliasGuestVA(va + bytes - 1);
  if (last < first || uint64_t(last) - first != bytes - 1 ||
      uint64_t(first) + bytes > getGuestSize()) return nullptr;
  return base + first;
}

// G3: the copy. Default: hostRangeReadableCached (kernel/guest_context.hpp: THE fault-free predicate - every page of
// the range committed, readable and not a guard page - answered from its interval cache, which every site that makes
// guest memory unreadable invalidates by the removal protocol, tools/test_hrr_invalidation_surface.py's inventory) and
// then a plain copy under SEH (guardedCopy). The OS copy decides every case the direct copy does not take: a range the
// predicate REFUSES (it is stricter than the OS copy - e.g. PAGE_EXECUTE, which ReadProcessMemory reads) and a direct
// copy that FAULTED (a stale cache answer). So the result is always exactly the OS copy's - the same bytes and the
// same refusals - and the syscall is paid only off the common path. The window between the predicate's answer and the
// copy (a removal racing a read: the default teardown's bracketed arena release, bracketed diagnostics; a guest free
// zeroes, it never decommits, kernel/stubs.cpp NtFreeVirtualMemory) is the fault the SEH fallback absorbs.
// =0: the OS copy, one NtReadVirtualMemory syscall per read - 0.53 ms per present on the draw thread in N's profile.
// Belt and braces (the coordinator, 10-24): the copy itself runs under SEH. A plain function with no C++ object to
// unwind; the copy is a CALL through a volatile pointer, so an access violation raised inside it is caught here
// without -fasync-exceptions. The harness's vectored handlers pass a read fault outside their own pages through to
// SEH (image_write_watchdog: writes only; gpu_write_watch: its write traps; the mirror VEH: the mirror range;
// resource_census: its diagnostic traps). Only an access violation / in-page error is caught; anything else searches on.
using CopyFn = void* (*)(void*, const void*, size_t);
CopyFn volatile copyFn = &::memcpy;
bool guardedCopy(void* out, const void* p, size_t bytes) noexcept {
  __try {
    copyFn(out, p, bytes);
    return true;
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    return false;
  }
}

bool read(uint8_t* base, uint32_t va, void* out, uint32_t bytes) noexcept {
  const uint8_t* p = range(base, va, bytes);
  if (!p) return false;
  if (directReadsOn()) {
    if (!hostRangeReadableCached(p, bytes)) {
      // Refused by the stricter predicate: the OS copy decides (Astra G3 P2: PAGE_EXECUTE is readable to it).
      unreadableReads.fetch_add(1, std::memory_order_relaxed);
      osReads.fetch_add(1, std::memory_order_relaxed);
      SIZE_T copied = 0;
      return ReadProcessMemory(GetCurrentProcess(), p, out, bytes, &copied) && copied == bytes;
    }
    if (guardedCopy(out, p, bytes)) {
      directReads.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    // The predicate said readable and the copy faulted: a stale cache answer (a removal that skipped the protocol).
    // Counted, one log line, and this read takes the OS copy below - it then fails exactly where it always did.
    const uint64_t f = directFaults.fetch_add(1, std::memory_order_relaxed) + 1;
    if (f == 1)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[HDR_ADAPT_DT] a direct metadata read faulted on a range the readable cache "
                  "still held (a removal outside the protocol?) - counted, that read took the OS copy (SR_HDR_DIRECT_READ)");
  }
  osReads.fetch_add(1, std::memory_order_relaxed);
  SIZE_T copied = 0;
  return ReadProcessMemory(GetCurrentProcess(), p, out, bytes, &copied) && copied == bytes;
}

bool word(uint8_t* base, uint32_t va, uint32_t& result) noexcept {
  std::array<uint8_t, 4> bytes;
  if (!read(base, va, bytes.data(), uint32_t(bytes.size()))) return false;
  result = be32(bytes.data());
  return true;
}

bool adaptationShader(uint8_t* base, uint32_t device, uint32_t& object) noexcept {
  uint32_t physical;
  if (device > UINT32_MAX - 0x3080u || !word(base, device + 0x3080u, object) ||
      !object || object > UINT32_MAX - 0x28u || !word(base, object + 0x18u, physical) || !physical)
    return false;
  std::array<uint8_t, 0x24> header;
  if (!read(base, object + 0x28u, header.data(), uint32_t(header.size()))) return false;
  const uint32_t flags = be32(header.data()), virt = be32(header.data() + 4);
  const uint32_t phys = be32(header.data() + 8), offset = be32(header.data() + 0x18);
  if ((flags & 0xffffff00u) != 0x102a1100u || (flags & 1u) ||
      virt < header.size() || virt > 0x10000u || !phys || phys > 0x100000u ||
      offset < header.size() || uint64_t(offset) + 8 > virt ||
      !range(base, object + 0x28u, virt) || !range(base, physical, phys)) return false;
  std::array<uint8_t, 8> shader;
  if (!read(base, object + 0x28u + offset, shader.data(), uint32_t(shader.size()))) return false;
  const uint32_t physOffset = be32(shader.data()), size = be32(shader.data() + 4);
  // Exact identity, including on pooled object/physical-buffer reuse. No pointer cache.
  std::array<uint8_t, 120> code;
  return size == code.size() && uint64_t(physOffset) + size <= phys &&
         read(base, physical + physOffset, code.data(), size) &&
         sr::gfx::ShaderCache::hashUcode(code.data(), code.size()) == 0x040D6633B0AA5D6Dull;
}

bool compiledSource(uint8_t* base, uint32_t device, uint32_t source,
                    uint32_t count, uint32_t shaderObject) noexcept {
  if (effect.base != base || !effect.effect || !effect.block ||
      effect.effect > UINT32_MAX - 8u) return false;
  std::array<uint8_t, 104> block;
  uint32_t effectDevice;
  if (!read(base, effect.block, block.data(), uint32_t(block.size())) ||
      !word(base, effect.effect + 8u, effectDevice) || effectDevice != device) return false;
  const uint32_t flags = be32(block.data() + 12), recordBase = be32(block.data() + 8);
  const uint32_t dataBase = be32(block.data());
  const uint32_t registers = (uint32_t(block[88]) << 8) | block[89];
  if ((flags & 0x20000000u) || !dataBase || !recordBase || registers != count ||
      uint64_t(dataBase) + be32(block.data() + 72) != source) return false;
  std::array<uint8_t, 32> record;
  uint32_t shader;
  if (!read(base, recordBase, record.data(), uint32_t(record.size()))) return false;
  const uint32_t reference = be32(record.data() + 26);
  if (!reference || reference > UINT32_MAX - 4u ||
      !word(base, reference + 4u, shader) || shader != shaderObject) return false;
  // The compiled c0 record must target this packed source. Its dirty comparison
  // can skip the marshal; the setter still uploads the unchanged authored buffer.
  const uint32_t begin = (flags & 0x10000000u) ? 2u : 1u;
  const uint32_t end = (uint32_t(block[86]) << 8) | block[87];
  if (end <= begin || end > 32u || !range(base, recordBase, end * 32u)) return false;
  for (uint32_t n = begin; n < end; ++n) {
    if (!read(base, recordBase + n * 32u, record.data(), uint32_t(record.size()))) return false;
    if (be32(record.data()) == source) return true;
  }
  return false;
}
}  // namespace

ReadStats readStats() noexcept {
  ReadStats s;
  s.direct = directReads.load(std::memory_order_relaxed);
  s.os = osReads.load(std::memory_order_relaxed);
  s.unreadable = unreadableReads.load(std::memory_order_relaxed);
  s.faults = directFaults.load(std::memory_order_relaxed);
  return s;
}
void setDirectReadsForTest(int mode) noexcept { directForTest.store(mode, std::memory_order_relaxed); }

bool scaledTimeScale(float authored, double seconds, float& result) noexcept {
  if (!std::isfinite(authored) || authored < 0 || !std::isfinite(seconds) || seconds < 0)
    return false;
  const double value = double(authored) * 30.0 * seconds;
  if (!std::isfinite(value) || value > std::numeric_limits<float>::max()) return false;
  result = float(value);
  return true;
}

RenderTickScope::RenderTickScope(uint8_t* base) noexcept : previous_(tick) {
  tick = {};
  if (!hdrAdaptationOn() || highfps::liveConfig().mode == highfps::Mode::Off) return;
  uint32_t manager;
  std::array<uint8_t, 16> data;
  if (!word(base, 0x8296D8DCu, manager) || !manager || manager > UINT32_MAX - 0x88u ||
      !read(base, manager + 0x78u, data.data(), uint32_t(data.size()))) return;
  const uint32_t dtBits = be32(data.data() + 12);
  float seconds;
  std::memcpy(&seconds, &dtBits, sizeof seconds);
  if (!std::isfinite(seconds) || seconds < 0) {
    const uint64_t n = invalidTick.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4) SR_LOG_BOOT(sr::LogSeverity::Warn, "[HDR_ADAPT_DT] invalid render interval; authored value retained");
    return;
  }
  tick = {base, manager, be32(data.data() + 8), seconds, true};
}

RenderTickScope::~RenderTickScope() { tick = previous_; }

CompiledEffectScope::CompiledEffectScope(uint8_t* base, uint32_t object, uint32_t block) noexcept
    : previous_(effect) { effect = {base, object, block}; }
CompiledEffectScope::~CompiledEffectScope() { effect = previous_; }

void afterPsConstantUpload(uint8_t* base, uint32_t device, uint32_t start,
                           uint32_t source, uint32_t count) noexcept {
  const bool holding = freezeAdaptHoldOn() && highfps::freezeHeldMs() > 0;
  const bool scaling = !holding && hdrAdaptationOn() && tick.enabled && tick.base == base;
  if ((!holding && !scaling) || start != 0 || !count || count > 512u ||
      !device || device > UINT32_MAX - 0x1700u || base != getGuestBase()) return;
  const uint32_t bytes = count * 16u;
  uint8_t* destination = range(base, device + 0x1700u, bytes);
  const uint8_t* input = range(base, source, bytes);
  if (!destination || !input) return;
  const uintptr_t d = reinterpret_cast<uintptr_t>(destination), s = reinterpret_cast<uintptr_t>(input);
  uint32_t shaderObject;
  if (!(s + bytes <= d || d + bytes <= s) || !adaptationShader(base, device, shaderObject) ||
      !compiledSource(base, device, source, count, shaderObject)) return;
  std::array<uint8_t, 4> authoredBytes;
  if (!read(base, device + 0x1700u, authoredBytes.data(), uint32_t(authoredBytes.size()))) return;
  const uint32_t authoredBits = be32(authoredBytes.data());
  float authored, adjusted = 0.0f;
  std::memcpy(&authored, &authoredBits, sizeof authored);
  if (scaling && !scaledTimeScale(authored, tick.seconds, adjusted)) return;
  uint32_t adjustedBits;
  std::memcpy(&adjustedBits, &adjusted, sizeof adjusted);
  // The successful setter owns this device storage on the render thread. Only x
  // changes, after both its normal copy and any differential check have finished.
  destination[0] = uint8_t(adjustedBits >> 24); destination[1] = uint8_t(adjustedBits >> 16);
  destination[2] = uint8_t(adjustedBits >> 8); destination[3] = uint8_t(adjustedBits);
  if (holding) {
    const uint64_t h = held.fetch_add(1, std::memory_order_relaxed) + 1;
    if (h <= 4 || (h & 1023u) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[SIM_FREEZE] eye adaptation held #%llu (authored %.9g -> 0)",
                  (unsigned long long)h, double(authored));
    return;
  }
  const uint64_t n = corrected.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 8 || (n & 1023u) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[HDR_ADAPT_DT] #%llu tick=%u dt=%.9f authored=%.9g adjusted=%.9g",
                (unsigned long long)n, tick.ordinal, tick.seconds, double(authored), double(adjusted));
}
}  // namespace sr::kernel::hdr

extern "C" void __imp__sub_823EAE38(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_823EAE38) {
  sr::kernel::hdr::CompiledEffectScope scope(base, ctx.r3.u32, ctx.r4.u32);
  __imp__sub_823EAE38(ctx, base);
}
