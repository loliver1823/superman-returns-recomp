#include "guest_harness.hpp"
#include "kernel/sr_oracle.hpp"
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "ppc/ppc_recomp_shared.h"
#include "kernel/export_registry.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/guest_protect.hpp"  // 2026-10-14: releaseGuestArena (freeGuestMemory)
#include "kernel/kpcr.hpp"
#include "kernel/file_io.hpp"
#include "kernel/stubs.hpp"
#include "kernel/xex_loader.hpp"
#include "app/crash_handler.hpp"
#include "app/log.hpp"
#include "kernel/perf_stats.hpp"  // iter-464: mirror demand-commit counters

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <float.h>
#include <xmmintrin.h>  // For _mm_getcsr/_mm_setcsr (SSE control)
#else
#include <sys/mman.h>
#endif

// Defined in ppc_import_glue.cpp — dumps main-thread kernel call ring for crash diagnosis.
extern void dumpMainThreadCallRing();

// iter-188: dispatch-table debug exports (defined in kernel/guest_context.cpp)
// so Frida can inverse-map host instruction pointers to guest VAs.
extern "C" {
  extern void** g_debugDispatchTable;
  extern uint64_t g_debugDispatchTableEntries;
  extern uint32_t g_debugDispatchCodeBase;
}

// iter-214 (PPC-aware Frida): expose the MAIN guest thread's live PPCContext and
// its register byte-offsets so an external tool reads TRUE guest registers and can
// walk the guest PPC back-chain (sub_XXXX frames) instead of host x86 frames.
// Populated in RunGuest once ctx is live (next to setMainGuestContext); g_debugMainCtx
// points at the &ctx on the harness thread stack — valid for the whole guest session,
// nulled on guest exit. PPCContext registers are stored host-native (NOT byte-swapped;
// only guest *memory* is big-endian), so a reader takes the U32 at ctx+offset directly.
extern "C" {
  __declspec(dllexport) void*    g_debugMainCtx       = nullptr;  // &PPCContext (main thread)
  __declspec(dllexport) uint32_t g_debugCtxGprOff[32] = {0};      // byte offset of r0..r31
  __declspec(dllexport) uint32_t g_debugCtxLrOff      = 0;        // offsetof(PPCContext, lr)
  __declspec(dllexport) uint32_t g_debugCtxCtrOff     = 0;        // offsetof(PPCContext, ctr)
  __declspec(dllexport) uint32_t g_debugCtxSize       = 0;        // sizeof(PPCContext)
  __declspec(dllexport) uint32_t g_debugCtxF1Off      = 0;        // offsetof(PPCContext, f1), host-native Float64
}

// iter-464 codegen (tools/regen/flag_transform.py): a generated function that reads a
// localized register at entry but has no direct caller (XenonAnalyse chunk duplicate,
// SEH filter, vtable-only target) loads the register from ctx and reports here once per
// site if it ever runs. ppc_config.h: SR_ABI_UNBRIDGED_ENTRY.
extern "C" void sr_abi_unbridged_entry(uint32_t va, const char* regs) {
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 64)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[ABI_UNBRIDGED] #%u sub_%08X entered: reads %s from ctx (no direct caller bridges it)",
                n, va, regs);
}

// Defined in kernel/image_write_watchdog.cpp — see iter-23 image-corruption hunt.
namespace sr { namespace kernel {
  void installImageWriteWatchdog();
  void startPeriodicImageRestore();
  // iter-24w: 20 kHz poller watching modA-D (0x8296D8D8/DC/E0/E4) — captures
  // main-thread guest LR/CTR/r1/r3 when they transition (zeroed after init).
  void startModuleGlobalsPoller();
  // iter-26: 1 Hz main-thread idle sampler — finds the post-tick=1000 wait
  // site by sampling main's lr/ctr/r1/r3.
  void startMainThreadIdleSampler();
  // iter-36: 50 kHz poller one indirection deeper — watches *(modA+0) and
  // *(modB+0), the vtable fields inside the module structs themselves.
  // iter-35 found these get NULL'd while the pointer slots stay restored.
  void startModuleStructWatch();
  // iter-43: background thread that patches module+4 (worldPtr) with
  // the correct FM worker context VA so NtQueueApcThread can wake workers.
  void startFMWorldPtrPatcher();
  // iter-379b: Dr0 write-watch (kernel/hw_watchpoint.cpp).
  bool armHwWriteWatchHost(void* hostAddr);
  // iter-432: owned-CS census for the guest-thread death handler (stubs.cpp).
  void dumpCsOwnedByCurrentThread(const char* tag);
} }

namespace sr { namespace kernel {
// Host-side dispatch table backing store (see ppc_context.h for extern decl).
// Allocated once at boot, populated by the harness, read-only thereafter.
PPCFunc** g_dispatchTable = nullptr;
static std::unique_ptr<PPCFunc*[]> g_dispatchTableStorage;
}}  // namespace sr::kernel

