// NATIVE ROADMAP N1 mode A: record-only native draw-state tracker + the
// in-process per-draw check against the PM4 oracle. Contract: native_device.hpp.
//
// Observe-only: reads the guest device the XDK maintains, writes nothing to the
// guest, never clears a dirty mask, and always lets the XDK run exactly as before.

#include "native_device.hpp"
#include "native_vs_patch.hpp"
#include "pm4_live_trace.hpp"
#include "shader_cache.hpp"

#include "xenos_state.hpp"
#include "../app/log.hpp"
#include "../kernel/guest_context.hpp"
#include "../kernel/native_brackets.hpp"
#include "../kernel/native_const_dedup.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// kernel/hle_api_observe.cpp (iter-442 S4.0): XXH3 of a bound shader object's ucode
// section, the shader-cache key the CP computes at IM_LOAD.
uint64_t s4HashShaderObjUncached(uint32_t obj, bool isVS, bool* usedTable);

namespace sr::native::dev {

namespace {
bool envOn(const char* n) {
  const char* e = std::getenv(n);
  return e && e[0] == '1';
}
uint32_t envU32(const char* n, uint32_t def) {
  const char* e = std::getenv(n);
  return (e && *e) ? uint32_t(std::strtoul(e, nullptr, 0)) : def;
}
}  // namespace

bool g_recordOn = envOn("SR_NATIVE_RECORD") || envOn("SR_NATIVE_DRAW");  // mode B 1a implies the record

namespace {

// ---- The projection: device shadow -> Xenos registers ----------------------
// Packed register shadow (ENGINE_MAP "NATIVE N0", inferred from anchors and
// checked here at runtime): each run is contiguous in the device and in the
// register file. The 3-dword scissor non-run at +0x2840 is mapped to
// 0x2080..0x2082 (PA_SC_WINDOW_OFFSET / SCISSOR_TL / BR) - a hypothesis this
// check settles.
struct Run {
  uint16_t devOff, reg, count;
};
constexpr Run kRuns[] = {
    {0x2800, 0x2000, 16}, {0x2840, 0x2080, 3},  {0x284C, 0x2100, 21}, {0x28A0, 0x2180, 5},
    {0x28B4, 0x2200, 12}, {0x28E4, 0x2280, 21}, {0x2938, 0x2300, 38}, {0x29D0, 0x2380, 8},
};
constexpr int kRegN = 124;
constexpr uint32_t kFetchOff = 0x400, kFetchReg = 0x4800, kFetchN = 192;  // 32 slots x 6
constexpr uint32_t kAluOff = 0x700, kAluReg = 0x4000, kAluN = 2048;       // VS c0-255, PS c0-255
constexpr uint32_t kAluBlock = 64, kAluBlocks = kAluN / kAluBlock;        // 16 vec4 per block
constexpr uint32_t kBoolOff = 0x2700, kBoolReg = 0x4900, kBoolN = 40;     // 8 bool + 32 loop
constexpr uint32_t kDevSpan = 0x29F0;                                     // bytes read per draw
constexpr uint32_t kDevPs = 12416, kDevVs = 12420;  // bound shader objects (ENGINE_MAP 6g.3)

uint16_t g_regOf[kRegN];
uint16_t g_devOf[kRegN];
bool g_tablesBuilt = [] {
  int i = 0;
  for (const Run& r : kRuns)
    for (uint16_t k = 0; k < r.count; k++, i++) {
      g_regOf[i] = uint16_t(r.reg + k);
      g_devOf[i] = uint16_t(r.devOff + 4 * k);
    }
  return i == kRegN;
}();

inline uint32_t be32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return _byteswap_ulong(v);
}

// Hash of one 64-dword constant block as BIG-ENDIAN bytes (the shadow's form);
// the CP side byte-swaps its host-order values before hashing.
uint64_t hashBlock(const uint8_t* p) {
  uint64_t h = 0x84222325CBF29CE4ull;
  for (uint32_t i = 0; i < kAluBlock * 4; i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = (h ^ w) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 31;
  }
  return h;
}

// ---- Records (seqlock ring keyed by bracket callId) ------------------------
struct Rec {
  std::atomic<uint32_t> seq{0};      // callId once the pre half is complete
  std::atomic<uint32_t> postSeq{0};  // callId once the post half is complete
  uint32_t dev = 0;
  uint32_t args[4] = {};
  uint8_t kind = 0;
  uint32_t pre[kRegN];
  uint32_t post[kRegN];
  uint32_t fetch[kFetchN];
  uint32_t boolc[kBoolN];
  uint64_t alu[kAluBlocks];
  uint64_t aluPost[kAluBlocks];
  uint32_t fetchPost[kFetchN];
  uint64_t vsPre = 0, psPre = 0, vsPost = 0, psPost = 0;  // bound-object ucode hashes
  uint32_t vsObj = 0;
  uint32_t vsUcodeVA = 0;  // *(vsObj + 0x20)
  uint64_t vsNative = 0;   // XXH3 of the natively patched VS program (gfx/native_vs_patch)
  uint32_t upStride = 0;   // DrawVerticesUP / BeginVertices stride argument (bytes)
  uint64_t upHash = 0;     // DrawVerticesUP: hash of the caller's count*stride vertex bytes at entry
  uint32_t upBytes = 0;
  uint64_t progPre = 0;    // XXH3 of the IM_LOAD range [a2, a2+size) before the call
  uint8_t zpass = 0;              // recorded inside BeginZPass..EndZPass
  uint32_t modeZ = 0;             // RB_MODECONTROL of the Z-pass execution (modeControl)
  std::atomic<uint32_t> checks{0};  // CP executions of this record's draw (CP thread)
};

uint32_t g_cap = 0, g_mask = 0;
std::atomic<Rec*> g_recs{nullptr};
uint32_t g_every = 1;
Rec* recs() {
  static Rec* r = [] {
    uint32_t cap = envU32("SR_NATIVE_RECORD_CAP", 16384);
    uint32_t c = 1;
    while (c < cap && c < (1u << 20)) c <<= 1;
    g_cap = c;
    g_mask = c - 1;
    g_every = std::max<uint32_t>(1, envU32("SR_NATIVE_RECORD_EVERY", 1));
    Rec* p = new Rec[c];
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_REC] armed: record-only N1 (mode A) cap=%u records (%zu KB) every=%u", c,
                size_t(c) * sizeof(Rec) / 1024, g_every);
    return p;
  }();
  g_recs.store(r, std::memory_order_release);
  return r;
}

// Render-side counters (any thread; relaxed).
std::atomic<uint64_t> g_recorded{0}, g_nested{0}, g_noBracket{0}, g_badDev{0}, g_sampledOut{0};
std::atomic<uint64_t> g_kindCount[kDrawKinds];

thread_local uint32_t t_lastCallId = 0;
thread_local Rec* t_lastRec = nullptr;

// The flush-derived shader-control registers, computed natively from the bound
// shader objects exactly as GpuEmitDrawState_82108668 does (recomp 0x82108668..,
// IDA): vsHdr = vs + *(vs+896) + 872 (variant 1 = *(vs+904) when no PS is bound
// and vs[872] & 0x20); psHdr = ps + *(ps+64) + 40;
//   SQ_PROGRAM_CNTL      0x2180 = vsHdr[+8]  | psHdr[+8]
//   SQ_CONTEXT_MISC      0x2181 = vsHdr[+12] | psHdr[+12]
//   SQ_INTERPOLATOR_CNTL 0x2182 = psHdr[+24]  (unchanged without a PS)
// (the immediate-load path passes vsHdr[+8] through GpuLoadShaderImmediate_82108300
// by pointer; the check measures whether it ever changes it).
struct ShaderRegs {
  bool ok = false;
  uint32_t progCntl = 0, contextMisc = 0, interp = 0;
  bool haveInterp = false;
};
ShaderRegs computeShaderRegs(uint32_t devVA) {
  ShaderRegs r;
  const uint32_t vs = sr::kernel::readGuestU32BE(devVA + 12420u);
  const uint32_t ps = sr::kernel::readGuestU32BE(devVA + 12416u);
  if (vs < 0x10000u) return r;
  uint32_t vsHdr = vs + sr::kernel::readGuestU32BE(vs + 896u) + 872u;
  if (ps < 0x10000u && (sr::kernel::readGuestU32BE(vs + 872u) & 0x20u))
    vsHdr = vs + sr::kernel::readGuestU32BE(vs + 904u) + 872u;
  r.progCntl = sr::kernel::readGuestU32BE(vsHdr + 8u);
  r.contextMisc = sr::kernel::readGuestU32BE(vsHdr + 12u);
  if (ps >= 0x10000u) {
    const uint32_t psHdr = ps + sr::kernel::readGuestU32BE(ps + 64u) + 40u;
    r.progCntl |= sr::kernel::readGuestU32BE(psHdr + 8u);
    r.contextMisc |= sr::kernel::readGuestU32BE(psHdr + 12u);
    r.interp = sr::kernel::readGuestU32BE(psHdr + 24u);
    r.haveInterp = true;
  }
  r.ok = true;
  return r;
}
constexpr int kIdx2180 = 16 + 3 + 21;  // runs: 0x2000 x16, 0x2080 x3, 0x2100 x21, then 0x2180
constexpr int kIdx2208 = 16 + 3 + 21 + 5 + 8;  // 0x2200 run starts at 45; 0x2208 = +8
constexpr int kIdx2203 = 16 + 3 + 21 + 5 + 3;
constexpr int kIdx2102 = 16 + 3 + 2;  // VGT_INDX_OFFSET (0x2100 run starts at 19)

uint32_t rdG(uint32_t va) { return sr::kernel::readGuestU32BE(va); }

// The bound shaders' literal constants (HLSL `def`), which the XDK loads at shader
// bind with LOAD_ALU_CONSTANT straight from the shader object
// (GpuRingEmitFetchConstants_sub_82108470, IDA): table t = a2 + *(a2+20) (none if 0),
// entries [t+20, t+20+*(t+16)) of {u16 vec4 index (absolute, PS at 256+), u16 dword
// count (0 ends), u32 offset}, source VA = offset + a3. VS: a2 = vs+872, a3 = *(vs+32);
// PS: a2 = ps+40, a3 = *(ps+24). Overlaid on the shadow image (BE bytes) in `alu`.
void overlayLiterals(uint8_t* alu, uint32_t a2, uint32_t a3) {
  const uint32_t rel = rdG(a2 + 20u);
  if (!rel) return;
  const uint32_t t = a2 + rel;
  const uint32_t end = t + 20u + rdG(t + 16u);
  for (uint32_t e = t + 20u; e + 8u <= end && e - t < 0x4000u; e += 8u) {
    const uint32_t w = rdG(e);
    const uint32_t idx = w >> 16, cnt = w & 0xFFFFu;
    if (!cnt) break;
    const uint32_t src = rdG(e + 4u) + a3;
    const uint32_t dst = 16u * idx;
    if (dst + 4u * cnt > 2048u * 4u) continue;
    const uint8_t* s = sr::kernel::guestVAToHost(src);
    const uint8_t* s2 = sr::kernel::guestVAToHost(src + 4u * cnt - 4u);
    if (!s || s2 != s + 4u * cnt - 4u) continue;
    std::memcpy(alu + dst, s, 4u * cnt);
  }
}

// RB_MODECONTROL (0x2208) as GpuEmitDrawState_82108668 emits it: the shadow mode
// (dev+10452) forced to colour+depth (4) when a PS rebind is flushed (dirty bit 52)
// and to depth-only (5) with no PS; then, if dev.byte[10808] & 0x40, depth-only when
// there is no PS or the PS neither writes colour (psHdr[+28] & 0x30) nor alpha-tests
// (RB_COLORCONTROL dev+10428 & 8).
uint32_t modeControl(uint32_t devVA, uint32_t shadow, uint64_t dirty10) {
  const uint32_t ps = rdG(devVA + 12416u);
  uint32_t m = shadow;
  if (ps < 0x10000u) {
    m = (m & ~7u) | 5u;
  } else if (((dirty10 >> 52) & 1u) && (m & 7u) != 4u) {
    m = (m & ~7u) | 4u;
  }
  const uint8_t* b = sr::kernel::guestVAToHost(devVA + 10808u);
  if (b && (*b & 0x40u)) {
    bool depthOnly = ps < 0x10000u;
    if (!depthOnly) {
      const uint32_t psHdr = ps + rdG(ps + 64u) + 40u;
      depthOnly = !(rdG(devVA + 10428u) & 8u) && !(rdG(psHdr + 28u) & 0x30u);
    }
    if (depthOnly) m = (m & ~7u) | 5u;
  }
  return m;
}

