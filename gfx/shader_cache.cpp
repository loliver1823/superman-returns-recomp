#include "shader_cache.hpp"
#include "shader_mkey.hpp"  // iter-463
#include "alpha_test.hpp"   // step 4 lane C round 4: SR_XLAT_ALPHA_TEST (requestAlphaTestVariant)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <intrin.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <tuple>
#include <unordered_set>

#define XXH_INLINE_ALL
#include "thirdparty/xxhash.h"

#include "../app/log.hpp"
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>  // dcb: mainThreadId (the boot-load lines name the process's main thread)
#endif

namespace sr {
namespace gfx {

namespace {
// iter-463: the runtime translator's hooks (constant-initialised: null until it registers).
std::atomic<const ShaderCacheRuntimeHooks*> g_rtHooks{nullptr};
// render/boot-attach: the blocking-wait observer (the CP's phase) and the stage-1 read hook (tests).
std::atomic<const ShaderCache::WaitObserver*> g_waitObs{nullptr};
std::atomic<ShaderCache::ReadHook> g_readHook{nullptr};
std::atomic<ShaderCache::PublishHook> g_publishHook{nullptr};

// dcb (boot.device_create's boot_order_gap, 2026-10-14; observe-only, always on, log-only), merged with
// render/boot-attach's staged load: the boot load (ShaderCache::runLoad - the installed cache's files, then the
// runtime translator's onLoaded: its DXC DLL load + hash and its persisted dir) runs on the "SR shader load" thread
// by default, off the attach's path (SR_SHADER_LOAD_ASYNC=0: inside D3D12Backend::init, before the attach). The
// [SHADER_CACHE] boot-load lines give a PLAYER-profile roll its split. What they measure: per-call WRAPPER wall time
// of readFile below (open + size + allocate + read + close, as the thread saw it) - not disk service time; a call
// over 5 ms is not proof of a seek or a cache miss (antivirus, queueing and scheduling look the same). Reads outside
// readFile (the DXC DLL load and hash, directory enumeration, the translator's other work) are NOT in these
// counters: the lines name that time as uncovered. NOT timing-neutral: the load pays a clock read per wrapped call.
// Per-load scoped statistics: the load's thread points t_readScope at its own counters (runLoad), so reads on any
// other thread (and any read outside the boot load) are neither counted nor timed.
struct ReadScope {
  uint64_t calls = 0, bytes = 0, wrapUs = 0, maxWrapUs = 0, over5ms = 0;
};
thread_local ReadScope* t_readScope = nullptr;
void readScopeFields(char* b, size_t n, const ReadScope& s) {
  std::snprintf(b, n, "%llu wrapped read calls, %.2f MiB returned, wrapper time %.1f ms, longest call %.1f ms, "
                "%llu calls over 5 ms",
                (unsigned long long)s.calls, double(s.bytes) / 1048576.0, double(s.wrapUs) / 1e3,
                double(s.maxWrapUs) / 1e3, (unsigned long long)s.over5ms);
}
constexpr uint64_t kOverUs = 5000;
uint64_t usSince(std::chrono::steady_clock::time_point t0) {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
}

bool readFileRaw(const std::filesystem::path& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f) return false;
  auto n = f.tellg();
  out.resize(size_t(n));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(out.data()), n);
  return f.good() || f.eof();
}
bool readFile(const std::filesystem::path& p, std::vector<uint8_t>& out) {
  ReadScope* sc = t_readScope;
  if (!sc) return readFileRaw(p, out);
  const auto t0 = std::chrono::steady_clock::now();
  const bool ok = readFileRaw(p, out);
  const uint64_t us = usSince(t0);
  sc->calls++;
  if (ok) sc->bytes += out.size();
  sc->wrapUs += us;
  if (us > sc->maxWrapUs) sc->maxWrapUs = us;
  if (us >= kOverUs) sc->over5ms++;
  return ok;
}
unsigned long threadIdNow() {
#ifdef _WIN32
  return static_cast<unsigned long>(GetCurrentThreadId());
#else
  return 0ul;
#endif
}
// The process's main thread = its earliest-created thread (the one main() runs on), so a reader can tell whether
// the boot load ran on it. 0 when it cannot be determined.
unsigned long mainThreadId() {
#ifdef _WIN32
  unsigned long best = 0;
  ULONGLONG bestT = ~0ull;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  const DWORD pid = GetCurrentProcessId();
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
    if (!h) continue;
    FILETIME c{}, e{}, k{}, u{};
    if (GetThreadTimes(h, &c, &e, &k, &u)) {
      const ULONGLONG t = (ULONGLONG(c.dwHighDateTime) << 32) | c.dwLowDateTime;
      if (t < bestT) {
        bestT = t;
        best = te.th32ThreadID;
      }
    }
    CloseHandle(h);
  }
  CloseHandle(snap);
  return best;
#else
  return 0ul;
#endif
}

// iter-463: parses sidecar TEXT (the boot loader reads the file into a string first) so a
// runtime translation's sidecar goes through the very same parser.
void parseSidecar(const std::string& text, std::vector<VfetchElement>& out,
                  std::vector<uint32_t>& slots, std::vector<VfetchElement>* rows = nullptr) {
  std::istringstream f(text);
  std::string line;
  while (std::getline(f, line)) {
    VfetchElement e{};
    uint32_t sgn = 0, integer = 0;
    if (sscanf(line.c_str(), "%u %d %u %u %u %u %u %u", &e.slot, &e.offsetDwords,
               &e.strideDwords, &e.format, &e.usage, &e.usageIndex, &sgn,
               &integer) < 6)
      continue;
    // Every slot the ucode fetches from, before the semantic dedup below.
    if (std::find(slots.begin(), slots.end(), e.slot) == slots.end()) slots.push_back(e.slot);
    e.isSigned = sgn != 0;
    e.isInteger = integer != 0;
    if (rows) rows->push_back(e);  // iter-463: every row, for the miss report
    // Dedup by usage+usageIndex — the layout feeds the D3D12 input signature,
    // which matches by semantic; a re-fetch of the same attribute is benign.
    bool dup = false;
    for (const auto& x : out)
      if (x.usage == e.usage && x.usageIndex == e.usageIndex) { dup = true; break; }
    if (!dup) out.push_back(e);
  }
}

uint32_t rd32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

// Parse the input-signature part out of a DXIL (DXBC) container.
// Container: 'DXBC' + 16B digest + u16 major + u16 minor + u32 totalSize +
// u32 partCount + u32 partOffsets[]. Part: fourcc + u32 payloadLen + payload.
// 'ISG1' elements are 32 bytes (name offset at +4, semantic index at +8);
// legacy 'ISGN' elements are 24 bytes (name offset at +0, index at +4).
// Name offsets are payload-relative, NUL-terminated. All reads bounds-checked
// (a truncated cache file must not fault the host).
void parseSignatureParts(const std::vector<uint8_t>& blob, const char* fcc1, const char* fcc0,
                         std::vector<SigElement>& out);
