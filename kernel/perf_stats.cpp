// iter-464 perf lane: frame events + per-thread CPU telemetry (SR_PERF_STATS=1).
// Contract and file formats: kernel/perf_stats.hpp, docs/PERFORMANCE.md.
//
// Observe-only. The two hooks (guest VdSwap, host Present) push a QPC stamp into a
// lock-free ring; one telemetry thread drains it to frames.csv, samples every
// thread's CPU time once per second (Toolhelp + GetThreadTimes + QueryThreadCycleTime,
// all fault-free queries) and logs a [PERF] summary every 10 s.

#include "perf_stats.hpp"

#include "guest_context.hpp"
#include "../app/log.hpp"

#include <windows.h>
#include <dxgi1_4.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" sr::kernel::GuestThreadSlot g_debugThreadRegistry[64];

namespace sr::perf {

namespace {
bool envOn(const char* n) {
  const char* e = std::getenv(n);
  return e && e[0] == '1';
}
}  // namespace

bool g_statsOn = envOn("SR_PERF_STATS");
std::atomic<uint32_t> g_mirrorFaults{0};
uint64_t g_mirrorFaultRip[kMirrorFaultRips] = {};

namespace {

// ------------------------------------------------------------------ clock
struct Qpc {
  LARGE_INTEGER f{}, t0{};
  int64_t epochMs0 = 0;
  Qpc() {
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    epochMs0 = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
  }
};
Qpc& qpc() {
  static Qpc q;
  return q;
}

// ------------------------------------------------------------------ event ring
struct Ev {
  std::atomic<uint64_t> seq{0};  // ticket + 1 once the slot is written
  uint64_t us = 0;
  uint32_t kind = 0;  // 'S' | 'P'
  uint32_t n = 0;
};
constexpr uint64_t kRing = 1u << 14;
Ev g_ring[kRing];
std::atomic<uint64_t> g_wr{0};
uint64_t g_rd = 0;  // telemetry thread only
std::atomic<uint32_t> g_swaps{0}, g_presents{0};
std::atomic<uint64_t> g_dropped{0};

// Producers never wait. The reader drains every 100 ms, so it is lapped only above
// ~160k events/s; a lapped slot is detected by its sequence and counted as dropped.
void push(uint32_t kind, uint32_t n) {
  const uint64_t t = g_wr.fetch_add(1, std::memory_order_relaxed);
  Ev& e = g_ring[t & (kRing - 1)];
  e.us = nowUs();
  e.kind = kind;
  e.n = n;
  e.seq.store(t + 1, std::memory_order_release);
}

std::string g_dir;
std::once_flag g_dirOnce;

void ensureDir() {
  std::call_once(g_dirOnce, [] {
    const char* d = std::getenv("SR_PERF_DIR");
    if (d && d[0]) {
      g_dir = d;
    } else {
      char b[96];
      std::snprintf(b, sizeof(b), "archive/v1_perf/run_%lu", (unsigned long)GetCurrentProcessId());
      g_dir = b;
    }
    // Create every component (CreateDirectoryA is one level at a time).
    std::string acc;
    for (size_t i = 0; i < g_dir.size(); ++i) {
      const char c = g_dir[i];
      acc.push_back(c);
      if ((c == '/' || c == '\\') && acc.size() > 1 && acc[acc.size() - 2] != ':')
        CreateDirectoryA(acc.c_str(), nullptr);
    }
    CreateDirectoryA(g_dir.c_str(), nullptr);
  });
}

// ------------------------------------------------------------------ threads
struct ThreadRec {
  HANDLE h = nullptr;
  bool alive = false;
  char name[48] = {};
};

void threadName(DWORD tid, HANDLE h, char* out, size_t cap) {
  for (int i = 0; i < 64; ++i) {
    if (g_debugThreadRegistry[i].tid == tid && g_debugThreadRegistry[i].name[0]) {
      std::snprintf(out, cap, "guest:%s", g_debugThreadRegistry[i].name);
      return;
    }
  }
  for (int i = 0; i < 64; ++i) {
    if (g_debugThreadRegistry[i].tid == tid) {
      std::snprintf(out, cap, "guest:slot%d", i);
      return;
    }
  }
  PWSTR desc = nullptr;
  if (h && SUCCEEDED(GetThreadDescription(h, &desc)) && desc && desc[0]) {
    WideCharToMultiByte(CP_UTF8, 0, desc, -1, out, int(cap), nullptr, nullptr);
    LocalFree(desc);
    for (char* p = out; *p; ++p)
      if (*p == ',' || *p == '\n') *p = ' ';
    return;
  }
  if (desc) LocalFree(desc);
  std::snprintf(out, cap, "-");
}


// ------------------------------------------------------------------ memory
// mem.csv (every second): the process's private bytes, working set, peak working set and
// commit, plus the GPU's local (VRAM) / non-local (system) usage BY THIS PROCESS
// (IDXGIAdapter3::QueryVideoMemoryInfo).
struct GpuMem {
  IDXGIAdapter3* a = nullptr;
  bool tried = false;
  void query(double& localMb, double& nonLocalMb) {
    localMb = nonLocalMb = -1;
    if (!tried) {
      tried = true;
      IDXGIFactory1* f = nullptr;
      if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) && f) {
        // The adapter with the most dedicated VRAM (the one the backend picks).
        IDXGIAdapter1* best = nullptr;
        SIZE_T bestMem = 0;
        IDXGIAdapter1* ad = nullptr;
        for (UINT i = 0; f->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
          DXGI_ADAPTER_DESC1 d{};
          ad->GetDesc1(&d);
          if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory > bestMem) {
            if (best) best->Release();
            best = ad;
            bestMem = d.DedicatedVideoMemory;
          } else {
            ad->Release();
          }
        }
        if (best) {
          best->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a));
          best->Release();
        }
        f->Release();
      }
    }
    if (!a) return;
    DXGI_QUERY_VIDEO_MEMORY_INFO li{}, ni{};
    if (SUCCEEDED(a->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &li)))
      localMb = double(li.CurrentUsage) / 1048576.0;
    if (SUCCEEDED(a->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &ni)))
      nonLocalMb = double(ni.CurrentUsage) / 1048576.0;
  }
};

