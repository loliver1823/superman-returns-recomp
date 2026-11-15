// iter-460: live PM4 trace writer (see pm4_live_trace.hpp). Record layouts are
// Xenia's trace_protocol.h (BSD-3-Clause, Xenia Project), the same ones
// gfx/gpu_trace_replay.cpp reads.
#include "pm4_live_trace.hpp"

#include "../app/log.hpp"
#include "../kernel/guest_context.hpp"
#include "gamma_ramp.hpp"
#include "shader_cache.hpp"
#include "xenos_state.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace sr::gfx::livetrace {
namespace {

constexpr uint32_t kXeniaRegisterCount = 0x5003;  // xenia register_file.h kRegisterCount
constexpr uint32_t kTitleId = 0x454107EDu;
constexpr uint32_t kMaxRead = 64u << 20;

struct Read {
  uint32_t phys;
  std::vector<uint8_t> data;
};
struct Packet {
  uint32_t phys = 0;
  bool hostSide = false;    // executed from a host copy (no guest address): recorded at kHostPacketPhys
  std::vector<uint8_t> bytes;
  std::vector<Read> reads;  // made while the packet executed
  uint32_t draw = 0;        // draw ordinal, 0 = not a draw
};
struct Ev {
  uint8_t kind;  // 0 packet start, 1 packet end, 2 loose read, 3 frame end (a multi-frame trace's swap event)
  uint32_t idx;
};

// The renderer thread's current draw: only ITS reads attach to that draw (the
// command processor thread reads IM_LOAD / WAIT_REG_MEM / resolve rects inside
// its own open packet at the same time).
thread_local uint32_t t_drawCtx = 0;

struct State {
  std::recursive_mutex mu;
  long target = -1;
  long first = 0;       // the open trace's first frame (its file name, tracedSwap)
  long framesLeft = 0;  // frames still to record into the open trace, this one included
  long zpdTaken = 0, zpdLastSwap = 0;  // requestOnQuery's requests so far, the swap of the last
  std::string dir;
  uint32_t swaps = 0;
  bool done = false;
  std::vector<Packet> pk;
  std::vector<Read> loose;
  std::vector<Ev> ev;
  std::vector<uint32_t> open;
  std::unordered_map<uint32_t, std::vector<Read>> byDraw;
  // (phys, bytes) -> content hash of that range's LAST record: a repeat is
  // skipped only while nothing re-recorded the range with other bytes (a
  // WAIT_REG_MEM value 4 -> 0 -> 4 must be recorded three times).
  std::unordered_map<uint64_t, uint64_t> last;
  std::vector<uint32_t> regs;
  uint32_t gammaTable[256] = {};
  uint32_t gammaPwl[128][3] = {};
  uint32_t gammaRw = 0;
  InitState init;
  InitProvider provider = nullptr;
  uint64_t readBytes = 0, readsDeduped = 0;
  std::vector<uint8_t> sidecar;  // NATIVE mode B: the .nrt records of this frame
};

State& st() {
  static State s;
  return s;
}

std::atomic<bool> g_rec{false};
std::atomic<bool> g_req{false};  // iter-462: hotkey request, consumed at the next swap

#if defined(SR_DIFFTEST)
// configureForTest: -1 = the environment decides (never set outside difftest)
std::atomic<int> g_testHotkey{-1};
std::atomic<long> g_testFrames{-1};
std::atomic<long> g_testZpd{-1};
#endif

bool hotkeyMode() {
#if defined(SR_DIFFTEST)
  const int t = g_testHotkey.load(std::memory_order_relaxed);
  if (t >= 0) return t != 0;
#endif
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_TRACE_HOTKEY");
    return e && e[0] == '1';
  }();
  return v;
}

// SR_PM4_TRACE_FRAMES: consecutive frames per trace (1..64, default 1).
long framesPerTrace() {
#if defined(SR_DIFFTEST)
  const long t = g_testFrames.load(std::memory_order_relaxed);
  if (t >= 1) return t;
#endif
  static const long v = [] {
    const char* e = std::getenv("SR_PM4_TRACE_FRAMES");
    const long n = e ? std::atol(e) : 1L;
    return n < 1 ? 1L : (n > 64 ? 64L : n);
  }();
  return v;
}

