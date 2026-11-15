// iter-464 perf lane: the HOST sampling profiler (SR_HOST_PROFILE_HZ=<hz>).
//
// Why a host profiler: the guest profiler (kernel/guest_profile.cpp) samples the guest
// LR, which is blind to everything the platform does on the guest's behalf (kernel
// imports, logging, VirtualQuery, lock spins, the PM4 command processor, D3D12). This
// one samples the host RIP and unwinds the host stack, so it names both: recompiled
// guest functions are host functions called sub_XXXXXXXX (the host symbol IS the guest
// VA) and every platform function has its own symbol in the exe's PDB.
//
// Per tick, for every thread of the process that consumed CPU since the previous tick
// (QueryThreadCycleTime), and once a second for the idle ones (wait sites):
//   SuspendThread -> GetThreadContext -> unwind -> ResumeThread, then aggregate.
// While a thread is suspended this thread takes NO lock and allocates NOTHING (the
// suspended thread may hold the heap or loader lock): the module table and each
// module's .pdata were captured beforehand; the unwind is RtlVirtualUnwind over our own
// binary search of .pdata (RtlLookupFunctionEntry can take the loader's SRW lock), and
// it only reads the target's own committed stack [Rsp, StackBase) - fault-free by
// construction (the harness VEH makes any host AV fatal).
//
// Output: <perf dir>/hprof_<n>.txt every SR_HOST_PROFILE_SECS (default 10) seconds and
// at exit; tools/perf/host_profile.py symbolizes and reports top-N per thread per era.

#include "perf_stats.hpp"
#include "hitch_log.hpp"

#include "guest_context.hpp"
#include "../app/log.hpp"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" sr::kernel::GuestThreadSlot g_debugThreadRegistry[64];

namespace sr::perf {

namespace {

constexpr int kMaxFrames = 64;

// ------------------------------------------------------------------ modules
struct Mod {
  uintptr_t base = 0, end = 0;
  const RUNTIME_FUNCTION* pdata = nullptr;
  uint32_t count = 0;
  std::string path;
};
std::vector<Mod> g_mods;  // sorted by base; append-only indices via g_modOrder
std::vector<int> g_modIndex;  // g_mods[i] -> stable index (order first seen)
int g_modNext = 0;
std::unordered_map<uintptr_t, int> g_modStable;  // base -> stable index

void refreshModules() {
  HMODULE hm[1024];
  DWORD need = 0;
  if (!EnumProcessModules(GetCurrentProcess(), hm, sizeof(hm), &need)) return;
  const DWORD n = std::min<DWORD>(need / sizeof(HMODULE), 1024);
  std::vector<Mod> mods;
  mods.reserve(n);
  for (DWORD i = 0; i < n; ++i) {
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), hm[i], &mi, sizeof(mi))) continue;
    Mod m;
    m.base = uintptr_t(mi.lpBaseOfDll);
    m.end = m.base + mi.SizeOfImage;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
      const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
      if (nt->Signature == IMAGE_NT_SIGNATURE &&
          nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
          nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXCEPTION) {
        const auto& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (d.VirtualAddress && d.Size) {
          m.pdata = reinterpret_cast<const RUNTIME_FUNCTION*>(m.base + d.VirtualAddress);
          m.count = d.Size / sizeof(RUNTIME_FUNCTION);
        }
      }
    }
    char path[MAX_PATH] = {};
    GetModuleFileNameA(hm[i], path, MAX_PATH);
    m.path = path;
    mods.push_back(std::move(m));
  }
  std::sort(mods.begin(), mods.end(), [](const Mod& a, const Mod& b) { return a.base < b.base; });
  g_mods.swap(mods);
  g_modIndex.resize(g_mods.size());
  for (size_t i = 0; i < g_mods.size(); ++i) {
    auto it = g_modStable.find(g_mods[i].base);
    if (it == g_modStable.end()) it = g_modStable.emplace(g_mods[i].base, g_modNext++).first;
    g_modIndex[i] = it->second;
  }
}

// Lock-free, allocation-free lookups (safe while a thread is suspended).
int findMod(uintptr_t pc) {
  int lo = 0, hi = int(g_mods.size()) - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (pc < g_mods[mid].base) hi = mid - 1;
    else if (pc >= g_mods[mid].end) lo = mid + 1;
    else return mid;
  }
  return -1;
}