void parseInputSignature(const std::vector<uint8_t>& blob, std::vector<SigElement>& out) {
  parseSignatureParts(blob, "ISG1", "ISGN", out);
}
// P1: the same element layout serves the OUTPUT signature (OSG1 / OSGN).
void parseOutputSignature(const std::vector<uint8_t>& blob, std::vector<SigElement>& out) {
  parseSignatureParts(blob, "OSG1", "OSGN", out);
}
void parseSignatureParts(const std::vector<uint8_t>& blob, const char* fcc1, const char* fcc0,
                         std::vector<SigElement>& out) {
  const uint8_t* d = blob.data();
  const size_t n = blob.size();
  if (n < 36 || std::memcmp(d, "DXBC", 4) != 0) return;
  const uint32_t partCount = rd32(d + 28);
  if (partCount > 64) return;
  if (32 + size_t(partCount) * 4 > n) return;
  for (uint32_t i = 0; i < partCount; i++) {
    const uint32_t po = rd32(d + 32 + size_t(i) * 4);
    if (size_t(po) + 8 > n) continue;
    const uint8_t* cc = d + po;
    const uint32_t plen = rd32(d + po + 4);
    if (size_t(po) + 8 + plen > n) continue;
    const bool isg1 = std::memcmp(cc, fcc1, 4) == 0;
    if (!isg1 && std::memcmp(cc, fcc0, 4) != 0) continue;
    const uint8_t* pay = d + po + 8;
    if (plen < 8) return;
    const uint32_t count = rd32(pay);
    const uint32_t paramOff = rd32(pay + 4);
    const uint32_t elemSize = isg1 ? 32u : 24u;
    const uint32_t nameField = isg1 ? 4u : 0u;
    const uint32_t idxField = isg1 ? 8u : 4u;
    if (count > 64) return;
    for (uint32_t e = 0; e < count; e++) {
      const size_t eb = size_t(paramOff) + size_t(e) * elemSize;
      if (eb + elemSize > plen) return;
      const uint32_t nameOff = rd32(pay + eb + nameField);
      const uint32_t semIdx = rd32(pay + eb + idxField);
      if (nameOff >= plen) return;
      const char* nm = reinterpret_cast<const char*>(pay + nameOff);
      const size_t maxLen = plen - nameOff;
      const size_t len = strnlen(nm, maxLen);
      if (len == 0 || len == maxLen) return;
      out.push_back({std::string(nm, len), semIdx});
    }
    return;  // one input-signature part per container
  }
}
}  // namespace

// iter-463: the miss report's state (declared in the header, defined before first use).
struct ShaderCache::MissState {
  std::mutex mu;
  struct Ucode {
    bool keyed = false;
    uint64_t key = 0;  // masked key
    std::vector<mkey::FetchRow> rows;
  };
  std::unordered_map<uint64_t, Ucode> ucode;            // VS hash -> captured ucode facts
  std::unordered_map<uint64_t, uint64_t> firstPs;       // VS hash -> PS of its first dropped draw
  std::vector<uint64_t> unreported;                     // first sightings not yet logged
  std::vector<std::pair<uint64_t, uint32_t>> drain;     // drops per VS since the last summary
  uint64_t pendingDraws = 0, totalDraws = 0;            // since the last summary / since boot
  std::chrono::steady_clock::time_point lastSummary{};
  // render/shader-coverage [SHADER_DROP] census (release/installer cb475b0f): every draw without its VS or PS.
  struct DropFirst {
    uint64_t hash = 0, other = 0;
    bool isVs = false;
    int state = 0;
  };
  std::unordered_set<uint64_t> dropSeen;                // hash ^ kind ^ state: first drop logged
  std::vector<DropFirst> dropUnreported;                // first drops not yet logged
  std::vector<std::tuple<uint64_t, bool, uint32_t>> dropTop;  // drops per shader since the summary
  uint64_t dropPending = 0, dropVs = 0, dropPs = 0;
  std::chrono::steady_clock::time_point lastDropSummary{};
};

const char* dropStateName(int st) {
  switch (st) {
    case kDropNoTranslator: return "no-runtime-translator";
    case kDropQueued: return "queued";
    case kDropWaiting: return "no-template";
    case kDropFailed: return "translation-failed";
    case kDropRefused: return "translation-refused";
    case kDropDisabled: return "translator-disabled";
    case kDropNoRequest: return "not-requested";
    case kDropWaitedOut: return "wait-budget-exceeded";
    case kDropAlphaVariantPending: return "alpha-test-variant-pending";
    case kDropAlphaVariantFailed: return "alpha-test-variant-unavailable";
    default: return "unknown";
  }
}

uint64_t srXxh3Avx2(const void* data, size_t bytes);  // gfx/hash_avx2.cpp

uint64_t ShaderCache::hashUcode(const void* data, size_t bytes) {
  // Identical results either way; AVX2 when the CPU has it (SR_HASH_AVX2=0 keeps SSE2).
  static const bool s_avx2 = [] {
    const char* e = std::getenv("SR_HASH_AVX2");
    if (e && e[0] == '0') return false;
    int r[4];
    __cpuid(r, 1);
    const bool osxsave = (r[2] & (1 << 27)) != 0, avx = (r[2] & (1 << 28)) != 0;
    if (!osxsave || !avx || (_xgetbv(0) & 6u) != 6u) return false;  // the OS saves YMM state
    __cpuidex(r, 7, 0);
    return (r[1] & (1 << 5)) != 0;  // AVX2
  }();
  return s_avx2 ? srXxh3Avx2(data, bytes) : XXH3_64bits(data, bytes);
}

void ShaderCache::buildEntry(bool isVs, std::vector<uint8_t> dxil, const std::string& sidecarText,
                             std::vector<uint8_t> variant, uint64_t mkey, CachedShader& sh) {
  sh = CachedShader{};
  sh.dxil = std::move(dxil);
#ifdef _WIN32
  if (FAILED(NativeShaderCode::copy(sh.dxil, sh.nativeCode)))
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[NATIVE_PIPELINE] shader bytecode ownership failed");
#endif
  if (isVs) {
    parseSidecar(sidecarText, sh.layout, sh.fetchSlots, &sh.fetchRows);
    parseInputSignature(sh.dxil, sh.signature);
    sh.dxilFetch = std::move(variant);
#ifdef _WIN32
    if (!sh.dxilFetch.empty() && FAILED(NativeShaderCode::copy(sh.dxilFetch, sh.nativeFetchCode)))
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[NATIVE_PIPELINE] fetch shader bytecode ownership failed");
#endif
    sh.mkey = mkey;
  } else {
    parseOutputSignature(sh.dxil, sh.outputs);
    // step 4 lane C (0005): the alpha-test variant (<hash>.ps.at.dxil), when there is one.
    sh.dxilAlphaTest = std::move(variant);
#ifdef _WIN32
    if (!sh.dxilAlphaTest.empty() && FAILED(NativeShaderCode::copy(sh.dxilAlphaTest, sh.nativeAlphaTestCode)))
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[NATIVE_PIPELINE] alpha-test shader bytecode ownership failed");
#endif
  }
}

