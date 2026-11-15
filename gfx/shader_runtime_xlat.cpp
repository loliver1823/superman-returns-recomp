// iter-463: runtime shader translation on a cache miss - see shader_runtime_xlat.hpp.
//
// Threads: onMiss / onMissDraw run on the CP thread (record_engine IM_LOAD, d3d12_backend), onContainer on
// guest threads (CreateShader hooks), onLoaded on whichever thread first loads the cache. All
// translation runs on one host worker thread that never touches guest memory. Every hook
// copies what it needs and returns; only onMiss blocks - the miss policy (render/shader-coverage, WAIT,
// default ON): the IM_LOAD that missed waits, bounded, for its translation, so no draw after it is dropped
// (dev) or fatal (release) because its shader was not translated yet.
#include "gfx/shader_runtime_xlat.hpp"
#include "gfx/shader_cache.hpp"
#include "app/log.hpp"
#include "kernel/hitch_log.hpp"  // iter-466 stutter: SR_HITCH_LOG (observe-only)
#include "kernel/sr_oracle.hpp"  // SR_XLAT_CONST_REL (step 4 lane C): a dev arm, none in the release

#include <atomic>
#include <cctype>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(SR_HAVE_XENOS_XLAT)
#include "gfx/shader_xlat_core.hpp"
#include "gfx/xenos_bridge.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define XXH_INLINE_ALL
#include "thirdparty/xxhash.h"
#endif

namespace sr {
namespace gfx {
namespace rtxlat {

namespace {
std::atomic<int> g_waitOverride{-1};
std::atomic<int64_t> g_budgetOverrideMs{-1};  // test: the wait budget (-1 = env / default)
std::atomic<uint32_t> g_jobDelayMs{0};        // test: the worker sleeps this long before each translation
// The miss policy's census (render/shader-coverage): every wait an IM_LOAD made for a translation.
std::atomic<uint64_t> g_waits{0}, g_waitAdmitted{0}, g_waitTimeouts{0}, g_waitOther{0}, g_waitUsTotal{0},
    g_waitUsMax{0};
}

bool enabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_SHADER_RUNTIME_XLAT");
    return !e || e[0] != '0';  // default ON since the replay proofs A-C + the live tutorial roll (iter-463)
  }();
  return v;
}

// Step 4 lane C: SR_XLAT_CONST_REL - the runtime translator decides each constant operand's a0 / aL indexing per
// operand (tools/xenosrecomp/0004, Xenia ucode.h src_const_is_addressed), as the offline tool that rebuilt
// data/shader_cache does. Baked ON with the regenerated cache (lane C NOTES 2026-10-18 round 2); =0 the earlier rule
// (every constant operand of an instruction indexed when either relative flag is set: every car after the first of an
// instanced draw drew with alpha 0) for a dev A/B. The release has no =0 arm. The runtime disk cache id includes it.
[[maybe_unused]] static bool constRelPerOperandOn() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_XLAT_CONST_REL");
    return !e || e[0] != '0';
  }();
  return v;
#else
  return true;
#endif
}

void setWaitModeForTest(int mode) { g_waitOverride.store(mode); }
void setWaitBudgetForTest(int64_t ms) { g_budgetOverrideMs.store(ms); }
void setJobDelayForTest(uint32_t ms) { g_jobDelayMs.store(ms); }

WaitStats waitStats() {
  WaitStats w;
  w.waits = g_waits.load(std::memory_order_relaxed);
  w.admitted = g_waitAdmitted.load(std::memory_order_relaxed);
  w.timeouts = g_waitTimeouts.load(std::memory_order_relaxed);
  w.other = g_waitOther.load(std::memory_order_relaxed);
  w.totalMs = double(g_waitUsTotal.load(std::memory_order_relaxed)) / 1e3;
  w.maxMs = double(g_waitUsMax.load(std::memory_order_relaxed)) / 1e3;
  return w;
}

#if !defined(SR_HAVE_XENOS_XLAT)
// Built without the translator (the XenosRecomp clone was missing at configure time; see
// cmake/xenos_xlat.cmake): say so once, change nothing.
namespace {
void onLoadedStub(ShaderCache&) {
  static std::atomic<bool> said{false};
  if (enabled() && !said.exchange(true))
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[SHADER_XLAT] runtime translation NOT BUILT (no tools/XenosRecomp-clone at configure "
                "time): a shader missing from the cache drops its draws for the whole run");
}
int onMissDrawStub(uint64_t, bool) { return kDropNoTranslator; }
int onMissAlphaTestDrawStub(uint64_t) { return kDropNoTranslator; }
const ShaderCacheRuntimeHooks kStubHooks{onLoadedStub, nullptr, onMissDrawStub, nullptr, nullptr, nullptr,
                                         onMissAlphaTestDrawStub};
const bool s_registered = (ShaderCache::setRuntimeHooks(&kStubHooks), true);
}  // namespace
bool waitIdle(uint32_t) { return true; }
bool stop(uint32_t) { return true; }
bool waitIdleForTest(uint32_t) { return true; }
std::string runtimeDirForTest() { return {}; }
std::string statusForTest(uint64_t, bool) { return "none"; }
std::string alphaTestStatusForTest(uint64_t) { return "none"; }

#else

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

// UTF-8 <-> path in both C++17 and C++20 (u8string() returns std::u8string in C++20).
inline std::string u8s(const std::filesystem::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}
inline std::filesystem::path u8p(const std::string& s) {
#if __cplusplus >= 202002L
  return std::filesystem::path(std::u8string(s.begin(), s.end()));
#else
  return std::filesystem::u8path(s);
#endif
}


// Bump when the admission rules in shader_xlat_core change what may be admitted.
constexpr uint32_t kRulesVersion = 1;
constexpr size_t kMaxContainers = 8192;
constexpr size_t kMaxRequests = 4096;

enum class St { Queued, Waiting, Admitted, Refused, Failed };

struct Job {
  ShaderCache* cache = nullptr;
  uint64_t hash = 0;
  bool isVs = false;
  bool alphaTest = false;  // step 4 lane C round 4: make only the alpha-test variant of a cached PS (atKey)
  std::vector<uint8_t> ucode;
  Clock::time_point requested{};
};

struct Incoming {
  std::vector<uint8_t> bytes;
  bool isVs = false;
  uint32_t n = 0;
};

struct Req {
  St st = St::Queued;
  uint64_t dropped = 0;
  bool loggedWaiting = false;
  bool waitedOut = false;  // an IM_LOAD waited the whole budget for it: later ones do not wait again
};

