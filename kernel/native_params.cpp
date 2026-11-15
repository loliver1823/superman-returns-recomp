// NATIVE shader-parameter writer (iter-467 guestcode round 5): RenderStateBlock_ApplyShaderParams
// sub_826092B8 as host code. 0.39 ms self of the 8.85 ms player-profile render tick (pure
// ingame window, docs/PERFORMANCE.md 2.11), a leaf the render walk calls per state block.
//
// What it does (r3 = the block's parameter list): for each entry {u16 id, u16 len, u16 off, -}
// until id == 0, it sets bit (id & 31) of the parameter dirty mask word 0x82998A00[id >> 5] and
// points the parameter table 0x82998B00[id] at the value - inline data at payload + off (len !=
// 0; the next entry follows the inline data) or the pointer stored in the entry (len == 0). The
// constant uploads (ShaderPs/Vs_UploadAllConstants) then read every value through that table.
//
// The transcription makes the same guest loads and stores in the same order (the entry's id is
// re-read where the guest re-reads it) and leaves r3 where the guest leaves it.
//
// The observer classifies every applied parameter (inline vs external) and whether the first 16
// bytes at its address changed since that address was last applied, and counts the distinct
// ids, addresses and 4 KB pages involved - the sizing input for constant-upload change tracking
// (docs/PERFORMANCE.md 2.14).
//
// GATES. SR_NATIVE_PARAMS (baked default ON under the iter-467 per-call rule, =0 = the guest body: CHECK 0 bad, faster per call, live 3x3 + in-roll A/B no regression - PERFORMANCE 2.16). SR_NATIVE_PARAMS_CHECK=1: per
// call, walk the list read-only to collect every word the call writes, run the guest original,
// keep its result, restore those words, run the native version, compare words + r3, put the
// guest's result back. SR_NATIVE_PARAMS_AB=1: in-process timing A/B.
// SR_PARAM_OBS=1 (named diagnostic, observe-only, reads guest memory): after each call, walk the
// list and classify every parameter it points at - inline (block data) or external (a pointer
// stored in the entry) - and whether the first 16 bytes there changed since that address was
// last applied. The sizing input for a constant-upload change-tracking design (2.14).
// C1: the CHECK, the A/B and SR_PARAM_OBS are development oracles, compiled out under SR_ORACLE=0
// (kernel/sr_oracle.hpp): there the three gates read as unset (no env read), so the release runs
// SR_NATIVE_PARAMS's native path or the guest body only.

#include "native_vmx.hpp"
#include "ab_toggle.hpp"
#include "sr_oracle.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" void __imp__sub_826092B8(PPCContext& __restrict ctx, uint8_t* base);

namespace sr::native::params {
namespace {

using namespace sr::native::vmx;

bool paramsOn() {  // read at the point of use (SR_AB_TOGGLE, kernel/ab_toggle.hpp)
  static const bool v = [] { const char* e = std::getenv("SR_NATIVE_PARAMS"); return !e || e[0] != '0'; }();
  static const int ab = sr::abt::reg("SR_NATIVE_PARAMS");
  return v && (ab < 0 || sr::abt::on(ab));
}
#if SR_ORACLE
bool paramsCheckOn() {
  static const bool v = [] { const char* e = std::getenv("SR_NATIVE_PARAMS_CHECK"); return e && e[0] == '1'; }();
  return v;
}
bool paramsAbOn() {
  static const bool v = [] { const char* e = std::getenv("SR_NATIVE_PARAMS_AB"); return e && e[0] == '1'; }();
  return v;
}

std::atomic<uint64_t> g_calls{0}, g_entries{0}, g_checked{0}, g_bad{0}, g_unchk{0};
AbTimer g_ab;
#endif  // SR_ORACLE

constexpr uint32_t kDirty = 0x82998A00u, kTable = 0x82998B00u;

inline uint32_t lhz(uint8_t* b, uint32_t va) {
  uint16_t v;
  std::memcpy(&v, gp(b, va), 2);
  return _byteswap_ushort(v);
}

// sub_826092B8; returns the guest's final r3
uint32_t applyShaderParams(uint8_t* b, uint32_t list, uint64_t& entries) {
  uint32_t r11 = list;
  uint32_t r3 = list + 8;
  for (;;) {
    ++entries;
    const uint32_t id = lhz(b, r11);
    const uint32_t w = (id >> 3) & 0x1FFFFFFCu;
    const uint32_t m = lwz(b, w + kDirty);
    stw(b, w + kDirty, (1u << (id & 0x1F)) | m);
    if (lhz(b, r11 + 2) != 0) {
      const uint32_t v = lhz(b, r11 + 4) + r3;
      const uint32_t id2 = lhz(b, r11);
      stw(b, (id2 << 2) + kTable, v);
      r11 = lhz(b, r11 + 2) + v;
    } else {
      const uint32_t id2 = lhz(b, r11);
      const uint32_t v = lwz(b, r3);
      r11 = r3 + 4;
      stw(b, (id2 << 2) + kTable, v);
    }
    const uint32_t next = lhz(b, r11);
    r3 = r11 + 8;
    if (next == 0) return r3;
  }
}

#if SR_ORACLE
void report() {
  double us[3];
  g_ab.means(us);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_PARAMS] calls=%llu entries=%llu checked=%llu checkBad=%llu unchecked=%llu | AB us/call "
              "guest=%.3f native=%.3f",
              (unsigned long long)g_calls.load(std::memory_order_relaxed),
              (unsigned long long)g_entries.load(std::memory_order_relaxed),
              (unsigned long long)g_checked.load(std::memory_order_relaxed),
              (unsigned long long)g_bad.load(std::memory_order_relaxed),
              (unsigned long long)g_unchk.load(std::memory_order_relaxed), us[0], us[1]);
}

