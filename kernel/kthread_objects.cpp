// iter-462 PLATFORM FIX: guest KTHREAD objects + ObReferenceObjectByHandle.
// Contract and evidence: kernel/kthread_objects.hpp. Reference (BSD-3):
// xenia-canary xboxkrnl_ob.cc:245-283, xthread.cc:180-200/480-489/520-527.
#include "kthread_objects.hpp"
#include "kpcr.hpp"   // iter-463 SR_PROCTYPE

#include "guest_context.hpp"
#include "phys_lifo.hpp"          // iter-465 physmem: PhysKindScope
#include "thread_mem_xenia.hpp"   // iter-465 physmem: SR_THREAD_MEM_XENIA overflow blocks
#include "../app/log.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // iter-463 SR_KE_RESUME_THREAD (host thread resume)
#endif

namespace sr {
namespace kernel {

namespace {
std::atomic<int> g_obKThreadTestMode{-1};   // -1 env, 0 off, 1 on

// handle -> (KTHREAD VA, open count). Handle numbers come from
// allocUnifiedHandle (monotonic, never reused), so an erased entry can never
// shadow a later handle.
struct HandleRec {
  uint32_t kt;
  uint32_t opens;
};
std::mutex g_handleMu;
std::unordered_map<uint32_t, HandleRec> g_handles;

// Bump arena over the harness-private carveout. Blocks are never freed.
std::mutex g_arenaMu;
uint32_t g_arenaCursor = kKThreadArenaLo;
bool g_arenaExhaustLogged = false;
// iter-465 physmem: blocks served after the arena filled (kthreadIsBlockVA).
std::unordered_set<uint32_t> g_overflowBlocks;   // under g_arenaMu

thread_local uint32_t t_self = 0;   // this host thread's KTHREAD VA

std::atomic<uint32_t> g_obRefCalls{0};
std::atomic<uint32_t> g_anonLogs{0};
std::atomic<uint32_t> g_liveCloseLogs{0};
std::atomic<uint32_t> g_registerMismatchLogs{0};

bool shouldLogN(uint32_t n, uint32_t firstN) {
  return n <= firstN || (n & (n - 1)) == 0;
}

// One 0xAB0 block: arena first, guestPhysAlloc once the arena is exhausted.
uint32_t allocBlockVA() {
  std::lock_guard<std::mutex> lk(g_arenaMu);
  if (g_arenaCursor + kKThreadStride <= kKThreadArenaHi) {
    const uint32_t va = g_arenaCursor;
    g_arenaCursor += kKThreadStride;
    return va;
  }
  // iter-465 physmem (SR_THREAD_MEM_XENIA): the overflow block comes from Xenia's
  // system heap (Memory::SystemHeapAlloc, v00000000 [0x30000000, 0x40000000)), not
  // from physical memory, where a never-freed block splits the freed world pool.
  const bool xen = threadmem::enabled();
  if (!g_arenaExhaustLogged) {
    g_arenaExhaustLogged = true;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[OBREF] KTHREAD arena [%08X,%08X) exhausted; falling back to %s",
                kKThreadArenaLo, kKThreadArenaHi,
                xen ? "the Xenia system heap" : "guestPhysAlloc");
  }
  uint32_t va = xen ? threadmem::allocSystem(kKThreadSize) : 0u;
  if (!va) {
    PhysKindScope pk(PhysKind::kKThread);
    va = guestPhysAlloc(kKThreadSize, 16u);
  }
  if (va) g_overflowBlocks.insert(va);
  return va;
}
}  // namespace

bool kthreadIsBlockVA(uint32_t va) {
  if (va >= kKThreadArenaLo && va < kKThreadArenaHi) return true;
  std::lock_guard<std::mutex> lk(g_arenaMu);
  return g_overflowBlocks.count(va) != 0;
}

bool obKThreadEnabled() {
  const int m = g_obKThreadTestMode.load(std::memory_order_relaxed);
  if (m >= 0) return m != 0;
  static const bool v = [] {
    const char* e = std::getenv("SR_OB_KTHREAD");
    return !e || e[0] != '0';
  }();
  return v;
}

void obKThreadSetEnabledForTest(int mode) {
  g_obKThreadTestMode.store(mode < 0 ? -1 : (mode ? 1 : 0), std::memory_order_relaxed);
}

// Zero the block and write the fields Xenia's XThread::InitializeGuestObject
// writes that a reader of this title can observe (all big-endian).
static bool kthreadInitBlock(uint32_t kt, uint32_t threadId, uint32_t startVA,
                             uint32_t stackVA, uint32_t stackSize,
                             uint32_t creationFlags) {
  if (!obKThreadEnabled() || kt == 0) return false;
  // Address-space bound for the whole block (arena / phys grants are committed).
  if (!guestVAToHost(kt) || !guestVAToHost(kt + kKThreadSize - 1)) return false;
  std::memset(guestVAToHost(kt), 0, kKThreadSize);
  writeGuestBE32(kt + kKtOffType, uint32_t(kDispatcherThread) << 24);   // type=6, rest 0
  writeGuestBE32(kt + kKtOffSignalState, 0u);
  writeGuestBE32(kt + kKtOffWaitList + 0, kt + kKtOffWaitList);         // empty wait list
  writeGuestBE32(kt + kKtOffWaitList + 4, kt + kKtOffWaitList);
  writeGuestBE32(kt + kKtOffMutantsList + 0, kt + kKtOffMutantsList);   // empty mutants list
  writeGuestBE32(kt + kKtOffMutantsList + 4, kt + kKtOffMutantsList);
  writeGuestBE32(kt + kKtOffStackBase, stackVA ? stackVA + stackSize : 0u);
  writeGuestBE32(kt + kKtOffStackLimit, stackVA);
  writeGuestBE32(kt + kKtOffThreadId, threadId);
  writeGuestBE32(kt + kKtOffStartAddress, startVA);
  writeGuestBE32(kt + kKtOffCreationFlags, creationFlags);
  // iter-463 SR_PROCTYPE: Xenia xthread.cc:207-217 copies the process type into every KTHREAD;
  // every thread the title creates (and the harness main thread) belongs to the TITLE process.
  // The XAudio worker re-types itself SYSTEM (audio_render_driver.cpp). No-op with the gate off.
  kthreadSetProcessType(kt, kProcTypeTitle);
  return true;
}

// Xenia XThread::Exit order: exit_status, terminated, then signal_state last
// (release fence) so a reader that sees signal_state=1 also sees the code.
static void kthreadMarkExitedVA(uint32_t kt, uint32_t code) {
  if (!obKThreadEnabled() || kt == 0) return;
  // Word at +0xBC: suspend_count | was_preempted | terminated | current_cpu.
  const uint32_t w = readGuestU32BE(kt + 0xBCu);
  if (w & 0x0000FF00u) return;   // already terminated: idempotent
  writeGuestBE32(kt + kKtOffExitStatus, code);
  writeGuestBE32(kt + 0xBCu, (w & 0xFFFF00FFu) | 0x00000100u);   // terminated = 1
  std::atomic_thread_fence(std::memory_order_release);
  writeGuestBE32(kt + kKtOffSignalState, 1u);
}

uint32_t kthreadCreate(uint32_t handle, uint32_t startVA, uint32_t stackVA,
                       uint32_t stackSize, uint32_t creationFlags) {
  if (!obKThreadEnabled()) return 0;
  const uint32_t kt = allocBlockVA();
  if (!kthreadInitBlock(kt, handle, startVA, stackVA, stackSize, creationFlags)) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[OBREF] kthreadCreate handle=%u: no block (kt=%08X)",
                handle, kt);
    return 0;
  }
  std::lock_guard<std::mutex> lk(g_handleMu);
  g_handles[handle] = HandleRec{kt, 1u};
  return kt;
}

