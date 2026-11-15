// NATIVE leaves: host implementations of hot XDK D3D leaf functions.
// Contract and gates: native_leaves.hpp.
// C1: the development oracles here - the CHECKs (SR_NATIVE_CONST_LEAF_CHECK, SR_NATIVE_CONST_UPLOAD_CHECK,
// SR_NATIVE_TYPE0_LEAF_CHECK, SR_NATIVE_CONST_SKIP_CHECK with its mutation test _SKIP_MUT, the dedup leaf's CHECK
// marking under SR_NATIVE_CONST_DEDUP_CHECK) and the sizing diagnostics (SR_NATIVE_LEAF_CALLERS,
// SR_NATIVE_CONST_UPLOAD_STATS) - are compiled out under SR_ORACLE=0 (kernel/sr_oracle.hpp): there each of those gates
// reads as unset (no env read). The native leaves, their fallbacks to the guest bodies and the counter lines they print
// stay (a line's CHECK counters then read 0, as with the CHECK unset).

#include <string>
#include <windows.h>
#include <intrin.h>
#include "native_leaves.hpp"

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "native_brackets.hpp"
#include "native_const_dedup.hpp"
#include "hdr_adaptation.hpp"
#include "sr_oracle.hpp"
#if SR_ORACLE
#include "gpu_write_watch.hpp"  // iter-467 guestcode: SR_NATIVE_CONST_SKIP's page generations
#endif
#include "ab_toggle.hpp"        // iter-467 guestcode: in-roll A/B (SR_AB_TOGGLE)
#include "../app/log.hpp"

#include <atomic>
#include <emmintrin.h>
#include <cstdlib>
#include <cstring>

namespace sr::native::leaf {

namespace {

bool envDefaultOn(const char* n) {
  const char* e = std::getenv(n);
  return !e || e[0] != '0';
}
#if SR_ORACLE
bool envOn(const char* n) {
  const char* e = std::getenv(n);
  return e && e[0] == '1';
}
#endif

const bool g_constLeafOn = envDefaultOn("SR_NATIVE_CONST_LEAF");
#if SR_ORACLE
const bool g_constCheck = envOn("SR_NATIVE_CONST_LEAF_CHECK");
#endif

std::atomic<uint64_t> g_native{0}, g_fallback{0}, g_checked{0}, g_checkBad{0};

inline uint64_t rdBE64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return _byteswap_uint64(v);
}
inline void wrBE64(uint8_t* p, uint64_t v) {
  v = _byteswap_uint64(v);
  std::memcpy(p, &v, 8);
}

// Host pointer for [va, va+len) when the whole range is contiguous after the
// recompiler's own alias fold (PPC_RAW_ADDR = base + aliasGuestVA(va) per access),
// else null.
//
// iter-466 tick (SR_NATIVE_LEAF_FASTRANGE): `vacuous` = the caller proved, once per leaf
// call, that isGuestVAInRange is true for EVERY 32-bit VA (rangeChecksVacuous below), so
// the two out-of-line checks (alias fold + TLS lookups each; ~10 per constant upload,
// 10.6% of the guest render thread with their inlined sites, PERFORMANCE 2.7) are skipped.
// Same result for every input: the checks only ever return true in that case.
uint8_t* hostRange(uint8_t* base, uint32_t va, uint32_t len, bool vacuous = false) {
  if (!len) return base + sr::kernel::aliasGuestVA(va);
  const uint32_t a0 = sr::kernel::aliasGuestVA(va);
  const uint32_t a1 = sr::kernel::aliasGuestVA(va + len - 1u);
  if (a1 < a0 || a1 - a0 != len - 1u) return nullptr;
  if (!vacuous &&
      (!sr::kernel::isGuestVAInRange(va) || !sr::kernel::isGuestVAInRange(va + len - 4u)))
    return nullptr;
  return base + a0;
}

// iter-466 tick: baked default ON (=0 restores the checks; PERFORMANCE 2.8).
const bool g_fastRange = envDefaultOn("SR_NATIVE_LEAF_FASTRANGE");

// isGuestVAInRange(va) = aliasGuestVA(va) + 4 <= size, with (base, size) = the calling
// thread's guest memory (getEffectiveGuestMemory: the TLS pair when the TLS base is set).
// The guest reservation is 4 GiB + a 64 KiB guard (guest/guest_harness.cpp kTotalSize),
// so with size >= 2^32 + 4 every check is true (aliasGuestVA returns a 32-bit value).
// A smaller (fallback) reservation, or a thread without the TLS pair, keeps the checks.
//
// iter-466 tick2 (SR_NATIVE_LEAF_TLCACHE): the answer is cached per thread, keyed by the guest
// base the caller passes (the thread's TLS base): the two out-of-line TLS getters were 1.6% of
// the guest render thread at ~40K constant uploads per frame (archive/v1_tick/p2). A thread's
// (base, size) pair is set once by setGuestMemory; a different base recomputes.
const bool g_tlCache = envDefaultOn("SR_NATIVE_LEAF_TLCACHE");  // baked ON (=0 A/B), PERFORMANCE 2.9
inline bool rangeChecksVacuousSlow() {
  return g_fastRange && sr::kernel::getGuestBase() != nullptr &&
         uint64_t(sr::kernel::getGuestSize()) >= 0x100000004ull;
}
inline bool rangeChecksVacuous(const uint8_t* base) {
  if (!g_tlCache) return rangeChecksVacuousSlow();
  thread_local constinit const uint8_t* t_base = nullptr;
  thread_local constinit bool t_vac = false;
  if (base != t_base) {
    t_vac = rangeChecksVacuousSlow();
    t_base = base;
  }
  return t_vac;
}

void report() {
  const uint64_t n = g_native.load(std::memory_order_relaxed);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_LEAF] setShaderConstantF native=%llu fallback=%llu checked=%llu checkBad=%llu",
              (unsigned long long)n, (unsigned long long)g_fallback.load(std::memory_order_relaxed),
              (unsigned long long)g_checked.load(std::memory_order_relaxed),
              (unsigned long long)g_checkBad.load(std::memory_order_relaxed));
}

}  // namespace

#if SR_ORACLE
thread_local const void* t_leafCaller = nullptr;
const bool g_leafCallersOn = envOn("SR_NATIVE_LEAF_CALLERS");
#endif  // (SR_ORACLE=0: constant false shims in native_leaves.hpp)

namespace {
// NATIVE N1 phase 1 check (SR_NATIVE_CONST_UPLOAD_CHECK, below): the leaf calls the guest
// upload body makes while armed, and the shadow right after its F list's last call.
constexpr uint32_t kUpSpan = 0x2800u;  // dev+0 .. +0x2800: dirty words + VS/PS F shadows
#if SR_ORACLE
constexpr uint32_t kUpMaxF = 512u;
struct UpCall {
  uint32_t dev, start, src, count;
  uint64_t mask;
};
struct UpCapture {
  uint32_t want = 0, got = 0;
  bool snapped = false;
  const uint8_t* devH = nullptr;
  UpCall calls[kUpMaxF];
  uint8_t snapA[kUpSpan];  // expected: pre-state + the native F pass
  uint8_t snapB[kUpSpan];  // actual: the shadow after the guest's F list
};
const bool g_uploadCheck = envOn("SR_NATIVE_CONST_UPLOAD_CHECK");
thread_local UpCapture* t_upCap = nullptr;  // armed by the upload override in check mode
#endif
}  // namespace

static void setShaderConstantFImpl(PPCContext& ctx, uint8_t* base, bool vsNotPs,
                                   void (*guestBody)(PPCContext&, uint8_t*));

void setShaderConstantF(PPCContext& ctx, uint8_t* base, bool vsNotPs,
                        void (*guestBody)(PPCContext&, uint8_t*)) {
#if SR_ORACLE
  UpCapture* cap = g_uploadCheck ? t_upCap : nullptr;
  if (!cap) [[likely]] {
    setShaderConstantFImpl(ctx, base, vsNotPs, guestBody);
    return;
  }
  const UpCall c{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u64};
  setShaderConstantFImpl(ctx, base, vsNotPs, guestBody);
  if (cap->got < cap->want) {
    cap->calls[cap->got++] = c;
    if (cap->got == cap->want) {
      std::memcpy(cap->snapB, cap->devH, kUpSpan);
      cap->snapped = true;
    }
  }
#else
  setShaderConstantFImpl(ctx, base, vsNotPs, guestBody);
#endif
}