struct State {
  std::mutex mu;
  std::condition_variable cv;      // worker wake-up
  std::condition_variable doneCv;  // a job ended (WAIT mode, tests)
  std::deque<Job> queue;
  std::vector<Job> waiting;        // no container yet; retried when containers arrive
  std::vector<Incoming> incoming;
  std::unordered_set<uint64_t> seenContainers;  // XXH3 of each captured container
  std::unordered_map<uint64_t, Req> reqs;       // key(hash, isVs)
  uint32_t nCaptured = 0;
  bool busy = false;
  bool workerStarted = false;
  bool stopping = false;  // process exit: the worker takes no new job (drainAtExit)
  bool perProcessDir = false;  // dir is a %TEMP% shader_cache-<id>-<pid> (prune finished rolls)
  std::atomic<bool> disabled{false};
  std::atomic<bool> loaded{false};
  fs::path dir;  // <root>/xr-<id>
  std::string id;
};

State& S() {
  static State* s = new State();  // never destroyed: the worker may outlive static teardown
  return *s;
}

uint64_t key(uint64_t h, bool isVs) { return isVs ? h : h ^ 0x9E3779B97F4A7C15ull; }
// step 4 lane C round 4: a cached PS's alpha-test variant request has its own request slot.
uint64_t atKey(uint64_t h) { return h ^ 0x9E3779B97F4A7C15ull ^ 0xA17E57A17E57A17Eull; }
uint64_t jobKey(const Job& j) { return j.alphaTest ? atKey(j.hash) : key(j.hash, j.isVs); }

// The miss policy (render/shader-coverage, 2026-10-17): WAIT is the default. An IM_LOAD whose shader the
// cache lacks waits for the translation it requested, so the draws behind it - the CP is in order - find
// it. =0 restores dropping those draws until the admission (the A/B).
bool waitMode() {
  const int o = g_waitOverride.load();
  if (o >= 0) return o != 0;
  static const bool v = [] {
    const char* e = std::getenv("SR_SHADER_RUNTIME_XLAT_WAIT");
    return !e || e[0] != '0';
  }();
  return v;
}

// The bound of one wait (SR_SHADER_RUNTIME_XLAT_WAIT_MS, default 30 s). Measured translations take
// 87-1122 ms (forcing lane shader_misses.tsv, 2026-10-11), the slowest seen 17.7 s on a loaded PC; past
// the bound the IM_LOAD goes on, and a draw that still has no shader is reported (dev: dropped, release:
// a named fatal) - a translator that slow is a fault, not a pending translation.
std::chrono::milliseconds waitBudget() {
  const int64_t o = g_budgetOverrideMs.load();
  if (o >= 0) return std::chrono::milliseconds(o);
  static const int64_t v = [] {
    // Astra round 1 (P3): the whole value must be digits, 1..600000 ms; anything else is the default.
    const char* e = std::getenv("SR_SHADER_RUNTIME_XLAT_WAIT_MS");
    if (!e || !*e) return int64_t(30000);
    char* end = nullptr;
    const long long ms = std::strtoll(e, &end, 10);
    return int64_t((end && *end == 0 && ms > 0 && ms <= 600000) ? ms : 30000);
  }();
  return std::chrono::milliseconds(v);
}

// A path-valued environment variable, read as UTF-16 so a non-ASCII path survives (narrow
// getenv hands back the ANSI code page, which is not UTF-8). Empty when unset.
fs::path envPath(const char* name) {
#ifdef _WIN32
  wchar_t wname[64];
  size_t i = 0;
  for (; name[i] && i + 1 < sizeof(wname) / sizeof(wname[0]); i++) wname[i] = wchar_t(uint8_t(name[i]));
  wname[i] = 0;
  const wchar_t* v = _wgetenv(wname);
  return (v && *v) ? fs::path(v) : fs::path();
#else
  const char* v = std::getenv(name);
  return (v && *v) ? fs::path(v) : fs::path();
#endif
}

fs::path exeDir() {
#ifdef _WIN32
  wchar_t buf[1024];
  const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(sizeof(buf) / sizeof(buf[0])));
  if (n > 0 && n < sizeof(buf) / sizeof(buf[0])) return fs::path(buf).parent_path();
#endif
  std::error_code ec;
  return fs::current_path(ec);
}

// The cache root and the rule that picked it (kernel/xam_content.cpp xamContentRoot).
fs::path runtimeRoot(const char** source) {
  std::error_code ec;
  if (fs::path e = envPath("SR_SHADER_RUNTIME_DIR"); !e.empty()) {
    *source = "SR_SHADER_RUNTIME_DIR";
    return e;
  }
  if (const char* inst = std::getenv("SR_INSTANCE_ID"); inst && *inst) {
    std::string id;
    for (const char* p = inst; *p && id.size() < 32; ++p)
      id.push_back(std::isalnum(uint8_t(*p)) ? *p : '_');
    fs::path tmp = fs::temp_directory_path(ec);
    if (ec || tmp.empty()) tmp = fs::current_path(ec);
    *source = "SR_INSTANCE_ID: fresh per-process scratch";
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    return tmp / "SupermanRecomp" / ("shader_cache-" + id + "-" + std::to_string(pid));
  }
  const fs::path ed = exeDir();
  if (!ed.empty() && fs::exists(ed / "portable.txt", ec)) {
    *source = "portable.txt";
    return ed / "shader_cache";
  }
#ifdef _WIN32
  if (const wchar_t* la = _wgetenv(L"LOCALAPPDATA"); la && *la) {
    *source = "LOCALAPPDATA";
    return fs::path(la) / "SupermanRecomp" / "shader_cache";
  }
#endif
  *source = "exe dir";
  return ed / "shader_cache";
}

std::string hex(uint64_t v) {
  char b[20];
  std::snprintf(b, sizeof(b), "%016" PRIx64, v);
  return b;
}

// SR_INSTANCE_ID rolls each get a fresh %TEMP%/SupermanRecomp/shader_cache-<id>-<pid>. Remove the
// ones whose process no longer runs (a reused pid only keeps a directory one roll longer).
// Called on the worker thread before its first job, never on a game thread.
void pruneStaleProcessDirs(const fs::path& mine) {
#ifdef _WIN32
  std::error_code ec;
  const fs::path parent = mine.parent_path().parent_path();  // <tmp>/SupermanRecomp
  size_t removed = 0;
  for (fs::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = u8s(it->path().filename());
    if (n.rfind("shader_cache-", 0) != 0 || it->path() == mine.parent_path()) continue;
    const size_t dash = n.rfind('-');
    if (dash == std::string::npos || dash + 1 >= n.size()) continue;
    char* endp = nullptr;
    const unsigned long pid = std::strtoul(n.c_str() + dash + 1, &endp, 10);
    if (!endp || *endp || pid == 0) continue;
    if (HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid))) {
      DWORD code = 0;
      const bool alive = GetExitCodeProcess(hp, &code) && code == STILL_ACTIVE;
      CloseHandle(hp);
      if (alive) continue;
    } else if (GetLastError() != ERROR_INVALID_PARAMETER) {
      continue;  // exists but not ours to query: leave it
    }
    std::error_code rec;
    if (fs::remove_all(it->path(), rec) != static_cast<std::uintmax_t>(-1) && !rec) removed++;
  }
  if (removed)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SHADER_XLAT] pruned %zu per-process cache dirs of finished rolls under %s",
                removed, u8s(parent).c_str());
