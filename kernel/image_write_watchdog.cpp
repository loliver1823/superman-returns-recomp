// iter-23 image-write watchdog: VirtualProtect a 4 KB page containing the
// audio class vtable (guest VA 0x8209A458) to PAGE_READONLY, then install
// a Vectored Exception Handler that catches writes to that page, logs the
// host RIP (resolved to module+offset) plus a few stack-frame return
// addresses, and unprotects the page on first hit so the program can
// continue.
//
// The point is to identify which recompiled function is doing the
// out-of-bounds memcpy that corrupts the audio class vtable. Once we know
// the host RIP we can read the corresponding ppc_recomp.NN.cpp:line and
// trace it back to the guest function and call site.
//
// Why a Vectored Exception Handler vs SEH: the existing SEH filter in
// guest_harness.cpp terminates the process on AV. We need to log + continue,
// and we want to be able to trigger this from any thread (the rogue write
// might come from the main guest thread or a worker). VEH runs first, on
// any thread, for any exception; we return EXCEPTION_CONTINUE_EXECUTION
// after handling so the recomp continues normally.

#include "../app/crash_handler.hpp"   // app/log-async: DbgHelpLock
#include "../app/log.hpp"
#include "guest_context.hpp"
#include "guest_protect.hpp"
#include "native_hardware_memory.hpp"
#include "sr_oracle.hpp"
#include "../guest/ppc/ppc_context.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "dbghelp.lib")
#endif

// iter-458 H-VQ (Astra 035): every image protection change bumps the
// hostRangeReadable cache generation. RO<->RW keeps the pages readable, so this
// is belt-and-braces; the bump is one atomic and safe inside the VEH.
// 2026-10-14 (transport pair admission A): every protection change made here is declared to kernel/guest_protect.hpp
// first, unconditionally (lock-free, safe inside the VEH; a range declared again takes no new slot, so declaring the
// read-write restores too costs nothing and keeps the funnel rule of tools/difftest/guest_protect_census.py simple).
// The always-on boot page of the default mode (g_bootPageHost, set by installImageWriteWatchdog) is a declared range;
// every opt-in mode - SR_WRITE_WATCH_VA, SR_IMAGE_RO, a runtime arm - excludes all arithmetic store admission.
static uintptr_t g_bootPageHost = 0;
static sr::kernel::guest_protect::Scope wdScope(LPVOID addr, SIZE_T size) {
  return g_bootPageHost && reinterpret_cast<uintptr_t>(addr) == g_bootPageHost && size == 0x1000
             ? sr::kernel::guest_protect::Scope::Range : sr::kernel::guest_protect::Scope::All;
}
static BOOL wdProtect(LPVOID addr, SIZE_T size, DWORD prot, PDWORD old) {
  sr::kernel::guest_protect::declare(addr, size, wdScope(addr, size));
#if !SR_ORACLE
  if (sr::kernel::nativeHardwareMemoryIntersects(addr, size)) {
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }
#endif
  sr::kernel::hostRangeReadableBeginRemoval();
  const BOOL r = VirtualProtect(addr, size, prot, old);
  sr::kernel::hostRangeReadableEndRemoval();
  return r;
}