void checkedCall(PPCContext& ctx, uint8_t* b) {
  // the read-only walk: every word the call writes (the dirty word, then the table slot)
  std::vector<uint32_t> addrs;
  {
    uint32_t r11 = ctx.r3.u32, r3 = r11 + 8;
    for (int guard = 0;; ++guard) {
      if (guard > 100000 || !sr::kernel::hostRangeReadable(gp(b, r11), 8)) {
        g_unchk.fetch_add(1, std::memory_order_relaxed);
        __imp__sub_826092B8(ctx, b);
        return;
      }
      const uint32_t id = lhz(b, r11);
      addrs.push_back(((id >> 3) & 0x1FFFFFFCu) + kDirty);
      addrs.push_back((id << 2) + kTable);
      if (lhz(b, r11 + 2) != 0) r11 = lhz(b, r11 + 2) + lhz(b, r11 + 4) + r3;
      else r11 = r3 + 4;
      r3 = r11 + 8;
      if (!sr::kernel::hostRangeReadable(gp(b, r11), 2)) {
        g_unchk.fetch_add(1, std::memory_order_relaxed);
        __imp__sub_826092B8(ctx, b);
        return;
      }
      if (lhz(b, r11) == 0) break;
    }
  }
  std::vector<uint32_t> pre(addrs.size()), guest(addrs.size());
  for (size_t i = 0; i < addrs.size(); ++i) pre[i] = lwz(b, addrs[i]);
  const PPCContext in = ctx;
  __imp__sub_826092B8(ctx, b);
  const PPCContext guestOut = ctx;
  for (size_t i = 0; i < addrs.size(); ++i) guest[i] = lwz(b, addrs[i]);
  for (size_t i = addrs.size(); i-- > 0;) stw(b, addrs[i], pre[i]);  // reverse: the first pre wins
  ctx = in;
  uint64_t entries = 0;
  const uint32_t r3 = applyShaderParams(b, ctx.r3.u32, entries);
  size_t bad = addrs.size();
  for (size_t i = 0; i < addrs.size(); ++i)
    if (lwz(b, addrs[i]) != guest[i]) {
      bad = i;
      break;
    }
  const bool r3bad = r3 != guestOut.r3.u32;
  for (size_t i = 0; i < addrs.size(); ++i) stw(b, addrs[i], guest[i]);
  ctx = guestOut;
  g_checked.fetch_add(1, std::memory_order_relaxed);
  g_entries.fetch_add(entries, std::memory_order_relaxed);
  if (bad != addrs.size() || r3bad) {
    const uint64_t m = g_bad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (m <= 20)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[NATIVE_PARAMS] MISMATCH #%llu list=%08X word %08X guest=%08X native=%08X r3 guest=%08X native=%08X",
                  (unsigned long long)m, in.r3.u32, bad < addrs.size() ? addrs[bad] : 0u,
                  bad < addrs.size() ? guest[bad] : 0u, bad < addrs.size() ? lwz(b, addrs[bad]) : 0u,
                  guestOut.r3.u32, r3);
  }
}


// ---- SR_PARAM_OBS (observe-only) -----------------------------------------------------------
bool paramObsOn() {
  static const bool v = [] { const char* e = std::getenv("SR_PARAM_OBS"); return e && e[0] == '1'; }();
  return v;
}
struct ObsStats {
  uint64_t calls = 0, entries = 0, inl = 0, ext = 0, same = 0, changed = 0, fresh = 0, sameExt = 0,
           changedExt = 0, inlBytes = 0;
  std::unordered_map<uint32_t, uint64_t> lastHash;  // value address -> hash of its first 16 bytes
  std::unordered_set<uint32_t> ids, pagesInl, pagesExt, pagesChanged;
};
ObsStats* g_obs = nullptr;