static void setShaderConstantFImpl(PPCContext& ctx, uint8_t* base, bool vsNotPs,
                                   void (*guestBody)(PPCContext&, uint8_t*)) {
  if (!g_constLeafOn) {
    guestBody(ctx, base);
    return;
  }
  // Guest semantics (IDA 820F4920 / 820F4A30, recomp ppc_recomp.1.cpp):
  //   r10 = dev + ((start + 112|368) << 4 & 0xFFFFFFF0)   (dev+0x700 / +0x1700 + 16*start)
  //   copy `count` 16-byte vectors from the unaligned source r5 (lvlx|lvrx = the
  //   16 bytes at the address), 4 at a time then 1 at a time;
  //   stack: std r7,48(r1); stw r3,-32(r1);  then  *(u64*)(dev + 0|8) |= r7.
  const uint32_t dev = ctx.r3.u32;
  const uint32_t start = ctx.r4.u32;
  const uint32_t src = ctx.r5.u32;
  const uint32_t count = ctx.r6.u32;
  const uint64_t mask = ctx.r7.u64;
#if SR_ORACLE
  // NATIVE N1 sizing (SR_NATIVE_LEAF_CALLERS=1, diagnostic): who uploads the constants -
  // per (caller LR, VS/PS, count) call counts, the top 16 logged every 2^20 calls.
  if (g_leafCallersOn) {
    struct Ent { uint32_t lr, cnt; uint64_t n; };
    static thread_local Ent t_tab[4096];
    static thread_local uint64_t t_calls = 0;
    static const uintptr_t s_exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uint32_t lr = uint32_t(reinterpret_cast<uintptr_t>(t_leafCaller) - s_exe);  // exe RVA
    const uint32_t key = (lr ^ (count << 1) ^ (vsNotPs ? 1u : 0u)) * 0x9E3779B1u >> 20;
    for (uint32_t k = 0; k < 4096; k++) {
      Ent& e = t_tab[(key + k) & 4095];
      if (e.n == 0) { e.lr = lr; e.cnt = (count << 1) | (vsNotPs ? 1u : 0u); }
      if (e.lr == lr && e.cnt == ((count << 1) | (vsNotPs ? 1u : 0u))) { e.n++; break; }
    }
    if ((++t_calls & 0xFFFFFu) == 0) {
      Ent top[16] = {};
      for (const Ent& e : t_tab)
        for (int i = 0; i < 16; i++)
          if (e.n > top[i].n) {
            for (int j = 15; j > i; j--) top[j] = top[j - 1];
            top[i] = e;
            break;
          }
      std::string line;
      for (const Ent& e : top) {
        if (!e.n) break;
        char b[64];
        std::snprintf(b, sizeof(b), " rva%08X/%s/c%u:%.1f%%", e.lr, (e.cnt & 1) ? "VS" : "PS", e.cnt >> 1,
                      100.0 * double(e.n) / double(t_calls));
        line += b;
      }
      SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_LEAF] callers after %llu calls:%s", (unsigned long long)t_calls,
                  line.c_str());
    }
  }
#endif  // SR_ORACLE
  const uint32_t dirtyOff = vsNotPs ? 0u : 8u;
  const uint32_t dstVA = dev + (((start + (vsNotPs ? 112u : 368u)) << 4) & 0xFFFFFFF0u);
  const uint32_t len = count * 16u;
  uint8_t* d = nullptr;
  const uint8_t* s = nullptr;
  uint8_t* dirty = nullptr;
  const bool vac = rangeChecksVacuous(base);
  if (count <= 512u) {
    d = hostRange(base, dstVA, len, vac);
    s = hostRange(base, src, len, vac);
    dirty = hostRange(base, dev + dirtyOff, 8, vac);
  }
  uint8_t* stk48 = hostRange(base, ctx.r1.u32 + 48u, 8, vac);
  uint8_t* stkM32 = hostRange(base, ctx.r1.u32 - 32u, 4, vac);
  const bool overlap = d && s && len && !(s + len <= d || d + len <= s);
  if (!d || !s || !dirty || !stk48 || !stkM32 || overlap) {
    g_fallback.fetch_add(1, std::memory_order_relaxed);
    guestBody(ctx, base);
    return;
  }
#if SR_ORACLE
  const PPCRegister r3 = ctx.r3, r4 = ctx.r4, r5 = ctx.r5, r6 = ctx.r6, r7 = ctx.r7;
#endif
  wrBE64(stk48, mask);
  {
    const uint32_t v = _byteswap_ulong(dev);
    std::memcpy(stkM32, &v, 4);
  }
  if (g_fastRange) {
    // count 16-byte vectors, source and destination disjoint (checked above): the same
    // bytes as the memcpy, without the out-of-line call for these short copies.
    for (uint32_t i = 0; i < count; i++)
      _mm_storeu_si128(reinterpret_cast<__m128i*>(d + 16u * i),
                       _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 16u * i)));
  } else if (len) {
    std::memcpy(d, s, len);
  }
  const uint64_t nd = rdBE64(dirty) | mask;
  wrBE64(dirty, nd);
  // Registers the guest body leaves behind (volatile under the ABI; kept for
  // differential tests): r9 = mask, r10 = the new dirty word, r11 = dev.
  ctx.r9.u64 = mask;
  ctx.r10.u64 = nd;
  ctx.r11.u64 = dev;
  // iter-466 tick2 (SR_NATIVE_LEAF_TLCACHE): one shared atomic add per 4096 calls per thread
  // instead of a lock-prefixed add per call (1.1%); the [NATIVE_LEAF] line still prints about
  // every 2^20 calls (counts lag by at most 4095 per thread).
  uint64_t n;
  bool due;
  if (g_tlCache) {
    thread_local constinit uint32_t t_pending = 0;
    if (++t_pending == 4096u) {
      t_pending = 0;
      n = g_native.fetch_add(4096, std::memory_order_relaxed) + 4096;
      due = (n >> 20) != ((n - 4096) >> 20);
    } else {
      n = 0;
      due = false;
    }
  } else {
    n = g_native.fetch_add(1, std::memory_order_relaxed) + 1;
    due = (n & 0xFFFFFu) == 1u;
  }

#if SR_ORACLE
  if (g_constCheck) {
    // Snapshot what native wrote, then run the guest body on the same arguments
    // over the same memory: both are idempotent, so any byte the guest changes
    // is a byte native got wrong.
    // iter-466 tick: thread_local, not an 8 KB local - the local array made EVERY call
    // (check off too) probe two stack pages (__chkstk, 1.2% of the constant leaf).
    static thread_local uint8_t snap[512 * 16];
    if (len) std::memcpy(snap, d, len);
    const uint64_t nDirty = rdBE64(dirty);
    ctx.r3 = r3;
    ctx.r4 = r4;
    ctx.r5 = r5;
    ctx.r6 = r6;
    ctx.r7 = r7;
    guestBody(ctx, base);
    g_checked.fetch_add(1, std::memory_order_relaxed);
    const bool bytesOk = !len || std::memcmp(snap, d, len) == 0;
    const bool dirtyOk = rdBE64(dirty) == nDirty;
    if (!bytesOk || !dirtyOk) {
      const uint64_t bad = g_checkBad.fetch_add(1, std::memory_order_relaxed) + 1;
      if (bad <= 16)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[NATIVE_LEAF] MISMATCH #%llu %s dev=%08X start=%u src=%08X count=%u mask=%016llX bytes=%d "
                    "dirty=%d lr=%08X",
                    (unsigned long long)bad, vsNotPs ? "VS" : "PS", dev, start, src, count,
                    (unsigned long long)mask, int(bytesOk), int(dirtyOk), uint32_t(ctx.lr));
    }
  }
#endif  // SR_ORACLE
  if (due) report();
}

namespace {

const bool g_type0LeafOn = envDefaultOn("SR_NATIVE_TYPE0_LEAF");
#if SR_ORACLE
const bool g_type0Check = envOn("SR_NATIVE_TYPE0_LEAF_CHECK");
#endif
std::atomic<uint64_t> g_t0Native{0}, g_t0Fallback{0}, g_t0Checked{0}, g_t0Bad{0};

struct T0Run {
  uint32_t reg, srcVA, regs;  // first register, source VA (16-aligned by lvx), register count
};

}  // namespace