bool ShaderCache::loadEntryFromDir(const std::filesystem::path& dir, uint64_t hash, bool isVs,
                                   CachedShader& out) {
  char stem[32];
  std::snprintf(stem, sizeof(stem), "%016" PRIx64 ".%s", hash, isVs ? "vs" : "ps");
  std::vector<uint8_t> dxil, vsf, side;
  if (!readFile(dir / (std::string(stem) + ".dxil"), dxil) || dxil.empty()) return false;
  uint64_t mk = 0;
  if (isVs) {
    readFile(dir / (std::string(stem) + ".vfetch"), side);
    readFile(dir / (std::string(stem) + ".vsf.dxil"), vsf);
    std::ifstream mf(dir / (std::string(stem) + ".mkey"));
    std::string line;
    if (mf && std::getline(mf, line)) mk = strtoull(line.c_str(), nullptr, 16);
  } else {
    readFile(dir / (std::string(stem) + ".at.dxil"), vsf);  // step 4 lane C (0005): the alpha-test variant
  }
  buildEntry(isVs, std::move(dxil), std::string(side.begin(), side.end()), std::move(vsf), mk, out);
  return true;
}

// ── render/boot-attach: the staged boot load (docs/NATIVE_ROADMAP.md "Boot attach") ─────────────────
std::filesystem::path ShaderCache::resolveInstalledDir() {
  return shippedDataDir("SR_SHADER_CACHE_DIR", "data/shader_cache/dxil");
}

// iter-465 player: a player's shortcut may start the game in another folder; the installed cache ships next to the
// exe (tools/player/make_player_build.ps1). render/shader-coverage (orchestrator 2026-10-17): the exe's own folder
// is now searched FIRST - before, a package started from a folder that had its own data/shader_cache (the dev
// checkout, the Ally's older checkout) read that folder's files: lF_R2 / lF_R4 compiled 7 resolve kernels at boot and
// tripped the watchdog's H3. A dev build (build/, a tools/snapshot_exe.ps1 copy) has no data/shader_cache/dxil beside
// it, so it still reads the working folder's.
std::filesystem::path ShaderCache::shippedDataDir(const char* envName, const char* rel, bool cwdFirst) {
  (void)cwdFirst;
  const char* env = envName ? std::getenv(envName) : nullptr;
  if (env && env[0]) return std::filesystem::path(env);
  const std::filesystem::path inCwd = rel;
#ifdef _WIN32
  static const bool oldOrder = [] {
    const char* e = std::getenv("SR_DATA_CWD_FIRST");
    return e && e[0] == '1';
  }();
  std::error_code ec;
  if ((cwdFirst || oldOrder) && std::filesystem::is_directory(inCwd, ec)) return inCwd;
  wchar_t buf[32768];
  const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof buf / sizeof buf[0]));
  if (n > 0 && n < sizeof buf / sizeof buf[0]) {
    const std::filesystem::path besideExe = std::filesystem::path(buf).parent_path() / inCwd;
    if (std::filesystem::is_directory(besideExe, ec)) return besideExe;
  }
#endif
  return inCwd;
}

// The names stage 1 admits (loadInstalled's filter): <16 hex>.<vs|ps>.dxil, not the .vs.vsf.dxil
// variants, non-empty. The size comes from the directory listing (no file is opened), so a file that
// lists but cannot be read passes here and is caught when stage 1 admits nothing (installedPresent).
bool ShaderCache::probeInstalledDir(const std::filesystem::path& dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return false;
  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.size() < 24 || name.substr(name.size() - 5) != ".dxil") continue;
    const bool isVs = name.compare(17, 2, "vs") == 0;
    if (isVs && name.substr(name.size() - 9) == ".vsf.dxil") continue;
    if (!isVs && name.substr(name.size() - 8) == ".at.dxil") continue;  // step 4 lane C (0005): a variant, not an entry
    std::error_code sec;
    const auto size = it->file_size(sec);
    if (!sec && size > 0) return true;
  }
  return false;
}

bool ShaderCache::bootLoadAsync() {
  static const bool v = [] {
    const char* e = std::getenv("SR_SHADER_LOAD_ASYNC");
    return !e || e[0] != '0';
  }();
  return v;
}

void ShaderCache::setWaitObserver(const WaitObserver* obs) { g_waitObs.store(obs, std::memory_order_release); }

void ShaderCache::hostWaitBegin(int what) {
  const WaitObserver* obs = g_waitObs.load(std::memory_order_acquire);
  if (obs && obs->begin) obs->begin(what);
}

void ShaderCache::hostWaitEnd(int what) {
  const WaitObserver* obs = g_waitObs.load(std::memory_order_acquire);
  if (obs && obs->end) obs->end(what);
}

bool ShaderCache::hostWaitDraining() {
  const WaitObserver* obs = g_waitObs.load(std::memory_order_acquire);
  return obs && obs->draining && obs->draining();
}
void ShaderCache::setReadHookForTest(ReadHook hook) { g_readHook.store(hook, std::memory_order_release); }
void ShaderCache::setPublishHookForTest(PublishHook hook) { g_publishHook.store(hook, std::memory_order_release); }

bool ShaderCache::installedPresent() const {
  if (stage_.load(std::memory_order_acquire) >= kStageInstalled) return !loadedEmpty_.load(std::memory_order_acquire);
  const int p = probe_.load(std::memory_order_acquire);
  if (p >= 0) return p == 1;
  waitStage(kStageInstalled);
  return !loadedEmpty_.load(std::memory_order_acquire);
}

bool ShaderCache::claimLoad() {
  int expected = kStageNone;
  return stage_.compare_exchange_strong(expected, kStageLoading, std::memory_order_acq_rel);
}

void ShaderCache::publish(int stage) {
  {
    std::lock_guard<std::mutex> lk(stageMu_);
    stage_.store(stage, std::memory_order_release);
  }
  stageCv_.notify_all();
}

namespace {
std::string currentThreadName() {
  char buf[96];
#ifdef _WIN32
  PWSTR w = nullptr;
  if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &w)) && w && *w) {
    size_t i = 0;
    for (; w[i] && i + 1 < sizeof(buf); i++) buf[i] = (w[i] < 128) ? char(w[i]) : '?';
    buf[i] = 0;
    LocalFree(w);
    return buf;
  }
  if (w) LocalFree(w);
  std::snprintf(buf, sizeof(buf), "tid %lu", static_cast<unsigned long>(GetCurrentThreadId()));
#else
  std::snprintf(buf, sizeof(buf), "a thread");
#endif
  return buf;
}
}  // namespace

double msSinceProcessStart() {
#ifdef _WIN32
  static const uint64_t created = [] {
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return uint64_t(0);
    return (uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime;
  }();
  FILETIME now{};
  GetSystemTimePreciseAsFileTime(&now);
  const uint64_t n = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
  return created && n >= created ? double(n - created) / 1e4 : -1.0;  // 100 ns units
#else
  return -1.0;
#endif
}

void ShaderCache::beginBootLoad() {
  if (!bootLoadAsync()) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[SHADER_CACHE] SR_SHADER_LOAD_ASYNC=0: the boot load runs inside the backend init, before the "
                "attach (the old order)");
    return;
  }
  const double at = msSinceProcessStart();
  const auto t0 = std::chrono::steady_clock::now();
  const std::filesystem::path dir = resolveInstalledDir();
  const bool present = probeInstalledDir(dir);
  probe_.store(present ? 1 : 0, std::memory_order_release);
  const double probeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const bool ownThread = startLoadAsync();
  // What stays synchronous here: the probe (one directory listing) and the thread's creation; if no thread can be
  // created the whole load has just run on this thread, and the line says so.
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[SHADER_CACHE] boot load on its own thread (SR_SHADER_LOAD_ASYNC=1, default 1): the probe of %s says %s "
              "(probe %.1f ms, from process +%.1f ms); the load %s (process +%.1f ms)",
              dir.string().c_str(), present ? "an installed cache" : "NO installed cache", probeMs, at,
              ownThread ? "runs on the loader thread, the CP waits for its installed stage"
                        : "RAN ON THIS THREAD (no thread could be created): the attach waited for it",
              msSinceProcessStart());
}

