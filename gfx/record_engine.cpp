// The record engine: the shipping half of the Xenos command processor - the device-state shadow (XenosRegisterFile,
// record_engine.hpp) and what a native record does to it: register runs with their side effects (the CP scratch
// write-back, the DC_LUT gamma ramp, the coherency accounting, the copy-destination latches), the shader binds and
// constant loads, the bin predication, the events, fences, coherency blocks, interrupts and swaps, and the draw emission
// (emitDraw -> the native draw builder -> the DrawCommandQueue). Split out of gfx/pm4_engine.cpp with the bodies
// unchanged (C1, docs/RELEASE_COMPOSITION.md 2.1 and 3.2 stage 2a, 2026-10-15): the PM4 packet walker - execute(),
// cpPacket(), the window scan - stays in Pm4Engine, which derives from this class and leaves the release link with the
// development oracle. Record consumers reach this class through gfx/record_context.cpp (the NativeRenderContext);
// nothing here decodes a packet header. The SR_NATIVE_EXEC executor's branches (dev-only since stage 0) are #if SR_ORACLE.

#include "record_engine.hpp"
#include "record_engine_detail.hpp"
#include "../kernel/tex_layout.hpp"   // iter-456p: the ONE canonical fetch decoder
#include "../kernel/gpu_hang_wd.hpp"   // iter-463: the fence-write latch
#include "../kernel/native_drain_census.hpp"  // strand A: drain census (observe-only)
#include "pm4_live_trace.hpp"
#include "native_zpd.hpp"             // p2.zpd: the covering-fence check before an EVENT_WRITE_SHD store
#include "../kernel/sr_oracle.hpp"
#if SR_ORACLE
#include "resource_memory_check.hpp"
#include "draw_contract.hpp"          // iter-456p: the target draw-contract row
#include "null_renderer.hpp"          // nativeExec* (the SR_NATIVE_EXEC executor thread beside the drain)
#include "../kernel/native_flush.hpp"   // cpSegPartDescribe (the SCRATCH_CFG observe line), cpCurrentRecord (the draw ledger)
#endif
#include "native_draw_builder.hpp"
#include <intrin.h>
#include "pm4_coverage.hpp"   // NATIVE N0.2: true per-packet counter (SR_PM4_COVERAGE)
#include "../kernel/native_meter.hpp"   // iter-467 native meter
#include "resource_census.hpp"          // mem.resources step 2a: the coherency note (no-op under SR_ORACLE=0)
#include "resource_memory.hpp"
#include "draw_ledger.hpp"              // iter-467 flicker: the per-draw ledger over frozen presents
#include "../kernel/native_exec.hpp"    // iter-467 native A: the executor's gates (inline no-ops under SR_ORACLE=0)
#include "../kernel/native_vblank.hpp"  // display.registers: the SCRATCH_REG1 (pc+4 rearm) generation
#include "gamma_ramp.hpp"
#include "shader_cache.hpp"
#include "xenos_state.hpp"
#include "xenos_ucode.hpp"
#include "cp_simd.hpp"
#include "../app/log.hpp"
namespace sr { namespace kernel { uint8_t* guestVAToHost(uint32_t guestVA); bool isGuestVAInRange(uint32_t guestVA); bool logVerbose(); } }

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sr {
namespace gfx {
using namespace redetail;

// The one device-state shadow (every engine instance writes it; the backend's drain snapshots it per draw).
XenosRegisterFile& getXenosRegisters() {
  static XenosRegisterFile s_regs;
  return s_regs;
}

namespace {
constexpr uint32_t kVfetchVerboseCap = 8;

// iter-369d: capture the RAW ucode of shaders that MISS the translated-shader
// cache, so they can be run through the offline XenosRecomp pipeline and folded
// back into data/shader_cache/dxil.
//
// Why this matters: the offline cache was built from the MENU GPU trace (53 VS +
// 56 PS). A live boot uses DIFFERENT shaders, so nearly every lookup misses and
// the draw falls back to the fixed-function painter — measured on a deep roll,
// only 84 of ~3900 draws took the translated path (the replay takes 3379 of
// 3501). That fallback is why every resolved surface comes back uniform.
//
// Writes <16hex>.<vs|ps>.bin — the raw BIG-ENDIAN ucode byte stream, which is
// exactly what the cache hashes and what the offline tool consumes.
// SR_SHADER_DUMP_MISSES=1 (default OFF — it writes files). render/shader-coverage:
// SR_SHADER_DUMP_MISSES_DIR=<dir> writes there instead of the working directory's shader_misses/ (created;
// a roll's evidence folder - the capture runs that grow the AOT cache).
bool shaderMissDumpEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_SHADER_DUMP_MISSES");
    return e && e[0] == '1';
  }();
  return v;
}

void dumpMissedShader(uint64_t hash, bool isVertex, const uint8_t* ucode,
                      size_t bytes) {
  if (!shaderMissDumpEnabled() || !ucode || !bytes) return;
  static std::mutex mu;
  static std::unordered_set<uint64_t> seen;
  std::lock_guard<std::mutex> lk(mu);
  if (seen.size() >= 4096 || !seen.insert(hash).second) return;  // once per hash
  static const std::string s_dir = [] {
    const char* e = std::getenv("SR_SHADER_DUMP_MISSES_DIR");
    std::string d = (e && *e) ? std::string(e) : std::string("shader_misses");
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(d), ec);  // best effort; a failure warns below
    return d;
  }();
  char name[48];
  std::snprintf(name, sizeof(name), "/%016llx.%s.bin", static_cast<unsigned long long>(hash), isVertex ? "vs" : "ps");
  const std::string path = s_dir + name;
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(ucode, 1, bytes, f);
    std::fclose(f);
  } else {
    static bool warned = false;
    if (!warned) {
      warned = true;
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[SHADER_MISS] cannot write %s — create the shader_misses/ "
                  "directory first (or set SR_SHADER_DUMP_MISSES_DIR)", path.c_str());
    }
  }
}
}  // namespace

// iter-441 MIGRATION STAGE-A: g_hleSuppressPm4Draws + g_hleShaderFix deleted
// (dead since the hle_d3d9.cpp draw-synth renderer was removed).

// SR_PM4_REGLOG=<hex reg>: the watched register (0 = none).
// iter-466 cp round 2: read once at static initialisation (the environment is the process's
// own from its start), not behind a function-local static's guard - writeReg and every type-3
// constant load ask for it, and the guard check was ~1.5% of the CP.
// iter-466 cp round 2 (SR_CP_SETCONST_SSE): the SET_CONSTANT / LOAD_ALU_CONSTANT runs of ALU,
// bool and loop constants (~1.8M register writes a gameplay frame) as one byte-swapping SSE copy
// into the register file - the same stores XenosRegisterFile::write makes (registers past the
// file dropped), with the constant-block dirty bits it sets.
static bool setConstSse() {
  static const bool v = cpsimd::gateLogged("SR_CP_SETCONST_SSE", true) && cpsimd::available();
  return v;
}
void redetail::bulkBeStore(XenosRegisterFile& xr, uint32_t start, const uint8_t* srcBE, uint32_t n) {
  const uint32_t inFile = start < XenosRegisterFile::kNumRegisters
                              ? std::min<uint32_t>(n, XenosRegisterFile::kNumRegisters - start)
                              : 0u;
  if (!inFile) return;
  cpsimd::bswapCopy32(xr.mutableData() + start, srcBE, inFile);
  xr.noteWritten(start, inFile);
}

static const uint32_t g_regLogWatch = [] {
  const char* e = std::getenv("SR_PM4_REGLOG");
  return e ? uint32_t(std::strtoul(e, nullptr, 16)) : 0u;
}();
static inline uint32_t regLogWatch() { return g_regLogWatch; }

// iter-465 perf lane: does [r0, r0+n) touch a register writeReg gives a side effect (the CP
// scratch write-back, the gamma ramp, the fetch-constant-0 census, SR_PM4_REGLOG, the copy
// destination latches)? A type-0 packet that touches none of them is pure register-file stores.
bool redetail::bulkType0Enabled() {  // SR_PM4_BULK_TYPE0 (default ON; =0 keeps the per-register A/B)
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_BULK_TYPE0");
    return !e || e[0] != '0';
  }();
  return v;
}
// Under the P2 trace (SR_P2_TRACE) a run over SCRATCH_UMSK / SCRATCH_ADDR (0x1DC / 0x1DD) takes writeReg too, so its
// `regw` record exists (observe-only: the same stores; the segd wedge's write-backs died on an untraced UMSK write).
#if SR_ORACLE
static const bool g_traceScratchCfg = [] {
  const char* e = std::getenv("SR_P2_TRACE");
  return e && e[0] && e[0] != '0';
}();
#else
// SR_ORACLE=0 (kernel/sr_oracle.hpp): the P2 trace is compiled out (kernel/p2_trace.hpp), so SR_P2_TRACE reads as unset.
static constexpr bool g_traceScratchCfg = false;
#endif
bool redetail::regRangeHasEffects(uint32_t r0, uint32_t n) {
  const uint64_t a = r0, b = uint64_t(r0) + n;  // [a, b)
  auto hit = [&](uint32_t lo, uint32_t hi) { return a <= hi && b > lo; };  // [lo, hi] inclusive
  if (hit(0x578u, 0x57Fu)) return true;
  if (g_traceScratchCfg && hit(0x1DCu, 0x1DDu)) return true;
  if (hit(sr::gfx::gamma::kRegRwMode, sr::gfx::gamma::kRegWriteEnMask)) return true;
  if (hit(0x4800u, 0x4805u)) return true;
  if (hit(0x2319u, 0x231Bu) || hit(0x319u, 0x31Bu)) return true;
  if (hit(0x0A30u, 0x0A30u)) return true;  // COHER_BASE_HOST: writeReg counts the coherency request (cpCoher*)
  const uint32_t w = regLogWatch();
  if (w && hit(w, w)) return true;
  return false;
}

// NATIVE strand A (SR_NATIVE_IDLE_DIAG=1, diagnostic): CP time spent in the pre-observation drain
// (cp.flush), by site: 0 WAIT_REG_MEM, 1 INTERRUPT, 2 XE_SWAP, 3 EVENT_WRITE_SHD, 4 MEM_WRITE,
// 5 REG_TO_MEM. The guest's BlockUntilIdle waits for exactly this (plus the CP's backlog).
void redetail::cpFlushTimed(void (*flush)(void*), void* user, int site) {
  if (!flush) return;
  // C1: a development measurement (the CP half of kernel/cs_timeout_underflow_fix.cpp's BlockUntilIdle timing); under
  // SR_ORACLE=0 (kernel/sr_oracle.hpp) the gate is not read and is false, as unset.
  static const bool s_on =
#if SR_ORACLE
      [] {
        const char* e = std::getenv("SR_NATIVE_IDLE_DIAG");
        return e && e[0] == '1';
      }();
#else
      false;
#endif
  if (!s_on) {
    flush(user);
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  flush(user);
  const auto t1 = std::chrono::steady_clock::now();
  static uint64_t s_n[6], s_total = 0;
  static double s_us[6], s_max[6];
  const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
  s_n[site]++;
  s_us[site] += us;
  if (us > s_max[site]) s_max[site] = us;
  if ((++s_total & 4095u) == 0) {
    static const char* const kName[6] = {"wait", "interrupt", "swap", "eventShd", "memWrite", "regToMem"};
    char line[512];
    int o = 0;
    for (int i = 0; i < 6; i++)
      if (s_n[i])
        o += std::snprintf(line + o, sizeof(line) - size_t(o), " %s n=%llu mean=%.0fus max=%.0fus", kName[i],
                           (unsigned long long)s_n[i], s_us[i] / double(s_n[i]), s_max[i]);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_IDLE] CP drains after %llu:%s", (unsigned long long)s_total,
                o ? line : " -");
  }
}


