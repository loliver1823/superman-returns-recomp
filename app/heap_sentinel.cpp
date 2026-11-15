// step4/relcrash: the LFH UserBlocks sentinel. Contract and the why: heap_sentinel.hpp.
#include "heap_sentinel.hpp"

#include "crash_handler.hpp"
#include "log.hpp"

#if defined(_WIN32)
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_set>
#include <vector>

extern "C" uint8_t* g_debugGuestBase;  // kernel/guest_context.cpp: the guest arena's base (process-wide)

namespace sr {
namespace {

constexpr uint32_t kUserBlocksSignature = 0xF0E0D0C0u;  // _HEAP_USERDATA_HEADER.Signature (x64 NT heap)
constexpr size_t kSigOffset = 0x14;
constexpr size_t kHeaderBytes = 0x18;
constexpr size_t kChunk = 64 * 1024;
constexpr uint64_t kMaxRegion = 0x40000000ull;            // 1 GB: no heap segment is larger
constexpr uint64_t kPattern = 0x0011111100101010ull;     // what s4h_wg_rel1's header held (the self-test writes it)

using Clock = std::chrono::steady_clock;

struct Known {
  uintptr_t ub;           // the UserBlocks header (user data of a backend heap block)
  uint64_t subseg;        // its +0 qword when last seen clean
  uint8_t entry[16];      // the backend _HEAP_ENTRY in front of it (ub - 0x10) at adoption
  Clock::time_point lastClean;
};

struct Stats {
  uint64_t scans = 0, checks = 0, adopted = 0, vanished = 0, followed = 0, transient = 0;
  uint32_t detected = 0, suspect = 0;
  double lastScanMs = 0, lastCheckMs = 0;
  uint64_t lastScanMB = 0;
};

bool readSafe(uintptr_t p, void* out, size_t n) {
  SIZE_T got = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(p), out, n, &got) && got == n;
}

// difftest --heap-dump-test mutation seam: 1 = every value counts as plausible (the sentinel can never fire).
std::atomic<int> g_sentinelMutation{0};

bool plausible(uint64_t q) {
  if (g_sentinelMutation.load(std::memory_order_relaxed) == 1) return true;
  return q == 0 || (q >= 0x10000 && q < 0x00007FFFFFFF0000ull && (q & 7) == 0);
}

int envInt(const char* name, int dflt, int lo, int hi) {
  const char* e = std::getenv(name);
  if (!e || !*e) return dflt;
  const int v = std::atoi(e);
  if (v == 1 && std::strcmp(name, "SR_HEAP_SENTINEL") == 0) return dflt;  // "1" = the default period
  return (std::min)(hi, (std::max)(lo, v));
}

void hexLine(char* out, size_t cap, uintptr_t at) {
  uint8_t b[16];
  if (!readSafe(at, b, sizeof(b))) {
    std::snprintf(out, cap, "%016llx  (unreadable)", (unsigned long long)at);
    return;
  }
  std::snprintf(out, cap,
                "%016llx  %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x",
                (unsigned long long)at, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11],
                b[12], b[13], b[14], b[15]);
}

class Sentinel {
 public:
  // difftest: the self-test on the calling thread (no thread, no waiting for the guest arena).
  bool selfTestNow() {
    selfTest();
    return lastSelfTestPass_;
  }

  Sentinel(int checkMs, int scanMs, bool selftest) : checkMs_(checkMs), scanMs_(scanMs), selftest_(selftest) {
    // The scan buffer lives outside the heap (its own VirtualAlloc region, skipped by the scan): a heap copy of
    // headers would be adopted as headers.
    buf_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kChunk + 0x100, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  }
  ~Sentinel() {
    if (buf_) VirtualFree(buf_, 0, MEM_RELEASE);
  }
  Sentinel(const Sentinel&) = delete;
  Sentinel& operator=(const Sentinel&) = delete;

