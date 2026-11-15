/**
 * sr_import_census.h - superman-recomp logic audit (docs/LOGIC_AUDIT.md 2): the per-export call census of a Xenia
 * build, the oracle half of the recomp's SR_AUDIT_IMPORTS (kernel/import_audit.cpp; the same JSON schema,
 * sr-import-audit/2, plus each export's module and ordinal).
 *
 * RECORDS ONLY. It never changes a call, a register, a result or the order of anything the guest sees. Its only
 * effects are reading the registers the export trampoline already holds, a few relaxed atomics and a side thread
 * that writes a file. With XENIA_IMPORT_CENSUS unset, each export trampoline pays one test of a function-local
 * static pointer, which is null.
 *
 *   XENIA_IMPORT_CENSUS=<path.json>   on; the census goes to that file
 *   XENIA_IMPORT_CENSUS_PERIOD=<s>    write interval in seconds (default 10); a final write runs at exit
 *
 * Per export, as in the recomp:
 *   ENTRY (before the export's C++ body runs): entries, the caller-lr histogram (16 bins), the first 8 calls' r3-r8.
 *   RETURN (after result.Store): returns, the result histogram (8 bins), those first calls' results. A void export
 *   is marked "void": true and has no results.
 */
#ifndef XENIA_KERNEL_UTIL_SR_IMPORT_CENSUS_H_
#define XENIA_KERNEL_UTIL_SR_IMPORT_CENSUS_H_

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xe {
namespace sr_census {

constexpr int kLrBins = 16;
constexpr int kResBins = 8;
constexpr int kFirst = 8;

struct Bin {
  std::atomic<uint64_t> key{0};  // value + 1 (0 = free)
  std::atomic<uint64_t> n{0};
};
struct FirstCall {
  uint32_t lr = 0, a[6] = {}, result = 0;
};
struct Slot {
  const char* name = nullptr;
  uint32_t module = 0, ordinal = 0;
  std::atomic<bool> is_void{false};
  std::atomic<uint64_t> entries{0}, returns{0};
  Bin lr[kLrBins];
  std::atomic<uint64_t> lr_other{0};
  Bin res[kResBins];
  std::atomic<uint64_t> res_other{0};
  std::atomic<uint32_t> first_n{0};
  FirstCall first[kFirst];
  std::atomic<uint8_t> first_entered[kFirst] = {};
  std::atomic<uint8_t> first_returned[kFirst] = {};
};

inline void Bump(Bin* bins, int n, std::atomic<uint64_t>& other, uint32_t value) {
  const uint64_t key = uint64_t(value) + 1;
  for (int i = 0; i < n; ++i) {
    uint64_t k = bins[i].key.load(std::memory_order_acquire);
    if (k == 0) {
      uint64_t expected = 0;
      k = bins[i].key.compare_exchange_strong(expected, key, std::memory_order_acq_rel) ? key : expected;
    }
    if (k == key) {
      bins[i].n.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  other.fetch_add(1, std::memory_order_relaxed);
}

class Census {
 public:
  // nullptr unless XENIA_IMPORT_CENSUS names a file. Never destroyed (exit handlers may run after statics die).
  static Census* Get() {
    static Census* c = [] () -> Census* {
      const char* p = std::getenv("XENIA_IMPORT_CENSUS");
      if (!p || !p[0]) return nullptr;
      Census* n = new Census();
      n->path_ = p;
      if (const char* s = std::getenv("XENIA_IMPORT_CENSUS_PERIOD")) {
        const int v = std::atoi(s);
        if (v > 0) n->period_s_ = unsigned(v);
      }
      n->t0_ = std::chrono::steady_clock::now();
      n->writer_ = std::thread([n] { n->Loop(); });
      std::atexit([] { Get()->Final(); });
      std::at_quick_exit([] { Get()->Final(); });
      return n;
    }();
    return c;
  }
  Slot* Register(const char* name, uint32_t module, uint32_t ordinal) {
    Slot* s = new Slot();
    s->name = name;
    s->module = module;
    s->ordinal = ordinal;
    std::lock_guard<std::mutex> lk(slots_m_);
    slots_.push_back(s);
    return s;
  }
  bool Write() {
    std::vector<Slot*> slots;
    {
      std::lock_guard<std::mutex> lk(slots_m_);
      slots = slots_;
    }
    std::lock_guard<std::mutex> wl(write_m_);
    std::sort(slots.begin(), slots.end(), [](Slot* a, Slot* b) { return std::strcmp(a->name, b->name) < 0; });
    std::string o;
    char b[320];
    std::snprintf(b, sizeof b,
                  "{\"schema\": \"sr-import-audit/2\", \"source\": \"xenia 0bd090dbe + sr_import_census\", "
                  "\"elapsed_s\": %.1f, \"final\": %s, \"exports\": [",
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count(),
                  final_.load() ? "true" : "false");
    o += b;
    bool first_rec = true;
    for (Slot* s : slots) {
      const uint64_t entries = s->entries.load(std::memory_order_relaxed);
      if (!entries) continue;  // registered when first called, so this is only a race at start
      o += first_rec ? "\n" : ",\n";
      first_rec = false;
      std::snprintf(b, sizeof b,
                    " {\"name\": \"%s\", \"module\": %u, \"ordinal\": %u, \"void\": %s, \"entries\": %llu, "
                    "\"returns\": %llu, \"unresolved\": 0, \"lr\": [",
                    s->name, s->module, s->ordinal, s->is_void.load() ? "true" : "false",
                    (unsigned long long)entries, (unsigned long long)s->returns.load(std::memory_order_relaxed));
      o += b;
      AppendBins(o, s->lr, kLrBins);
      std::snprintf(b, sizeof b, "], \"lr_other\": %llu, \"results\": [",
                    (unsigned long long)s->lr_other.load(std::memory_order_relaxed));
      o += b;
      AppendBins(o, s->res, kResBins);
      std::snprintf(b, sizeof b, "], \"results_other\": %llu, \"first\": [",
                    (unsigned long long)s->res_other.load(std::memory_order_relaxed));
      o += b;
      int written = 0;
      for (int i = 0; i < kFirst; ++i) {
        if (!s->first_entered[i].load(std::memory_order_acquire)) continue;
        const FirstCall& f = s->first[i];
        std::snprintf(b, sizeof b,
                      "%s{\"lr\": \"%08X\", \"args\": [\"%08X\", \"%08X\", \"%08X\", \"%08X\", \"%08X\", \"%08X\"], "
                      "\"result\": ",
                      written++ ? ", " : "", f.lr, f.a[0], f.a[1], f.a[2], f.a[3], f.a[4], f.a[5]);
        o += b;
        if (s->first_returned[i].load(std::memory_order_acquire) && !s->is_void.load()) {
          std::snprintf(b, sizeof b, "\"%08X\"}", f.result);
          o += b;
        } else {
          o += "null}";
        }
      }
      o += "]}";
    }
    o += "\n]}\n";
    const std::string tmp = path_ + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(o.data(), 1, o.size(), f) == o.size();
    std::fclose(f);
    std::remove(path_.c_str());
    return ok && std::rename(tmp.c_str(), path_.c_str()) == 0;
  }

 private:
  static void AppendBins(std::string& o, Bin* bins, int n) {
    std::vector<std::pair<uint32_t, uint64_t>> v;
    for (int i = 0; i < n; ++i)
      if (uint64_t k = bins[i].key.load(std::memory_order_acquire))
        v.emplace_back(uint32_t(k - 1), bins[i].n.load(std::memory_order_relaxed));
    std::sort(v.begin(), v.end(), [](auto& x, auto& y) { return x.second > y.second; });
    char b[48];
    for (size_t i = 0; i < v.size(); ++i) {
      std::snprintf(b, sizeof b, "%s[\"%08X\", %llu]", i ? ", " : "", v[i].first, (unsigned long long)v[i].second);
      o += b;
    }
  }
  void Loop() {
    std::unique_lock<std::mutex> lk(stop_m_);
    while (!stop_) {
      stop_cv_.wait_for(lk, std::chrono::seconds(period_s_), [this] { return stop_; });
      if (stop_) break;
      lk.unlock();
      Write();
      lk.lock();
    }
  }
  void Final() {
    if (final_.exchange(true)) return;
    {
      std::lock_guard<std::mutex> lk(stop_m_);
      stop_ = true;
    }
    stop_cv_.notify_all();
    if (writer_.joinable() && writer_.get_id() != std::this_thread::get_id()) writer_.join();
    Write();
  }

  std::string path_;
  unsigned period_s_ = 10;
  std::chrono::steady_clock::time_point t0_;
  std::mutex slots_m_, write_m_, stop_m_;
  std::condition_variable stop_cv_;
  bool stop_ = false;
  std::atomic<bool> final_{false};
  std::vector<Slot*> slots_;
  std::thread writer_;
};

// One slot per export (one per trampoline instantiation), created at the export's first call; null when off.
template <uint32_t MODULE, uint32_t ORDINAL>
inline Slot* SlotFor(const char* name) {
  static Slot* s = Census::Get() ? Census::Get()->Register(name, MODULE, ORDINAL) : nullptr;
  return s;
}

// The registers the trampoline already holds: lr = the caller's return address (the import thunk is a leaf),
// r3-r8 = the first six arguments.
template <typename Ctx>
inline int Enter(Slot* s, const Ctx* c) {
  s->entries.fetch_add(1, std::memory_order_relaxed);
  Bump(s->lr, kLrBins, s->lr_other, uint32_t(c->lr));
  const uint32_t i = s->first_n.fetch_add(1, std::memory_order_relaxed);
  if (i >= uint32_t(kFirst)) return -1;
  s->first[i].lr = uint32_t(c->lr);
  for (int k = 0; k < 6; ++k) s->first[i].a[k] = uint32_t(c->r[3 + k]);
  s->first_entered[i].store(1, std::memory_order_release);
  return int(i);
}
inline void Leave(Slot* s, int first, uint32_t result, bool has_result) {
  s->returns.fetch_add(1, std::memory_order_relaxed);
  if (has_result) {
    Bump(s->res, kResBins, s->res_other, result);
  } else {
    s->is_void.store(true, std::memory_order_relaxed);
  }
  if (first >= 0) {
    s->first[first].result = result;
    s->first_returned[first].store(1, std::memory_order_release);
  }
}

}  // namespace sr_census
}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_SR_IMPORT_CENSUS_H_