const RUNTIME_FUNCTION* findFn(const Mod& m, uint32_t rva) {
  if (!m.pdata) return nullptr;
  int lo = 0, hi = int(m.count) - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const RUNTIME_FUNCTION& f = m.pdata[mid];
    if (rva < f.BeginAddress) hi = mid - 1;
    else if (rva >= f.EndAddress) lo = mid + 1;
    else return &f;
  }
  return nullptr;
}

// The primary entry of a (possibly chained) function: the key a sample is charged to.
uint32_t rootBegin(const Mod& m, const RUNTIME_FUNCTION* f) {
  for (int guard = 0; guard < 8; ++guard) {
    uint32_t ud = f->UnwindData;
    if (ud & 1u) {  // indirect: points at another RUNTIME_FUNCTION
      f = reinterpret_cast<const RUNTIME_FUNCTION*>(m.base + (ud & ~1u));
      continue;
    }
    const uint8_t* ui = reinterpret_cast<const uint8_t*>(m.base + ud);
    const uint8_t flags = ui[0] >> 3;
    if (!(flags & UNW_FLAG_CHAININFO)) return f->BeginAddress;
    const uint8_t codes = ui[2];
    f = reinterpret_cast<const RUNTIME_FUNCTION*>(ui + 4 + ((codes + 1u) & ~1u) * 2u);
  }
  return f->BeginAddress;
}

// iter-465 perf2 (SR_HOST_PROFILE_RAWPC=1): the innermost frame keeps its exact rva (bit31 set,
// as for a leaf) instead of its function's begin, so llvm-symbolizer can name the LINE (and the
// inlined callee) a sample landed on. Off: function-level keys as before.
static const bool g_rawPc = [] {
  const char* e = std::getenv("SR_HOST_PROFILE_RAWPC");
  return e && e[0] == '1';
}();

// Unwind the suspended thread's host stack. Reads only [rsp0, stackHi).
int unwind(CONTEXT& c, uintptr_t stackHi, uint64_t* keys) {
  const uintptr_t stackLo = c.Rsp;
  int n = 0;
  for (; n < kMaxFrames; ++n) {
    const uintptr_t pc = c.Rip;
    if (!pc) break;
    const int mi = findMod(n ? pc - 1 : pc);
    if (mi < 0) {
      keys[n] = (uint64_t(0xFFFFu) << 32) | 0u;  // unknown code (no module)
      ++n;
      break;
    }
    const Mod& m = g_mods[mi];
    const uint32_t rva = uint32_t(pc - m.base);
    const RUNTIME_FUNCTION* f = findFn(m, n ? rva - 1 : rva);
    uint32_t fk = f ? rootBegin(m, f) : (rva | 0x80000000u);  // bit31 = raw (leaf) rva
    if (g_rawPc && n == 0) fk = rva | 0x80000000u;
    keys[n] = (uint64_t(uint32_t(g_modIndex[mi])) << 32) | fk;
    if (f) {
      PVOID hd = nullptr;
      DWORD64 ef = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, m.base, pc, const_cast<PRUNTIME_FUNCTION>(f), &c, &hd,
                       &ef, nullptr);
    } else {
      // A leaf function (no unwind data): its return address is at [rsp].
      if (c.Rsp < stackLo || c.Rsp + 8 > stackHi) {
        ++n;
        break;
      }
      c.Rip = *reinterpret_cast<const uint64_t*>(c.Rsp);
      c.Rsp += 8;
    }
    if (c.Rsp < stackLo || c.Rsp >= stackHi) {
      ++n;
      break;
    }
  }
  return n;
}

// ------------------------------------------------------------------ threads
typedef LONG(NTAPI* NtQIT)(HANDLE, ULONG, PVOID, ULONG, PULONG);
struct TBI {
  LONG ExitStatus;
  PVOID TebBaseAddress;
  struct {
    HANDLE UniqueProcess, UniqueThread;
  } ClientId;
  ULONG_PTR AffinityMask;
  LONG Priority, BasePriority;
};

struct StackRec {
  uint32_t count = 0;
  uint8_t wait = 0;
  uint8_t n = 0;
  uint64_t keys[kMaxFrames];
};