// RB_HIZCONTROL (0x2203) bit 0 as GpuCmdUpdateSurfaceState_82107110 recomputes it:
// (dev.byte[10810] >> 5) & (RB_DEPTHCONTROL >> 1) & 1; bit 1 on the simple branch
// ((dev[11580] >> 20) & 2) == 0 -> (dev[11580] >> 20) & 1, else left as the shadow.
uint32_t hizControl(uint32_t devVA, uint32_t shadow, uint32_t depthControl) {
  const uint8_t* b = sr::kernel::guestVAToHost(devVA + 10810u);
  const uint32_t b10810 = b ? *b : 0u;
  const uint32_t dc = depthControl;
  uint32_t h = (shadow & ~1u) | ((b10810 >> 5) & (dc >> 1) & 1u);
  const uint32_t r9 = (rdG(devVA + 11580u) >> 20) & 0xFFFu;
  uint32_t bit;
  if (!(r9 & 2u)) {
    bit = r9 & 1u;
  } else {
    // notPs = !(psHdr[+8] bit 27); the depth func (dc bits 4-6) must be one HiZ can
    // test - {2,4,6} when hiz bit 4 is set, else {1,2,3}; with stencil (dc bit 0)
    // the front fail/zfail ops (dc & 0x3800, 0xE0000) must be KEEP, and with
    // back-face stencil (dc bit 7) the back ones (0xE0000000, 0x03800000) too.
    const uint32_t ps = rdG(devVA + 12416u);
    uint32_t psFlag = 0;
    if (ps >= 0x10000u) psFlag = (rdG(ps + rdG(ps + 64u) + 48u) >> 27) & 1u;
    const uint32_t notPs = psFlag ? 0u : 1u;
    const uint32_t zf = (dc >> 4) & 7u;
    const uint32_t okFunc = (h & 0x10u) ? (zf == 4u || zf == 6u || zf == 2u) : (zf == 1u || zf == 3u || zf == 2u);
    bit = notPs & h & (dc >> 1) & okFunc & 1u;
    if (dc & 1u) {
      bit &= ((dc & 0x3800u) == 0u) & ((dc & 0xE0000u) == 0u);
      if (dc & 0x80u) bit &= ((dc & 0xE0000000u) == 0u) & ((dc & 0x03800000u) == 0u);
    }
  }
  return (h & ~2u) | (bit << 1);
}

// The VS program the GPU runs for this draw, computed natively: the variant's ucode
// section (container table as in kernel/hle_api_observe s4HashShaderObjUncached) copied
// to a host buffer and patched by the host port of ShaderAssembleVertexFetches for the
// bound declaration (dev+11536) and stream strides (dev+12256). Cached by inputs: the
// patch rewrites every fetch instruction from the object's saved fetch words, so the
// result does not depend on the object's current (possibly re-patched) bytes.
std::atomic<uint64_t> g_vsPatchFail{0}, g_vsGarbageDiff{0}, g_vsPatchMisses{0};
std::atomic<uint64_t> g_vsNone[3];  // early outs: no container table / bad size / no declaration
std::atomic<uint64_t> g_ipChecks{0}, g_ipBad{0};  // in-place differential

// THE differential against the guest's own output: when the XDK patched the object IN
// PLACE during this draw (its ucode hash changed across the call), the post-draw bytes
// ARE the guest patcher's result for these inputs; the port run on a copy of them must
// reproduce them exactly (it rewrites every fetch slot from the object's saved words).
bool readStrides(uint32_t devVA, uint32_t upStrideBytes, uint8_t strides[16]);
void inPlaceDifferential(uint32_t devVA, uint32_t vs, uint32_t ps, uint32_t callId, uint32_t upStrideBytes) {
  if (vs < 0x10000u) return;
  const uint32_t variant = (ps < 0x10000u && (rdG(vs + 872u) & 0x20u)) ? 1u : 0u;
  const uint32_t hdr = vs + rdG(vs + 8u * (variant + 112u)) + 872u;
  const uint32_t dest = sr::native::vspatch::ucodeDestVA(vs, variant);
  const uint32_t n = rdG(hdr + 28u);
  if (!n || n > 41u) return;
  // The span the patch writes: [dest, dest + 12 * (max entry + 1)).
  const uint32_t table = ((rdG(hdr + 24u) + 9u) << 2) + hdr;
  uint32_t maxIdx = 0;
  for (uint32_t i = 0; i < n; i++) maxIdx = std::max(maxIdx, rdG(table + 4u * i) & 0xFFFu);
  const uint32_t bytes = 12u * (maxIdx + 1u);
  const uint8_t* h = sr::kernel::guestVAToHost(dest);
  const uint8_t* h2 = sr::kernel::guestVAToHost(dest + bytes - 1u);
  if (!h || h2 != h + bytes - 1u) return;
  std::vector<uint8_t> buf(h, h + bytes);
  uint8_t strides[16];
  if (!readStrides(devVA, upStrideBytes, strides)) return;
  if (!sr::native::vspatch::assembleVertexFetches(vs, rdG(devVA + 11536u), strides, variant, buf.data(), buf.size(),
                                                  0, 0))
    return;
  g_ipChecks.fetch_add(1, std::memory_order_relaxed);
  if (std::memcmp(buf.data(), h, bytes) == 0) return;
  const uint64_t bad = g_ipBad.fetch_add(1, std::memory_order_relaxed);
  if (bad >= 12) return;
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t off = 12u * (rdG(table + 4u * i) & 0xFFFu);
    if (std::memcmp(buf.data() + off, h + off, 12) == 0) continue;
    uint32_t g[3], m[3];
    for (int k = 0; k < 3; k++) {
      std::memcpy(&g[k], h + off + 4 * k, 4);
      std::memcpy(&m[k], buf.data() + off + 4 * k, 4);
      g[k] = _byteswap_ulong(g[k]);
      m[k] = _byteswap_ulong(m[k]);
    }
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_VSPATCH] in-place diff #%llu call=%u vs=%08X entry %u/%u (tbl %08X) guest=%08X %08X %08X "
                "port=%08X %08X %08X",
                (unsigned long long)bad, callId, vs, i, n, rdG(table + 4u * i), g[0], g[1], g[2], m[0], m[1], m[2]);
  }
}
// XXH3 of the bytes the XDK would IM_LOAD for the bound VS right now.
uint64_t programHash(uint32_t vs, uint32_t ps) {
  if (vs < 0x10000u) return 0;
  const uint32_t variant = (ps < 0x10000u && (rdG(vs + 872u) & 0x20u)) ? 1u : 0u;
  const uint32_t src = sr::native::vspatch::ucodeDestVA(vs, variant);
  const uint32_t size = sr::native::vspatch::programBytes(vs, variant);
  if (!size || size > 0x40000u) return 0;
  const uint8_t* h = sr::kernel::guestVAToHost(src);
  const uint8_t* h2 = sr::kernel::guestVAToHost(src + size - 1u);
  if (!h || h2 != h + size - 1u) return 0;
  return sr::gfx::ShaderCache::hashUcode(h, size);
}

bool readStrides(uint32_t devVA, uint32_t upStrideBytes, uint8_t strides[16]) {
  const uint8_t* sp = sr::kernel::guestVAToHost(devVA + 12256u);
  if (!sp) return false;
  std::memcpy(strides, sp, 16);
  if (upStrideBytes) strides[0] = uint8_t(upStrideBytes / 4u);  // the XDK's UP stream 0
  return true;
}

uint64_t nativeVsHash(uint32_t devVA, uint32_t vs, uint32_t ps, uint32_t upStrideBytes) {
  if (vs < 0x10000u) return 0;
  const uint32_t variant = (ps < 0x10000u && (rdG(vs + 872u) & 0x20u)) ? 1u : 0u;
  // The program the XDK IM_LOADs: [a2, a2 + *(hdr+4)).
  const uint32_t src = sr::native::vspatch::ucodeDestVA(vs, variant);
  const uint32_t size = sr::native::vspatch::programBytes(vs, variant);
  const uint32_t hdr = vs + rdG(vs + 8u * (variant + 112u)) + 872u;
  if (!size || size > 0x40000u || (size & 3u)) { g_vsNone[1].fetch_add(1, std::memory_order_relaxed); return 0; }
  const uint32_t decl = rdG(devVA + 11536u);
  uint8_t strides[16];
  if (!readStrides(devVA, upStrideBytes, strides) || decl < 0x10000u) {
    g_vsNone[2].fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  uint64_t key = 0xCBF29CE484222325ull;
  auto mix = [&](uint64_t v) { key = (key ^ v) * 0x100000001B3ull; };
  mix(vs); mix(variant); mix(src); mix(size); mix(decl);
  uint64_t s0, s1;
  std::memcpy(&s0, strides, 8);
  std::memcpy(&s1, strides + 8, 8);
  mix(s0); mix(s1);
  static thread_local std::unordered_map<uint64_t, uint64_t> cache;
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  g_vsPatchMisses.fetch_add(1, std::memory_order_relaxed);
  const uint8_t* srcH = sr::kernel::guestVAToHost(src);
  const uint8_t* srcEnd = sr::kernel::guestVAToHost(src + size - 1u);
  if (!srcH || srcEnd != srcH + size - 1u) return 0;
  std::vector<uint8_t> buf(srcH, srcH + size), buf2;
  const int64_t bias = 0;
  buf2 = buf;
  const bool ok = sr::native::vspatch::assembleVertexFetches(vs, decl, strides, variant, buf.data(),
                                                             buf.size(), bias, 0);
  uint64_t h = 0;
  if (ok) {
    h = sr::gfx::ShaderCache::hashUcode(buf.data(), buf.size());
    // The uninitialised-stack quirk: the same inputs with the halfword all ones.
    sr::native::vspatch::assembleVertexFetches(vs, decl, strides, variant, buf2.data(), buf2.size(), bias,
                                               0xFFFF);
    if (std::memcmp(buf.data(), buf2.data(), buf.size()) != 0)
      g_vsGarbageDiff.fetch_add(1, std::memory_order_relaxed);
  } else {
    const uint64_t nf = g_vsPatchFail.fetch_add(1, std::memory_order_relaxed);
    if (nf < 12)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[NATIVE_VSPATCH] fail #%llu reason=%d vs=%08X variant=%u src=%08X size=%u dest=%08X bias=%lld "
                  "entries=%u",
                  (unsigned long long)nf, sr::native::vspatch::lastFailure(), vs, variant, src, size,
                  sr::native::vspatch::ucodeDestVA(vs, variant), (long long)bias, rdG(hdr + 28u));
  }
  if (cache.size() < 65536) cache.emplace(key, h);
  return h;
}

bool deviceSpan(uint32_t devVA, const uint8_t** out) {
  if (!devVA) return false;
  const uint8_t* p = sr::kernel::guestVAToHost(devVA);
  const uint8_t* q = sr::kernel::guestVAToHost(devVA + kDevSpan - 4);
  if (!p || !q || q != p + kDevSpan - 4) return false;
  *out = p;
  return true;
}

// ---- CP-side counters (CP thread only) -------------------------------------
struct Check {
  uint64_t draws = 0, joined = 0, noBracket = 0, otherOwner = 0, missing = 0, evicted = 0, torn = 0,
           postMissing = 0, clean = 0, regDraws = 0, fetchDraws = 0, aluDraws = 0, boolDraws = 0;
  uint64_t byOp[2] = {};
  uint64_t regMis[kRegN] = {}, regDerived[kRegN] = {};
  uint64_t fetchMis[32] = {}, fetchW0Only[32] = {};
  uint64_t aluMis[kAluBlocks] = {};
  uint64_t aluMisRead[kAluBlocks] = {}, aluDerived[kAluBlocks] = {}, aluReadDraws = 0, relDraws = 0;
  uint64_t fetchDerived[32] = {};
  uint64_t zpassDraws = 0, skippedDraws = 0;
  uint64_t boolMis[kBoolN] = {};
  uint64_t vsMis = 0, vsMisPost = 0, psMis = 0, psMisPost = 0, shaderEx = 0;
  uint64_t vsNatMis = 0, vsNatNone = 0, vsNatEx = 0, vsNatImm = 0, vsNatImmMis = 0;
  uint64_t cls[16] = {};  // mode B stage 0: draws by class (bit0 zpass, 1 UP/Begin-End, 2 chunked, 3 immediate VS)
  std::vector<uint64_t> aluWriters;  // [block][owner], owner = kEntryCount for UNSTAMPED
  uint64_t vsPostSeen[32] = {};      // distinct cp VS hashes already logged as post-mismatch
  uint32_t vsPostSeenN = 0;
  std::vector<uint64_t> ownerDraws;  // by kEntries index (draws whose bracket is not a draw entry)
  uint32_t exRegs[kRegN] = {}, exFetch[32] = {}, exAlu[kAluBlocks] = {}, exBool[kBoolN] = {};
  uint32_t exTotal = 0;
} g_ck;

