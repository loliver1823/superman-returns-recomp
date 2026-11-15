// iter-467 native-b, P1 row 9: the XDK's resolve / clear entries as native records (kernel/native_rec_b.hpp).
// C1: the dry CHECK (SR_NB_REC_CHECK) and the observe-only NaN watch (SR_NB_NANWATCH) are development oracles,
// compiled out under SR_ORACLE=0 (kernel/sr_oracle.hpp): both gates then read as unset. Record mode - its dry pre-pass,
// the live pass, the op list's render / apply and the shared port bodies (nb_rec_*.inc) - is unchanged.
#include "native_rec_b.hpp"

#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "native_flush.hpp"
#include "ab_toggle.hpp"
#include "native_brackets.hpp"
#include "sr_oracle.hpp"
#include "../app/log.hpp"
#if SR_ORACLE  // C1 stage 1: the CP's engine is the development PM4 walker; the release configuration links none
#include "../gfx/pm4_engine.hpp"
#else
#include "../gfx/record_engine.hpp"
#endif
#if SR_ORACLE  // the NaN watch's
#include "../gfx/hle_video.hpp"
#include "../gfx/pm4_live_trace.hpp"
#endif

namespace sr::native::recb {

namespace {

inline uint32_t be32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return _byteswap_ulong(v);
}
inline uint64_t be64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return _byteswap_uint64(v);
}
inline uint16_t be16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return _byteswap_ushort(v);
}
inline void put32(uint8_t* p, uint32_t v) {
  v = _byteswap_ulong(v);
  std::memcpy(p, &v, 4);
}
inline void put64(uint8_t* p, uint64_t v) {
  v = _byteswap_uint64(v);
  std::memcpy(p, &v, 8);
}
inline uint64_t rotl64(uint64_t v, unsigned s) { return s ? (v << s) | (v >> (64u - s)) : v; }

#if SR_ORACLE
bool envOn(const char* n) {
  const char* e = std::getenv(n);
  return e && e[0] == '1';
}
const bool g_check = envOn("SR_NB_REC_CHECK");
#else
constexpr bool g_check = false;  // SR_ORACLE=0: the dry CHECK is compiled out (reads as unset)
#endif
// Record mode: default ON (iter-467 native-b bake, job50: dry CHECK, frozen-pass A/B, flicker gate, meter); =0 keeps the
// A/B (the guest bodies write their PM4 again). The dry CHECK turns it off.
const bool g_onEnv = [] {
  const char* e = std::getenv("SR_NB_REC");
  return !e || e[0] != '0';
}() && !g_check;
const uint32_t g_kinds = [] {
  const char* e = std::getenv("SR_NB_REC_KINDS");
  return e && *e ? uint32_t(std::strtoul(e, nullptr, 0)) : 7u;
}();

// Statistics (one writer per side; torn reads only skew a report line).
struct Ctr {
  std::atomic<uint64_t> v{0};
  void add(uint64_t n = 1) { v.fetch_add(n, std::memory_order_relaxed); }
  uint64_t get() const { return v.load(std::memory_order_relaxed); }
};
const char* const kEntryName[kEntries] = {"resolve", "clearF", "gpr"};
#if SR_ORACLE
Ctr g_ckCalls[kEntries], g_ckFull[kEntries], g_ckStop[kEntries], g_ckOk[kEntries], g_ckBadLen[kEntries],
    g_ckBadBytes[kEntries], g_ckBadDev[kEntries], g_ckBadMem[kEntries], g_ckGuestKick[kEntries];
Ctr g_ckBytes[kEntries];
#endif
Ctr g_cpApplied, g_cpBad, g_cpOps, g_cpSkippedPred, g_cpNoEffect, g_cpRenderLenBad;
// 8210C090's format 6 (16_16_FLOAT) colour pack, the spliced body's D3DCOLOR form (nb_rec_resolve.inc rs_colorPack):
// how often it runs, and how often with a colour the hardware FLOAT16_2 pack would not also turn into 0.
Ctr g_fmt6Packs, g_fmt6NonZero;
// record mode (SR_NB_REC): per entry
Ctr g_recCalls[kEntries], g_recTaken[kEntries], g_recDeclOpen[kEntries], g_recDeclPre[kEntries],
    g_recLiveStop[kEntries], g_recItems[kEntries], g_recBytes[kEntries];
// p1.flush_residue (SR_NATIVE_FLUSH_SPLIT): entries taken whose pre-pass looked through a kick it could not model
// (kickT), runs the live pass chunked across the segment limit (chunks), and live stops after such a pre-pass (kickTStop:
// the transparency argument's witness, must stay 0).
Ctr g_recKickT[kEntries], g_recChunks[kEntries], g_recKickTStop[kEntries];
Ctr g_recWaitT[kEntries];  // p1.flush_residue: entries taken through a modelled GPU wait (Prod::waited)
// record mode's host cost (all entries): the pre-pass (device copy + dry port), Entry construction + open, the live pass
// (port + Entry close). TSC ticks; the report converts with the TSC rate measured against steady_clock.
Ctr g_recPreTsc, g_recOpenTsc, g_recLiveTsc;
const uint64_t g_tsc0 = __rdtsc();
const std::chrono::steady_clock::time_point g_clk0 = std::chrono::steady_clock::now();
#if SR_ORACLE
std::atomic<uint32_t> g_examples{0};
std::mutex g_stopMu;
std::vector<std::pair<std::string, uint64_t>> g_stopWhy[kEntries];

void noteStop(int entry, const char* why) {
  std::lock_guard<std::mutex> lk(g_stopMu);
  for (auto& p : g_stopWhy[entry])
    if (p.first == why) {
      p.second++;
      return;
    }
  if (g_stopWhy[entry].size() < 32) g_stopWhy[entry].emplace_back(why, 1);
}
#endif  // SR_ORACLE

// The device fields other threads write while an entry runs (strand A's list, kernel/native_flush.cpp asyncField):
// fence / time words. They are compared only when the port itself wrote them (Prod::wroteAsync).
// + dev+0x2D18 / 0x2D1C (11544 / 11548): the fetch-constant address range D3DDeviceCreate / InitAllocContexts' 8210E520
// resets ({0, FFFFFFFF}) and resource creation updates. During loading another thread moves it while an entry runs
// (job48: all 519 DEVICE BADs of 1.93M compared calls sat there, values like 1B984000 -> 1B2A0000 within a second, and
// none after the loading screen). No row-9 port reads or writes it.
bool asyncField(uint32_t off) {
  return (off >= 0x2A18u && off < 0x2BB4u && off != 0x2A38u) || off == 11344u || off == 11544u || off == 11548u;
}

}  // namespace

// ---- Prod ----
enum Mode : int { kModeDry = 0, kModeLive = 2 };

namespace {
void noteAsync(Prod& p, uint32_t off, uint32_t n) {
  if (!p.dry) return;
  for (uint32_t k = off & ~3u; k < off + n; k += 4)
    if (asyncField(k) && std::find(p.asyncWrites.begin(), p.asyncWrites.end(), k) == p.asyncWrites.end())
      p.asyncWrites.push_back(k);
}
}  // namespace
uint32_t Prod::r32(uint32_t off) const { return be32(d + off); }
uint64_t Prod::r64(uint32_t off) const { return be64(d + off); }
uint16_t Prod::r16(uint32_t off) const { return be16(d + off); }
void Prod::w32(uint32_t off, uint32_t v) {
  put32(d + off, v);
  noteAsync(*this, off, 4);
}
void Prod::w64(uint32_t off, uint64_t v) {
  put64(d + off, v);
  noteAsync(*this, off, 8);
}
void Prod::w8(uint32_t off, uint8_t v) {
  d[off] = v;
  noteAsync(*this, off, 1);
}

