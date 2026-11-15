// Step 4 item 2k: the pipeline census, the manifest recorder and the warm-up thread (see pso_warm.hpp).
#include "pso_warm.hpp"

#include "native_pipeline.hpp"
#include "pso_manifest.hpp"
#include "../app/log.hpp"
#include "../kernel/host_work.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace sr::gfx {

namespace {
bool gate(const char* name) {
  const char* e = std::getenv(name);
  return e && e[0] == '1';
}
// Step 4 2k bake (fix10, after the acceptance pso_acc1 + pso_rep9, NOTES 2026-10-20): the manifest recorder and the
// warm-up default ON (=0 turns either off); the census stays a diagnostic (=1).
bool gateDefaultOn(const char* name) {
  const char* e = std::getenv(name);
  return !e || e[0] != '0';
}
bool censusOn() { static const bool on = gate("SR_PSO_CENSUS"); return on; }
bool recordOn() { static const bool on = gateDefaultOn("SR_PSO_RECORD"); return on; }
bool warmOn() { static const bool on = gateDefaultOn("SR_PSO_WARM"); return on; }

// The build's identity for the user-recorded manifest (orchestrator 2026-10-20: a game update must not keep warming
// stale pipelines): the running image's PE TimeDateStamp + SizeOfImage, 16 hex digits - every build its own file.
// Astra (10:5x) P3: TimeDateStamp + SizeOfImage alone is not guaranteed unique, so the id also carries the image's
// CodeView record - the PDB GUID + age the linker writes per link (content-derived under /Brepro), read from the
// loaded image's debug directory (read-only data, never relocated): "<stamp><size>_<guid><age>", all hex.
std::string buildId() {
#if defined(_WIN32)
  const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (base && dos->e_magic == IMAGE_DOS_SIGNATURE) {
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature == IMAGE_NT_SIGNATURE) {
      char id[96];
      int n = snprintf(id, sizeof id, "%08lx%08lx_", (unsigned long)nt->FileHeader.TimeDateStamp,
                       (unsigned long)nt->OptionalHeader.SizeOfImage);
      const auto& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
      bool cv = false;
      if (dd.VirtualAddress && dd.Size >= sizeof(IMAGE_DEBUG_DIRECTORY)) {
        const auto* dbg = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + dd.VirtualAddress);
        for (size_t i = 0; i < dd.Size / sizeof(IMAGE_DEBUG_DIRECTORY) && !cv; ++i) {
          if (dbg[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || dbg[i].SizeOfData < 24 || !dbg[i].AddressOfRawData) continue;
          const uint8_t* rec = base + dbg[i].AddressOfRawData;
          if (std::memcmp(rec, "RSDS", 4) != 0) continue;
          for (int b = 0; b < 16; ++b) n += snprintf(id + n, sizeof id - size_t(n), "%02x", rec[4 + b]);
          uint32_t age;
          std::memcpy(&age, rec + 20, 4);
          n += snprintf(id + n, sizeof id - size_t(n), "%08lx", (unsigned long)age);
          cv = true;
        }
      }
      if (!cv) snprintf(id + n, sizeof id - size_t(n), "%016llx", (unsigned long long)nt->OptionalHeader.CheckSum);
      return id;
    }
  }
#endif
  return "0000000000000000_0000000000000000";
}

// The USER manifest: this player's recorded pipelines, in the user data dir, one file per build
// (%LOCALAPPDATA%\SupermanReturnsRecomp\pso_manifest_v1_<build id>.bin); SR_PSO_MANIFEST=<path> replaces it (tests,
// the acceptance sets). Recording writes only here - never into the install.
bool userPathDefault() {
  const char* e = std::getenv("SR_PSO_MANIFEST");
  return !(e && *e);
}
std::string manifestPath() {
  if (const char* e = std::getenv("SR_PSO_MANIFEST"); e && *e) return e;
  if (const char* la = std::getenv("LOCALAPPDATA"); la && *la)
    return std::string(la) + "\\SupermanReturnsRecomp\\pso_manifest_v1_" + buildId() + ".bin";
  return "pso_manifest_v1_" + buildId() + ".bin";
}