// memmap_<n>.txt (every SR_PERF_MEMMAP_SECS, default 60; 0 = off; and at exit): what holds
// the memory. A VirtualQuery walk of the whole address space (on this thread; the
// guest region costs ~10 ms per call, a walk tens of ms) summarised by type, plus the
// committed regions >= 16 MB with a RESIDENT estimate (QueryWorkingSetEx on every 16th
// page) and a label (the guest flat region, the base-4GB mirror, module images).
int g_memmapNo = 0;

void memmapCensus(const char* why) {
  static std::mutex s_mu;  // the periodic census (stats thread) vs the exit census
  std::lock_guard<std::mutex> lk(s_mu);
  char path[512];
  std::snprintf(path, sizeof(path), "%s/memmap_%03d.txt", g_dir.c_str(), ++g_memmapNo);
  FILE* f = std::fopen(path, "w");
  if (!f) return;
  const uint64_t t0 = nowUs();
  uint8_t* gbase = nullptr;
  size_t gsize = 0;
  sr::kernel::getGlobalGuestMemory(&gbase, &gsize);
  const uintptr_t mirrorLo = 0x2900000000ull, mirrorHi = mirrorLo + 0x87000000ull;
  struct Reg {
    uintptr_t allocBase, base;
    size_t size;
    DWORD type, protect;
  };
  std::vector<Reg> committed;
  uint64_t byType[3] = {0, 0, 0};  // private, mapped, image (committed bytes)
  uint64_t reserved = 0;
  size_t calls = 0;
  MEMORY_BASIC_INFORMATION mbi;
  for (uintptr_t p = 0x10000; p < 0x7FFFFFFF0000ull;) {
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi))) break;
    ++calls;
    const uintptr_t b = uintptr_t(mbi.BaseAddress);
    const size_t s = mbi.RegionSize;
    if (mbi.State == MEM_COMMIT) {
      const int ti = mbi.Type == MEM_PRIVATE ? 0 : mbi.Type == MEM_MAPPED ? 1 : 2;
      byType[ti] += s;
      committed.push_back({uintptr_t(mbi.AllocationBase), b, s, mbi.Type, mbi.Protect});
    } else if (mbi.State == MEM_RESERVE) {
      reserved += s;
    }
    if (b + s <= p) break;
    p = b + s;
  }
  const uint64_t tWalk = nowUs() - t0;
  // Group committed runs by allocation base.
  struct Alloc {
    uintptr_t base = 0;
    uint64_t committed = 0;
    DWORD type = 0;
    std::vector<std::pair<uintptr_t, size_t>> runs;
  };
  std::unordered_map<uintptr_t, Alloc> allocs;
  for (const Reg& r : committed) {
    Alloc& a = allocs[r.allocBase];
    a.base = r.allocBase;
    a.committed += r.size;
    a.type = r.type;
    a.runs.push_back({r.base, r.size});
  }
  std::vector<Alloc*> big;
  for (auto& kv : allocs)
    if (kv.second.committed >= (16ull << 20)) big.push_back(&kv.second);
  std::sort(big.begin(), big.end(), [](Alloc* a, Alloc* b) { return a->committed > b->committed; });
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
  std::fprintf(f,
               "# memmap v1 n=%d why=%s t_us=%llu walk_ms=%.1f vq_calls=%zu private_mb=%.0f ws_mb=%.0f\n"
               "committed_mb private=%.0f mapped=%.0f image=%.0f reserved_mb=%.0f\n"
               "guest_base=%p guest_size=%zx mirror=%llx-%llx\n",
               g_memmapNo, why, (unsigned long long)t0, double(tWalk) / 1e3, calls,
               double(pmc.PrivateUsage) / 1048576.0, double(pmc.WorkingSetSize) / 1048576.0,
               double(byType[0]) / 1048576.0, double(byType[1]) / 1048576.0,
               double(byType[2]) / 1048576.0, double(reserved) / 1048576.0, (void*)gbase, gsize,
               (unsigned long long)mirrorLo, (unsigned long long)mirrorHi);
  // Residency estimate: every 16th page of each big allocation (cap 65536 probes each).
  std::vector<PSAPI_WORKING_SET_EX_INFORMATION> probe;
  for (Alloc* a : big) {
    uint64_t probes = 0, resident = 0;
    size_t step = 16 * 4096;
    uint64_t pages = a->committed / 4096;
    if (pages / 16 > 65536) step = size_t((a->committed / 65536) & ~uint64_t(4095));
    if (step < 4096) step = 4096;
    probe.clear();
    for (auto& run : a->runs)
      for (uintptr_t p = run.first; p < run.first + run.second; p += step) {
        PSAPI_WORKING_SET_EX_INFORMATION w{};
        w.VirtualAddress = reinterpret_cast<PVOID>(p);
        probe.push_back(w);
      }
    if (!probe.empty() &&
        QueryWorkingSetEx(GetCurrentProcess(), probe.data(), DWORD(probe.size() * sizeof(probe[0])))) {
      for (auto& w : probe) {
        ++probes;
        if (w.VirtualAttributes.Valid) ++resident;
      }
    }
    const double resMb = probes ? double(a->committed) * double(resident) / double(probes) / 1048576.0 : -1;
    char label[300] = "-";
    if (gbase && a->base >= uintptr_t(gbase) && a->base < uintptr_t(gbase) + gsize)
      std::snprintf(label, sizeof(label), "guest-flat");
    else if (a->base >= mirrorLo && a->base < mirrorHi)
      std::snprintf(label, sizeof(label), "guest-mirror(base-4GB)");
    else if (a->type == MEM_IMAGE)
      GetModuleFileNameA(reinterpret_cast<HMODULE>(a->base), label, sizeof(label));
    std::fprintf(f, "alloc %llx committed_mb=%.1f resident_mb=%.1f type=%s runs=%zu label=%s\n",
                 (unsigned long long)a->base, double(a->committed) / 1048576.0, resMb,
                 a->type == MEM_PRIVATE ? "private" : a->type == MEM_MAPPED ? "mapped" : "image",
                 a->runs.size(), label);
  }
  // The guest flat region per 256 MB window: resident estimate (which guest windows hold RAM).
  if (gbase && gsize) {
    for (uint64_t w = 0; w < gsize && w < 0x100000000ull; w += 0x10000000ull) {
      probe.clear();
      for (uint64_t o = w; o < w + 0x10000000ull && o < gsize; o += 64 * 1024) {
        PSAPI_WORKING_SET_EX_INFORMATION x{};
        x.VirtualAddress = gbase + o;
        probe.push_back(x);
      }
      uint64_t res = 0;
      if (QueryWorkingSetEx(GetCurrentProcess(), probe.data(), DWORD(probe.size() * sizeof(probe[0]))))
        for (auto& x : probe)
          if (x.VirtualAttributes.Valid) ++res;
      std::fprintf(f, "guestwin %08llx resident_mb~%.0f\n", (unsigned long long)w,
                   256.0 * double(res) / double(std::max<size_t>(1, probe.size())));
    }
  }
  {
    const uint32_t mf = g_mirrorFaults.load(std::memory_order_relaxed);
    std::fprintf(f, "mirror_demand_commits=%u (SR_GUEST_MIRROR=lazy; 64 KB each)", mf);
    HMODULE exe = GetModuleHandleA(nullptr);
    for (int i = 0; i < kMirrorFaultRips && i < int(mf); ++i)
      std::fprintf(f, " rip=exe+%llx", (unsigned long long)(g_mirrorFaultRip[i] - uintptr_t(exe)));
    std::fputc('\n', f);
  }
  std::fprintf(f, "# census_ms=%.1f\n", double(nowUs() - t0) / 1e3);
  std::fclose(f);
}