namespace sr {
namespace guest {

namespace {

// Set by runGuestHarness before __try so sehFilter can log guest state.
static PPCContext* g_sehGuestCtx = nullptr;

// ─── Shared x64 instruction decode helpers ───────────────────────────────────
// Used by both main-thread sehFilter and render-thread renderFilter for
// divide-by-zero skip and null-read AV recovery.

// Write a value to an x64 GP register in CONTEXT by register index (0=RAX..15=R15).
static void setContextReg(PCONTEXT cx, int reg, DWORD64 val) {
  switch (reg) {
    case 0: cx->Rax = val; break; case 1: cx->Rcx = val; break;
    case 2: cx->Rdx = val; break; case 3: cx->Rbx = val; break;
    case 4: cx->Rsp = val; break; case 5: cx->Rbp = val; break;
    case 6: cx->Rsi = val; break; case 7: cx->Rdi = val; break;
    case 8: cx->R8  = val; break; case 9: cx->R9  = val; break;
    case 10: cx->R10 = val; break; case 11: cx->R11 = val; break;
    case 12: cx->R12 = val; break; case 13: cx->R13 = val; break;
    case 14: cx->R14 = val; break; case 15: cx->R15 = val; break;
  }
}

// Compute total instruction length from a position just past the opcode byte,
// where ip[pos] is the ModRM byte. Returns bytes consumed from ip[0] (the base
// position passed in, which includes prefix + opcode bytes already counted).
// rexB must be applied: when REX.B=1 and mod=0, rm=5 means [R13] (no disp)
// instead of [RIP+disp32].
static int computeModRMInstrLen(const uint8_t* ip, int basePos, int rexB) {
  if (basePos >= 15) return 0;
  uint8_t modrm = ip[basePos];
  uint8_t mod = (modrm >> 6) & 3;
  uint8_t rm  = (modrm & 7) | (rexB << 3); // apply REX.B to rm
  int len = basePos + 1; // +1 for ModRM byte
  if (mod != 3) {
    if ((rm & 7) == 4) len++; // SIB byte (rm=4 or rm=12 with REX.B)
    if (mod == 0 && (rm & 7) == 5) len += 4; // [RIP+disp32] or [R13] check uses low 3 bits
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
  }
  return len;
}

// Decode a div/idiv instruction at ip, return its length. Sets Rax=Rdx=0.
// Returns 0 if ip doesn't look like div/idiv.
static int skipDivInstruction(const uint8_t* ip, PCONTEXT ctx) {
  int pos = 0;
  int rexB = 0;
  if (ip[pos] >= 0x40 && ip[pos] <= 0x4F) {
    rexB = ip[pos] & 1;
    pos++;
  }
  if (ip[pos] != 0xF7) return 0;
  pos++; // past opcode
  int len = computeModRMInstrLen(ip, pos, rexB);
  if (len <= 0 || len > 15) return 0;
  ctx->Rip += len;
  ctx->Rax = 0;
  ctx->Rdx = 0;
  return len;
}

// Result of decoding a null-read faulting instruction.
struct NullReadDecodeResult {
  int instrLen;     // total instruction length (0 = decode failed)
  int destRegIdx;   // destination GP register index, or -1 if none (e.g. cmp/test)
};

// Decode the instruction at ip that faulted on a null-read AV.
// Handles common mov/movzx/movsx/cmp/test patterns.
static NullReadDecodeResult decodeNullReadInstruction(const uint8_t* ip) {
  NullReadDecodeResult r = {0, -1};
  int pos = 0;
  int rexR = 0, rexB = 0;
  // Skip legacy prefixes
  while (pos < 4 && (ip[pos] == 0x66 || ip[pos] == 0xF2 || ip[pos] == 0xF3 ||
                     ip[pos] == 0x26 || ip[pos] == 0x2E || ip[pos] == 0x36 ||
                     ip[pos] == 0x3E || ip[pos] == 0x64 || ip[pos] == 0x65)) pos++;
  // REX prefix
  if (ip[pos] >= 0x40 && ip[pos] <= 0x4F) {
    uint8_t rex = ip[pos++];
    rexR = (rex >> 2) & 1;
    rexB = rex & 1;
  }
  uint8_t opcode = ip[pos++];

  // mov r, [mem] (8B) or mov r8, [mem] (8A)
  if (opcode == 0x8B || opcode == 0x8A) {
    r.destRegIdx = ((ip[pos] >> 3) & 7) | (rexR << 3);
    r.instrLen = computeModRMInstrLen(ip, pos, rexB);
  }
  // movzx r, byte [mem] (0F B6), movzx r, word [mem] (0F B7)
  // movsx r, byte [mem] (0F BE), movsx r, word [mem] (0F BF)
  else if (opcode == 0x0F && (ip[pos] == 0xB6 || ip[pos] == 0xB7 ||
                               ip[pos] == 0xBE || ip[pos] == 0xBF)) {
    pos++;
    r.destRegIdx = ((ip[pos] >> 3) & 7) | (rexR << 3);
    r.instrLen = computeModRMInstrLen(ip, pos, rexB);
  }
  // movsxd (63)
  else if (opcode == 0x63) {
    r.destRegIdx = ((ip[pos] >> 3) & 7) | (rexR << 3);
    r.instrLen = computeModRMInstrLen(ip, pos, rexB);
  }
  // cmp/test — no destination register, just skip
  else if (opcode == 0x38 || opcode == 0x39 || opcode == 0x3A || opcode == 0x3B ||
           opcode == 0x84 || opcode == 0x85) {
    r.instrLen = computeModRMInstrLen(ip, pos, rexB);
  }
  // cmp [mem], imm8 (80 /7) or cmp [mem], imm8 sign-ext (83 /7)
  else if (opcode == 0x80 || opcode == 0x83) {
    r.instrLen = computeModRMInstrLen(ip, pos, rexB);
    if (r.instrLen > 0) r.instrLen += 1; // +imm8
  }
  return r;
}

// ──────────────────────────────────────────────────────────────────────────────

// SEH filter: capture exception info and return handler type (no lambda for clang).
static DWORD s_sehExceptionCode = 0;
static int sehFilter(DWORD code, EXCEPTION_POINTERS* info, void*& outFaultAddr,
                     DWORD& outAccessType, ULONG_PTR& outDataAddr) {
  s_sehExceptionCode = code;

  // Handle integer divide-by-zero: fix context (skip div, set quotient=0) and continue.
  if (code == 0xC0000094 && info && info->ContextRecord) {
    PCONTEXT ctx = info->ContextRecord;
    const uint8_t* ip = reinterpret_cast<const uint8_t*>(ctx->Rip);
    int skip = skipDivInstruction(ip, ctx);
    if (skip > 0) {
      static int divZeroCount = 0;
      divZeroCount++;
      if (divZeroCount <= 5 || (divZeroCount % 100 == 0)) {
        uint32_t guestLr = g_sehGuestCtx ? g_sehGuestCtx->lr : 0u;
        SR_LOG_BOOT(sr::LogSeverity::Info, "[BOOT] Divide-by-zero #%d at host 0x%p guest lr=0x%08X skip=%d -> continue (quotient=0)", divZeroCount, (void*)info->ExceptionRecord->ExceptionAddress, guestLr, skip);
      }
      return EXCEPTION_CONTINUE_EXECUTION;
    }
  }

  // Handle FPU exceptions: These shouldn't happen with proper MXCSR masking.
  // If we get here, something is unmasking exceptions. Log and crash for debugging.
  // 0xC000008D = FLOAT_DENORMAL_OPERAND, 0xC000008E = FLOAT_DIVIDE_BY_ZERO, 
  // 0xC000008F = FLOAT_INVALID_OPERATION, 0xC0000090 = FLOAT_OVERFLOW,
  // 0xC0000091 = FLOAT_STACK_CHECK, 0xC0000092 = FLOAT_UNDERFLOW, 0xC0000093 = FLOAT_INEXACT
  if (code >= 0xC000008D && code <= 0xC0000093 && info && info->ContextRecord) {
    static int fpuExCount = 0;
    fpuExCount++;
    uint32_t guestLr = g_sehGuestCtx ? g_sehGuestCtx->lr : 0u;
    unsigned int currentMxcsr = _mm_getcsr();
    SR_LOG_BOOT(sr::LogSeverity::Error, "[BOOT] FPU exception #%d code=0x%08X at host 0x%p guest lr=0x%08X MXCSR=0x%08X",
                fpuExCount, code, (void*)info->ExceptionRecord->ExceptionAddress, guestLr, currentMxcsr);
    // Don't try to continue - FPU exceptions indicate a real problem
    return EXCEPTION_EXECUTE_HANDLER;
  }

  // Handle access violation reading from null (address 0x0).
  // The game's PPC code or our stubs may pass null pointers to CRT functions
  // (e.g. _vsnprintf with null format string). Rather than terminating the main
  // thread, skip the faulting instruction and return 0 so the game can continue.
  // This extends runtime from ~1s to much longer, letting us reach menus/FMV.
  if (code == EXCEPTION_ACCESS_VIOLATION && info && info->ContextRecord &&
      info->ExceptionRecord->NumberParameters >= 2) {
    DWORD accessType = (DWORD)info->ExceptionRecord->ExceptionInformation[0];
    ULONG_PTR dataAddr = info->ExceptionRecord->ExceptionInformation[1];
    // Only handle reads from low addresses (null pointer dereferences).
    // Writes to null are more dangerous — let those crash.
    if (accessType == 0 && dataAddr < 0x10000) {
      static int s_nullReadCount = 0;
      s_nullReadCount++;
      PCONTEXT c = info->ContextRecord;
      const uint8_t* ip = reinterpret_cast<const uint8_t*>(c->Rip);
      NullReadDecodeResult dr = decodeNullReadInstruction(ip);
      if (dr.instrLen > 0 && dr.instrLen <= 15) {
        c->Rip += dr.instrLen;
        if (dr.destRegIdx >= 0) setContextReg(c, dr.destRegIdx, 0);
        if (s_nullReadCount <= 10 || (s_nullReadCount % 500 == 0)) {
          uint32_t guestLr = g_sehGuestCtx ? g_sehGuestCtx->lr : 0u;
          SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[BOOT] Null-read AV #%d at host %p (data=0x%llX) guest lr=0x%08X skip=%d reg=%d -> continue",
            s_nullReadCount, (void*)info->ExceptionRecord->ExceptionAddress,
            (unsigned long long)dataAddr, guestLr, dr.instrLen, dr.destRegIdx);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
      }
    }
  }

  if (info && info->ExceptionRecord) {
    outFaultAddr = (void*)info->ExceptionRecord->ExceptionAddress;
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
      outAccessType = (DWORD)info->ExceptionRecord->ExceptionInformation[0];
      outDataAddr = info->ExceptionRecord->ExceptionInformation[1];
    }
  }
  // Dump native return addresses from the stack for crash diagnosis
  if (info && info->ContextRecord) {
    PCONTEXT ctx = info->ContextRecord;
    uintptr_t rsp = ctx->Rsp;
    char buf[512];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "[NATIVE_STACK] RIP=0x%llX RSP=0x%llX RBP=0x%llX\n",
                    (unsigned long long)ctx->Rip, (unsigned long long)rsp, (unsigned long long)ctx->Rbp);
    // Read potential return addresses from stack and resolve modules
    HMODULE exeBase = GetModuleHandleA(nullptr);
    for (int i = 0; i < 8 && pos < (int)sizeof(buf) - 80; ++i) {
      uintptr_t val = 0;
      __try {
        val = *(uintptr_t*)(rsp + i * 8);
      } __except(EXCEPTION_EXECUTE_HANDLER) { val = 0xDEAD; }
      if (val > 0x7FF600000000ULL && val < 0x7FFFFFFFFFFF00ULL) {
        HMODULE hMod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)val, &hMod);
        uintptr_t off = hMod ? (val - (uintptr_t)hMod) : 0;
        const char* tag = (hMod == exeBase) ? "EXE" : "DLL";
        pos += snprintf(buf + pos, sizeof(buf) - pos, "[NATIVE_STACK] [%d] 0x%llX (%s+0x%llX)\n",
                        i, (unsigned long long)val, tag, (unsigned long long)off);
      }
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
    // Write directly to stderr handle
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hErr && hErr != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      WriteFile(hErr, buf, (DWORD)pos, &written, nullptr);
    }
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

// iter-463 charter (burndown item 10): ppc_stub_unmapped is DELETED. It filled
// every dispatch slot without a recompiled function and FABRICATED r3 = r4 = 0
// for any call there - so PPC_CALL_INDIRECT_FUNC's own "unmapped" branch could
// never fire ([UNMAPPED_CALL] 0/150 run logs), and the stub itself logged
// "unmapped stub call" in 1/81 run logs before 60a90bf, 0/69 after. Empty
// slots are now nullptr: an indirect call there reaches the named
// log-and-fail (kernel/indirect_call.cpp); every host lookup
// (PPC_LOOKUP_FUNC[_SAFE] in kernel/, guest_harness thread start) already
// null-checks.

// Runner for ExCreateThread: called on a new host thread with (startVA, startParam, stackVA, base, size).
// Sets up PPCContext and invokes the recompiled guest function at startVA so the render thread can run (VdSwap).
static void runGuestThreadEntry(uint32_t startVA, uint32_t startParam, uint32_t stackVA,
                                uint8_t* base, size_t size) {
  constexpr uint32_t kCodeBase = PPC_CODE_BASE;
  constexpr uint32_t kCodeEnd = 0x82A00000u;
  if (startVA < kCodeBase || startVA >= kCodeEnd) return;
  constexpr size_t kThreadStackSize = 256 * 1024;
  if (stackVA == 0) return;

  // Set thread-local guest memory so PPC_GUEST_BASE() / getGuestBase() work on this thread.
  // Without this, all PPC_LOAD_*/PPC_STORE_* dereference nullptr → immediate AV.
  sr::kernel::setGuestMemory(base, size);
  uint8_t* verifiedBase = sr::kernel::getGuestBase();
  if (!verifiedBase || verifiedBase != base) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[RenderThread] setGuestMemory failed: getGuestBase()=%p expected=%p (TLS broken?)",
      (void*)verifiedBase, (void*)base);
    return;
  }

  PPCContext ctx{};
  ctx.r1.u32 = stackVA + static_cast<uint32_t>(kThreadStackSize) - 0x100;
  ctx.r3.u32 = startParam;
  // step4/audit SR_XAPI_THREAD_STARTUP: XapiThreadStartup(start r3, context r4) - 0 unless ExCreateThread set it.
  ctx.r4.u32 = sr::kernel::takeThreadStartR4();
  ctx.r14.u32 = 0x829C0000u;
  // iter-463 PLATFORM FIX (SR_KPCR): r13 = this thread's KPCR, built by
  // ExCreateThread next to its KTHREAD (guestThreadFunc bound the KTHREAD).
  // Gate off = 0, as before.
  ctx.r13.u32 = sr::kernel::kpcrBindCurrent(stackVA + static_cast<uint32_t>(kThreadStackSize),
                                            stackVA, -1);
  ctx.lr = 0;
  ctx.ctr.u32 = 0;