// NATIVE strand A S1 (kernel/native_flush.hpp): a native draw record's register run - exactly the
// type-0 branch of execute() for a run (cpMode, not one-register), minus the header decode.
void RecordEngine::nativeRun(uint32_t startReg, uint32_t cnt0, const uint8_t* be) {
  startReg &= 0x7FFFu;
  if (cnt0 > 1 && bulkType0Enabled() && !regRangeHasEffects(startReg, cnt0)) {
    auto& xr = getXenosRegisters();
    static const bool s_bulkSse = cpsimd::gateLogged("SR_CP_BULK_SSE", true) && cpsimd::available();
    if (s_bulkSse) {
      bulkBeStore(xr, startReg, be, cnt0);
    } else {
      for (uint32_t i = 0; i < cnt0; i++) xr.write(startReg + i, readBE32(be + i * 4));
    }
    regWrites += cnt0;
  } else {
    for (uint32_t i = 0; i < cnt0; i++) writeReg(startReg + i, readBE32(be + i * 4));
  }
}

static constexpr uint64_t kShLoadPsSalt = 0x9E3779B97F4A7C15ull;  // SR_CP_SHLOAD_MEMO: PS keys

// ... and its DRAW_INDX: the bin predication of a predicated type-3 packet, then the draw.
void RecordEngine::nativeDraw(bool predicated, uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize) {
  if (predicated && (binSelect & binMask) == 0) {
    predSkipped++;
    return;
  }
  if (predicated) predDrawsRun++;  // tile census: a tile-predicated draw that runs in this bin
  opcodeHist[0x22]++;
  curPredicated = predicated;
  emitDraw(initiator, dmaBase, dmaSize);
  curPredicated = false;
}

// NATIVE strand A P1 rows 1+2: the packet bodies the PM4 path and the native records share. Each
// is the former case body of execute(), moved verbatim (the replay fixture, 9/9, is the proof that
// execute() is unchanged by the move).

void RecordEngine::applySetConstant(uint32_t offsetType, const uint8_t* valuesBE, uint32_t n) {
  // PM4_SET_CONSTANT — THE register write path.
  // index = offset_type & 0x7FF; type = (offset_type >> 16) & 0xFF;
  // type 0 -> ALU (0x4000), 1 -> FETCH (0x4800), 2 -> BOOL (0x4900),
  // 3 -> LOOP (0x4908), 4 -> REGISTERS (0x2000)
  uint32_t index = offsetType & 0x7FF;
  uint32_t constType = (offsetType >> 16) & 0xFF;
  uint32_t baseReg = 0;
  switch (constType) {
    case 0: baseReg = 0x4000; break;  // ALU constants
    case 1: baseReg = 0x4800; break;  // FETCH constants  <-- vertex fetch
    case 2: baseReg = 0x4900; break;  // BOOL constants
    case 3: baseReg = 0x4908; break;  // LOOP constants
    case 4: baseReg = 0x2000; break;  // config registers
    default: baseReg = 0; break;
  }
  if (baseReg) {
    // iter-461: ALU / BOOL / LOOP constants have no register side
    // effects (writeReg's scratch, DC_LUT, fetch-0 and copy-dest
    // latches all lie outside them) - a city frame writes ~1.9M of
    // them, and the checked per-register path was ~15 ms of it.
    if (constType != 1u && constType != 4u && !regLogWatch()) {
      auto& xr = getXenosRegisters();
      const uint32_t start = baseReg + index;
      if (setConstSse() && n > 0u) {
        bulkBeStore(xr, start, valuesBE, n);
      } else {
        for (uint32_t i = 0; i < n; i++) xr.write(start + i, readBE32(valuesBE + i * 4));
      }
      regWrites += n;
    } else {
      for (uint32_t i = 0; i < n; i++)
        writeReg(baseReg + index + i, readBE32(valuesBE + i * 4));
    }
  }
}

void RecordEngine::applyLoadAluConstant(uint32_t srcPhys, uint32_t offsetType, uint32_t sizeDwords) {
  // PM4_LOAD_ALU_CONSTANT — load constants FROM MEMORY
  uint32_t startIndex = offsetType & 0x7FF;
  uint32_t constType = (offsetType >> 16) & 0xFF;
  uint32_t baseReg = (constType == 1) ? 0x4800   // FETCH
                   : (constType == 2) ? 0x4900   // BOOL
                   : (constType == 3) ? 0x4908   // LOOP
                                      : 0x4000;  // ALU
  if (sizeDwords > 0 && sizeDwords < 4096) {
    const uint8_t* src = physToHost(user, srcPhys);
    // iter-460 (SR_PM4_CONST_TRACE=1): a load covering VS c4
    // (reg 0x4010): the value in the window read vs the other window.
    static const bool s_ct = [] {
      const char* e = std::getenv("SR_PM4_CONST_TRACE");
      return e && e[0] == '1';
    }();
    if (s_ct && baseReg == 0x4000 && startIndex <= 0x10 && startIndex + sizeDwords > 0x10) {
      static std::atomic<uint32_t> s_n{0};
      const uint32_t k = s_n.fetch_add(1);
      if (k < 300 || k % 1000 == 0) {
        const uint32_t off = (0x10 - startIndex) * 4;
        const uint8_t* a = sr::kernel::guestVAToHost(0xA0000000u + (srcPhys & 0x1FFFFFFFu));
        const uint8_t* e8 = sr::kernel::guestVAToHost(0x80000000u + (srcPhys & 0x1FFFFFFFu));
        auto f = [](const uint8_t* p) {
          if (!p) return -12345.0;
          const uint32_t u = readBE32(p);
          float v;
          std::memcpy(&v, &u, 4);
          return double(v);
        };
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[CONST_TRACE] #%u LOAD_ALU src=%08X start=%u n=%u c4.x: read(0xA)=%g 0x8=%g "
                    "| c4.y: 0xA=%g 0x8=%g",
                    k, srcPhys, startIndex, sizeDwords, f(a ? a + off : nullptr),
                    f(e8 ? e8 + off : nullptr), f(a ? a + off + 4 : nullptr),
                    f(e8 ? e8 + off + 4 : nullptr));
      }
    }
    if (src) {
      // iter-460 live trace: the constant block Xenia's playback must
      // see (without it every memory-loaded constant plays as zero:
      // the trace renders dark).
      if (cpMode) sr::gfx::livetrace::memRead(srcPhys, src, sizeDwords * 4u);
      if (baseReg != 0x4800u && !regLogWatch()) {  // iter-461: bulk (see SET_CONSTANT)
        auto& xr = getXenosRegisters();
        if (setConstSse()) {
          bulkBeStore(xr, baseReg + startIndex, src, sizeDwords);
        } else {
          for (uint32_t i = 0; i < sizeDwords; i++)
            xr.write(baseReg + startIndex + i, readBE32(src + i * 4));
        }
        regWrites += sizeDwords;
      } else {
        for (uint32_t i = 0; i < sizeDwords; i++)
          writeReg(baseReg + startIndex + i, readBE32(src + i * 4));
      }
    }
  }
}

void RecordEngine::applyImLoad(uint32_t addrType, uint32_t startSize) {
  // PM4_IM_LOAD — shader ucode, pointer to memory.
  //   dword0 = addr | shader_type(low 2 bits); dword1 = (start<<16) | size_dwords
  const uint32_t addr = addrType & ~0x3u;
  const uint32_t sizeDw = startSize & 0xFFFFu;
  const uint8_t* host = physToHost(user, addr);
  // iter-460 PLATFORM FIX: one physical address, two host windows in
  // our arena (0xA physical allocations, 0x8 heap-routed physical
  // grants - kernel/phys_window.hpp). The console reads the single
  // physical RAM, so when the engine's window does not hold Xenos
  // ucode but the other window does, the other window IS the shader.
  // Structural test only (hasValidControlFlow), fault-free bounds.
  // Measured before: 32,769 IM_LOADs rejected as all-zero at the title
  // city, leaving stale shaders bound (VSF hang, garbage geometry).
  if (host && sizeDw && sizeDw <= 4096u) {
    auto valid = [&](const uint8_t* p) {
      // iter-466 cp round 2 (SR_CP_SHLOAD_MEMO): bytes onShaderLoad already decoded
      // as valid ucode of this type need no second swap + check.
      static const bool s_vm = cpsimd::gate("SR_CP_SHLOAD_MEMO", true);
      if (s_vm) {
        const uint64_t k = ShaderCache::hashUcode(p, size_t(sizeDw) * 4) ^
                           ((addrType & 3u) == 0u ? 0ull : kShLoadPsSalt);
        auto it = shLoadMemo.find(k);
        if (it != shLoadMemo.end() && it->second.dwords == sizeDw) return true;
      }
      std::vector<uint32_t> tmp(sizeDw);
      for (uint32_t i = 0; i < sizeDw; i++) tmp[i] = readBE32(p + i * 4);
      return ucode::hasValidControlFlow(tmp.data(), tmp.size());
    };
    if (!valid(host)) {
      const uint32_t off = addr & 0x1FFFFFFFu;
      const uint32_t va8 = 0x80000000u + off, vaA = 0xA0000000u + off;
      const uint32_t altVa = (sr::kernel::guestVAToHost(va8) == host) ? vaA : va8;
      const uint8_t* alt = sr::kernel::guestVAToHost(altVa);
      if (alt && alt != host && sr::kernel::isGuestVAInRange(altVa + sizeDw * 4u - 1u) &&
          valid(alt)) {
        host = alt;
        imLoadWindowRescues++;
        static std::atomic<uint32_t> s_rl{0};
        const uint32_t rn = s_rl.fetch_add(1) + 1;
        if (rn <= 8 || (rn & (rn - 1)) == 0)
          SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[PM4_IMLOAD_WINDOW] #%u %s ucode at phys %08X is in the %s window "
                      "(%u dwords)",
                      rn, (addrType & 3u) == 0 ? "VERTEX" : "PIXEL", addr,
                      altVa == va8 ? "0x8" : "0xA", sizeDw);
      }
    }
  }
  onShaderLoad(addrType & 0x3u, addr, host, sizeDw);
}