// ------------------------------------------------------------------ main loop
void writeMeta() {
  std::string p = g_dir + "/meta.txt";
  FILE* f = std::fopen(p.c_str(), "w");
  if (!f) return;
  char exe[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::fprintf(f, "pid=%lu\nexe=%s\nepoch_ms_t0=%lld\nqpc_freq=%lld\n",
               (unsigned long)GetCurrentProcessId(), exe, (long long)qpc().epochMs0,
               (long long)qpc().f.QuadPart);
  // The SR_* environment (the run's gates).
  if (LPCH env = GetEnvironmentStringsA()) {
    for (LPCH s = env; *s; s += std::strlen(s) + 1)
      if (std::strncmp(s, "SR_", 3) == 0 || std::strncmp(s, "SDL_", 4) == 0)
        std::fprintf(f, "env %s\n", s);
    FreeEnvironmentStringsA(env);
  }
  std::fclose(f);
}

void statsMain() {
  ensureDir();
  writeMeta();
  FILE* ff = std::fopen((g_dir + "/frames.csv").c_str(), "w");
  FILE* tf = std::fopen((g_dir + "/threads.csv").c_str(), "w");
  if (ff) {
    setvbuf(ff, nullptr, _IOFBF, 1 << 16);
    std::fprintf(ff, "kind,n,t_us\n");
  }
  if (tf) {
    setvbuf(tf, nullptr, _IOFBF, 1 << 16);
    std::fprintf(tf, "t_us,tid,user_ms,kernel_ms,ctxsw,name\n");
  }
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PERF] telemetry on: dir=%s (frames.csv, threads.csv)",
              g_dir.c_str());
  FILE* mf = std::fopen((g_dir + "/mem.csv").c_str(), "w");
  if (mf) {
    setvbuf(mf, nullptr, _IOFBF, 1 << 14);
    std::fprintf(mf, "t_us,private_mb,ws_mb,peak_ws_mb,commit_mb,gpu_local_mb,gpu_nonlocal_mb,"
                     "sys_busy_cores,proc_cores,avail_ram_mb\n");
  }
  GpuMem gpu;
  // Machine load (other lanes' builds and jobs share this box): busy cores machine-wide and
  // this process's cores over the last second, plus available RAM - so a report can flag an
  // era measured under contention.
  uint64_t prevSysIdle = 0, prevSysTotal = 0, prevProcCpu = 0, prevLoadUs = 0;
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  const double nCpu = double(si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1);
  auto ft64 = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  int memmapSecs = 60;
  if (const char* e = std::getenv("SR_PERF_MEMMAP_SECS")) memmapSecs = std::max(0, std::atoi(e));
  uint64_t lastMemmapUs = nowUs();
  std::unordered_map<DWORD, ThreadRec> threads;
  std::vector<SysThread> sysThreads;
  const DWORD self = GetCurrentThreadId();
  uint64_t lastThreadsUs = 0, lastSummaryUs = nowUs();
  std::vector<uint64_t> swapUs;  // swap stamps since the last summary
  uint64_t prevSwapUs = 0;
  uint32_t prevPresents = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // Drain the ring.
    const uint64_t w = g_wr.load(std::memory_order_acquire);
    if (w - g_rd > kRing) {  // lapped: the oldest kRing slots are all that remain
      g_dropped.fetch_add(w - kRing - g_rd, std::memory_order_relaxed);
      g_rd = w - kRing;
    }
    while (g_rd < w) {
      Ev& e = g_ring[g_rd & (kRing - 1)];
      const uint64_t s = e.seq.load(std::memory_order_acquire);
      if (s != g_rd + 1) {
        if (s > g_rd + 1) {  // overwritten by a later lap while we read
          g_dropped.fetch_add(1, std::memory_order_relaxed);
          ++g_rd;
          continue;
        }
        break;  // the producer has not finished this slot yet
      }
      if (ff) std::fprintf(ff, "%c,%u,%llu\n", char(e.kind), e.n, (unsigned long long)e.us);
      if (e.kind == 'S') {
        if (prevSwapUs) swapUs.push_back(e.us - prevSwapUs);
        prevSwapUs = e.us;
      }
      ++g_rd;
    }
    const uint64_t now = nowUs();
    // Per-thread CPU, once a second.
    if (tf && now - lastThreadsUs >= 1000000) {
      lastThreadsUs = now;
      for (auto& kv : threads) kv.second.alive = false;
      sysThreads.clear();
      if (ownThreads(sysThreads)) {
        for (const SysThread& st : sysThreads) {
          if (st.tid == self) continue;
          ThreadRec& r = threads[st.tid];
          if (!r.h) r.h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, st.tid);
          r.alive = true;
          // Guest threads register (and name themselves) after they start: refresh
          // until the name is known.
          if (!r.name[0] || r.name[0] == '-' || std::strncmp(r.name, "guest:slot", 10) == 0)
            threadName(st.tid, r.h, r.name, sizeof(r.name));
          std::fprintf(tf, "%llu,%lu,%.1f,%.1f,%lu,%s\n", (unsigned long long)now,
                       (unsigned long)st.tid, double(st.user100ns) / 1e4,
                       double(st.kernel100ns) / 1e4, (unsigned long)st.ctxSwitches, r.name);
        }
      }
      for (auto it = threads.begin(); it != threads.end();) {
        if (!it->second.alive) {
          if (it->second.h) CloseHandle(it->second.h);
          it = threads.erase(it);
        } else {
          ++it;
        }
      }
      std::fflush(tf);
      if (ff) std::fflush(ff);
      if (mf) {
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc));
        double gl = -1, gn = -1;
        gpu.query(gl, gn);
        double sysBusy = -1, procCores = -1, availMb = -1;
        {
          FILETIME fi{}, fk{}, fu{}, pc{}, pe{}, pk{}, pu{};
          if (GetSystemTimes(&fi, &fk, &fu) &&
              GetProcessTimes(GetCurrentProcess(), &pc, &pe, &pk, &pu)) {
            const uint64_t idle = ft64(fi), total = ft64(fk) + ft64(fu);  // kernel includes idle
            const uint64_t proc = ft64(pk) + ft64(pu);
            if (prevLoadUs && now > prevLoadUs && total > prevSysTotal) {
              sysBusy = (1.0 - double(idle - prevSysIdle) / double(total - prevSysTotal)) * nCpu;
              procCores = double(proc - prevProcCpu) / (double(now - prevLoadUs) * 10.0);
            }
            prevSysIdle = idle;
            prevSysTotal = total;
            prevProcCpu = proc;
            prevLoadUs = now;
          }
          MEMORYSTATUSEX ms{};
          ms.dwLength = sizeof(ms);
          if (GlobalMemoryStatusEx(&ms)) availMb = double(ms.ullAvailPhys) / 1048576.0;
        }
        std::fprintf(mf, "%llu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%.0f\n",
                     (unsigned long long)now, double(pmc.PrivateUsage) / 1048576.0,
                     double(pmc.WorkingSetSize) / 1048576.0,
                     double(pmc.PeakWorkingSetSize) / 1048576.0,
                     double(pmc.PagefileUsage) / 1048576.0, gl, gn, sysBusy, procCores, availMb);
        std::fflush(mf);
      }
    }
    if (memmapSecs > 0 && now - lastMemmapUs >= uint64_t(memmapSecs) * 1000000ull) {
      lastMemmapUs = now;
      memmapCensus("periodic");
    }
    {
      static uint32_t s_lastMf = 0;
      const uint32_t mf = g_mirrorFaults.load(std::memory_order_relaxed);
      if (mf != s_lastMf) {
        s_lastMf = mf;
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[MIRROR_DEMAND] %u demand commit(s) of the base-4GB guest mirror; first rip=exe+%llx",
                    mf, (unsigned long long)(g_mirrorFaultRip[0] - uintptr_t(GetModuleHandleA(nullptr))));
      }
    }
    // The [PERF] summary, every 10 s.
    if (now - lastSummaryUs >= 10000000) {
      const double secs = double(now - lastSummaryUs) / 1e6;
      lastSummaryUs = now;
      const uint32_t presents = g_presents.load(std::memory_order_relaxed);
      double p50 = 0, p95 = 0, p99 = 0, mx = 0;
      if (!swapUs.empty()) {
        std::vector<uint64_t> v = swapUs;
        std::sort(v.begin(), v.end());
        auto pct = [&](double q) {
          return double(v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))]) / 1e3;
        };
        p50 = pct(0.50);
        p95 = pct(0.95);
        p99 = pct(0.99);
        mx = double(v.back()) / 1e3;
      }
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[PERF] t=%.0fs swaps/s=%.1f presents/s=%.1f frame_ms p50=%.1f p95=%.1f "
                  "p99=%.1f max=%.1f (swaps=%u presents=%u dropped=%llu)",
                  double(now) / 1e6, double(swapUs.size()) / secs,
                  double(presents - prevPresents) / secs, p50, p95, p99, mx,
                  g_swaps.load(std::memory_order_relaxed), presents,
                  (unsigned long long)g_dropped.load(std::memory_order_relaxed));
      prevPresents = presents;
      swapUs.clear();
    }
  }
}

}  // namespace