#if defined(_WIN32)
  _fpreset();
  _controlfp(_MCW_EM, _MCW_EM);
  unsigned int mxcsr = _mm_getcsr();
  mxcsr |= 0x1F80;
  _mm_setcsr(mxcsr);
#endif

  PPCFunc* fn = PPC_LOOKUP_FUNC_SAFE(base, startVA);
  if (!fn) return;
  // Anchor for host stack overflow check (sub_827606E0 depth / guard); render thread has default stack (~1 MB).
  char stack_anchor;
  sr::kernel::setGuestThreadStackAnchorForOverflowCheck(&stack_anchor);
  // iter-23 r1-corruption hunt: register THIS thread's PPCContext so the VEH
  // watchdog can read live r1/lr/ctr from the writer thread (not just main).
  // setCurrentGuestContext is thread_local, so this only affects this worker.
  sr::kernel::setCurrentGuestContext(&ctx,
      offsetof(PPCContext, lr),
      offsetof(PPCContext, ctr),
      offsetof(PPCContext, r3),
      offsetof(PPCContext, r1),
      offsetof(PPCContext, r31));
  sr::kernel::ExportRegistry::setGuestCallContext(true);
#if defined(_WIN32)
  static thread_local DWORD s_renderExCode = 0;
  static thread_local void* s_renderExAddr = nullptr;
  static thread_local uintptr_t s_renderFaultAddr = 0;
  auto renderFilter = [](EXCEPTION_POINTERS* ep) -> int {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    s_renderExCode = code;
    s_renderExAddr = ep->ExceptionRecord->ExceptionAddress;
    s_renderFaultAddr = 0;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
      s_renderFaultAddr = ep->ExceptionRecord->ExceptionInformation[1];

    // Handle integer divide-by-zero: skip instruction, set quotient/remainder to 0, continue.
    if (code == 0xC0000094 && ep->ContextRecord) {
      const uint8_t* ip = reinterpret_cast<const uint8_t*>(ep->ContextRecord->Rip);
      int skip = skipDivInstruction(ip, ep->ContextRecord);
      if (skip > 0) {
        static int s_renderDivZeroCount = 0;
        s_renderDivZeroCount++;
        if (s_renderDivZeroCount <= 5 || (s_renderDivZeroCount % 100 == 0))
          SR_LOG_BOOT(sr::LogSeverity::Info, "[RenderThread] Divide-by-zero #%d at host 0x%p guest_pc=0x%08X skip=%d -> continue (quotient=0)",
                      s_renderDivZeroCount, (void*)ep->ExceptionRecord->ExceptionAddress,
                      sr::kernel::getCurrentGuestPC(), skip);
        return EXCEPTION_CONTINUE_EXECUTION;
      }
    }

    // Handle AV reading from guest address 0 (null pointer in PPC code).
    // Render thread: PPC_LOAD_U32(0) → reads from getGuestBase()+0 → AV because
    // sub_8211BBE0 or similar gets r4=0 from unimplemented D3D vtable[4] output.
    // Skip instruction and set result to 0 so render loop survives.
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ContextRecord &&
        ep->ExceptionRecord->NumberParameters >= 2) {
      DWORD accessType = (DWORD)ep->ExceptionRecord->ExceptionInformation[0];
      ULONG_PTR dataAddr = ep->ExceptionRecord->ExceptionInformation[1];
      uintptr_t guestBase = reinterpret_cast<uintptr_t>(sr::kernel::getGuestBase());
      // Catch reads from guest address 0 (host addr = guestBase) or host addr 0 (CRT null)
      bool isGuestNull = (guestBase != 0 && dataAddr >= guestBase && dataAddr < guestBase + 0x1000);
      bool isHostNull = (dataAddr < 0x10000);
      if (accessType == 0 && (isGuestNull || isHostNull)) {
        static int s_renderNullCount = 0;
        s_renderNullCount++;
        PCONTEXT c = ep->ContextRecord;
        const uint8_t* ip = reinterpret_cast<const uint8_t*>(c->Rip);
        NullReadDecodeResult dr = decodeNullReadInstruction(ip);
        if (dr.instrLen > 0 && dr.instrLen <= 15) {
          c->Rip += dr.instrLen;
          if (dr.destRegIdx >= 0) setContextReg(c, dr.destRegIdx, 0);
          if (s_renderNullCount <= 10 || (s_renderNullCount % 200 == 0))
            SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[RenderThread] Null-read AV #%d at host %p data=0x%llX skip=%d reg=%d -> continue",
              s_renderNullCount, (void*)ep->ExceptionRecord->ExceptionAddress,
              (unsigned long long)dataAddr, dr.instrLen, dr.destRegIdx);
          return EXCEPTION_CONTINUE_EXECUTION;
        }
      }
    }

    // Handle FPU exceptions: mask and continue.
    if (code >= 0xC000008D && code <= 0xC0000093 && ep->ContextRecord) {
      unsigned int mxcsr = 0x1F80; // all exceptions masked
      _mm_setcsr((_mm_getcsr() & ~0x3F) | mxcsr);
      ep->ContextRecord->Rip += 3;
      static int s_renderFpuExCount = 0;
      s_renderFpuExCount++;
      if (s_renderFpuExCount <= 5 || (s_renderFpuExCount % 100 == 0))
        SR_LOG_BOOT(sr::LogSeverity::Info, "[RenderThread] FPU exception #%d code=0x%08X -> masked, continue",
                    s_renderFpuExCount, code);
      return EXCEPTION_CONTINUE_EXECUTION;
    }

    // iter-439: this exception is about to kill the thread (THREAD_FATAL in
    // the handler below). Capture a first-occurrence minidump HERE, where the
    // full exception context still exists — the __except swallows it and the
    // process-level crash handler never sees these.
    sr::writeThreadFatalMinidump(ep);
    return EXCEPTION_EXECUTE_HANDLER;
  };
  __try {
    fn(ctx, base);
  }
  __except (renderFilter(GetExceptionInformation())) {
    uint8_t* baseAtCrash = sr::kernel::getGuestBase();
    // iter-432 (roll-stability): a silently-dying guest thread orphans its
    // locks and converts one fault into a whole-boot deadlock that reads as a
    // "hang" (iter-428e/432a). Make every death LOUD and self-attributing:
    // distinctive countable tag, guest back-chain, and the owned-CS census.
    // iter-439: imageOfs makes the host RIP resolvable post-mortem (ASLR base
    // is gone with the process; the offset into our exe is stable).
    const uint8_t* exeBase = (const uint8_t*)GetModuleHandleW(nullptr);
    const long long imageOfs =
        (exeBase && (const uint8_t*)s_renderExAddr >= exeBase)
            ? (long long)((const uint8_t*)s_renderExAddr - exeBase) : -1;
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[THREAD_FATAL] guest thread startVA=0x%08X exception code=0x%08X at host=%p imageOfs=0x%llX fault=0x%llX "
      "ctx: lr=0x%08X ctr=0x%08X r3=0x%08X r1=0x%08X r31=0x%08X getGuestBase()=%p",
      startVA, s_renderExCode, s_renderExAddr, (unsigned long long)imageOfs,
      (unsigned long long)s_renderFaultAddr,
      ctx.lr, ctx.ctr.u32, ctx.r3.u32, ctx.r1.u32, ctx.r31.u32, (void*)baseAtCrash);
    // Guest back-chain: r1 sits in a stack region we mapped ourselves, so
    // bounded reads through the backchain are safe. Collect 0x82xxxxxx words.
    if (baseAtCrash) {
      char chain[360]; int cn = 0; chain[0] = 0;
      uint32_t sp = ctx.r1.u32; int words = 24;
      for (int hop = 0; hop < 8 && sp >= 0x40000000u && sp < 0xC0000000u && words > 0; ++hop) {
        auto rd = [&](uint32_t va) -> uint32_t {
          const uint8_t* p = baseAtCrash + va;
          return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                 (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        };
        const uint32_t next = rd(sp);
        for (uint32_t o = 4; o < 0x120 && words > 0; o += 4) {
          const uint32_t v = rd(sp + o);
          if (v >= 0x82000000u && v < 0x82990000u) {
            int w = snprintf(chain + cn, sizeof(chain) - cn, " %08X", v);
            if (w <= 0 || cn + w >= (int)sizeof(chain) - 1) { words = 0; break; }
            cn += w; --words;
          }
          if (next && sp + o >= next) break;
        }
        if (!next || next <= sp || next - sp > 0x10000u) break;
        sp = next;
      }
      SR_LOG_BOOT(sr::LogSeverity::Error, "[THREAD_FATAL] guest chain:%s",
                  chain[0] ? chain : " (none)");
    }
    sr::kernel::dumpCsOwnedByCurrentThread("THREAD_FATAL");
  }