struct Th {
  HANDLE h = nullptr;
  uintptr_t stackHi = 0;
  ULONG64 lastCyc = 0;
  uint64_t cycInterval = 0;
  bool alive = false;
  bool primed = false;
  char name[48] = {};
  uint32_t run = 0, waitS = 0, idle = 0;
  std::unordered_map<uint64_t, uint32_t> self, incl;
  std::unordered_map<uint64_t, StackRec> stacks;
  bool watch = false;  // iter-466 stutter: sampled every tick in hitch mode, busy or not
};

std::unordered_map<DWORD, Th> g_th;

void nameThread(DWORD tid, Th& t) {
  for (int i = 0; i < 64; ++i) {
    if (g_debugThreadRegistry[i].tid == tid && g_debugThreadRegistry[i].name[0]) {
      std::snprintf(t.name, sizeof(t.name), "guest:%s", g_debugThreadRegistry[i].name);
      return;
    }
  }
  for (int i = 0; i < 64; ++i)
    if (g_debugThreadRegistry[i].tid == tid) {
      std::snprintf(t.name, sizeof(t.name), "guest:slot%d", i);
      return;
    }
  PWSTR desc = nullptr;
  if (t.h && SUCCEEDED(GetThreadDescription(t.h, &desc)) && desc && desc[0]) {
    WideCharToMultiByte(CP_UTF8, 0, desc, -1, t.name, int(sizeof(t.name)), nullptr, nullptr);
    for (char* p = t.name; *p; ++p)
      if (*p == ' ') *p = '_';
  } else {
    std::snprintf(t.name, sizeof(t.name), "-");
  }
  if (desc) LocalFree(desc);
}

void refreshThreads(NtQIT ntqit, DWORD self) {
  for (auto& kv : g_th) kv.second.alive = false;
  static std::vector<SysThread> sys;
  sys.clear();
  if (!ownThreads(sys)) return;
  for (const SysThread& st : sys) {
    if (st.tid == self) continue;
    Th& t = g_th[st.tid];
    t.alive = true;
    if (!t.h) {
      t.h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                       FALSE, st.tid);
      if (t.h && ntqit) {
        TBI tbi{};
        if (ntqit(t.h, 0 /*ThreadBasicInformation*/, &tbi, sizeof(tbi), nullptr) == 0 &&
            tbi.TebBaseAddress) {
          const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(tbi.TebBaseAddress);
          t.stackHi = uintptr_t(tib->StackBase);
        }
      }
    }
    if (!t.name[0] || t.name[0] == '-' || std::strncmp(t.name, "guest:slot", 10) == 0) {
      nameThread(st.tid, t);
      // iter-466 stutter: the frame's threads (the CP = guest:GPU_CP, the host render thread,
      // and the guest render loop = the VdSwap caller, below) are sampled every tick in hitch
      // mode, so a hitch window shows where a WAITING thread waited, not only where busy ones
      // ran. (Sampling EVERY guest thread each tick wedged a boot: h1_b, CS_WEDGE.)
      t.watch = std::strcmp(t.name, "guest:GPU_CP") == 0 || std::strcmp(t.name, "SR_PM4_render") == 0;
    }
  }
  for (auto it = g_th.begin(); it != g_th.end();) {
    if (!it->second.alive && it->second.run + it->second.waitS + it->second.idle == 0) {
      if (it->second.h) CloseHandle(it->second.h);
      it = g_th.erase(it);
    } else {
      ++it;
    }
  }
}

void record(Th& t, bool wait, const uint64_t* keys, int n) {
  if (n <= 0) return;
  if (wait) t.waitS++;
  else t.run++;
  if (!wait) {
    t.self[keys[0]]++;
    // Inclusive: each distinct function once per sample.
    for (int i = 0; i < n; ++i) {
      bool dup = false;
      for (int j = 0; j < i && !dup; ++j) dup = keys[j] == keys[i];
      if (!dup) t.incl[keys[i]]++;
    }
  }
  uint64_t h = wait ? 0x9E3779B97F4A7C15ull : 0x100000001B3ull;
  for (int i = 0; i < n; ++i) h = (h ^ keys[i]) * 0x100000001B3ull;
  StackRec& s = t.stacks[h];
  if (!s.count) {
    s.n = uint8_t(n);
    s.wait = wait ? 1 : 0;
    std::memcpy(s.keys, keys, sizeof(uint64_t) * size_t(n));
  }
  s.count++;
}