bool ShaderCache::startLoadAsync() {
  if (!claimLoad()) return true;
  try {
    std::thread([this] {
#ifdef _WIN32
      SetThreadDescription(GetCurrentThread(), L"SR shader load");
#endif
      runLoad();
    }).detach();
    return true;
  } catch (const std::exception& ex) {
    // No thread: load here, as before (the claim is ours, so a waiter can only be released by us).
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[SHADER_CACHE] cannot start the load thread (%s): loading on this thread",
                ex.what());
    runLoad();
    return false;
  }
}

// Both stages, on the thread that claimed the load. Every stage is published even when its body
// throws (with what it loaded), so no waiter can wait forever on a failed load.
void ShaderCache::runLoad() {
  loaderId_.store(std::this_thread::get_id(), std::memory_order_release);
#ifdef _WIN32
  loaderTid_.store(uint32_t(GetCurrentThreadId()), std::memory_order_relaxed);
#endif
  // render/boot-attach + dcb (the dev merge): one load, one thread, one field set. The thread running this load -
  // the "SR shader load" thread, or the caller that runs it inline (SR_SHADER_LOAD_ASYNC=0, a thread that could
  // not be created, a cache nobody started) - points t_readScope at THIS load's counters: the installed stage's,
  // then the translator stage's. Reads on any other thread are not counted. Each stage is reported once:
  //   started        the process time, this thread and the main thread (tools/perf/boot_threads.py reads them)
  //   installed      its wall time, entries, files, end (process time, sampled before publication), dcb's
  //                  wrapped-read statistics and the wrapper-uncovered time
  //   runtime        the translator's own line (DXC load / hash, persisted-dir scan; gfx/shader_runtime_xlat.cpp)
  //   done           the whole load, the installed share, the translator stage with its wrapped reads, its end
  const unsigned long tid = threadIdNow(), mainTid = mainThreadId();
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[SHADER_CACHE] boot load: started at process +%.1f ms on thread %lu (main thread %lu), named '%s'",
              msSinceProcessStart(), tid, mainTid, currentThreadName().c_str());
  ReadScope cacheReads, trReads;
  ReadScope* const prevScope = t_readScope;
  t_readScope = &cacheReads;
  const char* s1Note = "";
  const auto t0 = std::chrono::steady_clock::now();
  try {
    loadInstalled();
  } catch (const std::exception& ex) {
    s1Note = ", FAILED";
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[SHADER_CACHE] boot load: the installed stage failed (%s) - continuing with the %zu VS + %zu PS it "
                "admitted (a missing shader goes to the runtime translator)",
                ex.what(), vs_.size(), ps_.size());
  }
  if (installedAbsent_) s1Note = ", ABSENT: no cache folder";
  loadedEmpty_.store(vs_.empty() && ps_.empty(), std::memory_order_release);
  const auto t1 = std::chrono::steady_clock::now();
  t_readScope = &trReads;
  // Astra round 2: each stage's end is sampled BEFORE its publication, so no reader released by it, and no attach
  // that saw it unpublished, can carry an earlier time than this end (T10).
  const double s1End = msSinceProcessStart();
  installedEndMs_.store(s1End, std::memory_order_relaxed);
  publish(kStageInstalled);
  if (auto* h = g_publishHook.load(std::memory_order_acquire)) h(kStageInstalled);
  const uint64_t s1Us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
  {
    char f[256];
    readScopeFields(f, sizeof f, cacheReads);
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[SHADER_CACHE] boot load: installed stage %.1f ms (%zu VS + %zu PS, %zu files read%s), ended at "
                "process +%.1f ms, on thread %lu (main thread %lu) - %s; uncovered by the wrapper: %.1f ms (directory "
                "enumeration, parsing, allocation)",
                double(s1Us) / 1e3, vs_.size(), ps_.size(), filesRead(), s1Note, s1End, tid, mainTid, f,
                double(s1Us > cacheReads.wrapUs ? s1Us - cacheReads.wrapUs : 0) / 1e3);
  }
  const auto tTr = std::chrono::steady_clock::now();  // after that line: the translator is not charged for it
  // iter-463: the runtime translator admits its persisted translations once the installed
  // cache is in (installed entries win), including when there is no installed cache at all.
  try {
    if (const ShaderCacheRuntimeHooks* h = g_rtHooks.load(std::memory_order_acquire))
      if (h->onLoaded) h->onLoaded(*this);
  } catch (const std::exception& ex) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[SHADER_CACHE] boot load: the runtime translator's boot failed (%s)",
                ex.what());
  }
  const auto t2 = std::chrono::steady_clock::now();
  t_readScope = prevScope;
  const double s2End = msSinceProcessStart();  // before publication, as stage 1's
  publish(kStageAll);
  loaderId_.store(std::thread::id(), std::memory_order_release);
  {
    const uint64_t totalUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(t2 - t0).count());
    const uint64_t trUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(t2 - tTr).count());
    char f[256];
    readScopeFields(f, sizeof f, trReads);
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[SHADER_CACHE] boot load done: %.1f ms on thread %lu (main thread %lu): installed cache %.1f ms%s, "
                "runtime translator %.1f ms - %s; uncovered by the wrapper: %.1f ms (the DXC load + hash on their "
                "own line, the translator's other work); ended at process +%.1f ms",
                double(totalUs) / 1e3, tid, mainTid, double(s1Us) / 1e3,
                installedAbsent_ ? " (ABSENT)" : *s1Note ? " (FAILED)" : "", double(trUs) / 1e3, f,
                double(trUs > trReads.wrapUs ? trUs - trReads.wrapUs : 0) / 1e3, s2End);
  }
}

// WP1 / WP2 (docs/NATIVE_ROADMAP.md "Boot attach"): block until `stage` is published. No timeout: the
// alternative to waiting is a dropped or wrong draw. Over 10 s a Warn names the wait every 10 s.
void ShaderCache::waitStageSlow(int stage) const {
  auto* self = const_cast<ShaderCache*>(this);
  if (self->claimLoad()) {  // nobody started the load: run it here (what call_once did)
    self->runLoad();
    return;
  }
  // The loader's own hooks (the runtime translator's onLoaded) read the cache during stage 2: never wait
  // on yourself. Stage 1 is published before any hook runs.
  if (loaderId_.load(std::memory_order_acquire) == std::this_thread::get_id()) return;
  const char* what = stage >= kStageAll ? "translator stage (persisted translations)" : "installed shader cache";
  const WaitObserver* obs = g_waitObs.load(std::memory_order_acquire);
  if (obs && obs->begin) obs->begin(stage);
  const auto t0 = std::chrono::steady_clock::now();
  {
    std::unique_lock<std::mutex> lk(stageMu_);
    while (stage_.load(std::memory_order_acquire) < stage) {
      if (stageCv_.wait_for(lk, std::chrono::seconds(10)) == std::cv_status::timeout &&
          stage_.load(std::memory_order_acquire) < stage) {
        lk.unlock();
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[SHADER_CACHE] %s still waiting for the %s after %.0f s: %zu files read so far",
                    currentThreadName().c_str(), what,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), filesRead());
        lk.lock();
      }
    }
  }
  if (obs && obs->end) obs->end(stage);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[SHADER_CACHE] %s waited %.1f ms for the %s",
              currentThreadName().c_str(),
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), what);
}

