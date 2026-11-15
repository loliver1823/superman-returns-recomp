#include "guest/ppc/ppc_config.h"
#include "guest/ppc/ppc_context.h"
#include "native_display.hpp"
#include "guest_call.hpp"
#include "guest_context.hpp"
#include "guest_protect.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <windows.h>

// Contract pins, read before implementation (HEAD 74ef828f8114e37a280aa6844a30fd51a62ff4ff):
// IDA default.xex SHA256 c8f243acd99de9a91f5ae4f409721c0e954e3d5eb96861419d3da07b8106db2b.
// IDA 820FDC48/4C loads frame then countdown; 58 stores frame+1; 64/68
// decrements positive countdown; 70/74 clears scratch+4; 78/7C reloads frame
// into last-release; 80 captures callback; 8C..A8 constructs/invokes its block.
// Generated ppc_recomp.1.cpp source0 body SHA256:
// 56838a208f40020860770d02c978823d146574fdbe09d266d7db4c7ad6328d8b.
// That localized body has no save/restore-helper writes or LR/CTR publication;
// it stores the backchain and uses a 144-byte frame, args at frame+96.
// No clock-phase, pacing callback, source-1 or MMIO behavior is implemented here.
#if !defined(SR_PPC_LOCALIZED) || !SR_PPC_LOCALIZED
#error Native source-0 requires the audited localized guest calling convention
#endif