constexpr uint32_t kExPer = 3, kExCap = 600;
bool exampleOk(uint32_t& per) {
  if (per >= kExPer || g_ck.exTotal >= kExCap) return false;
  per++;
  g_ck.exTotal++;
  return true;
}

const char* entryName(int e) {
  return (e >= 0 && e < sr::native::kEntryCount) ? sr::native::kEntries[e].name : "UNSTAMPED";
}

}  // namespace

// ---- NATIVE ALU file carried to the CP side (mode A' driven) ----
// The record keeps block HASHES; the values travel through this ring as deltas: each record
// pushes the 256-byte blocks whose hash changed since the previous record of its thread
// (render thread -> CP thread, stamped with the callId). The CP side applies the deltas in
// callId order to its own copy and checks it against the record's hashes before using it.
struct AluDelta {
  uint32_t callId, block;
  uint8_t data[kAluBlock * 4];
};
constexpr uint32_t kAluRingN = 1u << 16;  // 17 MB
AluDelta* g_aluRing = nullptr;
std::atomic<uint64_t> g_aluHead{0};  // producer (render thread)
uint64_t g_aluTail = 0;              // consumer (CP thread)
std::atomic_flag g_aluPushLock = ATOMIC_FLAG_INIT;
bool aluCarryOn();  // = the drive gate (defined with the drive code)

void aluPush(uint32_t callId, const uint8_t* img, const uint64_t* hashes) {
  static thread_local uint64_t prev[kAluBlocks];
  static thread_local bool init = false;
  if (!init) {
    for (auto& h : prev) h = ~0ull;
    init = true;
  }
  if (!g_aluRing) {
    static AluDelta* ring = new AluDelta[kAluRingN];
    g_aluRing = ring;
  }
  while (g_aluPushLock.test_and_set(std::memory_order_acquire)) {
  }
  uint64_t h = g_aluHead.load(std::memory_order_relaxed);
  for (uint32_t b = 0; b < kAluBlocks; b++) {
    if (hashes[b] == prev[b]) continue;
    prev[b] = hashes[b];
    AluDelta& e = g_aluRing[h & (kAluRingN - 1)];
    e.callId = callId;
    e.block = b;
    std::memcpy(e.data, img + b * kAluBlock * 4, kAluBlock * 4);
    h++;
  }
  g_aluHead.store(h, std::memory_order_release);
  g_aluPushLock.clear(std::memory_order_release);
}

struct PendingConst {
  uint32_t dev, dwordOff, dwords, data;
};
thread_local std::vector<PendingConst> t_pendingConst;
uint64_t g_constPackets = 0;  // render thread

void onConstantPacket(uint32_t devVA, uint32_t a2, uint32_t a3, uint32_t count, uint32_t data) {
  if (!g_recordOn || !data || !count) return;
  // D3DDeviceEmitPm4Packet_sub_82113010: SET_CONSTANT 0xC0002D00 | (4*count) << 16, index
  // (4 * (a3 - (a2 << 8))) & 0x7FC = the ALU dword offset (VS c0 = 0, PS c0 = 1024).
  const uint32_t off = (4u * (a3 - (a2 << 8))) & 0x7FCu;
  t_pendingConst.push_back(PendingConst{devVA, off, 4u * count, data});
  const uint64_t n = ++g_constPackets;
  if (n <= 4 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_REC] constant packet #%llu dev=%08X c%u x%u (a2=%u a3=%u) data=%08X",
                (unsigned long long)n, devVA, off / 4u, count, a2, a3, data);
}

// NATIVE mode B stage 1a (SR_NATIVE_DRAW, defined at the end of this file).
namespace {
void ndDisarm();
void ndArm(uint32_t devVA, const uint8_t* d, uint8_t kind);
void ndCommit(uint32_t devVA, const Rec& r);
}  // namespace

void onDrawBefore(uint32_t devVA, DrawKind kind, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7) {
  ndDisarm();  // a nested draw entry inside an armed bracket declines the outer one
  const uint32_t callId = sr::native::currentCallId();
  if (!callId) {
    g_noBracket.fetch_add(1, std::memory_order_relaxed);
    t_lastRec = nullptr;
    return;
  }
  if (callId == t_lastCallId) {  // a nested draw entry inside the same bracket
    g_nested.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  t_lastCallId = callId;
  t_lastRec = nullptr;
  if (g_every > 1 && (callId % g_every) != 0) {
    g_sampledOut.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint8_t* d = nullptr;
  if (!deviceSpan(devVA, &d)) {
    g_badDev.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Rec* base = recs();
  // 4-way set: replace the oldest (smallest callId; 0 = empty) of the 4 slots.
  Rec* set = &base[callId & g_mask & ~3u];
  int victim = 0;
  for (int k = 1; k < 4; k++)
    if (set[k].seq.load(std::memory_order_relaxed) < set[victim].seq.load(std::memory_order_relaxed)) victim = k;
  Rec& r = set[victim];
  r.seq.store(0, std::memory_order_relaxed);
  r.postSeq.store(0, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  r.dev = devVA;
  r.kind = uint8_t(kind);
  r.args[0] = r4;
  r.args[1] = r5;
  r.args[2] = r6;
  r.args[3] = r7;
  for (int i = 0; i < kRegN; i++) r.pre[i] = be32(d + g_devOf[i]);
  for (uint32_t i = 0; i < kFetchN; i++) r.fetch[i] = be32(d + kFetchOff + 4 * i);
  for (uint32_t i = 0; i < kBoolN; i++) r.boolc[i] = be32(d + kBoolOff + 4 * i);
  {
    // Native ALU image, a timeline like the CP's constant file: the constant packets the
    // game emitted since the last draw (B2, onConstantPacket), dirty shadow groups, then the
    // literals of a rebound shader.
    alignas(16) static thread_local uint8_t aluImg[kAluN * 4];
    static thread_local bool aluInit = false;
    auto be64 = [&](uint32_t off) {
      uint64_t v;
      std::memcpy(&v, d + off, 8);
      return _byteswap_uint64(v);
    };
    const uint64_t vsDirty = be64(0x0), psDirty = be64(0x8), dirty10 = be64(0x10);
    if (!aluInit) {
      std::memcpy(aluImg, d + kAluOff, sizeof(aluImg));
      aluInit = true;
    }
    for (const PendingConst& p : t_pendingConst) {  // ring words are guest (BE) like aluImg
      if (p.dev != devVA) continue;
      for (uint32_t k = 0; k < p.dwords; k++) {
        const uint32_t w = sr::kernel::readGuestU32BE(p.data + 4u * k);
        const uint32_t be = _byteswap_ulong(w);
        std::memcpy(aluImg + 4u * ((p.dwordOff + k) & (kAluN - 1u)), &be, 4);
      }
    }
    t_pendingConst.clear();
    for (uint64_t m = vsDirty; m;) {  // bit 63 = c0-c3 (cntlzd order of 821078C0)
      const uint32_t g = uint32_t(__builtin_clzll(m));
      std::memcpy(aluImg + 64u * g, d + kAluOff + 64u * g, 64);
      m &= ~(1ull << (63 - g));
    }
    for (uint64_t m = psDirty; m;) {
      const uint32_t g = uint32_t(__builtin_clzll(m));
      std::memcpy(aluImg + 4096u + 64u * g, d + kAluOff + 4096u + 64u * g, 64);
      m &= ~(1ull << (63 - g));
    }
    const uint32_t vsO = rdG(devVA + 12420u), psO = rdG(devVA + 12416u);
    if (((dirty10 >> 51) & 1u) && vsO >= 0x10000u) overlayLiterals(aluImg, vsO + 872u, rdG(vsO + 32u));
    if (((dirty10 >> 52) & 1u) && psO >= 0x10000u) overlayLiterals(aluImg, psO + 40u, rdG(psO + 24u));
    for (uint32_t b = 0; b < kAluBlocks; b++) r.alu[b] = hashBlock(aluImg + b * kAluBlock * 4);
    if (aluCarryOn()) aluPush(callId, aluImg, r.alu);
  }
  r.upHash = 0;
  r.upBytes = 0;
  if (kind == kDrawUP && r5 && r7 && uint64_t(r5) * r7 <= (4u << 20)) {
    // The caller's own vertex array, fully populated at entry (the XDK memcpys it into the ring).
    if (const uint8_t* src = sr::kernel::guestVAToHost(r6)) {
      r.upBytes = r5 * r7;
      r.upHash = sr::gfx::ShaderCache::hashUcode(src, r.upBytes);
    }
  }
  r.vsObj = sr::kernel::readGuestU32BE(devVA + kDevVs);
  r.vsUcodeVA = r.vsObj >= 0x10000u ? sr::kernel::readGuestU32BE(r.vsObj + 0x20u) : 0u;
  r.vsPre = s4HashShaderObjUncached(r.vsObj, true, nullptr);
  // DrawVerticesUP / BeginVertices put their stride/4 in stream 0 for the call; an
  // EndVertices draw was patched at its BeginVertices, so it inherits that stride.
  static thread_local uint32_t t_beginStride = 0;
  if (kind == kBeginVertices) t_beginStride = r6;
  r.upStride = kind == kDrawUP ? r7 : kind == kBeginVertices ? r6 : kind == kEndVertices ? t_beginStride : 0u;
  r.vsNative = nativeVsHash(devVA, r.vsObj, rdG(devVA + 12416u), r.upStride);
  r.progPre = programHash(r.vsObj, rdG(devVA + 12416u));
  r.psPre = s4HashShaderObjUncached(sr::kernel::readGuestU32BE(devVA + kDevPs), false, nullptr);
  // Native projection: the flush-derived registers a native renderer computes itself.
  r.zpass = sr::native::zPassActive() ? 1 : 0;
  r.checks.store(0, std::memory_order_relaxed);
  {
    const ShaderRegs sr_ = computeShaderRegs(devVA);
    if (sr_.ok) {
      r.pre[kIdx2180] = sr_.progCntl;
      r.pre[kIdx2180 + 1] = sr_.contextMisc;
      if (sr_.haveInterp) r.pre[kIdx2180 + 2] = sr_.interp;
    }
    uint64_t dirty10;
    std::memcpy(&dirty10, d + 0x10, 8);
    dirty10 = _byteswap_uint64(dirty10);
    // Z-pass: BeginZPass_8210DF38 sets dev.byte[10808] |= 0x40, so inside the Z-pass
    // GpuEmitDrawState decides depth-only (5) vs colour (4) PER DRAW (modeControl). c4
    // applied that rule to every draw on top of a blanket "5 on the first execution";
    // the rule belongs to the first (Z-pass) execution only, the replay runs the shadow.
    r.modeZ = modeControl(devVA, r.pre[kIdx2208], dirty10);
    r.pre[kIdx2203] = hizControl(devVA, r.pre[kIdx2203], r.pre[kIdx2203 - 3]);
  }
  r.seq.store(callId, std::memory_order_release);
  ndArm(devVA, d, uint8_t(kind));
  t_lastRec = &r;
  g_recorded.fetch_add(1, std::memory_order_relaxed);
  if (kind < kDrawKinds) g_kindCount[kind].fetch_add(1, std::memory_order_relaxed);
}

void onDrawAfter(uint32_t devVA) {
  Rec* r = t_lastRec;
  if (!r) return;
  t_lastRec = nullptr;
  const uint32_t callId = t_lastCallId;
  if (r->seq.load(std::memory_order_relaxed) != callId) return;
  const uint8_t* d = nullptr;
  if (!deviceSpan(devVA, &d)) return;
  for (int i = 0; i < kRegN; i++) r->post[i] = be32(d + g_devOf[i]);
  for (uint32_t i = 0; i < kFetchN; i++) r->fetchPost[i] = be32(d + kFetchOff + 4 * i);
  for (uint32_t b = 0; b < kAluBlocks; b++) r->aluPost[b] = hashBlock(d + kAluOff + b * kAluBlock * 4);
  r->vsPost = s4HashShaderObjUncached(sr::kernel::readGuestU32BE(devVA + kDevVs), true, nullptr);
  if (programHash(r->vsObj, rdG(devVA + 12416u)) != r->progPre)
    inPlaceDifferential(devVA, r->vsObj, rdG(devVA + 12416u), callId, r->upStride);
  r->psPost = s4HashShaderObjUncached(sr::kernel::readGuestU32BE(devVA + kDevPs), false, nullptr);
  r->postSeq.store(callId, std::memory_order_release);
  ndCommit(devVA, *r);
}

void noteDrawSkipped(uint32_t callId) {
  Rec* base = g_recs.load(std::memory_order_acquire);
  if (!base || !callId) return;
  Rec* set = &base[callId & g_mask & ~3u];
  for (int k = 0; k < 4; k++)
    if (set[k].seq.load(std::memory_order_acquire) == callId) {
      set[k].checks.fetch_add(1, std::memory_order_relaxed);
      g_ck.skippedDraws++;
      return;
    }
}

// ---- mode A' DRIVEN (CP thread only) ----
bool driveOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NATIVE_DRIVE");
    return e && e[0] == '1';
  }();
  return v;
}
bool aluCarryOn() {
  static const bool nd = [] {
    const char* e = std::getenv("SR_NATIVE_DRAW");
    return e && e[0] == '1';
  }();
  return driveOn() || nd;
}
struct Drive {
  bool active = false, fetch = false, alu = false;
  uint32_t reg[kRegN], fetchV[kFetchN], boolV[kBoolN], aluV[kAluN];
  uint64_t draws = 0, regWrites = 0, fetchWrites = 0, boolWrites = 0, upKept = 0;
  uint64_t aluDraws = 0, aluWrites = 0, aluStale = 0, aluLost = 0;
  uint64_t upMatch = 0, upBad = 0, upNoData = 0;
  alignas(16) uint8_t aluImg[kAluN * 4];  // the carried native ALU file (guest byte order)
  bool aluImgValid = false;
} g_drive;

// Apply the carried deltas up to callId; true when the image matches the record's hashes.
// Per-block version history on the CP side: the state AT callId X is, per block, the newest
// version with callId <= X. Executions out of call order need it - the Z-pass replays its draws
// in colour at EndZPass, after the deltas of every later draw of the pass were applied.
constexpr uint32_t kAluHist = 256;  // versions per block (32 x 256 x 256 B = 2 MB)
struct AluHist {
  uint32_t n = 0;  // versions pushed (ring of kAluHist)
  uint32_t callId[kAluHist];
  uint8_t data[kAluHist][kAluBlock * 4];
};
AluHist* g_aluHist = nullptr;

bool aluDrain(uint32_t callId, const uint64_t* hashes) {
  Drive& d = g_drive;
  if (!g_aluRing) return false;
  if (!g_aluHist) g_aluHist = new AluHist[kAluBlocks];
  const uint64_t head = g_aluHead.load(std::memory_order_acquire);
  if (head - g_aluTail > kAluRingN) {  // the producer lapped us: deltas lost
    d.aluLost++;
    g_aluTail = head - kAluRingN / 2;
  }
  while (g_aluTail < head) {
    const AluDelta& e = g_aluRing[g_aluTail & (kAluRingN - 1)];
    if (e.callId > callId) break;
    AluHist& h = g_aluHist[e.block];
    const uint32_t k = h.n++ % kAluHist;
    h.callId[k] = e.callId;
    std::memcpy(h.data[k], e.data, kAluBlock * 4);
    g_aluTail++;
  }
  for (uint32_t b = 0; b < kAluBlocks; b++) {
    const AluHist& h = g_aluHist[b];
    const uint32_t have = std::min(h.n, kAluHist);
    int found = -1;
    for (uint32_t i = 0; i < have; i++) {  // newest first
      const uint32_t k = (h.n - 1 - i) % kAluHist;
      if (h.callId[k] <= callId) {
        found = int(k);
        break;
      }
    }
    if (found < 0) return false;
    std::memcpy(d.aluImg + b * kAluBlock * 4, h.data[found], kAluBlock * 4);
    if (hashBlock(d.aluImg + b * kAluBlock * 4) != hashes[b]) return false;
  }
  return true;
}

bool skipOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NATIVE_SKIP");
    return e && e[0] == '1';
  }();
  return v && driveOn();
}
struct Skip {
  uint32_t lastId = 0;
  bool lastSkip = false;
  uint64_t brackets = 0, skipped = 0, noRec = 0, upFamily = 0, aluBad = 0, verify = 0;
  uint64_t pkSkipped = 0, dwSkipped = 0, pkKept = 0, drawsSkipped = 0;
} g_skip;