#else
  (void)mine;
#endif
}

// Temp name + rename, so a half-written file never has its final name. The close is checked:
// the ofstream destructor would flush the tail and ignore a failure.
bool writeAtomic(const fs::path& p, const void* data, size_t n) {
  fs::path tmp = p;
  tmp += ".tmp";
  std::error_code ec;
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    bool ok = bool(f);
    if (ok) f.write(static_cast<const char*>(data), std::streamsize(n));
    ok = ok && bool(f);
    if (f.is_open()) f.close();
    if (!ok || f.fail()) {
      fs::remove(tmp, ec);
      return false;
    }
  }
  if (fs::file_size(tmp, ec) != n || ec) {
    fs::remove(tmp, ec);
    return false;
  }
  fs::rename(tmp, p, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

// Files first, the .dxil the loader keys on LAST, and only when every other file landed: a
// crash or a failed write leaves no entry (a stale .dxil of this hash is removed first).
bool persist(const fs::path& dir, uint64_t h, const xlat::Result& r) {
  std::error_code ec;
  fs::create_directories(dir / "containers", ec);
  const std::string stem = hex(h) + (r.files.isVs ? ".vs" : ".ps");
  fs::remove(dir / (stem + ".dxil"), ec);
  if (r.files.isVs) {
    if (!writeAtomic(dir / (stem + ".vfetch"), r.files.sidecar.data(), r.files.sidecar.size())) return false;
    if (!r.files.vsf.empty() && !writeAtomic(dir / (stem + ".vsf.dxil"), r.files.vsf.data(), r.files.vsf.size()))
      return false;
    if (r.files.mkey) {
      const std::string m = hex(r.files.mkey) + "\n";
      if (!writeAtomic(dir / (stem + ".mkey"), m.data(), m.size())) return false;
    }
  } else if (!r.files.at.empty() && !writeAtomic(dir / (stem + ".at.dxil"), r.files.at.data(), r.files.at.size())) {
    return false;  // step 4 lane C (0005): the alpha-test variant
  }
  if (!writeAtomic(dir / (stem + ".xlat"), r.manifest.data(), r.manifest.size())) return false;
  if (!writeAtomic(dir / "containers" / ((r.files.isVs ? "vs_" : "ps_") + hex(h) + ".xpu"), r.container.data(),
                   r.container.size()))
    return false;
  return writeAtomic(dir / (stem + ".dxil"), r.files.dxil.data(), r.files.dxil.size());
}

// A DXIL container as DXC writes it: "DXBC", a non-zero digest (signed by dxil.dll), the total
// size field equal to the file size.
bool plausibleDxbc(const std::vector<uint8_t>& b) {
  if (b.size() < 32 || std::memcmp(b.data(), "DXBC", 4) != 0) return false;
  bool digest = false;
  for (size_t i = 4; i < 20; i++) digest |= b[i] != 0;
  uint32_t total = 0;
  std::memcpy(&total, b.data() + 24, 4);  // little-endian on every host we build
  return digest && total == b.size();
}

// A persisted entry is admitted at boot only when it is complete and well-formed; anything
// else is skipped (and retranslated on its next miss, overwriting it). Nothing is re-proved at
// boot: the directory is keyed by the translator id, which hashes every source that decides
// what is translated and admitted.
bool entryComplete(const fs::path& dir, uint64_t h, bool isVs, const CachedShader& e, std::string* why) {
  std::error_code ec;
  const std::string stem = hex(h) + (isVs ? ".vs" : ".ps");
  if (!plausibleDxbc(e.dxil)) return *why = "malformed .dxil", false;
  std::ifstream xf(dir / (stem + ".xlat"), std::ios::binary);
  const std::string xl((std::istreambuf_iterator<char>(xf)), std::istreambuf_iterator<char>());
  if (xl.empty()) return *why = "no .xlat", false;
  if (!isVs) {
    if (!e.dxilAlphaTest.empty() && !plausibleDxbc(e.dxilAlphaTest)) return *why = "malformed .ps.at.dxil", false;
    // step 4 lane C round 4 (Astra P2 2): a translation that made the variant persisted it; without the file the
    // entry is incomplete (retranslated), not a PS "without a variant".
    if (e.dxilAlphaTest.empty() && xl.find("\nat alpha-test variant") != std::string::npos)
      return *why = "the manifest names an alpha-test variant but there is no .ps.at.dxil", false;
    return true;
  }
  if (!fs::is_regular_file(dir / (stem + ".vfetch"), ec)) return *why = "no .vfetch", false;
  const bool proved = xl.find("\npath proved") != std::string::npos;
  if (proved && e.dxilFetch.empty()) return *why = "proved entry without .vsf.dxil", false;
  if (!e.dxilFetch.empty() && !plausibleDxbc(e.dxilFetch)) return *why = "malformed .vsf.dxil", false;
  return true;
}

// Pops and reads the installed cache for the stage rules (template/reference preference and the
// reference re-translation control).
xlat::InstalledLookup installedLookup(ShaderCache* cache) {
  return [cache](uint64_t h, bool isVs, xlat::InstalledFiles* out) {
    if (!isVs) return cache->findInstalledPs(h) != nullptr;
    const CachedShader* sh = cache->findInstalledVs(h);
    if (!sh) return false;
    if (out) {
      out->dxil = sh->dxil;
      out->vsf = sh->dxilFetch;
      std::ifstream f(cache->installedDir() / (hex(h) + ".vs.vfetch"), std::ios::binary);
      out->sidecar.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    return true;
  };
}

// render/shader-coverage: the shipped template corpus beside the installed cache - <installed dir>/../containers
// (data/shader_cache/containers: the compiled containers on the disc, tools/scan_disc_shaders.py). The game binds
// its .AST shaders in place, so CreateShader never hands them over; with them indexed, a bind-time variant the AOT
// cache lacks still translates at run time (proved against its template) and the waiting IM_LOAD draws it.
// SR_SHADER_CORPUS_DEFAULT=0 leaves it out (the A/B).
bool corpusDefault() {
  static const bool v = [] {
    const char* e = std::getenv("SR_SHADER_CORPUS_DEFAULT");
    return !e || e[0] != '0';
  }();
  return v;
}

void scanCorpus(xlat::ContainerIndex& index, const fs::path& installedDir) {
  std::string list = u8s(envPath("SR_SHADER_CONTAINER_DIRS"));  // scanContainerDir takes UTF-8
  std::error_code ec;
  const fs::path shipped = installedDir.empty() ? fs::path() : installedDir.parent_path() / "containers";
  const bool useShipped = corpusDefault() && !shipped.empty() && fs::is_directory(shipped, ec);
  if (useShipped) list = u8s(shipped) + (list.empty() ? "" : ";") + list;
  if (list.empty()) return;
  const auto t0 = Clock::now();
  size_t dirs = 0;
  xlat::ScanStats tot;
  for (size_t a = 0; a <= list.size();) {
    size_t b = list.find(';', a);
    if (b == std::string::npos) b = list.size();
    const std::string d = list.substr(a, b - a);
    a = b + 1;
    if (d.empty()) continue;
    const xlat::ScanStats s = xlat::scanContainerDir(d, index);
    tot.files += s.files;
    tot.added += s.added;
    tot.invalid += s.invalid;
    dirs++;
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[SHADER_XLAT] corpus (%sSR_SHADER_CONTAINER_DIRS): %zu files in %zu entries -> %zu new containers "
              "(%zu invalid); index now %zu (%zu VS) in %lld ms",
              useShipped ? "the shipped containers + " : "", tot.files, dirs, tot.added, tot.invalid, index.size(),
              index.vsCount(),
              (long long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
}

void finish(State& s, const Job& j, St st) {
  std::lock_guard<std::mutex> lk(s.mu);
  s.reqs[jobKey(j)].st = st;
  s.doneCv.notify_all();
}

// step 4 lane C round 4 (gfx/alpha_test.hpp use(), step (b)): the alpha-test variant of a PS the cache holds without
// one. The translation must reproduce the cached PS byte for byte - then the variant is that very shader plus Xenia's
// test (the provenance rule of the offline regen, lane C NOTES 2026-10-20) - else it is refused and the draws the test
// applies to are dropped (step (c)). Persisted as <h>.ps.atv.dxil + <h>.ps.atv (the manifest, written last, naming
// the plain DXIL's XXH3), admitted at the next boot onto the installed PS (onLoadedImpl).
bool persistAlphaTest(const fs::path& dir, uint64_t h, const xlat::Result& r, const std::vector<uint8_t>& plain) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string stem = hex(h) + ".ps.atv";
  fs::remove(dir / stem, ec);
  if (!writeAtomic(dir / (stem + ".dxil"), r.files.at.data(), r.files.at.size())) return false;
  const std::string m = r.manifest + "atv plain " + hex(XXH3_64bits(plain.data(), plain.size())) + "\n";
  return writeAtomic(dir / stem, m.data(), m.size());
}

void processAlphaTest(State& s, Job& j, xlat::Result& r, long long ms) {
  const CachedShader* ps = j.cache->findPs(j.hash);
  std::string why;
  St fail = St::Failed;
  if (!ps) {
    why = "the PS is not in the cache";
  } else if (r.files.at.empty()) {
    why = r.files.atSkip.empty() ? std::string("the PS writes no colour 0") : "variant unavailable: " + r.files.atSkip;
  } else if (r.files.dxil != ps->dxil) {
    why = "its plain translation differs from the cached PS (the variant would not be that shader)";
    fail = St::Refused;
  }
  if (!why.empty()) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[XLAT_ALPHA] PS %016" PRIX64 ": the runtime alpha-test variant was NOT made (%s) - its alpha-tested "
                "draws are dropped",
                j.hash, why.c_str());
    finish(s, j, fail);
    return;
  }
  const bool admitted = j.cache->admitRuntimeAlphaTest(j.hash, *ps, r.files.at);
  const long long since =
      (long long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - j.requested).count();
  finish(s, j, St::Admitted);
  fs::path dir;
  {
    std::lock_guard<std::mutex> lk(s.mu);
    dir = s.dir;
  }
  const bool saved = !dir.empty() && persistAlphaTest(dir, j.hash, r, ps->dxil);
  uint64_t dropped = 0;
  {
    std::lock_guard<std::mutex> lk(s.mu);
    dropped = s.reqs[jobKey(j)].dropped;
  }
  SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[XLAT_ALPHA] PS %016" PRIX64 ": runtime alpha-test variant admitted (%s) after %lld ms (translate %lld "
              "ms), %" PRIu64 " alpha-tested draws dropped meanwhile%s%s",
              j.hash, r.path.c_str(), since, ms, dropped, saved ? "" : "; NOT persisted",
              admitted ? "" : "; already present");
}

void process(State& s, xlat::ContainerIndex& index, xlat::Dxc& dxc, Job& j) {
  sr::hitch::Scope hitchScope(sr::hitch::kShaderXlat);
  const char* kind = j.isVs ? "VS" : j.alphaTest ? "PS alpha-test variant" : "PS";
  // What a missing translation costs: the shader's draws; a missing variant, only its alpha-tested draws.
  const char* cost =
      j.alphaTest ? "its alpha-tested draws are dropped" : "its draws have no shader (dev: dropped, release: stops)";
  if (const uint32_t d = g_jobDelayMs.load(std::memory_order_relaxed))  // test: a slow translation
    std::this_thread::sleep_for(std::chrono::milliseconds(d));
  const auto t0 = Clock::now();
  xlat::Result r = xlat::translate(j.hash, j.isVs, j.ucode.data(), j.ucode.size(), index, dxc,
                                   installedLookup(j.cache));
  const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
  if (r.status == xlat::Status::NoTemplate) {
    bool log = false;
    {
      std::lock_guard<std::mutex> lk(s.mu);
      Req& q = s.reqs[jobKey(j)];
      q.st = St::Waiting;
      log = !q.loggedWaiting;
      q.loggedWaiting = true;
      s.waiting.push_back(std::move(j));
      s.doneCv.notify_all();
    }
    if (log)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[SHADER_XLAT] WAITING %s %016" PRIX64 ": %s (index %zu containers) - %s; "
                  "retried whenever the game creates another shader",
                  kind, j.hash, r.reason.c_str(), index.size(), cost);
    return;
  }
  if (r.status != xlat::Status::Ok) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[SHADER_XLAT] %s %s %016" PRIX64 ": %s - %s", xlat::statusName(r.status), kind,
                j.hash, r.reason.c_str(), cost);
    finish(s, j, r.status == xlat::Status::Failed ? St::Failed : St::Refused);
    return;
  }
  if (j.alphaTest) {
    processAlphaTest(s, j, r, ms);
    return;
  }
  // Admit first, then wake a waiting IM_LOAD (the miss policy): the files below only serve the next start.
  CachedShader entry;
  ShaderCache::buildEntry(j.isVs, r.files.dxil, r.files.sidecar, j.isVs ? r.files.vsf : r.files.at, r.files.mkey,
                          entry);
  const bool admitted = j.cache->admitRuntime(j.hash, j.isVs, std::move(entry));
  const long long since =
      (long long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - j.requested).count();
  finish(s, j, St::Admitted);
  // Persist (best effort: an unwritable dir costs the next boot a re-translation, nothing else).
  fs::path dir;
  {
    std::lock_guard<std::mutex> lk(s.mu);
    dir = s.dir;
  }
  const bool saved = !dir.empty() && persist(dir, j.hash, r);
  uint64_t dropped = 0;
  {
    std::lock_guard<std::mutex> lk(s.mu);
    dropped = s.reqs[jobKey(j)].dropped;
  }
  SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[SHADER_XLAT] admitted %s %016" PRIX64 " (%s%s) after %lld ms (translate %lld ms), %" PRIu64
              " draws dropped meanwhile%s%s",
              kind, j.hash, r.path.c_str(), j.isVs && r.files.vsf.empty() ? ", no vsf variant" : "", since, ms,
              dropped, saved ? "" : "; NOT persisted", admitted ? "" : "; already present (installed wins)");
  // Step-4 lane C round 3 (Astra round-2 P2 on translated_samplers.cpp): our translated shaders take one sampler per
  // fetch slot from the fetch constant; a tfetch whose own filter fields override it (Xenia applies the instruction's
  // filter first) is drawn with the fetch constant's filters. No shader of the step-4 cached corpus has one
  // (J:/sr_s4render/tfcensus.json: 927 tfetches in 142 programs, all kUseFetchConst) - so say when one turns up.
  if (r.files.filterOverrides)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[SHADER_XLAT] %s %016" PRIX64 ": %u texture fetch(es) override the fetch constant's filters - "
                "ignored (one sampler per fetch slot)",
                kind, j.hash, r.files.filterOverrides);
  // The provenance (template, proof, checks), one log line each; the first two manifest lines
  // (shader, translator) are already known.
  for (size_t a = 0, n = 0; a < r.manifest.size() && n < 16; n++) {
    size_t b = r.manifest.find('\n', a);
    if (b == std::string::npos) b = r.manifest.size();
    if (n >= 2) SR_LOG_BOOT(sr::LogSeverity::Info, "[SHADER_XLAT]    %.*s", int(b - a), r.manifest.c_str() + a);
    a = b + 1;
  }
}