bool Prod::g32(uint32_t va, uint32_t& v) const {
  const uint8_t* h = sr::kernel::guestVAToHost(va);
  const uint8_t* e = sr::kernel::guestVAToHost(va + 3u);
  if (!h || e - h != 3) return false;
  v = be32(h);
  return true;
}
bool Prod::g16(uint32_t va, uint16_t& v) const {
  const uint8_t* h = sr::kernel::guestVAToHost(va);
  const uint8_t* e = sr::kernel::guestVAToHost(va + 1u);
  if (!h || e - h != 1) return false;
  v = be16(h);
  return true;
}
bool Prod::g8(uint32_t va, uint8_t& v) const {
  const uint8_t* h = sr::kernel::guestVAToHost(va);
  if (!h) return false;
  v = *h;
  return true;
}
bool Prod::gf32(uint32_t va, float& v) const {
  uint32_t u;
  if (!g32(va, u)) return false;
  std::memcpy(&v, &u, 4);
  return true;
}
void Prod::gw32(uint32_t va, uint32_t v) {
  uint8_t b[4];
  put32(b, v);
  gwBytes(va, b, 4);
}
void Prod::gwBytes(uint32_t va, const uint8_t* src, uint32_t n) {
  if (dry) {
    // p1.flush_residue: a placeholder allocation of the pre-pass past a transparent kick (Prod::alloc) - the live write
    // goes to the real allocation (command-buffer memory, mapped): nothing to check, nothing recorded.
    if (kicked && va >= kPlaceholderVA && va - kPlaceholderVA < phAlloc) return;
    // Recorded for the CHECK, not made; a write into the device span also lands in the copy (later reads see it);
    // an unmapped target stops here, so the record-mode pre-pass declines before the live pass could meet it.
    if (va >= dev && va + n <= dev + kDevSpan) {
      std::memcpy(d + (va - dev), src, n);
      noteAsync(*this, va - dev, n);
    }
    const uint8_t* h = sr::kernel::guestVAToHost(va);
    const uint8_t* e = sr::kernel::guestVAToHost(va + n - 1u);
    if (!h || uint32_t(e - h) != n - 1u) {
      stop("guest write unmapped");
      return;
    }
    if (recordGw) {
      gwrites.push_back(GW{va, uint32_t(gwData.size()), n});
      gwData.insert(gwData.end(), src, src + n);
    }
    return;
  }
  // Live: inside the device span the device bytes (d is the live device), else guest memory.
  if (va >= dev && va + n <= dev + kDevSpan) {
    std::memcpy(d + (va - dev), src, n);
    return;
  }
  uint8_t* h = sr::kernel::guestVAToHost(va);
  uint8_t* e = sr::kernel::guestVAToHost(va + n - 1u);
  if (!h || uint32_t(e - h) != n - 1u) {
    stop("guest write unmapped");
    return;
  }
  std::memcpy(h, src, n);
}

void Prod::t0(uint32_t reg, std::initializer_list<uint32_t> v) {
  ops.push_back(opT0(reg, uint32_t(v.size())));
  for (uint32_t x : v) ops.push_back(_byteswap_ulong(x));
}
void Prod::t0n(uint32_t reg, uint32_t n, const uint32_t* v) {
  ops.push_back(opT0(reg, n));
  for (uint32_t i = 0; i < n; i++) ops.push_back(_byteswap_ulong(v[i]));
}
void Prod::t0dev(uint32_t reg, uint32_t n, uint32_t devOff) {
  ops.push_back(opT0(reg, n));
  for (uint32_t i = 0; i < n; i++) {
    uint32_t w;
    std::memcpy(&w, d + devOff + 4u * i, 4);  // the shadow's bytes as they are (big-endian)
    ops.push_back(w);
  }
}
void Prod::t3(uint32_t opcode, bool pred, std::initializer_list<uint32_t> body) {
  ops.push_back(opT3(opcode, uint32_t(body.size()), pred));
  for (uint32_t x : body) ops.push_back(_byteswap_ulong(x));
}
void Prod::t3n(uint32_t opcode, bool pred, std::initializer_list<uint32_t> head, uint32_t n, const uint8_t* beTail) {
  ops.push_back(opT3(opcode, uint32_t(head.size()) + n, pred));
  for (uint32_t x : head) ops.push_back(_byteswap_ulong(x));
  for (uint32_t i = 0; i < n; i++) {
    uint32_t w;
    std::memcpy(&w, beTail + 4u * i, 4);
    ops.push_back(w);
  }
}
void Prod::pad(uint32_t n) {
  if (n) ops.push_back(kOpPad | n);
}

// ---- the op list: bytes, render, apply ----
uint32_t renderBytes(const uint32_t* w, uint32_t nWords) {
  uint32_t b = 0;
  for (uint32_t i = 0; i < nWords;) {
    const uint32_t h = w[i];
    if ((h & 0xFFFF0000u) == kOpPad) {  // PAD
      b += 4u * (h & 0xFFFFu);
      i += 1;
    } else if (h & 0x80000000u) {  // T3
      const uint32_t n = h & 0xFFFFu;
      b += 4u * (1u + n);
      i += 1 + n;
    } else {  // T0
      const uint32_t n = h >> 16;
      b += 4u * (1u + n);
      i += 1 + n;
    }
  }
  return b;
}

#if SR_ORACLE
void render(const uint32_t* w, uint32_t nWords, uint8_t* out) {
  for (uint32_t i = 0; i < nWords;) {
    const uint32_t h = w[i];
    if ((h & 0xFFFF0000u) == kOpPad) {
      for (uint32_t k = 0; k < (h & 0xFFFFu); k++, out += 4) put32(out, 0x80000000u);
      i += 1;
    } else if (h & 0x80000000u) {
      const uint32_t n = h & 0xFFFFu, op = (h >> 16) & 0xFFu, pred = (h >> 30) & 1u;
      put32(out, 0xC0000000u | (((n - 1u) & 0x3FFFu) << 16) | (op << 8) | pred);
      std::memcpy(out + 4, w + i + 1, 4u * n);
      out += 4u * (1u + n);
      i += 1 + n;
    } else {
      const uint32_t n = h >> 16, reg = h & 0x7FFFu;
      put32(out, ((n - 1u) << 16) | reg);
      std::memcpy(out + 4, w + i + 1, 4u * n);
      out += 4u * (1u + n);
      i += 1 + n;
    }
  }
}
#endif