bool ownThreads(std::vector<SysThread>& out) {
  typedef LONG(NTAPI * NtQSI)(ULONG, PVOID, ULONG, PULONG);
  static const NtQSI q = reinterpret_cast<NtQSI>(
      GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation"));
  if (!q) return false;
  thread_local std::vector<uint8_t> buf(1 << 20);
  ULONG need = 0;
  LONG st = 0;
  for (int i = 0; i < 4; ++i) {
    st = q(5 /*SystemProcessInformation*/, buf.data(), ULONG(buf.size()), &need);
    if (st != LONG(0xC0000004) /*STATUS_INFO_LENGTH_MISMATCH*/) break;
    buf.resize(std::max<size_t>(buf.size() * 2, size_t(need) + (64u << 10)));
  }
  if (st != 0) return false;
  // x64 SYSTEM_PROCESS_INFORMATION: NextEntryOffset +0x00, NumberOfThreads +0x04,
  // UniqueProcessId +0x50, threads at +0x100; SYSTEM_THREAD_INFORMATION (0x50 bytes):
  // KernelTime +0x00, UserTime +0x08, ClientId.UniqueThread +0x30, ContextSwitches +0x40,
  // ThreadState +0x44, WaitReason +0x48.
  const uintptr_t pid = GetCurrentProcessId();
  size_t off = 0;
  for (;;) {
    const uint8_t* p = buf.data() + off;
    uint32_t next, nthreads;
    uintptr_t upid;
    std::memcpy(&next, p, 4);
    std::memcpy(&nthreads, p + 4, 4);
    std::memcpy(&upid, p + 0x50, sizeof(upid));
    if (upid == pid) {
      for (uint32_t i = 0; i < nthreads; ++i) {
        const uint8_t* t = p + 0x100 + size_t(i) * 0x50;
        SysThread x;
        int64_t k, u;
        uintptr_t tid;
        std::memcpy(&k, t + 0x00, 8);
        std::memcpy(&u, t + 0x08, 8);
        std::memcpy(&tid, t + 0x30, sizeof(tid));
        std::memcpy(&x.ctxSwitches, t + 0x40, 4);
        std::memcpy(&x.state, t + 0x44, 4);
        std::memcpy(&x.waitReason, t + 0x48, 4);
        x.tid = uint32_t(tid);
        x.kernel100ns = uint64_t(k);
        x.user100ns = uint64_t(u);
        out.push_back(x);
      }
      return true;
    }
    if (!next) break;
    off += next;
    if (off >= buf.size()) break;
  }
  return false;
}

uint64_t nowUs() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  const Qpc& q = qpc();
  const uint64_t d = uint64_t(t.QuadPart - q.t0.QuadPart);
  return d / uint64_t(q.f.QuadPart) * 1000000ull +
         d % uint64_t(q.f.QuadPart) * 1000000ull / uint64_t(q.f.QuadPart);
}

