#include "native_recording.hpp"
#include "native_memory.hpp"
#include "native_flush.hpp"
#include "guest_context.hpp"
#include "sr_oracle.hpp"
#if SR_ORACLE
#include "gpu_write_watch.hpp"
#endif
#include "guest/ppc/ppc_recomp_shared.h"
#include <cstdlib>
#include <initializer_list>

namespace sr::native::recording {
namespace {
namespace K = sr::kernel;
uint32_t rd(uint32_t address) { return K::readGuestU32BE(address); }
void writeRecordingWord(uint32_t address, uint32_t value) { K::writeGuestBE32(address, value); }
uint32_t physical(uint32_t address) {
  return (((address >> 20) + 0x200u) & 0x1000u) + (address & 0x1FFFFFFFu);
}
bool writable(uint32_t address, uint32_t bytes) {
  if (!address || !bytes || address > UINT32_MAX - (bytes - 1u)) return false;
  uint8_t* first = K::guestVAToHost(address);
  uint8_t* last = K::guestVAToHost(address + bytes - 1u);
#if SR_ORACLE
  return first && last && last - first == ptrdiff_t(bytes - 1u) &&
      (sr::native::memory::writableRange(first, bytes) || K::gww::cpuWritableRange(first, bytes));
#else
  return first && last && last - first == ptrdiff_t(bytes - 1u) &&
      sr::native::memory::writableRange(first, bytes);
#endif
}
// p2.transport BRACKET's gate (SR_NATIVE_BRACKET, default ON since the transport bake, 2026-10-15; =0 off) as a zero-arg
// helper in this file too, so the override manifest generator names it for 82110C78; the same latched value as
// native_flush.cpp's bracketGate() (the same expression: unset or anything but '0' = ON).
bool bracketEndGate() {
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_BRACKET");
    return !e || e[0] != '0';
  }();
  return on;
}
}

bool kickGateOn() {
  static const bool on = [] {
    const char* value = std::getenv("SR_NATIVE_REPLAY_KICK");
    return value && value[0] == '1';
  }();
  return on;
}

bool endRecordingKick(PPCContext& ctx, uint8_t* base) {
  if (!kickGateOn()) return false;
  const uint32_t dev = ctx.r3.u32, stack = ctx.r1.u32;
  if (((dev | stack) & 3u) || stack < 0x1000u || !writable(dev, 0x4E00u) ||
      !writable(stack - 0x1000u, 0x1000u)) return false;
  // Admission precedes every helper and mutation. Helpers may extend these
  // lists; their returned storage remains caller-owned through the whole call.
  for (uint32_t offset : {0x32C8u, 0x32A0u, 0x32B4u}) {
    const uint32_t prefix = rd(dev + offset + 4u), terminal = rd(dev + offset + 8u);
    if (((prefix | terminal) & 3u) || !writable(prefix, 4u) || !writable(terminal, 4u)) return false;
  }
  const uint32_t oldTail = rd(dev + 0x32E4u);
  if ((oldTail & 3u) || !writable(oldTail, 4u)) return false;

  PPCContext work = ctx;
  work.r1.u32 = stack - 0xB0u;
  writeRecordingWord(work.r1.u32, stack);
  const uint32_t countOut = work.r1.u32 + 0x50u, physOut = work.r1.u32 + 0x54u;
  if (!(K::guestVAToHost(dev + 0x2A39u)[0] & 0x40u)) {
    work.r3.u64 = ctx.r3.u64;
    work.r4.u64 = work.r1.u64 + 0x54u;
    work.r5.u64 = work.r1.u64 + 0x50u;
    sub_820FD3B8(work, base);
  }
  work.r3.u64 = ctx.r3.u64; work.r4.u64 = 100u; work.r5.u64 = 32u;
  sub_820FCDF0(work, base);  // 100 dwords, 32-byte alignment (allocator multiplies by four).
  const uint64_t block = work.r3.u64;
  uint64_t blockEnd;
  sr::native::flush::ReplayKickCallback callbackTail;
  if (uint32_t(block)) {
    const uint32_t root = rd(dev + 0x32C8u);
    work.r3.u64 = ctx.r3.u64; work.r4.u64 = block - 4u;
    work.r5.u64 = ctx.r5.u64 | 1u;
    work.r6.u64 = uint64_t(int64_t(int32_t(0x82110B00u)));
    work.r7.u64 = root ? uint64_t(physical(root)) - 0x40000000ull : 0ull;
    if (!callbackTail.emit(work)) sub_820FDA80(work, base);
    blockEnd = work.r3.u64 + 4u;
  } else blockEnd = rd(physOut);  // Preserve the SDK's existing stack value on pre-existing failure.

  work.r3.u64 = ctx.r3.u64; work.r4.u64 = 32u; work.r5.u64 = 4u;
  sub_820FCDF0(work, base);
  uint64_t appendAt = uint32_t(work.r3.u64) ? work.r3.u64 : uint64_t(rd(physOut));
  const uint8_t flags = K::guestVAToHost(dev + 0x2A39u)[0];
  writeRecordingWord(dev + 0x3290u, 0u); writeRecordingWord(dev + 0x3294u, 0u);
  const uint32_t failed = (flags >> 6) & 1u;
  if (failed) {
    const uint32_t fields[] = {0x28u, 0x2Cu, 0x34C4u, 0x34C0u, 0x34B8u, 0x34BCu};
    for (unsigned i = 0; i < 6u; ++i) writeRecordingWord(dev + fields[i], rd(dev + 0x32F0u + i * 4u));
    K::guestVAToHost(dev + 0x2A39u)[0] = flags & 0xBFu;
    writeRecordingWord(dev + 0x30u, rd(dev + 0x32F4u) - 160u);
  } else {
    const uint32_t count = rd(countOut);
    for (uint32_t offset : {0x32C8u, 0x32A0u, 0x32B4u}) {
      writeRecordingWord(rd(dev + offset + 4u), physical(rd(dev + offset + 8u)) - 0x40000000u);
      writeRecordingWord(rd(dev + offset + 8u), 0xC0000000u);
      writeRecordingWord(dev + offset + 12u, 0u);
    }
    if (count) {
      work.r3.u64 = ctx.r3.u64; work.r4.u64 = appendAt;
      work.r5.u64 = rd(physOut); work.r6.u64 = count; work.r7.u64 = 0u;
      work.r8.u64 = ctx.r3.u64 + 0x32DCu;
      sub_820FD800(work, base);
      appendAt = work.r3.u64;
    }
    if (rd(dev + 0x4DF4u) || !(K::guestVAToHost(dev + 0x2A39u)[0] & 2u)) {
      work.r3.u64 = ctx.r3.u64; work.r4.u64 = appendAt;
      work.r5.u64 = physical(uint32_t(block));
      work.r6.u64 = uint64_t(int64_t(int32_t(uint32_t(blockEnd - block))) >> 2);
      work.r7.u64 = 1u; work.r8.u64 = ctx.r3.u64 + 0x32DCu;
      callbackTail.append(work, base);
    }
  }
  ctx.r3.u64 = failed;
  return true;
}
}

extern "C" void __imp__sub_82110C78(PPCContext& __restrict, uint8_t*);
PPC_FUNC(sub_82110C78) {
  // p2.transport BRACKET_END (SR_NATIVE_BRACKET, default ON since the transport bake): taken only for a bracket whose
  // begin was native.
  if (sr::native::recording::bracketEndGate() && sr::native::flush::bracketEnd(ctx, base)) return;
  if (!(sr::native::recording::kickGateOn() && sr::native::recording::endRecordingKick(ctx, base)))
    __imp__sub_82110C78(ctx, base);
}