namespace sr {
namespace kernel {

namespace {

#if defined(_WIN32)
uintptr_t g_protectedStart = 0;
uintptr_t g_protectedEnd = 0;
uint32_t  g_protectedGuestVA = 0;
std::atomic<unsigned> g_writeHits{0};
PVOID     g_vehHandle = nullptr;
// Snapshot of the protected page at boot, taken right after loadXexImage so
// the bytes are clean. Used by restoreImageWatchdogPage() to re-fill the page
// before audio bootstrap reads from it.
uint8_t   g_pageSnapshot[0x1000] = {};
bool      g_snapshotValid = false;

// Full image snapshot for the broader restore. Sized at 8 MB to cover the
// .text/.rdata region (which holds all vtables, function pointer tables,
// and string constants used across the engine). Stops short of the .data
// region (~0x829A0000+) which contains runtime-mutable globals (FM module
// list, audio client pointer, allocator pointers) that we must NOT restore.
//
// The image is 10 MB total (0x82000000..0x829E0000). 8 MB covers the
// immutable section; the last 2 MB is left untouched by restore.
constexpr uint32_t kImageSnapshotBaseVA = 0x82000000u;
constexpr size_t   kImageSnapshotSize   = 0x00800000u;  // 8 MB covers .text + .rdata
std::unique_ptr<uint8_t[]> g_imageSnapshot;
bool g_imageSnapshotValid = false;
std::atomic<bool> g_periodicRestoreRunning{false};
std::atomic<bool> g_periodicRestoreEnabled{true};
std::atomic<uint64_t> g_lastRestoreCorruptedWords{0};

// iter-323: env-target mode. SR_WRITE_WATCH_VA=<hex guest VA> retargets the
// watchdog at an arbitrary page (e.g. the GameKernelMgr cells at 0x8296D8D8
// whose zeroing kills the sim loop). Hot .data pages get constant legit
// writes, so unlike the legacy single-shot mode this one re-arms: unprotect,
// TF-single-step the faulting store, re-protect on the step exception.
// Full context logs only for writes landing on [VA, VA+SR_WRITE_WATCH_LEN);
// other writes in the page get a throttled one-liner.
uint32_t g_envWatchVA  = 0;   // 0 = legacy audio-vtable mode
uint32_t g_envWatchLen = 4;
// iter-462 SR_IMAGE_RO=1 (DIAGNOSTIC, default off): the XEX read-only data
// section [0x82000000, 0x820F0000) - every vtable - is made PAGE_READONLY the
// way the console and Xenia map it (xex_module.cc:1103-1110, page
// descriptors of default.xex: RODATA 82000000-820F0000, CODE -82910000,
// DATA -829C0000, RODATA -829E0000). Each write is logged with host RIP,
// callers and the guest back-chain, then single-stepped and re-protected.
// Built to name the writer that turns pool free lists into .rdata vtables
// (the power-select death at 0x8208D29C, the freeze precursors). The
// periodic image restore and the audio-vtable page restore are OFF in this
// mode - they would fault, and they hide exactly what this looks for.
constexpr uint32_t kRodataVA  = 0x82000000u;
constexpr uint32_t kRodataLen = 0x000F0000u;
unsigned g_rearmTargetBudget = 12;
bool imageRoDiag() {
  static const bool v = [] {
    const char* e = std::getenv("SR_IMAGE_RO");
    return e && e[0] == '1';
  }();
  return v;
}
std::atomic<unsigned> g_targetHits{0};
std::atomic<unsigned> g_noiseHits{0};
thread_local bool t_rearmPending = false;

LONG WINAPI imageWriteVeh(EXCEPTION_POINTERS* info) {
  if (!info || !info->ExceptionRecord || !info->ContextRecord)
    return EXCEPTION_CONTINUE_SEARCH;
  // iter-323 re-arm: after we unprotect + TF-step a faulting store, the very
  // next instruction raises SINGLE_STEP on the same thread — re-protect and
  // resume. Only consume steps WE armed (t_rearmPending), so foreign
  // single-steps (debuggers/Frida) pass through.
  if (info->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
    if (g_envWatchVA && t_rearmPending) {
      t_rearmPending = false;
      DWORD op = 0;
      wdProtect((LPVOID)g_protectedStart, g_protectedEnd - g_protectedStart,
                     PAGE_READONLY, &op);
      return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;
  if (info->ExceptionRecord->NumberParameters < 2)
    return EXCEPTION_CONTINUE_SEARCH;

  ULONG_PTR accessType = info->ExceptionRecord->ExceptionInformation[0];
  ULONG_PTR dataAddr   = info->ExceptionRecord->ExceptionInformation[1];

  // Only handle writes inside the protected page; everything else falls
  // through to whatever SEH is set up for the thread.
  if (accessType != 1) return EXCEPTION_CONTINUE_SEARCH;
  if (dataAddr < g_protectedStart || dataAddr >= g_protectedEnd)
    return EXCEPTION_CONTINUE_SEARCH;

  PCONTEXT ctx = info->ContextRecord;
  unsigned n = g_writeHits.fetch_add(1, std::memory_order_relaxed) + 1;

  // iter-357l: when the faulting RIP is inside a CRT (memset/memcpy), the RIP
  // alone is useless — every host memset looks the same. Scan the faulting
  // stack for the first return addresses that land in OUR module: that names
  // the caller (recomp body or kernel shim) which asked for the write.
  // Fault-free by construction: bounded scan, VirtualQuery-checked.
  char callerBuf[192] = {};
  {
    HMODULE hSelf = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&imageWriteVeh, &hSelf);
    MODULEINFO mi = {};
    if (hSelf) GetModuleInformation(GetCurrentProcess(), hSelf, &mi, sizeof(mi));
    const uintptr_t lo = (uintptr_t)mi.lpBaseOfDll;
    const uintptr_t hi = lo + mi.SizeOfImage;
    if (lo && ctx->Rsp) {
      size_t off = 0;
      int found = 0;
      MEMORY_BASIC_INFORMATION mbi = {};
      if (VirtualQuery((LPCVOID)ctx->Rsp, &mbi, sizeof(mbi)) &&
          (mbi.State == MEM_COMMIT)) {
        const uintptr_t stackEnd =
            (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        for (uintptr_t p = (uintptr_t)ctx->Rsp;
             p + 8 <= stackEnd && p < (uintptr_t)ctx->Rsp + 1024 && found < 3;
             p += 8) {
          const uintptr_t v = *(const uintptr_t*)p;
          if (v > lo && v < hi) {
            off += (size_t)snprintf(callerBuf + off, sizeof(callerBuf) - off,
                                    "%s+0x%llX ", found ? "" : "callers:",
                                    (unsigned long long)(v - lo));
            ++found;
            if (off >= sizeof(callerBuf) - 24) break;
          }
        }
      }
    }
  }

  // Resolve the faulting RIP to module + offset.
  HMODULE hMod = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCSTR)ctx->Rip, &hMod);
  char modName[MAX_PATH] = {};
  if (hMod) GetModuleFileNameA(hMod, modName, sizeof(modName));
  uintptr_t modOffset = hMod ? (uintptr_t)ctx->Rip - (uintptr_t)hMod : 0;

  // Compute the guest VA being written: dataAddr - guest_base.
  uint8_t* base = nullptr;
  size_t size = 0;
  getGlobalGuestMemory(&base, &size);
  uintptr_t guestVA = (uintptr_t)dataAddr - (uintptr_t)base;

  // iter-323 env-target mode: full context only for the watched cells;
  // other writes to the (hot) page get a throttled one-liner.
  bool fullLog = (n <= 50);
  if (g_envWatchVA) {
    const bool isTarget = guestVA >= g_envWatchVA &&
                          guestVA < (uintptr_t)g_envWatchVA + g_envWatchLen;
    if (isTarget) {
      // iter-347c: cap raised 12→64 — boot-time LEGIT registrations consumed
      // the old cap, so the real mid-boot wipe writes (t≈116s) were never
      // full-context logged (cost a false 'second boot' theory).
      fullLog = g_targetHits.fetch_add(1, std::memory_order_relaxed) < 64;
    } else {
      // iter-348d: the wipe is a MULTI-MB sweep (pair-move shift or giant
      // memset) that crosses the whole watched page bottom-up. Its stores in
      // the approach ramp just below the target window are the FIRST place
      // its loop registers (src/dst/count, and L for the Lua sweep) are
      // visible — full-log a few of those instead of one-lining everything.
      const bool ramp = guestVA + 0x200 >= g_envWatchVA && guestVA < g_envWatchVA;
      static std::atomic<unsigned> s_rampHits{0};
      if (ramp && s_rampHits.fetch_add(1, std::memory_order_relaxed) < 12) {
        fullLog = true;
      } else {
        fullLog = false;
        unsigned nn = g_noiseHits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (nn <= 10 || (nn % 2000) == 0)
          SR_LOG_BOOT(sr::LogSeverity::Info,
            "[IMAGE_WRITE_VEH] noise #%u write guestVA=0x%08llX tid=%lu RIP=%s+0x%llX",
            nn, (unsigned long long)guestVA, GetCurrentThreadId(),
            modName, (unsigned long long)modOffset);
      }
    }
  }

  if (fullLog) {
    // Pull main-thread guest registers directly from setCurrentGuestContext.
    // If the writer is the main thread, these are LIVE values at the moment
    // of fault — definitive vs. inferring r1 from store offsets.
    uint32_t gr1  = sr::kernel::getCurrentGuestR1();
    uint32_t gpc  = sr::kernel::getCurrentGuestPC();
    uint32_t gctr = sr::kernel::getCurrentGuestCtr();
    uint32_t gr3  = sr::kernel::getCurrentGuestR3();
    uint32_t gr31 = sr::kernel::getCurrentGuestR31();
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[IMAGE_WRITE_VEH] #%u WRITE at guest VA 0x%llX (host 0x%llX): tid=%lu RIP=0x%llX %s+0x%llX RBP=0x%llX RSP=0x%llX %s",
      n, (unsigned long long)guestVA, (unsigned long long)dataAddr,
      GetCurrentThreadId(),
      (unsigned long long)ctx->Rip, modName, (unsigned long long)modOffset,
      (unsigned long long)ctx->Rbp, (unsigned long long)ctx->Rsp, callerBuf);
    // Callee-saved regs (r28-r31) preserve their entry values across calls,
    // so r30 (commonly used as the function's `this` pointer) holds the
    // original r3 passed by the caller. r3/r4/r5 are volatile and may have
    // been overwritten by the time we see the fault.
    uint32_t gr30 = sr::kernel::getCurrentGuestRegAtOffset(offsetof(PPCContext, r30));
    uint32_t gr29 = sr::kernel::getCurrentGuestRegAtOffset(offsetof(PPCContext, r29));
    uint32_t gr28 = sr::kernel::getCurrentGuestRegAtOffset(offsetof(PPCContext, r28));
    uint32_t gr27 = sr::kernel::getCurrentGuestRegAtOffset(offsetof(PPCContext, r27));
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[IMAGE_WRITE_VEH]   guest regs (writer thread): r1=0x%08X lr=0x%08X ctr=0x%08X r3=0x%08X r31=0x%08X r30=0x%08X r29=0x%08X r28=0x%08X r27=0x%08X",
      gr1, gpc, gctr, gr3, gr31, gr30, gr29, gr28, gr27);