bool apply(sr::gfx::CpEngine& eng, const uint32_t* w, uint32_t nWords) {
  for (uint32_t i = 0; i < nWords;) {
    const uint32_t h = w[i];
    if ((h & 0xFFFF0000u) == kOpPad) {
      i += 1;
      continue;
    }
    if (h & 0x80000000u) {
      const uint32_t n = h & 0xFFFFu, op = (h >> 16) & 0xFFu;
      const bool pred = (h >> 30) & 1u;
      if (i + 1 + n > nWords || !n) return false;
      const uint32_t* b = w + i + 1;
      auto B = [&](uint32_t k) { return _byteswap_ulong(b[k]); };
      g_cpOps.add();
      if (op == 0x36u || op == 0x22u) {  // DRAW_INDX_2 / DRAW_INDX: nativeDraw re-tests the bin predication
        if (op == 0x36u) {
          const uint32_t init = B(0);
          const bool dma = ((init >> 6) & 3u) == 0u && n >= 3;
          eng.nativeDraw(pred, init, dma ? B(1) : 0u, dma ? B(2) : 0u);
        } else {
          if (n < 2) return false;
          const uint32_t init = B(1);
          const bool dma = ((init >> 6) & 3u) == 0u && n >= 4;  // execute()'s 0x22 case
          eng.nativeDraw(pred, init, dma ? B(2) : 0u, dma ? B(3) : 0u);
        }
      } else if (pred && (eng.binSelect & eng.binMask) == 0) {
        eng.predSkipped++;
        g_cpSkippedPred.add();
      } else {
        switch (op) {
          case 0x46:  // EVENT_WRITE
            eng.applyEventWrite(B(0));
            break;
          case 0x2B:  // IM_LOAD_IMMEDIATE: shaderType, startSize, ucode
            if (n < 2) return false;
            eng.applyImLoadImmediate(B(0) & 0x3u, B(1), reinterpret_cast<const uint8_t*>(b + 2), n);
            break;
          case 0x60:
          case 0x61:
          case 0x62:
          case 0x63:  // SET_BIN_MASK / SELECT lo / hi
            eng.applyBinReg(op - 0x60u, B(0));
            break;
          case 0x3B:  // INVALIDATE_STATE: execute() discards the mask (Xenia too)
            g_cpNoEffect.add();
            break;
          case 0x3C:  // WAIT_REG_MEM {waitInfo, pollAddr, ref, mask, wait}: the CP's own body (cpMode: the poll and the
                      // COHER_STATUS_HOST handshake; else execute()'s instant consumption). The resolve's one wait is
                      // {3, 0xA31, 0, 0x80000000, 8} right after its own 0x0A31 write (bit 31 clear): satisfied at once.
            if (n < 5) return false;
            eng.applyWaitRegMem(reinterpret_cast<const uint8_t*>(b), n);
            break;
          default:
            return false;
        }
      }
      i += 1 + n;
      continue;
    }
    const uint32_t n = h >> 16, reg = h & 0x7FFFu;
    if (!n || i + 1 + n > nWords) return false;
    eng.nativeRun(reg, n, reinterpret_cast<const uint8_t*>(w + i + 1));
    g_cpOps.add();
    i += 1 + n;
  }
  return true;
}

// ---- the ring model ----
// The current piece ends at every XDK kick / allocator site. Dry: rendered into dryBytes and the cursor advances.
// Live (record mode): one opaque record item per piece through strand A's Entry (xdkBytes = the piece's exact XDK
// length), and the cursor re-read from the Entry at every kick / allocator site.
namespace {
sr::native::flush::Entry& liveEntry(Prod& p) { return *static_cast<sr::native::flush::Entry*>(p.entry); }
void endPiece(Prod& p) {
  if (p.ops.empty()) return;
  const uint32_t nb = renderBytes(p.ops.data(), uint32_t(p.ops.size()));
  if (p.dry) {
#if SR_ORACLE
    if (p.keepBytes) {
      const size_t at = p.dryBytes.size();
      p.dryBytes.resize(at + nb);
      render(p.ops.data(), uint32_t(p.ops.size()), p.dryBytes.data() + at);
    }
#endif
  } else if (p.mode == kModeLive) {
    if (!liveEntry(p).opaque(p.kind, p.ops.data(), uint32_t(p.ops.size()), nb, false)) p.stop("live: opaque refused");
    p.items++;
  }
  p.c += nb;
  p.bytes += nb;
  p.ops.clear();
}
}  // namespace

uint32_t Prod::cursor() const { return c + renderBytes(ops.data(), uint32_t(ops.size())); }
uint32_t Prod::limit() const { return mode == kModeLive ? static_cast<const sr::native::flush::Entry*>(entry)->limit() : r32(0x2C); }

bool Prod::kick() {
  if (stopped) return false;
  endPiece(*this);
  if (mode == kModeLive) {  // the Entry makes the XDK's test (and STRESS's) and kicks; the cursor follows it
    liveEntry(*this).kick();
    c = liveEntry(*this).cursor();
    return true;
  }
  if (c > r32(0x30)) {
    if (transparent) {  // p1.flush_residue: record mode's pre-pass under split looks through the kick (Prod in the hpp)
      kicked = true;
      return true;
    }
    stop("kick");  // dry CHECK: the guest body kicks here (the call is a skip); record mode: the entry declines
    return false;
  }
  return true;
}

bool Prod::flush() {
  if (stopped) return false;
  endPiece(*this);
  if (mode == kModeLive) {  // 82107058: 820FD8C0 whatever the threshold; the cursor follows the Entry
    liveEntry(*this).flush();
    c = liveEntry(*this).cursor();
    return true;
  }
  if (transparent) {
    kicked = true;
    return true;
  }
  stop("chunk kick");  // the dry CHECK compares nothing across a segment switch
  return false;
}