// SR_PM4_TRACE_ZPD: traces requested by occlusion queries (requestOnQuery).
long zpdTraces() {
#if defined(SR_DIFFTEST)
  const long t = g_testZpd.load(std::memory_order_relaxed);
  if (t >= 0) return t;
#endif
  static const long v = [] {
    const char* e = std::getenv("SR_PM4_TRACE_ZPD");
    return e ? std::atol(e) : 0L;
  }();
  return v;
}

bool armedNow() {
  static const long target = [] {
    const char* e = std::getenv("SR_PM4_TRACE_SWAP");
    return e ? std::atol(e) : -1L;
  }();
  return target > 0 || hotkeyMode();
}

// The physical address of a host pointer into guest memory; false for any other host memory (a host copy).
bool guestPhys(const uint8_t* host, uint32_t& phys) {
  uint8_t* base = nullptr;
  size_t size = 0;
  sr::kernel::getGlobalGuestMemory(&base, &size);
  if (!base || !host || host < base || host >= base + size) return false;
  phys = uint32_t(size_t(host - base)) & 0x1FFFFFFFu;
  return true;
}

uint32_t hostToPhys(const uint8_t* host) {
  uint32_t phys = 0;
  return guestPhys(host, phys) ? phys : 0u;
}

void put(FILE* f, const void* p, size_t n) { std::fwrite(p, 1, n, f); }
void putU32(FILE* f, uint32_t v) { put(f, &v, 4); }

void writeMemoryRead(FILE* f, const Read& r) {
  putU32(f, 6);  // kMemoryRead
  const uint32_t len = uint32_t(r.data.size());
  const uint32_t rest[4] = {r.phys, 0u /* kNone */, len, len};
  put(f, rest, sizeof(rest));
  put(f, r.data.data(), len);
}

void openTrace(State& s) {
  s.sidecar.clear();
  s.pk.clear();
  s.loose.clear();
  s.ev.clear();
  s.open.clear();
  s.byDraw.clear();
  s.last.clear();
  s.readBytes = s.readsDeduped = 0;
  auto& regs = getXenosRegisters();
  s.regs.assign(kXeniaRegisterCount, 0u);
  for (uint32_t r = 0; r < kXeniaRegisterCount; r++) s.regs[r] = regs.read(r);
  const auto& ramp = sr::gfx::gamma::displayRamp();
  std::memcpy(s.gammaTable, ramp.table, sizeof(s.gammaTable));
  std::memcpy(s.gammaPwl, ramp.pwl, sizeof(s.gammaPwl));
  s.gammaRw = ramp.rwComponent;
  s.init = InitState{};
  if (s.provider) s.provider(s.init);
  s.first = s.target;
  s.framesLeft = framesPerTrace();
  g_rec.store(true, std::memory_order_release);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PM4_LIVE_TRACE] recording the frame after swap %u (%ld frame(s); edram %zu B, %zu GPU-written ranges)",
              s.swaps, s.framesLeft, s.init.edram.size(), s.init.gpuPages.size());
}