// GpuEmitType0RegPacketsVec_sub_821078C0(dev r3, mask r4 (64-bit), baseReg r5, src r6).
// Guest semantics (recomp of 0x821078C0..0x82107A2C, IDA): the cursor dev+0x28 points at the
// LAST dword written. While mask != 0: skip lz = cntlzd(mask) bits (register += 16*lz, source
// += 64*lz), take the run of leading ones (run bits = 16*run registers). If
// cursor + 4*(regs+3) >= dev+0x2C the run goes through GpuCmdEmitRegisterRunChunked_82107058
// (segment switch); else pad with type-2 NOPs (0x80000000) so the header lands at an address
// = 0xC mod 16 (cursor&15: 4 -> 1 NOP, 0 -> 2, 12 -> 3, 8 -> 0), write the type-0 header
// ((regs-1)<<16 | reg) and copy 64 bytes per bit with lvx128/stvx (16-byte aligned, a byte copy).
// Finally dev+0x28 = cursor. Native does the all-fast-path case; any run that would take the
// chunked path (or a non-contiguous host range) delegates the WHOLE call to the guest body
// before anything is written.
bool emitType0RegPacketsVec(PPCContext& ctx, uint8_t* base) {
  if (!g_type0LeafOn) return false;
  const uint32_t dev = ctx.r3.u32;
  uint64_t mask = ctx.r4.u64;
  uint32_t reg = ctx.r5.u32;
  uint32_t src = ctx.r6.u32;  // r30 + 4
  const bool vac = rangeChecksVacuous(base);
  uint8_t* devH = hostRange(base, dev, 48, vac);
  if (!devH) return false;
  uint32_t cur;
  std::memcpy(&cur, devH + 40, 4);
  cur = _byteswap_ulong(cur);
  uint32_t lim;
  std::memcpy(&lim, devH + 44, 4);
  lim = _byteswap_ulong(lim);
  T0Run runs[64];
  int nRuns = 0;
  uint32_t c = cur;
  const uint32_t cur0 = cur;
  while (mask) {
    const uint32_t lz = uint32_t(__builtin_clzll(mask));
    src += lz * 64u;
    reg += lz * 16u;
    mask <<= lz;
    const uint32_t run = (~mask) ? uint32_t(__builtin_clzll(~mask)) : 64u;
    const uint32_t regs = run * 16u;
    if (!(c + 4u * (regs + 3u) < lim)) {
      g_t0Fallback.fetch_add(1, std::memory_order_relaxed);
      return false;  // the chunked path (segment switch): guest body
    }
    const uint32_t a = c & 0xFu;
    const uint32_t pad = a == 4u ? 1u : a == 0u ? 2u : a == 12u ? 3u : 0u;
    c += 4u * (pad + 1u) + 64u * run;
    runs[nRuns++] = T0Run{reg, src, regs};
    src += 64u * run;
    reg += regs;
    mask = run >= 64u ? 0ull : (mask << run);
  }
  // One contiguous host range for everything written, and each source range.
  uint8_t* outH = hostRange(base, cur0 + 4u, c - cur0, vac);
  if (!outH && c != cur0) {
    g_t0Fallback.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  for (int i = 0; i < nRuns; i++)
    if (!hostRange(base, runs[i].srcVA & ~15u, runs[i].regs * 4u, vac)) {
      g_t0Fallback.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
  // Write.
  c = cur0;
  for (int i = 0; i < nRuns; i++) {
    const uint32_t a = c & 0xFu;
    const uint32_t pad = a == 4u ? 1u : a == 0u ? 2u : a == 12u ? 3u : 0u;
    for (uint32_t k = 0; k < pad; k++) {
      c += 4u;
      const uint32_t nop = 0x00000080u;  // 0x80000000 big-endian
      std::memcpy(outH + (c - cur0 - 4u), &nop, 4);
    }
    c += 4u;
    const uint32_t hdr = _byteswap_ulong(((runs[i].regs - 1u) << 16) | (runs[i].reg & 0xFFFFu) |
                                          (runs[i].reg & 0xFFFF0000u));
    std::memcpy(outH + (c - cur0 - 4u), &hdr, 4);
    const uint8_t* sH = base + sr::kernel::aliasGuestVA(runs[i].srcVA & ~15u);
    std::memcpy(outH + (c + 4u - cur0 - 4u), sH, runs[i].regs * 4u);
    c += runs[i].regs * 4u;
  }
  const uint32_t cBE = _byteswap_ulong(c);
  std::memcpy(devH + 40, &cBE, 4);
  ctx.r4.u64 = c;
  g_t0Native.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// NATIVE strand A (SR_NATIVE_CONST_DEDUP / _CHECK, native_const_dedup.hpp): the same emission,
// minus every 64-byte group the stream already holds. Runs are split at skipped groups; each
// emitted sub-run gets the guest's own padding / header / copy. A sub-run that would reach the
// segment limit hands the WHOLE call to the guest body before anything is written (and the
// adapter loses track). With CHECK, nothing is skipped and each would-be skip is marked for the CP.
// The same switch as native_const_dedup.cpp (default ON; =0 off unless its CHECK is on).
const bool g_dedupGate = [] {
  const char* e = std::getenv("SR_NATIVE_CONST_DEDUP");
#if SR_ORACLE
  return (!e || e[0] != '0') || envOn("SR_NATIVE_CONST_DEDUP_CHECK");
#else
  return !e || e[0] != '0';  // SR_ORACLE=0: no CHECK to turn it on
#endif
}();

bool emitType0Dedup(PPCContext& ctx, uint8_t* base) {
  namespace cd = sr::native::cdedup;
  if (!g_dedupGate) return false;
  if (cd::suspended()) {  // a sampled S1 flush CHECK draw: the plain guest body, comparable
    cd::lostStream();
    return false;
  }
  const uint32_t dev = ctx.r3.u32;
  uint64_t mask = ctx.r4.u64;
  uint32_t reg = ctx.r5.u32;
  uint32_t src = ctx.r6.u32;
  const bool vac = rangeChecksVacuous(base);
  uint8_t* devH = hostRange(base, dev, 13512u, vac);
  if (!devH) {
    cd::lostStream();
    return false;
  }
  uint32_t cur0, lim;
  std::memcpy(&cur0, devH + 40, 4);
  std::memcpy(&lim, devH + 44, 4);
  cur0 = _byteswap_ulong(cur0);
  lim = _byteswap_ulong(lim);
  const bool sync = cd::syncStream(base, dev, devH, cur0);
  const bool skipOn = sync && cd::on();
  struct Grp {
    uint32_t reg;
    const uint8_t* s;
    bool pred;
  };
  Grp g[64];
  uint32_t nG = 0;
  while (mask) {
    const uint32_t lz = uint32_t(__builtin_clzll(mask));
    src += lz * 64u;
    reg += lz * 16u;
    mask <<= lz;
    const uint32_t run = (~mask) ? uint32_t(__builtin_clzll(~mask)) : 64u;
    const uint8_t* sH = hostRange(base, src & ~15u, run * 64u, vac);
    if (!sH || nG + run > 64u) {
      cd::lostStream();
      return false;
    }
    for (uint32_t k = 0; k < run; k++) {
      const uint32_t r = reg + 16u * k;
      const bool alu = r >= cd::kAluReg && r + 16u <= cd::kAluReg + cd::kAluRegs && !((r - cd::kAluReg) & 15u);
      g[nG++] = Grp{r, sH + 64u * k, sync && alu && cd::resident((r - cd::kAluReg) >> 4, sH + 64u * k)};
    }
    src += 64u * run;
    reg += 16u * run;
    mask = run >= 64u ? 0ull : (mask << run);
  }
  // Sub-runs of emitted groups that are consecutive registers and source bytes. With CHECK nothing
  // is skipped, runs also split where the prediction changes, and each predicted run is announced by
  // a type-3 NOP {0xC0001000, 'DDCK'} the CP checks against its register file.
#if SR_ORACLE
  const bool check = cd::checkOn();
#endif
  auto runEnd = [&](uint32_t i) {
    uint32_t j = i + 1;
    while (j < nG && !(skipOn && g[j].pred) && g[j].reg == g[j - 1].reg + 16u && g[j].s == g[j - 1].s + 64u
#if SR_ORACLE  // the CHECK also splits runs where the prediction changes
           && (!check || g[j].pred == g[i].pred)
#endif
           )
      j++;
    return j;
  };
  uint32_t c = cur0;
  for (uint32_t i = 0; i < nG;) {
    if (skipOn && g[i].pred) {
      i++;
      continue;
    }
    const uint32_t j = runEnd(i);
    const uint32_t regs = 16u * (j - i);
#if SR_ORACLE
    if (check && g[i].pred) c += 8u;
#endif
    if (!(c + 4u * (regs + 3u) < lim)) {  // the chunked path (segment switch): guest body
      cd::lostStream();
      return false;
    }
    const uint32_t a = c & 0xFu;
    const uint32_t pad = a == 4u ? 1u : a == 0u ? 2u : a == 12u ? 3u : 0u;
    c += 4u * (pad + 1u) + 4u * regs;
    i = j;
  }
  uint8_t* outH = c != cur0 ? hostRange(base, cur0 + 4u, c - cur0, vac) : nullptr;
  if (c != cur0 && !outH) {
    cd::lostStream();
    return false;
  }
  // Write.
  c = cur0;
  for (uint32_t i = 0; i < nG;) {
    if (skipOn && g[i].pred) {
      i++;
      continue;
    }
    const uint32_t j = runEnd(i);
    const uint32_t regs = 16u * (j - i);
#if SR_ORACLE
    if (check && g[i].pred) {
      const uint32_t mk[2] = {_byteswap_ulong(0xC0001000u), _byteswap_ulong(cd::kCheckMagic)};
      std::memcpy(outH + (c + 4u - cur0 - 4u), mk, 8);
      c += 8u;
      cd::notePredictedRun(j - i);
    }
#endif
    const uint32_t a = c & 0xFu;
    const uint32_t pad = a == 4u ? 1u : a == 0u ? 2u : a == 12u ? 3u : 0u;
    for (uint32_t k = 0; k < pad; k++) {
      c += 4u;
      const uint32_t nop = 0x00000080u;  // 0x80000000 big-endian
      std::memcpy(outH + (c - cur0 - 4u), &nop, 4);
    }
    c += 4u;
    const uint32_t hdr = _byteswap_ulong(((regs - 1u) << 16) | g[i].reg);
    std::memcpy(outH + (c - cur0 - 4u), &hdr, 4);
    std::memcpy(outH + (c + 4u - cur0 - 4u), g[i].s, 4u * regs);
    for (uint32_t k = i; k < j; k++) {
      const uint32_t r = g[k].reg;
      if (!sync)
        continue;  // no dedup this call: the mirror stays invalid
      if (r >= cd::kAluReg && r + 16u <= cd::kAluReg + cd::kAluRegs && !((r - cd::kAluReg) & 15u))
        cd::store((r - cd::kAluReg) >> 4, g[k].s);
      else
        cd::invalidateRegs(r, 16u);
    }
    c += 4u * regs;
    i = j;
  }
  const uint32_t cBE = _byteswap_ulong(c);
  std::memcpy(devH + 40, &cBE, 4);
  ctx.r4.u64 = c;
  cd::noteEmitted(c);
  uint32_t nPred = 0;
  for (uint32_t i = 0; i < nG; i++) nPred += g[i].pred ? 1u : 0u;
  cd::countGroups(nG, nPred);
  g_t0Native.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void type0Dispatch(PPCContext& ctx, uint8_t* base, void (*guestBody)(PPCContext&, uint8_t*)) {
  if (g_dedupGate) {
    if (!emitType0Dedup(ctx, base)) guestBody(ctx, base);
    return;
  }
#if SR_ORACLE
  if (!g_type0Check) {
    if (!emitType0RegPacketsVec(ctx, base)) guestBody(ctx, base);
    return;
  }
  const PPCRegister r3 = ctx.r3, r4 = ctx.r4, r5 = ctx.r5, r6 = ctx.r6;
  uint8_t* devH = hostRange(base, ctx.r3.u32, 48);
  uint32_t cur0BE = 0;
  if (devH) std::memcpy(&cur0BE, devH + 40, 4);
  if (!devH || !emitType0RegPacketsVec(ctx, base)) {
    guestBody(ctx, base);
    return;
  }
  uint32_t cNatBE;
  std::memcpy(&cNatBE, devH + 40, 4);
  const uint32_t cur0 = _byteswap_ulong(cur0BE), cNat = _byteswap_ulong(cNatBE);
  const uint32_t len = cNat - cur0;
  static thread_local uint8_t snap[64 * 1024 + 256];
  uint8_t* outH = len ? hostRange(base, cur0 + 4u, len) : nullptr;
  if (len > sizeof(snap) || (len && !outH)) return;  // native result stands, unchecked
  if (len) std::memcpy(snap, outH, len);
  std::memcpy(devH + 40, &cur0BE, 4);  // rewind: the guest body re-emits the same packets
  ctx.r3 = r3;
  ctx.r4 = r4;
  ctx.r5 = r5;
  ctx.r6 = r6;
  guestBody(ctx, base);
  uint32_t cGuestBE;
  std::memcpy(&cGuestBE, devH + 40, 4);
  g_t0Checked.fetch_add(1, std::memory_order_relaxed);
  const bool curOk = cGuestBE == cNatBE;
  const bool bytesOk = curOk && (!len || std::memcmp(snap, outH, len) == 0);
  if (!curOk || !bytesOk) {
    const uint64_t bad = g_t0Bad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (bad <= 16)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[NATIVE_LEAF] TYPE0 MISMATCH #%llu dev=%08X mask=%016llX reg=%04X src=%08X cur0=%08X "
                  "native=%08X guest=%08X bytes=%d",
                  (unsigned long long)bad, r3.u32, (unsigned long long)r4.u64, r5.u32, r6.u32, cur0, cNat,
                  _byteswap_ulong(cGuestBE), int(bytesOk));
  }
  const uint64_t n = g_t0Checked.load(std::memory_order_relaxed);
  if ((n & 0xFFFFFu) == 1u)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_LEAF] type0Vec native=%llu fallback=%llu checked=%llu checkBad=%llu",
                (unsigned long long)g_t0Native.load(), (unsigned long long)g_t0Fallback.load(),
                (unsigned long long)n, (unsigned long long)g_t0Bad.load());
#else
  if (!emitType0RegPacketsVec(ctx, base)) guestBody(ctx, base);
#endif  // SR_ORACLE
}

}  // namespace sr::native::leaf