void RecordEngine::applyImLoadImmediate(uint32_t shaderType, uint32_t startSize, const uint8_t* ucodeBE,
                                     uint32_t cnt) {
  // PM4_IM_LOAD_IMMEDIATE — shader ucode INLINE in the packet.
  //   dword0 = shader_type; dword1 = (start<<16) | size_dwords; then ucode.
  uint32_t sizeDwords = startSize & 0xFFFFu;
  if (sizeDwords > 0 && (2u + sizeDwords) <= cnt)
    onShaderLoad(shaderType & 0x3u, 0, ucodeBE, sizeDwords, /*inlineUcode=*/true);
}

void RecordEngine::applyEventWrite(uint32_t initiator) {
  // PM4_EVENT_WRITE — event that writes to memory on completion.
  // Xenia ExecutePacketType3_EVENT_WRITE (pm4_command_processor_implement.h:867):
  // write the initiator to VGT_EVENT_INITIATOR (reg 0x21F9); count==1 is
  // just an event flag, count>1 (memory writeback) is unexpected in
  // practice and consumed by pktSz in execute().
  writeReg(0x21F9, initiator & 0x3F);
}

// p2.zpd (SR_NATIVE_ZPD, baked default ON since native/zpd-bake): EVENT_WRITE_ZPD's effect. The kRkZpd record
// (kernel/native_flush's port of GpuEmitTileSurfaceConfig_82101E08) and the development walker's 0x5B packet both land
// here: the count, VGT_EVENT_INITIATOR as the packet writes it, then the host occlusion query (zpdNative). The fake
// fallback (SR_CP_ZPD_FAKE) stays the walker's, for a packet the native path did not take.
bool RecordEngine::applyZpdEvent(uint32_t initiator, bool record) {
  sr::gfx::zpd::noteSource(record);  // the frame stamp (Astra round 2 P2-1): which form carried the event
  cpZpdEvents++;
  writeReg(0x21F9u, initiator & 0x3Fu);
  return zpdNative();
}

bool RecordEngine::zpdNative() {
  if (!cpMode || !submitDraws || !sr::gfx::zpd::on()) return false;
  applyZpd(getXenosRegisters().read(0x2325u));
  return true;
}

// The logical query (gfx/native_zpd) and its marker at this stream position. The draw queue is FIFO, so the marker
// reaches the batch list between the query's draws on every drain path (the CP's own drain or the executor's); the
// render loop opens / closes the host query there.
void RecordEngine::applyZpd(uint32_t sampleCountAddr) {
  namespace zpd = sr::gfx::zpd;
  const uint32_t a = sampleCountAddr & 0x1FFFFFFFu;
  // The bytes D3DQueryGetData_82102DC8 reads: VA 0xC0000000 + the slot's RB_SAMPLE_COUNT_ADDR value, through the
  // same window fold the guest's loads take (aliasGuestVA), so the write is exactly what the guest sees.
  uint8_t* slot = a ? sr::kernel::guestVAToHost(0xC0000000u + (a & ~(zpd::kSlotBytes - 1u))) : nullptr;
  // zpd7 (the Xenia oracle): inside a traced frame the query's outcome is logged against that trace's swap; a BEGIN
  // outside one may request the next frame (SR_PM4_TRACE_ZPD, with the trace hotkey armed).
  const uint32_t traced = sr::gfx::livetrace::tracedSwap();
  const zpd::Event ev = zpd::onEvent(a, slot, traced);
  cpZpdNative++;
  if (ev.op == zpd::kNone) return;
  if (ev.op == zpd::kBegin && !traced) sr::gfx::livetrace::requestOnQuery();
  const bool defer = sr::native::exec::deferring();
  GuestDrawCommand* s = getDrawCommandQueue().reserve();
#if SR_ORACLE  // the SR_NATIVE_EXEC executor is the development configuration's (deferring() is false under SR_ORACLE=0)
  if (!s && defer) s = sr::gfx::nativeExecReserve();  // a full queue waits for the executor
#endif
  if (!s && !defer) {                                 // a full queue: drain it, as the CP does before an effect
    cpFlushTimed(cp.flush, cp.user, 3);
    s = getDrawCommandQueue().reserve();
  }
  if (!s) {
    zpd::markerDropped(ev);
    return;
  }
  std::memset(s, 0, offsetof(GuestDrawCommand, aluVs));
  s->zpdOp = ev.op;
  s->zpdId = ev.id;
  getDrawCommandQueue().commit();
#if SR_ORACLE
  if (defer) sr::gfx::nativeExecKick();
#endif
}

void RecordEngine::applyBinReg(uint32_t which, uint32_t value) {
  switch (which & 3u) {
    case 0:  // PM4_SET_BIN_MASK_LO
      binMask = (binMask & 0xFFFFFFFF00000000ull) | value;
      break;
    case 1:  // PM4_SET_BIN_MASK_HI
      binMask = (binMask & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      break;
    case 2:  // PM4_SET_BIN_SELECT_LO
      binSelect = (binSelect & 0xFFFFFFFF00000000ull) | value;
      break;
    default:  // PM4_SET_BIN_SELECT_HI
      binSelect = (binSelect & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      break;
  }
}

void RecordEngine::writeReg(uint32_t reg, uint32_t value) {
  getXenosRegisters().write(reg, value);
  regWrites++;
  if (reg == 0x0A30u) {  // COHER_BASE_HOST: a coherency request (its COHER_STATUS / SIZE written just before)
    const uint32_t size = getXenosRegisters().read(0x0A2Fu);
    cpCoher++;
    if (getXenosRegisters().read(0x0A31u) == 0x01000000u) cpCoherWhole++;
    if (size && uint64_t(value) + size >= 0x100000000ull) cpCoherTop++;
    if (value == 0xFFFFF000u && size == 0x1000u) cpCoherPage++;
    // mem.resources step 2a (SR_MEM_RESOURCES=census): the guest's boundary, noted for the registry in stream order
    // (before the draws that follow it); the register writes and the handshake are unchanged. No-op otherwise.
    if (sr::gfx::memres::census())
      sr::gfx::memres::noteCoher(getXenosRegisters().read(0x0A31u), value, size);
    if (sr::gfx::memres::resourceMode())
      sr::gfx::memres::resourceMemory().noteCoherency(getXenosRegisters().read(0x0A31u), value, size);
  }
  if (cp.trace && (reg == 0x1DCu || reg == 0x1DDu)) {
    cp.trace(cp.user, 2u, reg, value);  // P2 trace: SCRATCH_UMSK / ADDR
#if SR_ORACLE  // the SCRATCH_CFG observe line describes the segment part being applied (kernel/native_flush.hpp): dev-only
    // Observe-only (the segh UMSK clobber, 2026-10-10): after the device-create pair every write is a suspect -
    // log where the CP was (the segment part, the window around the packet; for a gap, the guest memory at that
    // address now and the gap's first dwords as copied at the close).
    static std::atomic<uint32_t> s_cfgN{0};
    const uint32_t k = s_cfgN.fetch_add(1, std::memory_order_relaxed);
    if (k >= 2u && k < 10u) {
      char part[256] = "no segment part";
      uint32_t gva = 0, glen = 0;
      const uint8_t* gcopy = nullptr;
      const bool inSeg = sr::native::flush::cpSegPartDescribe(part, sizeof part, &gva, &gcopy, &glen);
      char line[1200];
      int o = std::snprintf(line, sizeof line, "[SCRATCH_CFG] #%u reg %03X = %08X | %s | window depth=%d bytes=%zu packet@%zu:",
                            k, reg, value, part, stallDepth, stallBytes, stallOff);
      if (stallHost && (!inSeg || gcopy)) {
        const size_t a = stallOff >= 64u ? stallOff - 64u : 0u, b = std::min(stallBytes, stallOff + 64u);
        for (size_t i = a; i + 4u <= b && o > 0 && size_t(o) < sizeof line - 12u; i += 4u)
          o += std::snprintf(line + o, sizeof line - size_t(o), "%s%08X", i == stallOff ? " *" : " ", readBE32(stallHost + i));
      }
      SR_LOG_BOOT(sr::LogSeverity::Warn, "%s", line);
      if (gcopy && stallHost == gcopy && stallOff < glen) {
        const uint32_t va = gva + uint32_t(stallOff);
        o = std::snprintf(line, sizeof line, "[SCRATCH_CFG] #%u the packet is gap+0x%zX = va %08X; guest memory there now:", k,
                          stallOff, va);
        for (uint32_t i = 0; i < 16u && o > 0 && size_t(o) < sizeof line - 12u; i++) {
          const uint8_t* h = sr::kernel::guestVAToHost(va - 32u + 4u * i);
          o += std::snprintf(line + o, sizeof line - size_t(o), "%s%08X", i == 8u ? " *" : " ", h ? readBE32(h) : 0xDEADDEADu);
        }
        o += std::snprintf(line + o, sizeof line - size_t(o), " | the gap's first dwords (copy):");
        for (uint32_t i = 0; i < 16u && 4u * i + 4u <= glen && o > 0 && size_t(o) < sizeof line - 12u; i++)
          o += std::snprintf(line + o, sizeof line - size_t(o), " %08X", readBE32(gcopy + 4u * i));
        SR_LOG_BOOT(sr::LogSeverity::Warn, "%s", line);
      }
    }
#endif  // SR_ORACLE
  }
  // CP scratch registers (SCRATCH_REG0..7 = 0x578..0x57F) write back to memory
  // at SCRATCH_ADDR (0x1DD) + 4n when SCRATCH_UMSK (0x1DC) bit n is set (xenia
  // command_processor.cc:545). The D3D runtime's interrupt handshake lives on
  // this: the kick block writes the ISR continuation {cb, arg} to SCRATCH_REG4/5
  // and the pending CPU mask to SCRATCH_REG0, all landing in the pump context
  // the guest ISR reads (GpuInterruptCallback_sub_820FDBA0).
  if (cpMode && reg >= 0x578u && reg <= 0x57Fu) {
    const uint32_t n = reg - 0x578u;
    if ((1u << n) & getXenosRegisters().read(0x1DCu)) {
      const uint32_t addr = getXenosRegisters().read(0x1DDu) + n * 4u;
      // display.registers (kernel/native_vblank.hpp): SCRATCH_REG1 lands on the vblank gate's pc+4 - the GATE's
      // rearm. The write-back is bracketed (Begin before the store is performed or queued, End after; a queued store
      // is bracketed again where the executor performs it), so the per-vblank check that sees a rearmed pc+4 can
      // tell it from a foreign write - only a change to this write-back's own value at its address is explained (host
      // counters only; no guest effect; native/dreg-hang).
      if (n == 1u) sr::native::vblank::noteScratch1Begin(addr, value);
      // NATIVE strand A executor: the write-back is a memory store in stream order (behind the
      // fences ahead of it), performed by the executor (execPerformStores).
#if SR_ORACLE  // the SR_NATIVE_EXEC executor (dev-only since stage 0): the store deferred to its thread
      const uint32_t q[4] = {sr::native::exec::kOpScratch + n, addr, value, 0u};
      if (sr::native::exec::deferring() && !sr::native::exec::frontend()) {
        sr::gfx::nativeExecPushStores(q, 1);
        if (cp.trace) cp.trace(cp.user, 4u, addr, value);  // P2 trace: deferred to the executor
      } else
#endif
      if (uint8_t* p = physToHost(user, addr)) {
        p[0] = uint8_t(value >> 24); p[1] = uint8_t(value >> 16);
        p[2] = uint8_t(value >> 8);  p[3] = uint8_t(value);
        cpScratchWrites++;
        if (cp.trace) cp.trace(cp.user, 1u, addr, value);  // iter-467 native-a P2 trace
      } else if (cp.trace) {
        cp.trace(cp.user, 3u, addr, value);  // P2 trace: the write-back could not be performed (unmapped SCRATCH_ADDR)
      }
      if (n == 1u) sr::native::vblank::noteScratch1End();
    }
  }
  // The display gamma ramp (DC_LUT): table/PWL writes and the RW_INDEX
  // auto-increment are register side effects (xenia command_processor.cc:562).
  if (reg >= sr::gfx::gamma::kRegRwMode && reg <= sr::gfx::gamma::kRegWriteEnMask) {
    struct Regs {
      uint32_t read(uint32_t r) { return getXenosRegisters().read(r); }
      void write(uint32_t r, uint32_t v) { getXenosRegisters().write(r, v); }
    } regs;
    sr::gfx::gamma::displayRamp().onRegisterWrite(reg, value, regs);
  }
  // iter-416: the FMV frame's movie draws bind texture fetch slots t1 and t2 but
  // NEVER t0 — and a YUV movie shader samples three planes (luma + two chroma).
  // Fetch slot 0 arrives entirely zero, so before blaming the shader or the
  // draw, establish whether the guest ever writes that register range at all:
  // 0x4800..0x4805 is fetch constant 0 (6 dwords per slot, 32 slots).
  if (reg >= 0x4800u && reg <= 0x4805u) {
    static std::atomic<uint64_t> s_f0{0};
    const uint64_t n = s_f0.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 12 || n % 500 == 0 || sr::kernel::logVerbose())
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[FETCH0_WRITE] #%llu reg=0x%04X value=%08X",
                  (unsigned long long)n, reg, value);
  }
  // SR_PM4_REGLOG=<hex reg>: every write to that register with the draw
  // ordinal it precedes (first 400).
  {
    const uint32_t s_watch = regLogWatch();
    static std::atomic<uint32_t> s_wn{0};
    if (s_watch && reg == s_watch && s_wn.fetch_add(1, std::memory_order_relaxed) < 400)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_REGLOG] reg=0x%04X value=%08X beforeDraw=%llu", reg,
                  value, (unsigned long long)(draws + 1));
  }
  if (reg == 0x2319 || reg == 0x319) {
    copyDestBase = (value == 0xFFFFFFFFu) ? 0u : value;
    if (copyDestBase) lastNonZeroCopyDestBase = copyDestBase;
  } else if (reg == 0x231A || reg == 0x31A) {
    copyDestPitch = value;
  } else if (reg == 0x231B || reg == 0x31B) {
    copyDestInfo = value;
  }
}