// 820FCDF0 GpuCmdBufAllocate (dry: on the device copy; live: the real call through the Entry). Two paths are modelled:
//   - dev+13140 == 0 (the game's own command buffer): 820FCBE8 GpuCmdBufReserveSpace mode 1, the tail carve from
//     dev+13496 (or limit + 60) upward, wrapping to dev+13504 past the buffer end dev+13488, refused while the Z-pass
//     recording pin dev+12944 / 12948 covers the range; then 820FCB30 GpuRingWaitForSpace, which returns at once when
//     the GPU progress word ([dev+0x2A10] + 4: generation & 3 | cursor) shows the range consumed, else waits (a stop:
//     the guest body waits instead);
//   - dev+13140 set with its +152 set: the user buffer's carve from dev+13496 downward.
// The callback path (dev+13140 set, +152 clear) stops.
uint32_t Prod::alloc(uint32_t dwords, uint32_t align) {
  if (stopped) return 0;
  endPiece(*this);
  if (mode == kModeLive) {  // the real 820FCDF0 between spans (the Entry publishes dev+0x28 first)
    const uint32_t va = liveEntry(*this).alloc(dwords, align);
    c = liveEntry(*this).cursor();
    return va;
  }
  if (kicked) {
    // p1.flush_residue: past a kick the pre-pass could not model the allocator state is unknown: assume the XDK's
    // allocation succeeds - the longest path, so every stop the live pass could meet is evaluated (its failure branch
    // returns or skips one draw and meets no other stop). A placeholder range; gwBytes does not mapping-check it.
    const uint32_t va = kPlaceholderVA + phAlloc;
    phAlloc += (4u * dwords + 15u) & ~15u;
    return va;
  }
  if (r8(10809) & 0x40u) return 0;  // the sticky failure byte: the XDK returns 0
  w32(0x28, c);                     // the XDK published the cursor before the call
  const uint32_t v6 = 4u * dwords;
  const uint32_t v7 = r32(13140);
  uint32_t result = 0;
  if (!v7) {
    // 820FCBE8(dev, 1, &v6, align), mode 1
    const uint32_t n4 = 4u * (v6 >> 2);
    uint32_t start = r32(13496);
    if (!start) start = r32(44) + 60u;
    uint32_t gen = r32(13500);
    const uint32_t m = ~(align - 1u);
    uint32_t v10 = (start + align - 1u) & m;
    uint32_t v11 = n4 + v10;
    bool refused = false;
    if (v11 > r32(13488)) {  // past the end of the buffer: wrap to dev+13504 (the next generation)
      const uint32_t pin = r32(12944);
      v10 = (r32(13504) + align - 1u) & m;
      v11 = n4 + v10;
      if ((pin && (r32(12948) == gen || (r32(12948) - gen == 1u && pin < v11))) || v11 > r32(13508)) {
        refused = true;
      } else {
        gen = gen + 1u;  // passed to 820FCB30 only (dev+13500 is not written here)
        w32(13504, v11);
      }
    } else {
      const uint32_t pin = r32(12944);
      if (pin) {
        const uint32_t d = r32(12948) - gen;
        if (d == 0xFFFFFFFFu || (d == 0u && pin < v11)) refused = true;
      }
      if (!refused) w32(13496, v11);
    }
    if (!refused) {
      // 820FCB30(dev, v11, gen): no wait when ((gen - progress) & 3) == 0, or == 1 and v11 <= progress & ~3.
      const uint32_t fb = r32(0x2A10);
      uint32_t prog = 0;
      if (!fb || !g32(fb + 4u, prog)) {
        stop("alloc: fence block unmapped");
        return 0;
      }
      const uint32_t dg = (gen - prog) & 3u;
      if (dg != 0u && (dg != 1u || v11 > (prog & ~3u))) {
        // p1.flush_residue (SR_NATIVE_FLUSH_SPLIT, record mode's pre-pass only - `transparent`): 820FCB30 waits for the
        // GPU and returns; the allocation is v10 either way (Prod in the hpp). The dry CHECK and SR_NATIVE_FLUSH_SPLIT=0
        // (the A/B; the default since the 2026-10-14 bake is ON) stop.
        // A timing callback (dev+13068, called by the wait's report 820F3328) is guest code the pre-pass cannot model.
        if (!transparent || r32(13068)) {
          stop("alloc would wait for the GPU");
          return 0;
        }
        waited = true;
      }
      result = v10;
    }
  } else {
    uint32_t v9h = 0;
    if (!g32(v7 + 152u, v9h)) {
      stop("alloc unmapped");
      return 0;
    }
    if (!v9h) {
      stop("alloc via the device callback");
      return 0;
    }
    uint32_t v8 = r32(13496);
    if (!v8) {
      uint32_t a156 = 0;
      if (!g32(v7 + 156u, a156)) {
        stop("alloc unmapped");
        return 0;
      }
      v8 = (a156 & 0xFFFFFFFCu) + v9h;
    }
    const uint32_t v9 = v8 - 4u * (v6 >> 2);
    const uint32_t v10 = v9 & ~(align - 1u);
    if (v10 >= r32(40) + 164u) {
      w32(13496, v10);
      const uint32_t v12 = uint32_t(4 * (int32_t(v8 - v10) >> 2));
      w32(44, r32(44) - v12);
      w32(48, r32(48) - v12);
      result = v10;
    }
  }
  // 820FCF24: the allocation total and the early segment close.
  const uint32_t v17 = r32(13516);
  const uint32_t v18 = r32(13512) + v6;
  w32(13512, v18);
  if (v18 > v17 && !r32(12944)) {
    const uint32_t v21 = r32(40);
    w32(48, v21);
    w32(44, v21 + 160u);
    if (r32(13504) != r32(13484)) w32(13496, r32(13488));
  }
  if (!result) w8(10809, uint8_t(r8(10809) | 0x40u));
  return result;
}

void Prod::reset() {
  ctx = nullptr;
  base = nullptr;
  dev = 0;
  d = nullptr;
  dry = false;
  mode = 0;
  entry = nullptr;
  stopped = false;
  why = nullptr;
  split = transparent = kicked = waited = false;
  phAlloc = 0;
  chunks = 0;
  gwrites.clear();
  gwData.clear();
  recordGw = true;
  keepBytes = true;
  asyncWrites.clear();
  ops.clear();
  kind = 0;
  bytes = 0;
  items = 0;
  dryBytes.clear();
  c0 = c = 0;
}

bool Prod::finish() {
  endPiece(*this);
  if (dry) w32(0x28, c);  // live: the Entry owns dev+0x28 (close() writes the span)
  return !stopped;
}

// ---- shared XDK building blocks ----
bool regRuns(Prod& p, uint64_t mask, uint32_t reg, uint32_t srcOff) {
  uint32_t r = reg, s = srcOff;
  uint64_t m = mask;
  while (m) {
    const uint32_t lz = uint32_t(__builtin_clzll(m));
    r += lz;
    s += 4u * lz;
    m <<= lz;
    const uint32_t rn = (~m) ? uint32_t(__builtin_clzll(~m)) : 64u;
    if (!(p.cursor() + 4u * rn < p.limit())) {  // 82107058's chunked path across a segment switch
      if (!p.split) {
        p.stop("run crosses the segment limit");
        return false;
      }
      // p1.flush_residue: 82107058 (1-dword elements) - the registers that fit, ((limit - cursor) >> 2) - 1 (signed),
      // as their own type-0 packet with no pad, then 820FD8C0 unconditionally and the rest from the new cursor.
      p.chunks++;
      uint32_t left = rn;
      for (;;) {
        const int32_t avail = (int32_t(p.limit() - p.cursor()) >> 2) - 1;
        if (avail > 0) {
          const uint32_t k = uint32_t(avail) < left ? uint32_t(avail) : left;
          p.t0dev(r, k, s);
          r += k;
          s += 4u * k;
          left -= k;
        }
        if (!left) break;
        if (!p.flush()) return false;
        if (p.mode != kModeLive && p.kicked) {  // the pre-pass past a transparent kick: the new segment is unknown
          p.t0dev(r, left, s);
          r += left;
          s += 4u * left;
          left = 0;
          break;
        }
      }
      m = rn >= 64u ? 0ull : (m << rn);
      continue;
    }
    p.t0dev(r, rn, s);
    r += rn;
    s += 4u * rn;
    m = rn >= 64u ? 0ull : (m << rn);
  }
  return true;
}

// The predicated-tiling test 820F0208 / 821072F8 make (byte 0x2A38 & 0x10, or & 0x20 with every RT / DS slot equal to
// its tile slot or empty).
static bool tiledState(const Prod& p) {
  const uint8_t b = p.r8(0x2A38);
  if (b & 0x10u) return true;
  if (!(b & 0x20u)) return false;
  for (uint32_t k = 0; k < 5; k++) {
    const uint32_t a = p.r32(0x2F88u + 4u * k), t = p.r32(0x30A4u + 4u * k);
    if (t != a && a != 0) return false;
  }
  return true;
}

bool hizWrite(Prod& p, uint32_t hiz) {
  // 821072F8: kick check, then (byte 0x2A3B & 0x20 and the tiling test) the tiled per-tile form, else {0x2203, hiz}.
  if (!p.kick()) return false;
  if ((p.r8(0x2A3B) & 0x20u) && tiledState(p)) {
    p.stop("tiled 0x2203");
    return false;
  }
  p.t0(0x2203u, {hiz});
  return true;
}

bool windowScissor(Prod& p, uint32_t l, uint32_t t, uint32_t r, uint32_t b) {
  if (!p.kick()) return false;
  if (tiledState(p)) {
    p.stop("tiled scissor");
    return false;
  }
  p.t0(0x2080u, {0u, ((t & 0x3FFFu) << 16) | (l & 0x3FFFu), ((b & 0x3FFFu) << 16) | (r & 0x3FFFu)});
  return true;
}