// D3DDevice_SetPixelShaderConstantF (no override existed; the VS twin lives in
// kernel/hle_api_observe.cpp next to its iter-450O observe histogram).
extern "C" void __imp__sub_820F4A30(PPCContext& __restrict ctx, uint8_t* base);
static void guestPsConstF(PPCContext& ctx, uint8_t* base) { __imp__sub_820F4A30(ctx, base); }

// GpuEmitType0RegPacketsVec (the XDK draw flush's constant upload into the command buffer).
extern "C" void __imp__sub_821078C0(PPCContext& __restrict ctx, uint8_t* base);
void sub_821078C0(PPCContext& __restrict ctx, uint8_t* base) {
  sr::native::leaf::type0Dispatch(ctx, base, [](PPCContext& c, uint8_t* b) { __imp__sub_821078C0(c, b); });
}
void sub_820F4A30(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820F4A30);  // NATIVE N0 (scripts/gen_native_brackets.py)
  const uint32_t device = ctx.r3.u32, start = ctx.r4.u32, source = ctx.r5.u32, count = ctx.r6.u32;
  if (sr::native::leaf::g_leafCallersOn) sr::native::leaf::t_leafCaller = _ReturnAddress();
  sr::native::leaf::setShaderConstantF(ctx, base, false, guestPsConstF);
  sr::kernel::hdr::afterPsConstantUpload(base, device, start, source, count);
}