#else
  fn(ctx, base);
#endif
  sr::kernel::ExportRegistry::setGuestCallContext(false);
}

#if defined(_WIN32)
// iter-427 SR_PHYS_UNIFY — console-true physical memory (MEMORY_MODEL_PLAN §3,
// amended). One 512 MB pagefile-backed section IS the physical RAM; the guest
// windows 0x8/0xA/0xC/0xE are MapViewOfFile3 views of it at identity offsets,
// so a write through ANY window is visible through every other — exactly the
// hardware behavior. The flat model's arithmetic folds missed this for guest-
// internal dataflow (the game writes the vdecl-patched movie VS via an 0xAE VA
// and memcpys it into the ring via the 0x8E VA — under the flat model that copy
// read zeros, and the ring received 27/30 dwords of 0x00: [PM4_REJECT]).
//
// Region map ('view' = the shared section, 'priv' = ordinary private pages):
//   [0x00000000,0x80000000)  priv   virtual heaps / low ranges
//   [0x80000000,0xA0000000)  view@0 cached window (XEX image, heap — identity phys)
//   [0xA0000000,0xC0000000)  view@0 64K-page physical window
//   [0xC0000000,0xD0000000)  view@0 16M-page physical window, first 256 MB
//   [0xD0000000,0xD0100000)  priv   harness fake-device carveout (guest_context.hpp)
//   [0xD0100000,0xE0000000)  view@0x10100000  rest of the 16M window
//   [0xE0000000,0x100000000) view@0 4K-page physical window
//   [0x100000000,size)       priv   guard (base+4GB must stay dereferenceable)
// The carveout means phys 0x10000000-0x100FFFFF is not reachable through the
// 0xC window (still reachable via 0x8/0xA/0xE); acceptable, documented.
static uint8_t* allocGuestMemoryUnified(uint64_t preferredBase, size_t size) {
  using PFN_VirtualAlloc2 = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG,
                                           MEM_EXTENDED_PARAMETER*, ULONG);
  using PFN_MapViewOfFile3 = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T,
                                            ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
  HMODULE kb = GetModuleHandleA("kernelbase.dll");
  auto pVirtualAlloc2 = kb ? (PFN_VirtualAlloc2)GetProcAddress(kb, "VirtualAlloc2") : nullptr;
  auto pMapViewOfFile3 = kb ? (PFN_MapViewOfFile3)GetProcAddress(kb, "MapViewOfFile3") : nullptr;
  if (!pVirtualAlloc2 || !pMapViewOfFile3) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[PHYS_UNIFY] VirtualAlloc2/MapViewOfFile3 unavailable");
    return nullptr;
  }
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
  constexpr uint64_t kPhysBytes = 0x20000000ull;  // 512 MB console RAM
  uint8_t* base = (uint8_t*)pVirtualAlloc2(GetCurrentProcess(), (PVOID)preferredBase, size,
                                           MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                           PAGE_NOACCESS, nullptr, 0);
  if (!base) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[PHYS_UNIFY] placeholder reserve failed err=%lu",
                GetLastError());
    return nullptr;
  }
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                      (DWORD)(kPhysBytes >> 32), (DWORD)(kPhysBytes & 0xFFFFFFFFu),
                                      nullptr);
  if (!section) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[PHYS_UNIFY] section create failed err=%lu", GetLastError());
    VirtualFree(base, 0, MEM_RELEASE);
    return nullptr;
  }
  struct Region { uint64_t start, sz, viewOfs; bool isView; };
  const Region regions[] = {
      {0x00000000ull, 0x80000000ull, 0, false},
      {0x80000000ull, 0x20000000ull, 0, true},
      {0xA0000000ull, 0x20000000ull, 0, true},
      {0xC0000000ull, 0x10000000ull, 0, true},
      {0xD0000000ull, 0x00100000ull, 0, false},        // fake-device carveout
      {0xD0100000ull, 0x0FF00000ull, 0x10100000ull, true},
      {0xE0000000ull, 0x20000000ull, 0, true},
      {0x100000000ull, size - 0x100000000ull, 0, false},  // guard past 4 GB
  };
  const size_t nRegions = sizeof(regions) / sizeof(regions[0]);
  bool ok = true;
  for (size_t i = 0; i < nRegions && ok; i++) {
    const Region& r = regions[i];
    // Split the placeholder (the final region is whatever placeholder remains,
    // so splitting it would fail with ERROR_INVALID_PARAMETER — skip).
    if (i + 1 < nRegions &&
        !VirtualFree(base + r.start, r.sz, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[PHYS_UNIFY] split @+0x%llX failed err=%lu",
                  (unsigned long long)r.start, GetLastError());
      ok = false;
      break;
    }
    void* got = nullptr;
    if (r.isView) {
      got = pMapViewOfFile3(section, GetCurrentProcess(), base + r.start, r.viewOfs, r.sz,
                            MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    } else {
      got = pVirtualAlloc2(GetCurrentProcess(), base + r.start, r.sz,
                           MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                           PAGE_READWRITE, nullptr, 0);
    }
    if (got != base + r.start) {
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[PHYS_UNIFY] %s @+0x%llX failed err=%lu",
                  r.isView ? "view" : "commit", (unsigned long long)r.start, GetLastError());
      ok = false;
    }
  }
  // The section handle can be closed — views keep it alive.
  CloseHandle(section);
  if (!ok) {
    // Tear down whatever landed; the caller falls back to the flat model.
    for (size_t i = 0; i < nRegions; i++) {
      const Region& r = regions[i];
      if (r.isView) UnmapViewOfFile(base + r.start);
    }
    VirtualFree(base, 0, MEM_RELEASE);
    return nullptr;
  }
  sr::kernel::g_physUnifyActive = true;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PHYS_UNIFY] ACTIVE: 512MB section, windows 0x8/0xA/0xC/0xE are true views "
              "(0xD0000000 carveout private) base=%p", (void*)base);
  return base;
}
#endif  // _WIN32