void workerMain() {
#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  SetThreadDescription(GetCurrentThread(), L"SR shader xlat");
#endif
  State& s = S();
  xlat::ContainerIndex index;
  xlat::Dxc dxc;
  bool corpusScanned = false;
  if (!dxc.ok())
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[SHADER_XLAT] cannot create a DXC compiler instance - every job fails");
  for (;;) {
    std::vector<Incoming> in;
    Job job;
    bool haveJob = false;
    {
      std::unique_lock<std::mutex> lk(s.mu);
      s.busy = false;
      s.doneCv.notify_all();
      s.cv.wait(lk, [&] { return s.stopping || !s.queue.empty() || !s.incoming.empty(); });
      if (s.stopping) {  // process exit (drainAtExit): park; nothing of ours is touched again
        s.doneCv.notify_all();
        s.cv.wait(lk, [] { return false; });
      }
      s.busy = true;
      in.swap(s.incoming);
      if (!s.queue.empty()) {
        job = std::move(s.queue.front());
        s.queue.pop_front();
        haveJob = true;
      }
    }
    size_t added = 0;
    for (auto& c : in) {
      xlat::Container parsed;
      std::string why;
      const std::string origin = std::string("live ") + (c.isVs ? "VS" : "PS") + " #" + std::to_string(c.n);
      if (index.size() < kMaxContainers &&
          xlat::parseContainer(c.bytes.data(), c.bytes.size(), origin, parsed, &why) &&
          index.add(std::move(parsed)))
        added++;
    }
    if (added) {
      std::lock_guard<std::mutex> lk(s.mu);
      for (auto& w : s.waiting) {  // new containers: retry every waiting request
        s.reqs[jobKey(w)].st = St::Queued;
        s.queue.push_back(std::move(w));
      }
      s.waiting.clear();
    }
    if (!haveJob) continue;
    if (!corpusScanned) {
      corpusScanned = true;
      fs::path dir;
      {
        std::lock_guard<std::mutex> lk(s.mu);
        dir = s.dir;
      }
      if (s.perProcessDir) pruneStaleProcessDirs(dir);
      try {
        scanCorpus(index, job.cache ? job.cache->installedDir() : fs::path());
      } catch (const std::exception& ex) {
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[SHADER_XLAT] corpus scan aborted: %s", ex.what());
      }
    }
    if (!dxc.ok()) {
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[SHADER_XLAT] FAILED %s %016" PRIX64 ": no DXC compiler instance",
                  job.isVs ? "VS" : "PS", job.hash);
      finish(s, job, St::Failed);
      continue;
    }
    // A C++ exception (filesystem, allocation) fails this job and keeps the game running; an
    // access violation cannot be caught here (the VEH), which is what the preflight is for.
    const uint64_t jh = job.hash;
    const bool jvs = job.isVs, jat = job.alphaTest;
    try {
      process(s, index, dxc, job);
    } catch (const std::exception& ex) {
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[SHADER_XLAT] FAILED %s %016" PRIX64 ": exception: %s - its draws have no shader (dev: dropped, release: stops)",
                  jvs ? "VS" : "PS", jh, ex.what());
      Job f;
      f.hash = jh;
      f.isVs = jvs;
      f.alphaTest = jat;
      finish(s, f, St::Failed);
    }
  }
}