// ---- NATIVE N1 phase 1: the engine's per-binding constant upload -----------------------------
// ShaderPs_UploadAllConstants_sub_8260B660 / ShaderVs_UploadAllConstants_sub_8260B2A0 (IDA +
// recomp ppc_recomp.74.cpp): for shader binding r3, four 0-terminated lists at r3+8 (PS) / +12 (VS),
// in this order: F {u16 paramId, u8 reg, u8 count} -> Set{Pixel,Vertex}ShaderConstantF(dev, reg,
// src = dword_82998B00[paramId], count, groupMask); I and B (same layout) -> Set*ShaderConstantI/B;
// then semantic callbacks {u32 fn, u16 paramId, u8 reg, u8 count}: texture params 0x400..0x7FF get
// fn(&{reg, count, _, _, tex = *(0x829886A4 + paramId*32), isVertex}) at r1+80, others fn(reg,
// count, isVertex). The sizing roll (SR_NATIVE_LEAF_CALLERS) put ~89% of the ~45K constant leaf calls
// per gameplay frame on these two F loops, almost all count 1.
//
// SR_NATIVE_CONST_UPLOAD (default ON, =0 = the guest bodies): the F list runs as host code (the leaf's copy + dirty OR per entry, in
// list order, one shadow range check per call instead of five per entry); the I/B lists and the
// callbacks are the guest's own calls in the guest's order with the guest's frame and arguments.
// An F entry outside the fast path (destination past the shadow span, source not contiguous or
// inside the shadow) goes through the leaf exactly as the guest loop calls it.
//
// SR_NATIVE_CONST_UPLOAD_CHECK=1: per call, the native F pass is applied to a COPY of the shadow,
// then the guest body runs for real (all four lists, in its order); the leaf wrapper records the F
// calls it makes and snapshots the shadow right after the last one. Equal call arguments and equal
// shadow bytes at that point mean everything after it (I/B lists, callbacks, any callback that
// reads a constant an F entry wrote) sees the same state under native as under the guest.
namespace sr::native::leaf {
namespace {

// iter-467: baked ON (=0 = the guest bodies, A/B). Check 25.4M calls 0 bad (u_chk); uncapped ingame
// 95.4/97.2/97.5 -> 107.8/109.1/109.0 fps, tick 10.39/10.33/10.31 -> 9.25/9.17/9.18 ms (u_a*/u_b*).
const bool g_uploadOn = envDefaultOn("SR_NATIVE_CONST_UPLOAD");
// Phase 2 sizing (SR_NATIVE_CONST_UPLOAD_STATS=1, diagnostic, with SR_NATIVE_CONST_UPLOAD=1): F
// entries whose bytes the shadow already holds, with every group of the mask already dirty (the
// entry is a no-op) or some group clean (skipping it would drop a re-upload: a PM4 change).
#if SR_ORACLE
const bool g_uploadStats = envOn("SR_NATIVE_CONST_UPLOAD_STATS");
std::atomic<uint64_t> g_stEnt{0}, g_stEqDirty{0}, g_stEqClean{0}, g_stAllNoop{0};
#endif
std::atomic<uint64_t> g_upCalls{0}, g_upNativeF{0}, g_upLeafF{0}, g_upWhole{0};
std::atomic<uint64_t> g_upChecked{0}, g_upBad{0}, g_upUnchecked{0}, g_upNested{0};

constexpr uint32_t kDevPtrVA = 0x8299ADF4u;       // dword_8299ADF4: the D3D device
constexpr uint32_t kParamTableVA = 0x82998B00u;   // dword_82998B00[paramId]: parameter value pointers
constexpr uint32_t kTexRegistryVA = 0x829886A4u;  // unk_829886A4 + paramId*32: texture parameters

// The guest loop's mask: srwi r11,reg,2; r10 = (reg+count-1)>>2 (32-bit); subf; clrldi 32;
// srad r10,(1<<63),r10; srd r7,r10,r11 (both shift amounts = the low 7 bits).
inline uint64_t groupMask(uint32_t reg, uint32_t cnt) {
  const uint32_t lo = reg >> 2;
  const uint32_t hi = uint32_t(reg + cnt - 1u) >> 2;
  const uint32_t sh = (hi - lo) & 0x7Fu;
  const uint64_t t = sh >= 64u ? ~0ull : uint64_t(INT64_MIN >> sh);
  const uint32_t sh2 = lo & 0x7Fu;
  return sh2 >= 64u ? 0ull : t >> sh2;
}

inline uint32_t fDestOff(bool vs, uint32_t reg) { return ((reg + (vs ? 112u : 368u)) << 4) & 0xFFFFFFF0u; }

// One F entry with the leaf's effect on `shadow` (the device's first kUpSpan bytes, or a copy):
// false = outside the fast path, nothing written.
inline bool applyF(uint8_t* base, uint8_t* shadow, const uint8_t* devH, bool vs, uint32_t reg,
                   uint32_t cnt, uint32_t src, uint64_t mask, bool vac, uint64_t& nd) {
  const uint32_t off = fDestOff(vs, reg);
  const uint32_t len = cnt * 16u;
  if (uint64_t(off) + len > kUpSpan) return false;
  const uint8_t* s = hostRange(base, src, len, vac);
  if (!s || (len && s < devH + kUpSpan && devH < s + len)) return false;
  uint8_t* d = shadow + off;
  for (uint32_t i = 0; i < cnt; i++)
    _mm_storeu_si128(reinterpret_cast<__m128i*>(d + 16u * i),
                     _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 16u * i)));
  uint8_t* dirty = shadow + (vs ? 0u : 8u);
  nd = rdBE64(dirty) | mask;
  wrBE64(dirty, nd);
  return true;
}

#if SR_ORACLE
// 0 = bytes differ (or outside the fast path), 1 = equal + groups already dirty, 2 = equal + a
// group clean.
inline int statEntry(uint8_t* base, const uint8_t* devH, bool vs, uint32_t reg, uint32_t cnt,
                     uint32_t src, uint64_t mask, bool vac) {
  const uint32_t off = fDestOff(vs, reg);
  const uint32_t len = cnt * 16u;
  if (uint64_t(off) + len > kUpSpan) return 0;
  const uint8_t* s = hostRange(base, src, len, vac);
  if (!s || std::memcmp(devH + off, s, len) != 0) return 0;
  return (rdBE64(devH + (vs ? 0u : 8u)) & mask) == mask ? 1 : 2;
}
#endif  // SR_ORACLE

void uploadReport() {
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_UPLOAD] calls=%llu nativeF=%llu leafF=%llu whole=%llu checked=%llu checkBad=%llu "
              "unchecked=%llu nested=%llu (lists F,I,B then callbacks, guest order)",
              (unsigned long long)g_upCalls.load(std::memory_order_relaxed),
              (unsigned long long)g_upNativeF.load(std::memory_order_relaxed),
              (unsigned long long)g_upLeafF.load(std::memory_order_relaxed),
              (unsigned long long)g_upWhole.load(std::memory_order_relaxed),
              (unsigned long long)g_upChecked.load(std::memory_order_relaxed),
              (unsigned long long)g_upBad.load(std::memory_order_relaxed),
              (unsigned long long)g_upUnchecked.load(std::memory_order_relaxed),
              (unsigned long long)g_upNested.load(std::memory_order_relaxed));
#if SR_ORACLE
  if (g_uploadStats)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_UPLOAD] stats entries=%llu eqDirty=%llu eqClean=%llu callsAllNoop=%llu",
                (unsigned long long)g_stEnt.load(std::memory_order_relaxed),
                (unsigned long long)g_stEqDirty.load(std::memory_order_relaxed),
                (unsigned long long)g_stEqClean.load(std::memory_order_relaxed),
                (unsigned long long)g_stAllNoop.load(std::memory_order_relaxed));
#endif
}

// Per-thread counts, published every 4096 calls (an atomic add per call would cost a visible
// share of what this saves).
void countUpload(uint32_t nativeF, uint32_t leafF) {
  thread_local constinit uint32_t t_calls = 0;
  thread_local constinit uint64_t t_nat = 0, t_leaf = 0;
  t_nat += nativeF;
  t_leaf += leafF;
  if (++t_calls < 4096u) return;
  g_upNativeF.fetch_add(t_nat, std::memory_order_relaxed);
  g_upLeafF.fetch_add(t_leaf, std::memory_order_relaxed);
  const uint64_t n = g_upCalls.fetch_add(t_calls, std::memory_order_relaxed) + t_calls;
  const bool due = (n >> 18) != ((n - t_calls) >> 18);
  t_calls = 0;
  t_nat = t_leaf = 0;
  if (due) uploadReport();
}

#if SR_ORACLE
struct UpCheckStore {
  UpCapture cap;
  UpCall expect[kUpMaxF];
};

