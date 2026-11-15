// iter-364: hardware write-watchpoint (x64 debug registers Dr0) — names the EXACT
// host store that clobbers an address, race-free.
//
// Why this and not the VEH page-guard: the guest heap page carries concurrent
// writes from other threads; any thread's page-fault globally unprotects the page,
// so the clobbering store slips through the window (SR_ALLOC_WATCH reproduces the
// clobber but logs no hit). A Dr0 data breakpoint is per-thread and address-exact:
// it traps ONLY writes to the watched 8 bytes, on the watched thread only.
//
// Usage (synchronous, on the calling thread):
//   sr::kernel::armHwWriteWatch(hostAddr8);   // arm before the suspect call
//   ...call the code that writes...           // [HWWATCH] prints RIP at the store
//   sr::kernel::disarmHwWriteWatch();
// The write itself still executes (this is a TRAP reported after the instruction,
// EXCEPTION_SINGLE_STEP with DR6 bit0) — we only observe + report, then keep going.

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "../app/log.hpp"
#include "../app/crash_handler.hpp"  // sr::kVehFirst (step4/relcrash: VEH order in ASan builds)

namespace sr { namespace kernel {
uint32_t getCurrentGuestPC();
uint32_t getCurrentGuestR1();
uint32_t getCurrentGuestR3();
} }

namespace {

std::atomic<void*> g_watchAddr{nullptr};
std::atomic<unsigned> g_hits{0};
// iter-380: hit budget before auto-disarm. The default of 4 blinded the queue-slot
// hunt: a byte-wise host memset burns one trap PER BYTE of the watched dword, so
// the whole budget went to a single benign zero-fill and the write that mattered
// (0.3 s later) was never seen. SR_WATCH_SLOT_HITS=<n> raises it per-hunt.
std::atomic<unsigned> g_maxHits{4};
PVOID g_veh = nullptr;
HMODULE g_selfMod = nullptr;

// Set/clear Dr0+Dr7 on a specific thread (must be suspended for a reliable
// SetThreadContext of debug registers).
bool setDebugRegsOnThread(HANDLE th, void* addr /*null = clear*/) {
  CONTEXT c{};
  c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
  if (!GetThreadContext(th, &c)) return false;
  if (addr) {
    c.Dr0 = (DWORD64)addr;
    // Dr7: L0=1 (enable local bp0); RW0=01 (write); LEN0=11 (4 bytes) —
    // 8-byte LEN (10) requires the addr 8-aligned; use 4 bytes twice-safe: our
    // tail word is 4-aligned, and the clobber is a run of zero words, so a 4-byte
    // write watch on the first word catches the run's first store. Bits:
    //   L0   = bit 0
    //   RW0  = bits 16-17 (01 = write)
    //   LEN0 = bits 18-19 (11 = 4 bytes)
    c.Dr7 = (c.Dr7 & ~0xFULL) | 0x1ULL;                 // enable L0, clear G0/rest of low nibble
    c.Dr7 = (c.Dr7 & ~(0xFULL << 16)) | (0x1ULL << 16)  // RW0 = 01 write
                                        | (0x3ULL << 18); // LEN0 = 11 (4 bytes)
    c.Dr6 = 0;
  } else {
    c.Dr0 = 0;
    c.Dr7 &= ~0x1ULL;   // disable L0
    c.Dr6 = 0;
  }
  c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
  return SetThreadContext(th, &c) != 0;
}

struct HelperArgs {
  HANDLE target;
  void* addr;
  volatile LONG done;
  bool ok;
};

DWORD WINAPI helperSetDebug(LPVOID p) {
  auto* a = (HelperArgs*)p;
  SuspendThread(a->target);
  a->ok = setDebugRegsOnThread(a->target, a->addr);
  ResumeThread(a->target);
  InterlockedExchange(&a->done, 1);
  return 0;
}

// A helper thread suspends the caller and programs the debug regs (you cannot
// reliably SetThreadContext(CONTEXT_DEBUG_REGISTERS) on a running thread — least
// of all your own). We block on the helper; SuspendThread on a thread waiting in
// WaitForSingleObject is fine.
bool programSelfDebugRegs(void* addr) {
  HANDLE self = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                       GetCurrentProcess(), &self, THREAD_ALL_ACCESS, FALSE, 0))
    return false;
  HelperArgs a{self, addr, 0, false};
  HANDLE h = CreateThread(nullptr, 0, helperSetDebug, &a, 0, nullptr);
  if (!h) { CloseHandle(self); return false; }
  WaitForSingleObject(h, 5000);
  CloseHandle(h);
  CloseHandle(self);
  return a.ok;
}