void defaultBlock(Prod& p) {
  p.t0(0x2100u, {0xFFFFu, 0u, 0u});
  p.t0(0x2204u, {0x10000u, 0x10000u, 0x300u});
  p.t0(0x2312u, {0xFFFFu});
  p.t0(0x200Du, {0u});
  p.t3(0x60u, false, {p.r32(0x3090)});
  uint64_t q18 = p.r64(0x18);
  q18 |= 0x400000ull;
  q18 |= 0x200000ull;
  p.w64(0x18, q18);
  uint64_t q10 = p.r64(0x10);
  q10 |= 0x80ull | 0x40ull | 0x20ull;
  p.w64(0x10, q10);
  p.w64(0x20, p.r64(0x20) | 0x80000ull);
  p.w64(0x18, p.r64(0x18) | 0x2000000ull);
}

// Phase C (Resolve 0x8210CA58; ClearImpl / ClearFullSurface share it - their ports say if not).
bool phaseC(Prod& p) {
  uint64_t q10 = p.r64(0x10);
  if (q10) {
    if (q10 & 0x100ull) {
      if (!hizWrite(p, p.r32(0x28C0))) return false;
      // 821072F8 returned q10 & ~0x100; the caller ORs it back into the (unchanged) q10: bit 0x100 stays set.
      p.w64(0x10, p.r64(0x10) | (q10 & ~0x100ull));
    }
    q10 = p.r64(0x10);
    if (q10 & 0xFFFull) {
      if (!regRuns(p, q10 << 52, 0x2200u, 0x28B4u)) return false;
      p.w64(0x10, p.r64(0x10) & ~0xFFFull);
    }
    q10 = p.r64(0x10);
    if (q10 & (0x1Full << 44)) {
      if (!regRuns(p, rotl64(q10, 15) & (0x1Full << 59), 0x2180u, 0x28A0u)) return false;
      p.w64(0x10, p.r64(0x10) & ~(0x1Full << 44));
    }
  }
  uint64_t q18 = p.r64(0x18);
  if (q18) {
    if (q18 & 0x7FFFFCull) {
      if (!regRuns(p, rotl64(q18, 41) & (0x1FFFFFull << 43), 0x2100u, 0x284Cu)) return false;
      p.w64(0x18, p.r64(0x18) & 0xFFFFFFFFFF800003ull);
    }
    q18 = p.r64(0x18);
    if (q18 & 0x7FFF800000ull) {
      if (!regRuns(p, rotl64(q18, 25) & (0xFFFFull << 48), 0x2000u, 0x2800u)) return false;
      p.w64(0x18, p.r64(0x18) & ~(0xFFFFull << 23));
    }
    q18 = p.r64(0x18);
    if (q18 & (0x1FFFFFull << 39)) {
      if (!regRuns(p, rotl64(q18, 4) & (0x1FFFFFull << 43), 0x2280u, 0x28E4u)) return false;
      p.w64(0x18, p.r64(0x18) & ~(0x1FFFFFull << 39));
    }
  }
  const uint64_t q20 = p.r64(0x20);
  if (q20 && (q20 & 0x3FFFFFFFFFull)) {
    if (!regRuns(p, q20 << 26, 0x2300u, 0x2938u)) return false;
    p.w64(0x20, p.r64(0x20) & ~0x3FFFFFFFFFull);
  }
  return true;
}

// ---- the entry ports ----
#include "nb_rec_resolve.inc"
#include "nb_rec_clear.inc"
#include "nb_rec_gpr.inc"

namespace {
using PortFn = void (*)(Prod&);
const PortFn kPort[kEntries] = {portResolve, portClearF, portGpr};

#if SR_ORACLE
// ---- the dry CHECK ----
struct CheckTls {
  bool armed = false;
  int entry = 0;
  uint32_t dev = 0, c0 = 0, lim0 = 0;
  uint8_t copy[kDevSpan];
  std::vector<uint8_t> bytes;
  std::vector<Prod::GW> gw;
  std::vector<uint8_t> gwData;
  std::vector<uint32_t> wroteAsync;
};
CheckTls& ckTls() {
  thread_local CheckTls* t = new CheckTls();
  return *t;
}

void example(int entry, const char* what, uint32_t dev, uint32_t c0, uint32_t at, uint32_t nv, uint32_t gv,
             uint32_t nlen, uint32_t glen) {
  if (g_examples.fetch_add(1, std::memory_order_relaxed) >= 48) return;
  SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[NB_REC_BAD] %s %s dev=%08X c0=%08X at +%u native=%08X guest=%08X native_len=%u guest_len=%u",
              kEntryName[entry], what, dev, c0, at, nv, gv, nlen, glen);
}

void report(bool force) {
  static std::atomic<uint64_t> s_n{0};
  const uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!force && n != 64 && (n & 0x3FFFu) != 0) return;
  for (int e = 0; e < kEntries; e++) {
    if (!g_ckCalls[e].get()) continue;
    std::string why;
    {
      std::lock_guard<std::mutex> lk(g_stopMu);
      char b[96];
      for (auto& p : g_stopWhy[e]) {
        std::snprintf(b, sizeof b, " %s=%llu", p.first.c_str(), (unsigned long long)p.second);
        why += b;
      }
    }
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[NB_REC] check %s: calls=%llu compared=%llu ok=%llu bad: len=%llu bytes=%llu device=%llu mem=%llu "
                "guest-kick=%llu | stopped=%llu:%s | compared bytes=%llu | fmt6 packs=%llu nonzero=%llu",
                kEntryName[e], (unsigned long long)g_ckCalls[e].get(), (unsigned long long)g_ckFull[e].get(),
                (unsigned long long)g_ckOk[e].get(), (unsigned long long)g_ckBadLen[e].get(),
                (unsigned long long)g_ckBadBytes[e].get(), (unsigned long long)g_ckBadDev[e].get(),
                (unsigned long long)g_ckBadMem[e].get(), (unsigned long long)g_ckGuestKick[e].get(),
                (unsigned long long)g_ckStop[e].get(), why.c_str(), (unsigned long long)g_ckBytes[e].get(),
                (unsigned long long)g_fmt6Packs.get(), (unsigned long long)g_fmt6NonZero.get());
  }
}

struct ExitReport {
  ~ExitReport() {
    if (g_check) report(true);
  }
} g_exitReport;
#endif  // SR_ORACLE

}  // namespace

bool on() {
  // The in-roll A/B (SR_AB_TOGGLE=SR_NB_REC) flips it per window / per present; each entry decides per call, so a
  // flip between two calls is exact.
  static const int s_ab = sr::abt::reg("SR_NB_REC");
  return g_onEnv && (s_ab < 0 || sr::abt::on(s_ab));
}
#if SR_ORACLE
bool checkOn() { return g_check; }
#endif
bool entryOn(int entry) { return (g_kinds >> entry) & 1u; }
#if SR_ORACLE
bool checkArmed() { return g_check && ckTls().armed; }

void checkBefore(int entry, PPCContext& ctx, uint8_t* base) {
  CheckTls& t = ckTls();
  t.armed = false;
  if (!g_check || !entryOn(entry)) return;
  const uint32_t dev = ctx.r3.u32;
  uint8_t* h = sr::kernel::guestVAToHost(dev);
  uint8_t* e = sr::kernel::guestVAToHost(dev + kDevSpan - 1u);
  if (!h || e - h != ptrdiff_t(kDevSpan - 1u)) return;
  g_ckCalls[entry].add();
  // ONE read of the live device. The async block's port-written dwords come from the port's own writes
  // (Prod::asyncWrites), never from diffing two reads: job45-47's ~0.1% DEVICE BADs were exactly that - LLVM's memcpy
  // forwarding turned memcpy(copy, before) into a second read of the live device, racing the pump / ISR writers
  // (job47 detail: before=00000000 native=40000000 live=00000000 at +10852, a value no port writes).
  std::memcpy(t.copy, h, kDevSpan);
  thread_local Prod p;
  p.reset();
  p.ctx = &ctx;
  p.base = base;
  p.dev = dev;
  p.d = t.copy;
  p.dry = true;
  p.mode = kModeDry;
  p.c0 = p.c = be32(t.copy + 0x28);
  // The port reads its arguments from ctx and writes nothing outside the copy (guest writes are recorded).
  kPort[entry](p);
  if (!p.stopped) p.finish();
  if (p.stopped) {
    g_ckStop[entry].add();
    noteStop(entry, p.why ? p.why : "?");
    report(false);
    return;
  }
  t.entry = entry;
  t.dev = dev;
  t.c0 = p.c0;
  t.bytes.swap(p.dryBytes);
  t.gw.swap(p.gwrites);
  t.gwData.swap(p.gwData);
  t.wroteAsync.swap(p.asyncWrites);
  t.armed = true;
}