ShaderCache::MissState* ShaderCache::missState() {
  std::call_once(missOnce_, [this] {
    if (missReportEnabled()) miss_ = new MissState();  // iter-463
  });
  return miss_;
}

// Stage 1 (the old ensureLoaded body, on the loader thread). Writes the installed maps; runLoad
// publishes them.
void ShaderCache::loadInstalled() {
    const std::filesystem::path dir = resolveInstalledDir();
    installedDir_ = dir;
    // render/boot-attach: every stage-1 read goes through the test hook and the progress count.
    auto readFile = [this](const std::filesystem::path& p, std::vector<uint8_t>& out) {
      filesRead_.fetch_add(1, std::memory_order_relaxed);
      if (const ReadHook hk = g_readHook.load(std::memory_order_acquire))
        if (!hk(p)) return false;
      return sr::gfx::readFile(p, out);
    };
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
      // iter-375: this is a WARNING, not an info note. Without the cache EVERY
      // translated draw silently falls back to the fixed-function painter — the
      // renderer keeps running and produces a plausible-looking wrong image, with
      // no error anywhere. That is the worst possible failure mode for a project
      // whose baseline is supposed to be reproducible by the human.
      //
      // The cache is NOT in the repo: data/ is gitignored because it holds
      // copyrighted game content, and these DXIL blobs are translated GAME
      // shaders, i.e. derived works. Regenerating them needs the ~12 GB golden
      // Xenia dump plus the XenosRecomp clone — see tools/xenosrecomp/README.md.
      // So a fresh clone CANNOT reproduce any render result until that is done,
      // and it must say so loudly rather than degrade in silence.
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[SHADER_CACHE] NO CACHE at %s — every translated draw will fall "
                  "back to fixed-function and the image will be WRONG. Regenerate "
                  "per tools/xenosrecomp/README.md (needs the golden dump).",
                  dir.string().c_str());
      installedAbsent_ = true;  // dcb's boot-load lines: the stage-1 line says ABSENT
      return;
    }
    std::unordered_map<uint64_t, std::vector<uint8_t>> fetchVariants, alphaTestVariants;
    for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      auto name = entry.path().filename().string();  // <16 hex>.<vs|ps>.dxil
      if (name.size() < 24 || name.substr(name.size() - 5) != ".dxil") continue;
      uint64_t hash = strtoull(name.substr(0, 16).c_str(), nullptr, 16);
      const bool isVs = name.compare(17, 2, "vs") == 0;
      if (isVs && name.size() >= 9 && name.substr(name.size() - 9) == ".vsf.dxil") {
        std::vector<uint8_t> blob;
        if (readFile(entry.path(), blob) && !blob.empty()) fetchVariants[hash] = std::move(blob);
        continue;
      }
      // step 4 lane C (0005): <hash>.ps.at.dxil, the alpha-test variant - attached after the scan.
      if (!isVs && name.size() >= 8 && name.substr(name.size() - 8) == ".at.dxil") {
        std::vector<uint8_t> blob;
        if (readFile(entry.path(), blob) && !blob.empty()) alphaTestVariants[hash] = std::move(blob);
        continue;
      }
      std::vector<uint8_t> dxil;
      if (!readFile(entry.path(), dxil) || dxil.empty()) continue;
      CachedShader sh;
      if (isVs) {
        auto sidecar = entry.path();
        sidecar.replace_extension(".vfetch");  // x.vs.dxil -> x.vs.vfetch
        std::vector<uint8_t> side;
        readFile(sidecar, side);
        // iter-463: <hash>.vs.mkey (tools/shader_mkey.py / stage_missing_vs.py) = the
        // program's masked key, for the miss report's twin lookup.
        uint64_t mk = 0;
        if (missReportEnabled()) {
          auto mkp = entry.path();
          mkp.replace_extension(".mkey");  // x.vs.dxil -> x.vs.mkey
          std::ifstream mf(mkp);
          std::string line;
          if (mf && std::getline(mf, line)) mk = strtoull(line.c_str(), nullptr, 16);
        }
        // The .vsf.dxil variant is attached after the scan (fetchVariants below).
        buildEntry(true, std::move(dxil), std::string(side.begin(), side.end()), {}, mk, sh);
        if (sh.mkey) vsByMkey_.emplace(sh.mkey, hash);
        vs_.emplace(hash, std::move(sh));
      } else {
        buildEntry(false, std::move(dxil), std::string(), {}, 0, sh);
        ps_.emplace(hash, std::move(sh));
      }
    }
    size_t nFetch = 0;
    for (auto& [h, blob] : fetchVariants) {
      auto it = vs_.find(h);
      if (it == vs_.end()) continue;
      it->second.dxilFetch = std::move(blob);
#ifdef _WIN32
      it->second.nativeFetchCode = {};
      if (FAILED(NativeShaderCode::copy(it->second.dxilFetch, it->second.nativeFetchCode)))
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[NATIVE_PIPELINE] fetch variant ownership failed");
#endif
      nFetch++;
    }
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SHADER_CACHE] %zu VS carry an in-shader vertex-fetch variant",
                nFetch);
    size_t nAlphaTest = 0;
    for (auto& [h, blob] : alphaTestVariants) {
      auto it = ps_.find(h);
      if (it == ps_.end()) continue;
      it->second.dxilAlphaTest = std::move(blob);
#ifdef _WIN32
      it->second.nativeAlphaTestCode = {};
      if (FAILED(NativeShaderCode::copy(it->second.dxilAlphaTest, it->second.nativeAlphaTestCode)))
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[NATIVE_PIPELINE] alpha-test variant ownership failed");
#endif
      nAlphaTest++;
    }
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SHADER_CACHE] %zu PS carry an alpha-test variant (.ps.at.dxil)", nAlphaTest);
    if (missReportEnabled())
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[SHADER_CACHE] %zu of %zu VS carry a masked key (.vs.mkey); the miss report names "
                  "a twin only for ucode noted at IM_LOAD (noteVsMissUcode)",
                  vsByMkey_.size(), vs_.size());
    // Census: signature-vs-sidecar mismatches are the E_INVALIDARG PSO class.
    size_t nNoSig = 0, nSigWider = 0;
    for (const auto& [h, sh] : vs_) {
      if (sh.signature.empty()) nNoSig++;
      else if (sh.signature.size() > sh.layout.size()) nSigWider++;
    }
    // P1: PS output signatures drive render-target binding; a PS with none
    // parsed would bind no color at all, so count them.
    size_t nPsNoOut = 0, nPsMrt = 0, nPsDepth = 0;
    for (const auto& [h, sh] : ps_) {
      if (sh.outputs.empty()) nPsNoOut++;
      if (sh.colorOutMask() & ~1u) nPsMrt++;
      if (sh.writesDepth()) nPsDepth++;
    }
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[SHADER_CACHE] loaded %zu VS + %zu PS translated shaders from %s "
                "(noSig=%zu sigWiderThanSidecar=%zu psNoOutputs=%zu psMrt=%zu psDepth=%zu)",
                vs_.size(), ps_.size(), dir.string().c_str(), nNoSig, nSigWider,
                nPsNoOut, nPsMrt, nPsDepth);
}