void finishTrace(State& s) {
  g_rec.store(false, std::memory_order_release);
  s.done = true;
  CreateDirectoryA(s.dir.c_str(), nullptr);
  char name[64];
  std::snprintf(name, sizeof(name), "/live_%08X_%ld.xtr", kTitleId, s.first);
  const std::string path = s.dir + name;
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[PM4_LIVE_TRACE] cannot open %s", path.c_str());
    return;
  }
  // TraceHeader: version, 40-byte build id (not validated by the reader), title id.
  putU32(f, 1);
  char sha[40];
  std::memset(sha, ' ', sizeof(sha));
  std::memcpy(sha, "superman-recomp live pm4 trace", 30);
  put(f, sha, sizeof(sha));
  putU32(f, kTitleId);
  // InitializeTrace: registers (no callbacks), gamma ramp, EDRAM, GPU-written memory.
  putU32(f, 10);  // kRegisters
  {
    const uint32_t count = kXeniaRegisterCount;
    putU32(f, 0);
    putU32(f, count);
    const uint8_t cb[4] = {0, 0, 0, 0};
    put(f, cb, 4);
    putU32(f, 0);
    putU32(f, count * 4u);
    put(f, s.regs.data(), size_t(count) * 4u);
  }
  putU32(f, 11);  // kGammaRamp
  {
    const uint8_t rw[4] = {uint8_t(s.gammaRw), 0, 0, 0};
    put(f, rw, 4);
    putU32(f, 0);
    putU32(f, uint32_t(sizeof(s.gammaTable) + sizeof(s.gammaPwl)));
    put(f, s.gammaTable, sizeof(s.gammaTable));
    put(f, s.gammaPwl, sizeof(s.gammaPwl));
  }
  if (!s.init.edram.empty()) {
    putU32(f, 8);  // kEdramSnapshot
    putU32(f, 0);
    putU32(f, uint32_t(s.init.edram.size()));
    put(f, s.init.edram.data(), s.init.edram.size());
  }
  for (const auto& g : s.init.gpuPages) writeMemoryRead(f, Read{g.first, g.second});
  uint64_t packets = 0, draws = 0, reads = 0, hostPackets = 0, frames = 1;
  // iter-462: loose reads come FIRST. The shared-memory path validates a
  // drain's vertex pages and the render's textures AFTER the packets that use
  // them executed (collected validation, texture pre-pass), so in stream order
  // Xenia drew before the bytes arrived and sampled zeros (dark renders of our
  // gameplay traces with unmappedReads=0). Our renderer consumed exactly one
  // version of each page per frame, so front-loading them is what it drew.
  // p2.zpd zpdx3: per frame of a multi-frame trace - a frame's loose reads
  // first in THAT frame (never a later frame's version in an earlier one).
  auto looseOf = [&](size_t from) {
    for (size_t i = from; i < s.ev.size() && s.ev[i].kind != 3; i++)
      if (s.ev[i].kind == 2) {
        writeMemoryRead(f, s.loose[s.ev[i].idx]);
        reads++;
      }
  };
  looseOf(0);
  for (size_t i = 0; i < s.ev.size(); i++) {
    const Ev& e = s.ev[i];
    if (e.kind == 2) continue;
    if (e.kind == 3) {  // a frame of a multi-frame trace ends: Xenia's swap event, then the next frame's loose reads
      putU32(f, 9);     // kEvent
      putU32(f, 0);     // kSwap
      frames++;
      looseOf(i + 1);
      continue;
    }
    const Packet& p = s.pk[e.idx];
    if (e.kind == 0) {
      putU32(f, 4);  // kPacketStart
      putU32(f, p.hostSide ? kHostPacketPhys : p.phys);
      if (p.hostSide) hostPackets++;
      putU32(f, uint32_t(p.bytes.size() / 4));
      put(f, p.bytes.data(), p.bytes.size());
      for (const Read& r : p.reads) writeMemoryRead(f, r), reads++;
      if (p.draw) {
        draws++;
        auto it = s.byDraw.find(p.draw);
        if (it != s.byDraw.end())
          for (const Read& r : it->second) writeMemoryRead(f, r), reads++;
      }
      packets++;
    } else {
      putU32(f, 5);  // kPacketEnd
    }
  }
  putU32(f, 9);  // kEvent
  putU32(f, 0);  // kSwap
  const long bytes = std::ftell(f);
  std::fclose(f);
  if (!s.sidecar.empty()) {  // NATIVE mode B 1a: the native replay's records (gfx/native_device)
    if (FILE* g = std::fopen((path + ".nrt").c_str(), "wb")) {
      std::fwrite(s.sidecar.data(), 1, s.sidecar.size(), g);
      std::fclose(g);
    }
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_LIVE_TRACE] wrote %s.nrt: %zu bytes", path.c_str(), s.sidecar.size());
    s.sidecar.clear();
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PM4_LIVE_TRACE] wrote %s: %llu frame(s), %llu packets (%llu from host copies, at %08X), %llu draws, "
              "%llu reads (%.1f MB read data, %llu deduped), %.1f MB",
              path.c_str(), (unsigned long long)frames, (unsigned long long)packets, (unsigned long long)hostPackets,
              kHostPacketPhys, (unsigned long long)draws, (unsigned long long)reads,
              double(s.readBytes) / 1048576.0, (unsigned long long)s.readsDeduped, double(bytes) / 1048576.0);
  s.pk.clear();
  s.loose.clear();
  s.ev.clear();
  s.byDraw.clear();
  s.last.clear();
  s.init = InitState{};
  // SR_PM4_TRACE_EVERY=<n>: another frame n swaps later, up to 8 in one run.
  static const long s_every = [] {
    const char* e = std::getenv("SR_PM4_TRACE_EVERY");
    return e ? std::atol(e) : 0L;
  }();
  static int s_count = 0;
  if (s_every > 1 && ++s_count < 8) {
    s.target += s_every;
    s.done = false;
  }
}

}  // namespace