bool stopLocked(State& s, std::unique_lock<std::mutex>& lk, uint32_t timeoutMs) {
  s.stopping = true;
  s.cv.notify_all();
  return s.doneCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return !s.busy; });
}

// Fallback for a CRT exit nobody prepared (rtxlat::stop not called): the ShaderCache and the
// State are never destroyed, and the log outlives this handler (atexit handlers run before the
// statics constructed ahead of their registration), so the in-flight job may end, then the
// worker parks. Bounded short: inside exit() a job was seen never to finish (see stop() in the
// header). The live game leaves through quick_exit and never runs this.
void drainAtExit() {
  State& s = S();
  std::unique_lock<std::mutex> lk(s.mu);
  if (s.stopping) return;
  stopLocked(s, lk, 3000);
}

void startWorkerLocked(State& s) {
  if (s.workerStarted) return;
  s.workerStarted = true;
  std::atexit(drainAtExit);
  std::thread(workerMain).detach();
}

// ── hooks ──
void onLoadedImpl(ShaderCache& cache);

// Runs inside ensureLoaded's scope guard (a destructor): nothing may escape it.
void onLoaded(ShaderCache& cache) {
  try {
    onLoadedImpl(cache);
  } catch (const std::exception& ex) {
    S().disabled = true;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[SHADER_XLAT] DISABLED: exception while opening the runtime cache: %s - a shader missing from "
                "the cache drops its draws",
                ex.what());
  }
}