// ------------------------------------------------------------------ hitch windows
// iter-466 stutter lane (SR_HITCH_LOG=1 with SR_HOST_PROFILE_HZ): every sample also goes to a
// time-stamped ring (innermost kHK frames); kernel/hitch_log.cpp posts each hitch's window
// [t0, t1] through a lock-free single-producer queue (the presenting thread may be suspended
// by this thread at any time, so it must never share a lock with it), and this thread writes
// the window's samples to hitch_samples.txt:
//   thread <tid> <name>                       once per thread
//   h <id> <t_us> <tid> <R|W> <key> <key>...  innermost first; keys as in hprof (mod:rva)
const bool g_hitchMode = [] {
  const char* e = std::getenv("SR_HITCH_LOG");
  return e && e[0] == '1';
}();
constexpr int kHK = 12;
struct HSamp {
  uint64_t tUs;
  uint32_t tid;
  uint8_t n, wait;
  uint64_t keys[kHK];
};
constexpr uint32_t kHRing = 1u << 16;
std::vector<HSamp> g_hring;  // sized once on the profiler thread before sampling starts
uint64_t g_hringN = 0;
struct HReq {
  uint32_t id;
  uint64_t t0, t1;
};
constexpr uint32_t kHReq = 128;
HReq g_hreq[kHReq];
std::atomic<uint32_t> g_hreqW{0}, g_hreqR{0};
std::atomic<bool> g_hitchReady{false};
FILE* g_hfile = nullptr;
std::unordered_map<DWORD, bool> g_hnamed;

void hitchPush(uint64_t tUs, DWORD tid, bool wait, const uint64_t* keys, int n) {
  HSamp& s = g_hring[g_hringN++ % kHRing];
  s.tUs = tUs;
  s.tid = tid;
  s.wait = wait ? 1 : 0;
  s.n = uint8_t(std::min(n, kHK));
  std::memcpy(s.keys, keys, sizeof(uint64_t) * s.n);
}

void hitchServe() {
  const uint32_t w = g_hreqW.load(std::memory_order_acquire);
  uint32_t r = g_hreqR.load(std::memory_order_relaxed);
  if (r == w) return;
  if (!g_hfile) {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/hitch_samples.txt", outDir());
    g_hfile = std::fopen(path, "w");
    if (g_hfile) {
      setvbuf(g_hfile, nullptr, _IOFBF, 1 << 16);
      std::fprintf(g_hfile, "# hitch_samples v1 pid=%lu\n", (unsigned long)GetCurrentProcessId());
    }
  }
  for (; r != w; ++r) {
    const HReq q = g_hreq[r % kHReq];
    if (!g_hfile) continue;
    const uint64_t lo = g_hringN > kHRing ? g_hringN - kHRing : 0;
    for (uint64_t i = g_hringN; i-- > lo;) {
      const HSamp& s = g_hring[i % kHRing];
      if (s.tUs < q.t0) break;
      if (s.tUs > q.t1) continue;
      auto it = g_th.find(s.tid);
      if (!g_hnamed[s.tid])
        std::fprintf(g_hfile, "thread %u %s\n", s.tid, it != g_th.end() ? it->second.name : "-");
      g_hnamed[s.tid] = true;
      std::fprintf(g_hfile, "h %u %llu %u %c", q.id, (unsigned long long)s.tUs, s.tid, s.wait ? 'W' : 'R');
      for (int k = 0; k < s.n; ++k) {
        std::fputc(' ', g_hfile);
        std::fprintf(g_hfile, "%u:%x", uint32_t(s.keys[k] >> 32), uint32_t(s.keys[k]));
      }
      std::fputc('\n', g_hfile);
    }
  }
  std::fflush(g_hfile);
  g_hreqR.store(r, std::memory_order_release);
}

int g_dumpNo = 0;
uint64_t g_intervalT0 = 0;

void writeKey(FILE* f, uint64_t k) {
  std::fprintf(f, "%u:%x", uint32_t(k >> 32), uint32_t(k));
}