namespace sr::native::display {
namespace {
constexpr uint32_t kFrameBytes = 0x90u;
constexpr uint32_t kInputChanged = 0xE053D002u;

// Every OS permission query this file makes (test::osQueries).
std::atomic<uint64_t> g_osQueries{0};
SIZE_T query(const void* at, MEMORY_BASIC_INFORMATION* region) {
  g_osQueries.fetch_add(1, std::memory_order_relaxed);
  return VirtualQuery(at, region, sizeof(*region));
}

// Host memory only: the swap callback's dispatch slot (callback() below).
bool readable(const void* pointer, size_t bytes) {
  const uintptr_t begin = reinterpret_cast<uintptr_t>(pointer);
  if (!begin || !bytes || bytes > UINTPTR_MAX - begin) return false;
  const uintptr_t end = begin + bytes;
  for (uintptr_t at = begin; at < end;) {
    MEMORY_BASIC_INFORMATION region{};
    if (!query(reinterpret_cast<void*>(at), &region) ||
        region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    switch (region.Protect & 0xFFu) {
      case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
      case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: break;
      default: return false;
    }
    const uintptr_t next = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    if (next <= at) return false;
    at = next;
  }
  return true;
}

// A guest range by arithmetic, no OS query (kernel/guest_protect.hpp; the transport pair's admit()): non-empty, no
// wrap, mapped contiguously into the registered guest arena - one committed read-write allocation in both memory
// models, nothing decommits inside it - and touching no range a foreign protector declared. What can still protect an
// admitted page is the GPU write tracker (PAGE_READONLY on the 0x8 / 0xA windows): this file's stores are plain host
// stores, so the first one into such a page faults into the tracker's handler exactly as the guest ISR's store does,
// and the handler releases the block before the store retries.
uint8_t* arenaRange(uint32_t va, uint32_t bytes) {
  if (!bytes || uint64_t(va) + bytes > (uint64_t(1) << 32u)) return nullptr;
  uint8_t* first = sr::kernel::guestVAToHost(va);
  uint8_t* last = sr::kernel::guestVAToHost(va + bytes - 1u);
  if (!first || !last || uintptr_t(last) != uintptr_t(first) + bytes - 1u) return nullptr;
  uint8_t* arena = nullptr;
  size_t arenaSize = 0;
  sr::kernel::getGlobalGuestMemory(&arena, &arenaSize);
  if (!arena || first < arena || size_t(last - arena) >= arenaSize) return nullptr;
  if (sr::kernel::guest_protect::intersects(first, bytes)) return nullptr;
  return first;
}

uint8_t* range(uint32_t va, uint32_t bytes, bool write) {
  (void)write;  // the arena is read-write; the tracker's read-only pages fault into its handler (arenaRange)
  uint8_t* first = arenaRange(va, bytes);
  return first;
}

bool overlap(const void* a, size_t an, const void* b, size_t bn) {
  const uintptr_t aa = uintptr_t(a), bb = uintptr_t(b);
  return aa < bb + bn && bb < aa + an;
}

uint32_t loadBE32(const uint8_t* pointer) {
  return __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(pointer));
}
void storeBE32(uint8_t* pointer, uint32_t value) {
  *reinterpret_cast<volatile uint32_t*>(pointer) = __builtin_bswap32(value);
}

// Admission predicts the few writes before callback capture. Comparing canonical
// host addresses preserves guest-VA aliasing, including scratch clearing a device
// field. This preview never mutates the guest or substitutes for later rereads.
struct Preview {
  struct Write { uint8_t* pointer; uint32_t value; };
  std::array<Write, 4> writes{};
  size_t count = 0;
  void add(uint8_t* pointer, uint32_t value) { writes[count++] = {pointer, value}; }
  uint32_t read(uint8_t* pointer) const {
    for (size_t i = count; i; --i)
      if (writes[i - 1u].pointer == pointer) return writes[i - 1u].value;
    return loadBE32(pointer);
  }
};

PPCFunc* callback(uint32_t va) {
  if ((va & 3u) || va < PPC_CODE_BASE || uint64_t(va) >= PPC_CODE_BASE + PPC_CODE_SIZE ||
      !sr::kernel::g_dispatchTable) return nullptr;
  PPCFunc** slot = sr::kernel::g_dispatchTable + ((va - PPC_CODE_BASE) >> 2u);
  if (!readable(slot, sizeof(*slot))) return nullptr;
  PPCFunc* fn = *slot;
  if (!fn) return nullptr;
  MEMORY_BASIC_INFORMATION region{};
  if (!query(reinterpret_cast<const void*>(fn), &region) ||
      region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return nullptr;
  switch (region.Protect & 0xFFu) {
    case PAGE_EXECUTE: case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: return fn;
    default: return nullptr;
  }
}
}  // namespace

Source0Result trySource0(PPCContext& ctx, uint8_t* base,
                         uint32_t registeredCallbackVA, uint32_t registeredContextVA,
                         uint32_t* exceptionOut) {
  const uint32_t dev = ctx.r4.u32, stack = ctx.r1.u32;
  if (registeredCallbackVA != kD3DIsr || !dev || dev != registeredContextVA ||
      ctx.r3.u32 != 0u || !base || base != sr::kernel::getGuestBase() ||
      (dev & 3u) || stack < kFrameBytes || (stack & 15u) ||
      dev > UINT32_MAX - 0x3B23u || (ctx.r13.u32 & 3u) ||
      ctx.r13.u32 > UINT32_MAX - 0x153u) return Source0Result::Declined;
  // Arithmetic admission's preconditions (kernel/guest_protect.hpp): no page-protecting diagnostic, no all-excluding
  // declaration, and the arena held for every guest access below (the callback included) - refused once retired.
  namespace gp = sr::kernel::guest_protect;
  if (gp::diagnosticRequested() || gp::excludesAll()) return Source0Result::Declined;
  const gp::ArenaLease arenaLease;
  if (!arenaLease) return Source0Result::Declined;

  uint8_t* state = range(dev + 0x3B10u, 20u, true);
  uint8_t* frame = range(stack - kFrameBytes, kFrameBytes, true);
  uint8_t* pcr = range(ctx.r13.u32, 0x154u, false);
  if (!ctx.r13.u32 || !state || !frame || !pcr ||
      pcr[sr::kernel::kKpcrOffCurrentCpu] != 2u || pcr[sr::kernel::kKpcrOffIrql] != 2u ||
      loadBE32(pcr + sr::kernel::kKpcrOffDpcActive) != 1u ||
      overlap(frame, kFrameBytes, state, 20u) || overlap(frame, kFrameBytes, pcr, 0x154u) ||
      overlap(state, 20u, pcr, 0x154u) ||
      (exceptionOut && (overlap(exceptionOut, sizeof(*exceptionOut), frame, kFrameBytes) ||
                       overlap(exceptionOut, sizeof(*exceptionOut), state, 20u)))) return Source0Result::Declined;

  // Initial loads retain SDK order. Stack/device overlap is excluded above, so
  // performing fault-free admission before the backchain store cannot change them.
  const uint32_t oldFrame = loadBE32(state + 4u);
  const uint32_t countdown = loadBE32(state + 12u);
  Preview preview;
  preview.add(state + 4u, oldFrame + 1u);
  uint8_t* gate = nullptr;
  if (int32_t(countdown) > 0) {
    preview.add(state + 12u, countdown - 1u);
    if (countdown == 1u) {
      uint8_t* scratchCell = range(dev + 0x2A14u, 4u, false);
      if (!scratchCell || overlap(frame, kFrameBytes, scratchCell, 4u)) return Source0Result::Declined;
      const uint32_t scratch = loadBE32(scratchCell);
      if (!scratch || (scratch & 3u) || scratch > UINT32_MAX - 7u) return Source0Result::Declined;
      gate = range(scratch + 4u, 4u, true);
      if (!gate || overlap(frame, kFrameBytes, gate, 4u) || overlap(pcr, 0x154u, gate, 4u) ||
          (exceptionOut && overlap(exceptionOut, sizeof(*exceptionOut), gate, 4u))) return Source0Result::Declined;
      preview.add(gate, 0u);
      preview.add(state + 8u, preview.read(state + 4u));
    }
  }
  const uint32_t callbackVA = preview.read(state);
  PPCFunc* const fn = callbackVA ? callback(callbackVA) : nullptr;
  if (callbackVA && !fn) return Source0Result::Declined;

  // All admission is complete. No branch below returns Declined.
  const uint64_t incomingStack = ctx.r1.u64;
  storeBE32(frame, stack);
  ctx.r1.u32 = stack - kFrameBytes;
  ctx.r10.u64 = oldFrame;
  ctx.r10.s64 += 1;
  storeBE32(state + 4u, ctx.r10.u32);
  ctx.r10.s64 = 0;
  if (int32_t(countdown) > 0) {
    storeBE32(state + 12u, countdown - 1u);
    if (countdown == 1u) {
      storeBE32(gate, 0u);
      storeBE32(state + 8u, loadBE32(state + 4u));
    }
  }
  // The callback is captured after the stores, as the SDK does. A concurrent
  // publication violating the admission lifetime is terminal, never replayed.
  if (loadBE32(state) != callbackVA) {
    ctx.r1.u64 = incomingStack;
    if (exceptionOut) *exceptionOut = kInputChanged;
    return Source0Result::CallbackFault;
  }
  uint32_t exception = 0;
  bool completed = true;
  if (fn) {
    ctx.r9.u64 = loadBE32(state + 16u);
    ctx.r3.s64 = ctx.r1.s64 + 96;
    storeBE32(frame + 104u, ctx.r10.u32);
    storeBE32(frame + 100u, ctx.r9.u32);
    ctx.r9.u64 = loadBE32(state + 4u);
    storeBE32(frame + 96u, ctx.r9.u32);
    completed = sr::kernel::detail::sehCall(fn, ctx, base, &exception);
  }
  // A callback must obey the ordinary guest stack ABI. On a trapped callback
  // restore the caller frame for diagnostics; its committed effects remain.
  if (completed) ctx.r1.s64 += kFrameBytes;
  else ctx.r1.u64 = incomingStack;
  if (exceptionOut) *exceptionOut = exception;
  return completed ? Source0Result::Completed : Source0Result::CallbackFault;
}

namespace test {
uint64_t osQueries() { return g_osQueries.load(std::memory_order_relaxed); }
}  // namespace test

}  // namespace sr::native::display