// The SHIPPED manifest: recorded on the release build itself and packaged beside it (tools/player/
// make_player_build.ps1: data\shader_cache\pso_manifest.bin) - what a player's FIRST run (the cold driver cache) warms
// from. Read-only. Found beside the exe first, then under the working folder (a dev checkout); SR_PSO_SHIPPED_MANIFEST
// =<path> replaces it, =0 turns it off. "" = none.
std::string shippedManifestPath() {
  if (const char* e = std::getenv("SR_PSO_SHIPPED_MANIFEST"); e && *e) return e[0] == '0' && !e[1] ? "" : e;
  std::error_code ec;
  const std::filesystem::path rel = std::filesystem::path("data") / "shader_cache" / "pso_manifest.bin";
#if defined(_WIN32)
  wchar_t buf[32768];
  const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(sizeof buf / sizeof buf[0]));
  if (n > 0 && n < sizeof buf / sizeof buf[0]) {
    const std::filesystem::path besideExe = std::filesystem::path(buf).parent_path() / rel;
    if (std::filesystem::is_regular_file(besideExe, ec)) return besideExe.string();
  }
#endif
  if (std::filesystem::is_regular_file(rel, ec)) return rel.string();
  return "";
}

// Older builds' user manifests in the same folder (pso_manifest_v1*.bin other than this build's) are removed at the
// first write - an update leaves no stale file behind. Only for the default user path.
// Astra (10:5x) P3: only exact user-manifest names, regular files, never this build's, and only files no instance
// wrote for 7 days (another build may be running beside this one; the fix12 interim asks >= 24 h - 7 days is more).
// fix12: the names are exact - buildId()'s "<16 hex>_<40 hex>" (stamp + size, CodeView GUID + age) or
// "<16 hex>_<16 hex>" (no CodeView record: the checksum), lowercase - and each removal runs under that file's own
// manifest lock (the one psom::mergeIntoFile takes; an instance of that build writing now holds it) with the age
// checked again inside it; the old build's lock file goes with it.
bool userManifestName(const std::string& n) {
  static const std::string pre = "pso_manifest_v1_", suf = ".bin";
  if (n.size() <= pre.size() + suf.size() || n.compare(0, pre.size(), pre) != 0 ||
      n.compare(n.size() - suf.size(), suf.size(), suf) != 0)
    return false;
  const std::string mid = n.substr(pre.size(), n.size() - pre.size() - suf.size());
  if ((mid.size() != 16 + 1 + 40 && mid.size() != 16 + 1 + 16) || mid[16] != '_') return false;
  for (size_t i = 0; i < mid.size(); ++i)
    if (i != 16 && !((mid[i] >= '0' && mid[i] <= '9') || (mid[i] >= 'a' && mid[i] <= 'f'))) return false;
  return true;
}
constexpr auto kStaleAge = std::chrono::hours(24 * 7);
void removeStaleUserManifests(const std::string& mine) {
  std::error_code ec;
  const std::filesystem::path me(mine);
  const auto now = std::filesystem::file_time_type::clock::now();
  for (std::filesystem::directory_iterator it(me.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    std::error_code fe;
    if (!userManifestName(name) || it->path().filename() == me.filename() || !it->is_regular_file(fe) || fe) continue;
    const auto when = it->last_write_time(fe);
    if (fe || now - when < kStaleAge) continue;   // possibly a build still in use
#if defined(_WIN32)
    const std::wstring lockPath = it->path().wstring() + L".lock";
    HANDLE lock = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock == INVALID_HANDLE_VALUE) continue;
    OVERLAPPED ov{};
    if (!LockFileEx(lock, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
      CloseHandle(lock);   // that build is writing it now: in use
      continue;
    }
    const auto again = std::filesystem::last_write_time(it->path(), fe);
    bool removed = false;
    std::error_code rm;
    if (!fe && now - again >= kStaleAge) {
      removed = std::filesystem::remove(it->path(), rm) && !rm;
      if (removed) {   // the old build's lock file too (deleted once every handle to it is closed)
        FILE_DISPOSITION_INFO del{TRUE};
        SetFileInformationByHandle(lock, FileDispositionInfo, &del, sizeof del);
      }
    }
    UnlockFileEx(lock, 0, 1, 0, &ov);
    CloseHandle(lock);
    if (fe || now - again < kStaleAge) continue;   // written again meanwhile
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PSO_MANIFEST] removed an older build's %s (unused 7+ days; %s)",
                name.c_str(), removed ? "done" : "FAILED");
#endif
  }
}