LONG CALLBACK hwWatchVeh(EXCEPTION_POINTERS* ep) {
  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT* c = ep->ContextRecord;
  if (!(c->Dr6 & 0x1)) return EXCEPTION_CONTINUE_SEARCH;   // not our Dr0 hit
  c->Dr6 = 0;
  // iter-428f: a Dr0 hit MUST be claimed even after disarm. The all-threads
  // sweep arms Dr0 on every thread, but disarm only cleared the current one:
  // any other thread's next write to the watched dword raised a SINGLE_STEP
  // nobody owned, fell through to the SEH filter, and KILLED that guest
  // thread mid-work (caught live: the movie presenter died inside
  // RenderStateTick's CS 42ms after budget-disarm -> the whole FMV transition
  // deadlock). Stale hits now self-heal: clear this thread's Dr0/Dr7 via the
  // ContextRecord (applied on resume, race-free) and continue.
  if (!g_watchAddr.load(std::memory_order_relaxed)) {
    c->Dr0 = 0;
    c->Dr7 &= ~0x1ULL;
    static std::atomic<unsigned> s_stale{0};
    unsigned k = s_stale.fetch_add(1, std::memory_order_relaxed) + 1;
    if (k <= 8)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[HWWATCH] stale Dr0 hit self-healed (disarm leak) tid=%lu RIP=%p (#%u)",
        GetCurrentThreadId(), (void*)c->Rip, k);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  unsigned n = g_hits.fetch_add(1, std::memory_order_relaxed) + 1;
  uintptr_t rip = (uintptr_t)c->Rip;
  auto modOff = [](uintptr_t addr, char* nameBuf, size_t nb) -> uintptr_t {
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &m) && m) {
      char path[MAX_PATH]; GetModuleFileNameA(m, path, MAX_PATH);
      const char* b = strrchr(path, '\\'); b = b ? b + 1 : path;
      strncpy(nameBuf, b, nb - 1); nameBuf[nb - 1] = 0;
      return addr - (uintptr_t)m;
    }
    strncpy(nameBuf, "?", nb); return 0;
  };
  // The store is a host memset (RIP in a system DLL). The CALLER (return address
  // at [Rsp]) is inside the recompiled allocator — that maps to the guest fn.
  uintptr_t* rsp = (uintptr_t*)c->Rsp;
  char ripMod[64] = {0}, retMod0[64] = {0}, retMod1[64] = {0};
  uintptr_t ripOff = modOff(rip, ripMod, sizeof(ripMod));
  uintptr_t ret0 = 0, ret1 = 0, ret0off = 0, ret1off = 0;
  __try {
    ret0 = rsp[0]; ret1 = rsp[1];
    ret0off = modOff(ret0, retMod0, sizeof(retMod0));
    ret1off = modOff(ret1, retMod1, sizeof(retMod1));
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  // The trap reports AFTER the store, so the watched dword already holds the
  // new value — log it (displayed as the guest big-endian dword) so the hit
  // sequence itself narrates what the slot went through.
  uint32_t newValBE = 0;
  if (void* wa = g_watchAddr.load(std::memory_order_relaxed)) {
    memcpy(&newValBE, wa, 4);
    newValBE = _byteswap_ulong(newValBE);
  }
  if (n <= 24)
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[HWWATCH] #%u WRITE to %p val=%08X | RIP=%s+0x%llX rax=%llX rcx=%llX rdx=%llX | "
      "ret[0]=%s+0x%llX | tid=%lu guestPC=%08X guestR1=%08X guestR3=%08X",
      n, g_watchAddr.load(), newValBE, ripMod, (unsigned long long)ripOff,
      (unsigned long long)c->Rax, (unsigned long long)c->Rcx, (unsigned long long)c->Rdx,
      retMod0, (unsigned long long)ret0off, GetCurrentThreadId(),
      sr::kernel::getCurrentGuestPC(), sr::kernel::getCurrentGuestR1(),
      sr::kernel::getCurrentGuestR3());
  (void)ret1off; (void)retMod1;
  // Disable after the hit budget so we don't single-step-storm.
  // iter-428f: clear THIS thread's regs through the ContextRecord (the old
  // helper-thread self-clear raced the guest's next store — hit #4's memset
  // run trapped again mid-clear). Other threads are cleared by the sweeper's
  // exit sweep; any hit that slips in before then self-heals above.
  if (n >= g_maxHits.load(std::memory_order_relaxed)) {
    g_watchAddr.store(nullptr);
    c->Dr0 = 0;
    c->Dr7 &= ~0x1ULL;
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

namespace sr { namespace kernel {

// iter-388: Dr registers are PER-THREAD; self-arm alone misses writes from
// late-created guest threads (why the 0x9239AAA0 image watch logged ZERO hits
// while the image demonstrably got overwritten). Sweep ALL process threads
// (suspend/set/resume) and re-sweep every second while armed so late-created
// threads get covered too. SR_WATCH_ALL_THREADS=0 opts out.
static void hwWatchSweepOnce(void* addr) {
  const DWORD self = GetCurrentThreadId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{}; te.dwSize = sizeof(te);
  const DWORD pid = GetCurrentProcessId();
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
    HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
    if (!th) continue;
    CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    bool needs = true;
    if (SuspendThread(th) != (DWORD)-1) {
      if (GetThreadContext(th, &c)) {
        // addr set: already armed on this addr -> skip. addr null (clear
        // sweep, iter-428f): only touch threads that still carry our Dr0.
        if (addr) needs = !(c.Dr0 == (DWORD64)addr && (c.Dr7 & 1));
        else      needs = (c.Dr7 & 1) != 0 || c.Dr0 != 0;
      }
      if (needs) setDebugRegsOnThread(th, addr);
      ResumeThread(th);
    }
    CloseHandle(th);
  }
  CloseHandle(snap);
}

static DWORD WINAPI hwWatchSweep(LPVOID) {
  // iter-428f: the sweeper armed Dr0 on EVERY thread, so it must also be the
  // one to take them all off. On disarm (g_watchAddr null) do one final
  // clear sweep instead of exiting with the process still booby-trapped.
  for (;;) {
    void* addr = g_watchAddr.load(std::memory_order_relaxed);
    hwWatchSweepOnce(addr);
    if (!addr) break;
    Sleep(1000);
  }
  return 0;
}

bool armHwWriteWatchHost(void* hostAddr) {
  if (!g_veh) {
    g_selfMod = GetModuleHandleW(nullptr);
    g_veh = AddVectoredExceptionHandler(sr::kVehFirst, hwWatchVeh);   // first = highest priority (last in ASan builds)
    if (!g_veh) return false;
  }
  g_hits.store(0, std::memory_order_relaxed);
  {
    const char* e = std::getenv("SR_WATCH_SLOT_HITS");
    unsigned m = e ? (unsigned)strtoul(e, nullptr, 10) : 4u;
    g_maxHits.store(m ? m : 4u, std::memory_order_relaxed);
  }
  g_watchAddr.store(hostAddr, std::memory_order_relaxed);
  bool ok = programSelfDebugRegs(hostAddr);
  SR_LOG_BOOT(ok ? sr::LogSeverity::Warn : sr::LogSeverity::Error,
    "[HWWATCH] arm Dr0 on %p (4-byte write) -> %s", hostAddr, ok ? "OK" : "FAILED");
  if (!ok) g_watchAddr.store(nullptr);
  if (ok) {
    const char* e = std::getenv("SR_WATCH_ALL_THREADS");
    if (!e || e[0] != '0') {
      // iter-397n: first pass SYNCHRONOUS — a clobber ~100ms after arming beat
      // the 1s-cadence sweep to the writer thread (zero hits on a real write).
      hwWatchSweepOnce(hostAddr);
      HANDLE h = CreateThread(nullptr, 0, hwWatchSweep, nullptr, 0, nullptr);
      if (h) { CloseHandle(h); SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[HWWATCH] all-threads Dr0 armed synchronously + 1s re-sweep"); }
    }
  }
  return ok;
}

void disarmHwWriteWatch() {
  g_watchAddr.store(nullptr, std::memory_order_relaxed);
  programSelfDebugRegs(nullptr);
  // iter-428f: the arm swept Dr0 onto every thread; the disarm must too.
  hwWatchSweepOnce(nullptr);
}

} }  // namespace
#else
#include <cstdint>
namespace sr { namespace kernel {
bool armHwWriteWatchHost(void*) { return false; }
void disarmHwWriteWatch() {}
} }
#endif