void checkAfter(int entry, PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
  CheckTls& t = ckTls();
  if (!t.armed || t.entry != entry) return;
  t.armed = false;
  uint8_t* h = sr::kernel::guestVAToHost(t.dev);
  if (!h) return;
  const uint32_t c1 = be32(h + 0x28);
  const uint32_t nlen = uint32_t(t.bytes.size());
  if (c1 < t.c0) {  // the guest switched segments although the dry run predicted no kick
    g_ckGuestKick[entry].add();
    example(entry, "GUEST-KICK", t.dev, t.c0, 0, 0, 0, nlen, 0);
    report(false);
    return;
  }
  g_ckFull[entry].add();
  bool bad = false;
  const uint32_t glen = c1 - t.c0;
  if (glen != nlen) {
    g_ckBadLen[entry].add();
    example(entry, "LEN", t.dev, t.c0, 0, 0, 0, nlen, glen);
    bad = true;
  }
  const uint32_t cmp = std::min(nlen, glen);
  if (cmp) {
    const uint8_t* g = sr::kernel::guestVAToHost(t.c0 + 4u);
    const uint8_t* ge = sr::kernel::guestVAToHost(t.c0 + 4u + cmp - 1u);
    if (g && ge - g == ptrdiff_t(cmp) - 1 && std::memcmp(g, t.bytes.data(), cmp) != 0) {
      uint32_t at = 0;
      while (at + 4 <= cmp && std::memcmp(g + at, t.bytes.data() + at, 4) == 0) at += 4;
      g_ckBadBytes[entry].add();
      example(entry, "BYTES", t.dev, t.c0, at, be32(t.bytes.data() + at), be32(g + at), nlen, glen);
      bad = true;
    }
  }
  {
    uint32_t nd = 0, first = 0;
    for (uint32_t k = 0; k < kDevSpan; k += 4) {
      const bool cmpIt = !asyncField(k) ||
                         std::find(t.wroteAsync.begin(), t.wroteAsync.end(), k) != t.wroteAsync.end();
      if (cmpIt && std::memcmp(t.copy + k, h + k, 4) != 0) {
        if (!nd) first = k;
        nd++;
      }
    }
    if (nd) {
      g_ckBadDev[entry].add();
      example(entry, "DEVICE", t.dev, t.c0, first, be32(t.copy + first), be32(h + first), nd, 0);
      // The first differing dwords in full: offset, the snapshot, the port's copy, the live device after the guest
      // body, and whether the offset was taken as port-written (wroteAsync) - so a DEVICE BAD names its own cause.
      if (g_examples.load(std::memory_order_relaxed) <= 48) {
        char b[512];
        int o = std::snprintf(b, sizeof b, "[NB_REC_BAD] %s DEVICE detail wa=%zu:", kEntryName[entry], t.wroteAsync.size());
        uint32_t shown = 0;
        for (uint32_t k = 0; k < kDevSpan && shown < 4 && o > 0 && o < int(sizeof b) - 80; k += 4) {
          const bool wa = std::find(t.wroteAsync.begin(), t.wroteAsync.end(), k) != t.wroteAsync.end();
          const bool cmpIt = !asyncField(k) || wa;
          if (!cmpIt || std::memcmp(t.copy + k, h + k, 4) == 0) continue;
          o += std::snprintf(b + o, sizeof b - o, " +%u native=%08X live=%08X%s", k, be32(t.copy + k), be32(h + k),
                             wa ? " wa" : "");
          shown++;
        }
        SR_LOG_BOOT(sr::LogSeverity::Warn, "%s", b);
      }
      bad = true;
    }
  }
  for (const Prod::GW& w : t.gw) {
    const uint8_t* g = sr::kernel::guestVAToHost(w.va);
    const uint8_t* nb = t.gwData.data() + w.off;
    if (!g || std::memcmp(g, nb, w.n) != 0) {
      g_ckBadMem[entry].add();
      uint32_t gv = 0, nv = 0;
      if (g && w.n >= 4) gv = be32(g);
      if (w.n >= 4) nv = be32(nb);
      example(entry, "MEM", t.dev, t.c0, w.va, nv, gv, w.n, 0);
      bad = true;
      break;
    }
  }
  if (!bad) {
    g_ckOk[entry].add();
    g_ckBytes[entry].add(nlen);
  }
  report(false);
}
#endif  // SR_ORACLE (the dry CHECK)

// ---- record mode (SR_NB_REC) ----
// Per call: (1) the pre-pass - the port dry on a device copy, exactly the CHECK's run. Any stop (a path not modelled,
// or a kick site that would kick: the segment boundary, P2-B's business) declines the WHOLE entry before anything is
// written: the guest body runs, counted per entry and cause. (2) The live pass on the real device through strand A's
// Entry: one opaque record item per piece between the XDK's kick / allocator sites, kick() and alloc() at those sites.
// The pre-pass is exact for the same device state, so the live pass meets no stop; under SR_NATIVE_SEG_STRESS the
// Entry may kick where the XDK would not, which the live pass follows through Entry::cursor() and the real 820FCDF0.
namespace {
struct PreTls {
  uint8_t copy[kDevSpan];
};
PreTls& preTls() {
  thread_local PreTls* t = new PreTls();
  return *t;
}
std::mutex g_recStopMu;
std::vector<std::pair<std::string, uint64_t>> g_recStopWhy[kEntries];
void noteRecStop(int entry, const char* why) {
  std::lock_guard<std::mutex> lk(g_recStopMu);
  for (auto& p : g_recStopWhy[entry])
    if (p.first == why) {
      p.second++;
      return;
    }
  if (g_recStopWhy[entry].size() < 32) g_recStopWhy[entry].emplace_back(why, 1);
}

void recReport(bool force) {
  static std::atomic<uint64_t> s_n{0};
  const uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!force && n != 64 && (n & 0x3FFFu) != 0) return;
  double usPre = 0, usOpen = 0, usLive = 0;
  {
    const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                                 g_clk0).count());
    const double tpu = ns > 0 ? double(__rdtsc() - g_tsc0) / (ns / 1000.0) : 0.0;  // TSC ticks per microsecond
    uint64_t calls = 0, taken = 0;
    for (int e = 0; e < kEntries; e++) {
      calls += g_recCalls[e].get();
      taken += g_recTaken[e].get();
    }
    if (tpu > 0 && calls) usPre = double(g_recPreTsc.get()) / tpu / double(calls);
    if (tpu > 0 && taken) {
      usOpen = double(g_recOpenTsc.get()) / tpu / double(taken);
      usLive = double(g_recLiveTsc.get()) / tpu / double(taken);
    }
  }
  for (int e = 0; e < kEntries; e++) {
    if (!g_recCalls[e].get()) continue;
    std::string why;
    {
      std::lock_guard<std::mutex> lk(g_recStopMu);
      char b[96];
      for (auto& p : g_recStopWhy[e]) {
        std::snprintf(b, sizeof b, " %s=%llu", p.first.c_str(), (unsigned long long)p.second);
        why += b;
      }
    }
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[NB_REC] rec %s: calls=%llu taken=%llu items=%llu bytes=%llu | declined open=%llu pre=%llu:%s | "
                "live-stop=%llu | cp applied=%llu bad=%llu ops=%llu pred-skipped=%llu no-effect=%llu render-len-bad=%llu | "
                "us/call (all entries) pre=%.2f open=%.2f live=%.2f | split kickT=%llu chunks=%llu kickTStop=%llu "
                "waitT=%llu",
                kEntryName[e], (unsigned long long)g_recCalls[e].get(), (unsigned long long)g_recTaken[e].get(),
                (unsigned long long)g_recItems[e].get(), (unsigned long long)g_recBytes[e].get(),
                (unsigned long long)g_recDeclOpen[e].get(), (unsigned long long)g_recDeclPre[e].get(), why.c_str(),
                (unsigned long long)g_recLiveStop[e].get(), (unsigned long long)g_cpApplied.get(),
                (unsigned long long)g_cpBad.get(), (unsigned long long)g_cpOps.get(),
                (unsigned long long)g_cpSkippedPred.get(), (unsigned long long)g_cpNoEffect.get(),
                (unsigned long long)g_cpRenderLenBad.get(), usPre, usOpen, usLive,
                (unsigned long long)g_recKickT[e].get(), (unsigned long long)g_recChunks[e].get(),
                (unsigned long long)g_recKickTStop[e].get(), (unsigned long long)g_recWaitT[e].get());
  }
}
struct RecExitReport {
  ~RecExitReport() {
    if (g_onEnv) recReport(true);
  }
} g_recExitReport;