template <bool VS>
void uploadCheck(PPCContext& ctx, uint8_t* base, uint8_t* devH, uint32_t dev, bool vac,
                 void (*guestWhole)(PPCContext&, uint8_t*)) {
  if (t_upCap) {  // a callback of an armed call re-entered: run it as the guest, unchecked
    g_upNested.fetch_add(1, std::memory_order_relaxed);
    guestWhole(ctx, base);
    return;
  }
  thread_local UpCheckStore* t_store = nullptr;
  if (!t_store) t_store = new UpCheckStore();
  UpCapture& cap = t_store->cap;
  UpCall* expect = t_store->expect;
  const uint32_t binding = ctx.r3.u32;
  std::memcpy(cap.snapA, devH, kUpSpan);
  uint32_t n = 0;
  bool partial = false;
  uint32_t p = binding + (VS ? 12u : 8u);
  for (uint32_t id = PPC_LOAD_U16(p); id; p += 4, id = PPC_LOAD_U16(p)) {
    if (n == kUpMaxF) {
      partial = true;
      break;
    }
    const uint32_t reg = PPC_LOAD_U8(p + 2), cnt = PPC_LOAD_U8(p + 3);
    const uint32_t src = PPC_LOAD_U32(((id << 2) & 0xFFFFFFFCu) + kParamTableVA);
    const uint64_t mask = groupMask(reg, cnt);
    expect[n++] = {dev, reg, src, cnt, mask};
    uint64_t nd;
    if (!applyF(base, cap.snapA, devH, VS, reg, cnt, src, mask, vac, nd)) partial = true;  // leaf path
  }
  cap.devH = devH;
  cap.want = partial ? 0u : n;
  cap.got = 0;
  cap.snapped = false;
  if (cap.want == 0) {
    std::memcpy(cap.snapB, devH, kUpSpan);
    cap.snapped = !partial;
  }
  t_upCap = partial ? nullptr : &cap;
  guestWhole(ctx, base);
  t_upCap = nullptr;
  if (partial) {
    g_upUnchecked.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  uint32_t badCall = UINT32_MAX;
  for (uint32_t i = 0; i < cap.got && badCall == UINT32_MAX; i++) {
    const UpCall& a = cap.calls[i];
    const UpCall& e = expect[i];
    if (a.dev != e.dev || a.start != e.start || a.src != e.src || a.count != e.count || a.mask != e.mask)
      badCall = i;
  }
  int badByte = -1;
  if (cap.snapped)
    for (uint32_t i = 0; i < kUpSpan; i++)
      if (cap.snapA[i] != cap.snapB[i]) {
        badByte = int(i);
        break;
      }
  g_upChecked.fetch_add(1, std::memory_order_relaxed);
  if (!cap.snapped || cap.got != n || badCall != UINT32_MAX || badByte >= 0) {
    const uint64_t bad = g_upBad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (bad <= 16)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[NATIVE_UPLOAD] MISMATCH #%llu %s binding=%08X dev=%08X F=%u guestF=%u snapped=%d "
                  "badCall=%d badByte=%d",
                  (unsigned long long)bad, VS ? "VS" : "PS", binding, dev, n, cap.got, int(cap.snapped),
                  badCall == UINT32_MAX ? -1 : int(badCall), badByte);
  }
  const uint64_t c = g_upChecked.load(std::memory_order_relaxed);
  if ((c & 0xFFFFu) == 0) uploadReport();
}
#endif  // SR_ORACLE

#if SR_ORACLE
// ---- iter-467 guestcode: SR_NATIVE_CONST_SKIP (default OFF; PERFORMANCE 2.16: rejected, +2.6 ms) ------------------
// SR_PARAM_OBS (PERFORMANCE 2.14): 99.72% of the parameter values an F entry copies are the bytes
// that address held at its previous application, so most of the ~45K source reads per frame (data
// misses: the upload's measured floor) re-read unchanged bytes into a shadow register that already
// holds them. The skip proves "unchanged" without the read: per shadow register (16 bytes of the
// device's first kUpSpan) it keeps the source VA it was last copied from, the 16 bytes copied, and
// the source page's write-watch generation (kernel/gpu_write_watch: the page is write-protected; any
// write - guest code, a host memcpy, a VEH-visible DMA-style copy - faults and clears it BEFORE the
// write lands, and kernel-mode writers either release it first (HostWriteScope: NtReadFile and the
// async read worker) or fail on the read-only page without changing it). An entry is skipped when,
// for every register, the source VA matches, the page is still protected under the recorded
// generation, and the shadow still holds the recorded bytes (an L1-resident compare): then the
// copy would store the bytes already there. The dirty OR is always done, exactly as the guest does,
// so the shadow, the dirty words and therefore everything the device emits are unchanged.
//
// Not covered (the skip is disabled): SR_PHYS_UNIFY's alias views (a write through the 0xC/0xE view
// bypasses the 0x8/0xA protection), SR_SHM_WRITE_TRACK=0, a thread whose guest base is not the
// global arena. Sources outside the watched windows (VA < 0x80000000) are copied every time.
//
// SR_NATIVE_CONST_SKIP_CHECK=1: every skipped register is read anyway and compared with the bytes
// the skip relied on; a mismatch is logged and the real bytes are copied.
// SR_NATIVE_CONST_SKIP_MUT=1 (with _CHECK; the mutation test): after a warm-up, the first page seen
// to CHANGE a recorded value has its watch ignored (its registers count as clean while the shadow
// holds the recorded bytes) - the check must then fire.
// Read at the point of use (SR_AB_TOGGLE may flip it per pace window; kernel/ab_toggle.hpp).
bool skipOn() {
  static const bool v = [] { const char* e = std::getenv("SR_NATIVE_CONST_SKIP"); return e && e[0] == '1'; }();
  static const int ab = sr::abt::reg("SR_NATIVE_CONST_SKIP");
  return v && (ab < 0 || sr::abt::on(ab));
}
#if SR_ORACLE
const bool g_skipCheck = envOn("SR_NATIVE_CONST_SKIP_CHECK");
const bool g_skipMut = envOn("SR_NATIVE_CONST_SKIP_MUT");
#else
// SR_ORACLE=0: the skip's CHECK and its mutation test are compiled out (both read as unset).
constexpr bool g_skipCheck = false, g_skipMut = false;
#endif

struct SkipSlot {
  uint32_t srcVA, gen, page, pad;
  __m128i val;
};
constexpr uint32_t kSkipSlots = kUpSpan / 16u;
constexpr uint32_t kSkipQueue = 256;
struct SkipState {
  const uint8_t* devH = nullptr;
  int usable = -1;  // -1 unknown, 0 off for this thread, 1 on
  uint32_t nq = 0, lastQ = ~0u;
  uint32_t calls = 0;
  uint64_t ent = 0, regSkip = 0, regCopy = 0, regRec = 0, regNoTrack = 0, regUnprot = 0, queued = 0,
           chk = 0;
  SkipSlot slot[kSkipSlots];
  uint32_t queue[kSkipQueue];
};
std::atomic<uint64_t> g_skEnt{0}, g_skSkip{0}, g_skCopy{0}, g_skRec{0}, g_skNoTrack{0}, g_skUnprot{0},
    g_skQueued{0}, g_skChk{0}, g_skBad{0}, g_skCalls{0};
std::atomic<uint32_t> g_mutPage{~0u};
std::atomic<uint64_t> g_mutFires{0};

// The write watch's page for a guest VA after the alias fold: the 0x8 / 0xA windows, else ~0u.
inline uint32_t skipPage(uint32_t a) {
  return (a >= 0x80000000u && a < 0xC0000000u) ? ((a & 0x1FFFFFFFu) >> 12) : ~0u;
}

void skipReport() {
  const sr::kernel::gww::Stats w = sr::kernel::gww::stats();
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[CONST_SKIP] calls=%llu entries=%llu regs skipped=%llu copied=%llu (recorded %llu, untracked %llu, "
              "unprotected %llu) pagesQueued=%llu | check regs=%llu bad=%llu | mut page=%08X fires=%llu | "
              "watch faults=%llu protect runs=%llu",
              (unsigned long long)g_skCalls.load(std::memory_order_relaxed),
              (unsigned long long)g_skEnt.load(std::memory_order_relaxed),
              (unsigned long long)g_skSkip.load(std::memory_order_relaxed),
              (unsigned long long)g_skCopy.load(std::memory_order_relaxed),
              (unsigned long long)g_skRec.load(std::memory_order_relaxed),
              (unsigned long long)g_skNoTrack.load(std::memory_order_relaxed),
              (unsigned long long)g_skUnprot.load(std::memory_order_relaxed),
              (unsigned long long)g_skQueued.load(std::memory_order_relaxed),
              (unsigned long long)g_skChk.load(std::memory_order_relaxed),
              (unsigned long long)g_skBad.load(std::memory_order_relaxed),
              g_mutPage.load(std::memory_order_relaxed) << 12,
              (unsigned long long)g_mutFires.load(std::memory_order_relaxed), (unsigned long long)w.faults,
              (unsigned long long)w.protects);
}