#if defined(_WIN32) && SR_ORACLE
// iter-464 perf lane: the guest mirror at base-4GB (see the SR_GUEST_MIRROR note in
// initGuestHarness) and its demand-commit handler for SR_GUEST_MIRROR=lazy.
constexpr uint64_t kMirrorBase = 0x2900000000ULL;
constexpr size_t kMirrorSize = 0x87000000ULL;  // covers 0x2986800002 (offset 0x86800002)
static uint8_t* g_mirrorSrcBase = nullptr;
static std::atomic_flag g_mirrorLock = ATOMIC_FLAG_INIT;

// Fault-free by construction: it only acts on an access violation INSIDE the reserved mirror
// range, commits that 64 KB chunk (VirtualQuery first, under a spin lock, so two threads
// faulting on one chunk commit and fill it once), copies the same chunk from the guest flat
// region (always committed) and resumes. Anything else continues the search. No logging
// here (the faulting thread may hold the log lock): counters only.
static LONG WINAPI mirrorDemandVeh(EXCEPTION_POINTERS* info) {
  if (!info || !info->ExceptionRecord ||
      info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
      info->ExceptionRecord->NumberParameters < 2 || !g_mirrorSrcBase)
    return EXCEPTION_CONTINUE_SEARCH;
  const uintptr_t addr = info->ExceptionRecord->ExceptionInformation[1];
  if (addr < kMirrorBase || addr >= kMirrorBase + kMirrorSize) return EXCEPTION_CONTINUE_SEARCH;
  const uintptr_t chunk = addr & ~uintptr_t(0xFFFF);
  while (g_mirrorLock.test_and_set(std::memory_order_acquire)) YieldProcessor();
  MEMORY_BASIC_INFORMATION mbi{};
  bool ok = VirtualQuery(reinterpret_cast<LPCVOID>(chunk), &mbi, sizeof(mbi)) != 0;
  if (ok && mbi.State != MEM_COMMIT) {
    void* p = VirtualAlloc(reinterpret_cast<LPVOID>(chunk), 0x10000, MEM_COMMIT, PAGE_READWRITE);
    ok = p != nullptr;
    if (ok) std::memcpy(p, g_mirrorSrcBase + (chunk - kMirrorBase), 0x10000);
  }
  g_mirrorLock.clear(std::memory_order_release);
  if (!ok) return EXCEPTION_CONTINUE_SEARCH;
  const uint32_t n = sr::perf::g_mirrorFaults.fetch_add(1, std::memory_order_relaxed);
  if (n < sr::perf::kMirrorFaultRips)
    sr::perf::g_mirrorFaultRip[n] = uint64_t(info->ContextRecord ? info->ContextRecord->Rip : 0);
  return EXCEPTION_CONTINUE_EXECUTION;
}
#endif  // _WIN32

uint8_t* allocGuestMemory(size_t size) {
#if defined(_WIN32)
  // Recompiled code does (low32(base) + offset); use 4GB-aligned base so no 32-bit overflow.
  constexpr uint64_t kPreferredBase = 0x2A00000000ULL;  // 4GB-aligned so (uint32_t)base == 0
  // iter-427: opt-in unified physical memory (see allocGuestMemoryUnified).
  {
    const char* e = std::getenv("SR_PHYS_UNIFY");
    if (e && e[0] == '1' && size > 0x100000000ull) {
      uint8_t* u = allocGuestMemoryUnified(kPreferredBase, size);
      if (u) return u;
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[PHYS_UNIFY] falling back to flat model");
    }
  }
#if defined(SR_ALIAS_IDENTITY_BUILD)
  // iter-467 guestcode EXPERIMENT (CMake SR_GUEST_ALIAS_IDENTITY): the guest lib addresses guest
  // memory as base + VA, which is only the console's memory when the windows are real views.
  SR_LOG_BOOT(sr::LogSeverity::Error,
              "[ALIAS_IDENTITY] this exe needs SR_PHYS_UNIFY=1 (alias-free guest addressing) - exiting");
  std::fflush(nullptr);
  ExitProcess(3);
#endif
  // iter-460 S1: write watch = the CPU-write detector for GPU-visible memory
  // (texture / shared-memory invalidation). Only when the PM4 path wants it.
  const DWORD ww = sr::kernel::guestWriteWatchWanted() ? DWORD(MEM_WRITE_WATCH) : 0u;
  void* p = VirtualAlloc((LPVOID)kPreferredBase, size, MEM_RESERVE | MEM_COMMIT | ww, PAGE_READWRITE);
  if (!p)
    p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT | ww, PAGE_READWRITE);
  sr::kernel::setGuestWriteWatchActive(p != nullptr && ww != 0);
  return static_cast<uint8_t*>(p);
#else
  void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return (p != MAP_FAILED) ? static_cast<uint8_t*>(p) : nullptr;
#endif
}

void freeGuestMemory(uint8_t* p, size_t size) {
  (void)size;
#if defined(_WIN32)
  // 2026-10-14 (Astra round A, P1): the release retires arithmetic store admission first and keeps the arena mapped
  // while a native call still holds it (kernel/guest_context.hpp releaseGuestArena), inside the H-VQ removal bracket.
  sr::kernel::releaseGuestArena(p, size);
#else
  munmap(p, size);
#endif
}