    // iter-343e: the zeroer is an AS-VM property-table op on an object whose
    // buffer field (+0x10) is in-image garbage. Dump the object candidates:
    // any callee-saved reg holding a plausible heap VA gets a 0x40-byte
    // neighborhood dump (fault-free, bounds-checked) so we can read the
    // corrupt object's fields {buf@+0x10, count@+0x1C} and its vtable.
    if (base) {
      struct RegVal { const char* name; uint32_t v; };
      RegVal cand[] = {{"r27", gr27}, {"r28", gr28}, {"r29", gr29},
                       {"r30", gr30}, {"r31", gr31}, {"r3", gr3}};
      for (const auto& rv : cand) {
        if (rv.v >= 0x84000000u && rv.v < 0x9F000000u && rv.v + 0x40 <= size) {
          const uint8_t* p = (const uint8_t*)base + rv.v;
          char hx[3 * 0x40 + 1];
          int o = 0;
          for (int i = 0; i < 0x40 && o < (int)sizeof(hx) - 4; ++i)
            o += snprintf(hx + o, sizeof(hx) - o, "%02X ", p[i]);
          hx[o] = '\0';
          SR_LOG_BOOT(sr::LogSeverity::Error,
            "[IMAGE_WRITE_VEH]   objdump %s=0x%08X: %s", rv.name, rv.v, hx);
        }
      }
    }