// iter-466 cp round 2 (SR_CP_SHADER_MEMO): the drain asks for the same VS / PS several times per
// draw and draw after draw (~1.3% of the CP in unordered_map finds + the runtime map's shared
// lock). A found entry never moves or changes (nothing is erased; an installed hash refuses a
// runtime twin), so the last FOUND answer per thread, cache and stage is exact; misses are
// not remembered (a runtime translation can land later).
static bool shaderMemoOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_CP_SHADER_MEMO");
    const bool on = !e || e[0] != '0';
    SR_LOG_BOOT(sr::LogSeverity::Info, "[CP_GATE] SR_CP_SHADER_MEMO=%d (default 1)", on ? 1 : 0);
    return on;
  }();
  return v;
}
struct ShaderFindMemo {
  const void* cache = nullptr;
  uint64_t h = 0;
  const CachedShader* p = nullptr;
};
thread_local ShaderFindMemo t_vsFind, t_psFind;

// render/boot-attach WP2: the installed maps are read only after stage 1 is published, and a miss is
// answered only after stage 2 (where the persisted runtime translations are admitted) - the answer a
// fully loaded cache gives. Both waits are one acquire load once the load is done.
const CachedShader* ShaderCache::findVs(uint64_t h) const {
  const bool memo = shaderMemoOn();
  if (memo && t_vsFind.p && t_vsFind.h == h && t_vsFind.cache == this) return t_vsFind.p;
  waitStage(kStageInstalled);
  const CachedShader* r = nullptr;
  auto it = vs_.find(h);
  if (it == vs_.end()) waitStage(kStageAll);
  if (it != vs_.end()) {
    r = &it->second;
  } else if (rtCount_.load(std::memory_order_acquire) != 0) {
    std::shared_lock<std::shared_mutex> lk(rtMu_);
    auto rt = rtVs_.find(h);
    r = rt != rtVs_.end() ? rt->second.get() : nullptr;
  }
  if (memo && r) t_vsFind = ShaderFindMemo{this, h, r};
  return r;
}

const CachedShader* ShaderCache::findPs(uint64_t h) const {
  const bool memo = shaderMemoOn();
  if (memo && t_psFind.p && t_psFind.h == h && t_psFind.cache == this) return t_psFind.p;
  waitStage(kStageInstalled);  // WP2, as findVs
  const CachedShader* r = nullptr;
  auto it = ps_.find(h);
  if (it == ps_.end()) waitStage(kStageAll);
  if (it != ps_.end()) {
    r = &it->second;
  } else if (rtCount_.load(std::memory_order_acquire) != 0) {
    std::shared_lock<std::shared_mutex> lk(rtMu_);
    auto rt = rtPs_.find(h);
    r = rt != rtPs_.end() ? rt->second.get() : nullptr;
  }
  if (memo && r) t_psFind = ShaderFindMemo{this, h, r};
  return r;
}

// ── iter-463: runtime translation (gfx/shader_runtime_xlat.cpp) ──────────────
const CachedShader* ShaderCache::findInstalledVs(uint64_t h) const {
  waitStage(kStageInstalled);  // render/boot-attach WP2
  auto it = vs_.find(h);
  return it != vs_.end() ? &it->second : nullptr;
}

const CachedShader* ShaderCache::findInstalledPs(uint64_t h) const {
  waitStage(kStageInstalled);
  auto it = ps_.find(h);
  return it != ps_.end() ? &it->second : nullptr;
}

void ShaderCache::setRuntimeHooks(const ShaderCacheRuntimeHooks* hooks) {
  g_rtHooks.store(hooks, std::memory_order_release);
}

void ShaderCache::requestTranslation(uint64_t hash, bool isVs, const void* ucode, size_t bytes) {
  const ShaderCacheRuntimeHooks* h = g_rtHooks.load(std::memory_order_acquire);
  if (h && h->onMiss && ucode && bytes) h->onMiss(*this, hash, isVs, ucode, bytes);
}

bool ShaderCache::wantsContainers() {
  const ShaderCacheRuntimeHooks* h = g_rtHooks.load(std::memory_order_acquire);
  return h && h->wantContainers && h->onContainer && h->wantContainers();
}

void ShaderCache::noteContainer(const void* blob, size_t bytes, bool isVs) {
  const ShaderCacheRuntimeHooks* h = g_rtHooks.load(std::memory_order_acquire);
  if (h && h->onContainer && blob && bytes) h->onContainer(blob, bytes, isVs);
}

// ── step 4 lane C round 4: the alpha-test variant of a PS whose entry has none (gfx/alpha_test.hpp use()) ──
const std::vector<uint8_t>* ShaderCache::alphaTestDxil(uint64_t h, const CachedShader& ps) const {
  if (!ps.dxilAlphaTest.empty()) return &ps.dxilAlphaTest;
  if (rtAtCount_.load(std::memory_order_acquire) == 0) return nullptr;
  std::shared_lock<std::shared_mutex> lk(rtMu_);
  auto it = rtAt_.find(h);
  return it != rtAt_.end() ? &it->second->dxil : nullptr;
}

bool ShaderCache::hasAlphaTestVariant(uint64_t h, const CachedShader& ps) const {
  return alphaTestDxil(h, ps) != nullptr;
}

#ifdef _WIN32
NativeShaderCode ShaderCache::alphaTestCode(uint64_t h, const CachedShader& ps) const {
  if (!ps.dxilAlphaTest.empty()) return ps.nativeAlphaTestCode;
  if (rtAtCount_.load(std::memory_order_acquire) == 0) return {};
  std::shared_lock<std::shared_mutex> lk(rtMu_);
  auto it = rtAt_.find(h);
  return it != rtAt_.end() ? it->second->code : NativeShaderCode{};
}
#endif

bool ShaderCache::needsAlphaTestVariant(uint64_t h, const CachedShader& ps) const {
  // Cheapest first: every IM_LOAD of a cached PS asks (record_engine), and all but six of the shipped ones have it.
  if (!ps.dxilAlphaTest.empty() || (ps.colorOutMask() & 1u) == 0u) return false;
  return !hasAlphaTestVariant(h, ps);
}

void ShaderCache::requestAlphaTestVariant(uint64_t hash, const void* ucode, size_t bytes) {
  if (!alphatest::enabled() || !ucode || !bytes) return;
  const CachedShader* ps = findPs(hash);
  if (!ps || !needsAlphaTestVariant(hash, *ps)) return;
  const ShaderCacheRuntimeHooks* h = g_rtHooks.load(std::memory_order_acquire);
  if (h && h->onMissAlphaTest) h->onMissAlphaTest(*this, hash, ucode, bytes);
}