void RecordEngine::onShaderLoad(uint32_t shaderType, uint32_t addr,
                             const uint8_t* ucodeHost, uint32_t sizeDwords, bool inlineUcode) {
  if (!ucodeHost || sizeDwords == 0) return;
  // The live trace records what IM_LOAD reads from memory. IM_LOAD_IMMEDIATE's ucode is in the packet itself: no
  // read (p2.zpd zpdx2: recorded as a read of physical 0, it overwrote the packets the trace placed there).
  if (cpMode && !inlineUcode) sr::gfx::livetrace::memRead(addr, ucodeHost, sizeDwords * 4u);
  // iter-369e: PLAUSIBILITY GATE. The old bound (0x10000 dwords = 256KB) is far
  // larger than any real Xenos shader — the menu shaders in the golden trace are
  // 27/99/123 dwords — so a desynced parser could hand this decoder arbitrary
  // guest memory and have it accepted. That is exactly what happened live:
  // "shaders" of 15KB..242KB whose bytes spell "!This program cannot be run in
  // DOS mode" and "rBreath_Hover" (guest PE data and string tables).
  // 4096 dwords (16KB) is still ~30x the largest real shader here.
  constexpr uint32_t kMaxShaderDwords = 4096;
  if (sizeDwords > kMaxShaderDwords) {
    badShaderLoads++;
    static std::atomic<uint32_t> s_badLog{0};
    const uint32_t bn = s_badLog.fetch_add(1, std::memory_order_relaxed);
    if (bn < 8 || (bn & (bn - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[PM4_REJECT] implausible IM_LOAD: %s addr=0x%08X %u dwords "
                  "(cap %u) — parser is off the command buffer; total=%llu",
                  shaderType == 0 ? "VERTEX" : "PIXEL", addr, sizeDwords,
                  kMaxShaderDwords, (unsigned long long)badShaderLoads);
    return;
  }
  const bool isVertex = (shaderType == 0);
  auto& slot = isVertex ? vertexUcode : pixelUcode;
  // iter-466 cp round 2 (SR_CP_SHLOAD_MEMO, see pm4_engine.hpp ShLoadMemo): a ucode already
  // decoded skips the swap, the control-flow check, the census walk and the bindings parse.
  // Not before the first-loads logs below are spent (they print the swapped dwords).
  static const bool s_shMemo = cpsimd::gateLogged("SR_CP_SHLOAD_MEMO", true);
  const ShLoadMemo* mm = nullptr;
  uint64_t memoKey = 0;
  if (s_shMemo && (isVertex ? vsLogN_ : psLogN_) >= 4 && (!isVertex || vfetchVerboseN_ >= kVfetchVerboseCap)) {
    memoKey = ShaderCache::hashUcode(ucodeHost, size_t(sizeDwords) * 4) ^ (isVertex ? 0ull : kShLoadPsSalt);
    auto it = shLoadMemo.find(memoKey);
    if (it != shLoadMemo.end() && it->second.dwords == sizeDwords) mm = &it->second;
  }
  if (!mm) {
  slot.assign(sizeDwords, 0);
  for (uint32_t i = 0; i < sizeDwords; i++) slot[i] = readBE32(ucodeHost + i * 4);

  // iter-370 STRUCTURE GATE. The size cap above only rejects the absurd; it
  // cannot tell a 108-byte shader from 108 bytes of something else. Require the
  // payload to actually decode as Xenos control flow (xenos_ucode.hpp
  // hasValidControlFlow) before it is allowed to become the bound shader.
  //
  // What this catches, measured live: five "shaders" the cache kept missing
  // turned out not to be shaders at all — one was a verbatim copy of the ring's
  // idle-frame fence packets (PM4 headers C0003B00/C0025800 and a ring address
  // 0x8A7EB446 sitting in the "ucode"), the rest were runs of 0x00000000 or
  // 0x000005FF. Accepting them repointed lastVs/PsUcodeHash, so every draw that
  // followed carried the WRONG shader identity into the cache lookup.
  //
  // The golden-trace replay is the completeness gate: it must keep vsLoads=512
  // psLoads=263, since rejecting a real shader would drop those immediately.
  if (!ucode::hasValidControlFlow(slot.data(), slot.size())) {
    bogusShaderLoads++;
    const uint32_t d0 = sizeDwords > 0 ? slot[0] : 0;
    const uint32_t d1 = sizeDwords > 1 ? slot[1] : 0;
    const uint32_t d2 = sizeDwords > 2 ? slot[2] : 0;
    const uint32_t d3 = sizeDwords > 3 ? slot[3] : 0;
    slot.clear();  // do not leave garbage bound as the current shader
    static std::atomic<uint32_t> s_bogusLog{0};
    const uint32_t gn = s_bogusLog.fetch_add(1, std::memory_order_relaxed);
    if (gn < 8 || (gn & (gn - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[PM4_REJECT] IM_LOAD payload is not Xenos ucode: %s "
                  "addr=0x%08X %u dwords | [0..3]=%08X %08X %08X %08X; total=%llu",
                  isVertex ? "VERTEX" : "PIXEL", addr, sizeDwords,
                  d0, d1, d2, d3, (unsigned long long)bogusShaderLoads);
    return;
  }
  if (s_shMemo && !isVertex && shLoadMemo.size() < 16384)
    shLoadMemo[ShaderCache::hashUcode(ucodeHost, size_t(sizeDwords) * 4) ^ kShLoadPsSalt].dwords = sizeDwords;
  }  // !mm
  if (isVertex) vertexShaderLoads++; else pixelShaderLoads++;
  // iter-404d: the ADDRESS of every accepted load. Paired with [FXSH_UCODE] on
  // the bind side this answers "is the program the movie binds ever actually
  // emitted?" as a set comparison. The existing census only reports FIRST
  // sighting per hash, which cannot distinguish "never emitted" from "emitted
  // once long ago".
  // C1 stage 1 (docs/RELEASE_COMPOSITION.md 9.9): an observe-only development census (the [IMLOAD_ID] marker of the
  // manifest's PM4 walker rule) - compiled out under SR_ORACLE=0; nothing else reads its counter.
#if SR_ORACLE
  {
    static std::atomic<unsigned> s_la{0};
    unsigned la = s_la.fetch_add(1, std::memory_order_relaxed) + 1;
    // iter-404e: addr is ALWAYS 0 here - these are IM_LOAD_IMMEDIATE packets
    // with the ucode inline in the command stream, so there is no address to
    // compare and the address-set plan was unworkable. The ucode HASH is the
    // identity, and it is already computed a few lines below for the cache
    // lookup; log it per load (not just on first sighting like [SHADER_SEEN])
    // so "which program is being emitted DURING the movie" is answerable over
    // time rather than once at boot.
    if (la <= 24 || (la % 250) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[IMLOAD_ID] #%u %s %016llX %u dwords",
                  la, isVertex ? "VS" : "PS",
                  (unsigned long long)ShaderCache::hashUcode(
                      ucodeHost, size_t(sizeDwords) * 4),
                  sizeDwords);
  }
#endif

  // iter-359c: translated-shader cache key — XXH3-64 of the RAW BE byte stream
  // (identical to the offline tool's hash of the container's ucode section).
  {
    auto& cache = getShaderCache();
    // render/boot-attach: no ensureLoaded here - it would wait for the translator stage on every
    // first IM_LOAD. findVs / findPs below wait for exactly the stage their answer needs (WP2,
    // docs/NATIVE_ROADMAP.md "Boot attach"); the live CP has already waited for the installed
    // stage before its first command (WP1).
    const uint64_t h = ShaderCache::hashUcode(ucodeHost, size_t(sizeDwords) * 4);
    const size_t ucodeBytes = size_t(sizeDwords) * 4;
    {  // the constant-use walk, once per ucode hash (NATIVE N1; SR_PM4_CONST_USED)
      static std::unordered_map<uint64_t, uint32_t> s_use;
      auto it = s_use.find(h);
      uint32_t use;
      if (it != s_use.end()) {
        use = it->second;
      } else {
        uint32_t mask = 0;
        bool rel = false;
        if (mm) {  // SR_CP_SHLOAD_MEMO skipped the swap (the use map is full)
          slot.assign(sizeDwords, 0);
          for (uint32_t i = 0; i < sizeDwords; i++) slot[i] = readBE32(ucodeHost + i * 4);
        }
        const bool ok = ucode::constantBlocksRead(slot.data(), slot.size(), &mask, &rel);
        use = ok ? (mask | (rel ? 0x10000u : 0u)) : 0x1FFFFu;
        if (s_use.size() < 65536) s_use.emplace(h, use);
        if (s_use.size() <= 64)
          SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_CUSE] %s %016llX %u dwords blocks=%04X rel=%d ok=%d",
                      isVertex ? "VS" : "PS", (unsigned long long)h, sizeDwords, mask, int(rel), int(ok));
      }
      if (isVertex) {
        lastVsConstUse = use;
        lastVsLoadAddr = addr;
        lastVsLoadDwords = sizeDwords;
      } else {
        lastPsConstUse = use;
      }
    }
    if (isVertex) {
      lastVsUcodeHash = h;
#if SR_ORACLE
      // mem.resources memres5 (SR_MEM_RESOURCES=census): the VS's vertex fetches analysed once per hash (which slots its
      // draws' indices bound: the census's index-bounded fetched bytes)
      if (sr::gfx::memres::census() || sr::gfx::memres::resourceConsumerEnabled())
        sr::gfx::memres::noteVsUcode(h, ucodeHost, ucodeBytes);
#endif
      if (cache.findVs(h)) {
        xlatVsHits++;
      } else {
        xlatMisses++;
        dumpMissedShader(h, true, ucodeHost, ucodeBytes);
        // iter-463: the [SHADER_MISS] report's twin lookup, and runtime translation
        // (SR_SHADER_RUNTIME_XLAT; both copy the bytes - ucodeHost is guest/ring memory).
        cache.noteVsMissUcode(h, ucodeHost, ucodeBytes);
        cache.requestTranslation(h, true, ucodeHost, ucodeBytes);
      }
    } else {
      lastPsUcodeHash = h;
      if (cache.findPs(h)) {
        xlatPsHits++;
        // step 4 lane C round 4 (gfx/alpha_test.hpp use(), step (b)): a cached PS that writes colour 0 without an
        // alpha-test variant gets one from the runtime translator now (the miss policy waits), before its first
        // alpha-tested draw; a no-op for every PS that has one (all but six of the shipped cache).
        cache.requestAlphaTestVariant(h, ucodeHost, ucodeBytes);
      } else {
        xlatMisses++;
        dumpMissedShader(h, false, ucodeHost, ucodeBytes);
        cache.requestTranslation(h, false, ucodeHost, ucodeBytes);  // iter-463
      }
    }
    // iter-397q: first-seen census of every distinct loaded shader hash — the
    // question "does the movie's YUV PS ever get IM_LOADed" needs an answer
    // that survives log caps. Bounded set, logs only on first sighting.
    // (A memoised ucode went through here when it was first decoded.)
    if (!mm) {
      static std::mutex s_m;
      static std::vector<uint64_t> s_seen;
      std::lock_guard<std::mutex> lk(s_m);
      bool found = false;
      for (uint64_t x : s_seen)
        if (x == h) { found = true; break; }
      if (!found && s_seen.size() < 256) {
        s_seen.push_back(h);
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[SHADER_SEEN] #%zu %s %016llX %u dwords cached=%d",
                    s_seen.size(), isVertex ? "VS" : "PS",
                    (unsigned long long)h, sizeDwords,
                    isVertex ? (cache.findVs(h) != nullptr)
                             : (cache.findPs(h) != nullptr));
      }
    }
  }
  uint32_t& n = isVertex ? vsLogN_ : psLogN_;
  if (n++ < 4) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[GPU:REPLAY] IM_LOAD %s shader: addr=0x%08X %u dwords | ucode[0..3]="
      "%08X %08X %08X %08X",
      isVertex ? "VERTEX" : "PIXEL", addr, sizeDwords,
      sizeDwords > 0 ? slot[0] : 0, sizeDwords > 1 ? slot[1] : 0,
      sizeDwords > 2 ? slot[2] : 0, sizeDwords > 3 ? slot[3] : 0);
  }

  // ── [VFETCH] vertex-format decode ─────────────────────────────────────────
  // The vertex format is NOT in the fetch constant (type/address/size/endian
  // only) — it lives in the vertex shader's vfetch instructions.
  if (isVertex && mm) {
    // The parse's results and counters, from the memo (the verbose census is spent).
    if (mm->bindOk) {
      vfetchOk++;
      if (lastVsBindingsHash != memoKey) {
        lastVsBindings = mm->bindings;
        lastVsBindingsHash = memoKey;
      }
    } else {
      vfetchFail++;
    }
    vfetchBindings += uint32_t(mm->bindings.size());
    vfetchAttrs += uint32_t(mm->attrFormats.size());
    for (const uint8_t f : mm->attrFormats)
      if (f < 64) vfetchFormatHist[f]++;
  }
  if (isVertex && !mm) {
    std::vector<ucode::VertexBinding> bindings;
    const bool ok = ucode::parseVertexShaderBindings(slot.data(), slot.size(),
                                                     bindings);
    if (ok) { vfetchOk++; lastVsBindings = bindings; lastVsBindingsHash = 0; } else vfetchFail++;
    if (s_shMemo && shLoadMemo.size() < 16384) {
      ShLoadMemo& e = shLoadMemo[ShaderCache::hashUcode(ucodeHost, size_t(sizeDwords) * 4)];  // VS: no salt
      e.dwords = sizeDwords;
      e.bindOk = ok;
      e.bindings = bindings;
      e.attrFormats.clear();
      for (const auto& bd : bindings)
        for (const auto& at : bd.attributes) e.attrFormats.push_back(uint8_t(std::min<uint32_t>(uint32_t(at.format), 255u)));
    }
    for (const auto& bd : bindings) {
      vfetchBindings++;
      for (const auto& at : bd.attributes) {
        vfetchAttrs++;
        if (uint32_t(at.format) < 64) vfetchFormatHist[uint32_t(at.format)]++;
      }
    }
    if (vfetchVerboseN_ < kVfetchVerboseCap) {
      vfetchVerboseN_++;
      if (!ok) {
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[VFETCH] vs@0x%08X %u dwords: PARSE FAILED "
                    "(ok=%u fail=%u total)",
                    addr, sizeDwords, vfetchOk, vfetchFail);
      } else {
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[VFETCH] vs@0x%08X %u dwords: %zu bindings "
                    "(ok=%u fail=%u total)",
                    addr, sizeDwords, bindings.size(), vfetchOk, vfetchFail);
        for (size_t b = 0; b < bindings.size(); b++) {
          const auto& bd = bindings[b];
          SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[VFETCH]   binding[%zu] fetchConst=%u strideWords=%u "
                      "attrs=%zu",
                      b, bd.fetchConstantIndex, bd.strideWords,
                      bd.attributes.size());
          for (size_t a = 0; a < bd.attributes.size(); a++) {
            const auto& at = bd.attributes[a];
            SR_LOG_BOOT(sr::LogSeverity::Info,
                        "[VFETCH]     attr[%zu] fmt=%s(%u) comps=%d "
                        "offsetWords=%d signed=%d integer=%d expAdj=%d",
                        a, ucode::formatName(at.format), uint32_t(at.format),
                        ucode::GetVertexFormatComponentCount(at.format),
                        at.offsetWords, at.isSigned ? 1 : 0,
                        at.isInteger ? 1 : 0, at.expAdjust);
          }
        }
      }
    }
  }
}