    // iter-348c: decode the ACTUAL store — host GPRs (cursors live in host
    // regs mid-loop) + 24 code bytes at RIP (identifies the instruction and
    // its operands), + raw guest r14-r26 (the guest-side cursor/count regs
    // the r27-r31 line misses). Ground truth beats loop-shape speculation.
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[IMAGE_WRITE_VEH]   host GPRs: rax=%llX rbx=%llX rcx=%llX rdx=%llX rsi=%llX rdi=%llX r8=%llX r9=%llX r10=%llX r11=%llX r12=%llX r13=%llX r14=%llX r15=%llX",
      (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rbx,
      (unsigned long long)ctx->Rcx, (unsigned long long)ctx->Rdx,
      (unsigned long long)ctx->Rsi, (unsigned long long)ctx->Rdi,
      (unsigned long long)ctx->R8, (unsigned long long)ctx->R9,
      (unsigned long long)ctx->R10, (unsigned long long)ctx->R11,
      (unsigned long long)ctx->R12, (unsigned long long)ctx->R13,
      (unsigned long long)ctx->R14, (unsigned long long)ctx->R15);
    {
      const uint8_t* ip = (const uint8_t*)ctx->Rip;
      char cb[3 * 24 + 1]; int co = 0;
      MEMORY_BASIC_INFORMATION mbi{};
      if (VirtualQuery(ip, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
          !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        for (int i = 0; i < 24; ++i)
          co += snprintf(cb + co, sizeof(cb) - co, "%02X ", ip[i]);
      }
      cb[co] = '\0';
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[IMAGE_WRITE_VEH]   code@RIP: %s", cb);
    }
    {
      char gr[16 * 13 + 1]; int go = 0;
      for (int ri = 14; ri <= 26; ++ri) {
        uint32_t v = sr::kernel::getCurrentGuestRegAtOffset(
            offsetof(PPCContext, r14) + (ri - 14) * sizeof(PPCRegister));
        go += snprintf(gr + go, sizeof(gr) - go, " r%d=%08X", ri, v);
        if (go >= (int)sizeof(gr) - 16) break;
      }
      gr[go] = '\0';
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[IMAGE_WRITE_VEH]   guest r14-r26:%s", gr);
    }

    // iter-348: LUA_STATE SNIFFER. The wipe writer is Lua's result-move
    // (poscall/luaV) writing through L->stack/ci fields that point at the
    // descriptor table. Lua internals are intra-TU (strong-defs blind), but
    // the VEH sees the writer's callee-saved regs — find L among them by
    // shape (nCcalls u16 @+0x2E sane, global ptr @+0x10 in heap) and dump
    // its first 0x60 bytes: stack/base/top/ci land in the log for decode.
    if (base) {
      auto rdg32 = [&](uint32_t va) -> uint32_t {
        if (va < 0x10000u || (size_t)va + 4 > size) return 0;
        const uint8_t* p = base + va;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
               (uint32_t(p[2]) << 8) | uint32_t(p[3]);
      };
      auto heapish = [](uint32_t v) { return v >= 0x84000000u && v < 0x9F000000u; };
      for (int ri = 14; ri <= 31; ++ri) {
        uint32_t v = sr::kernel::getCurrentGuestRegAtOffset(
            offsetof(PPCContext, r14) + (ri - 14) * sizeof(PPCRegister));
        if (!heapish(v) || (size_t)v + 0x60 > size) continue;
        const uint8_t* p = (const uint8_t*)base + v;
        uint16_t nCcalls = (uint16_t(p[0x2E]) << 8) | p[0x2F];
        uint32_t gptr = rdg32(v + 0x10);
        if (nCcalls > 300 || !heapish(gptr)) continue;
        char hx[3 * 0x60 + 1];
        int o = 0;
        for (int i = 0; i < 0x60 && o < (int)sizeof(hx) - 4; ++i)
          o += snprintf(hx + o, sizeof(hx) - o, "%02X ", p[i]);
        hx[o] = '\0';
        SR_LOG_BOOT(sr::LogSeverity::Error,
          "[IMAGE_WRITE_VEH]   [LUA_STATE?] r%d=0x%08X nCcalls=%u g=0x%08X dump: %s",
          ri, v, nCcalls, gptr, hx);
      }
    }

    // Symbolize via DbgHelp to identify the function name (and optionally
    // file:line). Init lazily — SymInitialize may already be active from
    // crash_handler_win.
    auto symbolize = [&](uintptr_t addr) {
      // app/log-async: DbgHelp is single-threaded (app/crash_handler.hpp DbgHelpLock); inside this VEH it is
      // waited for at most 1 s, and skipped when another thread holds it longer (or this one does).
      sr::DbgHelpLock dbg(1000);
      if (!dbg.held()) {
        SR_LOG_BOOT(sr::LogSeverity::Error, "[IMAGE_WRITE_VEH]   sym: (DbgHelp busy) 0x%llX",
                    (unsigned long long)addr);
        return;
      }
      static std::atomic<bool> s_init{false};
      if (!s_init.exchange(true)) {
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
      }
      char buf[sizeof(SYMBOL_INFO) + 512] = {};
      auto* sym = (SYMBOL_INFO*)buf;
      sym->SizeOfStruct = sizeof(SYMBOL_INFO);
      sym->MaxNameLen = 511;
      DWORD64 disp = 0;
      const char* name = "<no-symbol>";
      if (SymFromAddr(GetCurrentProcess(), addr, &disp, sym)) {
        name = sym->Name;
      }
      // Lookup file:line.
      IMAGEHLP_LINE64 line = {};
      line.SizeOfStruct = sizeof(line);
      DWORD lineDisp = 0;
      const char* file = "?";
      DWORD lineNo = 0;
      if (SymGetLineFromAddr64(GetCurrentProcess(), addr, &lineDisp, &line)) {
        file = line.FileName ? line.FileName : "?";
        lineNo = line.LineNumber;
      }
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[IMAGE_WRITE_VEH]   sym: %s+0x%llX  at %s:%lu",
        name, (unsigned long long)disp, file, lineNo);
    };

    symbolize(ctx->Rip);

    // iter-343h: GUEST BACKTRACE of the writer thread. The VEH executes ON the
    // faulting thread, so the TLS guest context (r1/lr) is the writer's own.
    // Walk the PPC back-chain: bc=*(r1); savedLR=*(bc-8); fp=bc. Fault-free
    // (bounds-checked). Names the full guest call chain of the write.
    if (base && gr1 >= 0x10000u && (size_t)gr1 + 8 <= size) {
      auto rdg = [&](uint32_t va) -> uint32_t {
        if (va < 0x10000u || (size_t)va + 4 > size) return 0;
        const uint8_t* p = base + va;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
               (uint32_t(p[2]) << 8) | uint32_t(p[3]);
      };
      uint32_t fp = gr1;
      char bt[256]; int o = 0;
      for (int d = 0; d < 6 && fp; ++d) {
        uint32_t bc = rdg(fp);
        if (bc <= fp || bc - fp > 0x100000u) break;   // sane ascending chain
        uint32_t slr = rdg(bc - 8);
        o += snprintf(bt + o, sizeof(bt) - o, " [%d]lr=%08X", d, slr);
        if (o >= (int)sizeof(bt) - 24) break;
        fp = bc;
      }
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[IMAGE_WRITE_VEH]   guest backtrace (writer):%s", o ? bt : " <none>");
    }

    // iter-343e: name the GUEST function containing the faulting host RIP by
    // inverting the recomp dispatch table (largest host fn-ptr <= RIP wins).
    // No PDB needed; ~2.6M table loads, capped to the first few traps.
    {
      // iter-348: the first-6 cap skipped the ACTUAL wipe hits (#784/#981 —
      // both morphologies went un-inverted; cost two speculation loops).
      // Always invert for writes INSIDE the env-watch window; first-6 for
      // the rest of the page.
      static std::atomic<unsigned> s_inv{0};
      const bool onTarget = g_envWatchVA && guestVA >= g_envWatchVA &&
                            guestVA < (uintptr_t)g_envWatchVA + g_envWatchLen;
      if (s_inv.fetch_add(1, std::memory_order_relaxed) < 6 || onTarget ||
          fullLog) {  // iter-348d: ramp captures need naming too
        uintptr_t rip = (uintptr_t)ctx->Rip;
        uintptr_t bestFn = 0;
        uint32_t bestVA = 0;
        for (uint32_t va = (uint32_t)PPC_CODE_BASE;
             va < (uint32_t)(PPC_CODE_BASE + PPC_CODE_SIZE); va += 4) {  // exact dispatch-table bounds (0x82000000 start underflowed it)
          PPCFunc* fn = PPC_LOOKUP_FUNC(base, va);
          uintptr_t f = (uintptr_t)fn;
          if (f && f <= rip && f > bestFn) { bestFn = f; bestVA = va; }
        }
        SR_LOG_BOOT(sr::LogSeverity::Error,
          "[IMAGE_WRITE_VEH]   guest fn for RIP: sub_%08X (host fn 0x%llX, RIP offset +0x%llX)",
          bestVA, (unsigned long long)bestFn,
          (unsigned long long)(rip - bestFn));
      }
    }