// Resolve path to game data so it works when run from build/ or project root.
static const char* resolveXexPath() {
  static std::string s_path;
  const char* rel = "Superman Returns (USA)/default.xex";
#if defined(_WIN32)
  char exeBuf[1024];
  exeBuf[0] = '\0';
  if (GetModuleFileNameA(nullptr, exeBuf, sizeof(exeBuf)) > 0) {
    std::string exe(exeBuf);
    size_t last = exe.find_last_of("\\/");
    if (last != std::string::npos) {
      std::string exeDir = exe.substr(0, last);
      // Try exe dir + ../Superman Returns (USA)/default.xex (project root when exe in build/)
      s_path = exeDir + "/../Superman Returns (USA)/default.xex";
      if (GetFileAttributesA(s_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        char normBuf[MAX_PATH];
        if (GetFullPathNameA(s_path.c_str(), sizeof(normBuf), normBuf, nullptr) > 0)
          s_path = normBuf;
        return s_path.c_str();
      }
      // Try exe dir + Superman Returns (USA)/default.xex (data next to exe)
      s_path = exeDir + "/Superman Returns (USA)/default.xex";
      if (GetFileAttributesA(s_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        char normBuf[MAX_PATH];
        if (GetFullPathNameA(s_path.c_str(), sizeof(normBuf), normBuf, nullptr) > 0)
          s_path = normBuf;
        return s_path.c_str();
      }
    }
  }
#endif
  return rel;  // cwd-relative (project root)
}

}  // namespace

void initializeDispatchTable() {
  constexpr size_t count = sr::kernel::kDispatchTableEntryCount;
  SR_LOG_BOOT(sr::LogSeverity::Info, "Guest harness: populating host-side dispatch table (%zu entries, %zu bytes)...",
              count, count * sizeof(PPCFunc*));
  auto storage = std::make_unique<PPCFunc*[]>(count);
  PPCFunc** table = storage.get();
  for (size_t i = 0; PPCFuncMappings[i].host != nullptr; ++i) {
    const size_t slot = (PPCFuncMappings[i].guest - size_t(PPC_CODE_BASE)) / 4;
    if (slot < count) table[slot] = PPCFuncMappings[i].host;
  }
  // Publish only the completed table, before any guest/PPC function can run.
  sr::kernel::g_dispatchTableStorage = std::move(storage);
  sr::kernel::g_dispatchTable = table;
  g_debugDispatchTable = reinterpret_cast<void**>(table);
  g_debugDispatchTableEntries = count;
  g_debugDispatchCodeBase = PPC_CODE_BASE;
}

bool runGuestHarness() {
  SR_LOG_BOOT(sr::LogSeverity::Info, "Guest harness: registering kernel stubs");
  sr::kernel::registerStubs();

  // Guest memory: base + 0x82000000 + image. Dispatch table lives in a host-only
  // allocation (see sr::kernel::g_dispatchTable below), so guest memory no longer
  // carves out space for it. The VA range [0x829E0000..0x83C00000) is now plain
  // zeroed guest RAM — harmless if some allocator lands there.
  constexpr size_t kImageBase = PPC_IMAGE_BASE;
  constexpr size_t kImageSize = PPC_IMAGE_SIZE;
  // iter-173: raised to cover phys window [0x82AE0000, 0xBF000000).
  // kPhysTop raised from 0xB4000000 to 0xBF000000 (944MB pool) to fit 10 × 128MB game allocs.
  constexpr size_t kHeapSize = 0x3C620000ull;  // ~966 MiB; image_end 0x829E0000 + heap = 0xBF000000
  // Cover full 32-bit guest VA so accesses like (r1 - 8) -> 0xFFFFFFF8 don't fault.
  // Also include base+0x100000000 (one past 4GB): some guest code writes there and faults otherwise.
  constexpr size_t kMinSize = kImageBase + kImageSize + kHeapSize;
  static_assert(kMinSize >= sr::kernel::kMinGuestSizeToCoverAllUsedVAs,
                "kMinSize must cover all used guest VAs; see kMinGuestSizeToCoverAllUsedVAs in guest_context.hpp");
  constexpr size_t kFourGB = 0x100000000ull;
  constexpr size_t kGuardPages = 0x10000ull;  // 64 KiB past 4GB so base+4GB is valid
  constexpr size_t kTotalSize = (kMinSize < kFourGB) ? (kFourGB + kGuardPages) : (kMinSize + kGuardPages);
  // Fallback size must be >= kMinGuestSizeToCoverAllUsedVAs (single source of truth for "cover all used VAs").
  constexpr size_t kFallbackMinSize = (sr::kernel::kMinGuestSizeToCoverAllUsedVAs + 0xFFFFull) & ~0xFFFFull;

  SR_LOG_BOOT(sr::LogSeverity::Info, "Guest harness: allocating %zu MiB guest memory",
              kTotalSize / (1024 * 1024));

  size_t allocSize = kTotalSize;
  uint8_t* base = allocGuestMemory(allocSize);
  if (!base && kTotalSize != kFallbackMinSize) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "Guest harness: 4GB alloc failed, trying %zu MiB (covers all used guest VAs)",
                kFallbackMinSize / (1024 * 1024));
    allocSize = kFallbackMinSize;
    base = allocGuestMemory(allocSize);
  }
  if (!base && allocSize != kMinSize) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "Guest harness: fallback failed, trying %zu MiB", kMinSize / (1024 * 1024));
    allocSize = kMinSize;
    base = allocGuestMemory(allocSize);
  }
  if (!base) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "Guest harness: failed to allocate guest memory");
    return false;
  }

  // Log actual allocation address - critical for debugging memory access issues
  SR_LOG_BOOT(sr::LogSeverity::Info, "Guest harness: allocated at base=0x%p (low32=0x%08X)", 
              (void*)base, (uint32_t)(uintptr_t)base);
  
  // Step 4 item 7d (lane D, 2026-10-20; SR_GUEST_DEMAND_ZERO, BAKED default ON in fix10 - =0 restores the memset): the
  // guest space is fresh VirtualAlloc / section memory, which the OS hands out zero-filled on first touch, so this 4 GB
  // memset changes no byte - it only makes every page resident at boot (the release census rel_new: guest-flat 4,092 of
  // 4,096 MB resident, every 256 MB window full; on the Ally the OS then trims and pages it out) and costs its time.
  // Demand-zero: the resident guest footprint is the guest's own (474-481 MB at the new game / Metropolis, relZ). The
  // zeros come from the OS: every fresh allocation / mapping reads as zero until written (VirtualAlloc, file mappings) -
  // ungranted regions included - so this memset never changed a byte; it ran once, before anything wrote. Reuse paths
  // (reload, arena reuse) never saw it either; where the kernel contract asks, the grants zero themselves
  // (kernel/stubs.cpp NtAllocateVirtualMemory unless X_MEM_NOZERO, the NT-contract fresh-page grants).
  static const bool s_demandZero = [] {
    const char* e = std::getenv("SR_GUEST_DEMAND_ZERO");
    return !e || e[0] != '0';
  }();
  if (s_demandZero) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "Guest harness: guest memory left demand-zero (SR_GUEST_DEMAND_ZERO default ON: fresh pages read as 0; "
                "=0 zeroes it at boot as before)");
  } else {
    SR_LOG_BOOT(sr::LogSeverity::Info, "Guest harness: zeroing guest memory... (SR_GUEST_DEMAND_ZERO=0)");
    std::memset(base, 0, allocSize);
  }

  // Load xex image (or .img) into guest memory.
  uint32_t xexLoadAddr = 0, xexImageSize = 0, xexEntryPoint = 0;
  const size_t maxCopy = (allocSize > 0xFFFFFFFFu) ? 0xFFFFFFFFu : allocSize;
  const char* xexPath = resolveXexPath();
  if (sr::kernel::loadXexImage(xexPath, base,
                               static_cast<uint32_t>(maxCopy),
                               xexLoadAddr, xexImageSize, xexEntryPoint)) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "Guest harness: loaded xex image %u bytes at VA 0x%08X (xex entry 0x%08X)", xexImageSize, xexLoadAddr, xexEntryPoint);
    // iter-23 audio bootstrap diagnostic: confirm the image data is present
    // immediately after load. The audio class vtable at guest VA 0x8209A458
    // should read as 0x8278D5E0 (sub_8278D5E0).
    {
      auto rd = [&](uint32_t va) {
        uint8_t* h = base + va;
        return (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) |
               (uint32_t(h[2]) <<  8) |  uint32_t(h[3]);
      };
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[POST_LOAD_DIAG] *(0x82000000)=0x%08X *(0x8209A458)=0x%08X *(0x826D4D60)=0x%08X *(0x82900000)=0x%08X",
        rd(0x82000000u), rd(0x8209A458u), rd(0x826D4D60u), rd(0x82900000u));
    }
  } else {
    // iter-465 physmem: never run the recompiled code over a zeroed image. Without
    // default.xex(.img) the static-ctor tables are empty, _initterm returns at once and
    // the first virtual call dies on a null vtable ([ICALL_FAIL] 828D0E50 in init2, 0.9 s
    // in) - which looked like a code defect in a roll launched from a folder without the
    // game data (archive/v1_physmem/rolls/wholerun1). Say what is missing and stop.
    char cwd[1024] = {0};
#if defined(_WIN32)
    GetCurrentDirectoryA(sizeof(cwd), cwd);
#endif
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[XEX_LOAD] FATAL: the game image is missing - tried the exe folder, its parent and the "
      "working directory '%s' (last path \"%s\"). Put the \"Superman Returns (USA)\" folder "
      "next to the exe or one level up, or start the game from the folder that holds it.",
      cwd, xexPath ? xexPath : "(null)");