void kthreadBindCurrent(uint32_t handle) {
  if (!obKThreadEnabled()) return;
  std::lock_guard<std::mutex> lk(g_handleMu);
  auto it = g_handles.find(handle);
  if (it != g_handles.end()) t_self = it->second.kt;
}

std::atomic<int> g_bindVaTestMode{-1};
void kthreadBindByVASetForTest(int mode) {
  g_bindVaTestMode.store(mode < 0 ? -1 : (mode ? 1 : 0), std::memory_order_relaxed);
}

bool kthreadBindByVAEnabled() {
  const int m = g_bindVaTestMode.load(std::memory_order_relaxed);
  if (m >= 0) return m != 0;
  static const bool v = [] {
    const char* e = std::getenv("SR_KT_BIND_VA");
    return !e || e[0] != '0';
  }();
  return v;
}

void kthreadBindCurrentVA(uint32_t kt) {
  if (!obKThreadEnabled() || kt == 0) return;
  t_self = kt;
}

uint32_t kthreadCurrent(bool createIfMissing) {
  if (!obKThreadEnabled()) return 0;
  if (t_self) return t_self;
  const uint32_t h = getCurrentThreadHandle();
  if (h) {
    std::lock_guard<std::mutex> lk(g_handleMu);
    auto it = g_handles.find(h);
    if (it != g_handles.end()) {
      t_self = it->second.kt;
      return t_self;
    }
  }
  if (!createIfMissing) return 0;
  // A thread the guest did not create through ExCreateThread (the harness main
  // thread, host threads): an anonymous KTHREAD. It gets a handle only when the
  // guest asks for one (NtDuplicateObject(-2)).
  const uint32_t kt = allocBlockVA();
  if (!kthreadInitBlock(kt, h, 0u, 0u, 0u, 0u)) return 0;
  t_self = kt;
  const uint32_t n = g_anonLogs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (shouldLogN(n, 20))
    SR_LOG_BOOT(sr::LogSeverity::Info, "[OBREF] anon KTHREAD #%u kt=%08X curHandle=%u", n, kt, h);
  return kt;
}