  void run() {
    SetThreadDescription(GetCurrentThread(), L"SR heap sentinel");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (!g_debugGuestBase) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    SR_LOG_BOOT(LogSeverity::Warn,
                "[HEAP_SENTINEL] on (SR_HEAP_SENTINEL): check every %d ms, discovery every %d ms, selftest %d; "
                "guest arena %p excluded",
                checkMs_, scanMs_, selftest_ ? 1 : 0, (void*)g_debugGuestBase);
    auto nextScan = Clock::now();
    auto nextCensus = Clock::now() + std::chrono::seconds(60);
    bool first = true;
    for (;;) {
      const auto now = Clock::now();
      if (now >= nextScan) {
        scan();
        nextScan = Clock::now() + std::chrono::milliseconds(scanMs_);
        if (first) {
          first = false;
          SR_LOG_BOOT(LogSeverity::Warn, "[HEAP_SENTINEL] first discovery: %zu UserBlocks headers in %llu MB, %.1f ms",
                      known_.size(), (unsigned long long)st_.lastScanMB, st_.lastScanMs);
          if (selftest_) selfTest();
        }
      }
      check(0);
      if (Clock::now() >= nextCensus) {
        census();
        nextCensus = Clock::now() + std::chrono::seconds(60);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(checkMs_));
    }
  }

 private:
  bool excluded(uintptr_t a, uint64_t n) const {
    const uintptr_t gb = reinterpret_cast<uintptr_t>(g_debugGuestBase);
    if (gb && a < gb + 0x100010000ull && a + n > gb - 0x100000000ull) return true;  // the guest arena + mirror
    const uintptr_t mine = reinterpret_cast<uintptr_t>(buf_);
    return a <= mine && mine < a + n;
  }

  void scan() {
    const auto t0 = Clock::now();
    uint64_t bytes = 0;
    uintptr_t cur = 0;
    MEMORY_BASIC_INFORMATION m{};
    while (VirtualQuery(reinterpret_cast<void*>(cur), &m, sizeof(m)) == sizeof(m)) {
      const uintptr_t base = reinterpret_cast<uintptr_t>(m.BaseAddress);
      const uint64_t size = m.RegionSize;
      cur = base + size;
      if (m.State != MEM_COMMIT || m.Type != MEM_PRIVATE || (m.Protect & 0xFF) != PAGE_READWRITE ||
          (m.Protect & (PAGE_GUARD | PAGE_WRITECOMBINE | PAGE_NOCACHE)) || size > kMaxRegion || excluded(base, size))
        continue;
      for (uint64_t off = 0; off < size; off += kChunk) {
        const size_t n = static_cast<size_t>((std::min<uint64_t>)(kChunk + 0x20, size - off));
        if (n < kHeaderBytes || !readSafe(base + off, buf_, n)) continue;
        bytes += n;
        const size_t lim = (std::min<size_t>)(kChunk, n - kHeaderBytes + 1);
        for (size_t i = 0; i < lim; i += 16) {
          uint32_t sig;
          std::memcpy(&sig, buf_ + i + kSigOffset, 4);
          if (sig != kUserBlocksSignature) continue;
          uint64_t q;
          std::memcpy(&q, buf_ + i, 8);
          if (q == 0 || !plausible(q)) continue;   // adopt intact, populated headers only
          const uintptr_t ub = base + off + i;
          if (!index_.insert(ub).second) continue;
          Known k{};
          k.ub = ub;
          k.subseg = q;
          if (i >= 16) std::memcpy(k.entry, buf_ + i - 16, 16);
          else if (!readSafe(ub - 16, k.entry, 16)) std::memset(k.entry, 0, 16);
          k.lastClean = Clock::now();
          known_.push_back(k);
          ++st_.adopted;
        }
      }
    }
    ++st_.scans;
    st_.lastScanMB = bytes >> 20;
    st_.lastScanMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  }