double msNow() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ── the census (the observer accumulates; the service thread logs) ──
enum Kind { kCp = 0, kRender = 1, kOther = 2, kWarm = 3, kKinds = 4 };
const char* const kKindName[kKinds] = {"the CP", "the render thread", "other threads", "the warm-up"};
struct Tally {
  std::atomic<uint64_t> n{0}, us{0}, maxUs{0};
};
Tally g_tally[kKinds];
std::atomic<uint64_t> g_failures{0}, g_slowLogged{0}, g_createsTotal{0};

void observe(const NativePipelineCreated& c) {
  const int t = sr::kernel::hostwork_detail::t_presenting;
  const Kind k = c.warm ? kWarm : t == 2 ? kCp : t == 1 ? kRender : kOther;
  if (FAILED(c.hr)) {
    g_failures.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint64_t us = uint64_t(c.ms * 1000.0);
  g_tally[k].n.fetch_add(1, std::memory_order_relaxed);
  g_tally[k].us.fetch_add(us, std::memory_order_relaxed);
  uint64_t m = g_tally[k].maxUs.load(std::memory_order_relaxed);
  while (us > m && !g_tally[k].maxUs.compare_exchange_weak(m, us, std::memory_order_relaxed)) {
  }
  g_createsTotal.fetch_add(1, std::memory_order_relaxed);
  if (censusOn() && (k == kCp || k == kRender) && c.ms > 100.0 &&
      g_slowLogged.fetch_add(1, std::memory_order_relaxed) < 16)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[PSO_CREATE] slow: %.1f ms on %s (pipeline #%llu: vs %016llX ps %016llX, %u RT, depth %u) - a draw "
                "waited for this compile",
                c.ms, kKindName[k], (unsigned long long)c.sequence,
                (unsigned long long)c.desc->vertex.hash(), (unsigned long long)c.desc->pixel.hash(),
                c.desc->colorCount, unsigned(c.desc->depthFormat));
}

// ── shared state ──
struct State {
  std::mutex mu;
  std::condition_variable cv;
  bool stop = false;
  NativePipelineCache* cache = nullptr;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root;   // the native xlat root (recorded / warmed under it)
  std::vector<NativePipelineDesc> loaded;             // the USER manifest as read at boot (merged into each write)
  std::vector<NativePipelineDesc> warmList;           // what the warm-up creates: the user's, then the shipped extras
  bool loadedReady = false;                           // under mu: loaded / warmList published (loadManifests)
  bool warmStarted = false;                           // set in the attach before the threads: the warm thread loads
  bool staleChecked = false;                          // removeStaleUserManifests ran (the first write)
  std::string path;
  std::thread service, warm;
  std::atomic<bool> warmDone{false};
  bool serviceExited = false, warmExited = false;   // under mu: the worker returned (psoShutdown waits on cv)
  std::chrono::steady_clock::time_point loadBy{};   // under mu: set by psoShutdown - the exit write's wait for the read
  uint64_t recordedGen = 0;     // the recording generation already in a written manifest (writeManifest)
  uint64_t lastCensusCreates = 0;
};
State& st() {
  static State* s = new State();   // never destroyed: the exit paths below run during static destruction
  return *s;
}
void loadManifests(bool withShipped);   // below: the workers' manifest read
std::atomic<void (*)()> g_loadHookForTest{nullptr};   // tests: runs in loadManifests before the read is published

void logCensus(const char* why) {
  State& s = st();
  if (!censusOn() || !s.cache) return;
  const auto c = s.cache->counters();
  char parts[4][96];
  for (int k = 0; k < kKinds; ++k) {
    const uint64_t n = g_tally[k].n.load(std::memory_order_relaxed);
    snprintf(parts[k], sizeof(parts[k]), "%s %llu (%.0f ms, max %.1f)", kKindName[k], (unsigned long long)n,
             double(g_tally[k].us.load(std::memory_order_relaxed)) * 1e-3,
             double(g_tally[k].maxUs.load(std::memory_order_relaxed)) * 1e-3);
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PSO_CREATE] %s: creates on %s | %s | %s | %s; failures %llu | cache: hits %llu, creates %llu, "
              "warm-made %llu of which a draw used %llu (%llu evicted unused for a draw, %llu not cached: the cache "
              "was full of draws'), single-flight waits %llu (SR_PSO_CENSUS, step 4 2k)",
              why, parts[kCp], parts[kRender], parts[kOther], parts[kWarm],
              (unsigned long long)g_failures.load(), (unsigned long long)c.hits, (unsigned long long)c.creates,
              (unsigned long long)c.warmCreates, (unsigned long long)c.warmUsed, (unsigned long long)c.warmEvicted,
              (unsigned long long)c.warmDropped, (unsigned long long)c.flightWaits);
}