void afterDraw() {
  Drive& d = g_drive;
  if (!d.active) return;
  sr::gfx::XenosRegisterFile& x = sr::gfx::getXenosRegisters();
  for (int i = 0; i < kRegN; i++) x.write(g_regOf[i], d.reg[i]);
  if (d.fetch)
    for (uint32_t k = 0; k < kFetchN; k++) x.write(kFetchReg + k, d.fetchV[k]);
  for (uint32_t k = 0; k < kBoolN; k++) x.write(kBoolReg + k, d.boolV[k]);
  if (d.alu)
    for (uint32_t k = 0; k < kAluN; k++) x.write(kAluReg + k, d.aluV[k]);
  d.active = false;
}

// Substitute the projection for this draw (values that already equal the CP's cost nothing).
void driveDraw(const uint32_t* pre, const uint32_t* fetch, const uint32_t* boolc, uint8_t kind, bool aluOk,
               bool keep = false) {
  Drive& d = g_drive;
  afterDraw();  // a draw whose afterDraw never ran (should not happen): restore first
  sr::gfx::XenosRegisterFile& x = sr::gfx::getXenosRegisters();
  d.fetch = kind != kDrawUP && kind != kBeginVertices && kind != kEndVertices;
  if (!d.fetch) d.upKept++;
  for (int i = 0; i < kRegN; i++) {
    d.reg[i] = x.read(g_regOf[i]);
    if (d.reg[i] != pre[i]) {
      x.write(g_regOf[i], pre[i]);
      d.regWrites++;
    }
  }
  if (d.fetch)
    for (uint32_t k = 0; k < kFetchN; k++) {
      d.fetchV[k] = x.read(kFetchReg + k);
      if (d.fetchV[k] != fetch[k]) {
        x.write(kFetchReg + k, fetch[k]);
        d.fetchWrites++;
      }
    }
  for (uint32_t k = 0; k < kBoolN; k++) {
    d.boolV[k] = x.read(kBoolReg + k);
    if (d.boolV[k] != boolc[k]) {
      x.write(kBoolReg + k, boolc[k]);
      d.boolWrites++;
    }
  }
  d.alu = aluOk;
  if (aluOk) {
    d.aluDraws++;
    for (uint32_t k = 0; k < kAluN; k++) {
      uint32_t be;
      std::memcpy(&be, d.aluImg + 4u * k, 4);
      const uint32_t v = _byteswap_ulong(be);
      d.aluV[k] = x.read(kAluReg + k);
      if (d.aluV[k] != v) {
        x.write(kAluReg + k, v);
        d.aluWrites++;
      }
    }
  } else {
    d.aluStale++;
  }
  d.active = !keep;  // a skipped bracket: the projection IS the state (no restore)
  const uint64_t n = ++d.draws;
  if ((n & (n - 1)) == 0 && n >= 1024)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_DRIVE] %llu draws built from the projection: %llu register, %llu fetch, %llu bool, "
                "%llu ALU values differed from the CP's; ALU carried on %llu draws (stale %llu, ring lost %llu); "
                "%llu UP-family draws kept the CP fetch slots; UP vertex data native == CP copy %llu, differ %llu, "
                "unchecked %llu",
                (unsigned long long)n, (unsigned long long)d.regWrites, (unsigned long long)d.fetchWrites,
                (unsigned long long)d.boolWrites, (unsigned long long)d.aluWrites,
                (unsigned long long)d.aluDraws, (unsigned long long)d.aluStale, (unsigned long long)d.aluLost,
                (unsigned long long)d.upKept, (unsigned long long)d.upMatch, (unsigned long long)d.upBad,
                (unsigned long long)d.upNoData);
  if ((n & (n - 1)) == 0 && n >= 1024 && skipOn())
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_SKIP] brackets %llu: state skipped %llu (draws %llu), parsed: verify %llu, UP-family %llu, "
                "no record %llu, ALU not carried %llu | packets skipped %llu (%llu dwords), kept %llu (uncovered regs)",
                (unsigned long long)g_skip.brackets, (unsigned long long)g_skip.skipped,
                (unsigned long long)g_skip.drawsSkipped, (unsigned long long)g_skip.verify,
                (unsigned long long)g_skip.upFamily, (unsigned long long)g_skip.noRec,
                (unsigned long long)g_skip.aluBad, (unsigned long long)g_skip.pkSkipped,
                (unsigned long long)g_skip.dwSkipped, (unsigned long long)g_skip.pkKept);
}

// Registers the projection carries: the 124 packed ones, ALU, fetch, bool/loop. coveredRun(r, n)
// = every register of [r, r+n) is carried (O(1): the end of the carried run starting at each reg).
bool coveredRun(uint32_t reg, uint32_t count) {
  static uint16_t runEnd[0x4A00];  // exclusive end of the carried run containing reg (0 = not carried)
  static bool init = false;
  if (!init) {
    static bool c[0x4A00];
    std::memset(c, 0, sizeof(c));
    for (int i = 0; i < kRegN; i++) c[g_regOf[i]] = true;
    for (uint32_t r = kAluReg; r < kAluReg + kAluN; r++) c[r] = true;
    for (uint32_t r = kFetchReg; r < kFetchReg + kFetchN; r++) c[r] = true;
    for (uint32_t r = kBoolReg; r < kBoolReg + kBoolN; r++) c[r] = true;
    uint16_t end = 0;
    for (int r = 0x4A00 - 1; r >= 0; r--) {
      if (!c[r]) {
        end = 0;
      } else if (!end) {
        end = uint16_t(r + 1);
      }
      runEnd[r] = c[r] ? end : 0;
    }
    init = true;
  }
  if (!count || reg >= 0x4A00u) return false;
  return runEnd[reg] && uint32_t(runEnd[reg]) >= reg + count;
}
Rec* findRec(uint32_t callId) {
  Rec* base = g_recs.load(std::memory_order_acquire);
  if (!base || !callId) return nullptr;
  Rec* set = &base[callId & g_mask & ~3u];
  for (int k = 0; k < 4; k++)
    if (set[k].seq.load(std::memory_order_acquire) == callId) return &set[k];
  return nullptr;
}
bool aluDrain(uint32_t callId, const uint64_t* hashes);
bool skipState(uint32_t callId, bool drawEntry, uint32_t reg, uint32_t count) {
  if (!skipOn() || !callId || !drawEntry) return false;
  Skip& k = g_skip;
  if (callId != k.lastId) {
    k.lastId = callId;
    k.lastSkip = false;
    k.brackets++;
    Rec* r = findRec(callId);
    if (!r) {
      k.noRec++;
    } else if (r->kind == kDrawUP || r->kind == kBeginVertices || r->kind == kEndVertices) {
      k.upFamily++;
    } else if ((callId & 15u) == 0) {
      k.verify++;
    } else {
      uint64_t h[kAluBlocks];
      std::memcpy(h, r->alu, sizeof(h));
      if (r->seq.load(std::memory_order_acquire) == callId && aluDrain(callId, h)) {
        k.lastSkip = true;
        k.skipped++;
      } else {
        k.aluBad++;
      }
    }
  }
  if (!k.lastSkip) return false;
  if (!coveredRun(reg, count)) {
    k.pkKept++;
    return false;
  }
  k.pkSkipped++;
  k.dwSkipped += count;
  return true;
}