  // Returns the number of events this pass. quietUb: the self-test's header - its event is recorded in
  // lastQuietUb_ only (no log, no dump, no count).
  int check(uintptr_t quietUb) {
    const auto t0 = Clock::now();
    int events = 0;
    for (size_t i = 0; i < known_.size();) {
      Known& k = known_[i];
      uint8_t h[kHeaderBytes];
      uint32_t sig = 0;
      if (!readSafe(k.ub, h, sizeof(h)) || (std::memcpy(&sig, h + kSigOffset, 4), sig != kUserBlocksSignature)) {
        ++st_.vanished;
        drop(i);
        continue;
      }
      uint64_t q;
      std::memcpy(&q, h, 8);
      const auto now = Clock::now();
      if (q == k.subseg) { k.lastClean = now; ++i; continue; }
      if (plausible(q)) { k.subseg = q; k.lastClean = now; ++st_.followed; ++i; continue; }
      // Implausible: confirm it persists (an update in flight would be gone in 2 ms).
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      uint64_t q2 = 0;
      uint8_t e[16] = {};
      if (!readSafe(k.ub, &q2, 8) || plausible(q2)) { ++st_.transient; ++i; continue; }
      const bool entryOk = readSafe(k.ub - 16, e, 16) && std::memcmp(e, k.entry, 16) == 0;
      report(k, q2, entryOk, k.ub == quietUb);
      ++events;
      drop(i);
    }
    ++st_.checks;
    st_.lastCheckMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return events;
  }

  void drop(size_t i) {
    index_.erase(known_[i].ub);
    known_[i] = known_.back();
    known_.pop_back();
  }

  void report(const Known& k, uint64_t now, bool entryOk, bool quiet) {
    if (quiet) { lastQuietUb_ = entryOk ? k.ub : 0; return; }
    const char* verdict = entryOk ? "DETECTED" : "SUSPECT (the backend entry changed: the block may be reused)";
    const uint32_t n = entryOk ? ++st_.detected : ++st_.suspect;
    const double cleanAgoMs = std::chrono::duration<double, std::milli>(Clock::now() - k.lastClean).count();
    MEMORY_BASIC_INFORMATION m{};
    VirtualQuery(reinterpret_cast<void*>(k.ub), &m, sizeof(m));
    SR_LOG_BOOT(LogSeverity::Error,
                "[HEAP_SENTINEL] %s #%u: UserBlocks %p SubSegment %016llx -> %016llx; last clean %.0f ms ago "
                "(check #%llu, every %d ms) | region base %p alloc %p size 0x%llx protect 0x%lx",
                verdict, n, (void*)k.ub, (unsigned long long)k.subseg, (unsigned long long)now, cleanAgoMs,
                (unsigned long long)st_.checks, checkMs_, m.BaseAddress, m.AllocationBase,
                (unsigned long long)m.RegionSize, m.Protect);
    char line[128];
    for (uintptr_t a = (k.ub & ~uintptr_t(15)) - 0x40; a < k.ub + 0x40; a += 16) {
      hexLine(line, sizeof(line), a);
      SR_LOG_BOOT(LogSeverity::Error, "[HEAP_SENTINEL]   %s%s", line, a == k.ub ? "   <- UserBlocks header" : "");
    }
    bool& dumped = entryOk ? dumpedDetected_ : dumpedSuspect_;
    if (!dumped) {
      dumped = true;
      const char* path = entryOk ? "superman_returns_heapsentinel.dmp" : "superman_returns_heapsentinel_suspect.dmp";
      const bool ok = writeDiagnosticMinidump(path, /*withHeap=*/true);
      SR_LOG_BOOT(LogSeverity::Error, "[HEAP_SENTINEL] heap minidump -> %s (%s)", path, ok ? "written" : "FAILED");
    }
  }

  void census() {
    SR_LOG_BOOT(LogSeverity::Warn,
                "[HEAP_SENTINEL] census known=%zu adopted=%llu vanished=%llu followed=%llu transient=%llu "
                "detected=%u suspect=%u | scans=%llu last %.0f ms %llu MB | checks=%llu last %.2f ms",
                known_.size(), (unsigned long long)st_.adopted, (unsigned long long)st_.vanished,
                (unsigned long long)st_.followed, (unsigned long long)st_.transient, st_.detected, st_.suspect,
                (unsigned long long)st_.scans, st_.lastScanMs, (unsigned long long)st_.lastScanMB,
                (unsigned long long)st_.checks, st_.lastCheckMs);
  }