// The session's xlat-root pipelines merged into the manifest (only when new ones exist, unless `force`).
void writeManifest(const char* why, bool force) {
  State& s = st();
  if (!recordOn() || !s.cache || !s.root) return;
  {
    std::lock_guard lk(s.mu);
    if (!s.loadedReady) return;   // the user manifest is still being read: a write now would drop its entries
  }
  // Astra (10:5x) #5: the recording generation - every draw-made pipeline and every warm-made one's FIRST draw use
  // (the cache's counters, read BEFORE the snapshot so nothing counted is missed): a warm pipeline drawn after the
  // last write makes the next write happen, though no newer pipeline was created
  const auto c = s.cache->counters();
  const uint64_t gen = (c.creates - c.warmCreates) + c.warmUsed;
  if (!force && gen <= s.recordedGen) return;
  const auto snap = s.cache->snapshot();
  std::vector<const NativePipelineDesc*> mine;
  mine.reserve(snap.size());
  for (const auto& e : snap) {
    if (e.desc.rootSignature.Get() != s.root.Get()) continue;
    if (e.warm && !e.drawHit) continue;   // warm-made and never drawn: already in a manifest, not this player's use
    mine.push_back(&e.desc);
  }
  const double t0 = msNow();
  if (!s.staleChecked) {
    s.staleChecked = true;
    if (userPathDefault()) removeStaleUserManifests(s.path);
  }
  // Astra (10:5x) #4: under the cross-process lock - the file as it is now, this instance's boot read, its new ones
  psom::MergeResult mr;
  std::string whyNot;
  const bool ok = psom::mergeIntoFile(s.path, s.loaded, mine, &mr, &whyNot);
  if (ok) s.recordedGen = gen;
  char left[96] = "";
  if (mr.skipped) snprintf(left, sizeof left, ", %zu left out (over the reader's limits)", mr.skipped);
  SR_LOG_BOOT(ok ? sr::LogSeverity::Info : sr::LogSeverity::Warn,
              "[PSO_MANIFEST] %s: %s %zu pipelines (%zu on disk, %zu from the boot read, %zu drawn this session%s) to "
              "%s, %zu KB, %.0f ms%s%s (SR_PSO_RECORD)",
              why, ok ? "wrote" : "FAILED to write", mr.total, mr.onDisk, s.loaded.size(), mine.size(), left,
              s.path.c_str(), mr.bytes / 1024, msNow() - t0, ok ? "" : ": ", ok ? "" : whyNot.c_str());
}

void serviceMain() {
#if defined(_WIN32)
  SetThreadDescription(GetCurrentThread(), L"SR pipeline service");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
  State& s = st();
  if (!s.warmStarted && (recordOn() || warmOn())) loadManifests(false);
  double lastWrite = msNow();
  std::unique_lock lk(s.mu);
  while (!s.stop) {
    s.cv.wait_for(lk, std::chrono::seconds(10), [&] { return s.stop; });
    if (s.stop) break;
    lk.unlock();
    const uint64_t creates = g_createsTotal.load(std::memory_order_relaxed);
    if (creates != s.lastCensusCreates) {   // a line only while pipelines are being created
      s.lastCensusCreates = creates;
      logCensus("every 10 s");
    }
    if (msNow() - lastWrite >= 30000.0) {
      writeManifest("every 30 s", false);
      lastWrite = msNow();
    }
    lk.lock();
  }
  // Astra (10:2x) #5: the shutdown asked - the last census line and the last manifest write run HERE, on this
  // thread, so psoShutdown waits for them with its deadline (a stuck disk write cannot hold a window close).
  // fix12 (Astra on fix11): the warm thread may still be reading the user manifest (a large one, a slow disk) - a
  // write before that read ends is skipped (it would drop the player's earlier entries), so the last write waits for
  // it, within the shutdown's deadline less a reserve for the write itself (loadBy).
  bool readDone = s.loadedReady;
  if (!readDone && recordOn()) readDone = s.cv.wait_until(lk, s.loadBy, [&] { return s.loadedReady; });
  lk.unlock();
  if (!readDone && recordOn())
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[PSO_MANIFEST] at exit: the user manifest is still being read - this "
                                       "session's new pipelines are not written (step 4 2k)");
  logCensus("at exit");
  writeManifest("at exit", false);
  lk.lock();
  s.serviceExited = true;
  s.cv.notify_all();
}