void checkDraw(uint32_t callId, int entry, uint32_t op, const CpShaders& sh) {
  const uint64_t cpVs = sh.vs, cpPs = sh.ps;
  Check& c = g_ck;
  if (c.ownerDraws.empty()) c.ownerDraws.assign(size_t(sr::native::kEntryCount) + 1, 0);
  c.draws++;
  c.byOp[op == 0x36u ? 1 : 0]++;
  if (!callId) {
    c.noBracket++;
    return;
  }
  Rec* base = g_recs.load(std::memory_order_acquire);
  if (!base) {  // nothing recorded yet: a draw owned by a non-draw entry (or none)
    c.otherOwner++;
    c.ownerDraws[entry >= 0 && entry < sr::native::kEntryCount ? entry : sr::native::kEntryCount]++;
    return;
  }
  Rec* set = &base[callId & g_mask & ~3u];
  int way = 0;
  uint32_t newest = 0;
  for (int k = 0; k < 4; k++) {
    const uint32_t q = set[k].seq.load(std::memory_order_acquire);
    if (q == callId) way = k;
    if (q > newest) newest = q;
  }
  Rec& r = set[way];
  const uint32_t s1 = r.seq.load(std::memory_order_acquire);
  if (s1 != callId) {
    // Draws inside a bracket of a non-draw entry (Clear, Resolve, Swap...) have no
    // record by design; a draw entry's bracket without one was evicted or sampled out.
    const char* n = entryName(entry);
    const bool drawEntry = std::strstr(n, "Draw") || std::strstr(n, "Vertices");
    if (!drawEntry) {
      c.otherOwner++;
      c.ownerDraws[entry >= 0 && entry < sr::native::kEntryCount ? entry : sr::native::kEntryCount]++;
    } else if (newest > callId) {
      c.evicted++;
    } else {
      c.missing++;
    }
    return;
  }
  uint32_t pre[kRegN], post[kRegN], fetch[kFetchN], fetchPost[kFetchN], boolc[kBoolN], args[4];
  uint64_t alu[kAluBlocks], aluPost[kAluBlocks];
  std::memcpy(pre, r.pre, sizeof(pre));
  std::memcpy(fetch, r.fetch, sizeof(fetch));
  std::memcpy(boolc, r.boolc, sizeof(boolc));
  std::memcpy(alu, r.alu, sizeof(alu));
  std::memcpy(args, r.args, sizeof(args));
  const uint8_t kind = r.kind;
  const uint32_t execIdx = r.checks.fetch_add(1, std::memory_order_relaxed);
  if (r.zpass) {
    c.zpassDraws++;
    // The Z-pass executes the draw depth-only first (RB_MODECONTROL kDepth = 5); the
    // XDK replays the same packets at EndZPass in colour (the shadow's value).
    if (execIdx == 0) pre[kIdx2208] = r.modeZ;
  }
  // DrawVertices (D3DDeviceEmitDrawVertices_sub_820FBBF8) splits a draw of more than 0xFFFF
  // vertices into DRAW packets of ((0xFFFF / vpp) & ~1) * vpp vertices, each preceded by its own
  // VGT_INDX_OFFSET = start + k * (chunk - overlap) (vpp / overlap: the XDK's per-primitive table
  // at 0x820B3668). Execution k of this record (mod the chunk count: the Z-pass replays them all)
  // is chunk k. A native renderer issues ONE draw; this models the XDK's packets for the check.
  if (kind == kDrawAuto && args[2] > 0xFFFFu && args[0] < 32u) {
    const uint32_t vpp = sr::kernel::readGuestU32BE(0x820B3668u + 8u * args[0]);
    const uint32_t overlap = sr::kernel::readGuestU32BE(0x820B366Cu + 8u * args[0]);
    if (vpp && vpp <= 0xFFFFu) {
      const uint32_t chunk = ((0xFFFFu / vpp) & ~1u) * vpp;
      if (chunk > overlap) {
        uint32_t n = 1;
        for (uint32_t left = args[2]; left > 0xFFFFu && n < 4096u; n++) left = overlap + left - chunk;
        pre[kIdx2102] = args[1] + (execIdx % n) * (chunk - overlap);
      }
    }
  }
  if (skipOn() && g_skip.lastId == callId && g_skip.lastSkip) {
    // The bracket's state packets were not executed: nothing to compare; the projection is the state.
    g_skip.drawsSkipped++;
    const bool aluOk = aluDrain(callId, alu);
    driveDraw(pre, fetch, boolc, kind, aluOk, true);
    return;
  }
  const uint64_t vsPre = r.vsPre, psPre = r.psPre, vsPost = r.vsPost, psPost = r.psPost;
  const uint64_t vsNative = r.vsNative;
  const uint32_t vsObj = r.vsObj;
  const bool postOk = r.postSeq.load(std::memory_order_acquire) == callId;
  if (postOk) {
    std::memcpy(post, r.post, sizeof(post));
    std::memcpy(fetchPost, r.fetchPost, sizeof(fetchPost));
    std::memcpy(aluPost, r.aluPost, sizeof(aluPost));
  }
  std::atomic_thread_fence(std::memory_order_acquire);
  if (r.seq.load(std::memory_order_relaxed) != s1) {
    c.torn++;
    return;
  }
  c.joined++;
  if (!postOk) c.postMissing++;
  const sr::gfx::XenosRegisterFile& x = sr::gfx::getXenosRegisters();

  bool regBad = false, fetchBad = false, aluBad = false, boolBad = false;
  for (int i = 0; i < kRegN; i++) {
    const uint32_t cp = x.read(g_regOf[i]);
    if (cp == pre[i]) continue;
    regBad = true;
    c.regMis[i]++;
    const bool derived = postOk && post[i] == cp;
    if (derived) c.regDerived[i]++;
    if (exampleOk(c.exRegs[i]))
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[NATIVE_REC_MISMATCH] reg=%04X dev+%04X call=%u kind=%u args=%08X,%08X,%08X,%08X "
                  "native=%08X post=%08X%s cp=%08X class=%s zp=%u ex=%u",
                  g_regOf[i], g_devOf[i], callId, kind, args[0], args[1], args[2], args[3], pre[i],
                  postOk ? post[i] : 0u, postOk ? "" : "(n/a)", cp, derived ? "flush-derived" : "not-in-shadow",
                  unsigned(r.zpass), execIdx);
  }
  for (uint32_t s = 0; s < 32; s++) {
    bool bad = false, w0only = true;
    for (uint32_t k = 0; k < 6; k++) {
      const uint32_t cp = x.read(kFetchReg + 6 * s + k);
      if (cp != fetch[6 * s + k]) {
        bad = true;
        if (k) w0only = false;
      }
    }
    if (!bad) continue;
    fetchBad = true;
    c.fetchMis[s]++;
    if (postOk) {
      bool der = true;
      for (uint32_t k = 0; k < 6; k++) der &= fetchPost[6 * s + k] == x.read(kFetchReg + 6 * s + k);
      if (der) c.fetchDerived[s]++;
    }
    if (w0only) c.fetchW0Only[s]++;
    if (exampleOk(c.exFetch[s]))
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[NATIVE_REC_MISMATCH] fetch slot=%u call=%u kind=%u native=%08X %08X %08X %08X %08X %08X "
                  "cp=%08X %08X %08X %08X %08X %08X",
                  s, callId, kind, fetch[6 * s], fetch[6 * s + 1], fetch[6 * s + 2], fetch[6 * s + 3],
                  fetch[6 * s + 4], fetch[6 * s + 5], x.read(kFetchReg + 6 * s), x.read(kFetchReg + 6 * s + 1),
                  x.read(kFetchReg + 6 * s + 2), x.read(kFetchReg + 6 * s + 3), x.read(kFetchReg + 6 * s + 4),
                  x.read(kFetchReg + 6 * s + 5));
  }
  const uint32_t* xr = x.data();
  // Blocks the active shaders may read (VS blocks 0-15, PS 16-31); relative = all of that stage.
  const uint32_t vsUse = (sh.vsUse & 0x10000u) ? 0xFFFFu : (sh.vsUse & 0xFFFFu);
  const uint32_t psUse = (sh.psUse & 0x10000u) ? 0xFFFFu : (sh.psUse & 0xFFFFu);
  const uint32_t readMask = vsUse | (psUse << 16);
  if ((sh.vsUse | sh.psUse) & 0x10000u) c.relDraws++;
  bool aluReadBad = false;
  for (uint32_t b = 0; b < kAluBlocks; b++) {
    uint32_t tmp[kAluBlock];
    for (uint32_t k = 0; k < kAluBlock; k++) tmp[k] = _byteswap_ulong(xr[kAluReg + b * kAluBlock + k]);
    const uint64_t cpHash = hashBlock(reinterpret_cast<const uint8_t*>(tmp));
    if (cpHash == alu[b]) continue;
    c.aluMis[b]++;
    if (postOk && aluPost[b] == cpHash) c.aluDerived[b]++;
    if (!((readMask >> b) & 1u)) continue;  // no active shader reads this block
    aluBad = true;
    aluReadBad = true;
    c.aluMisRead[b]++;
    if (exampleOk(c.exAlu[b]))
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[NATIVE_REC_MISMATCH] alu block=%u (%s c%u-c%u) call=%u kind=%u args=%08X,%08X,%08X,%08X",
                  b, b < 16 ? "VS" : "PS", (b % 16) * 16, (b % 16) * 16 + 15, callId, kind, args[0], args[1],
                  args[2], args[3]);
  }
  for (uint32_t i = 0; i < kBoolN; i++) {
    const uint32_t cp = x.read(kBoolReg + i);
    if (cp == boolc[i]) continue;
    boolBad = true;
    c.boolMis[i]++;
    if (exampleOk(c.exBool[i]))
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[NATIVE_REC_MISMATCH] %s reg=%04X call=%u kind=%u native=%08X cp=%08X",
                  i < 8 ? "bool" : "loop", kBoolReg + i, callId, kind, boolc[i], cp);
  }
  // The native VS (host-patched program) is the identity a native renderer binds.
  const bool vsBad = vsNative ? (vsNative != cpVs) : (vsPre != cpVs);
  const bool psBad = psPre != cpPs;
  if (!vsNative) c.vsNatNone++;
  else if (vsNative != cpVs) c.vsNatMis++;
  if (!sh.vsAddr) {
    c.vsNatImm++;
    if (vsNative && vsNative != cpVs) c.vsNatImmMis++;
  }
  if (execIdx == 0)
    c.cls[(r.zpass ? 1u : 0u) | (kind >= kDrawUP ? 2u : 0u) |
          ((kind == kDrawAuto && args[2] > 0xFFFFu) ? 4u : 0u) | (!sh.vsAddr ? 8u : 0u)]++;
  if (vsNative && vsNative != cpVs && c.vsNatEx < 16 && c.exTotal < kExCap) {
    c.vsNatEx++;
    c.exTotal++;
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_REC_MISMATCH] vsnative call=%u kind=%u vsObj=%08X native=%016llX pre=%016llX cp=%016llX "
                "cpLoad=%08X/%u",
                callId, kind, vsObj, (unsigned long long)vsNative, (unsigned long long)vsPre,
                (unsigned long long)cpVs, sh.vsAddr, sh.vsDwords);
  }
  c.vsMis += vsBad;
  c.psMis += psBad;
  if (postOk) {
    c.vsMisPost += vsPost != cpVs;
    c.psMisPost += psPost != cpPs;
  }
  if (postOk && vsPost != cpVs && c.vsPostSeenN < 32 && c.exTotal < kExCap) {
    bool seen = false;
    for (uint32_t k = 0; k < c.vsPostSeenN; k++) seen |= c.vsPostSeen[k] == cpVs;
    if (!seen) {
      c.vsPostSeen[c.vsPostSeenN++] = cpVs;
      c.exTotal++;
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[NATIVE_REC_MISMATCH] vspost call=%u kind=%u vsObj=%08X objUcode=%08X native=%016llX "
                  "post=%016llX cp=%016llX cpLoad=%08X/%u%s",
                  callId, kind, vsObj, r.vsUcodeVA, (unsigned long long)vsPre, (unsigned long long)vsPost,
                  (unsigned long long)cpVs, sh.vsAddr, sh.vsDwords, sh.vsAddr ? "" : " (immediate)");
    }
  }
  if ((vsBad || psBad) && c.shaderEx < 24 && c.exTotal < kExCap) {
    c.shaderEx++;
    c.exTotal++;
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_REC_MISMATCH] shader call=%u kind=%u vs native=%016llX post=%016llX cp=%016llX | "
                "ps native=%016llX post=%016llX cp=%016llX",
                callId, kind, (unsigned long long)vsPre, (unsigned long long)(postOk ? vsPost : 0),
                (unsigned long long)cpVs, (unsigned long long)psPre, (unsigned long long)(postOk ? psPost : 0),
                (unsigned long long)cpPs);
  }
  c.regDraws += regBad;
  c.fetchDraws += fetchBad;
  c.aluDraws += aluBad;
  c.aluReadDraws += aluReadBad;
  c.boolDraws += boolBad;
  if (!regBad && !fetchBad && !aluBad && !boolBad && !vsBad && !psBad) c.clean++;
  if (driveOn()) {
    uint64_t aluHashes[kAluBlocks];
    std::memcpy(aluHashes, alu, sizeof(aluHashes));
    const bool aluOk = aluDrain(callId, aluHashes);
    if (kind == kDrawUP) {
      // Native UP upload equivalence: the caller's bytes at entry == the bytes the CP fetches
      // through slot 31 (vertex fetch constant 95: address w4 & ~3, size w5 >> 2 dwords).
      const uint64_t upHash = r.upHash;
      const uint32_t upBytes = r.upBytes;
      const uint32_t w4 = x.read(kFetchReg + 6 * 31 + 4), w5 = x.read(kFetchReg + 6 * 31 + 5);
      const uint32_t addr = w4 & 0x1FFFFFFCu, size = ((w5 >> 2) & 0xFFFFFFu) * 4u;
      const uint8_t* cpData = (upBytes && size >= upBytes) ? sr::kernel::guestVAToHost(0xA0000000u | addr) : nullptr;
      if (!cpData) {
        g_drive.upNoData++;
      } else if (sr::gfx::ShaderCache::hashUcode(cpData, upBytes) == upHash) {
        g_drive.upMatch++;
      } else {
        g_drive.upBad++;
      }
    }
    driveDraw(pre, fetch, boolc, kind, aluOk);
  }
}