// This thread's skip state when the skip can run here, else null.
SkipState* skipState(uint8_t* base, const uint8_t* devH) {
  thread_local SkipState* t_sk = nullptr;
  if (!t_sk) t_sk = new SkipState();
  SkipState& S = *t_sk;
  if (S.usable < 0) {
    uint8_t* gb = nullptr;
    size_t gs = 0;
    sr::kernel::getGlobalGuestMemory(&gb, &gs);
    S.usable = (gb == base && !sr::kernel::g_physUnifyActive && sr::kernel::gww::enabled()) ? 1 : 0;
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[CONST_SKIP] thread %lu: skip %s (base %s global, phys unify %d, write watch %d) check=%d mut=%d",
                (unsigned long)GetCurrentThreadId(), S.usable ? "ON" : "OFF", gb == base ? "==" : "!=",
                int(sr::kernel::g_physUnifyActive), int(sr::kernel::gww::enabled()), int(g_skipCheck),
                int(g_skipMut));
  }
  if (!S.usable) return nullptr;
  if (S.devH != devH) {
    std::memset(S.slot, 0, sizeof(S.slot));
    S.devH = devH;
  }
  return &S;
}

inline void skipQueue(SkipState& S, uint32_t pg) {
  if (pg == S.lastQ) return;
  S.lastQ = pg;
  if (S.nq == kSkipQueue) {
    sr::kernel::gww::protectForValidation(S.queue, S.nq);
    S.nq = 0;
  }
  S.queue[S.nq++] = pg;
  S.queued++;
}

// Copy register i of an entry from its source and record what the next skip needs.
inline void skipCopyReg(SkipState& S, SkipSlot& x, uint8_t* d, const uint8_t* s, uint32_t a, uint32_t srcVA) {
  const uint32_t pg = skipPage(a), pgEnd = skipPage(a + 15u);
  const bool track = pg != ~0u && pg == pgEnd;
  const uint32_t g1 = track ? sr::kernel::gww::protGen(pg) : 0u;
  const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
  _mm_storeu_si128(reinterpret_cast<__m128i*>(d), v);
  S.regCopy++;
  if (!track) {
    x.gen = 0;
    S.regNoTrack++;
    return;
  }
#if SR_ORACLE
  if (g_skipMut) [[unlikely]] {
    // The mutation picks the first page (after ~4M entries) whose recorded value really changed.
    if (x.srcVA == srcVA && x.gen && x.page == pg &&
        _mm_movemask_epi8(_mm_cmpeq_epi8(v, x.val)) != 0xFFFF &&
        g_skEnt.load(std::memory_order_relaxed) > (1ull << 22)) {
      uint32_t none = ~0u;
      if (g_mutPage.compare_exchange_strong(none, pg, std::memory_order_acq_rel))
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[CONST_SKIP] MUT: the watch on page %08X is now ignored (value at %08X changed)", pg << 12,
                    srcVA);
    }
    if (pg == g_mutPage.load(std::memory_order_relaxed)) {
      x = SkipSlot{srcVA, 1u, pg, 0u, v};  // recorded as clean, whatever the watch says
      S.regRec++;
      return;
    }
  }
#endif  // SR_ORACLE
  std::atomic_thread_fence(std::memory_order_acquire);  // the source load stays before the re-read
  const uint32_t g2 = g1 ? sr::kernel::gww::protGen(pg) : 0u;
  if (g1 && g2 == g1) {
    x = SkipSlot{srcVA, g1, pg, 0u, v};
    S.regRec++;
    return;
  }
  x.gen = 0;
  S.regUnprot++;
  if (!g1) skipQueue(S, pg);
}

// The F entry with the skip: same contract as applyF (false = outside the fast path, nothing done).
inline bool applyFSkip(SkipState& S, uint8_t* base, uint8_t* devH, bool vs, uint32_t reg, uint32_t cnt,
                       uint32_t src, uint64_t mask, bool vac, uint64_t& nd) {
  const uint32_t off = fDestOff(vs, reg);
  const uint32_t len = cnt * 16u;
  if (uint64_t(off) + len > kUpSpan) return false;
  const uint8_t* s = hostRange(base, src, len, vac);
  if (!s || (len && s < devH + kUpSpan && devH < s + len)) return false;
  S.ent++;
  SkipSlot* sl = &S.slot[off >> 4];
  uint8_t* d = devH + off;
  const uint32_t a0 = uint32_t(s - base);
  const uint32_t mutPg = g_skipMut ? g_mutPage.load(std::memory_order_relaxed) : ~0u;
  for (uint32_t i = 0; i < cnt; i++) {
    SkipSlot& x = sl[i];
    const uint32_t srcVA = src + 16u * i;
    const bool clean = x.srcVA == srcVA && x.gen != 0 &&
                       (x.page == mutPg || sr::kernel::gww::protGen(x.page) == x.gen) &&
                       _mm_movemask_epi8(_mm_cmpeq_epi8(
                           _mm_loadu_si128(reinterpret_cast<const __m128i*>(d + 16u * i)), x.val)) == 0xFFFF;
    if (!clean) {
      skipCopyReg(S, x, d + 16u * i, s + 16u * i, a0 + 16u * i, srcVA);
      continue;
    }
    S.regSkip++;
#if SR_ORACLE
    if (g_skipCheck) [[unlikely]] {
      S.chk++;
      const __m128i real = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 16u * i));
      if (_mm_movemask_epi8(_mm_cmpeq_epi8(real, x.val)) != 0xFFFF) {
        const uint64_t bad = g_skBad.fetch_add(1, std::memory_order_relaxed) + 1;
        if (x.page == mutPg) g_mutFires.fetch_add(1, std::memory_order_relaxed);
        if (bad <= 16)
          SR_LOG_BOOT(sr::LogSeverity::Warn,
                      "[CONST_SKIP] CHECK MISMATCH #%llu %s reg=%u src=%08X page=%08X gen=%u now=%u%s",
                      (unsigned long long)bad, vs ? "VS" : "PS", reg + i, srcVA, x.page << 12, x.gen,
                      sr::kernel::gww::protGen(x.page), x.page == mutPg ? " (mutated page)" : "");
        S.regSkip--;
        skipCopyReg(S, x, d + 16u * i, s + 16u * i, a0 + 16u * i, srcVA);
      }
    }
#endif  // SR_ORACLE
  }
  uint8_t* dirty = devH + (vs ? 0u : 8u);
  nd = rdBE64(dirty) | mask;
  wrBE64(dirty, nd);
  return true;
}

// End of an upload call: protect the queued source pages, publish the counts every 4096 calls.
void skipEndCall(SkipState& S) {
  if (S.nq) {
    sr::kernel::gww::protectForValidation(S.queue, S.nq);
    S.nq = 0;
    S.lastQ = ~0u;
  }
  if (++S.calls < 4096u) return;
  g_skEnt.fetch_add(S.ent, std::memory_order_relaxed);
  g_skSkip.fetch_add(S.regSkip, std::memory_order_relaxed);
  g_skCopy.fetch_add(S.regCopy, std::memory_order_relaxed);
  g_skRec.fetch_add(S.regRec, std::memory_order_relaxed);
  g_skNoTrack.fetch_add(S.regNoTrack, std::memory_order_relaxed);
  g_skUnprot.fetch_add(S.regUnprot, std::memory_order_relaxed);
  g_skQueued.fetch_add(S.queued, std::memory_order_relaxed);
  g_skChk.fetch_add(S.chk, std::memory_order_relaxed);
  const uint64_t n = g_skCalls.fetch_add(S.calls, std::memory_order_relaxed) + S.calls;
  const bool due = (n >> 18) != ((n - S.calls) >> 18);
  S.calls = 0;
  S.ent = S.regSkip = S.regCopy = S.regRec = S.regNoTrack = S.regUnprot = S.queued = S.chk = 0;
  if (due) skipReport();
}