// Step 4 2k (orchestrator 2026-10-20): the manifests are read on a WORKER, never on the CP thread in the attach (a
// whole-game shipped manifest is 10+ MB): the warm thread reads the user's and the shipped one, then warms; with the
// warm-up off the service thread reads the user's. The recorder writes nothing until the user manifest is read
// (loadedReady), so a write never drops the player's earlier entries.
void loadManifests(bool withShipped) {
  State& s = st();
  const double t0 = msNow();
  std::vector<NativePipelineDesc> loaded, warmList;
  const std::string path = manifestPath();   // here, not in the attach: the build id reads the image's headers
  {
    std::lock_guard lk(s.mu);
    s.path = path;
  }
  if (warmOn() || recordOn()) {
    std::vector<uint8_t> bytes;
    std::string why;
    if (!psom::readFile(s.path, bytes, &why)) {
      SR_LOG_BOOT(why == "missing or unreadable" ? sr::LogSeverity::Info : sr::LogSeverity::Warn,
                  "[PSO_MANIFEST] user %s: %s (%s)", s.path.c_str(),
                  why == "missing or unreadable" ? "none yet" : "not used", why == "missing or unreadable"
                      ? (recordOn() ? "this session's pipelines will start it" : "recording is off") : why.c_str());
    } else if (!psom::parse(bytes, loaded, &why)) {
      loaded.clear();
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[PSO_MANIFEST] user %s: not used (%s, %zu bytes)", s.path.c_str(),
                  why.c_str(), bytes.size());
    } else {
      SR_LOG_BOOT(sr::LogSeverity::Info, "[PSO_MANIFEST] user %s: %zu pipelines read (%zu KB)", s.path.c_str(),
                  loaded.size(), bytes.size() / 1024);
    }
  }
  if (withShipped) {
    // the warm list: the user's own (this build's first-use order), then every shipped entry the user's lacks
    warmList = loaded;
    const std::string shipped = shippedManifestPath();
    std::vector<uint8_t> bytes;
    std::vector<NativePipelineDesc> fromShip;
    std::string why;
    if (shipped.empty()) {
      const char* off = std::getenv("SR_PSO_SHIPPED_MANIFEST");
      SR_LOG_BOOT(sr::LogSeverity::Info, "[PSO_MANIFEST] shipped: %s",
                  off && off[0] == '0' ? "off (SR_PSO_SHIPPED_MANIFEST=0)"
                                       : "none (data\\shader_cache\\pso_manifest.bin not beside the exe or under the "
                                         "working folder)");
    } else if (!psom::readFile(shipped, bytes, &why) || !psom::parse(bytes, fromShip, &why)) {
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[PSO_MANIFEST] shipped %s: not used (%s)", shipped.c_str(),
                  why.empty() ? "unreadable" : why.c_str());
    } else {
      std::unordered_set<uint64_t> have;
      for (const auto& d : warmList) have.insert(psom::identity(d));
      size_t added = 0;
      for (auto& d : fromShip)
        if (have.insert(psom::identity(d)).second) {
          warmList.push_back(std::move(d));
          ++added;
        }
      SR_LOG_BOOT(sr::LogSeverity::Info, "[PSO_MANIFEST] shipped %s: %zu pipelines (%zu KB), %zu not in the user's",
                  shipped.c_str(), fromShip.size(), bytes.size() / 1024, added);
    }
  }
  if (const auto hook = g_loadHookForTest.load(std::memory_order_acquire)) hook();
  const double ms = msNow() - t0;
  {
    std::lock_guard lk(s.mu);
    s.loaded = std::move(loaded);
    s.warmList = std::move(warmList);
    s.loadedReady = true;
  }
  s.cv.notify_all();
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PSO_MANIFEST] read in %.0f ms on the %s thread", ms,
              withShipped ? "warm-up" : "service");
}