void onGuestSwapSlow() { push('S', g_swaps.fetch_add(1, std::memory_order_relaxed) + 1); }
void onHostPresentSlow() { push('P', g_presents.fetch_add(1, std::memory_order_relaxed) + 1); }

const char* outDir() {
  if (!g_statsOn && !std::getenv("SR_HOST_PROFILE_HZ")) return "";
  ensureDir();
  return g_dir.c_str();
}

namespace {
void finalMemmap() {
  static std::atomic<bool> s_done{false};
  if (s_done.exchange(true)) return;
  memmapCensus("exit");
}
}  // namespace

#if SR_ORACLE
// ── step 4 G7 option (c): the host-side allocation census (perf_stats.hpp; NOTES 2026-10-20 15:34) ──
namespace {
struct CensusSource {
  const char* group;
  AllocCensusSource fn;
  void* ctx;
};
std::mutex g_censusMu;
std::vector<CensusSource> g_censusSources;
FILE* g_censusFile = nullptr;
uint64_t g_censusT = 0;

void censusEmit(const char* group, const char* name, double v) {
  std::fprintf(g_censusFile, "%llu,%s,%s,%.3f\n", (unsigned long long)g_censusT, group, name, v);
}
int heapClass(size_t n) { return n < 256 ? 0 : n < 4096 ? 1 : n < 65536 ? 2 : n < (1u << 20) ? 3 : 4; }
const char* const kHeapClass[5] = {"lt256", "lt4k", "lt64k", "lt1m", "ge1m"};
int regClass(uint64_t n) { return n < 65536 ? 0 : n < (1u << 20) ? 1 : n < (16u << 20) ? 2 : 3; }
const char* const kRegClass[4] = {"lt64k", "lt1m", "lt16m", "ge16m"};
constexpr double kMb = 1.0 / 1048576.0;

void censusOnce() {
  g_censusT = nowUs();
  char g[48], nm[64];
  // (1) the process heaps
  DWORD n = GetProcessHeaps(0, nullptr);
  std::vector<HANDLE> heaps(size_t(n) + 16);
  n = GetProcessHeaps(DWORD(heaps.size()), heaps.data());
  heaps.resize(std::min<size_t>(n, heaps.size()));
  const HANDLE proc = GetProcessHeap();
  std::vector<std::pair<uintptr_t, uintptr_t>> owned;   // walked heaps' regions + large blocks
  double unwalkedCommitted = 0;
  const uint64_t tw0 = nowUs();
  for (HANDLE h : heaps) {
    std::snprintf(g, sizeof g, "heap_%llx%s", (unsigned long long)uintptr_t(h), h == proc ? "_proc" : "");
    HEAP_SUMMARY hs{};
    hs.cb = sizeof hs;
    if (HeapSummary(h, 0, &hs)) {
      censusEmit(g, "allocated_mb", double(hs.cbAllocated) * kMb);
      censusEmit(g, "committed_mb", double(hs.cbCommitted) * kMb);
      censusEmit(g, "reserved_mb", double(hs.cbReserved) * kMb);
    }
    ULONG compat = 0;
    const bool lfh = HeapQueryInformation(h, HeapCompatibilityInformation, &compat, sizeof compat, nullptr) &&
                     compat == 2;   // an LFH heap is always serialized: HeapLock is defined on it
    if (!lfh) {
      unwalkedCommitted += double(hs.cbCommitted) * kMb;
      censusEmit(g, "walked", 0);
      continue;
    }
    if (!HeapLock(h)) {
      unwalkedCommitted += double(hs.cbCommitted) * kMb;
      censusEmit(g, "walked", 0);
      continue;
    }
    double busy[5] = {}, cnt[5] = {};
    PROCESS_HEAP_ENTRY e{};
    e.lpData = nullptr;
    while (HeapWalk(h, &e)) {
      if (e.wFlags & PROCESS_HEAP_REGION) {
        const uintptr_t b = uintptr_t(e.lpData);
        owned.push_back({b, b + e.Region.dwCommittedSize + e.Region.dwUnCommittedSize});
      } else if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
        const int c = heapClass(e.cbData);
        busy[c] += double(e.cbData);
        cnt[c] += 1;
        if (e.cbData >= (256u << 10)) owned.push_back({uintptr_t(e.lpData), uintptr_t(e.lpData) + e.cbData});
      }
    }
    HeapUnlock(h);
    censusEmit(g, "walked", 1);
    for (int c = 0; c < 5; ++c) {
      std::snprintf(nm, sizeof nm, "busy_mb_%s", kHeapClass[c]);
      censusEmit(g, nm, busy[c] * kMb);
      std::snprintf(nm, sizeof nm, "blocks_%s", kHeapClass[c]);
      censusEmit(g, nm, cnt[c]);
    }
  }
  censusEmit("heaps", "count", double(heaps.size()));
  censusEmit("heaps", "unwalked_committed_mb", unwalkedCommitted);
  censusEmit("heaps", "walk_ms", double(nowUs() - tw0) / 1e3);
  // (2) private committed memory no walked heap owns (by allocation), the guest arena and its mirror excluded
  std::sort(owned.begin(), owned.end());
  const auto isOwned = [&](uintptr_t p) {
    auto it = std::upper_bound(owned.begin(), owned.end(), std::make_pair(p, ~uintptr_t(0)));
    if (it == owned.begin()) return false;
    --it;
    return p >= it->first && p < it->second;
  };
  uint8_t* gbase = nullptr;
  size_t gsize = 0;
  sr::kernel::getGlobalGuestMemory(&gbase, &gsize);
  const uintptr_t gLo = uintptr_t(gbase), gHi = gLo + gsize;
  const uintptr_t mirrorLo = 0x2900000000ull, mirrorHi = mirrorLo + 0x87000000ull;
  std::unordered_map<uintptr_t, uint64_t> unowned;   // allocation base -> committed bytes
  double ownedMb = 0, guestMb = 0;
  MEMORY_BASIC_INFORMATION mbi;
  for (uintptr_t p = 0x10000; p < 0x7FFFFFFF0000ull;) {
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi))) break;
    const uintptr_t b = uintptr_t(mbi.BaseAddress);
    const size_t s = mbi.RegionSize;
    if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE) {
      if ((b >= gLo && b < gHi) || (b >= mirrorLo && b < mirrorHi)) guestMb += double(s) * kMb;
      else if (isOwned(b)) ownedMb += double(s) * kMb;
      else unowned[uintptr_t(mbi.AllocationBase)] += s;
    }
    if (b + s <= p) break;
    p = b + s;
  }
  double rcnt[4] = {}, rmb[4] = {}, total = 0;
  for (const auto& kv : unowned) {
    const int c = regClass(kv.second);
    rcnt[c] += 1;
    rmb[c] += double(kv.second) * kMb;
    total += double(kv.second) * kMb;
  }
  censusEmit("private", "guest_mb", guestMb);
  censusEmit("private", "heap_owned_mb", ownedMb);
  censusEmit("private", "unowned_mb", total);
  for (int c = 0; c < 4; ++c) {
    std::snprintf(nm, sizeof nm, "unowned_mb_%s", kRegClass[c]);
    censusEmit("private", nm, rmb[c]);
    std::snprintf(nm, sizeof nm, "unowned_allocs_%s", kRegClass[c]);
    censusEmit("private", nm, rcnt[c]);
  }
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
  censusEmit("process", "private_usage_mb", double(pmc.PrivateUsage) * kMb);
  // (3) + (4) the registered sources
  {
    std::lock_guard lk(g_censusMu);
    for (const auto& s : g_censusSources) s.fn(s.ctx, &censusEmit);
  }
  // (5) handles, GDI and USER objects
  DWORD hc = 0;
  GetProcessHandleCount(GetCurrentProcess(), &hc);
  censusEmit("handles", "kernel", double(hc));
  censusEmit("handles", "gdi", double(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)));
  censusEmit("handles", "user", double(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS)));
  std::fflush(g_censusFile);
}

