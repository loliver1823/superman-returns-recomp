// boot gaps (native/boot-gaps, 2026-10-16; docs/NATIVE_ROADMAP.md "BOOT GAPS BUILD NOTE"): the display controller's
// PM4 emitters as records, under SR_NATIVE_DISPLAY (default ON; =0 keeps every guest body and SWAP's persisted-display
// decline - the A/B arm) on the native transport (SR_NATIVE_SEGMENTLESS, record mode on).
//
// Who reaches them (IDA, read-only, 2026-10-16; every xref other than the code calls below is the .pdata entry):
//   GpuRingEmitGammaRamp_821056E0 / GpuRingEmitGammaRampWide_821057C0(dev, ramp)
//       <- D3DDeviceSetDisplayModeAndGamma_82105DD0 (when its linear ramp differs from dev+0x3510) and
//          GpuSetGammaRamp_82111CB8 (tail).
//   GpuSetGammaRamp_82111CB8(dev, mode, ramp|0) <- D3DDeviceSwap_82112050 only, under dev byte 0x2A3B bit 0x40, whose
//       one setter is that Swap's own system-command-buffer branch (sysbuf+8 != 0: never on this port - the host
//       descriptor is Xenia's zero fill, VdGetSystemCommandBuffer and the native SWAP alike). The XDK's SetGammaRamp is
//       not linked (dev+0x3510 is written only by 82105DD0).
//   GpuScalerEmitCommandBuffer_8210A788(dev, w, h, rect) <- 82112050 only, under byte 0x2A3A bit 0x10, set by
//       GpuScalerSetSourceRectNotify_8210A9F8 - called by D3DDeviceCreatePresentBuffers 82105B18 (device creation) and
//       by Swap when a non-null rectangle differs from dev+0x33D0; the four Swap callers pass 0, 0, 0 and dev+0x33D0
//       itself - and by Swap's sysbuf branch (never, as above).
//   D3DDeviceSetDisplayModeAndGamma_82105DD0 <- 82112050 only, under byte 0x2A39 bit 0x10 (the persisted display), set
//       only by 82105B18: consumed by the FIRST Swap (dcf2 census1: SWAP_DECL #1 why=flags2A39, frame 2's 527 literal
//       packets = 82105DD0's table ramp 2309 dwords + the scaler block + Swap's own type-0 / REG_RMW 0x1E4E).
// So on this title every display emitter runs at most once, at the first present; no settings menu reaches them (the
// retail options are gameplay / audio / controller; the port's DISPLAY OPTIONS brightness / gamma / upscale are host
// present-chain stages - gfx/d3d12_present_chain.cpp - that call no guest code).
//
// Record forms (kernel/native_record_format.hpp): kRkGammaRamp {form, values} and kRkScaler {0}, applied by the
// handlers below through the record engine's register writes (writeReg: the DC_LUT side effects of gfx/gamma_ramp.hpp)
// and its WAIT_REG_MEM (applyWaitRegMem), rendered back to the SDK's exact dwords for the CPPM4 split. 82111CB8's
// REG_RMW 0x1841 / type-0 0x1930 prefix is one Entry of a kRkRegRmw item and a run. Each leaf decides at the top - the
// gate, record mode, the transport window, outside the SDK oracles, a fault-free snapshot of what it reads, an Entry -
// and the guest body runs whole when it declines (counted). The ramp leaves and 82111CB8 leave r3..r10 as their asm
// does; the scaler leaf sets r3 (its one caller, Swap, reads no volatile register after it). r0 / r11 / r12 are not
// argument registers: the generated bodies keep them local (tools/regen/superman_regen.toml [localize]
// non_argument_as_local), so no SDK body writes their ctx copy and no leaf does either.
// Proof: build/difftest.exe --boot-gaps-test (the actual SDK bodies against these leaves on fixture devices) and
// --swap-sdk-test (the first Swap's persisted-display path); tools/perf/native_proof.py --plan boot_gaps_bake.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "guest/ppc/ppc_recomp_shared.h"
#include "export_registry.hpp"
#include "guest_context.hpp"
#include "native_flush.hpp"
#include "../app/log.hpp"
#include "../gfx/gamma_ramp.hpp"
#if SR_ORACLE  // C1 stage 1: the CP's engine is the development PM4 walker; the release configuration links none
#include "../gfx/pm4_engine.hpp"
#else
#include "../gfx/record_engine.hpp"
#endif