  // The activation witness (heap_sentinel.hpp): a private LFH heap, one header clobbered with kPattern, this
  // thread's own check must report it; then the header is restored and the heap destroyed.
  void selfTest() {
    HANDLE heap = HeapCreate(0, 0, 0);
    bool pass = false;
    char why[160] = "no private heap";
    if (heap) {
      std::vector<void*> blocks;
      for (int i = 0; i < 4000; ++i) blocks.push_back(HeapAlloc(heap, 0, 200));
      const uintptr_t victim = reinterpret_cast<uintptr_t>(blocks.back());
      uintptr_t ub = 0;
      for (uintptr_t p = victim & ~uintptr_t(15); p + 0x100000 > victim && p >= 16; p -= 16) {
        uint32_t sig = 0;
        if (!readSafe(p + kSigOffset, &sig, 4)) break;
        if (sig == kUserBlocksSignature) { ub = p; break; }
      }
      if (!ub) {
        std::snprintf(why, sizeof(why), "no UserBlocks signature below %p", (void*)victim);
      } else {
        scan();  // adopt the private heap's headers
        if (!index_.count(ub)) {
          std::snprintf(why, sizeof(why), "discovery did not adopt the test header %p", (void*)ub);
        } else {
          uint64_t orig;
          std::memcpy(&orig, reinterpret_cast<void*>(ub), 8);
          std::memcpy(reinterpret_cast<void*>(ub), &kPattern, 8);
          lastQuietUb_ = 0;
          check(ub);
          std::memcpy(reinterpret_cast<void*>(ub), &orig, 8);
          pass = lastQuietUb_ == ub;
          std::snprintf(why, sizeof(why), "header %p clobbered with %016llx: %s", (void*)ub,
                        (unsigned long long)kPattern, pass ? "reported as DETECTED" : "NOT reported");
        }
      }
      for (void* b : blocks) HeapFree(heap, 0, b);
      HeapDestroy(heap);
      for (size_t i = 0; i < known_.size();) {  // forget the destroyed heap's headers
        MEMORY_BASIC_INFORMATION m{};
        if (VirtualQuery(reinterpret_cast<void*>(known_[i].ub), &m, sizeof(m)) != sizeof(m) ||
            m.State != MEM_COMMIT) {
          drop(i);
          continue;
        }
        ++i;
      }
    }
    lastSelfTestPass_ = pass;
    SR_LOG_BOOT(pass ? LogSeverity::Warn : LogSeverity::Error, "[HEAP_SENTINEL] selftest %s: %s",
                pass ? "PASS" : "FAIL", why);
  }

  const int checkMs_, scanMs_;
  const bool selftest_;
  uint8_t* buf_ = nullptr;
  std::vector<Known> known_;
  std::unordered_set<uintptr_t> index_;
  Stats st_;
  bool dumpedDetected_ = false, dumpedSuspect_ = false;
  uintptr_t lastQuietUb_ = 0;
  bool lastSelfTestPass_ = false;
};

}  // namespace

bool heapSentinelSelfTestForTest(int mutation) {
  g_sentinelMutation.store(mutation, std::memory_order_relaxed);
  Sentinel s(100, 5000, true);
  const bool pass = s.selfTestNow();
  g_sentinelMutation.store(0, std::memory_order_relaxed);
  return pass;
}

void startHeapSentinel() {
  const char* e = std::getenv("SR_HEAP_SENTINEL");
  if (!e || !*e || e[0] == '0') return;
  const int checkMs = envInt("SR_HEAP_SENTINEL", 100, 10, 10000);
  const int scanMs = envInt("SR_HEAP_SENTINEL_SCAN_MS", 5000, 500, 600000);
  const char* st = std::getenv("SR_HEAP_SENTINEL_SELFTEST");
  const bool selftest = st && st[0] == '1';
  std::thread([checkMs, scanMs, selftest] {
    static Sentinel s(checkMs, scanMs, selftest);  // lives for the process (never joined)
    s.run();
  }).detach();
}

}  // namespace sr

#else
namespace sr {
void startHeapSentinel() {}
bool heapSentinelSelfTestForTest(int) { return false; }
}  // namespace sr
#endif