void warmMain() {
#if defined(_WIN32)
  SetThreadDescription(GetCurrentThread(), L"SR pipeline warm");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
  State& s = st();
  loadManifests(true);
  {
    std::lock_guard lk(s.mu);
    if (s.stop || s.warmList.empty()) {
      s.warmExited = true;
      s.cv.notify_all();
      if (!s.stop) SR_LOG_BOOT(sr::LogSeverity::Info, "[PSO_WARM] nothing to warm (no manifest pipelines)");
      return;
    }
  }
  NativePipelineCache::setSingleFlight(1);   // a warm compile must not hold the lock a draw's lookup needs
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PSO_WARM] start: %zu manifest pipelines on a below-normal thread (single flight on)",
              s.warmList.size());
  uint32_t cap = uint32_t(s.warmList.size());
  if (const char* e = std::getenv("SR_PSO_WARM_MAX"); e && *e) cap = uint32_t(std::min<unsigned long>(cap, std::strtoul(e, nullptr, 10)));
  const double t0 = msNow();
  uint32_t created = 0, present = 0, failed = 0, done = 0;
  for (uint32_t i = 0; i < cap; ++i) {
    {
      std::lock_guard lk(s.mu);
      if (s.stop) break;
    }
    NativePipelineDesc d = s.warmList[i];
    d.rootSignature = s.root;
    NativePipelineLease lease;
    const HRESULT hr = s.cache->acquire(d, lease, NativePipelineAcquire::Warm);
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);   // a waiting draw may have lent its own
#endif
    if (hr == S_OK) ++created;
    else if (hr == S_FALSE) ++present;
    else ++failed;
    ++done;
    if (done % 500 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[PSO_WARM] %u of %u: created %u, already there %u, failed %u, %.0f ms",
                  done, cap, created, present, failed, msNow() - t0);
  }
  s.warmDone.store(true, std::memory_order_release);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PSO_WARM] done: %u of %u manifest pipelines in %.0f ms - created %u, already there %u (a draw was "
              "first), failed %u (SR_PSO_WARM, step 4 2k)",
              done, cap, msNow() - t0, created, present, failed);
  std::lock_guard lk(s.mu);
  s.warmExited = true;
  s.cv.notify_all();
}

// Astra (10:2x) #4: attach and shutdown each run WHOLE under this one mutex - a shutdown never sees a half-made
// attach (state, manifest, threads), and an attach after a shutdown does nothing. Timed: a shutdown waits for an
// attach in progress no longer than its own deadline.
std::timed_mutex g_lifeMu;
std::atomic<void (*)()> g_attachHookForTest{nullptr};   // tests: runs inside the attach, under g_lifeMu
bool g_attached = false;   // under g_lifeMu: an attach completed (its threads exist)
bool g_shut = false;       // under g_lifeMu: a shutdown began (stop set)
bool g_joined = false;     // under g_lifeMu: the workers were joined and the cache released
}  // namespace

bool psoAnyOn() { return censusOn() || recordOn() || warmOn(); }

bool psoShutdown(uint32_t deadlineMs) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadlineMs);
  std::unique_lock life(g_lifeMu, std::defer_lock);
  if (!life.try_lock_until(until)) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[PSO_WARM] exit: the pipeline attach is still running after %u ms (reading the manifest?) - left "
                "running; the process ends without tearing down what it uses (step 4 2k)",
                deadlineMs);
    return false;
  }
  g_shut = true;               // an attach after this does nothing
  if (!g_attached || g_joined) return true;
  State& s = st();
  std::unique_lock lk(s.mu);
  if (!s.stop) {
    // the service's last write may wait for the manifest read until here: a quarter of the deadline (at most
    // 500 ms) is left for the write itself
    s.loadBy = until - std::chrono::milliseconds(std::min<uint32_t>(500, deadlineMs / 4));
    s.stop = true;
    s.cv.notify_all();
  }
  // Astra P2 1 + (10:2x) #5: JOIN, never detach, and never wait unbounded - the warm-up checks `stop` between
  // pipelines (at most the one create in the driver), the service does the last census + manifest write and
  // returns. A worker still busy at the deadline (a stuck driver compile, a stuck disk) is left running with
  // everything it uses: the caller then ends the process without tearing any of it down (quick_exit).
  const bool done = s.cv.wait_until(lk, until, [&] {
    return (s.serviceExited || !s.service.joinable()) && (s.warmExited || !s.warm.joinable());
  });
  if (!done) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[PSO_WARM] exit: %s still busy after %u ms (a driver compile or the manifest write) - left running; "
                "the process ends without tearing down what it uses (step 4 2k)",
                !(s.warmExited || !s.warm.joinable()) ? "the warm-up" : "the pipeline service", deadlineMs);
    return false;
  }
  lk.unlock();
  if (s.warm.joinable()) s.warm.join();         // both returned: these joins do not wait
  if (s.service.joinable()) s.service.join();
  NativePipelineCache::setObserver(nullptr);
  s.cache = nullptr;
  g_joined = true;
  return true;
}