bool ShaderCache::admitRuntimeAlphaTest(uint64_t h, const CachedShader& ps, std::vector<uint8_t> dxil) {
  if (dxil.empty() || (ps.colorOutMask() & 1u) == 0u || !ps.dxilAlphaTest.empty()) return false;
  auto v = std::make_unique<RuntimeAlphaTest>();
  v->dxil = std::move(dxil);
#ifdef _WIN32
  if (FAILED(NativeShaderCode::copy(v->dxil, v->code))) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[NATIVE_PIPELINE] runtime alpha-test variant ownership failed");
    return false;
  }
#endif
  std::unique_lock<std::shared_mutex> lk(rtMu_);
  if (rtAt_.count(h)) return false;
  rtAt_.emplace(h, std::move(v));
  rtAtCount_.fetch_add(1, std::memory_order_release);
  return true;
}

int ShaderCache::noteAlphaTestDrop(uint64_t psHash) {
  atDrops_.fetch_add(1, std::memory_order_relaxed);
  const ShaderCacheRuntimeHooks* h = g_rtHooks.load(std::memory_order_acquire);
  const int rs = (h && h->onMissAlphaTestDraw) ? h->onMissAlphaTestDraw(psHash) : int(kDropNoTranslator);
  const int st = (rs == kDropQueued || rs == kDropWaiting || rs == kDropWaitedOut) ? int(kDropAlphaVariantPending)
                                                                                    : int(kDropAlphaVariantFailed);
  noteDrop(psHash, false, st, 0);
  return st;
}

int ShaderCache::notePsMissDraw(uint64_t psHash) {
  const ShaderCacheRuntimeHooks* h = g_rtHooks.load(std::memory_order_acquire);
  const int st = (h && h->onMissDraw) ? h->onMissDraw(psHash, false) : int(kDropNoTranslator);
  noteDrop(psHash, false, st, 0);
  return st;
}

void ShaderCache::noteDrop(uint64_t hash, bool isVs, int state, uint64_t otherHash) {
  if (!missReportEnabled()) return;
  if (!missState()) return;
  std::lock_guard<std::mutex> lk(miss_->mu);
  miss_->dropPending++;
  (isVs ? miss_->dropVs : miss_->dropPs)++;
  bool found = false;
  for (auto& [hh, vs, n] : miss_->dropTop)
    if (hh == hash && vs == isVs) { n++; found = true; break; }
  if (!found && miss_->dropTop.size() < 64) miss_->dropTop.emplace_back(hash, isVs, 1u);
  // The first drop of this shader in this STATE (a queued shader that later fails is logged again).
  const uint64_t k = hash ^ (isVs ? 0x8000000000000000ull : 0) ^ (uint64_t(state) << 56);
  if (miss_->dropSeen.size() < 4096 && miss_->dropSeen.insert(k).second)
    miss_->dropUnreported.push_back({hash, otherHash, isVs, state});
}

bool ShaderCache::admitRuntime(uint64_t hash, bool isVs, CachedShader&& entry) {
  waitStage(kStageInstalled);  // render/boot-attach: the installed maps are complete before "installed wins"
  if (isVs ? vs_.count(hash) != 0 : ps_.count(hash) != 0) return false;  // installed wins
  std::unique_lock<std::shared_mutex> lk(rtMu_);
  auto& m = isVs ? rtVs_ : rtPs_;
  if (m.count(hash)) return false;
  m.emplace(hash, std::make_unique<CachedShader>(std::move(entry)));
  rtCount_.fetch_add(1, std::memory_order_release);
  return true;
}

// ── iter-463: vertex-shader miss report ──────────────────────────────────────
// SR_SHADER_MISS_REPORT (default ON; =0 = the old path: only the capped
// [XLAT_REJECT] line names a missing VS, no mkey index is loaded).
bool ShaderCache::missReportEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_SHADER_MISS_REPORT");
    return !e || e[0] != '0';
  }();
  return v;
}

uint64_t ShaderCache::maskedKey(const void* ucode, size_t bytes, bool* ok) {
  std::vector<uint8_t> m;
  const bool good = mkey::maskedUcode(static_cast<const uint8_t*>(ucode), bytes, m);
  if (ok) *ok = good;
  return good ? XXH3_64bits(m.data(), m.size()) : 0;
}

const CachedShader* ShaderCache::findVsTwin(uint64_t mk, uint64_t* twinHash) const {
  if (!mk) return nullptr;
  waitStage(kStageInstalled);  // render/boot-attach WP2: the mkey index is stage 1's
  // Several cached variants may share a program; the lowest hash is a stable choice.
  const CachedShader* best = nullptr;
  uint64_t bestHash = 0;
  auto range = vsByMkey_.equal_range(mk);
  for (auto it = range.first; it != range.second; ++it) {
    auto sh = vs_.find(it->second);
    if (sh == vs_.end()) continue;
    if (!best || it->second < bestHash) { best = &sh->second; bestHash = it->second; }
  }
  if (best && twinHash) *twinHash = bestHash;
  return best;
}

void ShaderCache::noteVsMissUcode(uint64_t hash, const void* ucode, size_t bytes) {
  if (!missReportEnabled() || !ucode || !bytes) return;
  if (!missState()) return;  // render/boot-attach: the report state no longer waits for the load
  std::lock_guard<std::mutex> lk(miss_->mu);
  if (miss_->ucode.size() >= 256 || miss_->ucode.count(hash)) return;
  MissState::Ucode u;
  const auto* p = static_cast<const uint8_t*>(ucode);
  bool ok = false;
  u.key = maskedKey(p, bytes, &ok);
  u.keyed = ok;
  if (ok) mkey::deriveFetchRows(p, bytes, u.rows);
  miss_->ucode.emplace(hash, std::move(u));
}