    // Walk the GUEST stack to find caller LRs. The faulting store was at
    // guest VA `dataAddr - host_base`, which equals (guest_r1 + 80) for
    // sub_822D2990's frame layout. So guest_r1 = guest_va - 80, original
    // caller's r1 = guest_r1 + 144 (sub_822D2990 allocates 144 bytes via
    // `stwu r1,-144(r1)`), and the caller's saved LR sits at
    // (caller_r1 - 8). Read it to identify the caller.
    if (base) {
      uint32_t guest_r1 = (uint32_t)guestVA - 80;
      uint32_t caller_r1 = guest_r1 + 144;
      auto rdGuest32 = [&](uint32_t va) -> uint32_t {
        if (va + 4 > size) return 0;
        uint8_t* h = base + va;
        return (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) |
               (uint32_t(h[2]) <<  8) |  uint32_t(h[3]);
      };
      uint32_t saved_lr = rdGuest32(caller_r1 - 8);
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[IMAGE_WRITE_VEH]   guest stack: r1(callee)=0x%08X r1(caller)=0x%08X saved_lr=0x%08X",
        guest_r1, caller_r1, saved_lr);
      // Walk further: the caller's caller's LR is at *(caller_r1 + caller_frame_size - 8).
      // We don't know the caller's frame size, so dump 16 qwords from caller_r1 to inspect.
      for (int i = 0; i < 16; ++i) {
        uint32_t va = caller_r1 + i * 8;
        uint32_t v_hi = rdGuest32(va);
        uint32_t v_lo = rdGuest32(va + 4);
        if (v_hi >= 0x82000000u && v_hi < 0x82A00000u) {
          SR_LOG_BOOT(sr::LogSeverity::Error,
            "[IMAGE_WRITE_VEH]   gstack[%d] @0x%08X = 0x%08X 0x%08X (looks like guest VA)",
            i, va, v_hi, v_lo);
        }
      }
    }

    // Use CaptureStackBackTrace for a proper unwound host call stack —
    // x64 frames don't follow simple RSP+N return-address patterns due
    // to PDATA-based unwinding, so the heuristic walk below misses
    // most frames. Symbolize each captured frame so we see the FULL
    // host call chain leading to the bad-r1 write.
    {
      void* frames[64] = {};
      USHORT nFrames = CaptureStackBackTrace(0, 64, frames, nullptr);
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[IMAGE_WRITE_VEH]   --- host backtrace (%u frames) ---", nFrames);
      for (USHORT i = 0; i < nFrames; ++i) {
        uintptr_t addr = (uintptr_t)frames[i];
        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &m);
        char modN[MAX_PATH] = {};
        if (m) GetModuleFileNameA(m, modN, sizeof(modN));
        const char* base = strrchr(modN, '\\');
        const char* tag = base ? base + 1 : modN;
        uintptr_t off = m ? (addr - (uintptr_t)m) : 0;
        SR_LOG_BOOT(sr::LogSeverity::Error,
          "[IMAGE_WRITE_VEH]   bt[%2u] 0x%llX (%s+0x%llX)",
          i, (unsigned long long)addr, tag, (unsigned long long)off);
        if (m == GetModuleHandleA(nullptr)) symbolize(addr);
      }
      SR_LOG_BOOT(sr::LogSeverity::Error, "[IMAGE_WRITE_VEH]   --- end backtrace ---");
    }

    // Legacy heuristic walk — kept as a fallback for cases where
    // CaptureStackBackTrace misses frames (rare on x64 with proper PDATA).
    HMODULE exeBase = GetModuleHandleA(nullptr);
    for (int i = 0; i < 24; ++i) {
      uintptr_t val = 0;
      __try { val = *(uintptr_t*)(ctx->Rsp + i * 8); }
      __except (EXCEPTION_EXECUTE_HANDLER) { val = 0xDEAD; }
      if (val > 0x7FF600000000ULL && val < 0x7FFFFFFFFFFF00ULL) {
        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)val, &m);
        uintptr_t off = m ? (val - (uintptr_t)m) : 0;
        const char* tag = (m == exeBase) ? "EXE" : (m ? "DLL" : "?");
        SR_LOG_BOOT(sr::LogSeverity::Error,
          "[IMAGE_WRITE_VEH]   stack[%d] 0x%llX (%s+0x%llX)",
          i, (unsigned long long)val, tag, (unsigned long long)off);
        if (m == exeBase) symbolize(val);
      }
    }
  }

  // Unprotect the page so the faulting store can execute.
  DWORD oldProtect = 0;
  wdProtect((LPVOID)g_protectedStart,
                 g_protectedEnd - g_protectedStart,
                 PAGE_READWRITE, &oldProtect);
  // iter-323 env-target mode: TF-single-step this store, re-protect on the
  // step exception — the watch stays armed across legit page traffic. Stop
  // re-arming once the target has been fully captured or the noise budget
  // is blown (leaves the page RW = legacy behavior).
  if (g_envWatchVA &&
      g_targetHits.load(std::memory_order_relaxed) < g_rearmTargetBudget &&
      n < 500000) {
    ctx->EFlags |= 0x100;   // trap flag
    t_rearmPending = true;
  } else if (n <= 1 || g_envWatchVA) {
    static std::atomic<bool> s_final{false};
    if (!s_final.exchange(true))
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[IMAGE_WRITE_VEH] page unprotected permanently (hits=%u targetHits=%u); continuing",
        n, g_targetHits.load());
  }

  // Re-execute the faulting instruction (now succeeds with PAGE_READWRITE).
  return EXCEPTION_CONTINUE_EXECUTION;
}
#endif  // _WIN32

}  // namespace

// iter-463 charter: restoreImageWatchdogPage / restoreImageRange (one-shot snapshot
// restores for the deleted bootstrapAudioClient) were deleted with their only caller.