extern "C" void __imp__sub_821056E0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821057C0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82111CB8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8210A788(PPCContext& __restrict ctx, uint8_t* base);

namespace sr::native::flush {

bool displayGate() {  // the gate as a zero-arg helper (the override manifest generator follows it)
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_DISPLAY");
    return !e || e[0] != '0';
  }();
  return on;
}

namespace {

std::atomic<uint64_t> g_ramp{0}, g_rampWide{0}, g_setGamma{0}, g_scaler{0}, g_persist{0};
std::atomic<uint64_t> g_rampDecl{0}, g_setGammaDecl{0}, g_scalerDecl{0}, g_scalerProgram{0}, g_reserveFail{0};
std::atomic<uint64_t> g_cpRamp{0}, g_cpScaler{0};

uint32_t rd(uint32_t va) { return sr::kernel::readGuestU32BE(va); }
void dispWrite32(uint32_t va, uint32_t v) { sr::kernel::writeGuestBE32(va, v); }
void wr16(uint32_t va, uint32_t v) {
  if (uint8_t* p = sr::kernel::guestVAToHost(va)) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
  }
}
uint32_t be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }
void put32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

// ---- kRkGammaRamp: 821056E0 (form 0, 2309 dwords) / 821057C0 (form 1, 1413 dwords) ----
constexpr uint32_t kRampTableWords = 257u, kRampPwlWords = 385u;
constexpr uint32_t kRampTableBytes = 2309u * 4u, kRampPwlBytes = 1413u * 4u;
uint32_t rampBytes(const uint32_t* w, uint32_t n) {
  if (!w) return 0u;
  if (w[0] == 0u && n == kRampTableWords) return kRampTableBytes;
  if (w[0] == 1u && n == kRampPwlWords) return kRampPwlBytes;
  return 0u;
}
// The payload from the 1536-byte ramp (three 256-entry big-endian u16 planes), exactly as the asm packs it.
// 821056E0: value i = ((p0[i] << 10) & 0x03FF0000 | p1[i]) << 4 & 0x3FFFFC00 | p2[i] >> 6 (`rlwimi r7,r8,10,6,15;
// rlwinm r7,r7,4,2,21; srwi r8,r5,6; or`), i.e. the 10-bit red / green / blue of DC_LUT_30_COLOR.
// 821057C0: segment i, plane k = (pk[2i + 1] << 16) | pk[2i] (`lhz; rotrwi 16; or` at -0x200 / -0x202, 0 / -2,
// 0x200 / 0x1FE from ramp + 0x202 + 4i): DC_LUT_PWL_DATA {base, delta}.
uint32_t rampPayload(const uint8_t* ramp, bool wide, uint32_t* w) {
  const uint8_t* p0 = ramp;
  const uint8_t* p1 = ramp + 0x200u;
  const uint8_t* p2 = ramp + 0x400u;
  w[0] = wide ? 1u : 0u;
  if (!wide) {
    for (uint32_t i = 0; i < 256u; i++) {
      const uint32_t r7 = ((be16(p0 + 2u * i) << 10) & 0x03FF0000u) | be16(p1 + 2u * i);
      w[1u + i] = ((r7 << 4) & 0x3FFFFC00u) | (be16(p2 + 2u * i) >> 6);
    }
    return kRampTableWords;
  }
  for (uint32_t i = 0; i < 128u; i++) {
    w[1u + 3u * i] = (be16(p0 + 4u * i + 2u) << 16) | be16(p0 + 4u * i);
    w[2u + 3u * i] = (be16(p1 + 4u * i + 2u) << 16) | be16(p1 + 4u * i);
    w[3u + 3u * i] = (be16(p2 + 4u * i + 2u) << 16) | be16(p2 + 4u * i);
  }
  return kRampPwlWords;
}
#if SR_ORACLE  // C1 stage 1: the renderers serve the PM4-output / CPPM4 oracles only (constant off in release)
// The SDK's dwords, dword for dword as 821056E0 / 821057C0 write them after their reserve.
void rampRender(uint32_t kind, const uint32_t* w, uint32_t n, uint8_t* out, uint32_t bytes) {
  if (kind != kRkGammaRamp || !out || !rampBytes(w, n) || rampBytes(w, n) != bytes) return;
  uint32_t k = 0;
  auto put = [&](uint32_t v) { put32(out + 4u * k++, v); };
  put(0x00011921u);  // RW_MODE, RW_INDEX
  put(w[0]);
  put(0u);
  put(0x00001927u);  // WRITE_EN_MASK
  put(7u);
  if (w[0] == 0u) {
    for (uint32_t i = 0; i < 256u; i++) {
      const uint32_t v = w[1u + i];
      const uint32_t words[9] = {0x00001925u, v, 0xC0054500u, 7u, 0x1925u, v, 0xFFFFFFFFu, 0x1922u, i + 1u};
      for (uint32_t x : words) put(x);
    }
  } else {
    for (uint32_t i = 0; i < 128u; i++) {
      const uint32_t words[11] = {0x00029924u, w[1u + 3u * i], w[2u + 3u * i], w[3u + 3u * i],
                                  0xC0054500u, 7u, 0x1924u, 0u, 0xFFFFFFFFu, 0x1922u, i + 1u};
      for (uint32_t x : words) put(x);
    }
  }
}
#endif  // SR_ORACLE
// The packets' effect through the record engine: the type-0 writes in order (0x9924 is a one-register write: all three
// words to DC_LUT_PWL_DATA), then each COND_WRITE {wait_info 7, poll register, ref, mask, RW_INDEX, i + 1}: function 7
// always matches (Xenia MatchValueAndRef / the walker's truth table), the target is a register (wait_info bit 8 clear):
// writeReg(0x1922, i + 1). The walker's executor sync before a COND_WRITE makes its POLLED value coherent; function 7
// polls nothing, and the DC_LUT registers feed only the scan-out, which the swap's own flush precedes.
bool rampApply(uint32_t kind, const uint32_t* w, uint32_t n, sr::gfx::CpEngine& eng) {
  if (kind != kRkGammaRamp || !rampBytes(w, n)) return false;
  eng.writeReg(0x1921u, w[0]);
  eng.writeReg(0x1922u, 0u);
  eng.writeReg(0x1927u, 7u);
  if (w[0] == 0u) {
    for (uint32_t i = 0; i < 256u; i++) {
      eng.writeReg(0x1925u, w[1u + i]);
      eng.writeReg(0x1922u, i + 1u);
    }
  } else {
    for (uint32_t i = 0; i < 128u; i++) {
      eng.writeReg(0x1924u, w[1u + 3u * i]);
      eng.writeReg(0x1924u, w[2u + 3u * i]);
      eng.writeReg(0x1924u, w[3u + 3u * i]);
      eng.writeReg(0x1922u, i + 1u);
    }
  }
  g_cpRamp.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// ---- kRkScaler: 8210A788's block with no scaler program ----
constexpr uint32_t kScalerBytes = 80u;
const uint32_t kScalerHead[18] = {0x00001973u, 0x10000u, 0x00001964u, 1u, 0x00001973u, 0u,
                                  0xC0043C00u, 3u, 0x1973u, 0u, 1u, 0x100u,  // WAIT_REG_MEM {3, 0x1973, 0, 1, 256}
                                  0x00001852u, 0u, 0x00001838u, 1u, 0x00001973u, 0x10000u};
const uint32_t kScalerTail[2] = {0x00001973u, 0u};
#if SR_ORACLE  // C1 stage 1: the renderer, as rampRender (the scaler leaf writes the same dwords to guest memory)
void scalerRender(uint32_t kind, const uint32_t* w, uint32_t n, uint8_t* out, uint32_t bytes) {
  if (kind != kRkScaler || !w || n != 1u || w[0] || !out || bytes != kScalerBytes) return;
  for (uint32_t i = 0; i < 18u; i++) put32(out + 4u * i, kScalerHead[i]);
  for (uint32_t i = 0; i < 2u; i++) put32(out + 72u + 4u * i, kScalerTail[i]);
}
#endif  // SR_ORACLE
bool scalerApply(uint32_t kind, const uint32_t* w, uint32_t n, sr::gfx::CpEngine& eng) {
  if (kind != kRkScaler || !w || n != 1u || w[0]) return false;
  eng.writeReg(0x1973u, 0x10000u);
  eng.writeReg(0x1964u, 1u);
  eng.writeReg(0x1973u, 0u);
  uint8_t wait[20];
  const uint32_t values[5] = {3u, 0x1973u, 0u, 1u, 256u};
  for (uint32_t i = 0; i < 5u; ++i) put32(wait + 4u * i, values[i]);
  eng.applyWaitRegMem(wait, 5u);
  eng.writeReg(0x1852u, 0u);
  eng.writeReg(0x1838u, 1u);
  eng.writeReg(0x1973u, 0x10000u);
  eng.writeReg(0x1973u, 0u);
  g_cpScaler.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void ensureDisplayHandlers() {
  static std::once_flag registered;
  std::call_once(registered, [] {
#if SR_ORACLE
    setOpaqueHandler(kRkGammaRamp, rampApply, rampRender);
    setOpaqueHandler(kRkScaler, scalerApply, scalerRender);
#else  // C1 stage 1: the record engine applies them; no renderer (no PM4 output in the release configuration)
    setOpaqueHandler(kRkGammaRamp, rampApply);
    setOpaqueHandler(kRkScaler, scalerApply);
#endif
  });
}

// A probe of the Entry: can an entry open on this device right now? Nothing is written either way.
bool entryWouldOpen(PPCContext& ctx, uint8_t* base, uint32_t dev) {
  Entry probe(ctx, base, dev);
  if (!probe.open()) return false;
  probe.abandon();
  return true;
}

}  // namespace

uint32_t displayRecordBytes(uint32_t kind, const uint32_t* w, uint32_t n) {
  if (kind == kRkGammaRamp) return rampBytes(w, n);
  if (kind == kRkScaler) return (w && n == 1u && !w[0]) ? kScalerBytes : 0u;
  return 0u;
}

// GpuRingEmitGammaRamp_821056E0 (wide = false) / GpuRingEmitGammaRampWide_821057C0 (wide = true) (dev r3, ramp r4):
// the reserve (2309 / 1413 dwords: 820FC910 at the asm's site), then one kRkGammaRamp record standing for every dword the
// asm writes; dev+0x28 as the asm leaves it (Entry::close). Rule 1: the ramp is read (fault-free snapshot) and the Entry
// opened before anything is written. The reserve's failure (820FCF90 returned 0: the SDK would store through a null
// cursor) emits nothing and is counted. r3..r10 as the asm leaves them (r11 is local to the generated body).
bool gammaRampLeaf(PPCContext& ctx, uint8_t* base, bool wide) {
  if (!displayActive()) return false;
  const uint32_t dev = ctx.r3.u32, ramp = ctx.r4.u32;
  uint8_t src[1536];
  uint32_t payload[kRampPwlWords];
  if (!guestRead(ramp, src, sizeof(src))) {
    g_rampDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint32_t n = rampPayload(src, wide, payload);
  ensureDisplayHandlers();
  Entry e(ctx, base, dev);
  if (!e.open()) {
    g_rampDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  // The reserve's new segment calls the guest allocator while this Entry owns the arena lock: any SDK emitter it reaches
  // stays its guest body (the Swap front write's and Entry::presentGate's rule) rather than opening a nested Entry.
  ShaderConstRunsOracle nested;
  const int reserved = e.reserve(wide ? 1413u : 2309u);
  if (reserved < 0) {  // the XDK's failure: nothing emitted (the SDK would write through cursor 0)
    e.close();
    g_reserveFail.fetch_add(1, std::memory_order_relaxed);
  } else if (reserved == 0 || !e.opaque(kRkGammaRamp, payload, n, wide ? kRampPwlBytes : kRampTableBytes)) {
    e.abandon();  // unreachable in record mode; the guest body re-reserves (it now fits) and writes the SDK's bytes
    g_rampDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  } else {
    e.close();
  }
  const uint32_t cursor = rd(dev + 40u);
  if (!wide) {
    ctx.r3.u64 = 0x1922u;
    ctx.r4.u64 = 0xFFFFFFFFC0054500ull;
    ctx.r5.u64 = ~0ull;
    ctx.r6.u64 = 0x1925u;
    ctx.r7.u64 = 7u;
    ctx.r8.u64 = payload[256];
    ctx.r9.u64 = 0x100u;
  } else {
    ctx.r3.u64 = ~0ull;
    ctx.r4.u64 = 0u;
    ctx.r5.u64 = 0x1924u;
    ctx.r6.u64 = 7u;
    ctx.r7.u64 = 0xFFFFFFFFC0054500ull;
    ctx.r8.u64 = payload[384];
    ctx.r9.u64 = 0x80u;
  }
  ctx.r10.u64 = cursor;
  (wide ? g_rampWide : g_ramp).fetch_add(1, std::memory_order_relaxed);
  return true;
}

// GpuSetGammaRamp_82111CB8(dev r3, mode r4, ramp r5 | 0): the kick check, REG_RMW {0x1841, 0xFFFFF8FF, wide ? 0x100 :
// 0} and type-0 0x1930 = wide ? 3 : 0 (wide = mode == 0x28280136, `cntlzw(0x28280136 - mode) << 3 & 0x100`) as one
// Entry of two items; then, with no ramp, the asm's linear ramp in its own frame (0x680 bytes, the planes at sp+0x50)
// through its builder (GpuBuildDisplayGammaRampPWL_82105458 / GpuBuildDisplayGammaRamp_82105278 - guest bodies), and the
// emitter (821057C0 / 821056E0, the leaves above) called with that frame as the asm calls it. r3..r10 = the emitter's.
bool setGammaRampLeaf(PPCContext& ctx, uint8_t* base) {
  if (!displayActive()) return false;
  const uint32_t dev = ctx.r3.u32, mode = ctx.r4.u32, ramp = ctx.r5.u32, sp = ctx.r1.u32;
  const bool wide = mode == 0x28280136u;
  const uint32_t frame = sp - 0x680u;
  // the frame the asm uses (its planes, the builders' frames below it: 0x8F0 more) must be the guest's to write
  if (sp < 0x1000u || !guestWritable(sp - 0x1000u, 0x1000u)) {
    g_setGammaDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  PPCContext work = ctx;  // the asm's calls run in its frame
  work.r1.u64 = frame;
  {
    Entry e(work, base, dev);
    if (!e.open()) {
      g_setGammaDecl.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    uint8_t value[4];
    put32(value, wide ? 3u : 0u);
    ShaderConstRunsOracle nested;  // the kick's segment switch under the arena lock (gammaRampLeaf's rule)
    if (!e.kick() || !e.regRmw(0x1841u, 0xFFFFF8FFu, wide ? 0x100u : 0u) || !e.run(0x1930u, 1u, value)) {
      e.abandon();  // unreachable in record mode (a kick taken stays the SDK's own); the guest body runs whole
      g_setGammaDecl.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    e.close();
  }
  dispWrite32(frame, sp);  // the back chain (stwu r1, -0x680(r1))
  uint32_t src = ramp;
  if (!ramp) {
    const uint32_t planes = frame + 0x50u;
    if (wide) {  // 0x82111D4C: 128 iterations, two entries each, the same value in all three planes
      uint32_t r9 = 0u;
      for (uint32_t i = 0; i < 128u; i++) {
        const uint32_t r10 = (r9 / 0x7Fu) & 0xFFFFu;
        r9 += 0xFFFFu;
        const uint32_t r8 = ((r9 / 0x7Fu) - r10) & 0xFFFFu;
        for (uint32_t p = 0; p < 3u; p++) {
          wr16(planes + 0x200u * p + 4u * i, r10);
          wr16(planes + 0x200u * p + 4u * i + 2u, r8);
        }
      }
    } else {  // 0x82111DB8: 256 entries, ((k * 0x3FF / 0xFF) << 6) & 0xFFC0 in all three planes
      uint32_t r10 = 0u;
      for (uint32_t k = 0; k < 256u; k++) {
        const uint32_t v = ((r10 / 0xFFu) << 6) & 0x3FFFC0u & 0xFFFFu;
        r10 += 0x3FFu;
        for (uint32_t p = 0; p < 3u; p++) wr16(planes + 0x200u * p + 2u * k, v);
      }
    }
    work.r3.u64 = planes;
    work.r4.u64 = planes;
    work.r5.u64 = 0u;
    if (wide) sub_82105458(work, base);
    else sub_82105278(work, base);
    src = planes;
  }
  work.r3.u64 = dev;
  work.r4.u64 = src;
  if (wide) sub_821057C0(work, base);
  else sub_821056E0(work, base);
  ctx.r3.u64 = work.r3.u64;
  ctx.r4.u64 = work.r4.u64;
  ctx.r5.u64 = work.r5.u64;
  ctx.r6.u64 = work.r6.u64;
  ctx.r7.u64 = work.r7.u64;
  ctx.r8.u64 = work.r8.u64;
  ctx.r9.u64 = work.r9.u64;
  ctx.r10.u64 = work.r10.u64;
  g_setGamma.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// GpuScalerEmitCommandBuffer_8210A788(dev r3, w r4, h r5, rect r6), from its asm in its own frame (0x9E0 bytes):
// GpuScalerNormalizeDestRect_8210A660(dev, w, h, rect, sp+0x80) and GpuScalerConfigureState_8210A0F8 (guest bodies,
// stack outputs only), RtlFillMemoryUlong(sp+0xC0, 0x320, 0x80000000), the reserve (220 dwords), then the platform's
// VdInitializeScalerCommandBuffer with the asm's arguments: with no program (0 dwords - this port's display has no
// hardware scaler) the block is one kRkScaler record of the SDK's 80 bytes; with a program (never on this platform) the
// SDK's own bytes are written - head, program, tail - a literal gap, counted (scalerProgram). The helpers precede the
// Entry: they write only the stack below sp and query kernel exports, so a decline after them (the Entry no longer
// opening) leaves the guest body to repeat them. r3 = the export's r3 (memcpy's result is the destination: cursor + 76).
bool scalerLeaf(PPCContext& ctx, uint8_t* base) {
  if (!displayActive()) return false;
  const uint32_t dev = ctx.r3.u32, w = ctx.r4.u32, h = ctx.r5.u32, rect = ctx.r6.u32, sp = ctx.r1.u32;
  auto& registry = sr::kernel::ExportRegistry::instance();
  const auto fill = registry.getExport("RtlFillMemoryUlong");
  const auto init = registry.getExport("VdInitializeScalerCommandBuffer");
  if (!fill || !init || sp < 0x1800u || !guestWritable(sp - 0x1800u, 0x1800u) || !entryWouldOpen(ctx, base, dev)) {
    g_scalerDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  ensureDisplayHandlers();
  const uint32_t f = sp - 0x9E0u;
  dispWrite32(f, sp);  // the back chain (stwu r1, -0x9E0(r1))
  PPCContext work = ctx;
  work.r1.u64 = f;
  work.r3.u64 = dev;
  work.r4.u64 = w;
  work.r5.u64 = h;
  work.r6.u64 = rect;
  work.r7.u64 = f + 0x80u;
  sub_8210A660(work, base);
  // 0x8210A7A8 .. 0x8210A890: the arguments of 8210A0F8, register for register.
  uint32_t r6 = rd(f + 0x80u), r5 = rd(f + 0x84u);
  uint32_t r9 = rd(f + 0x88u) - r6;
  const uint32_t r3d = rd(f + 0x8Cu) - r5;
  uint32_t r11 = rd(f + 0x90u);
  const uint32_t r10 = rd(dev + 0x4E04u);
  uint32_t r8 = rd(dev + 0x4DFCu);
  if (r10 != r8) {
    if (r11 >= r10 && r11 <= r8) r11 = r10;
    if (r9 >= r10 && r9 <= r8) {
      r8 = r8 - r11;
      r9 = r10;
      r6 = r8 >> 1;
    }
  }
  const uint32_t r7a = r10 - r11;  // 0x8210A804
  const uint32_t f94 = rd(f + 0x94u);
  const uint32_t r27 = r11 << 16;
  const uint32_t r8b = r7a >> 1;
  const uint32_t r7b = (rd(dev + 0x4E00u) - f94) >> 1;
  const uint32_t r31 = ((w & 0xFFFFu) << 16) | (h & 0xFFFFu);
  const uint32_t r28 = f94 & 0xFFFFu;
  const uint32_t a5 = ((r9 & 0xFFFFu) << 16) | (r3d & 0xFFFFu);
  const uint32_t a4 = ((r6 & 0xFFFFu) << 16) | (r5 & 0xFFFFu);
  dispWrite32(f + 0x70u, rd(f + 0x98u));
  dispWrite32(f + 0x64u, f + 0x3E0u);
  dispWrite32(f + 0x5Cu, f + 0xACu);
  dispWrite32(f + 0x54u, f + 0x70u);
  dispWrite32(f + 0x74u, rd(f + 0xA8u));
  const uint32_t r30 = ((r8b & 0xFFFFu) << 16) | (r7b & 0xFFFFu);
  work.r3.u64 = dev;
  work.r4.u64 = a4;
  work.r5.u64 = a5;
  work.r6.u64 = r30;
  work.r7.u64 = r28 | r27;
  work.r8.u64 = r31;
  work.r9.u64 = f + 0x74u;
  work.r10.u64 = f + 0x9Cu;
  sub_8210A0F8(work, base);
  fill(f + 0xC0u, 0x320u, 0x80000000u, 0u, 0u, 0u);  // RtlFillMemoryUlong(sp+0xC0, 800, 0x80000000)
  Entry e(work, base, dev);
  if (!e.open()) {  // the probe opened at the top; arena pressure since: the guest body repeats the pure helpers
    g_scalerDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  ShaderConstRunsOracle nested;  // the reserve's segment switch under the arena lock (gammaRampLeaf's rule)
  const int reserved = e.reserve(0xDCu);
  if (reserved < 0) {  // the XDK's failure: nothing emitted (the SDK would write through cursor 0)
    e.close();
    g_reserveFail.fetch_add(1, std::memory_order_relaxed);
    ctx.r3.u64 = 0u;
    g_scaler.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  if (reserved == 0) {  // unreachable in record mode
    e.abandon();
    g_scalerDecl.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  // 0x8210A8E4 .. 0x8210A9BC: VdInitializeScalerCommandBuffer's arguments (the stack words at sp+0x54 .. 0x6C as
  // stored; r8 / r10 re-read after 8210A0F8 wrote them).
  dispWrite32(f + 0x6Cu, 0xC8u);
  dispWrite32(f + 0x64u, f + 0xC0u);
  dispWrite32(f + 0x5Cu, f + 0x3E0u);
  dispWrite32(f + 0x54u, f + 0xACu);
  const uint32_t v47 = rd(f + 0x80u), v48 = rd(f + 0x84u), v49 = rd(f + 0x88u), v50 = rd(f + 0x8Cu);
  const uint32_t x3 = ((v47 & 0xFFFFu) << 16) | (v48 & 0xFFFFu);
  const uint32_t x4 = (((v49 - v47) & 0xFFFFu) << 16) | ((v50 - v48) & 0xFFFFu);
  sr::kernel::ExportRegistry::setCurrentExtraArgs(f + 0x9Cu, rd(f + 0x70u));
  const uint32_t program = init(x3, x4, r30, r28 | r27, r31, rd(f + 0x74u));
  if (program == 0u) {
    const uint32_t zero = 0u;
    if (!e.opaque(kRkScaler, &zero, 1u, kScalerBytes)) {  // unreachable in record mode
      e.abandon();
      g_scalerDecl.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    e.close();
  } else {
    // A scaler program from the platform export: the SDK's own bytes (a literal gap - fatal in the release build, by
    // design: no record form stands for kernel-made PM4), counted.
    e.close();
    const uint32_t at = rd(dev + 40u);
    for (uint32_t i = 0; i < 18u; i++) dispWrite32(at + 4u + 4u * i, kScalerHead[i]);
    for (uint32_t i = 0; i < program; i++) dispWrite32(at + 76u + 4u * i, rd(f + 0xC0u + 4u * i));
    dispWrite32(at + 76u + 4u * program, kScalerTail[0]);
    dispWrite32(at + 80u + 4u * program, kScalerTail[1]);
    dispWrite32(dev + 40u, at + 80u + 4u * program);
    g_scalerProgram.fetch_add(1, std::memory_order_relaxed);
  }
  ctx.r3.u64 = rd(dev + 40u) - 8u - 4u * program + 4u;  // memcpy's r3: its destination (the program's first dword)
  g_scaler.fetch_add(1, std::memory_order_relaxed);
  return true;
}

DisplayCounters displayCounters() {
  auto ld = [](const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); };
  DisplayCounters c;
  c.ramp = ld(g_ramp);
  c.rampWide = ld(g_rampWide);
  c.setGamma = ld(g_setGamma);
  c.scaler = ld(g_scaler);
  c.persist = ld(g_persist);
  c.rampDecl = ld(g_rampDecl);
  c.setGammaDecl = ld(g_setGammaDecl);
  c.scalerDecl = ld(g_scalerDecl);
  c.scalerProgram = ld(g_scalerProgram);
  c.reserveFail = ld(g_reserveFail);
  c.cpRamp = ld(g_cpRamp);
  c.cpScaler = ld(g_cpScaler);
  return c;
}
void displayNotePersist() { g_persist.fetch_add(1, std::memory_order_relaxed); }
// [NATIVE_FLUSH] <why> display gate=<SR_NATIVE_DISPLAY> modeGate=<SR_NATIVE_MODE_STATE> persist= ramp=<table>/<pwl>
// setGamma= scaler= modeState= | decl ramp= setGamma= scaler= modeState= | scalerProgram= reserveFail= | cp ramp= scaler=
// | lut=<hash> - one line per report (the periodic draws report and the graceful quit's terminal snapshot), the gates off
// included. lut = FNV-1a 64 over the display ramp the presenter applies to every frame (gfx/gamma_ramp.hpp displayRamp:
// the 256 table entries, then the 128 x 3 PWL entries, each as its 4 little-endian bytes): the first Swap's ramp lands
// there through the record (rampApply) or, with SR_NATIVE_DISPLAY=0, through the PM4 walker's gap part - the two arms of
// a proof must print the same value. Read from the report thread: the ramp is written once, at the first present.
uint64_t displayLutHash() {
  const auto& r = sr::gfx::gamma::displayRamp();
  uint64_t h = 0xCBF29CE484222325ull;
  auto mix = [&h](uint32_t v) {
    for (int b = 0; b < 4; b++) {
      h ^= (v >> (8 * b)) & 0xFFu;
      h *= 0x100000001B3ull;
    }
  };
  for (uint32_t i = 0; i < 256u; i++) mix(r.table[i]);
  for (uint32_t i = 0; i < 128u; i++)
    for (uint32_t j = 0; j < 3u; j++) mix(r.pwl[i][j]);
  return h;
}
void displayReport(const char* why) {
  const DisplayCounters c = displayCounters();
  static const bool modeGate = [] {
    const char* e = std::getenv("SR_NATIVE_MODE_STATE");
    return !e || e[0] != '0';
  }();
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_FLUSH] %s display gate=%d modeGate=%d persist=%llu ramp=%llu/%llu setGamma=%llu scaler=%llu "
              "modeState=%llu | decl ramp=%llu setGamma=%llu scaler=%llu modeState=%llu | scalerProgram=%llu "
              "reserveFail=%llu | cp ramp=%llu scaler=%llu | lut=%016llx",
              why, displayGate() ? 1 : 0, modeGate ? 1 : 0, (unsigned long long)c.persist, (unsigned long long)c.ramp,
              (unsigned long long)c.rampWide, (unsigned long long)c.setGamma, (unsigned long long)c.scaler,
              (unsigned long long)modeStateCalls(), (unsigned long long)c.rampDecl, (unsigned long long)c.setGammaDecl,
              (unsigned long long)c.scalerDecl, (unsigned long long)modeStateDecl(), (unsigned long long)c.scalerProgram,
              (unsigned long long)c.reserveFail, (unsigned long long)c.cpRamp, (unsigned long long)c.cpScaler,
              (unsigned long long)displayLutHash());
}

}  // namespace sr::native::flush

// The four display emitters: under SR_NATIVE_DISPLAY (default ON) on the native transport the record leaves above; the
// guest body otherwise (the gate off, a decline at the top, inside the SDK oracles).
PPC_FUNC(sub_821056E0) {
  if (sr::native::flush::displayGate() && sr::native::flush::gammaRampLeaf(ctx, base, false)) return;
  __imp__sub_821056E0(ctx, base);
}
PPC_FUNC(sub_821057C0) {
  if (sr::native::flush::displayGate() && sr::native::flush::gammaRampLeaf(ctx, base, true)) return;
  __imp__sub_821057C0(ctx, base);
}
PPC_FUNC(sub_82111CB8) {
  if (sr::native::flush::displayGate() && sr::native::flush::setGammaRampLeaf(ctx, base)) return;
  __imp__sub_82111CB8(ctx, base);
}
PPC_FUNC(sub_8210A788) {
  if (sr::native::flush::displayGate() && sr::native::flush::scalerLeaf(ctx, base)) return;
  __imp__sub_8210A788(ctx, base);
}
