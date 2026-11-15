#pragma once

struct _EXCEPTION_POINTERS;  // <windows.h> EXCEPTION_POINTERS, fwd-declared

namespace sr {

// Install crash handler. Call early in main().
// On Windows: sets unhandled exception filter, writes minidump + last log lines.
void installCrashHandler();

// Path where crash dumps will be written
const char* getCrashDumpPath();

// iter-439: first-occurrence minidump for a guest thread dying inside the
// harness __except (THREAD_FATAL) — that path swallows the exception, so the
// process-level filter above never fires and the fault was unresolvable
// post-mortem. Safe to call from an SEH filter; only the first call writes.
void writeThreadFatalMinidump(::_EXCEPTION_POINTERS* ex);

// iter-460: a diagnostic snapshot of a LIVE process (every thread's stack) -
// for hangs, where nothing faults. Returns true when the file was written.
// step4/relcrash: withHeap = also the host heap (see heapDumpEnabled), whatever SR_CRASH_DUMP_HEAP says.
bool writeDiagnosticMinidump(const char* path, bool withHeap = false);

// step4/relcrash: SR_CRASH_DUMP_HEAP=1 (default OFF; a diagnostic, read once by installCrashHandler). The crash,
// thread-fatal and diagnostic minidumps then also carry the host heap: the VA map (MiniDumpWithFullMemoryInfo), every
// committed readable MEM_PRIVATE region and the writable image pages (ntdll's heap globals for !heap, the exe's
// .data), minus the guest arena [g_debugGuestBase - 4 GB, + 4 GB), write-combined / uncached (GPU) mappings and any
// region over 1 GB, capped at 3.5 GB in total (the memory list's 32-bit file offsets). A full-memory dump would carry
// the guest arena: DbgHelp never calls RemoveMemoryCallback for one and IncludeVmRegionCallback does not drop regions
// (both tested; archive/step4_claude_20261018/relcrash/NOTES.md). Default dumps are unchanged.
bool heapDumpEnabled();

// step4/relcrash: the FirstHandler argument of every host AddVectoredExceptionHandler call. 1 (first) in every
// normal build - unchanged. 0 (last) in an ASan diagnostic build (CMake ASAN=ON defines SR_ASAN_BUILD): on Win64
// AddressSanitizer reserves its shadow memory and commits it page by page from its OWN first-chance VEH. A handler
// registered first after it runs before it, and that handler's instrumented reads of the dispatcher's
// EXCEPTION_POINTERS (fresh stack, uncommitted shadow) fault on the same shadow page again: an endless nested
// dispatch until the thread's stack overflows. That killed both ASan packages at boot (relcrash_asan6a: __chkstk;
// relcrash_asan17a: imageWriteVeh's first line; both just after XamNotifyCreateListener); a standalone repro on the
// VM dies 0xC0000005 with 1 and runs clean with 0 (archive/step4_claude_20261018/relcrash/asan_veh_test). Last in
// the chain, ASan commits its page first and passes every other exception on unchanged (it only claims shadow AVs).
#if defined(SR_ASAN_BUILD)
constexpr unsigned long kVehFirst = 0;
#else
constexpr unsigned long kVehFirst = 1;
#endif

// app/log-async (docs/TOOLKIT.md 3.2.5): DbgHelp is single-threaded ("all DbgHelp functions, such as
// this one, are single threaded": MiniDumpWriteDump, SymInitialize, SymFromAddr, ...), so every DbgHelp
// call in the host holds this one process-wide lock - the crash filter, the thread-fatal and diagnostic
// dumps, the hang watchdog's dump helpers, the image-write VEH's symbolizer, the native stack probe.
// It is a plain owner word (no CRT lock, no heap: usable in a crash filter): the constructor polls every
// 1 ms for at most timeoutMs (0xFFFFFFFF = no bound) and REFUSES at once when the calling thread already
// holds it (a fault inside DbgHelp reaching a filter that would call DbgHelp again). held() says whether
// this scope got it; a caller that did not skips its DbgHelp call.
class DbgHelpLock {
public:
  explicit DbgHelpLock(unsigned timeoutMs);
  ~DbgHelpLock();
  DbgHelpLock(const DbgHelpLock&) = delete;
  DbgHelpLock& operator=(const DbgHelpLock&) = delete;
  bool held() const { return held_; }

private:
  bool held_ = false;
};

}  // namespace sr