// Periodic-restore timer thread: every kPeriodMs, memcpy the image snapshot
// back into guest memory. Image data corruption from various engine paths
// (fiber stacks landing in .bss, register-aliasing mismatches, etc.) is
// treated as a fact-of-life — we restore aggressively rather than try to
// pin every offending function. The restore window must be small enough
// that downstream consumers (vtable dispatch, function-pointer lookups)
// see clean data when they read.
//
// Trade-off: if a function is mid-write to a snapshot-covered VA when the
// restore fires, we'll silently undo its write. For .text/.rdata (read-only
// by intent on real Xbox 360) this is fine — the engine never SHOULD be
// writing there. For .data (runtime-mutable globals) we'd corrupt state —
// hence the snapshot is bounded to 8 MB to avoid the 0x829Axxxx region.
void startPeriodicImageRestore() {
#if defined(_WIN32)
  // iter-463 charter: FORCING (reverts guest writes to the image), so master-
  // gated at the launch. The console and Xenia map CODE/RODATA read-only
  // instead (xex_module.cc:1103-1110); SR_IMAGE_RO=1 is that diagnostic.
  if (!sr::kernel::forcingAllowed())
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[IMAGE_WRITE_VEH] periodic image restore NOT started (forcing, needs SR_ALLOW_FORCING=1)");
  if (!sr::kernel::forcingAllowed()) return;
  if (imageRoDiag()) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[IMAGE_RO] periodic image restore NOT started (it would fault and hide the writers)");
    return;
  }
  if (g_periodicRestoreRunning.exchange(true)) return;

  std::thread([]() {
    constexpr int kPeriodMs = 1000;  // 1 Hz restore
    // Wait for snapshot to exist (installImageWriteWatchdog must run first).
    for (int i = 0; i < 1000 && !g_imageSnapshotValid; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!g_imageSnapshotValid) return;
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[IMAGE_WRITE_VEH] periodic-restore thread up — restoring 8 MB image every %d ms",
      kPeriodMs);

    while (g_periodicRestoreEnabled.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kPeriodMs));
      if (!g_periodicRestoreEnabled.load(std::memory_order_acquire)) break;
      uint8_t* base = nullptr;
      size_t size = 0;
      getGlobalGuestMemory(&base, &size);
      if (!base) continue;

      uint8_t* imgStart = base + kImageSnapshotBaseVA;
      // Compare-and-restore. Only count + log on rounds where corruption
      // actually existed, so the log doesn't fill with no-op restore lines.
      size_t diffWords = 0;
      const uint32_t* cur = reinterpret_cast<const uint32_t*>(imgStart);
      const uint32_t* snap = reinterpret_cast<const uint32_t*>(g_imageSnapshot.get());
      size_t nWords = kImageSnapshotSize / 4;
      for (size_t i = 0; i < nWords; ++i) {
        if (cur[i] != snap[i]) ++diffWords;
      }
      if (diffWords > 0) {
        // Temporarily unprotect the narrow watchdog page so the memcpy
        // doesn't fault on crossing it.
        DWORD oldProtect = 0;
        if (g_protectedStart != 0) {
          wdProtect((LPVOID)g_protectedStart, g_protectedEnd - g_protectedStart,
                         PAGE_READWRITE, &oldProtect);
        }
        std::memcpy(imgStart, g_imageSnapshot.get(), kImageSnapshotSize);
        if (g_protectedStart != 0) {
          DWORD prev = 0;
          wdProtect((LPVOID)g_protectedStart, g_protectedEnd - g_protectedStart,
                         PAGE_READONLY, &prev);
        }
        static std::atomic<unsigned> s_pulses{0};
        unsigned p = s_pulses.fetch_add(1, std::memory_order_relaxed) + 1;
        // Log first 3 pulses then every 10th to avoid log spam.
        if (p <= 3 || (p % 10 == 0)) {
          SR_LOG_BOOT(sr::LogSeverity::Info,
            "[IMAGE_WRITE_VEH] periodic-restore pulse #%u: %zu differing words restored",
            p, diffWords);
        }
        g_lastRestoreCorruptedWords.store(diffWords, std::memory_order_relaxed);
      }
    }
  }).detach();
#endif
}

// iter-60: stop the periodic image restore. Call after WALK_END so that
// FMGameRender's legitimate GPU-memory writes (render targets, ring buffers)
// inside 0x82000000-0x82800000 are not reverted every second.
void stopPeriodicImageRestore() {
  if (g_periodicRestoreEnabled.exchange(false, std::memory_order_acq_rel)) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[IMAGE_WRITE_VEH] periodic-restore stopped (post-WALK_END)");
  }
}

// Returns true if the periodic restore has NOT yet been stopped (i.e. FMGameRenderInit
// has not completed). Used by bootstrapAudioClient() to decide whether the bulk image
// restore is safe — after FMGameRenderInit runs, FMGameRender writes 10-20 words into
// the image range as render pipeline setup; restoring over them would kill cmd43.
bool isPeriodicRestoreActive() {
  return g_periodicRestoreEnabled.load(std::memory_order_acquire);
}

