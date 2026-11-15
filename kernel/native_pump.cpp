// Retail 82110460 software-list replay, derived from IDA and regenerated SDK.
// This is CPU library scheduling. GPU commands are published by the existing
// submission boundary; this core does not interpret PM4 or hardware registers.
#include "native_pump.hpp"
#include "native_brackets.hpp"  // SR_NATIVE_SCOPE on the pump driver's override
#include "guest_context.hpp"
#include "native_memory.hpp"
#include "sr_oracle.hpp"
#if SR_ORACLE
#include "gpu_write_watch.hpp"
#endif
#include "guest/ppc/ppc_recomp_shared.h"
#include <cstdlib>
#include <cstring>
#include <windows.h>

namespace sr::native::pump {
namespace {
namespace K = sr::kernel;
thread_local Publication publication;
#if defined(SR_DIFFTEST)
PublicationObserver observer = nullptr;
DriverCallbackObserver driverObserver = nullptr;
#endif
struct PublicationScope {
  Publication previous;
  PublicationScope(uint32_t record, uint32_t state) : previous(publication) {
    publication = {record, state};
  }
  ~PublicationScope() { publication = previous; }
};
uint32_t rd(uint32_t address) { return K::readGuestU32BE(address); }
void writeMarkerWord(uint32_t address, uint32_t value) { K::writeGuestBE32(address, value); }
bool mappedRange(uint32_t address, uint32_t bytes, bool write) {
  if (!address || !bytes || address > UINT32_MAX - (bytes - 1u)) return false;
  const uint8_t* first = K::guestVAToHost(address);
  const uint8_t* last = K::guestVAToHost(address + bytes - 1u);
  if (!first || !last || last - first != ptrdiff_t(bytes - 1u)) return false;
  if (write) {
#if SR_ORACLE
    return sr::native::memory::writableRange(first, bytes) || K::gww::cpuWritableRange(first, bytes);
#else
    return sr::native::memory::writableRange(first, bytes);
#endif
  }
  const uintptr_t begin = reinterpret_cast<uintptr_t>(first), end = begin + bytes;
  for (uintptr_t at = begin; at < end;) {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD protect = region.Protect & 0xFFu;
    if (protect != PAGE_READONLY && protect != PAGE_EXECUTE_READ &&
        !sr::native::memory::writableProtection(protect)) return false;
    const uintptr_t next = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    if (next <= at) return false;
    at = next;
  }
  return true;
}
}

bool markerGateOn() {
  static const bool on = [] {
    const char* value = std::getenv("SR_NATIVE_REPLAY_MARKERS");
    return value && value[0] == '1';
  }();
  return on;
}
bool driverGateOn() {
  static const bool on = [] {
    const char* value = std::getenv("SR_NATIVE_REPLAY_DRIVER");
    return value && value[0] == '1';
  }();
  return on;
}
Publication currentPublication() { return publication; }
#if defined(SR_DIFFTEST)
void setPublicationObserver(PublicationObserver value) { observer = value; }
void setDriverCallbackObserver(DriverCallbackObserver value) { driverObserver = value; }
#endif

bool replayMarkers(PPCContext& ctx, uint8_t* base) {
  if (!markerGateOn()) return false;
  const uint32_t state = ctx.r4.u32, stack = ctx.r1.u32;
  const uint32_t start = ctx.r3.u32;
  // Mapping/protection lifetimes and all reachable list nodes are owned by the
  // caller through this complete call, as in the SDK. Admit initial inputs
  // before any stack, state or publication mutation; never fall back mid-call.
  if (((state | stack | start) & 3u) || stack < 0xA0u ||
      !mappedRange(state, 0x17Cu, true) || !mappedRange(stack - 0xA0u, 0xA0u, true) ||
      !mappedRange(start, 4u, false) || !mappedRange(0x82000948u, 4u, false)) return false;
  const uint32_t deviceCell = rd(0x82000948u);
  if (!mappedRange(deviceCell, 4u, false)) return false;
  const uint32_t first = rd(start), kind = first >> 24;
  const uint32_t initialBytes = kind == 0x80u ? 252u : kind == 0x8Bu ? 12u :
                                (kind == 0x82u || kind == 0x8Cu) ? 8u : 4u;
  if (!mappedRange(start, initialBytes, false) ||
      (kind == 0x89u && !mappedRange(0x7F100000u, 0x20000u, true))) return false;

  PPCContext work = ctx;
  work.r1.u32 = stack - 0xA0u;
  writeMarkerWord(work.r1.u32, stack);
  const uint32_t device = rd(deviceCell);
  uint64_t cursor = ctx.r3.u64;
  auto binSelect = [&] {
    work.r3.u64 = ctx.r4.u64;
    sub_82110140(work, base);
  };
  for (;;) {
    const uint32_t header = rd(uint32_t(cursor));
    if (!(header & 0x80000000u) || (header >> 24) == 0x81u || header == 0xC0000000u) break;
    switch (header >> 24) {
      case 0x80u:
        std::memcpy(K::guestVAToHost(state + 0x74u), K::guestVAToHost(uint32_t(cursor) + 4u), 248u);
        cursor += 252u;
        writeMarkerWord(state + 0x2Cu, 0u); writeMarkerWord(state + 0x16Cu, 0x7FFFFFFFu);
        break;
      case 0x82u: {
        writeMarkerWord(work.r1.u32 + 0x50u, header & 0xFFFFFFu);
        writeMarkerWord(work.r1.u32 + 0x54u, rd(uint32_t(cursor) + 4u));
        work.r3.u64 = device;
        work.r4.u64 = work.r1.u64 + 0x50u;
        work.r5.u64 = 1u;
        PublicationScope scope(uint32_t(cursor), state);
#if defined(SR_DIFFTEST)
        if (observer) observer(currentPublication());
#endif
        sub_820FD218(work, base);
        cursor += 8u;
        break;
      }
      case 0x83u: break;  // SDK spin: re-read this same word until its producer changes it.
      case 0x84u: {
        writeMarkerWord(state + 0x170u, uint32_t(cursor)); cursor += 4u;
        uint32_t flags = rd(state + 0x28u) | 0x40000000u;
        if (rd(state + 0x2Cu) && (rd(state + 0x74u) & 2u)) flags |= 0x80000000u;
        writeMarkerWord(state + 0x28u, flags); binSelect();
        break;
      }
      case 0x85u: {
        cursor += 4u;
        const uint32_t flags = rd(state + 0x28u) & ~0x40000000u;
        writeMarkerWord(state + 0x28u, flags);
        if (flags & 0x80000000u) cursor = rd(state + 0x174u);
        else binSelect();
        break;
      }
      case 0x86u: {
        writeMarkerWord(state + 0x174u, uint32_t(cursor)); cursor += 4u;
        const uint32_t flags = rd(state + 0x28u) ^ 0x80000000u;
        writeMarkerWord(state + 0x28u, flags);
        if (flags & 0x80000000u) cursor = rd(state + 0x170u);
        else binSelect();
        break;
      }
      case 0x87u:
        writeMarkerWord(state + 0x178u, uint32_t(cursor)); cursor += 4u;
        writeMarkerWord(state + 0x28u, rd(state + 0x28u) | 0x20000000u); binSelect();
        break;
      case 0x88u: {
        cursor += 4u;
        const uint32_t tile = rd(state + 0x2Cu) + 1u;
        writeMarkerWord(state + 0x2Cu, tile); writeMarkerWord(state + 0x28u, rd(state + 0x28u) & ~0x20000000u);
        if (tile < rd(state + 0x78u)) cursor = rd(state + 0x178u);
        else binSelect();
        break;
      }
      case 0x89u:
        work.r3.u64 = 0u; work.r4.u64 = 0x7F100000u; work.r5.u64 = 0x20000u;
        work.r6.u64 = 1u; work.r7.u64 = 1u;
        __imp__KeLockL2(work, base);
        std::memset(K::guestVAToHost(0x7F100000u), 0, 0x20000u);
        MemoryBarrier(); cursor += 4u;
        break;
      case 0x8Au:
        work.r3.u64 = ctx.r4.u64; sub_821102B8(work, base);
        work.r3.u64 = 0u; __imp__KeUnlockL2(work, base); cursor += 4u;
        break;
      case 0x8Bu:
        if (rd(state + 0x16Cu) & 0x80000000u) {
          uint32_t list = rd(uint32_t(cursor) + 8u);
          work.r3.u64 = ctx.r4.u64 + 0x74u;
          work.r4.u64 = rd(uint32_t(cursor) + 4u); work.r5.u64 = 0u;
          sub_82110360(work, base);
          do {
            const uint32_t end = rd(list);
            uint32_t at = list + 4u;
            for (; at < end; at += 4u) {
              work.r3.u64 = ctx.r4.u64 + 0x74u;
              work.r4.u64 = rd(at); work.r5.u64 = 1u;
              sub_82110360(work, base);
            }
            list = rd(at);
          } while (list != 0xC0000000u);
        }
        cursor += 12u;
        break;
      case 0x8Cu: {
        const uint32_t mask = rd(uint32_t(cursor) + 4u);
        cursor += 8u;
        if (rd(state + 0x16Cu) & mask) {
          writeMarkerWord(state + 0x50u, uint32_t(cursor)); cursor = ctx.r4.u64 + 0x60u;
        }
        break;
      }
      default: cursor = uint64_t(header) + 4u; break;
    }
  }
  ctx.r3.u64 = cursor;
  return true;
}

// Retail 82110840: preserve the software lock, shared repeat-loop participants,
// marker rendezvous and EndSync decisions. Reachable records/helper inputs and
// their mapping/protection lifetimes are caller-owned through the complete call.
// This conversion does not yet assign or seal immutable list generations.
bool replayDriver(PPCContext& ctx, uint8_t* base) {
  if (!driverGateOn()) return false;
  const uint32_t pumpContext = ctx.r3.u32, stack = ctx.r1.u32;
  if (((pumpContext | stack) & 3u) || stack < 0x1000u ||
      !mappedRange(pumpContext, 4u, false) || !mappedRange(stack - 0x1000u, 0x1000u, true)) return false;
  const uint32_t state = rd(pumpContext);
  if ((state & 3u) || !mappedRange(state, 0x17Cu, true)) return false;

  PPCContext work = ctx;
  work.r1.u32 = stack - 0x80u;
  writeMarkerWord(work.r1.u32, stack);
  auto takeDriverLock = [&] {
    auto* word = reinterpret_cast<volatile LONG*>(K::guestVAToHost(state));
    while (InterlockedCompareExchange(word, LONG(_byteswap_ulong(1u)), 0) != 0) YieldProcessor();
  };
  takeDriverLock();
  const uint32_t participants = rd(state + 0x30u) + 1u;
  writeMarkerWord(state + 0x30u, participants);
  uint64_t cursor = ctx.r3.u64;
  bool repeatContinuation = int32_t(participants) > 1;
  if (!repeatContinuation) {
    const uint32_t pending = rd(state + 0x58u);
    if (!pending) {
      writeMarkerWord(state + 0x30u, 0u);
      writeMarkerWord(state, 0u);
      return true;  // SDK returns the incoming context pointer unchanged.
    }
    cursor = rd(state + 0x50u);
    writeMarkerWord(state + 0x34u, 1u);
    writeMarkerWord(state + 0x58u, 0u);
    if (cursor) writeMarkerWord(state + 0x50u, 0u);
    else {
      writeMarkerWord(state + 0x54u, pending);
      cursor = uint64_t(pending) + 4u;
    }
  }
  for (;;) {
    if (repeatContinuation) {
      const uint32_t iteration = rd(state + 0x18u) + 1u;
      writeMarkerWord(state + 0x18u, iteration);
      if (iteration < rd(state + 0x1Cu)) {
        work.r3.u64 = ctx.r3.u64;
        work.r4.u64 = rd(state + 0x14u);
        work.r5.u64 = rd(state + 0x20u);
        work.r6.u64 = rd(state + 0x18u);
        const uint32_t handler = rd(state + 0x10u);
        writeMarkerWord(state, 0u);
#if defined(SR_DIFFTEST)
        if (driverObserver) driverObserver(handler, work.r6.u32);
#endif
        PPC_LOOKUP_FUNC(base, handler)(work, base);
        continue;
      }
      cursor = rd(state + 0x24u);
      writeMarkerWord(state + 0x18u, 0u);
      repeatContinuation = false;
    }
    const uint32_t header = rd(uint32_t(cursor));
    if (!(header & 0x80000000u)) {
      writeMarkerWord(state + 0x18u, 0u);
      writeMarkerWord(state + 0x1Cu, header & 0xFFFFu);
      writeMarkerWord(state + 0x20u, uint32_t(cursor) + 4u);
      writeMarkerWord(state + 0x24u, uint32_t(cursor) + ((header >> 16) & 0x7FFFu) + 4u);
      // Retail is a do/while: even repeat count zero invokes iteration zero.
      work.r3.u64 = ctx.r3.u64;
      work.r4.u64 = rd(state + 0x14u);
      work.r5.u64 = rd(state + 0x20u);
      work.r6.u64 = 0u;
      const uint32_t handler = rd(state + 0x10u);
      writeMarkerWord(state, 0u);
#if defined(SR_DIFFTEST)
      if (driverObserver) driverObserver(handler, work.r6.u32);
#endif
      PPC_LOOKUP_FUNC(base, handler)(work, base);
      repeatContinuation = true;
    } else if (header == 0x81000000u) {
      writeMarkerWord(state + 0x10u, rd(uint32_t(cursor) + 4u));
      writeMarkerWord(state + 0x14u, rd(uint32_t(cursor) + 8u));
      cursor += 12u;
    } else if ((header & 0xC0000000u) == 0xC0000000u) {
      if (header != 0xC0000000u) cursor = uint64_t(header) + 4u;
      else {
        work.r3.u64 = state;
        work.r4.u64 = cursor;
        sub_82110768(work, base);
        cursor = work.r3.u64;
        if (!uint32_t(cursor)) {
          ctx.r3.u64 = cursor;
          return true;
        }
      }
    } else {
      writeMarkerWord(state + 0x6Cu, 1u);
      const uint32_t remaining = rd(state + 0x30u) - 1u;
      writeMarkerWord(state + 0x30u, remaining);
      if (!remaining) {
        work.r3.u64 = cursor;
        work.r4.u64 = state;
        sub_82110460(work, base);
        cursor = work.r3.u64;
        writeMarkerWord(state + 0x6Cu, 0u);
      } else {
        writeMarkerWord(state + 0x24u, uint32_t(cursor));
        writeMarkerWord(state, 0u);
        while (rd(state + 0x6Cu)) YieldProcessor();
        takeDriverLock();
        // SDK keeps this participant's cursor; the shared parked cursor is
        // repeat-loop bookkeeping, not a replacement for this local value.
      }
      writeMarkerWord(state + 0x30u, rd(state + 0x30u) + 1u);
    }
  }
}
}

extern "C" void __imp__sub_82110460(PPCContext& __restrict, uint8_t*);
PPC_FUNC(sub_82110460) {
  if (!(sr::native::pump::markerGateOn() && sr::native::pump::replayMarkers(ctx, base)))
    __imp__sub_82110460(ctx, base);
}
extern "C" void __imp__sub_82110840(PPCContext& __restrict, uint8_t*);
PPC_FUNC(sub_82110840) {
  SR_NATIVE_SCOPE(0x82110840);  // NATIVE N0 (scripts/gen_native_brackets.py): the pump driver's table row
  if (!(sr::native::pump::driverGateOn() && sr::native::pump::replayDriver(ctx, base)))
    __imp__sub_82110840(ctx, base);
}