// Caller holds miss_->mu (flushVsMissReport) or is single-threaded (the unit test).
std::string ShaderCache::diagnoseVsMiss(uint64_t hash) const {
  char buf[640];
  const MissState::Ucode* u = nullptr;
  if (miss_) {
    auto it = miss_->ucode.find(hash);
    if (it != miss_->ucode.end()) u = &it->second;
  }
  if (!u)
    return "ucode not noted, so no twin lookup (noteVsMissUcode keeps the first 256 missed VS) - "
           "SR_SHADER_DUMP_MISSES=1 dumps the ucode to shader_misses/ for tools/stage_missing_vs.py";
  if (!u->keyed)
    return "ucode captured but its control flow does not walk (no exec) - no masked key";
  uint64_t twin = 0;
  const CachedShader* t = findVsTwin(u->key, &twin);
  if (!t) {
    // The mkey keeps dst_swiz, the mini bit and prefetch; stage_missing_vs.py treats those
    // as bind-time patch fields too, so it may still prove a same-program sibling.
    std::snprintf(buf, sizeof(buf),
                  "no cached VS with the same program AND the same fetch swizzles/mini/prefetch "
                  "(mkey %016" PRIX64 ") - stage a translation (tools/stage_missing_vs.py)", u->key);
    return buf;
  }
  // Name the layout fields that differ (runtime ucode vs the twin's sidecar), row by row.
  std::string diff;
  const size_t n = std::min(u->rows.size(), t->fetchRows.size());
  for (size_t i = 0; i < n && diff.size() < 360; i++) {
    const auto& r = u->rows[i];
    const auto& c = t->fetchRows[i];
    char d[96];
    int k = std::snprintf(d, sizeof(d), " i%u:", r.instr);
    const int k0 = k;
    if (r.strideDwords != c.strideDwords)
      k += std::snprintf(d + k, sizeof(d) - k, " stride %u(cached %u)", r.strideDwords, c.strideDwords);
    if (r.offsetDwords != c.offsetDwords)
      k += std::snprintf(d + k, sizeof(d) - k, " off %d(%d)", r.offsetDwords, c.offsetDwords);
    if (r.format != c.format)
      k += std::snprintf(d + k, sizeof(d) - k, " fmt %u(%u)", r.format, c.format);
    if (r.isSigned != c.isSigned || r.isInteger != c.isInteger)
      k += std::snprintf(d + k, sizeof(d) - k, " sgn/int %d%d(%d%d)", int(r.isSigned), int(r.isInteger),
                         int(c.isSigned), int(c.isInteger));
    if (k != k0) diff += d;
  }
  if (u->rows.size() != t->fetchRows.size()) diff += " (fetch row count differs)";
  std::snprintf(buf, sizeof(buf),
                "same program as cached VS %016" PRIX64 " with another vertex layout:%s. Not reused: "
                "the PM4 path's .vs.vsf.dxil bakes stride/offset/format - stage a translation "
                "(tools/stage_missing_vs.py)", twin, diff.empty() ? " (layout identical?)" : diff.c_str());
  return buf;
}

int ShaderCache::noteVsMissDraw(uint64_t vsHash, uint64_t psHash) {
  int st = kDropNoTranslator;
  if (const ShaderCacheRuntimeHooks* h = g_rtHooks.load(std::memory_order_acquire))
    if (h->onMissDraw) st = h->onMissDraw(vsHash, true);  // iter-463 admission census
  noteDrop(vsHash, true, st, psHash);
  if (!missReportEnabled()) return st;
  if (!missState()) return st;
  std::lock_guard<std::mutex> lk(miss_->mu);
  miss_->pendingDraws++;
  miss_->totalDraws++;
  bool found = false;
  for (auto& [h, n] : miss_->drain)
    if (h == vsHash) { n++; found = true; break; }
  if (!found && miss_->drain.size() < 64) miss_->drain.emplace_back(vsHash, 1u);
  if (miss_->firstPs.size() < 256 && miss_->firstPs.emplace(vsHash, psHash).second)
    miss_->unreported.push_back(vsHash);
  return st;
}

void ShaderCache::flushVsMissReport() {
  if (!missReportEnabled()) return;
  // render/boot-attach: runs after every drain, so it never waits for the load. A dropped VS draw
  // implies its IM_LOAD passed WP2 already; before stage 1 there is nothing to report (the twin
  // lookup below reads stage 1's index).
  if (!installedLoaded()) return;
  if (!missState()) return;
  std::lock_guard<std::mutex> lk(miss_->mu);
  // [SHADER_DROP] is a milestone tag (app/log.cpp): a player log keeps every line, so a draw without its
  // shader is never silent (tools/shader_miss_census.py grades a log on them).
  for (const auto& d : miss_->dropUnreported) {
    // Astra round 1 (P3): a request still queued after its wait, or waiting for a template, can still be admitted
    // (the worker goes on; a new container retries it) - only failed / refused / no translator are final.
    const bool transient = d.state == kDropQueued || d.state == kDropWaitedOut || d.state == kDropWaiting ||
                           d.state == kDropAlphaVariantPending;
    if (d.isVs)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[SHADER_DROP] VS %016" PRIX64 " (with PS %016" PRIX64 "): draw DROPPED - %s%s", d.hash, d.other,
                  dropStateName(d.state), transient ? " (drawn once it is admitted)" : " - it will not draw this run");
    else
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[SHADER_DROP] PS %016" PRIX64 ": draw DROPPED - %s%s", d.hash,
                  dropStateName(d.state), transient ? " (drawn once it is admitted)" : " - it will not draw this run");
  }
  miss_->dropUnreported.clear();
  if (miss_->dropPending) {
    const auto now = std::chrono::steady_clock::now();
    if (now - miss_->lastDropSummary >= std::chrono::seconds(2)) {
      miss_->lastDropSummary = now;
      std::sort(miss_->dropTop.begin(), miss_->dropTop.end(),
                [](const auto& a, const auto& b) { return std::get<2>(a) > std::get<2>(b); });
      char top[256];
      int k = 0;
      top[0] = 0;
      for (size_t i = 0; i < miss_->dropTop.size() && i < 4 && k < int(sizeof(top)) - 40; i++)
        k += std::snprintf(top + k, sizeof(top) - size_t(k), " %s %016" PRIX64 "x%u",
                           std::get<1>(miss_->dropTop[i]) ? "VS" : "PS", std::get<0>(miss_->dropTop[i]),
                           std::get<2>(miss_->dropTop[i]));
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[SHADER_DROP] summary: %" PRIu64 " draws dropped since the last summary; since boot %" PRIu64
                  " (VS %" PRIu64 ", PS %" PRIu64 "); top:%s",
                  miss_->dropPending, miss_->dropVs + miss_->dropPs, miss_->dropVs, miss_->dropPs, top);
      miss_->dropPending = 0;
      miss_->dropTop.clear();
    }
  }
  for (uint64_t h : miss_->unreported)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[SHADER_MISS] VS %016" PRIX64 " has no translation - every draw using it is DROPPED "
                "(first with PS %016" PRIX64 "): %s",
                h, miss_->firstPs[h], diagnoseVsMiss(h).c_str());
  miss_->unreported.clear();
  // Drops accumulate across drains until a summary is logged (at most one per 2 s), so
  // the count covers the whole window, not one drain of it (iter-463 review).
  if (miss_->pendingDraws) {
    const auto now = std::chrono::steady_clock::now();
    if (now - miss_->lastSummary >= std::chrono::seconds(2)) {
      miss_->lastSummary = now;
      std::sort(miss_->drain.begin(), miss_->drain.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });
      char top[200];
      int k = 0;
      top[0] = 0;
      for (size_t i = 0; i < miss_->drain.size() && i < 4; i++)
        k += std::snprintf(top + k, sizeof(top) - k, " %016" PRIX64 "x%u", miss_->drain[i].first,
                           miss_->drain[i].second);
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[SHADER_MISS] since last summary: %" PRIu64 " draws dropped for %zu VS with no "
                  "translation (%" PRIu64 " since boot; top:%s)",
                  miss_->pendingDraws, miss_->drain.size(), miss_->totalDraws, top);
      miss_->pendingDraws = 0;
      miss_->drain.clear();
    }
  }
}

ShaderCache& getShaderCache() {
  // Never destroyed (iter-463): the runtime translator's worker admits into it and may still be
  // running while the CRT tears down the statics (the GPU replay returns from main).
  static ShaderCache* s_cache = new ShaderCache();
  return *s_cache;
}

}  // namespace gfx
}  // namespace sr