void onLoadedImpl(ShaderCache& cache) {
  State& s = S();
  if (!enabled()) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[SHADER_XLAT] off (SR_SHADER_RUNTIME_XLAT=0): a shader missing from the cache drops its draws");
    return;
  }
  const fs::path dxcEnv = envPath("SR_DXC_DIR");
  const fs::path dxcDir = !dxcEnv.empty() ? dxcEnv : exeDir();
  std::string why;
  if (!xlat::dxcLoad(dxcDir.wstring(), &why)) {
    s.disabled = true;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[SHADER_XLAT] DISABLED: %s (%s) - a shader missing from the cache drops its draws. The build "
                "copies dxcompiler.dll + dxil.dll next to the exe (cmake/xenos_xlat.cmake)",
                why.c_str(), u8s(dxcDir).c_str());
    return;
  }
  // SR_XLAT_CONST_REL: set before the first translation (the worker starts on a miss, after this).
  const bool perOperand = constRelPerOperandOn();
  xenos::setConstRelativePerOperand(perOperand);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[SHADER_XLAT] constant operand addressing: %s (SR_XLAT_CONST_REL=%d)",
              perOperand ? "per operand (Xenia)" : "either flag indexes every constant operand (old)",
              perOperand ? 1 : 0);
  // Translator identity: the compiled sources + shader_common.h, the DXC DLL bytes, our rules.
  const std::string src = xenos::translatorSourceId();
  XXH3_state_t st;
  XXH3_64bits_reset(&st);
  XXH3_64bits_update(&st, src.data(), src.size());
  const uint64_t dll = xlat::dxcDllHash();
  XXH3_64bits_update(&st, &dll, sizeof(dll));
  XXH3_64bits_update(&st, &kRulesVersion, sizeof(kRulesVersion));
  if (perOperand) {  // a different translation rule, a different runtime directory
    const uint8_t tag = 0xC4;
    XXH3_64bits_update(&st, &tag, sizeof(tag));
  }
  const std::string id = "xr-" + hex(XXH3_64bits_digest(&st));
  const char* source = "";
  const fs::path dir = runtimeRoot(&source) / id;
  {
    std::lock_guard<std::mutex> lk(s.mu);
    s.dir = dir;
    s.id = id;
    s.perProcessDir = std::strncmp(source, "SR_INSTANCE_ID", 14) == 0;
  }
  // Admit what earlier runs translated (the installed cache wins a clash).
  const auto tScan = Clock::now();  // dcb: the persisted-dir scan's wall time (observe-only)
  size_t nOk = 0, nShadow = 0, nDiffer = 0, nBad = 0;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = u8s(it->path().filename());  // <16 hex>.<vs|ps>.dxil
    if (name.size() != 24 || (name.compare(16, 8, ".vs.dxil") != 0 && name.compare(16, 8, ".ps.dxil") != 0))
      continue;
    const bool isVs = name[17] == 'v';
    const uint64_t h = std::strtoull(name.substr(0, 16).c_str(), nullptr, 16);
    CachedShader e;
    std::string bad;
    if (!ShaderCache::loadEntryFromDir(dir, h, isVs, e) || !entryComplete(dir, h, isVs, e, &bad)) {
      nBad++;
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[SHADER_XLAT] %s %016" PRIX64 ": persisted entry skipped (%s) - retranslated on its next miss",
                  isVs ? "VS" : "PS", h, bad.empty() ? "unreadable" : bad.c_str());
      continue;
    }
    const CachedShader* inst = isVs ? cache.findInstalledVs(h) : cache.findInstalledPs(h);
    if (inst) {
      nShadow++;
      if (inst->dxil != e.dxil) {
        nDiffer++;
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[SHADER_XLAT] %s %016" PRIX64 ": the runtime translation in %s DIFFERS from the installed "
                    "cache (the installed one is used)",
                    isVs ? "VS" : "PS", h, u8s(dir).c_str());
      }
      continue;
    }
    if (cache.admitRuntime(h, isVs, std::move(e))) nOk++;
  }
  // step 4 lane C round 4: the alpha-test variants earlier runs made for installed PS (processAlphaTest). Admitted
  // only onto the PS whose plain DXIL the manifest names (XXH3), so a changed installed cache never gets a stale one.
  size_t nAtOk = 0, nAtBad = 0;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = u8s(it->path().filename());  // <16 hex>.ps.atv
    if (name.size() != 23 || name.compare(16, 7, ".ps.atv") != 0) continue;
    const uint64_t h = std::strtoull(name.substr(0, 16).c_str(), nullptr, 16);
    std::ifstream mf(it->path(), std::ios::binary);
    const std::string man((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
    std::ifstream df(dir / (name + ".dxil"), std::ios::binary);
    std::vector<uint8_t> dx((std::istreambuf_iterator<char>(df)), std::istreambuf_iterator<char>());
    const CachedShader* ps = cache.findInstalledPs(h);
    const char* bad = nullptr;
    if (!plausibleDxbc(dx)) bad = "malformed .ps.atv.dxil";
    else if (!ps) bad = "no installed PS of that hash";
    else if (man.find("atv plain " + hex(XXH3_64bits(ps->dxil.data(), ps->dxil.size())) + "\n") == std::string::npos)
      bad = "made for another plain translation";
    else if (!cache.admitRuntimeAlphaTest(h, *ps, std::move(dx))) bad = "the PS has a variant already";
    if (bad) {
      nAtBad++;
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[XLAT_ALPHA] PS %016" PRIX64 ": persisted alpha-test variant skipped (%s)", h, bad);
    } else {
      nAtOk++;
    }
  }
  if (nAtOk || nAtBad)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[XLAT_ALPHA] %zu persisted runtime alpha-test variants admitted, %zu skipped",
                nAtOk, nAtBad);
  s.loaded = true;
  {
    // dcb (boot.device_create's boot_order_gap; observe-only, log-only, a milestone tag): the runtime translator's
    // share of the boot load, by phase - each DXC LoadLibraryW and the DLL hash (wrapper wall times on this thread,
    // not disk service time) and the persisted-dir scan; the [SHADER_CACHE] boot load done line holds the total.
    const xlat::DxcLoadTiming t = xlat::dxcLoadTiming();
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[SHADER_CACHE] boot load: runtime translator - LoadLibrary dxil.dll %.1f ms, dxcompiler.dll %.1f ms, "
                "DLL hash %.1f ms (%.2f MiB), persisted-dir scan %.1f ms (%zu admitted, %zu shadowed, %zu unreadable)",
                double(t.loadDxilUs) / 1e3, double(t.loadDxcUs) / 1e3, double(t.hashUs) / 1e3,
                double(t.hashBytes) / 1048576.0,
                double(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - tScan).count()) / 1e3,
                nOk, nShadow, nBad);
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[SHADER_XLAT] on: dir %s (%s); translator %.16s + dxc %s (%016" PRIX64 "); %zu persisted "
              "translations admitted, %zu shadowed by the installed cache (%zu differ), %zu unreadable%s",
              u8s(dir).c_str(), source, src.c_str(), xlat::dxcVersion().c_str(), dll, nOk, nShadow,
              nDiffer, nBad, waitMode() ? "; the miss policy: a miss WAITS for its translation" : "; NO WAIT (SR_SHADER_RUNTIME_XLAT_WAIT=0): a miss drops its draws until the admission");
}