#endif  // SR_ORACLE
template <bool VS>
void uploadAll(PPCContext& ctx, uint8_t* base, void (*guestWhole)(PPCContext&, uint8_t*),
               void (*leafF)(PPCContext&, uint8_t*), void (*setI)(PPCContext&, uint8_t*),
               void (*setB)(PPCContext&, uint8_t*)) {
  const uint32_t dev = PPC_LOAD_U32(kDevPtrVA);
  const bool vac = rangeChecksVacuous(base);
  uint8_t* devH = hostRange(base, dev, kUpSpan, vac);
  if (!devH) {
    g_upWhole.fetch_add(1, std::memory_order_relaxed);
    guestWhole(ctx, base);
    return;
  }
#if SR_ORACLE
  if (g_uploadCheck) {
    uploadCheck<VS>(ctx, base, devH, dev, vac, guestWhole);
    return;
  }
#endif
  // stwu r1,-144(r1)
  const uint32_t oldR1 = ctx.r1.u32;
  const uint32_t r1 = oldR1 - 144u;
  PPC_STORE_U32(r1, oldR1);
  ctx.r1.u32 = r1;
  uint32_t p = ctx.r3.u32 + (VS ? 12u : 8u);
  uint32_t nNative = 0, nLeaf = 0;
#if SR_ORACLE
  uint32_t stEnt = 0, stEqDirty = 0, stEqClean = 0;
#endif
  bool lastNative = false;
  uint32_t lReg = 0, lCnt = 0, lSrc = 0;
  uint64_t lMask = 0, lNd = 0;
#if SR_ORACLE
  SkipState* sk = skipOn() ? skipState(base, devH) : nullptr;
#endif
  for (uint32_t id = PPC_LOAD_U16(p); id; p += 4, id = PPC_LOAD_U16(p)) {
    const uint32_t reg = PPC_LOAD_U8(p + 2), cnt = PPC_LOAD_U8(p + 3);
    const uint32_t src = PPC_LOAD_U32(((id << 2) & 0xFFFFFFFCu) + kParamTableVA);
    const uint64_t mask = groupMask(reg, cnt);
    uint64_t nd = 0;
    bool ok;
#if SR_ORACLE
    if (g_uploadStats) [[unlikely]] {
      const int st = statEntry(base, devH, VS, reg, cnt, src, mask, vac);
      stEnt++;
      if (st == 1) stEqDirty++;
      if (st == 2) stEqClean++;
    }
#endif
    if (sr::native::g_anyOn) [[unlikely]] {
      static const int idx = sr::native::entryIndex(VS ? 0x820F4920u : 0x820F4A30u);
      sr::native::XdkScope scope(idx, ctx);
#if SR_ORACLE
      ok = sk ? applyFSkip(*sk, base, devH, VS, reg, cnt, src, mask, vac, nd)
              : applyF(base, devH, devH, VS, reg, cnt, src, mask, vac, nd);
    } else if (sk) {
      ok = applyFSkip(*sk, base, devH, VS, reg, cnt, src, mask, vac, nd);
#else
      ok = applyF(base, devH, devH, VS, reg, cnt, src, mask, vac, nd);
#endif
    } else {
      ok = applyF(base, devH, devH, VS, reg, cnt, src, mask, vac, nd);
    }
    if (ok) {
      nNative++;
      lastNative = true;
      lReg = reg, lCnt = cnt, lSrc = src, lMask = mask, lNd = nd;
    } else {
      nLeaf++;
      lastNative = false;
      ctx.r3.u64 = dev;
      ctx.r4.u64 = reg;
      ctx.r5.u64 = src;
      ctx.r6.u64 = cnt;
      ctx.r7.u64 = mask;
      leafF(ctx, base);
    }
  }
#if SR_ORACLE
  if (sk) skipEndCall(*sk);
#endif
  if (lastNative) {
    // What the guest loop's last leaf call leaves: its stack stores (std r7,48(r1); stw r3,-32(r1))
    // and registers (r3-r7 = its arguments, r9 = mask, r10 = new dirty word, r11 = dev).
    PPC_STORE_U64(r1 + 48u, lMask);
    PPC_STORE_U32(r1 - 32u, dev);
    ctx.r3.u64 = dev;
    ctx.r4.u64 = lReg;
    ctx.r5.u64 = lSrc;
    ctx.r6.u64 = lCnt;
    ctx.r7.u64 = lMask;
    ctx.r9.u64 = lMask;
    ctx.r10.u64 = lNd;
    ctx.r11.u64 = dev;
  }
  // I list, then B list: the guest's calls.
  void (*const ib[2])(PPCContext&, uint8_t*) = {setI, setB};
  for (auto* fn : ib) {
    p += 4;
    for (uint32_t id = PPC_LOAD_U16(p); id; p += 4, id = PPC_LOAD_U16(p)) {
      ctx.r11.u64 = (id << 2) & 0xFFFFFFFCu;
      ctx.r6.u64 = PPC_LOAD_U8(p + 3);
      ctx.r3.u64 = dev;
      ctx.r4.u64 = PPC_LOAD_U8(p + 2);
      ctx.r5.u64 = PPC_LOAD_U32(ctx.r11.u32 + kParamTableVA);
      fn(ctx, base);
    }
  }
  // Semantic callbacks {fn, u16 paramId, u8 reg, u8 count}, terminated by paramId 0.
  p += 4;
  for (uint32_t pid = PPC_LOAD_U16(p + 4); pid; p += 8, pid = PPC_LOAD_U16(p + 4)) {
    ctx.r10.u64 = pid;
    uint32_t fn;
    if (pid >= 0x400u && pid < 0x800u) {
      ctx.r9.u64 = kTexRegistryVA;
      PPC_STORE_U8(ctx.r1.u32 + 88, uint8_t(VS ? 1 : 0));
      ctx.r3.s64 = ctx.r1.s64 + 80;
      PPC_STORE_U8(ctx.r1.u32 + 80, PPC_LOAD_U8(p + 6));
      const uint32_t tex = PPC_LOAD_U32(((pid << 5) & 0x1FFFE0u) + kTexRegistryVA);
      ctx.r11.u64 = tex;
      PPC_STORE_U8(ctx.r1.u32 + 81, PPC_LOAD_U8(p + 7));
      fn = PPC_LOAD_U32(p);
      ctx.r10.u64 = fn;
      PPC_STORE_U32(ctx.r1.u32 + 84, tex);
    } else {
      ctx.r4.u64 = PPC_LOAD_U8(p + 7);
      ctx.r5.s64 = VS ? 1 : 0;
      ctx.r3.u64 = PPC_LOAD_U8(p + 6);
      fn = PPC_LOAD_U32(p);
      ctx.r11.u64 = fn;
    }
    PPC_CALL_INDIRECT_FUNC(fn);
  }
  // addi r1,r1,144
  ctx.r1.s64 = ctx.r1.s64 + 144;
#if SR_ORACLE
  if (g_uploadStats) [[unlikely]] {
    g_stEnt.fetch_add(stEnt, std::memory_order_relaxed);
    g_stEqDirty.fetch_add(stEqDirty, std::memory_order_relaxed);
    g_stEqClean.fetch_add(stEqClean, std::memory_order_relaxed);
    if (stEnt && stEqDirty == stEnt) g_stAllNoop.fetch_add(1, std::memory_order_relaxed);
  }
#endif
  countUpload(nNative, nLeaf);
}

}  // namespace
}  // namespace sr::native::leaf

extern "C" void __imp__sub_8260B660(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8260B2A0(PPCContext& __restrict ctx, uint8_t* base);
static void guestUploadPs(PPCContext& ctx, uint8_t* base) { __imp__sub_8260B660(ctx, base); }
static void guestUploadVs(PPCContext& ctx, uint8_t* base) { __imp__sub_8260B2A0(ctx, base); }
static void leafPsF(PPCContext& ctx, uint8_t* base) { sub_820F4A30(ctx, base); }
static void leafVsF(PPCContext& ctx, uint8_t* base) { sub_820F4920(ctx, base); }
static void psI(PPCContext& ctx, uint8_t* base) { sub_820F4C40(ctx, base); }
static void psB(PPCContext& ctx, uint8_t* base) { sub_820F4B98(ctx, base); }
static void vsI(PPCContext& ctx, uint8_t* base) { sub_820F4BF0(ctx, base); }
static void vsB(PPCContext& ctx, uint8_t* base) { sub_820F4B40(ctx, base); }

// ShaderPs_UploadAllConstants (NATIVE N1 phase 1, SR_NATIVE_CONST_UPLOAD; see above).
void sub_8260B660(PPCContext& __restrict ctx, uint8_t* base) {
  if (!sr::native::leaf::g_uploadOn
#if SR_ORACLE  // the CHECK runs uploadAll too (SR_ORACLE=0: no CHECK)
      && !sr::native::leaf::g_uploadCheck
#endif
  ) {
    __imp__sub_8260B660(ctx, base);
    return;
  }
  sr::native::leaf::uploadAll<false>(ctx, base, guestUploadPs, leafPsF, psI, psB);
}
// ShaderVs_UploadAllConstants (NATIVE N1 phase 1, SR_NATIVE_CONST_UPLOAD; see above).
void sub_8260B2A0(PPCContext& __restrict ctx, uint8_t* base) {
  if (!sr::native::leaf::g_uploadOn
#if SR_ORACLE  // the CHECK runs uploadAll too (SR_ORACLE=0: no CHECK)
      && !sr::native::leaf::g_uploadCheck
#endif
  ) {
    __imp__sub_8260B2A0(ctx, base);
    return;
  }
  sr::native::leaf::uploadAll<true>(ctx, base, guestUploadVs, leafVsF, vsI, vsB);
}