inline uint64_t hash16(uint8_t* b, uint32_t va) {
  uint64_t a, c;
  std::memcpy(&a, gp(b, va), 8);
  std::memcpy(&c, gp(b, va + 8), 8);
  return (a * 0x9E3779B97F4A7C15ull) ^ (c + 0x632BE59BD9B4E019ull + (a >> 29));
}

void observe(uint8_t* b, uint32_t list) {
  if (!g_obs) g_obs = new ObsStats();
  ObsStats& o = *g_obs;
  ++o.calls;
  uint32_t r11 = list, r3 = list + 8;
  for (int guard = 0; guard < 100000; ++guard) {
    if (!sr::kernel::hostRangeReadable(gp(b, r11), 8)) return;
    const uint32_t id = lhz(b, r11), len = lhz(b, r11 + 2);
    uint32_t v;
    bool ext;
    if (len != 0) {
      v = lhz(b, r11 + 4) + r3;
      r11 = len + v;
      ext = false;
      o.inlBytes += len;
    } else {
      v = lwz(b, r3);
      r11 = r3 + 4;
      ext = true;
    }
    ++o.entries;
    o.ids.insert(id);
    (ext ? o.ext : o.inl) += 1;
    (ext ? o.pagesExt : o.pagesInl).insert(v >> 12);
    if (v && sr::kernel::hostRangeReadable(gp(b, v), 16)) {
      const uint64_t h = hash16(b, v);
      auto it = o.lastHash.find(v);
      if (it == o.lastHash.end()) {
        ++o.fresh;
        o.lastHash.emplace(v, h);
      } else if (it->second == h) {
        ++o.same;
        if (ext) ++o.sameExt;
      } else {
        ++o.changed;
        if (ext) ++o.changedExt;
        o.pagesChanged.insert(v >> 12);
        it->second = h;
      }
    }
    if (!sr::kernel::hostRangeReadable(gp(b, r11), 2) || lhz(b, r11) == 0) break;
    r3 = r11 + 8;
  }
  if ((o.calls & 0x3FFFF) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[PARAM_OBS] calls=%llu entries=%llu inline=%llu (bytes %llu) external=%llu | value vs last apply at "
                "that address: same=%llu changed=%llu new=%llu (external same=%llu changed=%llu) | ids=%zu addrs=%zu "
                "pages inline=%zu external=%zu changed=%zu",
                (unsigned long long)o.calls, (unsigned long long)o.entries, (unsigned long long)o.inl,
                (unsigned long long)o.inlBytes, (unsigned long long)o.ext, (unsigned long long)o.same,
                (unsigned long long)o.changed, (unsigned long long)o.fresh, (unsigned long long)o.sameExt,
                (unsigned long long)o.changedExt, o.ids.size(), o.lastHash.size(), o.pagesInl.size(),
                o.pagesExt.size(), o.pagesChanged.size());
}
#endif  // SR_ORACLE

}  // namespace
}  // namespace sr::native::params

// RenderStateBlock_ApplyShaderParams (NATIVE shader-parameter writer; see the top of this file).
void sub_826092B8(PPCContext& __restrict ctx, uint8_t* base) {
  using namespace sr::native::params;
#if SR_ORACLE
  if (paramObsOn()) {
    const uint32_t list = ctx.r3.u32;
    __imp__sub_826092B8(ctx, base);
    observe(base, list);
    return;
  }
  if (paramsAbOn()) {
    const uint64_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) {
      double t[3];
      g_ab.means(t);
    }
    const int mode = int((n >> 9) & 1);
    const uint64_t t0 = __rdtsc();
    if (mode == 0) {
      __imp__sub_826092B8(ctx, base);
    } else {
      uint64_t e = 0;
      ctx.r3.u64 = applyShaderParams(base, ctx.r3.u32, e);
    }
    g_ab.add(mode, __rdtsc() - t0);
    if ((n & 0x3FFFF) == 0) report();
    return;
  }
  if (paramsCheckOn()) {
    const uint64_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    checkedCall(ctx, base);
    if ((n & 0x3FFFF) == 0) report();
    return;
  }
#endif  // SR_ORACLE
  if (paramsOn()) {
    uint64_t e = 0;
    ctx.r3.u64 = applyShaderParams(base, ctx.r3.u32, e);
    return;
  }
  __imp__sub_826092B8(ctx, base);
}