const char* stName(St st) {
  switch (st) {
    case St::Queued: return "still queued";
    case St::Waiting: return "no template container (WAITING)";
    case St::Admitted: return "admitted";
    case St::Refused: return "REFUSED";
    default: return "FAILED";
  }
}

// THE MISS POLICY (render/shader-coverage, 2026-10-17). Called by onMiss on the CP thread with s.mu held:
// the IM_LOAD that missed blocks until its translation leaves the queue - admitted, or answered no
// (no template, refused, failed) - or the budget passes. The CP executes in order, so every draw behind
// this IM_LOAD sees the answer: no draw is dropped (dev) or fatal (release) because its shader was still
// being translated. What it can block, and why that cannot deadlock:
//   - the CP thread holds no CP / ring / queue lock while it executes a record (kernel/gpu_submit.cpp
//     releases c.mu first); the guest waits on its own fences meanwhile, as behind a busy GPU;
//   - the worker never waits on the CP, the render thread or a guest thread: it reads its own queue, the
//     container index (guest threads only push copies), DXC and the files; admitRuntime waits only for the
//     installed stage, published before any request (onMiss needs `loaded`, set at the end of stage 2);
//   - the boot load: a request exists only after the translator stage (WP2), so a wait never overlaps it.
// For its duration the CP is in its host-wait phase (ShaderCache::hostWaitBegin -> the CP's wait observer,
// kCpHostWaitTranslation), so the hang watchdog does not read the CP's own stall as a GPU hang.
void waitForTranslation(State& s, std::unique_lock<std::mutex>& lk, uint64_t k, uint64_t hash, bool isVs,
                        bool alphaTest = false) {
  if (s.reqs[k].st != St::Queued || s.reqs[k].waitedOut) return;
  const char* kind = isVs ? "VS" : alphaTest ? "PS alpha-test variant" : "PS";
  const char* cost = alphaTest ? "its alpha-tested draws are dropped"
                               : "the draws using it go on without it (dev: dropped; release: fatal)";
  const auto t0 = Clock::now();
  const auto budget = waitBudget();
  const auto deadline = t0 + budget;
  const uint64_t n = g_waits.fetch_add(1, std::memory_order_relaxed) + 1;
  lk.unlock();
  ShaderCache::hostWaitBegin(ShaderCache::kWaitTranslation);
  lk.lock();
  bool timedOut = false, drainCut = false;
  auto deadline2 = deadline;  // shortened once the graceful quit drains the CP (Astra round 1, P2)
  auto nextNote = t0 + std::chrono::seconds(5);
  while (s.reqs[k].st == St::Queued) {
    auto until = deadline2 < nextNote ? deadline2 : nextNote;
    const auto poll = Clock::now() + std::chrono::milliseconds(250);  // the drain flag is polled, not signalled
    if (!drainCut && poll < until) until = poll;
    if (s.doneCv.wait_until(lk, until) != std::cv_status::timeout || s.reqs[k].st != St::Queued) continue;
    const auto now = Clock::now();
    if (!drainCut && ShaderCache::hostWaitDraining()) {
      // kernel/quit.cpp gives the CP drain 10 s (SR_QUIT_DRAIN_MS): the wait takes at most 5 s of it.
      drainCut = true;
      if (now + std::chrono::seconds(5) < deadline2) deadline2 = now + std::chrono::seconds(5);
      lk.unlock();
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[SHADER_WAIT] %s %016" PRIX64 ": the graceful quit drains the CP - this wait ends within 5 s",
                  kind, hash);
      lk.lock();
      continue;
    }
    if (now >= deadline2) {
      timedOut = true;
      s.reqs[k].waitedOut = true;
      break;
    }
    if (now >= nextNote) {
      nextNote += std::chrono::seconds(5);
      const size_t queued = s.queue.size();
      lk.unlock();
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[SHADER_WAIT] %s %016" PRIX64 ": still waiting for its translation after %.1f s (budget %.0f s, "
                  "%zu other jobs queued)",
                  kind, hash, std::chrono::duration<double>(now - t0).count(),
                  std::chrono::duration<double>(budget).count(), queued);
      lk.lock();
    }
  }
  const St st = s.reqs[k].st;
  lk.unlock();
  ShaderCache::hostWaitEnd(ShaderCache::kWaitTranslation);
  const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
  const uint64_t total = g_waitUsTotal.fetch_add(us, std::memory_order_relaxed) + us;
  uint64_t mx = g_waitUsMax.load(std::memory_order_relaxed);
  while (us > mx && !g_waitUsMax.compare_exchange_weak(mx, us, std::memory_order_relaxed)) {}
  (timedOut ? g_waitTimeouts : st == St::Admitted ? g_waitAdmitted : g_waitOther)
      .fetch_add(1, std::memory_order_relaxed);
  // [SHADER_WAIT] is a milestone tag (app/log.cpp): every wait is named in a player log too.
  if (timedOut)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[SHADER_WAIT] %s %016" PRIX64 ": NOT translated within the %.0f s budget%s - %s (wait #%" PRIu64
                ", %.1f ms waited in all)",
                kind, hash, std::chrono::duration<double>(budget).count(), drainCut ? " (cut short by the quit)" : "",
                cost, n, double(total) / 1e3);
  else
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[SHADER_WAIT] %s %016" PRIX64 ": %s after %.1f ms of waiting (wait #%" PRIu64
                ", %.1f ms waited in all)",
                kind, hash, stName(st), double(us) / 1e3, n, double(total) / 1e3);
  lk.lock();  // onMiss's lock is held again on return (its unique_lock unlocks it)
}