void RecordEngine::emitDraw(uint32_t vgtDrawInitiator, uint32_t dmaBasePhys,
                         uint32_t dmaSizeReg) {
  auto& xeRegs = getXenosRegisters();
  draws++;
  // iter-467 flicker: the per-draw ledger (SR_DRAW_LEDGER=1, CP thread only).
  const bool ledger = cpMode && sr::gfx::ledger::on();
#if SR_ORACLE  // the ledger's note names the record being applied (kernel/native_flush.hpp cpCurrentRecord): dev-only
  if (ledger)
    sr::gfx::ledger::noteDraw(xeRegs, vgtDrawInitiator, dmaBasePhys, dmaSizeReg, lastVsUcodeHash, lastPsUcodeHash,
                              binSelect, binMask, sr::native::flush::cpCurrentRecord(), physToHost, user);
#endif
  // VGT_DRAW_INITIATOR: prim_type = bits [5:0], num_indices = bits [31:16]
  // (Xenia registers.h:327). The old `& 0xFFFF` extracted the flag bits
  // (source_select/major_mode/index_size) — every count was garbage or a
  // 256 fallback. num_indices == 0 is a null draw: skip, no guessing.
  const uint32_t idxCount = vgtDrawInitiator >> 16;
  if (idxCount == 0) {
    if (ledger) sr::gfx::ledger::noteDropped(1);
    return;
  }
  sr::native::dcensus::noteDraw();  // NATIVE strand A drain census (observe-only)
  if (sr::native::dcensus::tileOn()) {
    if ((xeRegs.read(0x2208u) & 7u) == 6u) resolveDraws++;
    // A tile-predicated draw that blends: drawing it once per tile it overlaps (single-tile execution
    // of the union of the per-tile lists) would blend twice where the lists overlap.
    if (curPredicated && xeRegs.read(0x2201u) != 0x00010001u) predDrawsBlend++;
  }

  // NATIVE mode B 1a: the per-draw replay oracle (SR_REPLAY_DRAW_HASH=<file>). One line per draw:
  // the hash of the whole register file the backend builds the draw from, the shader pair and the
  // draw arguments. The native replay (SR_REPLAY_NRT=1) and the PM4 replay of the same .xtr must
  // write identical lines: same backend input per draw = same pixels per draw.
  {
    static FILE* s_dh = [] {
      const char* e = std::getenv("SR_REPLAY_DRAW_HASH");
      return (e && *e) ? std::fopen(e, "w") : static_cast<FILE*>(nullptr);
    }();
    if (s_dh) {
      const auto& xr = getXenosRegisters();
      const uint64_t h = sr::gfx::ShaderCache::hashUcode(reinterpret_cast<const uint8_t*>(xr.data()),
                                                         XenosRegisterFile::kNumRegisters * 4u);
      std::fprintf(s_dh, "%llu %016llX %016llX %016llX %08X %08X %08X\n", (unsigned long long)draws,
                   (unsigned long long)h, (unsigned long long)lastVsUcodeHash,
                   (unsigned long long)lastPsUcodeHash, vgtDrawInitiator, dmaBasePhys, dmaSizeReg);
      std::fflush(s_dh);
    }
  }

  // ── iter-456p: THE DRAW-RANGE LIMIT (SR_PM4_DRAW_LIMIT=N, default off) ─────
  // The oracle pass has to establish WHICH draws put pixels in the malformed-
  // backdrop ROI. The frame dump reads the RESOLVED framebuffer, so there is no
  // mid-frame readback to hang a per-draw probe on; replaying the same trace
  // with the draw list truncated at N and diffing the resolved frames answers
  // the same question with machinery that already exists.
  //
  // A full replay of this trace is ~3 s, so a sweep is cheap. Default OFF and
  // observer-safe: unset, `draws` is compared against 0 and nothing is skipped.
  {
    static const uint64_t s_limit = [] {
      const char* e = std::getenv("SR_PM4_DRAW_LIMIT");
      return e ? strtoull(e, nullptr, 0) : 0ull;
    }();
    if (s_limit && draws > s_limit) return;
  }

  // ── iter-456p: THE LIVE FETCH-REGISTER INVENTORY (SR_PM4_TEX_INVENTORY=1) ──
  // ⚠ NAMED PRECISELY, on the director's mandatory correction: this is a LIVE
  // FETCH-REGISTER inventory, NOT a sampled-texture inventory. It walks all 32
  // slots and logs every slot whose descriptor type is texture; it never
  // intersects that set with the ACTIVE SHADER's texture-binding list, so
  // stale-but-valid fetch constants remain in the rows. Do not describe its
  // distinct descriptors as "sampled". Xenia's parsed
  // Shader::TextureBinding::fetch_constant is the authoritative used-slot
  // filter, and applying it is a separate slice.
  // The director's oracle-first ruling requires, per draw that touches the
  // malformed-backdrop ROI, "the raw fetch descriptor, address, format,
  // dimensions, pitch, tiling/endian, and level" of every sampled texture, so
  // the set can be mapped into our HLE domain by structural fingerprint rather
  // than by assuming heap addresses match.
  //
  // Observer-only and default OFF: it reads the register file and logs. It sets
  // no state and skips no draw, so a replay with it on renders what a replay
  // with it off renders.
  //
  // ⚠ The DECODE is delegated to kernel/tex_layout.hpp's decodeFetchConstant(),
  // the one canonical decoder. The raw dwords are printed alongside it so a
  // reader can re-derive the decode instead of trusting this one.
// ── iter-456p: THE TARGET DRAW-CONTRACT ROW (SR_DRAW_CONTRACT=<ordinal>) ──
// Emits ONE row for ONE requested ordinal so an oracle draw and its structural
// counterpart in our own fixture can be diffed field by field. Observer-only
// and default OFF: it reads assembled state and formats a line.
// SR_ORACLE=0 (kernel/sr_oracle.hpp): compiled out with the HLE side of the same row (gfx/d3d12_backend.cpp), so
// SR_DRAW_CONTRACT reads as unset in the release configuration.
#if SR_ORACLE
{
  static const uint64_t s_want = [] {
    const char* e = std::getenv("SR_DRAW_CONTRACT");
    return e ? strtoull(e, nullptr, 0) : 0ull;
  }();
  if (s_want && draws == s_want) {
    DrawContract dc;
    dc.ordinal = draws;
    dc.source = DrawContractSource::PM4;
    // shader pair - the engine already tracks both ucode hashes at IM_LOAD
    dc.haveShaders = true;
    dc.vsHash = lastVsUcodeHash;
    dc.psHash = lastPsUcodeHash;

    // fixed-function, viewport/scissor and VTE, read from the register file at
    // the addresses this tree already documents in d3d12_backend.hpp.
    dc.haveFixed = true;
    dc.depthControl  = xeRegs.read(0x2200);   // RB_DEPTHCONTROL
    dc.blendControl0 = xeRegs.read(0x2201);   // RB_BLENDCONTROL0
    dc.cullMode      = xeRegs.read(0x2205);   // PA_SU_SC_MODE_CNTL
    dc.colorMask     = xeRegs.read(0x2104);   // RB_COLOR_MASK
    dc.haveVp = true;
    dc.haveScissor = true;
    dc.haveCull = true;
    dc.vte   = xeRegs.read(0x2206);           // PA_CL_VTE_CNTL
    dc.vp[0] = xeRegs.read(0x210F);           // PA_CL_VPORT_XSCALE
    dc.vp[1] = xeRegs.read(0x2110);           // PA_CL_VPORT_XOFFSET
    dc.vp[2] = xeRegs.read(0x2111);           // PA_CL_VPORT_YSCALE
    dc.vp[3] = xeRegs.read(0x2112);           // PA_CL_VPORT_YOFFSET
    dc.vp[4] = xeRegs.read(0x2113);           // PA_CL_VPORT_ZSCALE
    dc.vp[5] = xeRegs.read(0x2114);           // PA_CL_VPORT_ZOFFSET
    dc.scissorTL = xeRegs.read(0x2081);       // PA_SC_WINDOW_SCISSOR_TL
    dc.scissorBR = xeRegs.read(0x2082);       // PA_SC_WINDOW_SCISSOR_BR

    dc.havePrim = true;
    dc.primType = vgtDrawInitiator & 0x3F;
    dc.srcSelect = (vgtDrawInitiator >> 6) & 3u;
    dc.indexType = (vgtDrawInitiator >> 11) & 1u;
    dc.drawCount = idxCount;
    dc.countDomain = "idxCount(draw-initiator)";
    dc.haveSrcSelect = true;
    dc.srcSelectDomain = "source_select(draw-initiator)";
    dc.haveIndexType = true;
    dc.texDomain = DrawContract::TexDomain::RegisterResident;
    // every active vfetch the current VS actually binds
    uint32_t vi = 0;
    for (const auto& b : lastVsBindings) {
      if (vi >= 4) break;
      auto vf = xeRegs.getVertexFetch(b.fetchConstantIndex);
      if (vf.type == 0) continue;
      const uint32_t base =
          XenosRegisterFile::kFetchConstBase + b.fetchConstantIndex * 2;
      dc.vfetch[vi].present = true;
      dc.vfetch[vi].slot = b.fetchConstantIndex;
      dc.vfetch[vi].raw[0] = xeRegs.read(base);
      dc.vfetch[vi].raw[1] = xeRegs.read(base + 1);
      dc.vfetch[vi].strideWords = b.strideWords;
      dc.vfetch[vi].attrCount = (uint32_t)b.attributes.size();
      vi++;
    }
    // sampled-slot descriptors: every texture-typed slot, raw
    for (uint32_t slot = 0; slot < XenosRegisterFile::kTextureFetchSlots;
         slot++) {
      uint32_t f[6];
      xeRegs.getTextureFetchRaw(slot, f);
      if ((f[0] & 3u) != 2u) continue;
      dc.tex[slot].present = true;
      dc.tex[slot].slot = slot;
      dc.tex[slot].haveRaw = true;
      for (int i = 0; i < 6; i++) dc.tex[slot].raw[i] = f[i];
      dc.tex[slot].key = (f[1] >> 12) << 12;
      dc.texCount++;
    }
    static char s_row[16384];
    formatDrawContract(dc, s_row, sizeof(s_row));
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[DRAW_CONTRACT] BEGIN");
    SR_LOG_BOOT(sr::LogSeverity::Warn, "%s", s_row);
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[DRAW_CONTRACT] END");
  }
}
#endif
  {
    static const bool s_inv = [] {
      const char* e = std::getenv("SR_PM4_TEX_INVENTORY");
      return e && e[0] == '1';
    }();
    if (s_inv) {

      for (uint32_t slot = 0; slot < XenosRegisterFile::kTextureFetchSlots; slot++) {
        uint32_t f[6];
        xeRegs.getTextureFetchRaw(slot, f);
        if ((f[0] & 3u) != 2u) continue;          // not a texture fetch
        const sr::kernel::texlayout::FetchDesc fd =
            sr::kernel::texlayout::decodeFetchConstant(f);
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[PM4_TEXINV] draw=%llu slot=%u raw=%08X,%08X,%08X,%08X,%08X,%08X "
            "| valid=%d base=%08X fmt=%u dim=%s stacked=%d %ux%ux%u pitch=%u "
            "tiled=%d endian=%u mip=%u..%u",
            (unsigned long long)draws, slot,
            f[0], f[1], f[2], f[3], f[4], f[5],
            fd.valid ? 1 : 0, fd.basePhys, fd.fmt,
            sr::kernel::texlayout::texDimName(fd.dim), fd.stacked ? 1 : 0,
            fd.width, fd.height, fd.depth,
            fd.pitchTexels, fd.tiled ? 1 : 0, fd.endian, fd.mipMin, fd.mipMax);
      }
    }
  }

  // Pick the vfetch binding from the current VS (persisted at IM_LOAD) whose
  // fetch slot the game actually populated. Fall back to slot 0 when the VS
  // parse is unavailable (never observed on the golden trace: 512/512 parse).
  const ucode::VertexBinding* bind = nullptr;
  for (const auto& b : lastVsBindings) {
    auto vf = xeRegs.getVertexFetch(b.fetchConstantIndex);
    if (vf.type == 3 && vf.addressDwords != 0) { bind = &b; break; }
  }
  auto vf0 = bind ? xeRegs.getVertexFetch(bind->fetchConstantIndex)
                  : xeRegs.getVertexFetch(0);
  if (vf0.type == 0 || vf0.addressDwords == 0) {
    if (ledger) sr::gfx::ledger::noteDropped(2);
    return;
  }

  // iter-461: the command is built IN its ring slot (nothing is published
  // until commit() below); the head is zeroed, the constant blocks at the end
  // are copied whole from the register file. A full slot (or a non-submitting
  // instance) builds into a scratch command that is never published.
  static thread_local GuestDrawCommand s_scratch;
  GuestDrawCommand* slot = submitDraws ? getDrawCommandQueue().reserve() : nullptr;
  // NATIVE strand A executor: a full queue waits for the executor (never drops the draw).
