// iter-467 sound lane (diagnostic, observe only): the HOST call stacks of the registered guest
// threads, symbolized from the PDB - the recompiled guest functions are host functions named
// sub_XXXXXXXX, so a native stack names the guest call chain even though the register
// localization (iter-465 codegen) keeps lr/r3 out of the PPCContext the 20 s sampler reads.
//
// Mechanics: SuspendThread -> GetThreadContext -> RtlLookupFunctionEntry/RtlVirtualUnwind into a
// fixed array -> ResumeThread, and only then SymFromAddr (dbghelp may allocate or lock; nothing
// that can block runs while a thread is suspended). Fault-free: RtlVirtualUnwind reads the
// suspended thread's own committed stack; a frame without unwind data ends the walk.
// Callers: kernel/apu/audio_system.cpp SR_AUDIO_CB_PROBE (a render callback running > 60 ms).
#include "guest_context.hpp"
#include "../app/crash_handler.hpp"   // app/log-async: DbgHelpLock
#include "../app/log.hpp"

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace sr::kernel {

extern "C" __declspec(dllexport) GuestThreadSlot g_debugThreadRegistry[64];

namespace {

std::string symName(DWORD64 pc) {
  // app/log-async: DbgHelp is single-threaded - the process-wide DbgHelpLock (app/crash_handler.hpp) replaces
  // this file's own mutex, so the probe never runs beside a minidump or another symbolizer.
  char out[320];
  sr::DbgHelpLock dbg(5000);
  if (!dbg.held()) {
    std::snprintf(out, sizeof(out), "%016llX", (unsigned long long)pc);
    return out;
  }
  static bool s_init = [] {
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
  }();
  (void)s_init;
  char buf[sizeof(SYMBOL_INFO) + 256] = {};
  auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = 255;
  DWORD64 disp = 0;
  if (SymFromAddr(GetCurrentProcess(), pc, &disp, sym))
    std::snprintf(out, sizeof(out), "%s+0x%llX", sym->Name, (unsigned long long)disp);
  else
    std::snprintf(out, sizeof(out), "%016llX", (unsigned long long)pc);
  return out;
}

int captureStack(DWORD tid, DWORD64* pcs, int maxFrames) {
  HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
  if (!h) return 0;
  int n = 0;
  if (SuspendThread(h) != (DWORD)-1) {
    CONTEXT c{};
    c.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(h, &c)) {
      for (; n < maxFrames && c.Rip; ++n) {
        pcs[n] = c.Rip;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (!fe) {   // leaf function: the return address is at [rsp]
          MEMORY_BASIC_INFORMATION mbi{};
          if (!VirtualQuery(reinterpret_cast<void*>(c.Rsp), &mbi, sizeof(mbi)) ||
              mbi.State != MEM_COMMIT)
            break;
          c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
          c.Rsp += 8;
          continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fe, &c, &handlerData, &establisher, nullptr);
      }
    }
    ResumeThread(h);
  }
  CloseHandle(h);
  return n;
}

}  // namespace

void dumpGuestThreadNativeStacks(const char* tag, int maxFrames) {
  if (maxFrames > 32) maxFrames = 32;
  const DWORD self = GetCurrentThreadId();
  for (int i = 0; i < 64; ++i) {
    const GuestThreadSlot& s = g_debugThreadRegistry[i];
    if (!s.tid || s.tid == self) continue;
    DWORD64 pcs[32] = {};
    const int n = captureStack(s.tid, pcs, maxFrames);
    std::string line;
    for (int k = 0; k < n; ++k) {
      if (k) line += " <- ";
      line += symName(pcs[k]);
    }
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[%s] tid=%u name=%s %s", tag, s.tid, s.name[0] ? s.name : "-",
                line.c_str());
  }
}

// step 4 7a (lane D): one host thread by id, registered or not (the [CS_HOLDER_STACK] holder: the thread that last
// entered the lock is not always in g_debugThreadRegistry - set1 2026-10-18 had the holder outside it). Logged as
// name=holder. Same suspend -> unwind -> resume -> symbolize path as above.
void dumpHostThreadNativeStack(const char* tag, uint32_t tid, int maxFrames) {
  if (!tid || tid == GetCurrentThreadId()) return;
  if (maxFrames > 32) maxFrames = 32;
  DWORD64 pcs[32] = {};
  const int n = captureStack(tid, pcs, maxFrames);
  std::string line;
  for (int k = 0; k < n; ++k) {
    if (k) line += " <- ";
    line += symName(pcs[k]);
  }
  SR_LOG_BOOT(sr::LogSeverity::Warn, "[%s] tid=%u name=holder %s", tag, tid, n ? line.c_str() : "(no stack)");
}

}  // namespace sr::kernel