const uint32_t kKindOf[kEntries] = {sr::native::flush::kRkResolve, sr::native::flush::kRkClear,
                                    sr::native::flush::kRkClearFull};

bool runRecord(int entry, PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  uint8_t* h = sr::kernel::guestVAToHost(dev);
  uint8_t* he = sr::kernel::guestVAToHost(dev + kDevSpan - 1u);
  if (!h || he - h != ptrdiff_t(kDevSpan - 1u)) return false;
  g_recCalls[entry].add();
  const uint64_t t0 = __rdtsc();
  // p1.flush_residue: SR_NATIVE_FLUSH_SPLIT's window, decided once for the entry (both passes)
  const bool split = sr::native::flush::flushSplitActive();
  bool preKicked = false, preWaited = false;
  {  // (1) the pre-pass: no ring bytes rendered, no guest writes kept (only their mapping is checked)
    PreTls& t = preTls();
    std::memcpy(t.copy, h, kDevSpan);
    thread_local Prod p;
    p.reset();
    p.ctx = &ctx;
    p.base = base;
    p.dev = dev;
    p.d = t.copy;
    p.dry = true;
    p.mode = kModeDry;
    p.keepBytes = false;
    p.recordGw = false;
    p.split = p.transparent = split;
    p.c0 = p.c = be32(t.copy + 0x28);
    kPort[entry](p);
    if (!p.stopped) p.finish();
    if (p.stopped) {
      g_recDeclPre[entry].add();
      noteRecStop(entry, p.why ? p.why : "?");
      g_recPreTsc.add(__rdtsc() - t0);
      recReport(false);
      return false;
    }
    preKicked = p.kicked;
    preWaited = p.waited;
  }
  const uint64_t t1 = __rdtsc();
  g_recPreTsc.add(t1 - t0);
  // (2) the live pass
  sr::native::flush::Entry e(ctx, base, dev);
  if (!e.open()) {
    g_recDeclOpen[entry].add();
    recReport(false);
    return false;
  }
  const uint64_t t2 = __rdtsc();
  g_recOpenTsc.add(t2 - t1);
  thread_local Prod p;
  p.reset();
  p.ctx = &ctx;
  p.base = base;
  p.dev = dev;
  p.d = h;
  p.dry = false;
  p.mode = kModeLive;
  p.entry = &e;
  p.kind = kKindOf[entry];
  p.split = split;  // the live pass chunks as the pre-pass did; its kicks are the Entry's real ones (never transparent)
  p.c0 = p.c = e.cursor();
  kPort[entry](p);
  p.finish();
  if (p.stopped) {  // not expected (the pre-pass is exact); the items written so far stand - counted, never silent
    g_recLiveStop[entry].add();
    if (preKicked) g_recKickTStop[entry].add();
    noteRecStop(entry, p.why ? p.why : "live ?");
  }
  e.close();
  g_recLiveTsc.add(__rdtsc() - t2);
  g_recTaken[entry].add();
  g_recItems[entry].add(p.items);
  g_recBytes[entry].add(p.bytes);
  if (preKicked) g_recKickT[entry].add();
  if (preWaited) g_recWaitT[entry].add();
  g_recChunks[entry].add(p.chunks);
  recReport(false);
  return true;
}

// The CP side of kinds 7..9 (the payload is the op list, the same for the three entries).
bool applyOpaque(uint32_t kind, const uint32_t* w, uint32_t n, sr::gfx::CpEngine& eng) {
  (void)kind;
  const bool ok = apply(eng, w, n);
  (ok ? g_cpApplied : g_cpBad).add();
  return ok;
}
#if SR_ORACLE
void renderOpaque(uint32_t kind, const uint32_t* w, uint32_t n, uint8_t* out, uint32_t bytes) {
  (void)kind;
  if (renderBytes(w, n) != bytes) {  // never expected (the CHECK proves the lengths): NOPs, counted
    for (uint32_t k = 0; k + 4u <= bytes; k += 4u) put32(out + k, 0x80000000u);
    g_cpRenderLenBad.add();
    return;
  }
  render(w, n, out);
}
#endif
struct RegisterAtInit {
  RegisterAtInit() { registerHandlers(); }
} g_registerAtInit;
}  // namespace

#if SR_ORACLE
namespace {
struct NanWatch {
  uint32_t phys[8] = {};
  uint32_t len[8] = {};   // 0 = one 32-bit word; else a byte range scanned per present
  bool half[8] = {};      // the range holds big-endian 16-bit floats (count exponent-31 halves: host inf / NaN)
  int n = 0;
  int traceLeft = 0;
};
const NanWatch& nanWatchCfg() {
  static const NanWatch w = [] {
    NanWatch c;
    if (const char* e = std::getenv("SR_NB_NANWATCH")) {
      // ADDR[/LEN[h]] entries (hex), ':'-separated: a word, or a range of float32 (or, with h, float16) values.
      const char* s = e;
      while (*s && c.n < 8) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(s, &end, 16);
        if (end == s) break;
        c.phys[c.n] = uint32_t(v) & 0x1FFFFFFFu;
        if (*end == '/') {
          char* e2 = nullptr;
          c.len[c.n] = uint32_t(std::strtoul(end + 1, &e2, 16)) & ~3u;
          end = e2;
          if (*end == 'h') { c.half[c.n] = true; end++; }
        }
        c.n++;
        if (*end != ':') break;
        s = end + 1;
      }
    }
    if (const char* t = std::getenv("SR_NB_NANWATCH_TRACE")) c.traceLeft = std::atoi(t);
    return c;
  }();
  return w;
}
std::atomic<uint64_t> g_nwPresent{~0ull};
std::mutex g_nwMu;
uint32_t g_nwLast[8] = {};
bool g_nwSeen[8] = {};
int g_nwTraceLeft = -1;
uint64_t g_nwLastTrace = 0;
bool nwFinite(uint32_t v) { return (v & 0x7F800000u) != 0x7F800000u; }
}  // namespace