// iter-357l: RUNTIME arming for a DYNAMIC target. The env-target mode above
// needs the VA at startup, but the address we must watch (the XMV ASF
// parser's reader back-pointer at parser+0x260) is heap and only exists for
// a few hundred microseconds inside sub_82486820. This arms the exact same
// machinery (VEH + re-arm + TF single-step + full context logging for the
// target span) at a VA chosen at runtime, and disarms on request.
//
// Returns false if the watchdog isn't installed or VirtualProtect fails.
// Safe to call repeatedly; re-arming to a new page moves the protection.
bool armRuntimeWriteWatch(uint32_t guestVA, uint32_t len) {
#if defined(_WIN32)
  if (!g_vehHandle || !guestVA) return false;
  uint8_t* base = nullptr;
  size_t size = 0;
  getGlobalGuestMemory(&base, &size);
  if (!base) return false;
#if !SR_ORACLE
  if (nativeHardwareMemoryIntersects(base + (guestVA & ~uint32_t(0xFFFu)), 0x1000)) {
    SetLastError(ERROR_ACCESS_DENIED);
    return false;  // refuse before replacing the current watch's state
  }
#endif
  g_bootPageHost = 0;  // a runtime arm is a diagnostic: every later protection excludes all (wdProtect)

  const uint32_t pageVA = guestVA & ~uint32_t(0xFFFu);
  const uintptr_t pageHost = (uintptr_t)base + pageVA;

  // Drop protection on the previous page (if any) before moving.
  if (g_protectedStart && g_protectedStart != pageHost) {
    DWORD op = 0;
    wdProtect((LPVOID)g_protectedStart, g_protectedEnd - g_protectedStart,
                   PAGE_READWRITE, &op);
  }

  g_envWatchVA = guestVA;          // enables the re-arming path in the VEH
  g_envWatchLen = len ? len : 4;
  g_protectedStart = pageHost;
  g_protectedEnd = pageHost + 0x1000;
  g_protectedGuestVA = pageVA;
  g_snapshotValid = false;         // heap page: no boot snapshot to restore

  DWORD oldProtect = 0;
  if (!wdProtect((LPVOID)pageHost, 0x1000, PAGE_READONLY, &oldProtect)) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[IMAGE_WRITE_VEH] runtime arm FAILED at guest 0x%08X (host 0x%llX gle=%lu)",
      guestVA, (unsigned long long)pageHost, GetLastError());
    g_envWatchVA = 0;
    return false;
  }
  SR_LOG_BOOT(sr::LogSeverity::Warn,
    "[IMAGE_WRITE_VEH] RUNTIME ARM: watching guest 0x%08X..0x%08X on page "
    "0x%08X (host 0x%llX, was prot 0x%lX)",
    guestVA, guestVA + g_envWatchLen, pageVA, (unsigned long long)pageHost,
    oldProtect);
  return true;
#else
  (void)guestVA; (void)len;
  return false;
#endif
}

void disarmRuntimeWriteWatch() {
#if defined(_WIN32)
  if (!g_protectedStart) return;
  DWORD op = 0;
  wdProtect((LPVOID)g_protectedStart, g_protectedEnd - g_protectedStart,
                 PAGE_READWRITE, &op);
  g_envWatchVA = 0;
  g_protectedStart = 0;
  g_protectedEnd = 0;
#endif
}

// Install the watchdog: VirtualProtect a 4 KB page around the audio class
// vtable VA 0x8209A458 and register a VEH that catches writes there.
// Call after loadXexImage and after registerExCreateThreadRunner.
void installImageWriteWatchdog() {
#if defined(_WIN32)
  static std::atomic<bool> s_installed{false};
  if (s_installed.exchange(true)) return;

  uint8_t* base = nullptr;
  size_t size = 0;
  getGlobalGuestMemory(&base, &size);
  if (!base) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[IMAGE_WRITE_VEH] cannot install: guest memory not registered");
    return;
  }

  // Protect just the 4 KB page containing 0x8209A458 (the audio class vtable).
  // A wider range catches more, but also produces noise from legitimate .data
  // writes. The audio vtable's page is in .rdata where writes are real bugs.
  // iter-323: SR_WRITE_WATCH_VA=<hex> retargets at any guest cell (re-arming
  // mode, see imageWriteVeh); SR_WRITE_WATCH_LEN sets the target span.
  constexpr uint32_t kTargetVA = 0x8209A458u;
  constexpr uint32_t kPageMask = ~uint32_t(0xFFFu);
  uint32_t targetVA = kTargetVA;
  if (const char* e = std::getenv("SR_WRITE_WATCH_VA")) {
    g_envWatchVA = (uint32_t)strtoul(e, nullptr, 16);
    if (const char* l = std::getenv("SR_WRITE_WATCH_LEN"))
      g_envWatchLen = (uint32_t)strtoul(l, nullptr, 0);
    if (g_envWatchVA) {
      targetVA = g_envWatchVA;
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[IMAGE_WRITE_VEH] env-target mode: VA=0x%08X len=%u (re-arming)",
        g_envWatchVA, g_envWatchLen);
    }
  }
  uint32_t protLen = 0x1000;
  if (imageRoDiag()) {
    targetVA = kRodataVA;
    g_envWatchVA = kRodataVA;
    g_envWatchLen = kRodataLen;
    g_rearmTargetBudget = 64;
    protLen = kRodataLen;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[IMAGE_RO] diagnostic: .rdata 0x%08X..0x%08X read-only; every write is logged "
      "(first 64 with full context) and the image restores are off",
      kRodataVA, kRodataVA + kRodataLen);
  }
  uint32_t pageVA = targetVA & kPageMask;
  uintptr_t pageHost = (uintptr_t)base + pageVA;
#if !SR_ORACLE
  if (nativeHardwareMemoryIntersects(reinterpret_cast<void*>(pageHost), protLen)) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[NATIVE_MMIO_BAD] write-watch targets hardware reservation VA=%08X len=%u", pageVA, protLen);
    std::_Exit(3);  // refuse before the snapshot reads no-access memory
  }