#if defined(_WIN32)
    if (!std::getenv("SR_INSTANCE_ID"))
      MessageBoxW(nullptr,
                  L"The game data folder \"Superman Returns (USA)\" was not found next to the "
                  L"game or one folder up.",
                  L"Superman Returns", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    ExitProcess(3);
#endif
    return false;
  }
  // Try xex entry 0x82468d10 (_xstart in ppc_func_mapping) to run game init; fallback to safe 0x828A7778.
  const uint32_t kXexEntryVA = 0x82468d10u;
  const uint32_t callbackEntry = (xexEntryPoint == kXexEntryVA) ? xexEntryPoint : kXexEntryVA;  // prefer _xstart
  SR_LOG_BOOT(sr::LogSeverity::Info, "Guest harness: callback 0x%08X (_xstart)", callbackEntry);

  sr::kernel::setGuestMemory(base, allocSize);

  // iter-379b (SR_WATCH_SLOT=<hex guest VA>, default off): Dr0 write-watch on a
  // single static guest address, armed before ANY guest code runs. Built for
  // the DOGMA per-thread pool-slot hunt (0x82966A18 holds 0x82966A10 with no
  // identifiable writer in the xref set) — but takes any VA. Read-only
  // diagnostic; kernel/hw_watchpoint.cpp reports writer RIP + guestPC.
  {
    const char* w = std::getenv("SR_WATCH_SLOT");
    if (w && w[0]) {
      const uint32_t va = (uint32_t)strtoul(w, nullptr, 16);
      if (va && sr::kernel::armHwWriteWatchHost(base + va))
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[HWWATCH] armed on guest VA 0x%08X (host %p)", va, base + va);
      else
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[HWWATCH] FAILED to arm on 0x%08X", va);
    }
  }

  SR_LOG_BOOT(sr::LogSeverity::Info, "Guest harness: initializing kernel data structures...");
  sr::kernel::initKernelDataStructures();

  // The legacy mirror is Oracle-only. Recompiled addressing zero-extends VAs;
  // release must neither copy across its hardware hole nor expose a mirror of it.
#if defined(_WIN32) && SR_ORACLE
  // Commit and populate mirror at (base - 4GB). Main() reserves; we commit and copy. Covers sign-extended reads to 0x80800000 etc.
  // iter-464 perf lane (memory): SR_GUEST_MIRROR selects how the 2.1 GB mirror is backed.
  //   copy (unset; today's behaviour): commit it AND copy guest memory into it. The copy READS
  //        2.1 GB of the flat guest region and WRITES 2.1 GB of mirror, so ~4.2 GB of
  //        demand-zero pages become resident at boot, and 2.1 GB is charged to private bytes.
  //   commit: commit it, copy nothing (no resident pages until something touches them).
  //   lazy:   reserve only (main.cpp already reserved it); the first touch of a 64 KB chunk
  //        faults into mirrorDemandVeh, which commits that chunk and fills it from the live
  //        guest memory at the same offset (what the init-time copy held for that chunk, as
  //        of the first touch). Nothing is charged until touched, and every touch is counted
  //        with its RIP (sr::perf::g_mirrorFaults; the SR_PERF_STATS memmap census and the
  //        [MIRROR_DEMAND] line report them) - the recompiler zero-extends every guest
  //        address, so nothing is expected to touch it at all.
  static const int s_mirrorMode = [] {
    const char* e = std::getenv("SR_GUEST_MIRROR");
    if (e && std::strcmp(e, "commit") == 0) return 1;
    if (e && std::strcmp(e, "lazy") == 0) return 2;
    return 0;
  }();
  void* mirror = nullptr;
  if (s_mirrorMode == 2) {
    g_mirrorSrcBase = base;
    AddVectoredExceptionHandler(1, mirrorDemandVeh);
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "Guest harness: mirror at 0x%llX reserved only, committed on demand (SR_GUEST_MIRROR=lazy)",
                (unsigned long long)kMirrorBase);
  } else {
    mirror = VirtualAlloc((LPVOID)kMirrorBase, kMirrorSize, MEM_COMMIT, PAGE_READWRITE);
    if (mirror) {
      if (s_mirrorMode == 0) std::memcpy(mirror, base, kMirrorSize);
      SR_LOG_BOOT(sr::LogSeverity::Info, "Guest harness: mirror at 0x%p (EDRAM workaround) %s", mirror,
                  s_mirrorMode == 0 ? "copied" : "committed, NOT copied (SR_GUEST_MIRROR=commit)");
    } else {
      SR_LOG_BOOT(sr::LogSeverity::Error, "Guest harness: mirror commit FAILED (reserve in main?)");
    }
  }
  (void)mirror;  // keep mirror alive for process lifetime
#endif

  initializeDispatchTable();

  sr::kernel::registerExCreateThreadRunner(runGuestThreadEntry, base, allocSize);

  // iter-23: install the image-write watchdog. Marks the page containing
  // guest VA 0x8209A458 (audio class vtable) as PAGE_READONLY and registers
  // a Vectored Exception Handler that logs host RIP + stack on the first
  // illegal write, then unprotects the page so execution continues.
  // Forward decl of the impl in kernel/image_write_watchdog.cpp.
  sr::kernel::installImageWriteWatchdog();
  // iter-23 1 Hz periodic restore of 8 MB .text/.rdata. iter-463 charter:
  // FORCING (reverts guest writes), master-gated inside - a default run
  // starts nothing (SR_ALLOW_FORCING=1 re-enables it as a diagnostic).
  sr::kernel::startPeriodicImageRestore();
  // iter-24w MODGLOB poller: restores/rejects guest writes to the modA-D
  // manager slots 0x8296D8D8..E4. iter-463 charter: FORCING, master-gated at
  // the launch; SR_NO_MODGLOB_POLLER=1 stays as the sub-gate under the master.
  if (sr::kernel::forcingAllowed()) {
    const char* noPoller = std::getenv("SR_NO_MODGLOB_POLLER");
    if (noPoller && noPoller[0] == '1') {
      SR_LOG_BOOT(sr::LogSeverity::Info, "[MODGLOB] poller DISABLED (SR_NO_MODGLOB_POLLER=1)");
    } else {
      sr::kernel::startModuleGlobalsPoller();
    }
  }
  // iter-26: 1 Hz sampler to capture main's wait-site post-tick=1000.
  sr::kernel::startMainThreadIdleSampler();
  // iter-36: 50 kHz watcher on the vtable field inside modA/modB structs.
  sr::kernel::startModuleStructWatch();
  // iter-43: patch module+4 (worldPtr) with FM worker context VAs so
  // NtQueueApcThread can wake workers and submit draw calls.
  sr::kernel::startFMWorldPtrPatcher();
  // iter-378: seedDogmaFallbackPool call removed (fabricator deleted).

  SR_LOG_BOOT(sr::LogSeverity::Info, "Guest harness: initializing PPCContext");

  PPCContext ctx{};
  // Main thread stack: placed at 0xBE000000 (1 MB), separate from the XEX image.
  // Previously at top of image (0x829DFF00), where deep stack usage overlapped
  // with .data/.bss — static destructors zeroing globals would corrupt the stack.
  // iter-458 H-STACK: one shared definition (guest_context.hpp) - the physical
  // allocator excludes exactly this range from its arena.
  using sr::kernel::kMainStackBase;
  using sr::kernel::kMainStackSize;
  std::memset(base + kMainStackBase, 0, kMainStackSize);
  ctx.r1.u32 = kMainStackBase + kMainStackSize - 0x100;
  ctx.lr = 0;
  ctx.ctr.u32 = 0;
  // r13 = the thread's KPCR: set below once the main thread has its handle (SR_KPCR).
  // r14 = SDA2 base (small data area 2) - game accesses globals via r14+offset
  // The game uses negative offsets like r14-6964 to access global data.
  // Set r14 to point into the data section so these accesses hit valid memory.
  // Data section is roughly 0x828F0000-0x829E0000 based on code/image layout.
  ctx.r13.u32 = 0;
  ctx.r14.u32 = 0x829C0000;  // SDA2 base - allows r14-0x8000 to r14+0x8000 range
  // Entry sub_820F0000: run callback r6 exactly r5 times; r3 = base, r4 = stride.
  ctx.r3.u32 = 0;
  ctx.r4.u32 = 0;
  ctx.r5.u32 = 1;   // run callback once (use 1 for _xstart; was 10 for NtAlloc-only test)
  ctx.r6.u32 = callbackEntry;

  SR_LOG_BOOT(sr::LogSeverity::Info, "Guest harness: calling entry point sub_820F0000 (r6=0x%08X)", callbackEntry);
  sr::kernel::ExportRegistry::setGuestCallContext(true);

  // Expose guest context so main loop can sample PC when guest is idle (spinning in recomp code).
  sr::kernel::setCurrentGuestContext(&ctx, offsetof(PPCContext, lr), offsetof(PPCContext, ctr), offsetof(PPCContext, r3), offsetof(PPCContext, r1), offsetof(PPCContext, r31));
  sr::kernel::setIsMainGuestThread(true);
  // iter-24w: process-global snapshot of main thread's PPCContext for cross-
  // thread pollers (e.g. moduleGlobalsPoller).
  sr::kernel::setMainGuestContext(&ctx, offsetof(PPCContext, lr), offsetof(PPCContext, ctr), offsetof(PPCContext, r3), offsetof(PPCContext, r1));

  // iter-214: publish the live context + register offsets for PPC-aware Frida.
  g_debugMainCtx   = &ctx;
  g_debugCtxLrOff  = (uint32_t)offsetof(PPCContext, lr);
  g_debugCtxCtrOff = (uint32_t)offsetof(PPCContext, ctr);
  g_debugCtxSize   = (uint32_t)sizeof(PPCContext);
  g_debugCtxF1Off  = (uint32_t)offsetof(PPCContext, f1);
