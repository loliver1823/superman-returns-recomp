// step4/audit: SR_AUDIT_IMPORTS - the per-import call census of an audit build. Contract in import_audit.hpp.
// Compiled only when the CMake option SR_AUDIT_IMPORTS is ON (never in the release configuration).
#include "import_audit.hpp"

#include "guest_context.hpp"
#include "../app/log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sr {
namespace audit {
namespace {

constexpr int kSlots = 1024;    // 214 function imports + test names; open addressing on the name pointer
constexpr int kLrBins = 16;     // distinct caller lrs per export (counted at entry); the rest count as lr_other
constexpr int kResBins = 8;     // distinct results per export (counted at return); the rest count as results_other
constexpr int kFirst = 8;       // the first calls' arguments (entry) and result (return)

struct Bin {
  std::atomic<uint64_t> key{0};   // value + 1 (0 = free)
  std::atomic<uint64_t> n{0};
};
struct FirstCall {
  uint32_t lr = 0, a[6] = {}, result = 0;
};
struct Slot {
  std::atomic<const char*> name{nullptr};
  std::atomic<uint64_t> entries{0};
  std::atomic<uint64_t> returns{0};
  std::atomic<uint64_t> unresolved{0};
  Bin lr[kLrBins];
  std::atomic<uint64_t> lrOther{0};
  Bin res[kResBins];
  std::atomic<uint64_t> resOther{0};
  std::atomic<uint32_t> firstN{0};
  FirstCall first[kFirst];
  std::atomic<uint8_t> firstEntered[kFirst];
  std::atomic<uint8_t> firstReturned[kFirst];
};
Slot g_slots[kSlots];
std::atomic<uint64_t> g_tableFull{0};

// PATHS: the file opens in call order (import_audit.hpp).
constexpr int kPathRecs = 16384;
constexpr int kPathLen = 160;
struct PathRec {
  const char* name = nullptr;
  uint32_t lr = 0, r4 = 0, result = 0;
  char path[kPathLen] = {};
  std::atomic<uint8_t> entered{0};
  std::atomic<uint8_t> returned{0};
};
PathRec g_paths[kPathRecs];
std::atomic<uint32_t> g_pathN{0};
std::atomic<uint64_t> g_pathDropped{0};

// The OBJECT_ATTRIBUTES argument of a path export, or 0. Names are compared by text (the glue passes literals).
uint32_t pathOaArg(const char* name, uint32_t r3, uint32_t r5) {
  if (name[0] != 'N' || name[1] != 't') return 0;
  if (std::strcmp(name, "NtCreateFile") == 0 || std::strcmp(name, "NtOpenFile") == 0) return r5;
  if (std::strcmp(name, "NtQueryFullAttributesFile") == 0) return r3;
  return 0;
}

// Xbox OBJECT_ATTRIBUTES: +0 RootDirectory, +4 ANSI_STRING*; ANSI_STRING: +0 be16 Length, +4 Buffer. Range-checked;
// "" when unreadable. A handle-relative name is prefixed "<root XXXXXXXX>".
void readOaPath(uint32_t oa, char* out, size_t cap) {
  namespace K = sr::kernel;
  out[0] = 0;
  // A diagnostic's host read: hostRangeReadableCached (committed, readable, not a guard page; guest_context.hpp).
  auto ok = [](uint32_t va, uint32_t n) {
    if (va == 0 || uint64_t(va) + n > 0x100000000ull || !K::isGuestVAInRange(va) || !K::isGuestVAInRange(va + n - 1))
      return false;
    const uint8_t* h = K::guestVAToHost(va);
    return h && K::hostRangeReadableCached(h, n);
  };
  if (!ok(oa, 12)) return;
  const uint32_t root = K::readGuestU32BE(oa);
  const uint32_t nameVA = K::readGuestU32BE(oa + 4);
  if (!ok(nameVA, 8)) return;
  const uint32_t len = K::readGuestU32BE(nameVA) >> 16;
  const uint32_t buf = K::readGuestU32BE(nameVA + 4);
  if (!len || !ok(buf, len)) return;
  const uint8_t* p = K::guestVAToHost(buf);
  if (!p) return;
  size_t o = 0;
  if (root != 0 && root != 0xFFFFFFFDu) o = size_t(std::snprintf(out, cap, "<root %08X>", root));
  for (uint32_t i = 0; i < len && o + 1 < cap; ++i) out[o++] = char(p[i]);
  out[o] = 0;
}

void appendJsonString(std::string& o, const char* s) {
  o += '"';
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
    if (*p == '"' || *p == '\\') { o += '\\'; o += char(*p); }
    else if (*p < 0x20 || *p >= 0x7F) { char b[8]; std::snprintf(b, sizeof b, "\\u%04X", unsigned(*p)); o += b; }
    else o += char(*p);
  }
  o += '"';
}
std::atomic<bool> g_started{false};
std::once_flag g_startOnce;
std::mutex g_writeMutex;
std::chrono::steady_clock::time_point g_t0;

// The writer's state lives on the heap and is never destroyed: the final flush can run from at_quick_exit (no static
// destructors) or from atexit (before the statics constructed earlier are destroyed), and the writer thread is
// stopped and joined there, so no census code runs during CRT teardown.
struct Writer {
  std::mutex m;
  std::condition_variable cv;
  bool stop = false;
  std::thread t;
  std::string path;
  unsigned periodS = 10;
};
Writer* g_writer = nullptr;
std::atomic<bool> g_finalDone{false};

Slot* slotFor(const char* name) {
  const size_t h = size_t((uint64_t(uintptr_t(name)) * 0x9E3779B97F4A7C15ull) >> 54);
  for (int i = 0; i < kSlots; ++i) {
    Slot& s = g_slots[(h + size_t(i)) & (kSlots - 1)];
    const char* k = s.name.load(std::memory_order_acquire);
    if (k == name) return &s;
    if (!k) {
      const char* expected = nullptr;
      if (s.name.compare_exchange_strong(expected, name, std::memory_order_acq_rel) || expected == name) return &s;
    }
  }
  return nullptr;
}

void bump(Bin* bins, int nBins, std::atomic<uint64_t>& other, uint32_t value) {
  const uint64_t key = uint64_t(value) + 1;
  for (int i = 0; i < nBins; ++i) {
    uint64_t k = bins[i].key.load(std::memory_order_acquire);
    if (k == 0) {
      uint64_t expected = 0;
      if (bins[i].key.compare_exchange_strong(expected, key, std::memory_order_acq_rel)) k = key;
      else k = expected;
    }
    if (k == key) {
      bins[i].n.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  other.fetch_add(1, std::memory_order_relaxed);
}

std::string defaultOutPath() {
  const char* e = std::getenv("SR_AUDIT_IMPORTS_OUT");
  if (e && e[0]) return e;
#ifdef _WIN32
  char tmp[MAX_PATH] = {};
  const DWORD n = GetTempPathA(MAX_PATH, tmp);
  std::string dir = (n && n < MAX_PATH) ? std::string(tmp) : std::string(".\\");
  dir += "SupermanRecomp";
  CreateDirectoryA(dir.c_str(), nullptr);
  return dir + "\\import_audit_" + std::to_string(GetCurrentProcessId()) + ".json";
#else
  return "import_audit.json";
#endif
}

void finalFlush() {
  if (g_finalDone.exchange(true) || !g_writer) return;
  {
    std::lock_guard<std::mutex> lk(g_writer->m);
    g_writer->stop = true;
  }
  g_writer->cv.notify_all();
  if (g_writer->t.joinable() && g_writer->t.get_id() != std::this_thread::get_id()) g_writer->t.join();
  importAuditWriteJson(g_writer->path.c_str());
}

void writerLoop() {
  std::unique_lock<std::mutex> lk(g_writer->m);
  while (!g_writer->stop) {
    g_writer->cv.wait_for(lk, std::chrono::seconds(g_writer->periodS), [] { return g_writer->stop; });
    if (g_writer->stop) break;
    lk.unlock();
    importAuditWriteJson(g_writer->path.c_str());
    lk.lock();
  }
}

void start() {
  g_t0 = std::chrono::steady_clock::now();
  g_writer = new Writer;
  g_writer->path = defaultOutPath();
  if (const char* p = std::getenv("SR_AUDIT_IMPORTS_PERIOD")) {
    const int v = std::atoi(p);
    if (v > 0) g_writer->periodS = unsigned(v);
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[IMPORT_AUDIT] census on (SR_AUDIT_IMPORTS build): every import call counted per export (entries "
              "before the export, returns after it, caller lr, result, first %d calls); JSON every %u s and at exit "
              "(atexit + at_quick_exit) -> %s",
              kFirst, g_writer->periodS, g_writer->path.c_str());
  g_writer->t = std::thread(writerLoop);
  std::atexit(finalFlush);
  std::at_quick_exit(finalFlush);
}

void hex8(std::string& o, uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "\"%08X\"", v);
  o += b;
}

struct FirstOut {
  FirstCall c;
  bool returned;
};
struct Agg {
  uint64_t entries = 0, returns = 0, unresolved = 0, lrOther = 0, resOther = 0;
  std::map<uint32_t, uint64_t> lr, res;
  std::vector<FirstOut> first;
};

void appendBins(std::string& o, const std::map<uint32_t, uint64_t>& m) {
  std::vector<std::pair<uint32_t, uint64_t>> v(m.begin(), m.end());
  std::sort(v.begin(), v.end(), [](auto& x, auto& y) { return x.second > y.second; });
  char b[48];
  for (size_t i = 0; i < v.size(); ++i) {
    o += i ? ", [" : "[";
    hex8(o, v[i].first);
    std::snprintf(b, sizeof b, ", %llu]", (unsigned long long)v[i].second);
    o += b;
  }
}

}  // namespace

ImportAuditToken importEnter(const char* name, uint32_t lr, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                             uint32_t r7, uint32_t r8) {
  ImportAuditToken t;
  if (!name) return t;
  if (!g_started.load(std::memory_order_acquire)) {
    std::call_once(g_startOnce, start);
    g_started.store(true, std::memory_order_release);
  }
  Slot* s = slotFor(name);
  if (!s) {
    g_tableFull.fetch_add(1, std::memory_order_relaxed);
    return t;
  }
  t.slot = s;
  s->entries.fetch_add(1, std::memory_order_relaxed);
  bump(s->lr, kLrBins, s->lrOther, lr);
  const uint32_t i = s->firstN.fetch_add(1, std::memory_order_relaxed);
  if (i < uint32_t(kFirst)) {
    s->first[i].lr = lr;
    const uint32_t a[6] = {r3, r4, r5, r6, r7, r8};
    std::memcpy(s->first[i].a, a, sizeof a);
    s->firstEntered[i].store(1, std::memory_order_release);
    t.first = int(i);
  }
  if (const uint32_t oa = pathOaArg(name, r3, r5)) {
    const uint32_t k = g_pathN.fetch_add(1, std::memory_order_relaxed);
    if (k < uint32_t(kPathRecs)) {
      PathRec& p = g_paths[k];
      p.name = name;
      p.lr = lr;
      p.r4 = r4;
      readOaPath(oa, p.path, sizeof p.path);
      p.entered.store(1, std::memory_order_release);
      t.path = int(k);
    } else {
      g_pathDropped.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return t;
}

void importLeave(const ImportAuditToken& t, uint32_t result, bool resolved) {
  Slot* s = static_cast<Slot*>(t.slot);
  if (!s) return;
  s->returns.fetch_add(1, std::memory_order_relaxed);
  if (!resolved) s->unresolved.fetch_add(1, std::memory_order_relaxed);
  bump(s->res, kResBins, s->resOther, result);
  if (t.first >= 0 && t.first < kFirst) {
    s->first[t.first].result = result;
    s->firstReturned[t.first].store(1, std::memory_order_release);
  }
  if (t.path >= 0 && t.path < kPathRecs) {
    g_paths[t.path].result = result;
    g_paths[t.path].returned.store(1, std::memory_order_release);
  }
}

bool importAuditWriteJson(const char* path) {
  if (!path || !path[0]) return false;
  std::lock_guard<std::mutex> lk(g_writeMutex);
  // One record per export NAME (two call sites may pass different literal pointers for one name).
  std::map<std::string, Agg> agg;
  for (Slot& s : g_slots) {
    const char* n = s.name.load(std::memory_order_acquire);
    if (!n) continue;
    Agg& a = agg[n];
    a.entries += s.entries.load(std::memory_order_relaxed);
    a.returns += s.returns.load(std::memory_order_relaxed);
    a.unresolved += s.unresolved.load(std::memory_order_relaxed);
    a.lrOther += s.lrOther.load(std::memory_order_relaxed);
    a.resOther += s.resOther.load(std::memory_order_relaxed);
    for (Bin& b : s.lr)
      if (uint64_t k = b.key.load(std::memory_order_acquire)) a.lr[uint32_t(k - 1)] += b.n.load(std::memory_order_relaxed);
    for (Bin& b : s.res)
      if (uint64_t k = b.key.load(std::memory_order_acquire)) a.res[uint32_t(k - 1)] += b.n.load(std::memory_order_relaxed);
    for (int i = 0; i < kFirst; ++i)
      if (s.firstEntered[i].load(std::memory_order_acquire) && a.first.size() < size_t(kFirst))
        a.first.push_back({s.first[i], s.firstReturned[i].load(std::memory_order_acquire) != 0});
  }
  const double elapsed = g_started.load(std::memory_order_acquire)
                             ? std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count()
                             : 0.0;
  std::string o;
  o.reserve(64 * 1024);
  char b[320];
  std::snprintf(b, sizeof b,
                "{\"schema\": \"sr-import-audit/2\", \"elapsed_s\": %.1f, \"final\": %s, \"table_full\": %llu, ",
                elapsed, g_finalDone.load() ? "true" : "false",
                (unsigned long long)g_tableFull.load(std::memory_order_relaxed));
  o += b;
#ifdef _WIN32
  std::snprintf(b, sizeof b, "\"pid\": %lu, ", (unsigned long)GetCurrentProcessId());
  o += b;
#endif
  o += "\"exports\": [";
  bool firstRec = true;
  for (auto& [name, a] : agg) {
    o += firstRec ? "\n" : ",\n";
    firstRec = false;
    std::snprintf(b, sizeof b, " {\"name\": \"%s\", \"entries\": %llu, \"returns\": %llu, \"unresolved\": %llu, \"lr\": [",
                  name.c_str(), (unsigned long long)a.entries, (unsigned long long)a.returns,
                  (unsigned long long)a.unresolved);
    o += b;
    appendBins(o, a.lr);
    std::snprintf(b, sizeof b, "], \"lr_other\": %llu, \"results\": [", (unsigned long long)a.lrOther);
    o += b;
    appendBins(o, a.res);
    std::snprintf(b, sizeof b, "], \"results_other\": %llu, \"first\": [", (unsigned long long)a.resOther);
    o += b;
    for (size_t i = 0; i < a.first.size(); ++i) {
      const FirstOut& f = a.first[i];
      o += i ? ", {\"lr\": " : "{\"lr\": ";
      hex8(o, f.c.lr);
      o += ", \"args\": [";
      for (int k = 0; k < 6; ++k) {
        if (k) o += ", ";
        hex8(o, f.c.a[k]);
      }
      o += "], \"result\": ";
      if (f.returned) hex8(o, f.c.result);
      else o += "null";
      o += "}";
    }
    o += "]}";
  }
  // PATHS in call order (a record still entering - its path not yet published - ends the list for this snapshot)
  const uint32_t nPaths = std::min<uint32_t>(g_pathN.load(std::memory_order_acquire), uint32_t(kPathRecs));
  std::snprintf(b, sizeof b, "\n], \"paths_dropped\": %llu, \"paths\": [",
                (unsigned long long)g_pathDropped.load(std::memory_order_relaxed));
  o += b;
  for (uint32_t k = 0; k < nPaths; ++k) {
    const PathRec& p = g_paths[k];
    if (!p.entered.load(std::memory_order_acquire)) break;
    std::snprintf(b, sizeof b, "%s{\"seq\": %u, \"export\": \"%s\", \"lr\": ", k ? ",\n " : "\n ", k, p.name);
    o += b;
    hex8(o, p.lr);
    o += ", \"r4\": ";
    hex8(o, p.r4);
    o += ", \"path\": ";
    appendJsonString(o, p.path);
    o += ", \"result\": ";
    if (p.returned.load(std::memory_order_acquire)) hex8(o, p.result);
    else o += "null";
    o += "}";
  }
  o += "\n]}\n";
  const std::string tmp = std::string(path) + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(o.data(), 1, o.size(), f) == o.size();
  std::fclose(f);
  if (!ok) return false;
#ifdef _WIN32
  return MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
  return std::rename(tmp.c_str(), path) == 0;
#endif
}

void importAuditResetForTest() {
  std::lock_guard<std::mutex> lk(g_writeMutex);
  for (Slot& s : g_slots) {
    s.name.store(nullptr, std::memory_order_relaxed);
    s.entries.store(0, std::memory_order_relaxed);
    s.returns.store(0, std::memory_order_relaxed);
    s.unresolved.store(0, std::memory_order_relaxed);
    for (Bin& b : s.lr) { b.key.store(0, std::memory_order_relaxed); b.n.store(0, std::memory_order_relaxed); }
    for (Bin& b : s.res) { b.key.store(0, std::memory_order_relaxed); b.n.store(0, std::memory_order_relaxed); }
    s.lrOther.store(0, std::memory_order_relaxed);
    s.resOther.store(0, std::memory_order_relaxed);
    s.firstN.store(0, std::memory_order_relaxed);
    for (auto& d : s.firstEntered) d.store(0, std::memory_order_relaxed);
    for (auto& d : s.firstReturned) d.store(0, std::memory_order_relaxed);
  }
  g_tableFull.store(0, std::memory_order_relaxed);
  for (PathRec& p : g_paths) {
    p.entered.store(0, std::memory_order_relaxed);
    p.returned.store(0, std::memory_order_relaxed);
  }
  g_pathN.store(0, std::memory_order_relaxed);
  g_pathDropped.store(0, std::memory_order_relaxed);
}

}  // namespace audit
}  // namespace sr