void noteAluWrite(int entry, uint32_t reg, uint32_t count) {
  Check& c = g_ck;
  const size_t owners = size_t(sr::native::kEntryCount) + 1;
  if (c.aluWriters.empty()) c.aluWriters.assign(kAluBlocks * owners, 0);
  if (reg >= kAluReg + kAluN || reg + count <= kAluReg || !count) return;
  const uint32_t lo = std::max(reg, kAluReg) - kAluReg;
  const uint32_t hi = std::min(reg + count, kAluReg + kAluN) - kAluReg;
  const size_t o = (entry >= 0 && entry < sr::native::kEntryCount) ? size_t(entry) : owners - 1;
  for (uint32_t b = lo / kAluBlock; b <= (hi - 1) / kAluBlock; b++) c.aluWriters[b * owners + o]++;
}

void ndReportHook(const char* why);
void report(const char* why) {
  if (!g_recordOn) return;
  ndReportHook(why);
  const Check& c = g_ck;
  // Top mismatching registers (by count), with the flush-derived share.
  std::vector<int> idx;
  for (int i = 0; i < kRegN; i++)
    if (c.regMis[i]) idx.push_back(i);
  std::sort(idx.begin(), idx.end(), [&](int a, int b) { return c.regMis[a] > c.regMis[b]; });
  std::string regs;
  char b[96];
  for (size_t k = 0; k < idx.size() && k < 40; k++) {
    std::snprintf(b, sizeof(b), "%s%04X:%llu/%llu", regs.empty() ? "" : ",", g_regOf[idx[k]],
                  (unsigned long long)c.regMis[idx[k]], (unsigned long long)c.regDerived[idx[k]]);
    regs += b;
  }
  std::string fetch, alu, boolc;
  for (int s = 0; s < 32; s++)
    if (c.fetchMis[s]) {
      std::snprintf(b, sizeof(b), "%s%d:%llu/w0=%llu/derived=%llu", fetch.empty() ? "" : ",", s,
                    (unsigned long long)c.fetchMis[s], (unsigned long long)c.fetchW0Only[s],
                    (unsigned long long)c.fetchDerived[s]);
      fetch += b;
    }
  for (uint32_t k = 0; k < kAluBlocks; k++)
    if (c.aluMis[k]) {
      std::snprintf(b, sizeof(b), "%s%s%u:%llu/read=%llu/derived=%llu", alu.empty() ? "" : ",", k < 16 ? "vs" : "ps",
                    (k % 16) * 16, (unsigned long long)c.aluMis[k], (unsigned long long)c.aluMisRead[k],
                    (unsigned long long)c.aluDerived[k]);
      alu += b;
    }
  for (uint32_t k = 0; k < kBoolN; k++)
    if (c.boolMis[k]) {
      std::snprintf(b, sizeof(b), "%s%04X:%llu", boolc.empty() ? "" : ",", kBoolReg + k,
                    (unsigned long long)c.boolMis[k]);
      boolc += b;
    }
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_REC] %s recorded=%llu (idx=%llu auto=%llu up=%llu begin=%llu end=%llu) nested=%llu noBracketRec=%llu "
              "badDev=%llu sampledOut=%llu | cpDraws=%llu (0x22=%llu 0x36=%llu) joined=%llu clean=%llu "
              "regDraws=%llu fetchDraws=%llu aluReadDraws=%llu relDraws=%llu boolDraws=%llu otherOwner=%llu noBracket=%llu "
              "missing=%llu evicted=%llu torn=%llu postMissing=%llu zpassDraws=%llu predSkipped=%llu vsNative(mis=%llu none=%llu [noTbl=%llu size=%llu noDecl=%llu] imm=%llu immMis=%llu patchFail=%llu garbageDiff=%llu patches=%llu inPlace=%llu/bad %llu) vsMis=%llu/post %llu psMis=%llu/post %llu | regs(mis/derived)=%s | fetch=%s | alu=%s | "
              "boolloop=%s",
              why, (unsigned long long)g_recorded.load(), (unsigned long long)g_kindCount[0].load(),
              (unsigned long long)g_kindCount[1].load(), (unsigned long long)g_kindCount[2].load(),
              (unsigned long long)g_kindCount[3].load(), (unsigned long long)g_kindCount[4].load(),
              (unsigned long long)g_nested.load(),
              (unsigned long long)g_noBracket.load(), (unsigned long long)g_badDev.load(),
              (unsigned long long)g_sampledOut.load(), (unsigned long long)c.draws,
              (unsigned long long)c.byOp[0], (unsigned long long)c.byOp[1], (unsigned long long)c.joined,
              (unsigned long long)c.clean, (unsigned long long)c.regDraws, (unsigned long long)c.fetchDraws,
              (unsigned long long)c.aluReadDraws, (unsigned long long)c.relDraws, (unsigned long long)c.boolDraws, (unsigned long long)c.otherOwner,
              (unsigned long long)c.noBracket, (unsigned long long)c.missing, (unsigned long long)c.evicted,
              (unsigned long long)c.torn, (unsigned long long)c.postMissing, (unsigned long long)c.zpassDraws,
              (unsigned long long)c.skippedDraws, (unsigned long long)c.vsNatMis, (unsigned long long)c.vsNatNone,
              (unsigned long long)g_vsNone[0].load(), (unsigned long long)g_vsNone[1].load(),
              (unsigned long long)g_vsNone[2].load(), (unsigned long long)c.vsNatImm,
              (unsigned long long)c.vsNatImmMis, (unsigned long long)g_vsPatchFail.load(),
              (unsigned long long)g_vsGarbageDiff.load(), (unsigned long long)g_vsPatchMisses.load(),
              (unsigned long long)g_ipChecks.load(), (unsigned long long)g_ipBad.load(),
              (unsigned long long)c.vsMis,
              (unsigned long long)c.vsMisPost, (unsigned long long)c.psMis, (unsigned long long)c.psMisPost,
              regs.empty() ? "-" : regs.c_str(),
              fetch.empty() ? "-" : fetch.c_str(), alu.empty() ? "-" : alu.c_str(),
              boolc.empty() ? "-" : boolc.c_str());
  {
    uint64_t tot = 0, z = 0, up = 0, ch = 0, im = 0;
    for (uint32_t m = 0; m < 16; m++) {
      tot += c.cls[m];
      if (m & 1u) z += c.cls[m];
      if (m & 2u) up += c.cls[m];
      if (m & 4u) ch += c.cls[m];
      if (m & 8u) im += c.cls[m];
    }
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_REC] %s classes draws=%llu plain=%llu zpass=%llu up=%llu chunked=%llu immVS=%llu "
                "zpass&immVS=%llu zpassOnly=%llu immVSOnly=%llu",
                why, (unsigned long long)tot, (unsigned long long)c.cls[0], (unsigned long long)z,
                (unsigned long long)up, (unsigned long long)ch, (unsigned long long)im,
                (unsigned long long)c.cls[9], (unsigned long long)c.cls[1], (unsigned long long)c.cls[8]);
  }
  if (!c.aluWriters.empty()) {
    const size_t owners = size_t(sr::native::kEntryCount) + 1;
    std::string w;
    for (uint32_t blk = 0; blk < kAluBlocks; blk++) {
      if (!c.aluMis[blk]) continue;
      std::vector<std::pair<uint64_t, size_t>> v;
      for (size_t o = 0; o < owners; o++)
        if (c.aluWriters[blk * owners + o]) v.push_back({c.aluWriters[blk * owners + o], o});
      std::sort(v.rbegin(), v.rend());
      std::snprintf(b, sizeof(b), "%s%s%u<-", w.empty() ? "" : " | ", blk < 16 ? "vs" : "ps", (blk % 16) * 16);
      w += b;
      for (size_t k = 0; k < v.size() && k < 4; k++) {
        std::snprintf(b, sizeof(b), "%s%s:%llu", k ? "," : "",
                      entryName(v[k].second < size_t(sr::native::kEntryCount) ? int(v[k].second) : -1),
                      (unsigned long long)v[k].first);
        w += b;
      }
    }
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_REC] %s alu writers of mismatching blocks: %s", why,
                w.empty() ? "-" : w.c_str());
  }
  if (!c.ownerDraws.empty()) {
    std::vector<int> o;
    for (size_t i = 0; i < c.ownerDraws.size(); i++)
      if (c.ownerDraws[i]) o.push_back(int(i));
    std::sort(o.begin(), o.end(), [&](int a, int bb) { return c.ownerDraws[a] > c.ownerDraws[bb]; });
    std::string s;
    for (size_t k = 0; k < o.size() && k < 16; k++) {
      std::snprintf(b, sizeof(b), "%s%s:%llu", s.empty() ? "" : ",", entryName(o[k] < sr::native::kEntryCount ? o[k] : -1),
                    (unsigned long long)c.ownerDraws[o[k]]);
      s += b;
    }
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_REC] %s draw owners without a draw record: %s", why,
                s.empty() ? "-" : s.c_str());
  }
}