void kthreadRegisterHandle(uint32_t handle, uint32_t kt) {
  if (!obKThreadEnabled() || handle == 0 || kt == 0) return;
  std::lock_guard<std::mutex> lk(g_handleMu);
  auto it = g_handles.find(handle);
  if (it == g_handles.end()) {
    g_handles.emplace(handle, HandleRec{kt, 1u});
    return;
  }
  // Our NtDuplicateObject(-2) hands back the thread's OWN handle number, so a
  // duplicate is one more open of the existing entry, never a replacement.
  it->second.opens++;
  if (it->second.kt != kt) {
    const uint32_t n = g_registerMismatchLogs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (shouldLogN(n, 10))
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[OBREF] register handle=%u kt=%08X but mapped kt=%08X (kept) opens=%u",
                  handle, kt, it->second.kt, it->second.opens);
  }
}

void kthreadAliasHandle(uint32_t newHandle, uint32_t srcHandle) {
  if (!obKThreadEnabled() || newHandle == 0) return;
  std::lock_guard<std::mutex> lk(g_handleMu);
  auto it = g_handles.find(srcHandle);
  if (it == g_handles.end()) return;
  const uint32_t kt = it->second.kt;
  auto dst = g_handles.find(newHandle);
  if (dst == g_handles.end()) g_handles.emplace(newHandle, HandleRec{kt, 1u});
  else dst->second.opens++;
}

void kthreadCloseHandle(uint32_t handle) {
  if (!obKThreadEnabled()) return;
  uint32_t erasedKt = 0;
  {
    std::lock_guard<std::mutex> lk(g_handleMu);
    auto it = g_handles.find(handle);
    if (it == g_handles.end()) return;
    if (it->second.opens > 1) {
      it->second.opens--;
      return;
    }
    erasedKt = it->second.kt;
    g_handles.erase(it);
  }
  // Closing the last handle of a thread that still runs is legal (the thread
  // keeps running; the handle is just gone), but it is the shape that used to
  // clone workers, so name it.
  if ((readGuestU32BE(erasedKt + kKtOffSignalState) & 0xFFu) == 0) {
    const uint32_t n = g_liveCloseLogs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (shouldLogN(n, 20))
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[OBREF] close #%u lr=%08X handle=%u kt=%08X: last handle of a LIVE thread",
                  n, getCurrentGuestLR(), handle, erasedKt);
  }
}

void kthreadMarkExited(uint32_t code) {
  if (!obKThreadEnabled()) return;
  kthreadMarkExitedVA(kthreadCurrent(false), code);
}