void request(ShaderCache& cache, uint64_t hash, bool isVs, bool alphaTest, const void* ucode, size_t bytes) {
  State& s = S();
  if (!enabled() || s.disabled.load(std::memory_order_relaxed) || !s.loaded.load(std::memory_order_acquire))
    return;
  const uint64_t k = alphaTest ? atKey(hash) : key(hash, isVs);
  std::unique_lock<std::mutex> lk(s.mu);
  auto it = s.reqs.find(k);
  if (it == s.reqs.end()) {
    if (s.reqs.size() >= kMaxRequests) {
      static std::atomic<bool> said{false};
      lk.unlock();
      if (!said.exchange(true))
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[SHADER_WAIT] %s %016" PRIX64 ": the request table is full (%zu) - not translated; its draws "
                    "have no shader",
                    isVs ? "VS" : "PS", hash, kMaxRequests);
      return;
    }
    s.reqs.emplace(k, Req{});
    Job j;
    j.cache = &cache;
    j.hash = hash;
    j.isVs = isVs;
    j.alphaTest = alphaTest;
    j.ucode.assign(static_cast<const uint8_t*>(ucode), static_cast<const uint8_t*>(ucode) + bytes);
    j.requested = Clock::now();
    s.queue.push_back(std::move(j));
    startWorkerLocked(s);
    s.cv.notify_one();
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SHADER_XLAT] request %s %016" PRIX64 " (%zu bytes)%s",
                isVs ? "VS" : alphaTest ? "PS alpha-test variant" : "PS", hash, bytes,
                waitMode() ? " - waiting for it" : "");
  }
  if (waitMode()) waitForTranslation(s, lk, k, hash, isVs, alphaTest);
}

void onMiss(ShaderCache& cache, uint64_t hash, bool isVs, const void* ucode, size_t bytes) {
  request(cache, hash, isVs, false, ucode, bytes);
}

// step 4 lane C round 4, step (b): the IM_LOAD of a cached PS that writes colour 0 without an alpha-test variant
// (ShaderCache::requestAlphaTestVariant). Waits under the miss policy as onMiss does, so the PS's first alpha-tested
// draw finds the variant.
void onMissAlphaTest(ShaderCache& cache, uint64_t hash, const void* ucode, size_t bytes) {
  request(cache, hash, false, true, ucode, bytes);
}

int dropState(St st) {
  switch (st) {
    case St::Queued: return kDropQueued;
    case St::Waiting: return kDropWaiting;
    case St::Refused: return kDropRefused;
    case St::Failed: return kDropFailed;
    default: return kDropQueued;  // admitted meanwhile: this draw came before the admission
  }
}

int onMissDraw(uint64_t hash, bool isVs) {
  State& s = S();
  if (!enabled()) return kDropNoTranslator;
  if (s.disabled.load(std::memory_order_relaxed)) return kDropDisabled;
  if (!s.loaded.load(std::memory_order_acquire)) return kDropNoTranslator;
  std::lock_guard<std::mutex> lk(s.mu);
  auto it = s.reqs.find(key(hash, isVs));
  if (it == s.reqs.end()) return kDropNoRequest;
  it->second.dropped++;
  // Astra round 1 (P3): a draw that missed after the IM_LOAD waited out its budget keeps that cause even when the
  // worker admits the shader between the draw's lookup and this call.
  const St st = it->second.st;
  return it->second.waitedOut && (st == St::Queued || st == St::Admitted) ? kDropWaitedOut : dropState(st);
}

int onMissAlphaTestDraw(uint64_t hash) {
  State& s = S();
  if (!enabled()) return kDropNoTranslator;
  if (s.disabled.load(std::memory_order_relaxed)) return kDropDisabled;
  if (!s.loaded.load(std::memory_order_acquire)) return kDropNoTranslator;
  std::lock_guard<std::mutex> lk(s.mu);
  auto it = s.reqs.find(atKey(hash));
  if (it == s.reqs.end()) return kDropNoRequest;
  it->second.dropped++;
  const St st = it->second.st;
  // as onMissDraw: a request whose IM_LOAD waited out its budget keeps that cause
  return it->second.waitedOut && (st == St::Queued || st == St::Admitted) ? int(kDropWaitedOut) : dropState(st);
}

bool wantContainers() { return enabled() && !S().disabled.load(std::memory_order_relaxed); }

void onContainer(const void* blob, size_t bytes, bool isVs) {
  State& s = S();
  const uint64_t h = XXH3_64bits(blob, bytes);
  std::lock_guard<std::mutex> lk(s.mu);
  if (s.seenContainers.size() >= kMaxContainers || !s.seenContainers.insert(h).second) return;
  Incoming in;
  in.bytes.assign(static_cast<const uint8_t*>(blob), static_cast<const uint8_t*>(blob) + bytes);
  in.isVs = isVs;
  in.n = ++s.nCaptured;
  s.incoming.push_back(std::move(in));
  if (s.workerStarted) s.cv.notify_one();  // else the first request starts the worker
}

const ShaderCacheRuntimeHooks kHooks{onLoaded,    onMiss,          onMissDraw,         wantContainers,
                                     onContainer, onMissAlphaTest, onMissAlphaTestDraw};
const bool s_registered = (ShaderCache::setRuntimeHooks(&kHooks), true);

}  // namespace

bool waitIdle(uint32_t timeoutMs) {
  State& s = S();
  std::unique_lock<std::mutex> lk(s.mu);
  if (!s.workerStarted) return true;
  return s.doneCv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                           [&] { return s.queue.empty() && s.incoming.empty() && !s.busy; });
}

bool stop(uint32_t timeoutMs) {
  State& s = S();
  std::unique_lock<std::mutex> lk(s.mu);
  if (!s.workerStarted) {
    s.stopping = true;
    return true;
  }
  return stopLocked(s, lk, timeoutMs);
}

bool waitIdleForTest(uint32_t timeoutMs) {
  State& s = S();
  std::unique_lock<std::mutex> lk(s.mu);
  return s.doneCv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                           [&] { return s.queue.empty() && s.incoming.empty() && !s.busy; });
}

std::string runtimeDirForTest() {
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  return u8s(s.dir);
}

namespace {
std::string statusOf(uint64_t k) {
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  auto it = s.reqs.find(k);
  if (it == s.reqs.end()) return "none";
  switch (it->second.st) {
    case St::Queued: return "queued";
    case St::Waiting: return "waiting-template";
    case St::Admitted: return "admitted";
    case St::Refused: return "refused";
    default: return "failed";
  }
}
}  // namespace

std::string statusForTest(uint64_t hash, bool isVs) { return statusOf(key(hash, isVs)); }
std::string alphaTestStatusForTest(uint64_t hash) { return statusOf(atKey(hash)); }

#endif  // SR_HAVE_XENOS_XLAT

}  // namespace rtxlat
}  // namespace gfx
}  // namespace sr