// ---- NATIVE mode B stage 1a: native draws (SR_NATIVE_DRAW, default OFF) --------------------------
// docs/NATIVE_ROADMAP.md "Mode B, the structural step". At a DrawIndexedVertices / DrawVertices entry
// the XDK body still runs (every guest-visible side effect stays the game's own) and encodes its
// packets into the command buffer as always. If that bracket is plain - no segment kick inside it,
// not in the Z-pass, only state the projection carries (type-0 / type-1 runs over covered
// registers, SET_CONSTANT / LOAD_ALU_CONSTANT into carried files, NOPs) and exactly one final
// DRAW_INDX / DRAW_INDX_2 - the render thread copies the bracket into a record slot and overwrites
// it IN PLACE with one type-3 NOP of the same length carrying {magic, slot}. The segment is not
// kicked yet, so the CP has not seen these bytes; the cursor, segment kicks and fences advance
// exactly as before. The CP reaches the NOP in stream order and runs ndExecute: it puts the
// original bytes back (a replay of the segment then runs plain PM4), writes the registers the
// bracket wrote FROM THE NATIVE PROJECTION (packed registers, fetch, bool/loop, the carried ALU
// file), and executes only the DRAW packet. Fallback to the original packets (exact): the carried
// ALU file disagrees with the record, every SR_NATIVE_DRAW_VERIFY_EVERY-th bracket (default 0 =
// none), and while a live trace records (the .xtr stays plain PM4 - the oracle P - and the .nrt
// sidecar receives the record for the native replay N, gfx/gpu_trace_replay SR_REPLAY_NRT=1).
namespace {
constexpr uint32_t kNdMagic = 0x4E445257u;  // 'NDRW'
constexpr uint32_t kNdCap = 2048u;          // slots (power of two)
constexpr uint32_t kNdMaxBytes = 8192u;     // longest bracket taken natively
constexpr uint32_t kNdMaxPass = 8u;         // packets run as PM4 in order: shader loads, bin mask/select
constexpr uint32_t kNdMaxLits = 8u;         // literal writes to SHADER_CONSTANT_FLUSH_FETCH 0x5000-0x5002
constexpr uint32_t kNdMaxRanges = 48u;

const bool g_ndOn = envOn("SR_NATIVE_DRAW");
const uint32_t g_ndVerifyEvery = envU32("SR_NATIVE_DRAW_VERIFY_EVERY", 0);

struct NdRange {
  uint16_t reg, count;
};
struct NdLit {
  uint32_t reg, value;
};
struct NdSlot {
  std::atomic<uint32_t> seq{0xFFFFFFFFu};       // published by the render thread
  std::atomic<uint32_t> consumed{0xFFFFFFFFu};  // = seq once the CP executed it
  uint32_t callId = 0, len = 0, drawOff = 0, bracketVA = 0;
  uint8_t kind = 0, nRanges = 0, nPass = 0;
  NdRange ranges[kNdMaxRanges];
  uint16_t passOff[kNdMaxPass], passLen[kNdMaxPass];  // byte offset / length in the bracket
  uint8_t passAfter[kNdMaxPass];  // ranges written before each pass-through packet (stream order)
  uint8_t nLits = 0;
  NdLit lits[kNdMaxLits];  // the fetch emitter's SHADER_CONSTANT_FLUSH_FETCH trailer, as written
  uint32_t pre[kRegN];
  uint32_t fetch[kFetchN];
  uint32_t boolc[kBoolN];
  uint64_t alu[kAluBlocks];
  uint8_t orig[kNdMaxBytes];
};
NdSlot* g_nd = nullptr;
uint32_t g_ndHead = 0;                  // producer: the render thread (draw entries)
std::mutex g_ndProduceMu;               // draw entries from more than one thread (rare)

struct NdArm {
  bool armed = false;
  uint32_t cur0 = 0, lim0 = 0, thr0 = 0;
};
thread_local NdArm t_nd;

// Render-side decline reasons and CP-side outcomes (relaxed counters).
enum NdWhy { kNdTaken, kNdKick, kNdZpass, kNdLen, kNdHost, kNdPacket, kNdNoDraw, kNdFull, kNdWhyN };
const char* const kNdWhyName[kNdWhyN] = {"taken", "kick", "zpass", "len", "host", "packet", "noDraw", "full"};
std::atomic<uint64_t> g_ndWhy[kNdWhyN];
std::atomic<uint64_t> g_ndBadOp[256];
std::atomic<uint64_t> g_ndLenHist[4];  // declined lengths: <12, <=16K, <=64K, more
// SET_CONSTANT / LOAD_ALU_CONSTANT type -> first register (Pm4Engine case 0x2D; 5 = unknown).
constexpr uint32_t kConstBase[6] = {0x4000u, 0x4800u, 0x4900u, 0x4908u, 0x2000u, 0u};
// Why ndParse declined (render thread counters) and the first uncovered register seen.
enum NdParseWhy { kPwTrunc, kPwUncov, kPwRanges, kPwCopyDest, kPwNotLast, kPwConst, kPwPass, kPwEnd, kPwN };
const char* const kPwName[kPwN] = {"trunc", "uncovered", "ranges", "copyDest", "notLast", "constType", "pass", "end"};
std::atomic<uint64_t> g_pw[kPwN];
std::atomic<uint32_t> g_uncovReg[0x6000];
std::atomic<uint64_t> g_ndNative{0}, g_ndAluFallback{0}, g_ndVerify{0}, g_ndTraced{0}, g_ndLost{0};

int16_t g_packedIdx[0x2400];  // register -> index into Rec::pre (-1 = not packed)
bool g_packedBuilt = [] {
  for (auto& v : g_packedIdx) v = -1;
  for (int i = 0; i < kRegN; i++) g_packedIdx[g_regOf[i]] = int16_t(i);
  return true;
}();

void ndReport(const char* why) {
  std::string bad;
  char b[48];
  for (int op = 0; op < 256; op++) {
    const uint64_t n = g_ndBadOp[op].load(std::memory_order_relaxed);
    if (!n) continue;
    std::snprintf(b, sizeof(b), " %s%02X:%llu", op >= 0x80 ? "t" : "op", op & 0x7F, (unsigned long long)n);
    bad += b;
  }
  std::string w;
  for (int i = 0; i < kNdWhyN; i++) {
    std::snprintf(b, sizeof(b), " %s=%llu", kNdWhyName[i], (unsigned long long)g_ndWhy[i].load());
    w += b;
  }
  std::string pw;
  for (int i = 0; i < kPwN; i++) {
    std::snprintf(b, sizeof(b), " %s=%llu", kPwName[i], (unsigned long long)g_pw[i].load());
    pw += b;
  }
  std::vector<std::pair<uint32_t, uint32_t>> uc;
  for (uint32_t r = 0; r < 0x6000u; r++)
    if (const uint32_t n = g_uncovReg[r].load(std::memory_order_relaxed)) uc.push_back({n, r});
  std::sort(uc.rbegin(), uc.rend());
  for (size_t i = 0; i < uc.size() && i < 12; i++) {
    std::snprintf(b, sizeof(b), " %04X:%u", uc[i].second, uc[i].first);
    pw += b;
  }
  SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_DRAW] %s parse declines:%s", why, pw.c_str());
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_DRAW] %s render:%s | CP: native=%llu aluFallback=%llu verify=%llu traced=%llu lost=%llu | "
              "declined packets:%s | declined len <12/<=16K/<=64K/more %llu/%llu/%llu/%llu",
              why, w.c_str(), (unsigned long long)g_ndNative.load(), (unsigned long long)g_ndAluFallback.load(),
              (unsigned long long)g_ndVerify.load(), (unsigned long long)g_ndTraced.load(),
              (unsigned long long)g_ndLost.load(), bad.empty() ? " -" : bad.c_str(),
              (unsigned long long)g_ndLenHist[0].load(), (unsigned long long)g_ndLenHist[1].load(),
              (unsigned long long)g_ndLenHist[2].load(), (unsigned long long)g_ndLenHist[3].load());
}

void ndDecline(NdWhy w) {
  const uint64_t n = g_ndWhy[w].fetch_add(1, std::memory_order_relaxed) + 1;
  (void)n;
}