bool recording() { return g_rec.load(std::memory_order_acquire); }

uint32_t hostPhys(const uint8_t* host) { return hostToPhys(host); }

void sidecarAppend(const void* data, size_t bytes) {
  if (!recording()) return;
  State& s = st();
  std::lock_guard<std::recursive_mutex> lk(s.mu);
  const uint8_t* b = static_cast<const uint8_t*>(data);
  s.sidecar.insert(s.sidecar.end(), b, b + bytes);
}

void packetBegin(const uint8_t* host, uint32_t dwords) {
  if (!armedNow() || !recording()) return;
  State& s = st();
  std::lock_guard<std::recursive_mutex> lk(s.mu);
  Packet p;
  p.hostSide = !guestPhys(host, p.phys);
  p.bytes.assign(host, host + size_t(dwords) * 4u);
  s.pk.push_back(std::move(p));
  const uint32_t idx = uint32_t(s.pk.size() - 1);
  s.open.push_back(idx);
  s.ev.push_back(Ev{0, idx});
}

void packetEnd(uint32_t drawOrdinal, bool isSwap) {
  if (!armedNow()) return;
  // Outside a traced frame only swaps matter (hotkey mode is armed all run).
  if (!isSwap && !recording()) return;
  State& s = st();
  std::lock_guard<std::recursive_mutex> lk(s.mu);
  if (recording() && !s.open.empty()) {
    const uint32_t idx = s.open.back();
    s.open.pop_back();
    if (drawOrdinal) s.pk[idx].draw = drawOrdinal;
    s.ev.push_back(Ev{1, idx});
  }
  if (!isSwap || (s.done && !hotkeyMode())) return;
  if (s.target < 0) {
    const char* t = std::getenv("SR_PM4_TRACE_SWAP");
    s.target = t ? std::atol(t) : 0L;
    const char* d = std::getenv("SR_PM4_TRACE_DIR");
    s.dir = (d && *d) ? d : "archive/live_xtr";
  }
  s.swaps++;
  // iter-462 hotkey: open now, record the frame up to the next swap.
  static int s_hotkeyShots = 0;
  if (!recording() && g_req.exchange(false) && s_hotkeyShots < 32) {
    s_hotkeyShots++;
    s.target = long(s.swaps) + 1;
    s.done = false;
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_LIVE_TRACE] hotkey #%d: tracing swap %ld",
                s_hotkeyShots, s.target);
  }
  if (recording() && long(s.swaps) >= s.target) {
    if (s.framesLeft > 1) {
      // p2.zpd zpdx3, SR_PM4_TRACE_FRAMES: the next frame goes into the same trace. The swap event follows the swap
      // packet's PacketEnd, where Xenia's own writer puts it (pm4_command_processor_implement.h, PM4_XE_SWAP).
      s.framesLeft--;
      s.target++;
      s.ev.push_back(Ev{3, 0});
    } else {
      finishTrace(s);
    }
  } else if (!recording() && long(s.swaps) == s.target - 1) {
    openTrace(s);
  }
}