void dump(const char* why, int hz) {
  const uint64_t t1 = nowUs();
  char path[512];
  std::snprintf(path, sizeof(path), "%s/hprof_%03d.txt", outDir(), ++g_dumpNo);
  FILE* f = std::fopen(path, "w");
  if (f) {
    setvbuf(f, nullptr, _IOFBF, 1 << 18);
    std::fprintf(f, "# host_profile v1 pid=%lu n=%d why=%s hz=%d t0_us=%llu t1_us=%llu\n",
                 (unsigned long)GetCurrentProcessId(), g_dumpNo, why, hz,
                 (unsigned long long)g_intervalT0, (unsigned long long)t1);
    for (size_t i = 0; i < g_mods.size(); ++i)
      std::fprintf(f, "mod %d %llx %llx %s\n", g_modIndex[i], (unsigned long long)g_mods[i].base,
                   (unsigned long long)(g_mods[i].end - g_mods[i].base), g_mods[i].path.c_str());
    std::vector<std::pair<DWORD, Th*>> order;
    for (auto& kv : g_th) order.push_back({kv.first, &kv.second});
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.second->cycInterval > b.second->cycInterval; });
    for (auto& [tid, tp] : order) {
      Th& t = *tp;
      if (!t.run && !t.waitS) continue;
      std::fprintf(f, "thread %lu name=%s run=%u wait=%u idle=%u cycles=%llu\n", (unsigned long)tid,
                   t.name[0] ? t.name : "-", t.run, t.waitS, t.idle,
                   (unsigned long long)t.cycInterval);
      auto top = [&](const char* tag, const std::unordered_map<uint64_t, uint32_t>& m, size_t k) {
        std::vector<std::pair<uint64_t, uint32_t>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < v.size() && i < k; ++i) {
          std::fprintf(f, "%s %lu %u ", tag, (unsigned long)tid, v[i].second);
          writeKey(f, v[i].first);
          std::fputc('\n', f);
        }
      };
      top("self", t.self, 400);
      top("incl", t.incl, 600);
      std::vector<const StackRec*> sv;
      sv.reserve(t.stacks.size());
      for (auto& kv : t.stacks) sv.push_back(&kv.second);
      std::sort(sv.begin(), sv.end(), [](const StackRec* a, const StackRec* b) { return a->count > b->count; });
      for (size_t i = 0; i < sv.size() && i < 300; ++i) {
        std::fprintf(f, "stk %lu %c %u ", (unsigned long)tid, sv[i]->wait ? 'W' : 'R', sv[i]->count);
        for (int j = 0; j < sv[i]->n; ++j) {
          if (j) std::fputc(';', f);
          writeKey(f, sv[i]->keys[j]);
        }
        std::fputc('\n', f);
      }
    }
    std::fclose(f);
  }
  for (auto& kv : g_th) {
    Th& t = kv.second;
    t.run = t.waitS = t.idle = 0;
    t.cycInterval = 0;
    t.self.clear();
    t.incl.clear();
    t.stacks.clear();
  }
  g_intervalT0 = t1;
}

std::atomic<bool> g_stop{false};
std::atomic<bool> g_exited{false};