void nanWatchTick() {
  const NanWatch& w = nanWatchCfg();
  if (!w.n) return;
  // The guest's VdSwap count (kernel/stubs.cpp). sr::gfx::hle::presentCount() stays 0 on the PM4 path (job60: every
  // line read "present 0").
  const uint64_t pc = sr::kernel::getVdSwapCount();
  uint64_t prev = g_nwPresent.load(std::memory_order_relaxed);
  if (prev == pc || !g_nwPresent.compare_exchange_strong(prev, pc, std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lk(g_nwMu);
  if (g_nwTraceLeft < 0) g_nwTraceLeft = w.traceLeft;
  bool anyBad = false;
  char vals[8 * 40];
  int vp = 0;
  for (int i = 0; i < w.n; i++) {
    const uint32_t bytes = w.len[i] ? w.len[i] : 4u;
    const uint8_t* h = sr::kernel::guestVAToHost(0xA0000000u + w.phys[i]);
    const uint8_t* e = sr::kernel::guestVAToHost(0xA0000000u + w.phys[i] + bytes - 1u);
    if (!h || e - h != ptrdiff_t(bytes - 1u)) continue;  // unmapped: fault-free skip
    if (!w.len[i]) {
      const uint32_t v = be32(h);
      float f;
      std::memcpy(&f, &v, 4);
      if (!nwFinite(v)) anyBad = true;
      if (!g_nwSeen[i] || nwFinite(v) != nwFinite(g_nwLast[i]))
        SR_LOG_BOOT(sr::LogSeverity::Info, "[NB_NANWATCH] present %llu phys %08X = %08X (%g)%s",
                    (unsigned long long)pc, w.phys[i], v, double(f),
                    !g_nwSeen[i] ? " first read" : (nwFinite(v) ? " -> FINITE again" : " -> NON-FINITE"));
      g_nwSeen[i] = true;
      g_nwLast[i] = v;
      if (vp < int(sizeof(vals)) - 32) vp += std::snprintf(vals + vp, sizeof(vals) - size_t(vp), " %08X=%08X", w.phys[i], v);
      continue;
    }
    // A range: count the non-finite values (float32 exponent 255, or float16 exponent 31 = host inf / NaN), and the
    // +inf ones separately (0x7F800000 / 0x7C00: the value a log / sum overflow carries forward).
    uint32_t bad = 0, pinf = 0, firstOff = ~0u;
    if (w.half[i]) {
      for (uint32_t o = 0; o + 2u <= bytes; o += 2u) {
        const uint32_t x = (uint32_t(h[o]) << 8) | h[o + 1u];
        if ((x & 0x7C00u) == 0x7C00u) {
          bad++;
          if (x == 0x7C00u) pinf++;
          if (firstOff == ~0u) firstOff = o;
        }
      }
    } else {
      for (uint32_t o = 0; o + 4u <= bytes; o += 4u) {
        const uint32_t x = be32(h + o);
        if (!nwFinite(x)) {
          bad++;
          if (x == 0x7F800000u) pinf++;
          if (firstOff == ~0u) firstOff = o;
        }
      }
    }
    if (!g_nwSeen[i] || bad != g_nwLast[i])
      SR_LOG_BOOT(sr::LogSeverity::Info, "[NB_NANWATCH] present %llu range %08X/%X%s: %u non-finite (%u +inf), first at +%X%s",
                  (unsigned long long)pc, w.phys[i], bytes, w.half[i] ? "h" : "", bad, pinf,
                  firstOff == ~0u ? 0u : firstOff, !g_nwSeen[i] ? " (first read)" : "");
    g_nwSeen[i] = true;
    g_nwLast[i] = bad;
    if (vp < int(sizeof(vals)) - 32) vp += std::snprintf(vals + vp, sizeof(vals) - size_t(vp), " %08X:%u", w.phys[i], bad);
  }
  if (pc % 600 == 0) SR_LOG_BOOT(sr::LogSeverity::Info, "[NB_NANWATCH] present %llu:%s", (unsigned long long)pc, vals);
  if (anyBad && g_nwTraceLeft > 0 && pc >= g_nwLastTrace + 2) {
    g_nwTraceLeft--;
    g_nwLastTrace = pc;
    sr::gfx::livetrace::requestCapture();
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NB_NANWATCH] present %llu: live trace requested (%d left)",
                (unsigned long long)pc, g_nwTraceLeft);
  }
}
#endif  // SR_ORACLE (the NaN watch)

bool resolve(PPCContext& ctx, uint8_t* base) { return runRecord(kResolve, ctx, base); }
bool clearF(PPCContext& ctx, uint8_t* base) { return runRecord(kClearF, ctx, base); }
bool gprAlloc(PPCContext& ctx, uint8_t* base) { return runRecord(kGpr, ctx, base); }

void registerHandlers() {
  namespace nf = sr::native::flush;
#if SR_ORACLE
  nf::setOpaqueHandler(nf::kRkResolve, applyOpaque, renderOpaque);
#else
  nf::setOpaqueHandler(nf::kRkResolve, applyOpaque);
#endif
#if SR_ORACLE
  nf::setOpaqueHandler(nf::kRkClear, applyOpaque, renderOpaque);
#else
  nf::setOpaqueHandler(nf::kRkClear, applyOpaque);
#endif
#if SR_ORACLE
  nf::setOpaqueHandler(nf::kRkClearFull, applyOpaque, renderOpaque);
#else
  nf::setOpaqueHandler(nf::kRkClearFull, applyOpaque);
#endif
}

void recCounters(int entry, uint64_t out[kRecCounters]) {
  const bool ok = entry >= 0 && entry < kEntries;
  out[8] = ok ? g_recWaitT[entry].get() : 0u;
  out[0] = ok ? g_recCalls[entry].get() : 0u;
  out[1] = ok ? g_recTaken[entry].get() : 0u;
  out[2] = ok ? g_recKickT[entry].get() : 0u;
  out[3] = ok ? g_recChunks[entry].get() : 0u;
  out[4] = ok ? g_recKickTStop[entry].get() : 0u;
  out[5] = ok ? g_recLiveStop[entry].get() : 0u;
  out[6] = ok ? g_recDeclOpen[entry].get() : 0u;
  out[7] = ok ? g_recDeclPre[entry].get() : 0u;
}

}  // namespace sr::native::recb

// The XDK's SetShaderGPRAllocation (IDA: GpuRingEmitRegisterBlock_sub_820F5570) and its tail call
// D3DDeviceClearFullSurface 82100488 - the row-9 hook (it replaces the generated observe shim; the bracket scope stays
// first, scripts/gen_native_brackets.py).
extern "C" void __imp__sub_820F5570(PPCContext& __restrict ctx, uint8_t* base);
void sub_820F5570(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820F5570);  // NATIVE N0 (scripts/gen_native_brackets.py)
  namespace nb = sr::native::recb;
  if (nb::on() && nb::entryOn(nb::kGpr) && nb::gprAlloc(ctx, base)) return;
  if (nb::checkOn()) nb::checkBefore(nb::kGpr, ctx, base);
  __imp__sub_820F5570(ctx, base);
  if (nb::checkOn()) nb::checkAfter(nb::kGpr, ctx, base);
}