#if SR_ORACLE
  if (!slot && submitDraws && cpMode && sr::native::exec::deferring()) slot = sr::gfx::nativeExecReserve();
#endif
  GuestDrawCommand& drawCmd = slot ? *slot : s_scratch;
  NativeVertexLayout layout;
  if (bind) {
    layout.strideBytes = bind->strideWords * 4;  // REAL stride from the VS vfetch
    // Position = the attribute at offset 0 of the binding (fall back to attr 0);
    // color = the first non-position attribute.
    const ucode::Attribute* posAttr = nullptr;
    const ucode::Attribute* colAttr = nullptr;
    for (const auto& at : bind->attributes) {
      if (at.offsetWords == 0 && !posAttr) posAttr = &at;
      else if (at.offsetWords != 0 && !colAttr) colAttr = &at;
    }
    if (!posAttr && !bind->attributes.empty()) posAttr = &bind->attributes[0];
    if (posAttr) {
      layout.posOffsetBytes = static_cast<uint32_t>(posAttr->offsetWords) * 4u;
      layout.posFormat = static_cast<uint32_t>(posAttr->format);
    }
    if (colAttr) {
      layout.colorOffsetBytes = static_cast<uint32_t>(colAttr->offsetWords) * 4u;
      layout.colorFormat = static_cast<uint32_t>(colAttr->format);
    }
  } else {
    // Legacy guess, kept only for the parse-failed path.
    uint32_t guessStride = (vf0.sizeWords > 0) ? (vf0.sizeWords * 4 / idxCount) : 44;
    if (guessStride < 12) guessStride = 12;
    if (guessStride > 256) guessStride = 44;
    layout.strideBytes = guessStride;
  }
  const NativeDrawInput input{vgtDrawInitiator, dmaBasePhys, dmaSizeReg, vf0, layout,
                              getDrawCommandQueue().committed(), uint32_t(draws),
                              lastVsUcodeHash, lastPsUcodeHash, copyDestBase, copyDestPitch, copyDestInfo};
  if (!buildNativeDrawState({xeRegs.data(), XenosRegisterFile::kNumRegisters}, input, drawCmd)) {
    nativeRecordFailed = true;
    return;
  }
  if (memres::resourceMode() && (drawCmd.modeControl & 7u) == 6u)
    drawCmd.resourceGpuGeneration = memres::resourceMemory().reserveGpu();
  // P1 EDRAM model captures (gfx/edram.hpp). High register, else low alias,
  // exactly as modeControl / copyControl above.
  {
    // Resolve rectangle (draw_util.cc:953-974): vertex fetch 0 must be a
    // vertex fetch (type 3) of exactly 6 dwords; values are GpuSwap'd by the
    // fetch's endian (0 none, 1 8in16, 2 8in32, 3 16in32).
    drawCmd.resolveVertsValid = false;
    if (((drawCmd.modeControl & 7u) == 6u)) {
      const uint32_t d0 = xeRegs.read(0x4800), d1 = xeRegs.read(0x4801);
      const uint32_t addr = d0 & ~3u, size = (d1 >> 2) & 0xFFFFFFu, endian = d1 & 3u;
      const uint8_t* vp = ((d0 & 3u) == 3u && size == 6u) ? physToHost(user, addr) : nullptr;
      if (vp) {
        if (cpMode) sr::gfx::livetrace::memRead(addr, vp, 24u);  // the resolve rect
        for (int i = 0; i < 6; i++) {
          uint32_t v;
          std::memcpy(&v, vp + 4 * i, 4);
          if (endian == 1u) v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
          else if (endian == 2u) v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
          else if (endian == 3u) v = (v >> 16) | (v << 16);
          std::memcpy(&drawCmd.resolveVerts[i], &v, 4);
        }
        drawCmd.resolveVertsValid = true;
      }
    }
  }
  {
    const uint32_t* rf = xeRegs.data();
    // SR_PM4_CONST_USED=1: copy only the 256-byte blocks the bound shaders read (the blocks
    // they do not read keep whatever the queue slot held; nothing reads them).
    static const bool s_constUsed = [] {  // default ON (=0 keeps the full copies, A/B)
      const char* e = std::getenv("SR_PM4_CONST_USED");
      return !e || e[0] != '0';
    }();
    drawCmd.vsConstUse = s_constUsed ? lastVsConstUse : 0x1FFFFu;
    drawCmd.psConstUse = s_constUsed ? lastPsConstUse : 0x1FFFFu;
    auto copyUsed = [](uint32_t* dst, const uint32_t* src, uint32_t use) {
      if ((use & 0x10000u) || (use & 0xFFFFu) == 0xFFFFu || (use & 0xFFFFu) == 0u) {
        std::memcpy(dst, src, 4096);
        return;
      }
      for (uint32_t m = use & 0xFFFFu; m;) {
        const uint32_t b = uint32_t(__builtin_ctz(m));
        std::memcpy(dst + 64u * b, src + 64u * b, 256);
        m &= m - 1u;
      }
    };
    // iter-466 cp round 2 (SR_CP_CONST_TRACK): Xenia re-uploads a float-constant buffer only when
    // a register it reads was written (d3d12 command_processor WriteRegister -> up_to_date). The
    // game re-sends most constant blocks before every draw with the SAME values (replay lg1/gp1:
    // 62% of the used blocks written between two draws, 18% changed), so a published command
    // carries only the used blocks whose values differ from what this producer last sent; the
    // drain keeps the rest (D3D12Backend::cvMerge). SR_CP_CONST_TRACK_CHECK=1 also copies every
    // used block and the drain compares its merged copy with them.
    static const bool s_ctrack = cpsimd::gateLogged("SR_CP_CONST_TRACK", true);
    static const bool s_ccheck = s_ctrack && cpsimd::gate("SR_CP_CONST_TRACK_CHECK", false);
    if (s_ctrack) cvPending |= xeRegs.takeAluDirty();
    if (s_ctrack && slot) {
      auto full = [](uint32_t u) { return ((u & 0x10000u) || (u & 0xFFFFu) == 0u) ? 0xFFFFu : (u & 0xFFFFu); };
      const uint32_t used = full(drawCmd.vsConstUse) | (full(drawCmd.psConstUse) << 16);
      if (getDrawCommandQueue().takeConstResync()) cvValid = 0;
      uint32_t need = 0;
      for (uint32_t m = used & (~cvValid | cvPending); m;) {
        const uint32_t b = uint32_t(__builtin_ctz(m));
        m &= m - 1u;
        const uint32_t* src = rf + 0x4000 + 64u * b;
        uint32_t* sh = cvShadow + 64u * b;
        if (!((cvValid >> b) & 1u) || std::memcmp(sh, src, 256) != 0) {
          std::memcpy(sh, src, 256);
          need |= 1u << b;
        }
      }
      cvValid |= used;
      cvPending &= ~used;
      // Mutation test for the check (SR_CP_CONST_TRACK_TEST_DROP=1): every 97th command drops one
      // changed block from what it sends - the CHECK must report MISMATCH lines.
      static const bool s_drop = s_ctrack && cpsimd::gate("SR_CP_CONST_TRACK_TEST_DROP", false);
      if (s_drop && need && (cvSeq % 97u) == 0u) need &= need - 1u;
      cvBlocksUsed += uint32_t(__builtin_popcount(used));
      cvBlocksSent += uint32_t(__builtin_popcount(need));
      for (uint32_t m = s_ccheck ? used : need; m;) {
        const uint32_t b = uint32_t(__builtin_ctz(m));
        m &= m - 1u;
        uint32_t* dst = b < 16u ? drawCmd.aluVs + 64u * b : drawCmd.aluPs + 64u * (b - 16u);
        std::memcpy(dst, cvShadow + 64u * b, 256);
      }
      drawCmd.constTracked = true;
      drawCmd.constPresent = need;
      drawCmd.constSeq = ++cvSeq;
    } else {
      copyUsed(drawCmd.aluVs, rf + 0x4000, drawCmd.vsConstUse);
      copyUsed(drawCmd.aluPs, rf + 0x4400, drawCmd.psConstUse);
    }
    // Keep packed shader capture after resolve-memory and float tracking, at
    // the original producer point. Fixed-state assembly does not own these arrays.
    std::memcpy(drawCmd.boolConsts, rf + 0x4900, sizeof(drawCmd.boolConsts));
    std::memcpy(drawCmd.fetchRaw, rf + 0x4800, sizeof(drawCmd.fetchRaw));
  }

  // iter-409: the return value used to be DISCARDED. push() fails when the ring
  // is full (4096), and a full ring means the guest issued draws faster than the
  // present loop drained them — those draws vanish with no trace at all. Any
  // "the game only draws N things" reading taken while this was silent was
  // measuring our ring, not the game. Counters are UNCAPPED by construction;
  // only the log line is periodic.
  static std::atomic<uint64_t> s_pushed{0}, s_dropped{0};
  // iter-441 MIGRATION STAGE-A: draw submission is a PER-INSTANCE property.
  // The replay fixture submits (offline regression harness); the LIVE instance
  // does not — the live renderer is the API-boundary HLE path (gfx/hle_video),
  // and the live PM4 engine is retained solely as the console's command
  // processor (fence writeback, interrupt counting, parse-health metrics).
  // This replaces the iter-427g/435c env-gated suppression (SR_HLE_RENDER /
  // SR_PM4_DRAW_OFF), which was coupled to the deleted kernel/hle_d3d9.cpp
  // draw producer — and which, being instance-blind, also suppressed the
  // REPLAY fixture's draws whenever those gates were at their baked defaults.
  if (!submitDraws) return;
  // NATIVE strand A executor CHECK: the draw's index + vertex-fetch ranges hashed at push.