void profMain(int hz, int secs) {
  // Highest priority: a sample must not wait behind the threads it measures.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  SetThreadDescription(GetCurrentThread(), L"SR host profiler");
  // NOT timeBeginPeriod(1): on Windows 10 2004+ that changes the timer quantum of the
  // whole process, i.e. every guest sleep - the profiler would change what it measures.
  // A per-timer high-resolution waitable timer instead (as sr::timebase::PeriodicMs).
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  const DWORD self = GetCurrentThreadId();
  auto ntqit = reinterpret_cast<NtQIT>(
      GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread"));
  refreshModules();
  refreshThreads(ntqit, self);
  g_intervalT0 = nowUs();
  if (g_hitchMode) {
    g_hring.resize(kHRing);
    g_hitchReady.store(true, std::memory_order_release);
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[HOST_PROFILE] sampling at %d Hz, interval %d s -> %s/hprof_*.txt", hz, secs,
              outDir());
  const auto period = std::chrono::microseconds(1000000 / std::max(1, hz));
  auto next = std::chrono::steady_clock::now() + period;
  auto lastRefresh = std::chrono::steady_clock::now();
  auto lastDump = lastRefresh;
  // Busy threshold: ~2% of a tick at ~3.5 GHz.
  const ULONG64 busyCyc = ULONG64(3500000000.0 / double(hz) * 0.02);
  uint64_t tick = 0;
  uint64_t keys[kMaxFrames];
  while (!g_stop.load(std::memory_order_relaxed)) {
    const auto due = next - std::chrono::steady_clock::now();
    if (due.count() > 0) {
      if (timer) {
        LARGE_INTEGER li;
        li.QuadPart = -std::max<int64_t>(
            1, std::chrono::duration_cast<std::chrono::nanoseconds>(due).count() / 100);
        if (SetWaitableTimer(timer, &li, 0, nullptr, nullptr, FALSE))
          WaitForSingleObject(timer, INFINITE);
      } else {
        std::this_thread::sleep_until(next);
      }
    }
    next += period;
    const auto now = std::chrono::steady_clock::now();
    if (now > next + period * 4) next = now + period;  // fell behind: do not burst
    ++tick;
    const bool waitTick = (tick % uint64_t(std::max(1, hz))) == 0;  // idle threads: once a second
    const uint64_t tickUs = g_hitchMode ? nowUs() : 0;
    for (auto& kv : g_th) {
      Th& t = kv.second;
      if (!t.alive || !t.h) continue;
      ULONG64 cyc = 0;
      if (!QueryThreadCycleTime(t.h, &cyc)) continue;
      const ULONG64 d = t.primed ? cyc - t.lastCyc : 0;
      t.lastCyc = cyc;
      t.primed = true;
      t.cycInterval += d;
      const bool busy = d >= busyCyc;
      const bool watched =
          g_hitchMode && (t.watch || kv.first == DWORD(sr::hitch::renderLoopTid()));
      if (!busy && !waitTick && !watched) {
        t.idle++;
        continue;
      }
      if (!t.stackHi) continue;
      if (SuspendThread(t.h) == DWORD(-1)) continue;
      CONTEXT c{};
      c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      int n = 0;
      if (GetThreadContext(t.h, &c)) n = unwind(c, t.stackHi, keys);
      ResumeThread(t.h);
      // The aggregate histograms keep their v1 meaning (busy every tick, idle once a second).
      if (busy || waitTick) record(t, !busy, keys, n);
      else t.idle++;
      if (g_hitchMode && n > 0) hitchPush(tickUs, kv.first, !busy, keys, n);
    }
    if (g_hitchMode) hitchServe();
    if (now - lastRefresh >= std::chrono::seconds(1)) {
      lastRefresh = now;
      refreshModules();
      refreshThreads(ntqit, self);
    }
    if (now - lastDump >= std::chrono::seconds(secs)) {
      lastDump = now;
      dump("periodic", hz);
    }
  }
  if (timer) CloseHandle(timer);
  g_exited.store(true, std::memory_order_release);
}

int g_hz = 0;

void finalDump() {
  static std::atomic<bool> s_done{false};
  if (s_done.exchange(true)) return;
  g_stop.store(true, std::memory_order_relaxed);
  // Let the sampler leave its loop (it never blocks while a thread is suspended).
  for (int i = 0; i < 200 && !g_exited.load(std::memory_order_acquire); ++i) Sleep(5);
  if (g_exited.load(std::memory_order_acquire)) dump("exit", g_hz);
}

}  // namespace

void hostProfileHitch(uint32_t id, uint64_t t0Us, uint64_t t1Us) {
  if (!g_hitchMode || !g_hitchReady.load(std::memory_order_acquire)) return;
  const uint32_t w = g_hreqW.load(std::memory_order_relaxed);
  if (w - g_hreqR.load(std::memory_order_acquire) >= kHReq) return;  // the profiler is behind: drop
  g_hreq[w % kHReq] = HReq{id, t0Us, t1Us};
  g_hreqW.store(w + 1, std::memory_order_release);
}

void startHostProfiler() {
  static std::atomic<bool> s_started{false};
  if (s_started.exchange(true)) return;
  const char* e = std::getenv("SR_HOST_PROFILE_HZ");
  if (!e || !e[0] || e[0] == '0') return;
  g_hz = std::clamp(std::atoi(e), 1, 4000);
  int secs = 10;
  if (const char* s = std::getenv("SR_HOST_PROFILE_SECS")) secs = std::clamp(std::atoi(s), 1, 3600);
  (void)outDir();
  std::atexit(finalDump);
  std::at_quick_exit(finalDump);
  std::thread([secs] { profMain(g_hz, secs); }).detach();
}

}  // namespace sr::perf