// Parse the bracket [0, len) at `p` (guest bytes): true = natively executable; ranges = the
// registers it writes; drawOff = the DRAW packet's offset.
bool ndParse(const uint8_t* p, uint32_t len, NdRange* ranges, uint8_t& nRanges, uint32_t& drawOff, uint16_t* passOff,
             uint16_t* passLen, uint8_t& nPass, NdLit* lits, uint8_t& nLits, uint8_t* passAfter) {
  uint32_t off = 0;
  nRanges = 0;
  nPass = 0;
  nLits = 0;
  uint8_t boundary = 0;  // ranges never merge across a pass-through packet (stream order)
  bool haveDraw = false;
  auto uncov = [&](uint32_t reg, uint32_t count) -> bool {  // true = covered
    if (coveredRun(reg, count)) return true;
    g_pw[kPwUncov].fetch_add(1, std::memory_order_relaxed);
    for (uint32_t k = 0; k < count; k++)
      if (!coveredRun(reg + k, 1) && reg + k < 0x6000u) {
        g_uncovReg[reg + k].fetch_add(1, std::memory_order_relaxed);
        break;
      }
    return false;
  };
  auto addRange = [&](uint32_t reg, uint32_t count) -> bool {
    if (!count) return true;
    // RB_COPY_DEST_BASE / PITCH / INFO feed engine fields in Pm4Engine::writeReg: not native here.
    if (reg <= 0x231Bu && reg + count > 0x2319u) {
      g_pw[kPwCopyDest].fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    if (nRanges > boundary && ranges[nRanges - 1].reg + ranges[nRanges - 1].count == reg &&
        ranges[nRanges - 1].count + count < 0xFFFFu) {
      ranges[nRanges - 1].count = uint16_t(ranges[nRanges - 1].count + count);
      return true;
    }
    if (nRanges >= kNdMaxRanges) {
      g_pw[kPwRanges].fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    ranges[nRanges++] = NdRange{uint16_t(reg), uint16_t(count)};
    return true;
  };
  while (off + 4 <= len) {
    if (haveDraw) {  // the DRAW must be the last packet
      g_pw[kPwNotLast].fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    const uint32_t h = be32(p + off);
    const uint32_t type = h >> 30;
    if (h == 0) {
      off += 4;
      continue;
    }
    if (type == 0) {
      const uint32_t cnt = ((h >> 16) & 0x3FFFu) + 1u, reg = h & 0x7FFFu;
      if (off + 4u * (1u + cnt) > len) {
        g_pw[kPwTrunc].fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      if (reg >= 0x5000u && reg + (h & 0x8000u ? 1u : cnt) <= 0x5003u) {
        // SHADER_CONSTANT_FLUSH_FETCH_0..2 (Xenia register_table.inc): no side effect in Xenia or
        // Pm4Engine, a value the register file holds - kept as the literal the XDK wrote.
        for (uint32_t k = 0; k < cnt; k++) {
          if (nLits >= kNdMaxLits) {
            g_pw[kPwRanges].fetch_add(1, std::memory_order_relaxed);
            return false;
          }
          lits[nLits++] = NdLit{(h & 0x8000u) ? reg : reg + k, be32(p + off + 4u + 4u * k)};
        }
      } else if (h & 0x8000u) {  // one-register write: cnt values into reg
        if (!uncov(reg, 1) || !addRange(reg, 1)) return false;
      } else {
        if (!uncov(reg, cnt) || !addRange(reg, cnt)) return false;
      }
      off += 4u * (1u + cnt);
      continue;
    }
    if (type == 1) {
      const uint32_t r0 = h & 0x7FFu, r1 = (h >> 11) & 0x7FFu;
      if (off + 12u > len || !uncov(r0, 1) || !uncov(r1, 1)) return false;
      if (!addRange(r0, 1) || !addRange(r1, 1)) return false;
      off += 12u;
      continue;
    }
    if (type == 2) {
      off += 4u;
      continue;
    }
    const uint32_t op = (h >> 8) & 0x7Fu, cnt = ((h >> 16) & 0x3FFFu) + 1u;
    if (off + 4u * (1u + cnt) > len) return false;
    // Bin predication: the XDK sets it on every DRAW (0xC0032201); the engine still applies
    // (binSelect & binMask) when it executes the DRAW. A predicated STATE packet is not native.
    if ((h & 1u) && op != 0x22u && op != 0x36u) {
      g_ndBadOp[0x80 | op].fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    const uint8_t* body = p + off + 4;
    switch (op) {
      case 0x10:  // NOP
      case 0x3B:  // INVALIDATE_STATE: a hint, discarded by Xenia (:1374) and by Pm4Engine
        break;
      case 0x27:  // IM_LOAD (shader ucode by address): run as PM4 before the draw until stage 2
      case 0x2B:  // IM_LOAD_IMMEDIATE (the patched VS copy): the same until stage 2
      case 0x60:  // SET_BIN_MASK_LO / HI, SET_BIN_SELECT_LO / HI: CP predication state
      case 0x61:
      case 0x62:
      case 0x63:
        if (nPass >= kNdMaxPass) {
          g_pw[kPwPass].fetch_add(1, std::memory_order_relaxed);
          return false;
        }
        passOff[nPass] = uint16_t(off);
        passAfter[nPass] = nRanges;
        boundary = nRanges;
        passLen[nPass++] = uint16_t(4u * (1u + cnt));
        break;
      case 0x2D: {  // SET_CONSTANT: info, values
        if (cnt < 2) return false;
        const uint32_t info = be32(body), type3 = (info >> 16) & 0xFFu, offs = info & 0x7FFu;
        const uint32_t base = kConstBase[type3 < 5u ? type3 : 5u];
        if (!base) g_pw[kPwConst].fetch_add(1, std::memory_order_relaxed);
        if (!base || !uncov(base + offs, cnt - 1u) || !addRange(base + offs, cnt - 1u)) return false;
        break;
      }
      case 0x2F: {  // LOAD_ALU_CONSTANT: address, info, count
        if (cnt < 3) return false;
        const uint32_t info = be32(body + 4), type3 = (info >> 16) & 0xFFu, offs = info & 0x7FFu;
        const uint32_t n = be32(body + 8);
        const uint32_t base = kConstBase[type3 < 5u ? type3 : 5u];
        if (!base) g_pw[kPwConst].fetch_add(1, std::memory_order_relaxed);
        if (!base || !uncov(base + offs, n) || !addRange(base + offs, n)) return false;
        break;
      }
      case 0x22:  // DRAW_INDX
      case 0x36:  // DRAW_INDX_2
        haveDraw = true;
        drawOff = off;
        break;
      default:
        g_ndBadOp[op].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    off += 4u * (1u + cnt);
  }
  if (!(haveDraw && off == len)) {
    g_pw[kPwEnd].fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

void ndDisarm() { t_nd.armed = false; }

// Render thread, inside onDrawBefore once the record is complete.
void ndArm(uint32_t devVA, const uint8_t* d, uint8_t kind) {
  t_nd.armed = false;
  if (!g_ndOn) return;
  if (kind != kDrawIndexed && kind != kDrawAuto) return;
  t_nd.cur0 = be32(d + 0x28);
  t_nd.lim0 = be32(d + 0x2C);
  t_nd.thr0 = be32(d + 0x30);
  // XDK Z-pass mode: 820FC000 tests the BYTE at dev+10808 (lbz; rlwinm 0,30,30), i.e. bit 25 of
  // the big-endian dword there (the qword-bit reading, dword +10812 bit 1, was a different flag).
  if (rdG(devVA + 10808u) & 0x02000000u) {
    ndDecline(kNdZpass);
    return;
  }
  t_nd.armed = true;
}

// Render thread, inside onDrawAfter (the XDK body has returned).
void ndCommit(uint32_t devVA, const Rec& r) {
  if (!t_nd.armed) return;
  t_nd.armed = false;
  const uint8_t* d = nullptr;
  if (!deviceSpan(devVA, &d)) return;
  const uint32_t cur1 = be32(d + 0x28);
  if (be32(d + 0x2C) != t_nd.lim0 || be32(d + 0x30) != t_nd.thr0 || cur1 < t_nd.cur0) {
    ndDecline(kNdKick);
    return;
  }
  const uint32_t len = cur1 - t_nd.cur0;
  if (len < 12u || len > kNdMaxBytes || (len & 3u)) {
    ndDecline(kNdLen);
    g_ndLenHist[len < 12u ? 0 : len <= 16384u ? 1 : len <= 65536u ? 2 : 3].fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t va = t_nd.cur0 + 4u;
  uint8_t* host = sr::kernel::guestVAToHost(va);
  uint8_t* hostEnd = sr::kernel::guestVAToHost(va + len - 1u);
  if (!host || hostEnd != host + len - 1u) {
    ndDecline(kNdHost);
    return;
  }
  NdRange ranges[kNdMaxRanges];
  uint8_t nRanges = 0, nPass = 0;
  uint32_t drawOff = 0;
  uint16_t passOff[kNdMaxPass], passLen[kNdMaxPass];
  NdLit lits[kNdMaxLits];
  uint8_t nLits = 0;
  uint8_t passAfter[kNdMaxPass];
  if (!ndParse(host, len, ranges, nRanges, drawOff, passOff, passLen, nPass, lits, nLits, passAfter)) {
    ndDecline(kNdPacket);
    return;
  }
  // Strand A constant dedup: the stream up to the bracket's end is final - scan it before the
  // marker hides it.
  sr::native::cdedup::advanceTo(devVA, cur1);
  if (!g_nd) g_nd = new NdSlot[kNdCap];
  std::lock_guard<std::mutex> lk(g_ndProduceMu);
  const uint32_t seq = g_ndHead;
  NdSlot& s = g_nd[seq & (kNdCap - 1u)];
  // A slot is reused only once the CP executed its previous record (in any order).
  const uint32_t prev = s.seq.load(std::memory_order_relaxed);
  if (prev != 0xFFFFFFFFu && s.consumed.load(std::memory_order_acquire) != prev) {
    ndDecline(kNdFull);
    return;
  }
  s.seq.store(0xFFFFFFFFu, std::memory_order_relaxed);
  s.callId = r.seq.load(std::memory_order_relaxed);
  s.len = len;
  s.drawOff = drawOff;
  s.bracketVA = va;
  s.kind = r.kind;
  s.nRanges = nRanges;
  std::memcpy(s.ranges, ranges, sizeof(NdRange) * nRanges);
  s.nPass = nPass;
  s.nLits = nLits;
  std::memcpy(s.lits, lits, sizeof(NdLit) * nLits);
  std::memcpy(s.passOff, passOff, sizeof(uint16_t) * nPass);
  std::memcpy(s.passAfter, passAfter, nPass);
  std::memcpy(s.passLen, passLen, sizeof(uint16_t) * nPass);
  std::memcpy(s.pre, r.pre, sizeof(s.pre));
  std::memcpy(s.fetch, r.fetch, sizeof(s.fetch));
  std::memcpy(s.boolc, r.boolc, sizeof(s.boolc));
  std::memcpy(s.alu, r.alu, sizeof(s.alu));
  std::memcpy(s.orig, host, len);
  s.seq.store(seq, std::memory_order_release);
  g_ndHead = seq + 1u;
  // The marker: a type-3 NOP covering the whole bracket (count field = payload dwords - 1).
  const uint32_t hdr = 0xC0000000u | (((len / 4u) - 2u) << 16) | (0x10u << 8);
  const uint32_t w[3] = {_byteswap_ulong(hdr), _byteswap_ulong(kNdMagic), _byteswap_ulong(seq)};
  std::memcpy(host, w, sizeof(w));
  const uint64_t n = g_ndWhy[kNdTaken].fetch_add(1, std::memory_order_relaxed) + 1;
  if ((n & (n - 1)) == 0 && n >= 1024) ndReport("periodic");
}

// The value the projection gives register `reg` (CP thread).
inline bool ndValue(const NdSlot& s, uint32_t reg, uint32_t& v) {
  if (reg < 0x2400u && g_packedIdx[reg] >= 0) {
    v = s.pre[g_packedIdx[reg]];
    return true;
  }
  if (reg - kFetchReg < kFetchN) {
    v = s.fetch[reg - kFetchReg];
    return true;
  }
  if (reg - kBoolReg < kBoolN) {
    v = s.boolc[reg - kBoolReg];
    return true;
  }
  if (reg - kAluReg < kAluN) {
    uint32_t be;
    std::memcpy(&be, g_drive.aluImg + 4u * (reg - kAluReg), 4);
    v = _byteswap_ulong(be);
    return true;
  }
  return false;
}

// .nrt sidecar record (native replay N): the bracket's place and its projection.
struct NrtRec {
  uint32_t magic, phys, len, drawOff, kind, nRanges, nPass, nLits;
  uint16_t passOff[kNdMaxPass];
  uint8_t passAfter[kNdMaxPass];
  NdLit lits[kNdMaxLits];
  NdRange ranges[kNdMaxRanges];
  uint32_t pre[kRegN];
  uint32_t fetch[kFetchN];
  uint32_t boolc[kBoolN];
  uint32_t alu[kAluN];  // host order
};
}  // namespace

bool ndOn() { return g_ndOn; }

void ndExecute(uint8_t* host, uint32_t seq, void (*exec)(void*, const uint8_t*, size_t), void* eng) {
  if (!g_nd) {
    g_ndLost.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  NdSlot& s = g_nd[seq & (kNdCap - 1u)];
  if (s.seq.load(std::memory_order_acquire) != seq) {
    const uint64_t n = g_ndLost.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8) SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_DRAW] LOST slot seq=%u (slot holds %u)", seq, s.seq.load());
    return;
  }
  // The original packets go back in place: a later replay of this segment runs plain PM4.
  std::memcpy(host, s.orig, s.len);
  const bool aluOk = aluDrain(s.callId, s.alu);
  const bool trace = sr::gfx::livetrace::recording();
  static uint32_t s_n = 0;
  const bool verify = g_ndVerifyEvery && (++s_n % g_ndVerifyEvery) == 0;
  if (trace && aluOk) {
    static NrtRec rec;
    rec.magic = 0x4E524543u;  // 'NREC'
    rec.phys = sr::gfx::livetrace::hostPhys(host);
    rec.len = s.len;
    rec.drawOff = s.drawOff;
    rec.kind = s.kind;
    rec.nRanges = s.nRanges;
    rec.nPass = s.nPass;
    std::memcpy(rec.passAfter, s.passAfter, sizeof(rec.passAfter));
    rec.nLits = s.nLits;
    std::memcpy(rec.lits, s.lits, sizeof(rec.lits));
    std::memcpy(rec.passOff, s.passOff, sizeof(rec.passOff));
    std::memcpy(rec.ranges, s.ranges, sizeof(rec.ranges));
    std::memcpy(rec.pre, s.pre, sizeof(rec.pre));
    std::memcpy(rec.fetch, s.fetch, sizeof(rec.fetch));
    std::memcpy(rec.boolc, s.boolc, sizeof(rec.boolc));
    for (uint32_t k = 0; k < kAluN; k++) {
      uint32_t be;
      std::memcpy(&be, g_drive.aluImg + 4u * k, 4);
      rec.alu[k] = _byteswap_ulong(be);
    }
    sr::gfx::livetrace::sidecarAppend(&rec, sizeof(rec));
  }
  if (!aluOk || verify || trace) {
    (!aluOk ? g_ndAluFallback : verify ? g_ndVerify : g_ndTraced).fetch_add(1, std::memory_order_relaxed);
    exec(eng, host, s.len);
  } else {
    sr::gfx::XenosRegisterFile& x = sr::gfx::getXenosRegisters();
    if (sr::native::cdedup::checkOn()) sr::native::cdedup::cpCheckPackets(host, s.len, x.mutableData());
    // Stream order: the ranges written before each pass-through packet, the packet, ... the rest.
    auto applyRanges = [&](uint32_t from, uint32_t to) {
      for (uint32_t i = from; i < to; i++)
        for (uint32_t k = 0; k < s.ranges[i].count; k++) {
          const uint32_t reg = s.ranges[i].reg + k;
          uint32_t v;
          if (ndValue(s, reg, v)) x.write(reg, v);
        }
    };
    uint32_t done = 0;
    for (uint32_t i = 0; i < s.nPass; i++) {
      applyRanges(done, s.passAfter[i]);
      done = s.passAfter[i];
      exec(eng, host + s.passOff[i], s.passLen[i]);
    }
    applyRanges(done, s.nRanges);
    for (uint32_t i = 0; i < s.nLits; i++) x.write(s.lits[i].reg, s.lits[i].value);
    g_ndNative.fetch_add(1, std::memory_order_relaxed);
    exec(eng, host + s.drawOff, s.len - s.drawOff);
  }
  s.consumed.store(seq, std::memory_order_release);
}

// Replay (gfx/gpu_trace_replay, SR_REPLAY_NRT=1): the registers a recorded bracket wrote, from its
// projection. `rec` = one NrtRec of the sidecar.
size_t nrtRecordBytes() { return sizeof(NrtRec); }
namespace {
void nrtApplyRanges(const NrtRec& r, uint32_t from, uint32_t to) {
  sr::gfx::XenosRegisterFile& x = sr::gfx::getXenosRegisters();
  for (uint32_t i = from; i < to && i < r.nRanges && i < kNdMaxRanges; i++)
    for (uint32_t k = 0; k < r.ranges[i].count; k++) {
      const uint32_t reg = r.ranges[i].reg + k;
      if (reg < 0x2400u && g_packedIdx[reg] >= 0) x.write(reg, r.pre[g_packedIdx[reg]]);
      else if (reg - kFetchReg < kFetchN) x.write(reg, r.fetch[reg - kFetchReg]);
      else if (reg - kBoolReg < kBoolN) x.write(reg, r.boolc[reg - kBoolReg]);
      else if (reg - kAluReg < kAluN) x.write(reg, r.alu[reg - kAluReg]);
    }
}
}  // namespace

int nrtPacket(const void* rec, uint32_t phys) {
  const NrtRec& r = *static_cast<const NrtRec*>(rec);
  if (r.magic != 0x4E524543u || phys < r.phys || phys >= r.phys + r.len) return 0;
  static const void* s_rec = nullptr;  // the replay is one thread
  static uint32_t s_done = 0;
  if (s_rec != rec) {
    s_rec = rec;
    s_done = 0;
  }
  for (uint32_t i = 0; i < r.nPass && i < kNdMaxPass; i++)
    if (phys == r.phys + r.passOff[i]) {  // the ranges before it, then the packet as PM4
      nrtApplyRanges(r, s_done, r.passAfter[i]);
      s_done = std::max<uint32_t>(s_done, r.passAfter[i]);
      return 2;
    }
  if (phys == r.phys + r.drawOff) {
    nrtApplyRanges(r, s_done, r.nRanges);
    sr::gfx::XenosRegisterFile& x = sr::gfx::getXenosRegisters();
    for (uint32_t i = 0; i < r.nLits && i < kNdMaxLits; i++) x.write(r.lits[i].reg, r.lits[i].value);
    s_rec = nullptr;
    return 3;
  }
  return 1;
}

void ndReportHook(const char* why) {
  if (g_ndOn) ndReport(why);
}

}  // namespace sr::native::dev