void kthreadMarkHandleExited(uint32_t handle, uint32_t code) {
  if (!obKThreadEnabled()) return;
  kthreadMarkExitedVA(kthreadLookup(handle), code);
}

uint32_t obReferenceObjectByHandle(uint32_t handle, uint32_t typeVA, uint32_t outVA) {
  if (!obKThreadEnabled()) return 0;
  // ob.cc:249 - the kernel pre-initialises *out to 0.
  if (outVA) writeGuestBE32(outVA, 0u);
  uint32_t kt = (handle == kCurrentThreadPseudoHandle) ? kthreadCurrent(true)
                                                       : kthreadLookup(handle);
  uint32_t status = 0;
  if (kt == 0) {
    status = kObStatusInvalidHandle;
  } else if (typeVA != 0) {
    // Only thread objects have guest bodies here. Compare against the value the
    // guest itself loads (the ExThreadObjectType import cell), and accept any
    // type while that cell is unresolved.
    const uint32_t threadType = readGuestU32BE(kExThreadObjectTypeCell);
    if (threadType != 0 && typeVA != threadType) status = kObStatusObjectTypeMismatch;
  }
  if (status == 0 && outVA) writeGuestBE32(outVA, kt);

  const uint32_t n = g_obRefCalls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (shouldLogN(n, 40)) {
    const uint32_t sig = kt ? (readGuestU32BE(kt + kKtOffSignalState) & 0xFFu) : 0u;
    const uint32_t ex = kt ? readGuestU32BE(kt + kKtOffExitStatus) : 0u;
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[OBREF] #%u lr=%08X h=%08X type=%08X kt=%08X status=%08X signal=%u exit=%08X",
                n, getCurrentGuestLR(), handle, typeVA, kt, status, sig, ex);
  }
  return status;
}

// ── iter-463 SR_KE_RESUME_THREAD ───────────────────────────────────────────────────────────
namespace {
std::atomic<int> g_keResumeTestMode{-1};
std::mutex g_ktHostMu;
std::unordered_map<uint32_t, HANDLE> g_ktHost;   // KTHREAD VA -> duplicated host thread handle
}  // namespace

bool keResumeThreadEnabled() {
  const int m = g_keResumeTestMode.load(std::memory_order_relaxed);
  if (m >= 0) return m != 0;
  static const bool v = [] {
    const char* e = std::getenv("SR_KE_RESUME_THREAD");   // iter-463 rolls: default ON (=0 keeps the A/B)
    return !e || e[0] != '0';
  }();
  return v;
}

void keResumeThreadSetEnabledForTest(int mode) { g_keResumeTestMode.store(mode); }

void kthreadRecordHostThread(uint32_t kt, void* hostThread) {
  if (!kt || !hostThread) return;
  HANDLE dup = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), (HANDLE)hostThread, GetCurrentProcess(), &dup,
                       THREAD_SUSPEND_RESUME | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0))
    return;
  std::lock_guard<std::mutex> lk(g_ktHostMu);
  auto it = g_ktHost.find(kt);
  if (it != g_ktHost.end()) CloseHandle(it->second);   // a reused block (never freed today)
  g_ktHost[kt] = dup;
}

long kthreadResumeHost(uint32_t kt) {
  HANDLE h = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_ktHostMu);
    auto it = g_ktHost.find(kt);
    if (it != g_ktHost.end()) h = it->second;
  }
  if (!h) return -1;
  const DWORD prev = ResumeThread(h);
  return prev == (DWORD)-1 ? -1 : (long)prev;
}

uint32_t kthreadLookup(uint32_t handle) {
  std::lock_guard<std::mutex> lk(g_handleMu);
  auto it = g_handles.find(handle);
  return it != g_handles.end() ? it->second.kt : 0u;
}

uint32_t kthreadOpenCount(uint32_t handle) {
  std::lock_guard<std::mutex> lk(g_handleMu);
  auto it = g_handles.find(handle);
  return it != g_handles.end() ? it->second.opens : 0u;
}

uint32_t kthreadArenaCursorForTest(uint32_t newCursor) {
  std::lock_guard<std::mutex> lk(g_arenaMu);
  const uint32_t old = g_arenaCursor;
  g_arenaCursor = newCursor;
  return old;
}

}  // namespace kernel
}  // namespace sr