void censusMain(int secs) {
  SetThreadDescription(GetCurrentThread(), L"SR alloc census");
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(secs));
    censusOnce();
  }
}
}  // namespace

void allocCensusRegister(const char* group, AllocCensusSource fn, void* ctx) {
  std::lock_guard lk(g_censusMu);
  g_censusSources.push_back({group, fn, ctx});
}
#endif  // SR_ORACLE

void start() {
  static std::atomic<bool> s_started{false};
  if (s_started.exchange(true)) return;
  (void)qpc();  // t0 = process start (as near as main allows)
  if (g_statsOn) {
    ensureDir();
    std::atexit(finalMemmap);
    std::at_quick_exit(finalMemmap);
    std::thread(statsMain).detach();
#if SR_ORACLE
    // step 4 G7 option (c): SR_ALLOC_CENSUS=1 (default OFF; diagnostic)
    if (const char* e = std::getenv("SR_ALLOC_CENSUS"); e && e[0] == '1') {
      const std::string path = g_dir + "/alloc_census.csv";
      g_censusFile = std::fopen(path.c_str(), "w");
      if (g_censusFile) {
        std::fprintf(g_censusFile, "t_us,group,name,value\n");
        int secs = 30;
        if (const char* s = std::getenv("SR_ALLOC_CENSUS_SECS")) secs = std::max(5, std::atoi(s));
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[ALLOC_CENSUS] on: every %d s to %s (SR_ALLOC_CENSUS, diagnostic)", secs,
                    path.c_str());
        std::thread(censusMain, secs).detach();
      }
    }
#endif
  }
  startHostProfiler();
}

}  // namespace sr::perf