void memRead(uint32_t phys, const uint8_t* host, uint32_t bytes) {
  if (!recording() || !host || !bytes) return;
  State& s = st();
  std::lock_guard<std::recursive_mutex> lk(s.mu);
  if (bytes > kMaxRead) bytes = kMaxRead;
  phys &= 0x1FFFFFFFu;
  const uint64_t h = ShaderCache::hashUcode(host, bytes);
  auto [it, fresh] = s.last.try_emplace((uint64_t(phys) << 32) | bytes, h);
  if (!fresh) {
    if (it->second == h) {
      s.readsDeduped++;
      return;
    }
    it->second = h;
  }
  Read r{phys, std::vector<uint8_t>(host, host + bytes)};
  s.readBytes += bytes;
  if (t_drawCtx) {
    s.byDraw[t_drawCtx].push_back(std::move(r));
  } else if (!s.open.empty()) {
    s.pk[s.open.back()].reads.push_back(std::move(r));
  } else {
    s.loose.push_back(std::move(r));
    s.ev.push_back(Ev{2, uint32_t(s.loose.size() - 1)});
  }
}

void setDrawContext(uint32_t drawOrdinal) { t_drawCtx = drawOrdinal; }

void setInitProvider(InitProvider fn) {
  State& s = st();
  std::lock_guard<std::recursive_mutex> lk(s.mu);
  s.provider = fn;
}

bool armed() { return armedNow(); }

void requestCapture() {
  if (hotkeyMode()) g_req.store(true, std::memory_order_release);
}

void requestOnQuery() {
  const long k = zpdTraces();
  static const long s_gap = [] {
    const char* e = std::getenv("SR_PM4_TRACE_ZPD_GAP");
    const long v = e ? std::atol(e) : 0L;
    return v > 0 ? v : 600L;
  }();
  if (k <= 0 || !hotkeyMode() || recording() || g_req.load(std::memory_order_acquire)) return;
  State& s = st();
  std::lock_guard<std::recursive_mutex> lk(s.mu);
  if (s.zpdTaken >= k || (s.zpdTaken && long(s.swaps) - s.zpdLastSwap < s_gap)) return;
  s.zpdTaken++;
  s.zpdLastSwap = long(s.swaps);
  g_req.store(true, std::memory_order_release);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_LIVE_TRACE] zpd request #%ld of %ld at swap %u (an occlusion query began)",
              s.zpdTaken, k, s.swaps);
}

uint32_t tracedSwap() {
  if (!recording()) return 0;
  State& s = st();
  std::lock_guard<std::recursive_mutex> lk(s.mu);
  return s.first > 0 ? uint32_t(s.first) : 0u;
}

#if defined(SR_DIFFTEST)
void configureForTest(bool hotkey, long frames, const char* dir) {
  State& s = st();
  std::lock_guard<std::recursive_mutex> lk(s.mu);
  g_testHotkey.store(hotkey ? 1 : 0, std::memory_order_relaxed);
  g_testFrames.store(frames, std::memory_order_relaxed);
  g_testZpd.store(hotkey ? 64 : 0, std::memory_order_relaxed);
  g_rec.store(false, std::memory_order_release);
  g_req.store(false, std::memory_order_release);
  s.target = 0;  // the hotkey's idle state (not -1: packetEnd would read SR_PM4_TRACE_SWAP / _DIR over the test's)
  s.first = s.framesLeft = s.zpdTaken = s.zpdLastSwap = 0;
  s.swaps = 0;
  s.done = false;
  s.pk.clear();
  s.loose.clear();
  s.ev.clear();
  s.open.clear();
  s.byDraw.clear();
  s.last.clear();
  s.dir = dir ? dir : "";
}
#endif

}  // namespace sr::gfx::livetrace