#if SR_ORACLE
  if (slot && cpMode && sr::native::exec::checkOn() && sr::native::exec::deferring()) {
    uint32_t used[3] = {0u, 0u, 0u};
    for (const auto& b : lastVsBindings)
      if (b.fetchConstantIndex < 96u) used[b.fetchConstantIndex >> 5] |= 1u << (b.fetchConstantIndex & 31u);
    sr::native::exec::chkAtPush(drawCmd, used);
  }
#endif
  if (slot) {
    getDrawCommandQueue().commit();
#if SR_ORACLE
    if (cpMode && sr::native::exec::deferring()) sr::gfx::nativeExecKick();  // OFF windows: no wakeups
#endif
    // iter-466 cp round 2 (SR_CP_DRAIN_LEAN): a plain bump of this log counter - a locked
    // fetch_add per draw also waited for the slot's stores to drain (the emitting thread is
    // the only writer while an engine submits).
    static const bool s_lean = cpsimd::gate("SR_CP_DRAIN_LEAN", true);
    uint64_t n;
    if (s_lean) {
      n = s_pushed.load(std::memory_order_relaxed) + 1;
      s_pushed.store(n, std::memory_order_relaxed);
    } else {
      n = s_pushed.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    if (n % 2000 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[DRAW_FLOW] pushed=%llu droppedFull=%llu",
                  (unsigned long long)n,
                  (unsigned long long)s_dropped.load(std::memory_order_relaxed));
  } else {
    if (ledger) sr::gfx::ledger::noteDropped(3);
    const uint64_t d = s_dropped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (d <= 4 || d % 1000 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[DRAW_FLOW] DROPPED (queue full, cap %u) drop#%llu pushed=%llu",
                  DrawCommandQueue::kCapacity, (unsigned long long)d,
                  (unsigned long long)s_pushed.load(std::memory_order_relaxed));
  }
}