void psoAttachTo(NativePipelineCache* cache, ID3D12RootSignature* root) {
  if (!psoAnyOn() || !cache) return;
  std::lock_guard life(g_lifeMu);   // held to the end: the shutdown sees no attach or a whole one
  if (g_attached || g_shut) return;
  State& s = st();
  // A worker of an earlier attach still joinable (a reset that did not join it): assigning the new threads onto it
  // would be std::terminate(). Never in the game (one attach per process); refuse, loudly, instead.
  if (s.service.joinable() || s.warm.joinable()) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[PSO_WARM] attach refused: a worker of an earlier attach is still joinable "
                                        "(no pipeline census, recording or warm-up this session)");
    return;
  }
  s.cache = cache;
  s.root = root;
  NativePipelineCache::setObserver(&observe);
  if (const auto hook = g_attachHookForTest.load(std::memory_order_acquire)) hook();
  s.warmStarted = warmOn() && s.root;   // the warm thread then reads the manifests; else the service does
  s.service = std::thread(serviceMain);
  if (s.warmStarted) s.warm = std::thread(warmMain);
  g_attached = true;
}

void psoSetAttachHookForTest(void (*hook)()) { g_attachHookForTest.store(hook, std::memory_order_release); }
void psoSetLoadHookForTest(void (*hook)()) { g_loadHookForTest.store(hook, std::memory_order_release); }

void psoWriteManifestForTest() {
  State& s = st();
  for (int i = 0; i < 5000; ++i) {   // after the worker's read (a write before it is skipped by design)
    {
      std::lock_guard lk(s.mu);
      if (s.loadedReady) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  writeManifest("test", false);
}

bool psoWorkersRunningForTest() {
  State& s = st();
  std::lock_guard lk(s.mu);
  return (s.service.joinable() && !s.serviceExited) || (s.warm.joinable() && !s.warmExited);
}

void psoResetForTest() {
  std::lock_guard life(g_lifeMu);
  State& s = st();
  // A shutdown that returned false (a worker still busy at its deadline) leaves the workers running, their
  // std::threads joinable: the game then ends the process (quick_exit), but a test resets and attaches again - and
  // move-assigning the next attach's thread onto a joinable std::thread is std::terminate() (0xC0000409; the
  // intermittent sr_pso_warm_test death of 10-24 00:34). So: stop them and JOIN, without a deadline (a test may wait;
  // the workers never take g_lifeMu). The caller keeps the cache it attached alive until this returns.
  bool joined = false;
  {
    std::lock_guard lk(s.mu);
    s.stop = true;
    s.cv.notify_all();
  }
  if (s.warm.joinable()) { s.warm.join(); joined = true; }
  if (s.service.joinable()) { s.service.join(); joined = true; }
  if (joined && !g_joined)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[PSO_WARM] test reset: a worker a shutdown left running was stopped and "
                                       "joined (psoResetForTest)");
  NativePipelineCache::setObserver(nullptr);
  std::lock_guard lk(s.mu);
  s.stop = false;
  s.serviceExited = s.warmExited = false;
  s.cache = nullptr;
  s.root.Reset();
  s.loaded.clear();
  s.warmList.clear();
  s.loadedReady = s.warmStarted = false;
  s.staleChecked = false;
  s.recordedGen = s.lastCensusCreates = 0;
  s.warmDone.store(false);
  g_attached = g_shut = g_joined = false;
}

}  // namespace sr::gfx