#endif
  // The default mode's one page is a declared range (guest_protect.hpp); the opt-in modes exclude all.
  if (!g_envWatchVA && !imageRoDiag() && protLen == 0x1000) g_bootPageHost = pageHost;

  g_protectedStart = pageHost;
  g_protectedEnd   = pageHost + protLen;
  g_protectedGuestVA = pageVA;

  // Snapshot the page BEFORE protecting, while it's still PAGE_READWRITE.
  // This gives us a clean copy to restore from later — see
  // restoreImageWatchdogPage() called from bootstrapAudioClient().
  std::memcpy(g_pageSnapshot, (void*)pageHost, 0x1000);
  g_snapshotValid = true;

  // Also snapshot the broader .text + .rdata range so we can restore vtables
  // / strings / function pointers across the audio bootstrap's dispatch
  // chain. The full image is 10 MB but the first 2 MB is typically all
  // we need for .rdata (audio uses pool vtables at ~0x82006000, audio
  // vtable at 0x8209A458, etc.).
  uint8_t* imgStart = base + kImageSnapshotBaseVA;
  g_imageSnapshot = std::make_unique<uint8_t[]>(kImageSnapshotSize);
  std::memcpy(g_imageSnapshot.get(), imgStart, kImageSnapshotSize);
  g_imageSnapshotValid = true;

  // Install the VEH BEFORE protecting, so the first fault gets handled.
  g_vehHandle = AddVectoredExceptionHandler(sr::kVehFirst, imageWriteVeh);   // 1 = first (0 in ASan builds)
  if (!g_vehHandle) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[IMAGE_WRITE_VEH] AddVectoredExceptionHandler failed (gle=%lu)", GetLastError());
    return;
  }

  DWORD oldProtect = 0;
  if (!wdProtect((LPVOID)pageHost, protLen, PAGE_READONLY, &oldProtect)) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[IMAGE_WRITE_VEH] VirtualProtect(PAGE_READONLY) failed at host 0x%llX (gle=%lu)",
      (unsigned long long)pageHost, GetLastError());
    RemoveVectoredExceptionHandler(g_vehHandle);
    g_vehHandle = nullptr;
    return;
  }

  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[IMAGE_WRITE_VEH] watchdog installed: guest VA 0x%08X..0x%08X (host 0x%llX..0x%llX) PAGE_READONLY (was 0x%lX)",
    pageVA, pageVA + protLen,
    (unsigned long long)pageHost, (unsigned long long)(pageHost + protLen),
    oldProtect);

  // 2026-07-13 (charter DIAG, read-only): 1 Hz poller on the env-target window.
  // The zeroer we're hunting is roll-dependent; the VEH only catches it if it
  // fires while armed, and the natural READER (splash sequencer) only runs some
  // rolls. This poller makes EVERY roll report if/when the watched cells change:
  // first-change timestamp + before/after bytes. Reads only — never writes.
  if (g_envWatchVA) {
    const uint32_t wVA = g_envWatchVA;
    const uint32_t wLen = g_envWatchLen > 64 ? 64 : (g_envWatchLen ? g_envWatchLen : 4);
    uint8_t* hostPtr = base + wVA;
    std::thread([wVA, wLen, hostPtr]() {
      uint8_t snap[64];
      std::memcpy(snap, hostPtr, wLen);
      char hex[3 * 64 + 1];
      auto dump = [&](const uint8_t* p) {
        int o = 0;
        for (uint32_t i = 0; i < wLen && o < (int)sizeof(hex) - 4; ++i)
          o += snprintf(hex + o, sizeof(hex) - o, "%02X ", p[i]);
        hex[o] = '\0';
        return hex;
      };
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[WATCH_POLL] armed on 0x%08X len=%u initial: %s", wVA, wLen, dump(snap));
      // iter-345: also watch the PSM dispatcher delegate slot *(*PSM)+4
      // (should hold 0x822AB748 once FMGamePlayStateMgr's FULL ctor runs;
      // empty slot on non-firing rolls = the determinism gate). Read-only.
      uint32_t lastSlot = 0xDEADBEEFu, lastPsm = 0;
      auto rdg32 = [&](uint32_t va) -> uint32_t {
        uint8_t* b = nullptr; size_t sz = 0; getGlobalGuestMemory(&b, &sz);
        if (!b || va < 0x10000u || (size_t)va + 4 > sz) return 0;
        const uint8_t* p = b + va;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
               (uint32_t(p[2]) << 8) | uint32_t(p[3]);
      };
      int changes = 0;
      for (int tick = 0; tick < 600 && changes < 8; ++tick) {  // 10 min
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        if ((tick % 10) == 0) {
          uint32_t psm = sr::kernel::getFmGamePlayStateMgrInstance();
          uint32_t sub = psm ? rdg32(psm) : 0;
          uint32_t slot = sub ? rdg32(sub + 4) : 0;
          if (slot != lastSlot || psm != lastPsm) {
            lastSlot = slot; lastPsm = psm;
            SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PSM_DELEGATE] t=%ds psm=%08X sub=%08X slot[+4]=%08X%s",
              tick, psm, sub, slot,
              slot == 0x822AB748u ? "  (dispatcher REGISTERED)" : "");
          }
          // iter-345g: VD pump gates — real device (linked at 0x8299ADF4):
          // flag byte dev+0x2A3A (pump poll waits for bit1), pending-op rec
          // dev+0x2A14, swap cb dev+0x3B10. Read-only.
          uint32_t dev = rdg32(0x8299ADF4u);
          if (dev >= 0x10000u) {
            uint8_t* b = nullptr; size_t sz = 0; getGlobalGuestMemory(&b, &sz);
            uint8_t flag = (b && (size_t)dev + 0x2A3B <= sz) ? b[dev + 0x2A3Au] : 0xEE;
            static uint8_t s_lastFlag = 0xEE; static uint32_t s_lastRec = 0xEE;
            uint32_t rec = rdg32(dev + 0x2A14u);
            static uint32_t s_last3B1C = 0xEE;
            uint32_t thr = rdg32(dev + 0x3B1Cu);
            if (flag != s_lastFlag || rec != s_lastRec || thr != s_last3B1C) {
              s_lastFlag = flag; s_lastRec = rec; s_last3B1C = thr;
              SR_LOG_BOOT(sr::LogSeverity::Info,
                "[VD_GATES] t=%ds dev=%08X flag2A3A=%02X rec2A14=%08X swapCb3B10=%08X "
                "frameCtr3B14=%u done3B18=%u throttle3B1C=%u",
                tick, dev, flag, rec, rdg32(dev + 0x3B10u),
                rdg32(dev + 0x3B14u), rdg32(dev + 0x3B18u), thr);
            }
          }
        }
        if (std::memcmp(snap, hostPtr, wLen) != 0) {
          ++changes;
          SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[WATCH_POLL] CHANGE #%d at t=%ds on 0x%08X: was %s", changes, tick, wVA, dump(snap));
          std::memcpy(snap, hostPtr, wLen);
          SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[WATCH_POLL] CHANGE #%d now: %s", changes, dump(snap));
        }
      }
      SR_LOG_BOOT(sr::LogSeverity::Info, "[WATCH_POLL] done (changes=%d)", changes);
    }).detach();
  }
#else
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[IMAGE_WRITE_VEH] non-Windows host; watchdog disabled");
#endif
}

}  // namespace kernel
}  // namespace sr