// ExecutePacketType3_WAIT_REG_MEM: stall the CP until (value & mask) matches
// ref under the wait_info compare function. This is what makes the D3D
// runtime's interrupt handshake work: the kick block (GpuEmitCommandBlock_
// sub_820FDA80) waits for its own scratch write-back, raises INTERRUPT, then
// waits until the guest ISR has cleared the pending CPU bit.
// PM4_REG_RMW (Xenia pm4_command_processor_implement.h:758-782): rmw_info bit31 = AND with
// reg[and_mask & 0x1FFF] else the immediate; bit30 = OR with reg[or_mask & 0x1FFF] else the immediate.
// The packet and the kRkRegRmw record (boot.device_create) both land here.
void RecordEngine::applyRegRmw(uint32_t rmwInfo, uint32_t andMask, uint32_t orMask) {
  const uint32_t reg = rmwInfo & 0x1FFFu;
  uint32_t value = getXenosRegisters().read(reg);
  value &= (rmwInfo & 0x80000000u) ? getXenosRegisters().read(andMask & 0x1FFFu) : andMask;
  value |= (rmwInfo & 0x40000000u) ? getXenosRegisters().read(orMask & 0x1FFFu) : orMask;
  writeReg(reg, value);
}

void RecordEngine::applyEventWriteShd(uint32_t initiator, uint32_t addrWithEndian, uint32_t value) {
  uint32_t address = addrWithEndian;
  writeReg(0x21F9u, initiator & 0x3Fu);  // VGT_EVENT_INITIATOR
  const uint32_t data = ((initiator >> 31) & 1u) && cp.counter ? cp.counter(cp.user) : value;
#if SR_ORACLE  // the SR_NATIVE_EXEC executor (dev-only since stage 0): the store in stream order on its thread
  if (sr::native::exec::deferring()) {
    // NATIVE strand A executor: the store goes in stream order; the write-back window (regs
    // 0xA04 / 0xA05) is resolved here, at its stream position.
    const uint32_t a = address & ~3u;
    uint32_t wbVa = 0;
    if (a > 0x1FFFFFFFu) {
      const uint32_t wbBase = getXenosRegisters().read(0xA04u), wbSize = getXenosRegisters().read(0xA05u);
      const uint32_t wbOff = a - wbBase;
      if (wbBase != 0 && wbOff < wbSize && cp.virtToHost) wbVa = 0x7F000000u + wbOff;
    }
    const uint32_t q[4] = {sr::native::exec::kOpEvShd, address, data, wbVa};
    sr::gfx::nativeExecPushStores(q, 1);
    return;
  }
#endif
  cpFlushTimed(cp.flush, cp.user, 3);
  // p2.zpd: a drained END not written yet - the query's covering fence may be this store (strict: write first).
  if (cp.zpdFence && sr::gfx::zpd::fenceNeedsFlush()) cp.zpdFence(cp.user);
  const uint32_t endian = address & 3u;
  address &= ~3u;
  uint8_t* dst = physToHost(user, address);
  if (address > 0x1FFFFFFFu) {
    // Writeback window: WRITEBACK_START/SIZE (0xA04/0xA05) -> VA 0x7F000000.
    const uint32_t wbBase = getXenosRegisters().read(0xA04u);
    const uint32_t wbSize = getXenosRegisters().read(0xA05u);
    const uint32_t wbOff = address - wbBase;
    if (wbBase != 0 && wbOff < wbSize && cp.virtToHost)
      dst = cp.virtToHost(cp.user, 0x7F000000u + wbOff);
  }
  if (dst) {
    const uint32_t v = gpuSwap32(data, endian);
    dst[0] = uint8_t(v); dst[1] = uint8_t(v >> 8); dst[2] = uint8_t(v >> 16); dst[3] = uint8_t(v >> 24);
    cpMemWrites++;
  }
  if (cp.trace) cp.trace(cp.user, 0u, address, data);  // iter-467 native-a P2 trace (fb+0 / fb+4)
  // iter-463 (GPU_HANG_WD): the fence word's last write and its window.
  if (sr::kernel::gpuHangWdIsFence(address))
    sr::kernel::gpuHangWdNoteFenceWrite(0x58u, address, data, dst);
}

void RecordEngine::applyCoherency(uint32_t base, uint32_t size, bool whole) {
  writeReg(0x0A31u, whole ? 0x01000000u : 0x03000000u);  // COHER_STATUS_HOST
  writeReg(0x0A2Fu, size);                               // COHER_SIZE_HOST
  writeReg(0x0A30u, base);                               // COHER_BASE_HOST
  // The block's WAIT_REG_MEM {3, 0xA31, 0, 0x80000000, 8}: the register-mode handshake exactly as the packet.
  static const uint8_t kWait[20] = {0, 0, 0, 3, 0, 0, 0x0A, 0x31, 0, 0, 0, 0, 0x80, 0, 0, 0, 0, 0, 0, 8};
  applyWaitRegMem(kWait, 5);
}

void RecordEngine::applyInterrupt(uint32_t cpuMask) {
  if (!cpMode) return;
  cpInterrupts++;
  if (!cp.interrupt) return;
  sr::native::exec::syncReason(2);
  if (!sr::native::exec::frontend()) cpFlushTimed(cp.flush, cp.user, 1);  // FRONTEND: at parse
  cp.interrupt(cp.user, cpuMask & 0x3Fu);
}

void RecordEngine::applySwap(uint32_t fbPhys, uint32_t width, uint32_t height) {
  if (!cpMode) return;
  cpSwaps++;
  // NATIVE N0.2 (gfx/pm4_coverage.hpp presented): every present the CP applies ends a coverage frame - a native swap
  // record (SR_NATIVE_SWAP_BLOCK) has no XE_SWAP packet for packet3's frame end (native/composed-census).
  if (pm4cov::on()) pm4cov::presented();
  if (sr::gfx::ledger::on()) sr::gfx::ledger::onSwap(cpSwaps);
  if (!cp.swap) return;
  if (!sr::native::exec::deferring()) cpFlushTimed(cp.flush, cp.user, 2);
  cp.swap(cp.user, fbPhys, width, height);
}

void RecordEngine::applyWaitRegMem(const uint8_t* bodyBE, uint32_t cnt) {
  if (cpMode && cnt >= 5 && bodyBE) cpWaitRegMem(bodyBE);
}

void RecordEngine::cpWaitRegMem(const uint8_t* body) {
  const uint32_t waitInfo = readBE32(body);
  const uint32_t pollAddr = readBE32(body + 4);
  const uint32_t ref = readBE32(body + 8);
  const uint32_t mask = readBE32(body + 12);
  const uint32_t wait = readBE32(body + 16);
  const bool isMem = (waitInfo & 0x10u) != 0;
  cpWaits++;
  auto sample = [&]() -> uint32_t {
    if (isMem) {
      const uint8_t* src = physToHost(user, pollAddr & ~3u);
      return src ? gpuSwap32(rawLE32(src), pollAddr & 3u) : 0u;
    }
    const uint32_t reg = pollAddr & 0xFFFFu;
    if (reg == 0x0A31u) {  // COHER_STATUS_HOST: MakeCoherent() marks it done
      if (getXenosRegisters().read(reg) & 0x80000000u) getXenosRegisters().write(reg, 0);
    }
    return getXenosRegisters().read(reg);
  };
  auto matches = [&](uint32_t v) {
    v &= mask;
    const uint32_t truth =
        (uint32_t(v < ref) << 1) | (uint32_t(v <= ref) << 2) | (uint32_t(v == ref) << 3) |
        (uint32_t(v != ref) << 4) | (uint32_t(v >= ref) << 5) | (uint32_t(v > ref) << 6) |
        (1u << 7);
    return ((truth >> (waitInfo & 7u)) & 1u) != 0;
  };
  // iter-460 live trace: the player must find the satisfied value in memory.
  auto tracePoll = [&]() {
    if (!isMem || !sr::gfx::livetrace::recording()) return;
    if (const uint8_t* src = physToHost(user, pollAddr & ~3u))
      sr::gfx::livetrace::memRead(pollAddr & ~3u, src, 4u);
  };
  // NATIVE strand A executor: a memory poll reads what the stream has stored so far - stores still
  // queued for the executor land first (a poll satisfied by a stale value must not pass).
#if SR_ORACLE  // the SR_NATIVE_EXEC executor (dev-only since stage 0)
  if (isMem && sr::gfx::nativeExecPendingEffects() &&
      sr::gfx::nativeExecPendingStoreAt(physToHost(user, pollAddr & ~3u))) {
    sr::native::exec::syncReason(1);
    cpFlushTimed(cp.flush, cp.user, 0);
  }
#endif
  uint32_t v = sample();
  if (matches(v)) {
    tracePoll();
    sr::native::dcensus::noteWait(false);
    return;
  }
  cpWaitsBlocked++;
  sr::native::dcensus::noteWait(true);
  cpFlushTimed(cp.flush, cp.user, 0);
  const auto t0 = std::chrono::steady_clock::now();
  auto nextLog = t0 + std::chrono::seconds(2);
  for (;;) {
    v = sample();
    if (matches(v)) break;
    if (cp.running && !cp.running(cp.user)) return;
    // Xenia: wait >= 0x100 sleeps wait/0x100 ms per poll (vsync cvar on).
    if (wait >= 0x100u)
      std::this_thread::sleep_for(std::chrono::milliseconds(wait / 0x100u));
    else
      std::this_thread::yield();
    const auto now = std::chrono::steady_clock::now();
    if (now >= nextLog) {
      const bool first = nextLog == t0 + std::chrono::seconds(2);
      nextLog = now + std::chrono::seconds(10);
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[CP_WAIT] stalled %.1fs: %s 0x%08X value=%08X & %08X fn=%u ref=%08X",
                  std::chrono::duration<double>(now - t0).count(),
                  isMem ? "mem" : "reg", pollAddr, v, mask, waitInfo & 7u, ref);
#if SR_ORACLE  // C1 stage 1: the development walker's window (it alone sets stallHost); the release has no walker
      if (first && stallHost) {  // the window the CP is in, with the stalled packet marked (observe-only)
        char line[640];
        int n = std::snprintf(line, sizeof line, "[CP_WAIT] window depth=%d bytes=%zu packet@%zu:", stallDepth, stallBytes, stallOff);
        const size_t dw = stallBytes / 4u < 48u ? stallBytes / 4u : 48u;
        for (size_t i = 0; i < dw && n > 0 && size_t(n) < sizeof line - 12; i++)
          n += std::snprintf(line + n, sizeof line - size_t(n), "%s%08X", i * 4u == stallOff ? " *" : " ", readBE32(stallHost + i * 4u));
        SR_LOG_BOOT(sr::LogSeverity::Warn, "%s", line);
      }
#else
      (void)first;
#endif
    }
  }
  tracePoll();
}

}  // namespace gfx
}  // namespace sr