#define SR_GPROFF(n) g_debugCtxGprOff[n] = (uint32_t)offsetof(PPCContext, r##n)
  SR_GPROFF(0);  SR_GPROFF(1);  SR_GPROFF(2);  SR_GPROFF(3);  SR_GPROFF(4);  SR_GPROFF(5);
  SR_GPROFF(6);  SR_GPROFF(7);  SR_GPROFF(8);  SR_GPROFF(9);  SR_GPROFF(10); SR_GPROFF(11);
  SR_GPROFF(12); SR_GPROFF(13); SR_GPROFF(14); SR_GPROFF(15); SR_GPROFF(16); SR_GPROFF(17);
  SR_GPROFF(18); SR_GPROFF(19); SR_GPROFF(20); SR_GPROFF(21); SR_GPROFF(22); SR_GPROFF(23);
  SR_GPROFF(24); SR_GPROFF(25); SR_GPROFF(26); SR_GPROFF(27); SR_GPROFF(28); SR_GPROFF(29);
  SR_GPROFF(30); SR_GPROFF(31);
#undef SR_GPROFF

  bool guestReturned = false;
#if defined(_WIN32)
  static void* s_sehFaultAddr = nullptr;
  static DWORD s_sehAccessType = 0;
  static ULONG_PTR s_sehDataAddr = 0;
  static PPCContext* s_sehGuestCtx = nullptr;
  static uint8_t* s_sehGuestBase = nullptr;
  static size_t s_sehGuestSize = 0;
  g_sehGuestCtx = &ctx;
  s_sehGuestCtx = &ctx;
  s_sehGuestBase = base;
  s_sehGuestSize = allocSize;
  
  // CRITICAL: Disable all FPU exceptions before running guest code
  // Recompiled PowerPC code performs FPU operations that may trigger
  // inexact/overflow/underflow exceptions which would crash the host
#pragma warning(push)
#pragma warning(disable: 4996)  // _controlfp deprecation
  _fpreset();  // Reset FPU to default state
  _controlfp(_MCW_EM, _MCW_EM);  // Mask all x87 FPU exceptions
  // Also set MXCSR for SSE - mask all SSE exceptions
  unsigned int mxcsr = _mm_getcsr();
  mxcsr |= 0x1F80;  // Set all exception mask bits (bits 7-12)
  _mm_setcsr(mxcsr);
#pragma warning(pop)
  
  // Main/harness thread gets a guest handle so NtDuplicateObject(-2) returns it.
  sr::kernel::setCurrentThreadHandle(sr::kernel::allocUnifiedHandle());
  // iter-463 PLATFORM FIX (SR_KPCR): the main thread's KPCR (its anonymous
  // KTHREAD, CPU 0: Xenia's five XHostThreads - audio, XMA, CP, vsync,
  // dispatch; all creation_flags 0 = round-robin - are expected to be created
  // before "Main XThread", so next_cpu wraps to 0; unverified against a live
  // Xenia KPCR). Gate off = r13 stays 0.
  ctx.r13.u32 = sr::kernel::kpcrBindCurrent(kMainStackBase + kMainStackSize, kMainStackBase, 0);
  SR_LOG_BOOT(sr::LogSeverity::Info, "Guest harness: main r13 (KPCR) = 0x%08X", ctx.r13.u32);

  // Set stack anchor with 256 MB limit so main thread cannot overflow (0xC00000FD after ~90s otherwise). ExCreateThread-created threads set anchor in runGuestThreadEntry with same limit.
  char stack_anchor;
  sr::kernel::setGuestThreadStackAnchorForOverflowCheck(&stack_anchor);
  __try {
    sub_820F0000(ctx, base);
    guestReturned = true;
  }
  __except (sehFilter(GetExceptionCode(), GetExceptionInformation(),
                     s_sehFaultAddr, s_sehAccessType, s_sehDataAddr)) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "Guest harness: exception 0x%08X at host IP 0x%p",
                s_sehExceptionCode, s_sehFaultAddr);
    // Resolve the module containing the crash IP
    {
      HMODULE hMod = nullptr;
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             (LPCSTR)s_sehFaultAddr, &hMod)) {
        char modName[MAX_PATH] = {};
        GetModuleFileNameA(hMod, modName, sizeof(modName));
        uintptr_t offset = (uintptr_t)s_sehFaultAddr - (uintptr_t)hMod;
        SR_LOG_BOOT(sr::LogSeverity::Error, "  crash module: %s + 0x%llX", modName, (unsigned long long)offset);
      }
    }
    if (s_sehExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
      SR_LOG_BOOT(sr::LogSeverity::Error, "  %s at data address 0x%llX",
                  s_sehAccessType ? "write" : "read", (unsigned long long)s_sehDataAddr);
    }
    if (s_sehGuestCtx && s_sehGuestBase) {
      uint8_t* baseLow = s_sehGuestBase;
      uint8_t* baseHigh = s_sehGuestBase + s_sehGuestSize;
      bool inGuest = (s_sehDataAddr >= (ULONG_PTR)baseLow && s_sehDataAddr < (ULONG_PTR)baseHigh);
      SR_LOG_BOOT(sr::LogSeverity::Error, "  guest base=0x%p size=0x%zX (fault %s guest memory)",
                  (void*)baseLow, s_sehGuestSize, inGuest ? "inside" : "outside");
      SR_LOG_BOOT(sr::LogSeverity::Error, "  guest ctx: lr=0x%08X r1=0x%08X r3=0x%08X",
                  s_sehGuestCtx->lr, s_sehGuestCtx->r1.u32, s_sehGuestCtx->r3.u32);
      if (!inGuest && s_sehDataAddr != 0)
        SR_LOG_BOOT(sr::LogSeverity::Error, "  fault offset from base: %+lld (0x%llX)",
                    (long long)s_sehDataAddr - (long long)(ULONG_PTR)baseLow,
                    (unsigned long long)(s_sehDataAddr - (ULONG_PTR)baseLow));
    }
    // Dump main thread kernel call ring for crash diagnosis
    ::dumpMainThreadCallRing();
    SR_LOG_BOOT(sr::LogSeverity::Info, "Guest exit: reason=exception");
    sr::kernel::ExportRegistry::setGuestCallContext(false);
    g_debugMainCtx = nullptr;  // iter-214: ctx stack frame about to be invalid
    sr::kernel::setCurrentGuestContext(nullptr, 0);
    freeGuestMemory(base, allocSize);
    return false;
  }
#else
  try {
    sub_820F0000(ctx, base);
    guestReturned = true;
  } catch (const std::exception& e) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "Guest harness: C++ exception: %s", e.what());
    sr::kernel::ExportRegistry::setGuestCallContext(false);
    g_debugMainCtx = nullptr;  // iter-214: ctx stack frame about to be invalid
    sr::kernel::setCurrentGuestContext(nullptr, 0);
    freeGuestMemory(base, allocSize);
    return false;
  } catch (...) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "Guest harness: unknown C++ exception");
    sr::kernel::ExportRegistry::setGuestCallContext(false);
    g_debugMainCtx = nullptr;  // iter-214: ctx stack frame about to be invalid
    sr::kernel::setCurrentGuestContext(nullptr, 0);
    freeGuestMemory(base, allocSize);
    return false;
  }
#endif

  SR_LOG_BOOT(sr::LogSeverity::Info, "Guest exit: reason=return lr=0x%08X", ctx.lr);
  sr::kernel::ExportRegistry::setGuestCallContext(false);
  sr::kernel::setCurrentGuestContext(nullptr, 0);
  if (guestReturned)
    SR_LOG_BOOT(sr::LogSeverity::Info, "Guest harness: entry point returned");
  freeGuestMemory(base, allocSize);
  return true;
}

}  // namespace guest
}  // namespace sr
