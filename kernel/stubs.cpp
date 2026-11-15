#include "stubs.hpp"
#include "sr_oracle.hpp"
#include "cs_reinit.hpp"       // step 6 I8: RtlInitializeCriticalSection's re-init counts
#include "native_meter.hpp"   // iter-467 native meter
#include "native_flush.hpp"
#include "native_display.hpp"
#include "native_vblank.hpp"   // display.registers (SR_NATIVE_VBLANK, default ON since native/dreg-bake)
#include "graphics_interrupt_registration.hpp"  // VD / CP delivery leases
#include "guest_protect.hpp"   // native/dreg-hang: the host source 0's arithmetic admission refuses under its diagnostics
#include "guest_context.hpp"
#include "guest_call.hpp"   // iter-345c: VD graphics-interrupt (vblank) service
#include "file_io.hpp"
#include "phys_alloc.hpp"      // iter-458 H-FREE: the reusable physical allocator
#include "gpu_phys_route.hpp"  // iter-458 HR1 (Astra 072): mmAllocSizeAnomalies
#include "phys_window.hpp"     // iter-458 (Astra 043): GPU physical grant registry
#include "phys_lifo.hpp"       // iter-463: LIFO reuse (retained bytes in the free log)
#include "tex_census.hpp"      // iter-458 (Astra 068): releases feed the texture census
#include "kthread_objects.hpp" // iter-462: guest KTHREADs + ObReferenceObjectByHandle (SR_OB_KTHREAD)
#include "kpcr.hpp"            // iter-463: per-thread KPCR (SR_KPCR) + Ke/Kf spinlocks (SR_KE_SPINLOCK)
#include "xam_content.hpp"     // iter-463: XAM content packages / real saves (SR_XAM_CONTENT_FS)
#include "devtools.hpp"        // iter-463 phase 2: the level-skip virtual pad (devtool class, default off)
#include "mouse_ui.hpp"        // iter-464: the in-game mouse cursor's pad presses (platform input)
#include "mod_library.hpp"     // iter-465: the mod library overlay (neutral pad while open)
#include "costume_viewer.hpp"  // iter-465 viewer: the right stick turns / zooms the suit
#include "xam_profile.hpp"     // iter-463: XamUserReadProfileSettings (SR_XAM_PROFILE)
#include "import_fixes.hpp"    // step4/audit: Xenia-equal import answers (logic audit 9a), one gate each
#include "mod_overlay.hpp"     // iter-463: mods/<name>/ asset overlay (docs/MODS.md)
#include "mod_manager.hpp"     // shared UTF-8 host path conversion
#include "sr_timebase.hpp"     // iter-463: 50 MHz guest clock domain (SR_TIMEBASE_50MHZ)
#include "highfps.hpp"         // iter-464: PORT FEATURE high frame rate (SR_HIGHFPS, default off)
#include "ab_toggle.hpp"       // iter-467 native-a: SR_AB_PRESENT (the per-present in-roll A/B flip at VdSwap)
#include "p2_trace.hpp"        // iter-467 native-a: SR_P2_TRACE (the present record)
#include "hitch_log.hpp"       // iter-466 stutter: SR_HITCH_LOG (observe-only)
#include "ab_toggle.hpp"       // iter-467 p2: SR_VD_SCANOUT_REGS joins the in-roll A/B (SR_AB_TOGGLE)
#include "gfx/null_renderer.hpp"
#include "gpu_submit.hpp"
#include "gpu_hang_wd.hpp"  // iter-463 GPU_HANG_WD: device latch + CRITSEC holder table
#include "input_recorder.hpp"
#include "host_automation.hpp"  // iter-464 bgruns: harness keyboard channel + input isolation
#include "quit.hpp"             // tools/graceful-quit: the VD_ISR loop's safe point
#include "pc_controls.hpp"      // iter-464 controls: keyboard + mouse layout, mouse-look, Dodge
#include "host_input.hpp"     // iter-464 input lane: SDL controllers, hot-plug, rumble, prompts
#if SR_ORACLE
#include "gpu_write_watch.hpp"  // iter-466 cp lane: GPU write tracking (host kernel-mode writes)
#endif
#include "../gfx/resource_census.hpp"  // mem.resources step 2a: NtReadFile's note (SR_MEM_RESOURCES=census; no-op under SR_ORACLE=0)
#include "../gfx/resource_memory.hpp"
#include "../app/log.hpp"
#include "guest_alloc_fail.hpp"   // step4 stuck-load: [GUEST_ALLOC_FAIL], kept by the player log
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <float.h>
#include <malloc.h>
#include <windows.h>
#include <io.h>      // iter-463: _get_osfhandle (NtReadFile positioned read)
#include <Xinput.h>
#include <SDL3/SDL.h>   // iter-450S: keyboard-as-gamepad (SDL_GetKeyboardState)

namespace {
// SDL3's SDL_GetKeyboardState is a scancode-indexed `const bool*` (SDL2: `const Uint8*`, 1 = held).
// Every reader here takes the byte table the harness channel also uses; a bool is that byte (0/1).
static_assert(sizeof(bool) == 1, "the keyboard table is read as bytes");
const uint8_t* sdlKeyboardBytes() {
  return reinterpret_cast<const uint8_t*>(SDL_GetKeyboardState(nullptr));
}
}  // namespace
#include "kernel/mm_phys.hpp"        // iter-463 SR_MM_PHYSADDR
#include "kernel/apu/xma_device.hpp"  // iter-463 SR_XMA_NEW
#include "kernel/apu/audio_system.hpp"  // iter-463 SR_AUDIO_OUT
#include "kernel/perf_stats.hpp"     // iter-464 perf: SR_PERF_STATS guest swap stamps
#include "kernel/audio_cmdq_obs.hpp"  // iter-463 D-AUDIO-CMDBUF: SR_DIAG_IO_DELAY_MS (S3 control)
#include "kernel/ntavm_xenia_heap.hpp"  // iter-463 D-AUDIO-CMDBUF: SR_NTAVM_XENIA_HEAP
#include "kernel/thread_mem_xenia.hpp"  // iter-465 physmem: SR_THREAD_MEM_XENIA
#include "kernel/native_exec.hpp"  // iter-467 native A: SR_NATIVE_EXEC_PROF placement
#endif

namespace sr {
namespace kernel {

#define STUB_LOG(name, r3, r4, r5, r6, r7, r8, ret) \
  SR_LOG_KERNEL_IMPORT("%s(r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X) -> %08X", \
                       name, r3, r4, r5, r6, r7, r8, ret)

// STATUS_SUCCESS
constexpr uint32_t kStatusSuccess = 0;
constexpr uint32_t kStatusInvalidHandle = 0xC0000008u;
constexpr uint32_t kStatusInvalidInfoClass = 0xC0000003u;
constexpr uint32_t kStatusInvalidParameter = 0xC000000Du;
constexpr uint32_t kStatusNoMemory = 0xC0000017u;

static uint64_t perfCounterTicks() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() / 100u;
}

// Guest memory is big-endian (PPC_STORE writes bswap'd values).
// All writes to guest memory from C++ stubs must also be big-endian
// so PPC_LOAD reads back the correct values.
static void writeGuest64(uint32_t guestPtr, uint64_t value) {
  uint8_t* host = guestVAToHost(guestPtr);
  if (host) {
    host[0] = (value >> 56) & 0xFF;
    host[1] = (value >> 48) & 0xFF;
    host[2] = (value >> 40) & 0xFF;
    host[3] = (value >> 32) & 0xFF;
    host[4] = (value >> 24) & 0xFF;
    host[5] = (value >> 16) & 0xFF;
    host[6] = (value >> 8) & 0xFF;
    host[7] = value & 0xFF;
  }
}

static void writeGuest32(uint32_t guestPtr, uint32_t value) {
  uint8_t* host = guestVAToHost(guestPtr);
  if (host) {
    host[0] = (value >> 24) & 0xFF;
    host[1] = (value >> 16) & 0xFF;
    host[2] = (value >> 8) & 0xFF;
    host[3] = value & 0xFF;
  }
}

// ========== Sync Object Handle Tables ==========
// Two tables:
// 1) Nt-level: guest handle ID (small integer) -> host HANDLE
//    Used by NtCreateEvent, NtCreateSemaphore, NtReleaseSemaphore, NtWaitForSingleObjectEx.
// 2) Ke-level: guest VA of kernel object -> host HANDLE
//    Used by KeInitializeEvent, KeInitializeSemaphore, KeReleaseSemaphore, KeWaitForSingleObject.
// Both events and semaphores are Win32 HANDLE, so the same tables work for both.
#ifdef _WIN32
namespace {
  // --- Nt-level handle table (handle ID -> host HANDLE) ---
  // Handle IDs come from allocUnifiedHandle() to avoid collisions with file/thread handles.
  std::mutex g_eventMutex;
  std::unordered_map<uint32_t, HANDLE> g_eventHandles;
  // step4/audit SR_NT_EVENT_TYPE (Astra P2 2): the handles in g_eventHandles that ARE events (NtCreateEvent's); the
  // table also holds semaphores and threads. Filled and read only with the gate on; guarded by g_eventMutex.
  std::unordered_set<uint32_t> g_eventTyped;
  // step4/audit test seam (difftest N11): called by NtWaitForSingleObjectEx once it holds the host HANDLE of r3,
  // before it waits. nullptr (always, outside the test) = nothing.
  std::atomic<void (*)(uint32_t)> g_ntWaitAcquiredHookForTest{nullptr};

  // --- Ke-level handle table (guest VA -> host HANDLE) ---
  std::mutex g_keSyncMutex;
  std::unordered_map<uint32_t, HANDLE> g_keSyncHandles;

  // --- Thread handle tracking ---
  // On Xbox 360, NtWaitForSingleObjectEx on a thread handle returns STATUS_USER_APC
  // when an APC fires on that thread. On Windows, it waits for thread termination.
  // We use per-thread completion events to implement Xbox 360 semantics.
  std::mutex g_threadHandleMutex;
  std::unordered_set<uint32_t> g_threadHandles;
  std::unordered_map<uint32_t, HANDLE> g_threadCompletionEvents;
}

static HANDLE eventHandleGet(uint32_t guestHandle) {
  std::lock_guard<std::mutex> lock(g_eventMutex);
  auto it = g_eventHandles.find(guestHandle);
  return (it != g_eventHandles.end()) ? it->second : nullptr;
}

// ========== iter-463 wait / semaphore platform fixes (Xenia-faithful) ==========
// Each is its own gate, default ON; =0 restores the pre-iter-463 path exactly.
// Reference: xenia-canary src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc and
// src/xenia/kernel/xobject.cc (XObject::Wait / TimeoutTicksToMs).
static bool srGateDefaultOn(const char* name) {
  const char* e = std::getenv(name);
  return !e || e[0] != '0';
}
// (a) SR_WAIT_FULL_TIMEOUT: honour the guest's timeout in full. Xenia passes
// TimeoutTicksToMs(*timeout) straight to the host wait (xobject.cc:199-205);
// the old path capped every finite Ke/Nt wait at 5000 ms "during development".
static bool waitFullTimeoutOn() {
  static const bool v = srGateDefaultOn("SR_WAIT_FULL_TIMEOUT");
  return v;
}
// (b) SR_WAIT_ALERTABLE: NtWaitForSingleObjectEx waits alertably only when the
// guest asked (Xenia xboxkrnl_threading.cc:1024-1030 -> xobject.cc:204-205
// `xe::threading::Wait(wait_handle, alertable ? true : false, ...)`). The old
// path always waited alertably, so a NON-alertable guest wait could return
// STATUS_USER_APC (0xC0) when a host APC hit the thread.
static bool waitAlertableOn() {
  static const bool v = srGateDefaultOn("SR_WAIT_ALERTABLE");
  return v;
}
// (c) SR_THREAD_WAIT_STATUS: a wait on a thread handle that completes because
// the thread exited returns STATUS_SUCCESS (xobject.cc:207-216: kSuccess ->
// X_STATUS_SUCCESS; USER_APC only for kUserCallback), honours the guest timeout
// and alertable flag, and the thread object stays signalled for every later
// waiter (a thread is a notification object: Xenia waits on the host thread
// handle itself). The old path returned 0xC0 for the exit, capped finite waits
// at 5000 ms, and used an auto-reset event so only ONE waiter ever saw the exit.
static bool threadWaitStatusOn() {
  static const bool v = srGateDefaultOn("SR_THREAD_WAIT_STATUS");
  return v;
}
// (d) SR_NTWAIT_ESCAPE_SCOPE: the 8 s INFINITE-wait escape (docs/BANDAIDS.md)
// fakes a STATUS_USER_APC return - forcing, and Xenia has no such path. With
// the gate on it runs only as named forcing (SR_ALLOW_FORCING=1) and only on
// the real main guest thread (isMainGuestThread()), never on a host-created or
// worker thread whose thread-handle slot reads <= 2. =0 restores the old
// always-on escape keyed on getCurrentThreadHandle() <= 2.
static bool ntWaitEscapeScopeOn() {
  static const bool v = srGateDefaultOn("SR_NTWAIT_ESCAPE_SCOPE");
  return v;
}
// (e) SR_SEM_STATUS: NtReleaseSemaphore status codes as in Xenia
// (xboxkrnl_threading.cc:807-832): STATUS_SEMAPHORE_LIMIT_EXCEEDED when the
// host release fails (count would pass the maximum, or count <= 0), and
// STATUS_INVALID_HANDLE for an unknown handle. The old path returned SUCCESS
// for everything and coerced count <= 0 to 1.
static bool semStatusOn() {
  static const bool v = srGateDefaultOn("SR_SEM_STATUS");
  return v;
}
// (g) GPU_SEM auto-track: the render thread's timed waits registered the waited
// handle as a "GPU completion semaphore" that a host thread then released every
// 16 ms and whose timed waits returned SUCCESS at once - releases the guest
// never made (FORCING, same class as the deleted DOGMA_AUTORELEASE). Now opt-in only:
// SR_GPU_SEM_AUTOTRACK=1 or SR_ALLOW_FORCING=1.
// Charter rule 1: forcing runs ONLY behind the master gate. SR_GPU_SEM_AUTOTRACK
// is not an enable of its own; under SR_ALLOW_FORCING=1 it defaults on and
// SR_GPU_SEM_AUTOTRACK=0 switches it off for an A/B inside a forced roll.
#if SR_ORACLE
static bool gpuSemAutotrackForcing() {
  static const bool own = srGateDefaultOn("SR_GPU_SEM_AUTOTRACK");
  return own && sr::kernel::forcingAllowed();
}
#endif
// (h) SR_READ_PENDING: NtReadFile on a handle opened WITHOUT
// FILE_SYNCHRONOUS_IO_ALERT/NONALERT returns STATUS_PENDING (the IOSB keeps the
// real status and count), and the file object is waitable (an auto-reset event
// set after every read). Xenia xboxkrnl_io.cc:167-169, xfile.cc:23/198/xfile.h:182.
// The guest's XAPI CreateFileA sets FILE_SYNCHRONOUS_IO_NONALERT only when
// FILE_FLAG_OVERLAPPED is clear (0x824688DC), so the overlapped movie/stream
// readers see ReadFile -> FALSE + ERROR_IO_PENDING, as on the console. The old
// path returned STATUS_SUCCESS for every handle.
// DEFAULT OFF (=1 enables): it needs the guest's GetLastError to read 997 after
// XAPI ReadFile's RtlNtStatusToDosError(STATUS_PENDING), and our
// RtlNtStatusToDosError was the generic stub (returns 0) until
// SR_NTSTATUS_DOSERR (Xenia's table, xam_content.cpp) was baked default ON by
// step4/audit fix batch 1 (2026-10-18); this gate was not part of that bake and
// needs its own A/B (with SR_NTSTATUS_DOSERR=0 the failure below returns).
// With PENDING on and the stub in place the stream readers see error 0 != 997,
// treat every read as failed and the intro movies are skipped (roll
// archive/v1_waitfix/e1_on1: 0 MPSM lines, psmCnt FFFFFFFF from t=0, 21
// ret=PENDING reads on fmvlegal handle 52). Bake ON together with DOSERR.
static bool readPendingOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_READ_PENDING");
    return e && e[0] == '1';
  }();
  return v;
}
// (i) SR_KESEM_STATUS: KeReleaseSemaphore passes the adjustment through and
// returns the previous count (xeKeReleaseSemaphore, xboxkrnl_threading.cc:743-758),
// and NtCreateSemaphore with an invalid count/limit fails with
// STATUS_INVALID_PARAMETER (NtCreateSemaphore_entry :785-792). The old path
// coerced adjustment <= 0 to 1, returned 0, and coerced limit <= 0 to 1024.
static bool keSemStatusOn() {
  static const bool v = srGateDefaultOn("SR_KESEM_STATUS");
  return v;
}

constexpr uint32_t kStatusSemaphoreLimitExceeded = 0xC0000047u;  // X_STATUS_SEMAPHORE_LIMIT_EXCEEDED
constexpr uint32_t kStatusEndOfFile = 0xC0000011u;               // X_STATUS_END_OF_FILE
constexpr uint32_t kStatusAccessViolationRead = 0xC0000005u;     // X_STATUS_ACCESS_VIOLATION
constexpr uint32_t kStatusPending = 0x00000103u;                 // X_STATUS_PENDING

// Guest PLARGE_INTEGER timeout (non-null VA) -> host milliseconds.
//   < 0 : relative, 100 ns units (xobject.cc:183-185)
//   = 0 : poll
//   > 0 : kept as the pre-iter-463 path read it (100 ns units). Xenia reads a
//         positive value as milliseconds (xobject.cc:180-182, a NetDll quirk);
//         neither models an NT absolute time. No guest caller seen uses one.
// SR_WAIT_FULL_TIMEOUT=0 restores the old 5000 ms cap exactly.
static DWORD guestTimeoutToMs(uint32_t timeoutVA) {
  if (!guestVAToHost(timeoutVA)) return 16;   // unreadable pointer: old short wait
  const uint32_t hi = readGuestU32BE(timeoutVA);
  const uint32_t lo = readGuestU32BE(timeoutVA + 4);
  const int64_t t = static_cast<int64_t>((static_cast<uint64_t>(hi) << 32) | lo);
  uint64_t ms64 = 0;
  if (t < 0) ms64 = (0ull - static_cast<uint64_t>(t)) / 10000ull;
  else if (t > 0) ms64 = static_cast<uint64_t>(t) / 10000ull;
  if (!waitFullTimeoutOn()) {
    DWORD ms = static_cast<DWORD>(ms64);
    return ms > 5000 ? 5000 : ms;
  }
  return ms64 >= INFINITE ? (INFINITE - 1) : static_cast<DWORD>(ms64);
}

// iter-DOGMA-RE-10: Walker watchdog hook. Called by a watchdog thread when an
// FM CTOR is detected to have stalled. Pumps a single ReleaseSemaphore on every
// known semaphore handle to wake any blocked NtWait. Cheap (idempotent — extra
// counts on an already-released sem just accumulate up to max).
unsigned sr::kernel::releaseAllPendingSemaphoresPanic() {
  unsigned released = 0;
#ifdef _WIN32
  // 1) Nt-level: NtCreateSemaphore objects (g_eventHandles, guest handle IDs).
  std::vector<HANDLE> ntSnapshot;
  {
    std::lock_guard<std::mutex> lock(g_eventMutex);
    ntSnapshot.reserve(g_eventHandles.size());
    for (auto& kv : g_eventHandles)
      if (kv.second) ntSnapshot.push_back(kv.second);
  }
  LONG prev = 0;
  for (HANDLE h : ntSnapshot)
    if (ReleaseSemaphore(h, 1, &prev)) ++released;

  // 2) Ke-level: KeWaitForSingleObject objects (g_keSyncHandles, guest VAs).
  // DOGMA allocator waits via KeWait, so these were NOT covered by the old panic.
  // Each handle can be a Semaphore or Event — try ReleaseSemaphore first, fall
  // back to SetEvent so both kinds get signaled.
  std::vector<HANDLE> keSnapshot;
  {
    std::lock_guard<std::mutex> lock(g_keSyncMutex);
    keSnapshot.reserve(g_keSyncHandles.size());
    for (auto& kv : g_keSyncHandles)
      if (kv.second) keSnapshot.push_back(kv.second);
  }
  LONG prev2 = 0;
  for (HANDLE h : keSnapshot) {
    if (ReleaseSemaphore(h, 1, &prev2)) { ++released; continue; }
    if (SetEvent(h)) ++released;
  }
#endif
  return released;
}

static bool ntEventTypeOn();   // step4/audit SR_NT_EVENT_TYPE (defined with stub_NtCreateEvent)

static void eventHandleClose(uint32_t guestHandle) {
  std::lock_guard<std::mutex> lock(g_eventMutex);
  auto it = g_eventHandles.find(guestHandle);
  if (it != g_eventHandles.end()) {
    if (it->second) CloseHandle(it->second);
    g_eventHandles.erase(it);
  }
  if (ntEventTypeOn()) g_eventTyped.erase(guestHandle);
}

void ntWaitAcquiredHookForTest(void (*hook)(uint32_t handle)) {
  g_ntWaitAcquiredHookForTest.store(hook, std::memory_order_release);
}

bool ntEventSetByHandle(uint32_t guestHandle) {
  std::lock_guard<std::mutex> lock(g_eventMutex);
  auto it = g_eventHandles.find(guestHandle);
  if (it == g_eventHandles.end() || !it->second) return false;
  if (ntEventTypeOn() && !g_eventTyped.count(guestHandle)) return false;
  return SetEvent(it->second) != 0;
}

static HANDLE keSyncHandleGet(uint32_t guestVA) {
  std::lock_guard<std::mutex> lock(g_keSyncMutex);
  auto it = g_keSyncHandles.find(guestVA);
  return (it != g_keSyncHandles.end()) ? it->second : nullptr;
}

static void keSyncHandleSet(uint32_t guestVA, HANDLE h) {
  std::lock_guard<std::mutex> lock(g_keSyncMutex);
  auto old = g_keSyncHandles.find(guestVA);
  if (old != g_keSyncHandles.end() && old->second && old->second != h)
    CloseHandle(old->second);
  g_keSyncHandles[guestVA] = h;
}

// Signal keSync objects in the game heap range (0x8A000000–0x8C000000):
// These are GPU sync events/semaphores allocated by the game's GPU init code.
// VAs vary by run, so we can't hardcode them; heap-range filter excludes fake
// device objects (0xD0010000+) which must not be spuriously signaled.
// Called from the GPU IRQ thread to simulate GPU-done DPC signaling.
static void signalKeSyncGpuEvents() {
  std::lock_guard<std::mutex> lock(g_keSyncMutex);
  static int s_log = 0;
  for (auto& [va, h] : g_keSyncHandles) {
    if (!h) continue;
    // Only signal heap-allocated objects (game GPU sync events live here).
    if (va < 0x8A000000u || va >= 0x8C000000u) continue;
    // Try SetEvent first; fall back to ReleaseSemaphore for semaphore-type objects.
    if (!SetEvent(h)) {
      LONG prev = 0;
      ReleaseSemaphore(h, 1, &prev);
    }
    if (++s_log <= 10)
      SR_LOG_KERNEL_IMPORT("[GPU_IRQ] signalKeSyncGpuEvents: VA=0x%08X", va);
  }
}

static void markAsThreadHandle(uint32_t guestHandle) {
  std::lock_guard<std::mutex> lock(g_threadHandleMutex);
  g_threadHandles.insert(guestHandle);
  if (g_threadCompletionEvents.find(guestHandle) == g_threadCompletionEvents.end()) {
    // iter-463 (c) SR_THREAD_WAIT_STATUS: manual reset - an exited thread stays
    // signalled for every waiter (Xenia waits on the host thread handle).
    const BOOL manualReset = threadWaitStatusOn() ? TRUE : FALSE;
    g_threadCompletionEvents[guestHandle] = CreateEventA(nullptr, manualReset, FALSE, nullptr);
  }
}

static bool isThreadHandle(uint32_t guestHandle) {
  std::lock_guard<std::mutex> lock(g_threadHandleMutex);
  return g_threadHandles.count(guestHandle) > 0;
}

static HANDLE getThreadCompletionEvent(uint32_t guestHandle) {
  std::lock_guard<std::mutex> lock(g_threadHandleMutex);
  auto it = g_threadCompletionEvents.find(guestHandle);
  return (it != g_threadCompletionEvents.end()) ? it->second : nullptr;
}

static void signalThreadCompletion(uint32_t guestHandle) {
  std::lock_guard<std::mutex> lock(g_threadHandleMutex);
  auto it = g_threadCompletionEvents.find(guestHandle);
  if (it != g_threadCompletionEvents.end()) SetEvent(it->second);
}

// Forward declaration — defined near CS stubs (needed by stub_ExTerminateThread).
static void releaseDeadThreadCSes(uint32_t gHandle);

static bool asyncReadOn();  // iter-466 stutter: SR_ASYNC_READ (defined with stub_NtReadFile)
// Unified lookup: try Nt-level handle table, then Ke-level VA table, then dereference guest pointer.
static HANDLE syncHandleLookup(uint32_t r3) {
  HANDLE h = eventHandleGet(r3);       // Nt-level handle ID
  if (!h) h = keSyncHandleGet(r3);      // Ke-level object at guest VA
  if (!h && guestVAToHost(r3)) {
    uint32_t deref = readGuestU32BE(r3); // r3 may be pointer containing a handle ID
    h = eventHandleGet(deref);
    if (!h) h = keSyncHandleGet(deref);
  }
  // iter-463 (h) SR_READ_PENDING: a file object is waitable (Xenia
  // XFile::GetWaitHandle = async_event_). Last, so no other table is shadowed.
  // iter-466 stutter: with SR_ASYNC_READ the file object is what a read without an event
  // completes (XAPI ReadFile waits on the file handle after STATUS_PENDING).
  if (!h && (readPendingOn() || asyncReadOn()))
    h = static_cast<HANDLE>(sr::kernel::fileHandleWaitEvent(r3));
  return h;
}

// Render thread (sub_82110A30) waits on event at context+32; log shows 0x83EEC7CC.
// Nothing on our side signals it (GPU callback would on real HW). Signal from VdSwap so the
// waiter gets signaled each frame instead of always timing out.
static void signalRenderThreadEventIfPresent() {
  constexpr uint32_t kRenderThreadEventVA = 0x83EEC7CCu;
  HANDLE h = syncHandleLookup(kRenderThreadEventVA);
  if (h) SetEvent(h);
}

#if SR_ORACLE
// Ring buffer RPTR writeback: the GPU writes its read pointer (RPTR) to a physical address
// so the CPU can check how far the GPU has processed commands. VdEnableRingBufferRPtrWriteBack
// tells us where to write the RPTR. We set RPTR = WPTR in VdSwap to simulate instant GPU
// processing, preventing the ring buffer from appearing full and blocking the render thread.
static std::atomic<uint32_t> g_rptrWritebackVA{0};
static std::atomic<uint32_t> g_lastWptrFromVdSwap{0};  // for GPU interrupt thread to re-advance RPTR
// iter-176: ring buffer base VA — kept separately so device+10768 can hold the RPTR writeback ptr
// as sub_820FD9A8 expects (it dereferences device[10768] to read RPTR, not the ring base itself).
static std::atomic<uint32_t> g_ringBufBase{0xD0010000u};

// iter-178: VA set by gpu_ring_alloc_fix when sub_82467F40 successfully allocates ring memory.
// VdInitializeRingBuffer reads this as the ring base when r3/r6 are invalid (MmGetPhysicalAddress
// returns 0 since it's stub_generic_success, so r3=0 at VdInitializeRingBuffer call site).
std::atomic<uint32_t> g_gpuRingAllocVA{0};

uint32_t getGpuRingBase() {
  return g_ringBufBase.load(std::memory_order_acquire);
}

// iter-180: VdInitializeRingBuffer fires before ring memory is allocated, so g_ringBufBase
// stays fake. Called by gpu_ring_alloc_fix after the first alloc to backfill it.
// iter-467 p2 fix (4), forcing-audit U1 (docs/OVERRIDES.json sub_82467F40; NATIVE_ROADMAP P2.9):
// SR_VD_RING_ARG (BAKED default ON iter-467; =0 is the A/B) - VdInitializeRingBuffer takes the ring from
// its argument, the physical address the title got from MmGetPhysicalAddress (real since iter-463,
// SR_MM_PHYSADDR), as Xenia's GraphicsSystem::InitializeRingBuffer does (command_processor.cc:430-440:
// size = 1 << (r4 + 3)). The sub_82467F40 override then has no ring to claim, so its pre-claim heap route
// is never taken (gpu_ring_alloc_fix.cpp Hr1Call); that route is now master-gated (SR_ALLOW_FORCING), so
// SR_VD_RING_ARG=0 alone finds no ring - the legacy claim needs SR_VD_RING_ARG=0 SR_ALLOW_FORCING=1.
// Needs SR_MM_PHYSADDR and SR_GPU_PHYS_ROUTE (both default ON). Proof (p2fix1, exe 98054cc00c58):
// [VD_RING] phys=0x1EF2E000 bytes=0x10000 -> VA 0xBEF2E000 = the first doorbell's ring, no
// [GPU_RING_ALLOC] / [RING_BASE] line, replay 9/9 SAME, whole-run check + flicker gate PASS.
bool vdRingFromArgOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_VD_RING_ARG");
    return !e || e[0] != '0';
  }();
  return v;
}

void trySetRingBufBaseFromAlloc(uint32_t allocVA) {
  uint32_t expected = 0xD0010000u;
  if (g_ringBufBase.compare_exchange_strong(expected, allocVA, std::memory_order_release)) {
    SR_LOG_KERNEL_IMPORT("[RING_BASE] Deferred VdInitializeRingBuffer: g_ringBufBase <- 0x%08X (from alloc)", allocVA);
  }
}

#else
bool vdRingFromArgOn() { return true; }
#endif

#if SR_ORACLE
// GPU completion semaphore tracking: only track semaphores that the render thread
// actually waits on after VdSwap. Previously we tracked ALL init=0 semaphores, which
// caused condvar semaphores on other threads to get spurious GPU-interrupt releases.
static std::vector<uint32_t> g_gpuCompletionSemaphoreHandles;
static std::mutex g_gpuSemMutex;
static std::atomic<DWORD> g_renderThreadNativeId{0};

static void trackGpuCompletionSemaphore(uint32_t guestHandle) {
  std::lock_guard<std::mutex> lock(g_gpuSemMutex);
  for (uint32_t gh : g_gpuCompletionSemaphoreHandles)
    if (gh == guestHandle) return;
  g_gpuCompletionSemaphoreHandles.push_back(guestHandle);
  SR_LOG_KERNEL_IMPORT("[GPU_SEM] Now tracking handle=%u (render thread wait)", guestHandle);
}

static bool isGpuCompletionSemaphore(uint32_t guestHandle) {
  std::lock_guard<std::mutex> lock(g_gpuSemMutex);
  for (uint32_t gh : g_gpuCompletionSemaphoreHandles)
    if (gh == guestHandle) return true;
  return false;
}

// Signal all tracked GPU completion semaphores (called from VdSwap and GPU interrupt thread).
static void signalGpuCompletionSemaphores() {
  std::lock_guard<std::mutex> lock(g_gpuSemMutex);
  static int s_logCount = 0;
  static int s_callCount = 0;
  ++s_callCount;
  for (uint32_t gh : g_gpuCompletionSemaphoreHandles) {
    HANDLE h = eventHandleGet(gh);
    if (h) {
      LONG prevCount = -1;
      BOOL ok = ReleaseSemaphore(h, 1, &prevCount);
      if (gh == 9 && (s_callCount <= 5 || s_callCount % 50 == 0 || !ok))
        SR_LOG_KERNEL_IMPORT("[GPU_SEM] Release handle=%u host=%p ok=%d prevCount=%ld (call#%d)",
                             gh, (void*)h, (int)ok, prevCount, s_callCount);
      else if (++s_logCount <= 40)
        SR_LOG_KERNEL_IMPORT("[GPU_SEM] Release handle=%u host=%p ok=%d prevCount=%ld",
                             gh, (void*)h, (int)ok, prevCount);
    } else {
      if (++s_logCount <= 40)
        SR_LOG_KERNEL_IMPORT("[GPU_SEM] handle=%u NOT FOUND in eventHandles!", gh);
    }
  }
  if (g_gpuCompletionSemaphoreHandles.empty() && ++s_logCount <= 5) {
    SR_LOG_KERNEL_IMPORT("[GPU_SEM] No GPU completion semaphores tracked!");
  }
}

// GPU interrupt simulation thread: periodically signals GPU completion semaphores.
// On real Xbox 360 hardware, the GPU fires an interrupt after processing each command batch,
// and the interrupt DPC signals completion events. We simulate this with a background thread
// running at ~60Hz (16ms interval) that signals all tracked GPU completion semaphores.
static std::atomic<bool> g_gpuInterruptRunning{false};
static std::thread g_gpuInterruptThread;

// Advance fence VAs so a spinning render thread (post-VdSwap poll) can proceed.
// Use a monotonically increasing "logical frame" so after ring wrap we keep advancing
// even when VdSwap count is stuck (render thread blocked before next VdSwap).
// Also re-advance RPTR so sub_820FD9A8's ring-buffer wait (render thread stuck after
// frame 34) can proceed when the ring appears "full".
static void advanceFencesIfStalled() {
  static std::atomic<uint32_t> s_logicalFrame{0};
  uint32_t nextVal = ++s_logicalFrame;
  constexpr uint32_t kMainFenceVA = 0x9480C000u;
  constexpr uint32_t kFenceVA2   = 0x9480C004u;
  uint32_t cur0 = readGuestU32BE(kMainFenceVA);
  uint32_t cur4 = readGuestU32BE(kFenceVA2);
  if (nextVal > cur0) writeGuest32(kMainFenceVA, nextVal);
  if (nextVal > cur4) writeGuest32(kFenceVA2, nextVal);
  // Re-advance RPTR so ring-buffer wait in sub_820FD9A8 can exit (stall after frame 34).
  uint32_t rptrVA = g_rptrWritebackVA.load(std::memory_order_acquire);
  uint32_t lastWptr = g_lastWptrFromVdSwap.load(std::memory_order_acquire);
  if (rptrVA != 0 && lastWptr != 0) {
    writeGuest32(rptrVA, lastWptr);
  }
}

static void gpuInterruptThreadFunc() {
  SR_LOG_KERNEL_IMPORT("[GPU_IRQ] GPU interrupt simulation thread started (16ms interval)");
  int signalCount = 0;
  while (g_gpuInterruptRunning.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    if (!g_gpuInterruptRunning.load(std::memory_order_relaxed)) break;
    // Advance fences so a render-thread fence poll can proceed if it's waiting.
    advanceFencesIfStalled();
    // iter-358h: processGpuCommandsBackground() deleted — it scanned PAST the
    // write pointer in the fake 8-byte slot format and wrote fence values to
    // addresses decoded from wrong offsets (arbitrary guest-memory scribbler).
    // Real fences (EVENT_WRITE_SHD) execute in-order via the shared PM4 engine
    // in onVdSwapFrame.
    signalGpuCompletionSemaphores();
    // GPU sync events (heap-range NotificationEvent objects, VA varies per run):
    // NOT signaled from the GPU IRQ thread.  Signaling here permanently sets the
    // manual-reset event → STATUS_SUCCESS on the very first KeWait → render thread
    // exits the GPU wait loop immediately, skipping VdSwap render work.
    // signalKeSyncGpuEvents();  // intentionally disabled
    // FIX iter-149: kernel GPU fence at VA=0x00000000 — the game's vtable dispatcher
    // calls KeWaitForSingleObject(0x00000000, INFINITE) waiting for GPU completion.
    // On real HW the GPU interrupt DPC calls KeSetEvent(0x00000000). Replicate here.
    // VA=0 is kernel-range (not heap), so this doesn't trigger the heap-object issue.
    {
      HANDLE hFence = keSyncHandleGet(0x00000000u);
      if (hFence) {
        static std::atomic<int> s_fenceLog{0};
        int n = s_fenceLog.fetch_add(1, std::memory_order_relaxed);
        if (n < 5 || n % 1000 == 0)
          SR_LOG_KERNEL_IMPORT("[GPU_IRQ] signalKernelGpuFence VA=0 host=%p (tick#%d)", (void*)hFence, signalCount);
        SetEvent(hFence);
      }
    }
    // FIX iter-163: GPU IO range fence at VA=0xD0011000 — after 4 VdSwaps the game
    // calls KeWaitForSingleObject(0xD0011000, INFINITE) waiting for GPU ring completion.
    // Type=0xD0 (unknown to our kernel) → auto-reset event, never signaled → infinite stall.
    // On real HW the GPU DPC fires when the ring read-pointer catches the write-pointer.
    // Signal it here the same way we handle VA=0. Auto-reset → no permanent-signaled issue.
    {
      HANDLE hFence163 = keSyncHandleGet(0xD0011000u);
      if (hFence163) {
        static std::atomic<int> s_fence163Log{0};
        int n163 = s_fence163Log.fetch_add(1, std::memory_order_relaxed);
        if (n163 < 5 || n163 % 1000 == 0)
          SR_LOG_KERNEL_IMPORT("[GPU_IRQ] signalGpuFence VA=0xD0011000 host=%p (tick#%d)", (void*)hFence163, signalCount);
        SetEvent(hFence163);
      }
    }
    signalCount++;
    if (signalCount <= 5 || signalCount % 300 == 0) {
      SR_LOG_KERNEL_IMPORT("[GPU_IRQ] tick #%d", signalCount);
    }
  }
  SR_LOG_KERNEL_IMPORT("[GPU_IRQ] GPU interrupt simulation thread stopped");
}

static void startGpuInterruptThread() {
  // iter-463 charter: a fake 16 ms GPU IRQ (host-counter fences, RPTR := WPTR,
  // releases every tracked GPU semaphore, SetEvent on VA 0 / 0xD0011000) is
  // FORCING. Only the legacy synchronous VdSwap path (SR_PM4_CP=0) reaches
  // here; the async CP is the platform implementation. Master-gated.
  if (!sr::kernel::forcingAllowed()) return;
  if (g_gpuInterruptRunning.load()) return;
  g_gpuInterruptRunning.store(true, std::memory_order_relaxed);
  g_gpuInterruptThread = std::thread(gpuInterruptThreadFunc);
  g_gpuInterruptThread.detach();
}

static void stopGpuInterruptThread() {
  g_gpuInterruptRunning.store(false, std::memory_order_relaxed);
}
#endif  // SR_ORACLE: synthetic interrupt worker and semaphore tracking


#endif

static void writeGuest16(uint32_t guestPtr, uint16_t value) {
  uint8_t* host = guestVAToHost(guestPtr);
  if (host) {
    host[0] = (value >> 8) & 0xFF;
    host[1] = value & 0xFF;
  }
}

// ========== TLS Storage ==========
// Thread Local Storage implementation for Xbox 360 kernel stubs.
// Slot indices are process-wide (g_nextTlsIndex is global), but values are per-thread.
// iter-DOGMA-RE-2: bumped from 64 → 8192. Observed iter-37 logs spam
// `KeTlsGetValue(index=4096) FAIL (invalid)` — that's a real index the game
// uses (probably a kernel-reserved slot range), and our 64-slot cap was
// truncating it. Fail mode: returns 0, which is indistinguishable from
// "successfully read 0" — so callers may have been silently using 0 as a
// function pointer or pool VA, breaking work-queue dispatch in the workers.
static constexpr uint32_t kMaxTlsSlots = 8192;
static thread_local uint32_t g_tlsValues[kMaxTlsSlots] = {0};
// Module-encoded TLS indices like 262151 (0x40007) exceed the fixed array.
// Store large indices in a per-thread overflow map so they round-trip correctly.
static thread_local std::unordered_map<uint32_t, uint32_t> g_tlsOverflow;
static std::atomic<uint32_t> g_nextTlsIndex{1};  // 0 can indicate failure, so start at 1

// iter-364 arena-trace: settle where the guest dlmalloc arena is placed (rewind vs
// bypass of guestHeapAlloc's g_heapNext). Logs every NtAVM call's base/size/type/branch/
// result so the reserve(base=0)->commit(base!=0) reserve-then-commit pairs are visible.
static const bool s_arenaTrace = [] {
  const char* e = std::getenv("SR_ARENA_TRACE");
  return e && e[0] == '1';
}();

// iter-395: NtAVM allocation registry backing a REAL MmQueryAllocationSize.
// The golden heap-header diff came down to ONE WORD: console heap +0x48
// (segment/reserve end) = base+reserveSize; ours = ZERO — because the heap
// creator learns its allocation's extent via MmQueryAllocationSize, which was
// a generic 0-returning stub. With end=0 the heap's unsigned room math never
// exhausts, so it carves past its segment without ever committing (the
// "grows without NtAVM" wall = the FX-region overlap = effects 0/120).
namespace {
struct NtavmGrant { uint32_t base, size; };
std::mutex g_ntavmGrantsMu;
std::vector<NtavmGrant> g_ntavmGrants;
void recordNtavmGrant(uint32_t base, uint32_t size) {
  std::lock_guard<std::mutex> lk(g_ntavmGrantsMu);
  for (auto& g : g_ntavmGrants) {
    if (base >= g.base && base < g.base + g.size) {
      // commit inside an existing reserve — extend if it outgrows the record
      const uint32_t end = base + size, gend = g.base + g.size;
      if (end > gend) g.size = end - g.base;
      return;
    }
  }
  g_ntavmGrants.push_back({base, size});
}
// iter-463: drop the record of a released region (exact base) so a later grant at
// the same VA does not inherit its extent.
void eraseNtavmGrant(uint32_t base) {
  std::lock_guard<std::mutex> lk(g_ntavmGrantsMu);
  for (size_t i = 0; i < g_ntavmGrants.size(); ++i)
    if (g_ntavmGrants[i].base == base) {
      g_ntavmGrants.erase(g_ntavmGrants.begin() + static_cast<long>(i));
      return;
    }
}
uint32_t lookupNtavmGrant(uint32_t va, uint32_t* baseOut) {
  std::lock_guard<std::mutex> lk(g_ntavmGrantsMu);
  for (const auto& g : g_ntavmGrants) {
    if (va >= g.base && va < g.base + g.size) {
      if (baseOut) *baseOut = g.base;
      return g.size;
    }
  }
  return 0;
}

// ===== iter-401 PLATFORM FIX: virtual memory is RECYCLED on release ========
// The console kernel returns released VA to the system; ours never did —
// NtFreeVirtualMemory was a generic success-stub and guestHeapAlloc is a
// forward bump, so every grant was permanently lost. Measured: 373.8 MB
// granted across ~5 minutes of intro-movie playback, 0 bytes recovered, and
// the run dies with `NtAllocateVirtualMemory FAILED: size=0x40520 (no memory)`
// exactly when the 6th movie's WMV3 decoder asks for its arena — after which
// XMV_START fails and the create path spins forever on a pipeline whose
// workers were never started (the iter-400 ENTER-no-EXIT wall).
//
// The title frees through dlmalloc's Win32 two-step (MEM_DECOMMIT at
// lr=8246C290, then MEM_RELEASE at lr=8246D5B0 for the same segment):
//   MEM_RELEASE  (0x8000) — the reservation is gone. Recycle the whole grant.
//   MEM_DECOMMIT (0x4000) — the VA stays RESERVED and the guest may re-commit
//                           in place, so DO NOT hand it to anyone else; just
//                           zero it (NT hands back zero pages on re-commit).
// Recycling a still-reserved range is how this project earned five separate
// two-allocator-overlap disasters — hence the asymmetry above, deliberately.
//
// Only ranges WE granted from the bump are recycled: commit-in-place requests
// (guest-chosen base) are the guest's own reservations and are left alone.
// EXACT bump grants, tracked separately from g_ntavmGrants on purpose:
// recordNtavmGrant EXTENDS its records when a commit lands inside an existing
// reserve, so a record is an over-approximation of any single allocation.
// Recycling off that record frees memory the guest never released — the
// over-free that produced guest-thread AVs on the first attempt at this fix.
// This registry stores what the bump actually handed out, verbatim, and is the
// ONLY thing eligible for reuse.
struct NtavmBumpGrant { uint32_t base, size; };
std::mutex g_ntavmBumpMu;
std::vector<NtavmBumpGrant> g_ntavmBump;

void recordBumpGrant(uint32_t base, uint32_t size) {
  std::lock_guard<std::mutex> lk(g_ntavmBumpMu);
  for (auto& g : g_ntavmBump)
    if (g.base == base) { g.size = size; return; }
  g_ntavmBump.push_back({base, size});
}

// Exact-base match only. Returns 0 if this VA was not a bump grant (guest-chosen
// commit-in-place bases, sub-range frees, and anything else are left alone).
uint32_t takeBumpGrant(uint32_t base) {
  std::lock_guard<std::mutex> lk(g_ntavmBumpMu);
  for (size_t i = 0; i < g_ntavmBump.size(); ++i) {
    if (g_ntavmBump[i].base != base) continue;
    const uint32_t sz = g_ntavmBump[i].size;
    g_ntavmBump.erase(g_ntavmBump.begin() + static_cast<long>(i));
    return sz;
  }
  return 0;
}

struct NtavmFreeBlock { uint32_t base, size; };
std::mutex g_ntavmFreeMu;
std::vector<NtavmFreeBlock> g_ntavmFree;
std::atomic<uint64_t> g_ntavmRecycledBytes{0};
std::atomic<uint64_t> g_ntavmReusedBytes{0};

bool ntavmRecycleEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_NTAVM_RECYCLE");
    return !e || e[0] != '0';
  }();
  return s_on;
}

// Insert sorted + coalesce with neighbours, so a movie's segments come back as
// one large block rather than a shredded list.
void ntavmFreeInsert(uint32_t base, uint32_t size) {
  if (!size) return;
  std::lock_guard<std::mutex> lk(g_ntavmFreeMu);
  size_t i = 0;
  while (i < g_ntavmFree.size() && g_ntavmFree[i].base < base) ++i;
  g_ntavmFree.insert(g_ntavmFree.begin() + static_cast<long>(i), {base, size});
  // coalesce backwards then forwards
  if (i > 0 && g_ntavmFree[i - 1].base + g_ntavmFree[i - 1].size == g_ntavmFree[i].base) {
    g_ntavmFree[i - 1].size += g_ntavmFree[i].size;
    g_ntavmFree.erase(g_ntavmFree.begin() + static_cast<long>(i));
    --i;
  }
  if (i + 1 < g_ntavmFree.size() &&
      g_ntavmFree[i].base + g_ntavmFree[i].size == g_ntavmFree[i + 1].base) {
    g_ntavmFree[i].size += g_ntavmFree[i + 1].size;
    g_ntavmFree.erase(g_ntavmFree.begin() + static_cast<long>(i + 1));
  }
  g_ntavmRecycledBytes.fetch_add(size, std::memory_order_relaxed);
}

// First fit; splits the remainder back into the list. Returns 0 when nothing
// fits, so the caller falls through to the bump exactly as before.
uint32_t ntavmFreeTake(uint32_t size) {
  if (!size) return 0;
  std::lock_guard<std::mutex> lk(g_ntavmFreeMu);
  for (size_t i = 0; i < g_ntavmFree.size(); ++i) {
    if (g_ntavmFree[i].size < size) continue;
    const uint32_t va = g_ntavmFree[i].base;
    if (g_ntavmFree[i].size == size) {
      g_ntavmFree.erase(g_ntavmFree.begin() + static_cast<long>(i));
    } else {
      g_ntavmFree[i].base += size;
      g_ntavmFree[i].size -= size;
    }
    g_ntavmReusedBytes.fetch_add(size, std::memory_order_relaxed);
    return va;
  }
  return 0;
}
}  // namespace

// iter-437f PLATFORM FIX — XamAlloc with its REAL contract. It was registered
// as stub_RtlAllocateHeap ("same as heap alloc"), but the signatures are
// incompatible: XamAlloc(dwFlags r3, dwSize r4, ppvBuffer r5) returns an
// HRESULT and writes the buffer to *ppvBuffer; RtlAllocateHeap(heap, flags,
// SIZE r5) returns the pointer in r3. Under the old mapping the game's one
// XamAlloc call read r5 (the OUT POINTER) as a size, returned the buffer VA
// where an HRESULT belongs (huge value = FAILED hr), and never wrote *ppv.
//
// MEASURED CONSEQUENCE (the movie chroma bottom-strip): the movie system
// calls XamAlloc for its plane buffers; seeing "failure" it fell back to its
// emergency path — packing Y/U/V after its decoder arena at base+0x6A0000
// with U->V stride 0x43000, 2KB SHORT of the U plane's own 768x360 fetch
// extent, so sampled chroma rows 357+ read V-plane bytes = wrong chroma on
// the last ~6 screen rows of every movie. On Xenia XamAlloc succeeds and the
// planes are separate page-granular allocations (frida-verified descending
// 0x44000 spacing, clean bottom rows).
//
// 4KB alignment: plane buffers become TEXTURE bases; the Xenos fetch encodes
// base bits [31:12], so sub-page bases are unrepresentable. xam's pool hands
// out page-aligned blocks for these sizes; we guarantee it by over-allocating
// and rounding up (bump heap never frees — the slack is accounted).
static uint32_t stub_XamAlloc(uint32_t r3, uint32_t r4, uint32_t r5,
                              uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r6; (void)r7; (void)r8;
  const uint32_t size = r4 ? r4 : 1;
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!r5) return 0x80070057u;  // E_INVALIDARG
  uint32_t va = sr::kernel::guestHeapAlloc(size + 0x1FFFu);
  if (!va) {
    writeGuest32(r5, 0);
    SR_LOG_KERNEL_IMPORT("[XAM_ALLOC] #%u size=0x%X FAILED (heap exhausted)", n, r4);
    sr::kernel::noteGuestAllocFailure(sr::kernel::GuestAllocApi::XamAlloc, r4, r3, 0, "heap exhausted");
    return 0x8007000Eu;  // E_OUTOFMEMORY
  }
  va = (va + 0xFFFu) & ~0xFFFu;  // page-align for texture-base use
  writeGuest32(r5, va);
  if (n <= 16 || n % 500 == 0)
    SR_LOG_KERNEL_IMPORT("[XAM_ALLOC] #%u flags=0x%X size=0x%X -> 0x%08X (S_OK)", n, r3, r4, va);
  return 0;  // S_OK
}

uint32_t stub_MmQueryAllocationSize(uint32_t r3, uint32_t r4, uint32_t r5,
                                    uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  // Console contract: returns the size of the NtAVM allocation containing the
  // address (the CRT heap creator queries its own base to set segment end).
  // SR_MMQAS=0 restores the legacy generic-0 for A/B.
  static const bool s_on = [] {
    const char* e = std::getenv("SR_MMQAS");
    return !e || e[0] != '0';
  }();
  uint32_t base = 0;
  const uint32_t sz = s_on ? lookupNtavmGrant(r3, &base) : 0;
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 24 || (n & (n - 1)) == 0)
    SR_LOG_KERNEL_IMPORT("[MMQAS] #%u va=0x%08X -> size=0x%X (base=0x%08X) lr=%08X",
                         n, r3, sz, base, sr::kernel::getCurrentGuestLR());
  return sz;
}

uint32_t stub_NtAllocateVirtualMemory(uint32_t r3, uint32_t r4, uint32_t r5,
                                      uint32_t r6, uint32_t r7, uint32_t r8) {
  sr::hitch::Scope hitchScope(sr::hitch::kVmAlloc);
  (void)r7;
  (void)r8;
  // Xbox 360 signature (4 params, no ProcessHandle/ZeroBits):
  //   r3 = PPVOID BaseAddress (in/out, guest pointer to VA variable)
  //   r4 = PSIZE_T RegionSize (in/out, guest pointer to size variable)
  //   r5 = ULONG  AllocationType (MEM_COMMIT etc.)
  //   r6 = ULONG  Protect        (PAGE_READWRITE etc.)
  uint32_t size = readGuestU32BE(r4);
  const uint32_t traceReqVA = s_arenaTrace ? readGuestU32BE(r3) : 0;
  static std::atomic<uint32_t> s_ntavmSeq{0};
  if (size == 0) {
    STUB_LOG("NtAllocateVirtualMemory", r3, r4, r5, r6, r7, r8, kStatusSuccess);
    return kStatusSuccess;
  }
  // iter-327 ROOT FIX (the scribble-family trunk): honor a non-null requested
  // BaseAddress. The DOGMA pool backing (sub_8278E018 -> sub_828A7778)
  // reserves its own VA range and asks for commit-in-place with page-aligned
  // base+size; we used to IGNORE the base, grant an unrelated heap chunk and
  // zero-fill it — an 11.5MB host memset sweeping live objects (FMUI worker
  // queue fields, the FE-SM cell 0x829761E0, the GameKernelMgr cells...) =
  // the iter-191..326 byte-strip/scribble family. Flat guest memory is
  // zero-backed and always writable, so commit-in-place = echo the request,
  // no fill (Windows contract: re-commit preserves contents).
  // SR_NTAVM_BASE=0 restores the legacy grant for A/B.
  static const bool s_honorBase = [] {
    const char* e = std::getenv("SR_NTAVM_BASE");
    return !e || e[0] != '0';
  }();
  uint32_t reqVA = readGuestU32BE(r3);
  if (s_honorBase && reqVA >= 0x10000u && reqVA < 0xC0000000u) {
    const uint32_t base = reqVA & ~0xFFFu;
    const uint32_t sz = (size + (reqVA - base) + 0xFFFu) & ~0xFFFu;
    // iter-364 PROPER ALLOCATOR COORDINATION: this range is committed at the guest's chosen base
    // WITHOUT reserving it from guestHeapAlloc's bump — so register it. guestHeapAlloc then skips
    // it (deterministic, [HEAP_RESV]) instead of bumping into it and zero-fill-clobbering the live
    // object (the D3DX effect-compiler +104 region overlap, and the same class as the VB/WMA walls).
    registerReservedRange(base, base + sz);
    recordNtavmGrant(base, sz);   // iter-395: MmQueryAllocationSize registry
    writeGuest32(r3, base);
    writeGuest32(r4, sz);
    SR_LOG_KERNEL_IMPORT(
        "NtAllocateVirtualMemory: commit-in-place VA=0x%08X size=0x%X (req 0x%08X/0x%X)",
        base, sz, reqVA, size);
    if (s_arenaTrace)
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[NTAVM] #%u reqVA=%08X size=%X type=%08X -> COMMIT-IN-PLACE base=%08X sz=%X (registered) lr=%08X",
        s_ntavmSeq.fetch_add(1) + 1, traceReqVA, size, r5, base, sz, getCurrentGuestLR());
    STUB_LOG("NtAllocateVirtualMemory", r3, r4, r5, r6, r7, r8, kStatusSuccess);
    return kStatusSuccess;
  }
  // iter-393 PLATFORM FIX (SR_NTAVM_ALIGN, default ON; =0 reverts): NT
  // CONFORMANCE for kernel-chosen allocations. The console kernel returns
  // 64KB-aligned bases and granularity-rounded sizes; we returned RAW bump
  // grants (bases like 0x8AC24270, size echoed unrounded). RtlHeap and the
  // DOGMA backing reserver (lr=828A779C) then do their own page/64KB
  // arithmetic on those values, so the guest's believed segment extents
  // DRIFT from what the bump actually granted — accumulated over the CRT
  // heap's twelve growth segments the guest's arena-top ran ~5MB past our
  // cursor's accounting and it handed out 0x8B20EEE0, which the bump ALSO
  // granted 5 preprocessor calls later; the grant's zero-fill killed the
  // live D3DX effect region's +8 counter (HWWATCH-caught) = the whole
  // 120-FXERR effects wall (5th two-allocator-overlap instance).
  static const bool s_alignFix = [] {
    const char* e = std::getenv("SR_NTAVM_ALIGN");
    return !e || e[0] != '0';
  }();
  // iter-394 PLATFORM FIX (SR_NTAVM_VMREGION, default ON; =0 reverts to the
  // bump): serve kernel-chosen allocations from the CONSOLE'S OWN VA REGION.
  // Golden dump: the console CRT process heap handle (dword_829AC16C) is
  // 0x40000000 — the Xbox kernel places virtual allocations in the dedicated
  // 0x40000000+ range, STRUCTURALLY DISJOINT from the 0x8xxxxxxx space where
  // the title's own allocators (pool backings, image, statics, our bump)
  // live. Serving base==0 NtAVM from the bump window interleaved kernel
  // grants with game arenas — the root soil of the entire two-allocator-
  // overlap family (5 confirmed instances: iter-361 VB/tex, iter-364 FX
  // region, iter-388 thread stacks, iter-390 catch, iter-393 FX on the real
  // heap). A dedicated 64KB-granular cursor at 0x40000000 (cap 0x7E000000,
  // below the EDRAM window at 0x7E800000) reproduces the console contract.
  // ⚠️ DEFAULT-OFF (iter-394, 2026-07-27): console-TRUE but 0/4 deep on both
  // integration attempts. Attempt 1: our own iter-192 grant sanitizer replaced
  // every legit 0x40xxxxxx pool grant (fixed — range taught). Attempt 2:
  // STILL 0/4 with a NEW deterministic shape (FE free-runs ~5300 ticks, the
  // whole load pipeline dead, sanitizer silent) — at least one more consumer
  // rejects 0x4xxxxxxx addresses (suspect: pool→GPU physical-address path /
  // MmGetPhysicalAddress identity mapping). Opt-in =1 for the next arc; the
  // proven default remains the aligned bump path below.
  static const bool s_vmRegion = [] {
    const char* e = std::getenv("SR_NTAVM_VMREGION");
    return e && e[0] == '1';
  }();
  static std::atomic<uint32_t> s_vmNext{0x40000000u};
  constexpr uint32_t kVmCap = 0x7E000000u;
  uint32_t grantSize = size;
  uint32_t va = 0;
  // iter-463 cmdbuf2 PLATFORM FIX (SR_NTAVM_XENIA_HEAP, default ON; =0 keeps the
  // legacy bump for the A/B): place kernel-chosen allocations where Xenia does - v00000000 for
  // 4 KB pages, v40000000 for X_MEM_LARGE_PAGES (0x20000000), bottom-up unless
  // X_MEM_TOP_DOWN (0x00100000), size rounded to 64 KB (xboxkrnl_memory.cc:111-183,
  // memory.cc LookupHeapByType). Golden sound_gameplay: the DOGMA pool and its EA
  // sound blocks live at 0x02xxxxxx-0x04xxxxxx; ours were at 0x87xxxxxx, and EA's
  // signed `% 16` header alignment (ParseTagStreamAndAlignData_sub_82330CA8) is a
  // no-op on negative addresses -> misparsed SNR header -> D-AUDIO-CMDBUF.
  if (sr::kernel::xheap::enabled()) {
    const bool large = (r5 & 0x20000000u) != 0;    // X_MEM_LARGE_PAGES
    const bool topDown = (r5 & 0x00100000u) != 0;  // X_MEM_TOP_DOWN
    uint32_t g = 0;
    const uint32_t x = sr::kernel::xheap::alloc(size, large, topDown, &g);
    uint8_t* host = x ? guestVAToHost(x) : nullptr;
    if (host && guestVAToHost(x + g - 1)) {
      if (!(r5 & 0x00800000u)) std::memset(host, 0, g);   // !X_MEM_NOZERO: fresh pages are zero
      recordNtavmGrant(x, g);   // MmQueryAllocationSize registry
      writeGuest32(r3, x);
      writeGuest32(r4, g);
      static std::atomic<uint32_t> s_xn{0};
      const uint32_t xn = s_xn.fetch_add(1, std::memory_order_relaxed) + 1;
      if (xn <= 64 || (xn & (xn - 1)) == 0)
        SR_LOG_KERNEL_IMPORT(
            "[NTAVM_XHEAP] #%u %s va=0x%08X size=0x%X (req 0x%X type=0x%08X) lr=%08X", xn,
            large ? "v40000000" : "v00000000", x, g, size, r5, getCurrentGuestLR());
      STUB_LOG("NtAllocateVirtualMemory", r3, r4, r5, r6, r7, r8, kStatusSuccess);
      return kStatusSuccess;
    }
    if (x) sr::kernel::xheap::release(x);
    SR_LOG_KERNEL_IMPORT("NtAllocateVirtualMemory FAILED: size=0x%X type=0x%08X (xenia heap full)",
                         size, r5);
    sr::kernel::noteGuestAllocFailure(sr::kernel::GuestAllocApi::NtAllocateVirtualMemory, size, r5, r6,
                                      "xenia heap full");
    STUB_LOG("NtAllocateVirtualMemory", r3, r4, r5, r6, r7, r8, kStatusNoMemory);
    return kStatusNoMemory;
  }
  if (s_vmRegion) {
    grantSize = (size + 0xFFFFu) & ~0xFFFFu;              // 64KB granularity
    uint32_t prevVm = s_vmNext.load(std::memory_order_relaxed);
    do {
      if (prevVm + grantSize > kVmCap || prevVm + grantSize < prevVm) { va = 0; break; }
      va = prevVm;
    } while (!s_vmNext.compare_exchange_weak(prevVm, prevVm + grantSize,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed));
    if (va) {
      uint8_t* host = guestVAToHost(va);
      if (host) std::memset(host, 0, grantSize);          // NT contract: fresh pages are zero
      else va = 0;                                        // range unmapped -> fall through
    }
  }
  // iter-401: reuse a released range before growing the bump. Recycled blocks
  // are already 64KB-aligned and 64KB-granular (that is how they were granted),
  // so the alignFix padding dance is unnecessary on this path.
  if (va == 0 && ntavmRecycleEnabled()) {
    const uint32_t want = (size + 0xFFFFu) & ~0xFFFFu;
    uint32_t reuse = ntavmFreeTake(want);
    if (reuse) {
      uint8_t* host = guestVAToHost(reuse);
      if (host) {
        std::memset(host, 0, want);   // NT contract: fresh pages are zero
        va = reuse;
        grantSize = want;
        static std::atomic<unsigned> s_reuseLog{0};
        unsigned rn = s_reuseLog.fetch_add(1, std::memory_order_relaxed) + 1;
        if (rn <= 16 || (rn % 256) == 0)
          SR_LOG_KERNEL_IMPORT(
              "[NTAVM_REUSE] #%u va=0x%08X size=0x%X (recycled %llu MB, reused %llu MB)",
              rn, va, grantSize,
              (unsigned long long)(g_ntavmRecycledBytes.load() >> 20),
              (unsigned long long)(g_ntavmReusedBytes.load() >> 20));
      } else {
        ntavmFreeInsert(reuse, want);  // unmapped — put it back, fall through
      }
    }
  }
  if (va == 0 && s_alignFix) {
    grantSize = (size + 0xFFFFu) & ~0xFFFFu;              // 64KB granularity
    uint32_t raw = guestHeapAlloc(static_cast<size_t>(grantSize) + 0x10000u);
    if (raw == 0) raw = guestHeapAllocFallback(static_cast<size_t>(grantSize) + 0x10000u);
    if (raw) va = (raw + 0xFFFFu) & ~0xFFFFu;             // aligned base inside the grant
  } else if (va == 0) {
    va = guestHeapAlloc(size);
    if (va == 0) va = guestHeapAllocFallback(size);
  }
  if (va == 0) {
    SR_LOG_KERNEL_IMPORT("NtAllocateVirtualMemory FAILED: size=0x%X (no memory)", size);
    sr::kernel::noteGuestAllocFailure(sr::kernel::GuestAllocApi::NtAllocateVirtualMemory, size, r5, r6, "no memory");
    if (s_arenaTrace)
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[NTAVM] #%u reqVA=%08X size=%X type=%08X -> NO_MEMORY lr=%08X",
        s_ntavmSeq.fetch_add(1) + 1, traceReqVA, size, r5, getCurrentGuestLR());
    STUB_LOG("NtAllocateVirtualMemory", r3, r4, r5, r6, r7, r8, kStatusNoMemory);
    return kStatusNoMemory;
  }
  if (s_arenaTrace)
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[NTAVM] #%u reqVA=%08X size=%X type=%08X -> HEAPALLOC va=%08X gsz=%X (g_heapNext advanced) lr=%08X",
      s_ntavmSeq.fetch_add(1) + 1, traceReqVA, size, r5, va, grantSize, getCurrentGuestLR());
  recordNtavmGrant(va, grantSize);   // iter-395: MmQueryAllocationSize registry
  // iter-401: exact, recycle-eligible extent. Reaching here means the kernel
  // chose the base (the commit-in-place path returns earlier), so every grant
  // recorded here is ours to reclaim on MEM_RELEASE.
  if (ntavmRecycleEnabled()) recordBumpGrant(va, grantSize);
  writeGuest32(r3, va);
  writeGuest32(r4, grantSize);
  SR_LOG_KERNEL_IMPORT("NtAllocateVirtualMemory: allocated VA=0x%08X size=0x%X (req 0x%X)", va, grantSize, size);
  STUB_LOG("NtAllocateVirtualMemory", r3, r4, r5, r6, r7, r8, kStatusSuccess);
  return kStatusSuccess;
}

// iter-401 PLATFORM FIX: NtFreeVirtualMemory actually frees.
// NTSTATUS NtFreeVirtualMemory(PVOID* BaseAddress, PSIZE_T RegionSize, ULONG FreeType, ...)
// r3/r4 are IN/OUT pointers into guest memory; r5 carries the free type.
uint32_t stub_NtFreeVirtualMemory(uint32_t r3, uint32_t r4, uint32_t r5,
                                  uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r6; (void)r7; (void)r8;
  constexpr uint32_t kMemDecommit = 0x4000u;
  constexpr uint32_t kMemRelease  = 0x8000u;

  uint32_t base = (guestVAToHost(r3) != nullptr) ? readGuestU32BE(r3) : 0u;
  uint32_t size = (guestVAToHost(r4) != nullptr) ? readGuestU32BE(r4) : 0u;
  uint32_t type = r5;
  // Some callers pass the values directly rather than through pointers.
  if (!base && r3 >= 0x10000u) base = r3;
  if (!size && r4 >= 0x10000u && r4 < 0x10000000u) size = r4;

  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;

  uint32_t grantBase = 0;
  const uint32_t grantSize = base ? lookupNtavmGrant(base, &grantBase) : 0u;

  // iter-463 SR_NTAVM_XENIA_HEAP: MEM_RELEASE of a region the Xenia heaps granted
  // frees it (Xenia BaseHeap::Release); MEM_DECOMMIT keeps the reservation.
  if (base && (type & kMemRelease) && sr::kernel::xheap::enabled()) {
    const uint32_t xs = sr::kernel::xheap::release(base);
    if (xs) {
      eraseNtavmGrant(base);
      if (n <= 24 || (n % 256) == 0)
        SR_LOG_KERNEL_IMPORT("[NTAVM_FREE] #%u RELEASE va=0x%08X size=0x%X (xenia heap)", n, base, xs);
      STUB_LOG("NtFreeVirtualMemory", r3, r4, r5, r6, r7, r8, kStatusSuccess);
      return kStatusSuccess;
    }
  }

  if (ntavmRecycleEnabled() && base) {
    // Release: the reservation is gone -> recycle, but ONLY an exact bump grant
    // (see recordBumpGrant). Do not zero here: a stale reader then reads stale
    // data instead of faulting, and the reuse path zeroes before handing the
    // range out, which is where the NT contract actually applies.
    const uint32_t exact = (type & kMemRelease) ? takeBumpGrant(base) : 0u;
    if (exact) {
      ntavmFreeInsert(base, exact);
      if (n <= 24 || (n % 256) == 0)
        SR_LOG_KERNEL_IMPORT(
            "[NTAVM_FREE] #%u RELEASE va=0x%08X size=0x%X (recycled %llu MB total)",
            n, base, exact,
            (unsigned long long)(g_ntavmRecycledBytes.load() >> 20));
    } else if (type & kMemRelease) {
      if (n <= 24 || (n % 256) == 0)
        SR_LOG_KERNEL_IMPORT(
            "[NTAVM_FREE] #%u RELEASE va=0x%08X not-a-bump-grant (left alone; record=0x%X@%08X)",
            n, base, grantSize, grantBase);
    } else if (type & kMemDecommit) {
      // Decommit keeps the reservation: the guest may re-commit in place, so
      // the range is never handed to anyone else and is left untouched.
      if (n <= 24 || (n % 256) == 0)
        SR_LOG_KERNEL_IMPORT("[NTAVM_FREE] #%u DECOMMIT va=0x%08X size=0x%X (kept reserved)",
                             n, base, size ? size : grantSize);
    } else if (n <= 24) {
      SR_LOG_KERNEL_IMPORT("[NTAVM_FREE] #%u UNHANDLED va=0x%08X size=0x%X type=0x%X grant=0x%X@%08X",
                           n, base, size, type, grantSize, grantBase);
    }
  }
  STUB_LOG("NtFreeVirtualMemory", r3, r4, r5, r6, r7, r8, kStatusSuccess);
  return kStatusSuccess;
}

uint32_t stub_KeQueryPerformanceCounter(uint32_t r3, uint32_t r4, uint32_t r5,
                                        uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4;
  (void)r5;
  (void)r6;
  (void)r7;
  (void)r8;
  // iter-463 SR_TIMEBASE_50MHZ: the SAME 50 MHz domain as mftb and
  // KeQueryPerformanceFrequency (legacy: steady_clock/100 = 10 MHz units against a
  // 50 MHz frequency, own epoch). Latent either way: the XEX does not import it
  // (only the kernel/harness.cpp probe calls it; Xenia does not export it).
  uint64_t ticks = sr::timebase::enabled() ? sr_guest_mftb() : perfCounterTicks();
  writeGuest64(r3, ticks);
  STUB_LOG("KeQueryPerformanceCounter", r3, r4, r5, r6, r7, r8, kStatusSuccess);
  return kStatusSuccess;
}

uint32_t stub_KeQueryPerformanceFrequency(uint32_t r3, uint32_t r4, uint32_t r5,
                                           uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  // Xbox 360 KeQueryPerformanceFrequency: returns the Xenon timebase frequency
  // (50 MHz) in r3 and takes no arguments (Xenia xboxkrnl_threading.cc returns it,
  // nothing else). iter-463: the old "if r3 looks like a guest pointer, write 50 MHz
  // there" branch is gone - r3 on entry is whatever the caller last had in it, so
  // that wrote 8 bytes into guest image memory the game never asked us to touch.
  constexpr uint32_t kFrequency = static_cast<uint32_t>(sr::timebase::kGuestHz);
  STUB_LOG("KeQueryPerformanceFrequency", r3, r4, r5, r6, r7, r8, kFrequency);
  return kFrequency;
}

// ===== iter-399: guest-truth embedded-KEVENT semantics ======================
// The movie pipeline (MoviePipeline_ShutdownWorkerThreads_82480888 /
// StreamWorkerLoop_824819E8) inline-initializes eight KEVENTs embedded at
// pipeline+0x54..0xC4 (direct guest stores: type byte, SignalState, self-linked
// wait list — no KeInitializeEvent import call) and drives them via
// KeSetEvent/KeWaitForSingleObject thunks (0x8247FB90../0x8247FAD0..). The old
// host-event mirror never read the guest SignalState: a pre-signaled event was
// auto-created NON-signaled at first wait = lost wake = the movie-2 boundary
// wedge (iter-398f→h). Fix: for title-VA event objects the guest SignalState
// word (va+4) is the source of truth — waits observe/consume it, set/reset
// write it; the host event is only a wake token. Kernel-range VAs (0,
// 0xDxxxxxxx GPU fence slots), Nt-handle IDs, deref-resolved
// pointer-to-handle objects, and semaphores (header type != 0/1; DOGMA waits
// ride the same map) keep the legacy host-only path — the GPU IRQ thread and
// VdSwap signal those handles directly host-side.
#ifdef _WIN32
struct KeTruthInfo {
  bool eligible;
  uint8_t type;   // 0 = NotificationEvent (manual), 1 = SynchronizationEvent (auto)
  uint8_t* hdr;   // host pointer to the guest DISPATCHER_HEADER
};

// SR_KEVT_GUEST_TRUTH=0 restores the legacy host-event-mirror semantics (A/B).
static bool keGuestTruthEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_KEVT_GUEST_TRUTH");
    return !e || e[0] != '0';
  }();
  return s_on;
}

// Eligibility is a STRUCTURAL type-marker test, not a range test (project rule:
// ASCII/heap garbage passes ranges). The decisive marker is the self-linked
// WaitListHead an initialized KEVENT always carries: Flink == Blink == va+8.
// Nothing on our port ever inserts into a guest wait list, so it stays self-
// linked for the object's whole life. A "pointer holding an Nt handle ID"
// object (served by syncHandleLookup's deref fallback) cannot match it.
//
// An earlier version of this probe disqualified via that deref fallback
// instead; it silently excluded the very objects it was written for, because
// a pre-signaled event's first dword is 0 and VA 0 is a registered GPU-fence
// handle. Structural marker first — that class of miss is why the rule exists.
static KeTruthInfo keGuestTruthProbe(uint32_t va) {
  KeTruthInfo out{false, 0, nullptr};
  if (!keGuestTruthEnabled()) return out;
  if (va < 0x10000u || va >= 0xA0000000u) return out;  // null page / kernel+phys ranges
  if ((va & 3u) != 0) return out;                      // SignalState CAS needs alignment
  uint8_t* hdr = guestVAToHost(va);
  if (!hdr) return out;
  uint8_t type = hdr[0];
  if (type > 1) return out;                    // events only (5=semaphore, 6=thread)
  // NB: bytes +1..+3 (Absolute/Size/Inserted) are NOT written by the inline
  // guest init — only the type byte is — so they can be heap garbage. Do not
  // test them.
  if (eventHandleGet(va)) return out;          // r3 is actually an Nt handle ID
  uint32_t state = readGuestU32BE(va + 4);
  if (state > 1) return out;                   // KEVENT SignalState is boolean
  if (readGuestU32BE(va + 8) != va + 8) return out;   // WaitListHead.Flink self-link
  if (readGuestU32BE(va + 12) != va + 8) return out;  // WaitListHead.Blink self-link
  out.eligible = true;
  out.type = type;
  out.hdr = hdr;
  return out;
}

// iter-463 SR_KEVT_SET_ATOMIC (audio arc; default ON since the iter-463 rolls, =0 keeps the A/B). KeSetEvent on a guest-truth
// event writes SignalState = 1 and THEN sets the host wake token. A waiter whose fast path runs
// between the two consumes the state and drains a token that is not set yet; the setter's
// SetEvent then leaves a stale token, and the next wait on that auto-reset event wakes with no
// signal (difftest --ke-waitmulti-test W13: 10 spurious worker wakes in 200000 kicks). Live, the
// XAudio pump <-> worker handshake (ServiceSync 0x82117F10 / AudioWorkerThreadMain 0x82117810 on
// 0x829AC0B8 / 0x829AC094) deadlocked with both events at 0 (archive/v1_audio/rolls/a_all_r2).
// On the console KeSetEvent is atomic under the dispatcher lock; here the setter's state write
// + token set and a waiter's consume + drain are made atomic with respect to each other.
static std::mutex g_kevtSetMu;
static bool kevtSetAtomicOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_KEVT_SET_ATOMIC");   // iter-463 rolls: default ON (=0 keeps the A/B)
    return !e || e[0] != '0';
  }();
  return v;
}

// Atomically consume one auto-reset signal (guest SignalState nonzero -> 0).
// Raw-word CAS is endian-agnostic: we only need the 0 <-> nonzero transition.
static bool keGuestEventTryConsume(uint8_t* hdr) {
  volatile LONG* p = reinterpret_cast<volatile LONG*>(hdr + 4);
  LONG cur = *p;
  while (cur != 0) {
    LONG prev = InterlockedCompareExchange(p, 0, cur);
    if (prev == cur) return true;
    cur = prev;
  }
  return false;
}

// Faithful KeInitializeEvent/KeInitializeSemaphore guest header write, so the
// type byte is authoritative for keGuestTruthProbe even when the only init the
// object ever saw was our stub (the old stubs left guest memory as heap
// garbage, which would misclassify).
// SR_KEVT_INIT_HDR=0 restores the legacy "host handle only, guest memory
// untouched" init (A/B knob, separate from the wait/set guest-truth path).
static bool keInitHeaderEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_KEVT_INIT_HDR");
    return !e || e[0] != '0';
  }();
  return s_on;
}

static void keWriteGuestDispatcherHeader(uint32_t va, uint8_t type, uint32_t signalState) {
  if (!keInitHeaderEnabled()) return;
  if (va < 0x10000u || va >= 0xA0000000u) return;
  uint8_t* hdr = guestVAToHost(va);
  if (!hdr) return;
  hdr[0] = type;  // DISPATCHER_HEADER.Type
  hdr[1] = 0;     // Absolute
  hdr[2] = 4;     // Size (dwords)
  hdr[3] = 0;     // Inserted
  writeGuest32(va + 4, signalState);
  writeGuest32(va + 8, va + 8);   // WaitListHead.Flink (self-linked)
  writeGuest32(va + 12, va + 8);  // WaitListHead.Blink
}

// [KEVT] per-VA log budget: first 16 per VA + every 512th (movie events stay
// fully visible under per-frame VD-event spam).
static int kevtLogBudget(uint32_t va) {
  static std::mutex s_m;
  static std::unordered_map<uint32_t, int> s_counts;
  std::lock_guard<std::mutex> lock(s_m);
  int n = ++s_counts[va];
  return (n <= 16 || (n % 512) == 0) ? n : 0;
}
#endif  // _WIN32

uint32_t stub_KeInitializeEvent(uint32_t r3, uint32_t r4, uint32_t r5,
                                uint32_t r6, uint32_t r7, uint32_t r8) {
  // r3 = PKEVENT (guest VA of the kernel event object)
  // r4 = Type (0 = NotificationEvent/manual-reset, 1 = SynchronizationEvent/auto-reset)
  // r5 = State (TRUE = signaled, FALSE = not-signaled)
#ifdef _WIN32
  BOOL manualReset = (r4 == 0) ? TRUE : FALSE;
  BOOL initialState = (r5 != 0) ? TRUE : FALSE;
  HANDLE h = CreateEventA(nullptr, manualReset, initialState, nullptr);
  if (h) keSyncHandleSet(r3, h);
  keWriteGuestDispatcherHeader(r3, (r4 == 0) ? 0u : 1u, (r5 != 0) ? 1u : 0u);
  static int s_logCount = 0;
  if (++s_logCount <= 20)
    SR_LOG_KERNEL_IMPORT("KeInitializeEvent(obj=0x%08X type=%u state=%u) -> host=%p",
                         r3, r4, r5, (void*)h);
#endif
  return kStatusSuccess;
}

// iter-346: VD interrupt-pump thread context (captured at ExCreateThread;
// its wait event lives at ctx+8). VdSwap signals it = swap-complete interrupt.
static std::atomic<uint32_t> g_vdPumpCtxVA{0};

uint32_t stub_KeSetEvent(uint32_t r3, uint32_t r4, uint32_t r5,
                         uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
#ifdef _WIN32
  // iter-399 guest-truth: latch the guest SignalState BEFORE waking the host
  // event, so a concurrent waiter's reset-recheck always observes it.
  KeTruthInfo ti = keGuestTruthProbe(r3);
  uint32_t prevState = 0;
  // iter-463 SR_KEVT_SET_ATOMIC: state write + token set as one step w.r.t. a waiter's consume.
  std::unique_lock<std::mutex> setLk(g_kevtSetMu, std::defer_lock);
  if (ti.eligible && kevtSetAtomicOn()) setLk.lock();
  if (ti.eligible) {
    prevState = readGuestU32BE(r3 + 4);
    writeGuest32(r3 + 4, 1u);
  }
  HANDLE h = syncHandleLookup(r3);
  // Auto-create if not found (matching KeWaitForSingleObject auto-creation)
  if (!h && guestVAToHost(r3)) {
    // Honor the guest type byte when known (manual vs auto host event).
    BOOL manualReset = (ti.eligible && ti.type == 0) ? TRUE : FALSE;
    h = CreateEventA(nullptr, manualReset, FALSE, nullptr);
    if (h) {
      keSyncHandleSet(r3, h);
      static int s_autoLog = 0;
      if (++s_autoLog <= 10)
        SR_LOG_KERNEL_IMPORT("KeSetEvent auto-created event for obj=0x%08X host=%p", r3, (void*)h);
    }
  }
  if (h) SetEvent(h);
  if (setLk.owns_lock()) setLk.unlock();
  if (ti.eligible) {
    int n = kevtLogBudget(r3);
    if (n)
      // iter-428f: lr attribution — on stuck rolls three movie worker events
      // (0x83A7D294/D2B4/D2C4-class) never get set; the good-roll setter's lr
      // names the producer whose startup loses the race.
      SR_LOG_KERNEL_IMPORT("[KEVT] set va=0x%08X type=%u prev=%u tid=%lu lr=%08X (#%d)",
                           r3, ti.type, prevState, GetCurrentThreadId(),
                           sr::kernel::getCurrentGuestLR(), n);
    return prevState;  // NT contract: KeSetEvent returns the previous state
  }
#endif
  return kStatusSuccess;
}

uint32_t stub_KeResetEvent(uint32_t r3, uint32_t r4, uint32_t r5,
                           uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
#ifdef _WIN32
  // iter-399 guest-truth: clear the guest SignalState too.
  KeTruthInfo ti = keGuestTruthProbe(r3);
  uint32_t prevState = 0;
  if (ti.eligible) {
    prevState = readGuestU32BE(r3 + 4);
    writeGuest32(r3 + 4, 0u);
  }
  HANDLE h = syncHandleLookup(r3);
  if (h) ResetEvent(h);
  if (ti.eligible) {
    int n = kevtLogBudget(r3);
    if (n)
      SR_LOG_KERNEL_IMPORT("[KEVT] reset va=0x%08X type=%u prev=%u tid=%lu (#%d)",
                           r3, ti.type, prevState, GetCurrentThreadId(), n);
    return prevState;
  }
#endif
  STUB_LOG("KeResetEvent", r3, r4, r5, r6, r7, r8, kStatusSuccess);
  return kStatusSuccess;
}

uint32_t stub_KeWaitForSingleObject(uint32_t r3, uint32_t r4, uint32_t r5,
                                    uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r8;
  // r3 = object pointer (guest VA of KEVENT/KSEMAPHORE, or Nt-level handle)
  // r7 = timeout pointer (0 = infinite, nonzero = pointer to LARGE_INTEGER timeout)

  constexpr uint32_t kStatusTimeout = 258u; // STATUS_TIMEOUT (0x102)

#ifdef _WIN32
  // iter-399 guest-truth branch: title-VA event objects wait on the guest
  // SignalState word itself (fixes the pre-signaled-embedded-KEVENT lost wake
  // = the movie-2 boundary wedge). The host event is only a wake token; a
  // host token with guest state still 0 is honored as a legacy signal (GPU
  // IRQ / VdSwap / panic paths SetEvent handles directly, host-side only).
  {
    KeTruthInfo ti = keGuestTruthProbe(r3);
    if (ti.eligible) {
      HANDLE hTok = keSyncHandleGet(r3);
      if (!hTok) {
        hTok = CreateEventA(nullptr, FALSE, FALSE, nullptr);  // pure wake token
        if (hTok) keSyncHandleSet(r3, hTok);
        int nc = kevtLogBudget(r3);
        if (nc)
          SR_LOG_KERNEL_IMPORT("[KEVT] wait auto-create va=0x%08X type=%u state=%u tid=%lu",
                               r3, ti.type, readGuestU32BE(r3 + 4), GetCurrentThreadId());
      }
      // Timeout parse (same convention as legacy path below).
      bool infinite = (r7 == 0);
      DWORD ms = INFINITE;
      if (!infinite) {
        ms = guestTimeoutToMs(r7);  // iter-463 (a): full timeout, SR_WAIT_FULL_TIMEOUT
      }
      {
        int n = kevtLogBudget(r3);
        if (n)
          SR_LOG_KERNEL_IMPORT("[KEVT] wait-enter va=0x%08X type=%u state=%u inf=%d ms=%lu tid=%lu (#%d)",
                               r3, ti.type, readGuestU32BE(r3 + 4), infinite ? 1 : 0,
                               infinite ? 0ul : (unsigned long)ms, GetCurrentThreadId(), n);
      }
      ULONGLONG deadline = infinite ? 0 : (GetTickCount64() + ms);
      int slices = 0;
      for (;;) {
        // 1) Guest SignalState is authoritative: pre-signals and direct guest
        //    stores (inline init/reset, pipeline reuse) are visible here.
        if (readGuestU32BE(r3 + 4) != 0) {
          bool consumed = true;
          // iter-463 SR_KEVT_SET_ATOMIC: consume + drain cannot fall between a setter's state
          // write and its token set (that left a stale token = a later spurious wake).
          std::unique_lock<std::mutex> setLk(g_kevtSetMu, std::defer_lock);
          if (ti.type == 1 && kevtSetAtomicOn()) setLk.lock();
          if (ti.type == 1) consumed = keGuestEventTryConsume(ti.hdr);
          if (consumed) {
            // Drain the matching host token so a later waiter can't wake on a
            // signal this wait already consumed (auto-reset events only).
            if (ti.type == 1) {
              HANDLE hDrain = keSyncHandleGet(r3);
              if (hDrain) WaitForSingleObject(hDrain, 0);
            }
            if (setLk.owns_lock()) setLk.unlock();
            int n = kevtLogBudget(r3);
            if (n)
              SR_LOG_KERNEL_IMPORT("[KEVT] wait-ok va=0x%08X via=guest-state tid=%lu (#%d)",
                                   r3, GetCurrentThreadId(), n);
            return kStatusSuccess;
          }
          continue;  // lost the consume race — re-read state
        }
        DWORD slice = 200;
        if (!infinite) {
          ULONGLONG now = GetTickCount64();
          if (now >= deadline) {
            int n = kevtLogBudget(r3);
            if (n)
              SR_LOG_KERNEL_IMPORT("[KEVT] wait-timeout va=0x%08X tid=%lu (#%d)",
                                   r3, GetCurrentThreadId(), n);
            return kStatusTimeout;
          }
          ULONGLONG left = deadline - now;
          if (left < slice) slice = static_cast<DWORD>(left);
        }
        HANDLE hNow = keSyncHandleGet(r3);  // re-fetch: self-heals create races
        if (!hNow) hNow = hTok;
        DWORD wr = hNow ? WaitForSingleObject(hNow, slice) : WAIT_TIMEOUT;
        if (wr == WAIT_OBJECT_0) {
          // Host wake token. If a guest-truth setter fired, the state is
          // already latched — consume it. If not (legacy host-side signaler),
          // honor the token directly rather than swallowing it.
          if (ti.type == 1) keGuestEventTryConsume(ti.hdr);
          int n = kevtLogBudget(r3);
          if (n)
            SR_LOG_KERNEL_IMPORT("[KEVT] wait-ok va=0x%08X via=host-token tid=%lu (#%d)",
                                 r3, GetCurrentThreadId(), n);
          return kStatusSuccess;
        }
        if (wr == WAIT_FAILED && !infinite) return kStatusTimeout;
        if (infinite && (++slices % 150) == 0)
          SR_LOG_KERNEL_IMPORT("[KEVT] wait-stuck va=0x%08X type=%u state=%u tid=%lu slices=%d",
                               r3, ti.type, readGuestU32BE(r3 + 4), GetCurrentThreadId(), slices);
      }
    }
  }
  HANDLE h = syncHandleLookup(r3);
  if (h) {
    DWORD ms = INFINITE;
    if (r7 != 0) {
      // Guest timeout (100ns units, negative = relative). iter-463 (a): no
      // 5000 ms cap unless SR_WAIT_FULL_TIMEOUT=0.
      ms = guestTimeoutToMs(r7);
    } else {
      // r7 == 0 → INFINITE wait. Same condvar issue as NtWaitForSingleObjectEx:
      // returning STATUS_TIMEOUT corrupts condvar spurious wakeup counters.
      static std::atomic<int> s_keInfWaitLog{0};
      int n = ++s_keInfWaitLog;
      if (n <= 20 || (n % 500 == 0))
        SR_LOG_KERNEL_IMPORT("[KeWait] INFINITE wait on obj=0x%08X host=%p (blocking, call#%d)",
                             r3, (void*)h, n);
      for (;;) {
        DWORD result = WaitForSingleObject(h, 200);
        if (result == WAIT_OBJECT_0) {
          static int s_sigCount = 0;
          if (++s_sigCount <= 50 || s_sigCount % 500 == 0)
            SR_LOG_KERNEL_IMPORT("[KeWait] INFINITE obj=0x%08X signaled (call#%d)", r3, s_sigCount);
          return kStatusSuccess;
        }
        if (result == WAIT_FAILED) {
          return kStatusTimeout;
        }
      }
    }
    DWORD result = WaitForSingleObject(h, ms);
    if (result == WAIT_TIMEOUT) {
      static int s_timeoutCount = 0;
      if (++s_timeoutCount <= 50)
        STUB_LOG("KeWaitForSingleObject(timeout)", r3, r4, r5, r6, r7, r8, kStatusTimeout);
      return kStatusTimeout;
    }
    static int s_signaledCount = 0;
    if (++s_signaledCount <= 50)
      STUB_LOG("KeWaitForSingleObject(signaled)", r3, r4, r5, r6, r7, r8, kStatusSuccess);
    return kStatusSuccess;
  }
  // No handle found — try auto-creating from DISPATCHER_HEADER at guest VA.
  if (!h && guestVAToHost(r3)) {
    uint8_t* hdr = guestVAToHost(r3);
    uint8_t type = hdr[0];  // DISPATCHER_HEADER.Type (byte 0, PPC big-endian = raw byte order)
    // Types: 0=NotificationEvent, 1=SynchronizationEvent, 5=Semaphore
    if (type <= 1) {
      BOOL manualReset = (type == 0) ? TRUE : FALSE;
      h = CreateEventA(nullptr, manualReset, FALSE, nullptr);
      if (h) {
        keSyncHandleSet(r3, h);
        static int s_autoLogE = 0;
        if (++s_autoLogE <= 10)
          SR_LOG_KERNEL_IMPORT("KeWaitForSingleObject auto-created EVENT for obj=0x%08X type=%u host=%p",
                               r3, type, (void*)h);
      }
    } else if (type == 5) {
      h = CreateSemaphoreA(nullptr, 0, 1024, nullptr);
      if (h) {
        keSyncHandleSet(r3, h);
        static int s_autoLogS = 0;
        if (++s_autoLogS <= 10)
          SR_LOG_KERNEL_IMPORT("KeWaitForSingleObject auto-created SEMAPHORE for obj=0x%08X host=%p",
                               r3, (void*)h);
      }
    } else {
      // Unknown type — create a generic auto-reset event as fallback
      h = CreateEventA(nullptr, FALSE, FALSE, nullptr);
      if (h) {
        keSyncHandleSet(r3, h);
        static int s_autoLogU = 0;
        if (++s_autoLogU <= 10)
          SR_LOG_KERNEL_IMPORT("KeWaitForSingleObject auto-created UNKNOWN-type(%u) event for obj=0x%08X host=%p",
                               type, r3, (void*)h);
      }
    }
  }
  if (h) {
    DWORD ms = INFINITE;
    if (r7 != 0) {
      ms = guestTimeoutToMs(r7);  // iter-463 (a)
    }
    DWORD result = WaitForSingleObject(h, ms);
    if (result == WAIT_TIMEOUT) {
      static int s_autoTimeoutCount = 0;
      if (++s_autoTimeoutCount <= 50)
        STUB_LOG("KeWaitForSingleObject(auto-timeout)", r3, r4, r5, r6, r7, r8, kStatusTimeout);
      return kStatusTimeout;
    }
    static int s_autoSigCount = 0;
    if (++s_autoSigCount <= 50)
      STUB_LOG("KeWaitForSingleObject(auto-signaled)", r3, r4, r5, r6, r7, r8, kStatusSuccess);
    return kStatusSuccess;
  }
  // Truly no handle, no guest VA — use existing fallback
  if (r7 != 0) {
    Sleep(16);
    static int s_noHandleTimeoutCount = 0;
    if (++s_noHandleTimeoutCount <= 5)
      STUB_LOG("KeWaitForSingleObject(no-handle,timeout)", r3, r4, r5, r6, r7, r8, kStatusTimeout);
    return kStatusTimeout;
  }
  static int s_noHandleInfCount = 0;
  if (++s_noHandleInfCount <= 5)
    STUB_LOG("KeWaitForSingleObject(no-handle,inf)", r3, r4, r5, r6, r7, r8, kStatusSuccess);
#endif
  return kStatusSuccess;
}

#ifdef _WIN32
// iter-463 SR_KE_WAITMULTI (default ON since the iter-463 rolls; =0 = the generic stub,
// which returned 0 = "object 0 signalled" at once without waiting or consuming anything).
// Xenia KeWaitForMultipleObjects_entry (xboxkrnl_threading.cc:1057-1088) + XObject::WaitMultiple
// (xobject.cc:265-340): WaitAny (type 1) returns STATUS_WAIT_0 + the index of the object that
// satisfied the wait (lowest index when several are signalled) and consumes only that one;
// WaitAll (type 0) returns 0 once every object is signalled together and consumes all of them;
// STATUS_TIMEOUT 0x102; an unresolvable object = STATUS_INVALID_PARAMETER.
// Args: r3 count, r4 PVOID Object[], r5 wait type, r6 reason, r7 mode, r8 alertable,
//       r9 PLARGE_INTEGER timeout (0 = infinite), r10 wait-block array (unused on the host).
// Composed over the SAME object model the single wait uses (so a signal can never be seen by
// one and missed by the other):
//   * title-VA KEVENTs (keGuestTruthProbe): guest SignalState is the truth, the host event is a
//     wake token; auto-reset events are consumed by CAS, then their token drained;
//   * KTHREADs in the harness arena (type 6): guest signal_state (+4), written at exit (polled);
//   * everything else (semaphores, Nt handles, GPU fence slots): the host handle, auto-created
//     from the dispatcher type byte exactly as KeWaitForSingleObject does.
// Not modelled (same as the single wait): alertable APC delivery (X_STATUS_USER_APC).
// The only caller today: XAudioServiceSync_sub_82117F10 @0x82117FC0 - WaitAny, infinite, on the
// XAudio worker's done / exit KEVENTs 0x829AC094 / 0x829AC0D8 (only when engine+304 = a worker
// exists, i.e. once SR_PROCTYPE makes KeGetCurrentProcessType return TITLE).
static bool keWaitMultiOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_KE_WAITMULTI");   // iter-463 rolls: default ON (=0 keeps the A/B)
    return !e || e[0] != '0';
  }();
  return v;
}

namespace {
struct KeWmObj {
  uint32_t va = 0;
  int kind = 0;          // 0 guest-truth event, 1 KTHREAD (guest signal_state), 2 host handle
  uint8_t type = 0;      // truth: 0 notification, 1 synchronization
  uint8_t* hdr = nullptr;
  HANDLE h = nullptr;    // truth: wake token; host: the object
  bool latched = false;  // WaitAll: a host-token wake that carried no guest state
};
std::atomic<uint32_t> g_keWmLogs{0}, g_keWmStuckLogs{0};
}  // namespace

// Same auto-create rule as KeWaitForSingleObject's "no handle" branch.
static HANDLE keWmHostHandle(uint32_t va) {
  HANDLE h = syncHandleLookup(va);
  if (h || !guestVAToHost(va)) return h;
  const uint8_t type = guestVAToHost(va)[0];
  if (type <= 1) h = CreateEventA(nullptr, type == 0 ? TRUE : FALSE, FALSE, nullptr);
  else if (type == 5) h = CreateSemaphoreA(nullptr, 0, 1024, nullptr);
  else h = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (h) keSyncHandleSet(va, h);
  return h;
}

// Is guest-side object o signalled right now? Never consumes.
static bool keWmGuestSignalled(const KeWmObj& o) {
  if (o.kind == 0) return o.latched || readGuestU32BE(o.va + 4) != 0;
  if (o.kind == 1) return readGuestU32BE(o.va + 4) != 0;
  return false;
}

// Consume a truth event after its wait was satisfied (auto-reset only).
static bool keWmConsumeTruth(KeWmObj& o) {
  if (o.kind != 0 || o.type != 1) return true;
  if (o.latched) { o.latched = false; return true; }
  // iter-463 SR_KEVT_SET_ATOMIC: same consume + drain atomicity as the single wait.
  std::unique_lock<std::mutex> setLk(g_kevtSetMu, std::defer_lock);
  if (kevtSetAtomicOn()) setLk.lock();
  if (!keGuestEventTryConsume(o.hdr)) return false;
  if (HANDLE t = keSyncHandleGet(o.va)) WaitForSingleObject(t, 0);   // drain the token
  return true;
}

// Undo a host acquisition (WaitAll compensation when a guest CAS lost a race).
static void keWmUndoHost(HANDLE h) {
  LONG prev = 0;
  if (!ReleaseSemaphore(h, 1, &prev)) SetEvent(h);
}

uint32_t stub_KeWaitForMultipleObjects(uint32_t r3, uint32_t r4, uint32_t r5,
                                       uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r6; (void)r7;
  constexpr uint32_t kTimeout = 0x102u, kUnsuccessful = 0xC0000001u;
  const uint32_t count = r3, arrVA = r4;
  const bool waitAny = r5 != 0;
  const uint32_t timeoutVA = ExportRegistry::getCurrentArgR9();
  const uint32_t lr = sr::kernel::getCurrentGuestLR();
  const uint32_t logN = g_keWmLogs.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool logThis = logN <= 20 || (logN & (logN - 1)) == 0;
  if (count == 0) return kUnsuccessful;           // host WaitAny/WaitAll(0) fails -> kFailed
  if (count > 64 || !guestVAToHost(arrVA) || !guestVAToHost(arrVA + count * 4 - 1))
    return kStatusInvalidParameter;
  KeWmObj objs[64];
  bool anyPollOnly = false;
  for (uint32_t i = 0; i < count; ++i) {
    KeWmObj& o = objs[i];
    o.va = readGuestU32BE(arrVA + i * 4);
    KeTruthInfo ti = keGuestTruthProbe(o.va);
    if (ti.eligible) {
      o.kind = 0;
      o.type = ti.type;
      o.hdr = ti.hdr;
      o.h = keSyncHandleGet(o.va);
      if (!o.h) {
        o.h = CreateEventA(nullptr, FALSE, FALSE, nullptr);   // pure wake token (single wait)
        if (o.h) keSyncHandleSet(o.va, o.h);
      }
      continue;
    }
    const uint8_t* hp = guestVAToHost(o.va);
    // iter-465 physmem: any KTHREAD block we built (an arena-overflow block too).
    if (hp && hp[0] == kDispatcherThread && kthreadIsBlockVA(o.va)) {
      o.kind = 1;
      anyPollOnly = true;
      continue;
    }
    o.kind = 2;
    o.h = keWmHostHandle(o.va);
    if (!o.h) {
      if (logThis)
        SR_LOG_KERNEL_IMPORT("[KE_WAITMULTI] obj[%u]=0x%08X unresolvable -> INVALID_PARAMETER lr=%08X",
                             i, o.va, lr);
      return kStatusInvalidParameter;
    }
  }
  const bool infinite = timeoutVA == 0;
  const DWORD ms = infinite ? INFINITE : guestTimeoutToMs(timeoutVA);
  const ULONGLONG deadline = infinite ? 0 : GetTickCount64() + ms;
  const DWORD sliceMax = anyPollOnly ? 20 : 200;   // guest-only signals are polled
  if (logThis)
    SR_LOG_KERNEL_IMPORT("[KE_WAITMULTI] #%u enter n=%u %s obj0=%08X obj1=%08X inf=%d ms=%lu alertable=%u lr=%08X tid=%lu",
                         logN, count, waitAny ? "ANY" : "ALL", objs[0].va, count > 1 ? objs[1].va : 0u,
                         infinite ? 1 : 0, infinite ? 0ul : (unsigned long)ms, r8, lr, GetCurrentThreadId());
  auto done = [&](uint32_t st, const char* via) {
    if (logThis)
      SR_LOG_KERNEL_IMPORT("[KE_WAITMULTI] #%u -> 0x%X via=%s lr=%08X", logN, st, via, lr);
    return st;
  };
  int slices = 0;
  HANDLE hs[64];
  uint32_t hmap[64];
  for (;;) {
    DWORD slice = sliceMax;
    if (!infinite) {
      const ULONGLONG now = GetTickCount64();
      const ULONGLONG left = now >= deadline ? 0 : deadline - now;
      if (left < slice) slice = DWORD(left);
    }
    if (waitAny) {
      // 1) Already signalled, lowest index first.
      for (uint32_t i = 0; i < count; ++i) {
        KeWmObj& o = objs[i];
        if (o.kind == 2) {
          if (WaitForSingleObject(o.h, 0) == WAIT_OBJECT_0) return done(i, "host");
        } else if (keWmGuestSignalled(o)) {
          if (keWmConsumeTruth(o)) return done(i, "guest-state");
        }
      }
      if (!infinite && slice == 0) return done(kTimeout, "timeout");
      // 2) Block on every wake token / host object.
      DWORD n = 0;
      for (uint32_t i = 0; i < count; ++i)
        if (objs[i].h) { hs[n] = objs[i].h; hmap[n] = i; ++n; }
      DWORD wr = WAIT_TIMEOUT;
      if (n) wr = WaitForMultipleObjects(n, hs, FALSE, slice);
      else Sleep(slice);
      if (wr < WAIT_OBJECT_0 + n) {
        const uint32_t i = hmap[wr - WAIT_OBJECT_0];
        KeWmObj& o = objs[i];
        if (o.kind == 2) return done(i, "host-wait");
        // Truth token: consume the latched guest state; a token with no guest state (a
        // host-side signaller) is honoured directly, exactly as the single wait does.
        if (o.type == 1) keGuestEventTryConsume(o.hdr);
        return done(i, "host-token");
      }
      if (wr == WAIT_FAILED && !infinite) return done(kTimeout, "wait-failed");
    } else {
      // WaitAll: every guest-side object signalled (or latched), then every host object at once.
      bool guestAll = true;
      for (uint32_t i = 0; i < count; ++i)
        if (objs[i].kind != 2 && !keWmGuestSignalled(objs[i])) { guestAll = false; break; }
      if (guestAll) {
        DWORD n = 0;
        for (uint32_t i = 0; i < count; ++i)
          if (objs[i].kind == 2) { hs[n] = objs[i].h; hmap[n] = i; ++n; }
        const DWORD wr = n ? WaitForMultipleObjects(n, hs, TRUE, slice) : WAIT_OBJECT_0;
        if (wr < WAIT_OBJECT_0 + (n ? n : 1)) {
          // Consume the guest side; if a CAS lost a race, give everything back and retry.
          uint32_t consumed[64];
          uint32_t nc = 0;
          bool ok = true;
          for (uint32_t i = 0; i < count && ok; ++i) {
            if (objs[i].kind != 0) continue;
            const bool wasLatched = objs[i].latched;
            if (keWmConsumeTruth(objs[i])) {
              if (objs[i].type == 1 && !wasLatched) consumed[nc++] = i;
            } else {
              ok = false;
            }
          }
          if (ok) return done(0, "all");
          for (uint32_t k = 0; k < nc; ++k) writeGuest32(objs[consumed[k]].va + 4, 1u);
          for (DWORD k = 0; k < n; ++k) keWmUndoHost(hs[k]);
          continue;
        }
        if (!infinite && slice == 0) return done(kTimeout, "timeout");
        if (wr == WAIT_FAILED && !infinite) return done(kTimeout, "wait-failed");
      } else {
        if (!infinite && slice == 0) return done(kTimeout, "timeout");
        // Sleep on the tokens of the unsignalled truth events (KTHREADs are polled).
        DWORD n = 0;
        for (uint32_t i = 0; i < count; ++i)
          if (objs[i].kind == 0 && !keWmGuestSignalled(objs[i]) && objs[i].h) {
            hs[n] = objs[i].h; hmap[n] = i; ++n;
          }
        DWORD wr = WAIT_TIMEOUT;
        if (n) wr = WaitForMultipleObjects(n, hs, FALSE, slice);
        else Sleep(slice);
        if (wr < WAIT_OBJECT_0 + n) {
          KeWmObj& o = objs[hmap[wr - WAIT_OBJECT_0]];
          if (readGuestU32BE(o.va + 4) == 0) o.latched = true;   // host-side signaller
        }
      }
    }
    if (infinite && (++slices % 150) == 0) {
      const uint32_t s = g_keWmStuckLogs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (s <= 20 || (s & (s - 1)) == 0)
        SR_LOG_KERNEL_IMPORT("[KE_WAITMULTI] wait-stuck n=%u %s obj0=%08X(st=%u) obj1=%08X(st=%u) lr=%08X tid=%lu slices=%d",
                             count, waitAny ? "ANY" : "ALL", objs[0].va, readGuestU32BE(objs[0].va + 4),
                             count > 1 ? objs[1].va : 0u, count > 1 ? readGuestU32BE(objs[1].va + 4) : 0u,
                             lr, GetCurrentThreadId(), slices);
    }
  }
}

#endif  // _WIN32

uint32_t stub_RtlAllocateHeap(uint32_t r3, uint32_t r4, uint32_t r5,
                              uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r3;
  (void)r4;
  (void)r6;
  (void)r7;
  (void)r8;
  if (r5 == 0) {
    STUB_LOG("RtlAllocateHeap", r3, r4, r5, r6, r7, r8, 0u);
    return 0;
  }
  uint32_t ret = guestHeapAlloc(r5);
  if (ret == 0)
    sr::kernel::noteGuestAllocFailure(sr::kernel::GuestAllocApi::RtlAllocateHeap, r5, r4, 0, "heap exhausted");
  STUB_LOG("RtlAllocateHeap", r3, r4, r5, r6, r7, r8, ret);
  return ret;
}

// ========== DbgPrint / _vsnprintf / sprintf ==========
// FIX iter-189: guestVAToHost doesn't validate that the page is committed; a
// garbage %s arg (e.g. VA 0x1F2307B8, crash iter189b) faulted inside strnlen.
// SEH-guard the probe; plain function so __try needs no unwinding.
static size_t safeStrnlen(const char* p, size_t maxLen) {
#if defined(_WIN32)
  __try {
    return strnlen(p, maxLen);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return SIZE_MAX;
  }
#else
  return strnlen(p, maxLen);
#endif
}

// Read a null-terminated ASCII string from guest memory (up to maxLen chars).
static std::string readGuestCString(uint32_t guestVA, size_t maxLen = 512) {
  if (guestVA == 0) return "";
  uint8_t* host = guestVAToHost(guestVA);
  if (!host) return "";
  size_t len = safeStrnlen(reinterpret_cast<const char*>(host), maxLen);
  if (len == SIZE_MAX) return "";
  return std::string(reinterpret_cast<const char*>(host), len);
}

// Write a null-terminated ASCII string to guest memory (up to maxLen bytes including null).
static void writeGuestCString(uint32_t guestVA, const char* str, size_t maxLen) {
  if (guestVA == 0 || !str || maxLen == 0) return;
  uint8_t* host = guestVAToHost(guestVA);
  if (!host) return;
  size_t len = std::strlen(str);
  if (len >= maxLen) len = maxLen - 1;
  std::memcpy(host, str, len);
  host[len] = '\0';
}

uint32_t stub_DbgPrint(uint32_t r3, uint32_t r4, uint32_t r5,
                       uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  // r3 = pointer to null-terminated string (already formatted by _vsnprintf).
  std::string msg = readGuestCString(r3);
  if (!msg.empty()) {
    // Trim trailing newline for cleaner log output.
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
      msg.pop_back();
    if (!msg.empty()) {
      SR_LOG_KERNEL_IMPORT("[GAME:DBG] (lr=0x%08X) %s", getCurrentGuestPC(), msg.c_str());
    }
  }
  return 0;
}

// iter-189: RtlRaiseException/RtlUnwind were stub_generic_success — the guest
// throw site is compiled as NORETURN, so silently returning sends the thread
// through unreachable code with stale registers (seed of the .data container
// corruption: wild memmoves, zero-sweeps, byte-shifted blob copies).
// Step 1 (this stub): log every raise with live LR + decoded EXCEPTION_RECORD
// so each site can be neutralized at the source. Behavior unchanged (returns).
static uint32_t stub_RtlRaiseException(uint32_t r3, uint32_t r4, uint32_t r5,
                                       uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  uint32_t code = 0, flags = 0, addr = 0, nParams = 0, p0 = 0, p1 = 0;
  if (r3 != 0) {
    code    = sr::kernel::readGuestU32BE(r3 + 0);
    flags   = sr::kernel::readGuestU32BE(r3 + 4);
    addr    = sr::kernel::readGuestU32BE(r3 + 12);
    nParams = sr::kernel::readGuestU32BE(r3 + 16);
    p0      = sr::kernel::readGuestU32BE(r3 + 20);
    p1      = sr::kernel::readGuestU32BE(r3 + 24);
  }
  static std::atomic<unsigned> s_count{0};
  unsigned n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 40 || (n & (n - 1)) == 0) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[RTL_RAISE] #%u code=0x%08X flags=0x%08X exAddr=0x%08X nParams=%u p0=0x%08X p1=0x%08X "
      "liveLR=0x%08X r1=0x%08X tid=%lu — NO UNWIND, thread continues into noreturn shadow",
      n, code, flags, addr, nParams, p0, p1,
      getCurrentGuestPC(), sr::kernel::getCurrentGuestR1(), GetCurrentThreadId());
  }
  return kStatusSuccess;
}

static uint32_t stub_RtlUnwind(uint32_t r3, uint32_t r4, uint32_t r5,
                               uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r5; (void)r6; (void)r7; (void)r8;
  static std::atomic<unsigned> s_count{0};
  unsigned n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 40 || (n & (n - 1)) == 0) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[RTL_UNWIND] #%u targetFrame=0x%08X targetIp=0x%08X liveLR=0x%08X r1=0x%08X tid=%lu",
      n, r3, r4, getCurrentGuestPC(), sr::kernel::getCurrentGuestR1(), GetCurrentThreadId());
  }
  return kStatusSuccess;
}

// _vsnprintf(r3=buf, r4=count, r5=format, r6=va_list_ptr)
// We can't fully parse PPC va_list, so we copy the format string to the output
// buffer. This preserves %d/%x etc. but gives us the message structure.
// Also: try to read up to 4 integer args from the va_list for common cases.
static uint32_t stub_vsnprintf(uint32_t r3, uint32_t r4, uint32_t r5,
                               uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r7; (void)r8;
  std::string fmt = readGuestCString(r5);
  if (fmt.empty() || r3 == 0 || r4 == 0) return 0;

  // Copy format string to output buffer as a baseline.
  writeGuestCString(r3, fmt.c_str(), r4);

  // Return the number of chars written (not including null).
  size_t maxWrite = (r4 > 0) ? (r4 - 1) : 0;
  if (fmt.size() < maxWrite) maxWrite = fmt.size();
  uint32_t written = static_cast<uint32_t>(maxWrite);
  return written;
}

// sprintf(r3=buf, r4=format, r5..r8=args)
// PPC calling convention: first 4 integer args in r5-r8, %s args are guest VA pointers.
static uint32_t stub_sprintf(uint32_t r3, uint32_t r4, uint32_t r5,
                             uint32_t r6, uint32_t r7, uint32_t r8) {
  std::string fmt = readGuestCString(r4);
  if (fmt.empty() || r3 == 0) return 0;

  uint32_t args[] = {r5, r6, r7, r8};
  unsigned argIdx = 0;
  std::string result;
  result.reserve(256);

  for (size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%' || i + 1 >= fmt.size()) {
      result += fmt[i];
      continue;
    }
    size_t start = i++;
    // Flags
    while (i < fmt.size() && (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == '0' || fmt[i] == ' ' || fmt[i] == '#')) i++;
    // Width
    while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') i++;
    // Precision
    if (i < fmt.size() && fmt[i] == '.') { i++; while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') i++; }
    // Length modifier
    if (i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'h')) i++;
    if (i >= fmt.size()) { result.append(fmt, start, i - start); break; }
    char spec = fmt[i];
    std::string fmtSpec(fmt, start, i - start + 1);
    uint32_t val = (argIdx < 4) ? args[argIdx] : 0;
    char tmp[128] = {};
    switch (spec) {
      case 'd': case 'i': snprintf(tmp, sizeof(tmp), fmtSpec.c_str(), (int32_t)val); argIdx++; break;
      case 'u': snprintf(tmp, sizeof(tmp), fmtSpec.c_str(), val); argIdx++; break;
      case 'x': case 'X': snprintf(tmp, sizeof(tmp), fmtSpec.c_str(), val); argIdx++; break;
      case 'p': snprintf(tmp, sizeof(tmp), "0x%08X", val); argIdx++; break;
      case 'c': tmp[0] = (char)val; tmp[1] = 0; argIdx++; break;
      case 's': { std::string s = readGuestCString(val); snprintf(tmp, sizeof(tmp), "%s", s.c_str()); argIdx++; break; }
      case 'f': case 'e': case 'g': snprintf(tmp, sizeof(tmp), fmtSpec.c_str(), 0.0); argIdx++; break;
      case '%': tmp[0] = '%'; tmp[1] = 0; break;
      default: snprintf(tmp, sizeof(tmp), "%s", fmtSpec.c_str()); break;
    }
    result += tmp;
  }
  writeGuestCString(r3, result.c_str(), 4096);
  static unsigned s_sprintfLog = 0;
  if (++s_sprintfLog <= 30) {
    SR_LOG_KERNEL_IMPORT("[SPRINTF] fmt=\"%.60s\" -> \"%.120s\"", fmt.c_str(), result.c_str());
  }
  return static_cast<uint32_t>(result.size());
}

uint32_t stub_KeDelayExecutionThread(uint32_t r3, uint32_t r4, uint32_t r5,
                                       uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r3;
  (void)r6;
  (void)r7;
  (void)r8;
  // r3 = WaitMode, r4 = BOOLEAN Alertable, r5 = PLARGE_INTEGER Interval.
  // Interval is in 100ns units, stored big-endian in guest memory.
  // Negative value = relative timeout (common case).
  uint8_t* host = guestVAToHost(r5);
  int64_t interval100ns = 0;
  if (host) {
    // Read big-endian int64
    uint64_t raw = ((uint64_t)host[0] << 56) | ((uint64_t)host[1] << 48) |
                   ((uint64_t)host[2] << 40) | ((uint64_t)host[3] << 32) |
                   ((uint64_t)host[4] << 24) | ((uint64_t)host[5] << 16) |
                   ((uint64_t)host[6] << 8)  | (uint64_t)host[7];
    interval100ns = (int64_t)raw;
  }
  // Negative = relative timeout; positive = absolute (treat as small sleep).
  uint64_t sleepUs = 0;
  if (interval100ns < 0) {
    // Relative timeout: absolute value in 100ns units -> microseconds
    sleepUs = (uint64_t)(-interval100ns) / 10;
  } else if (interval100ns > 0) {
    // Absolute time: just sleep a frame tick
    sleepUs = 1000; // 1ms default
  }
  // Cap at 100ms to avoid blocking too long
  if (sleepUs > 100000) sleepUs = 100000;
  // iter-464 perf lane, SR_KDELAY_XENIA=1: Xenia's XThread::Delay (kernel/xthread.cc) -
  // an absolute or zero interval is 0 ms, a relative one is truncated to whole ms, and
  // 0 ms YIELDS (Sleep(0) / alertable SleepEx(0)) instead of sleeping. The legacy path
  // below rounds everything under 1 ms UP to a 1 ms sleep, which the default 15.6 ms
  // timer quantum turns into ~15.6 ms.
  static const bool s_xeniaDelay = [] {
    const char* e = std::getenv("SR_KDELAY_XENIA");
    return e && e[0] == '1';
  }();
  if (s_xeniaDelay) {
    const DWORD ms = interval100ns < 0 ? DWORD(std::min<uint64_t>(sleepUs, 100000) / 1000) : 0;
#ifdef _WIN32
    if (r4) {
      if (SleepEx(ms, TRUE) == WAIT_IO_COMPLETION) return 0xC0u;  // STATUS_USER_APC
      return kStatusSuccess;
    }
    Sleep(ms);  // Sleep(0) = Xenia MaybeYield (yields to ready threads on any core)
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
    return kStatusSuccess;
  }
  // Minimum 1ms sleep to avoid busy-wait
  if (sleepUs < 1000) sleepUs = 1000;
  DWORD sleepMs = (DWORD)(sleepUs / 1000);

  // Throttled logging
  static thread_local uint64_t s_count = 0;
  s_count++;
  if (s_count == 1 || s_count == 100 || s_count == 10000) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[KERNEL] KeDelayExecutionThread count=%llu interval100ns=%lld sleepUs=%llu alertable=%u",
      (unsigned long long)s_count, (long long)interval100ns, (unsigned long long)sleepUs, r4);
  }

#ifdef _WIN32
  // r4 = alertable flag. Use SleepEx so QueueUserAPC can interrupt this sleep.
  // When WAIT_IO_COMPLETION fires, return STATUS_USER_APC (0xC0 = 192) so callers
  // (sub_8246A898) exit the wait loop and workers re-check their work queues.
  if (r4) {
    DWORD result = SleepEx(sleepMs, TRUE);
    if (result == WAIT_IO_COMPLETION) {
      static thread_local uint64_t s_apcCount = 0;
      if (++s_apcCount <= 20)
        fprintf(stderr, "[KeDelayExecutionThread] APC fired on thread=%u (apc#%llu)\n",
                sr::kernel::getCurrentThreadHandle(), (unsigned long long)s_apcCount);
      return 0xC0u; // STATUS_USER_APC — signals alertable wait was interrupted
    }
    return kStatusSuccess;
  }
#endif
  std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
  return kStatusSuccess;
}

// iter-449r PLATFORM FIX bookkeeping: Xam content enumerators. The guest
// closes these with NtClose (sub_824680F0), not XamContentClose, so the map
// lives up here where stub_NtClose can release entries. handle -> items
// remaining (always 0 today: a fresh console with no save packages).
namespace {
std::mutex g_xamEnumMutex;
std::unordered_map<uint32_t, uint32_t> g_xamEnums;
uint32_t g_xamEnumNextHandle = 0x0005E000u;  // g_xnNextHandle-style counter,
                                             // stays < 0x00100000 (handles
                                             // are small ints, NtReadFile
                                             // convention detector)
void xamEnumClose(uint32_t handle) {
  std::lock_guard<std::mutex> lk(g_xamEnumMutex);
  g_xamEnums.erase(handle);
}
}  // namespace

// NtQueryDirectoryFile keeps its find rule and cursor per open directory
// handle. The state is declared below with the syscall implementation, but it
// must be retired here with the handle rather than leaking until process exit.
static void ntQueryDirClose(uint32_t handle);
// step4/audit SR_XNOTIFY_XENIA (Astra batch-2 P2, reviews of 0492b836 / 170216cc): a closed listener handle stops
// naming its listener, which stays registered as on Xenia (defined with the notification queue below; a no-op with the
// gate off).
// 0: not a Xenia-path listener; 1: its handle closed now; 2: its handle was closed already (STATUS_INVALID_HANDLE)
static int xnotifyListenerClose(uint32_t handle);
// True when `handle` is (or was, *closed = true) a Xenia-path listener's handle. False with the gate off.
static bool xnotifyIsListener(uint32_t handle, bool* closed);

uint32_t stub_NtClose(uint32_t r3, uint32_t r4, uint32_t r5,
                      uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4;
  (void)r5;
  (void)r6;
  (void)r7;
  (void)r8;
  // SR_XNOTIFY_XENIA (Astra reviews of 0492b836 / 170216cc): a listener handle's close drops only its mapping, in one
  // step with the listener check; the listener and its host event stay (Xenia). A second close of it is refused.
  const int xnClose = xnotifyListenerClose(r3);
  if (xnClose == 2) {
    STUB_LOG("NtClose", r3, r4, r5, r6, r7, r8, kStatusInvalidHandle);
    return kStatusInvalidHandle;   // Xenia ObjectTable::ReleaseHandle: the handle is gone
  }
#ifdef _WIN32
  if (xnClose == 0) eventHandleClose(r3);
#endif
  fileHandleClose(r3);
  ntQueryDirClose(r3);
  cmdq::ioDiagNoteClose(r3);   // no-op unless SR_DIAG_IO_DELAY_MS
  xamEnumClose(r3);
  // iter-462 (SR_OB_KTHREAD): drop one open of a thread handle's KTHREAD mapping.
  if (obKThreadEnabled()) kthreadCloseHandle(r3);
  xamContentOnNtClose(r3);  // iter-463 (SR_XAM_CONTENT_FS): enumerators + package files
  STUB_LOG("NtClose", r3, r4, r5, r6, r7, r8, kStatusSuccess);
  return kStatusSuccess;
}

namespace {
  static std::atomic<int> s_createFileSuccessLogCount{0};
  static std::atomic<int> s_createFileDirLogCount{0};

  std::FILE* openReadBinaryFile(const std::string& path) {
#if defined(_WIN32)
    // Host paths are UTF-8; the console's narrow guest file names are appended
    // to a Unicode installation root. Never reinterpret that root in the ACP.
    if (path.size() >= INT_MAX) return nullptr;
    std::wstring wide(path.size() + 1, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1,
                             wide.data(), static_cast<int>(wide.size()))) return nullptr;
    std::FILE* fp = nullptr;
    if (_wfopen_s(&fp, wide.c_str(), L"rb") == 0) return fp;
    return nullptr;
#else
    return std::fopen(path.c_str(), "rb");
#endif
  }
}

// iter-463 (h): the 9th argument (first stack slot, low word of the 64-bit
// slot) at import entry, [r1 + 0x54] - NtCreateFile's CreateOptions.
static uint32_t ntStackArg9() {
  const uint32_t r1 = sr::kernel::getCurrentGuestR1();
  if (r1 == 0) return 0;
  const uint32_t va = r1 + 0x54u;
  return isGuestVAInRange(va) ? readGuestU32BE(va) : 0;
}

// r3 = OUT PHANDLE FileHandle, r4 = DesiredAccess, r5 = POBJECT_ATTRIBUTES
// createOptions: NtCreateFile's 9th argument / NtOpenFile's OpenOptions (r8).
static uint32_t ntCreateFileCommon(uint32_t r3, uint32_t r4, uint32_t r5,
                                   uint32_t r6, uint32_t r7, uint32_t r8,
                                   uint32_t createOptions) {
  (void)r4;
  (void)r6;
  (void)r7;
  (void)r8;
  // iter-32 diag: log guest LR for the FIRST few NtCreateFile calls so we
  // can identify which guest path-builder is producing corrupt strings.
  {
    static std::atomic<int> s_lrLogCount{0};
    int n = s_lrLogCount.fetch_add(1, std::memory_order_relaxed);
    if (n < 10) {
      uint32_t lr = sr::kernel::getCurrentGuestLR();
      SR_LOG_KERNEL_FILE("NtCreateFile call#%d caller_lr=0x%08X OA=0x%08X", n + 1, lr, r5);
    }
  }
  std::string path = resolveGuestPath(r5);
  std::string requestedPath = path;
  std::FILE* f = nullptr;
  uint32_t handle = 0;
  // iter-463 mod overlay (mod_overlay.hpp): an enabled mod's file shadows or adds
  // this disc path, opened read-only. No mod enabled -> returns "" at once.
  if (!path.empty() && modOverlayActive()) {
    std::string modPath = modOverlayResolveFile(path);
    if (!modPath.empty()) {
      f = openReadBinaryFile(modPath);
      if (f) path = modPath;
      else SR_LOG_BOOT(sr::LogSeverity::Error, "[MOD] error: open failed for \"%s\" - the disc file is used", modPath.c_str());
    }
  }
  if (!path.empty()) {
    if (!f) f = openReadBinaryFile(path);
    // step4/audit batch 3 (logic audit 2.0g, baked 2026-10-20): NO name alias. The February bring-up alias
    // (DATA/objtiv1.AST -> objtiv.AST, sound1 -> sound, fmv1..5 -> fmv) answered SUCCESS for seven files the retail
    // disc does not hold, so the title mounted seven extra Rtam storages that Xenia and the console never have. Such
    // an open now fails with OBJECT_NAME_NOT_FOUND, as on Xenia (xboxkrnl_io.cc:47-111). A mod's REAL file of that
    // name (a mission mod's objtiv1.AST) is served by the overlay above (mod-overlay test M7f).
    if (f) {
      handle = fileHandleAlloc(f);
      cmdq::ioDiagNoteOpen(handle, requestedPath);   // no-op unless SR_DIAG_IO_DELAY_MS
      // iter-463 (h) SR_READ_PENDING: Xenia NtCreateFile_entry is_synchronous
      // (xboxkrnl_io.cc:94-96).
      constexpr uint32_t kFileSynchronousIoAlert = 0x10u, kFileSynchronousIoNonAlert = 0x20u;
      // iter-466 stutter: SR_ASYNC_READ needs the same per-handle flag.
      if (handle && (readPendingOn() || asyncReadOn()))
        fileHandleSetSynchronous(handle,
            (createOptions & (kFileSynchronousIoAlert | kFileSynchronousIoNonAlert)) != 0);
    } else {
      std::error_code ec;
      if ((std::filesystem::is_directory(std::filesystem::u8path(requestedPath), ec) && !ec) ||
          modOverlayHasDir(requestedPath)) {  // iter-463: a folder only a mod provides
        handle = fileHandleAllocDir(requestedPath);
        if (handle && s_createFileDirLogCount.fetch_add(1, std::memory_order_relaxed) < 3)
          SR_LOG_KERNEL_FILE("NtCreateFile dir path=\"%s\" -> handle=%u", requestedPath.c_str(), handle);
      }
    }
  }
  if (handle != 0 && r3 != 0)
    writeGuestBE32(r3, handle);
  if (!handle && !requestedPath.empty())
    SR_LOG_KERNEL_FILE("NtCreateFile path=\"%s\" -> FILE_NOT_FOUND", requestedPath.c_str());
  if (handle && !path.empty() && fileHandleGet(handle)) {
    if (s_createFileSuccessLogCount.fetch_add(1, std::memory_order_relaxed) < 50)
      SR_LOG_KERNEL_FILE("NtCreateFile path=\"%s\" -> handle=%u", path.c_str(), handle);
  }
  STUB_LOG("NtCreateFile", r3, r4, r5, r6, r7, r8, handle ? kStatusSuccess : 0xC0000034u /* FILE_NOT_FOUND */);
  return handle ? kStatusSuccess : 0xC0000034u;
}

// NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
//              AllocationSize, FileAttributes, ShareAccess r9,
//              CreateDisposition r10, CreateOptions [stack arg 9])
uint32_t stub_NtCreateFile(uint32_t r3, uint32_t r4, uint32_t r5,
                           uint32_t r6, uint32_t r7, uint32_t r8) {
  sr::hitch::Scope hitchScope(sr::hitch::kFileOpen);
  // iter-463 (SR_XAM_CONTENT_FS): paths inside a mounted content package.
  { uint32_t st; if (xamContentTryNtCreateFile(r3, r4, r5, r6, r7, r8, &st)) return st; }
  return ntCreateFileCommon(r3, r4, r5, r6, r7, r8, ntStackArg9());
}

// NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
//            ShareAccess r7, OpenOptions r8). NT order: the options are r8.
// Xenia's NtOpenFile_entry takes r7 (the share mask) as its options - a Xenia
// quirk (xam_content.cpp documents the guest's DeleteFileA: share 7 in r7,
// options 0x4040 in r8); here the console's own argument order wins.
uint32_t stub_NtOpenFile(uint32_t r3, uint32_t r4, uint32_t r5,
                         uint32_t r6, uint32_t r7, uint32_t r8) {
  sr::hitch::Scope hitchScope(sr::hitch::kFileOpen);
  { uint32_t st; if (xamContentTryNtOpenFile(r3, r4, r5, r6, r7, r8, &st)) return st; }  // iter-463
  return ntCreateFileCommon(r3, r4, r5, r6, r7, r8, r8);
}

// Forward decls (defined near stub_NtQueueApcThread below).
static void CALLBACK noop_apc_callback(ULONG_PTR);
static HANDLE eventHandleGet(uint32_t guestHandle);

// NtReadFile — REAL NT contract (iter-189, docs/OTHER_RECOMPS_LESSONS.md §2/§3):
//   r3=FileHandle, r4=Event, r5=ApcRoutine, r6=ApcContext,
//   r7=IoStatusBlock, r8=Buffer, r9=Length, r10=ByteOffset
// The old stub assumed r4=IOSB / r5=Buffer. With a real-convention caller that
// passes Event/ApcRoutine, that fread'd file data INTO .text (r5 = code VA) and
// scribbled status words at VA == event handle. Worse, completion was never
// signaled: this engine is APC-driven (iter-43), so an async streaming read that
// sleeps for its completion APC stalls forever — the menu-blocker signature
// (exactly one file ever read).
// Detection order: real convention first, then the two legacy wrapper layouts.
// iter-463 (f) SR_ATOMIC_READ, default ON: positioned reads under the per-handle
// I/O lock (see stub_NtReadFile). NtQueryInformationFile / NtSetInformationFile
// take the same lock so their position moves cannot interleave with a read.
static bool atomicReadOn() {
  static const bool v = srGateDefaultOn("SR_ATOMIC_READ");
  return v;
}

// ── iter-466 stutter lane: SR_ASYNC_READ - overlapped reads complete asynchronously ──
// The console's kernel completes a read on a handle opened for overlapped I/O (no
// FILE_SYNCHRONOUS_IO_*) asynchronously: NtReadFile queues it and returns STATUS_PENDING,
// the DVD/HDD DMA finishes later, the IOSB is written and the event + file object are
// signalled (+ the completion APC). The title's stream readers are built for that: e.g.
// CalAsyncFileStream_Read_sub_824776F0 re-issues ReadFile on the freed blocks of its ring
// (tolerating ERROR_IO_PENDING 997) and only waits (GetOverlappedResult) on the block it
// consumes, so the disk latency of the blocks ahead is hidden. Completed synchronously on
// the calling thread (Xenia's NtReadFile does the same: "if (true || is_synchronous())",
// xboxkrnl_io.cc), every re-issue blocks for the full host read: a movie's 128 KB block
// read on the guest render loop held a frame 270-536 ms on an HDD (stutter lane, roll
// h1_a). Here the read runs on one FIFO worker (the device queue: completion order =
// submission order); NtReadFile clears the event and the file object's event (NT does
// when it queues the I/O), writes IOSB {PENDING, 0} and returns STATUS_PENDING. The
// worker reads under the same per-handle I/O lock and file-position rule as the
// synchronous path, writes the IOSB, then sets the event and the file object's event and
// wakes the issuing thread's alertable waits (the synchronous path's completion, moved).
// The XAPI ReadFile maps PENDING through RtlNtStatusToDosError -> 997 (xam_content.cpp
// maps exactly STATUS_PENDING while this gate is on). Only real-convention calls with an
// explicit offset on a non-synchronous handle take this path; everything else is as before.
// SR_DIAG_READ_LATENCY_MS=<ms> (named DIAGNOSTIC, default off; timing only, never data): every
// host read of a guest NtReadFile sleeps <ms> first, on whichever thread performs it (the
// caller on the synchronous path, the worker under SR_ASYNC_READ). It models a slow device
// (an HDD under load: roll h1_a saw 270-536 ms per 128 KB) so the async A/B does not depend on
// whether the OS file cache happens to hold the movie from the previous roll.
static uint32_t diagReadLatencyMs() {
  static const uint32_t v = [] {
    const char* e = std::getenv("SR_DIAG_READ_LATENCY_MS");
    const uint32_t ms = e ? uint32_t(std::strtoul(e, nullptr, 10)) : 0u;
    if (ms) SR_LOG_BOOT(sr::LogSeverity::Info, "[DIAG_READ_LATENCY] every guest file read waits %u ms (diagnostic)", ms);
    return ms;
  }();
  return v;
}
static bool asyncReadOn() {
  // Default ON since iter-466 (same-exe A/B under SR_DIAG_READ_LATENCY_MS=40, rolls l7_*;
  // whole-run frames f7_frames_on); =0 restores the synchronous completion.
  static const bool v = [] {
    const char* e = std::getenv("SR_ASYNC_READ");
    return !e || e[0] != '0';
  }() && atomicReadOn() && xamContentFsEnabled();
  return v;
}
namespace {
struct AioJob {
  uint32_t handle = 0, bufVA = 0, len = 0, iosbVA = 0, eventHandle = 0, issuerTid = 0;
  uint64_t off = 0;
  bool apc = false;
};
struct Aio {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<AioJob> q;
  bool started = false;
};
Aio& aio() {
  static Aio* a = new Aio();  // never destroyed: the worker may outlive static teardown
  return *a;
}
}  // namespace
static void aioComplete(const AioJob& j) {
  sr::hitch::Scope hitchScope(sr::hitch::kFileRead);
  static const bool s_eofStatus = srGateDefaultOn("SR_READ_EOF_STATUS");
  uint32_t st = kStatusSuccess;
  size_t n = 0;
  uint8_t* buf = guestVAToHost(j.bufVA);
  {
    std::shared_ptr<std::mutex> io = fileHandleIoLock(j.handle);
    std::unique_lock<std::mutex> hold;
    const uint64_t l0 = sr::hitch::begin();
    if (io) hold = std::unique_lock<std::mutex>(*io);
    const uint64_t l1 = sr::hitch::begin();
    uint64_t r0 = 0, r1 = 0;
    std::FILE* f = fileHandleGet(j.handle);  // an NtClose may have won the lock
    if (!f) {
      st = kStatusInvalidHandle;
    } else if (!buf) {
      st = s_eofStatus ? kStatusAccessViolationRead : kStatusSuccess;
    } else {
#if SR_ORACLE
      sr::kernel::gww::HostWriteScope gwwScope(j.bufVA, j.len);
#endif
      HANDLE os = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f)));
      LARGE_INTEGER size{};
      const bool haveSize = os != INVALID_HANDLE_VALUE && GetFileSizeEx(os, &size) != 0;
      if (os == INVALID_HANDLE_VALUE) {
        st = s_eofStatus ? kStatusEndOfFile : kStatusSuccess;
      } else if (s_eofStatus && haveSize && j.off >= static_cast<uint64_t>(size.QuadPart)) {
        st = kStatusEndOfFile;
      } else {
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(j.off);
        ov.OffsetHigh = static_cast<DWORD>(j.off >> 32);
        DWORD got = 0;
        r0 = sr::hitch::begin();
        if (const uint32_t dl = diagReadLatencyMs()) Sleep(dl);
        const BOOL ok = ReadFile(os, buf, j.len, &got, &ov);
        r1 = sr::hitch::begin();
        if (!ok) {
          got = 0;
          if (s_eofStatus) st = kStatusEndOfFile;  // Xenia HostPathFile::ReadSync: any failure -> EOF
        }
        n = got;
        if (st == kStatusSuccess) (void)_fseeki64(f, static_cast<__int64>(j.off + n), SEEK_SET);
      }
    }
    if (l0) sr::hitch::slowIo(j.handle, j.len, j.off, l0, l1, r0, r1, f);
  }
  if (st & 0x80000000u) n = 0;
  hitchScope.bytes = n;
  // mem.resources step 2a (SR_MEM_RESOURCES=census): the bytes landed - a host write the registry hears of before the
  // guest can (the IOSB below); a no-op otherwise and under SR_ORACLE=0.
  if (buf && n && sr::gfx::memres::census()) sr::gfx::memres::noteReadFile(j.bufVA, static_cast<uint32_t>(n));
  if (buf && n && sr::gfx::memres::resourceMode())
    sr::gfx::memres::resourceMemory().noteFileRead(j.bufVA, static_cast<uint32_t>(n));
  writeGuestBE32(j.iosbVA + 0, st);
  writeGuestBE32(j.iosbVA + 4, static_cast<uint32_t>(n));
  // The IOSB is written before anything is signalled (SetEvent is a full barrier).
  if (j.eventHandle) {
    if (HANDLE he = eventHandleGet(j.eventHandle)) SetEvent(he);
  }
  if (HANDLE fe = static_cast<HANDLE>(fileHandleWaitEvent(j.handle))) SetEvent(fe);
  if (j.apc && st == kStatusSuccess && j.issuerTid) {
    if (HANDLE t = OpenThread(THREAD_SET_CONTEXT, FALSE, j.issuerTid)) {
      QueueUserAPC(noop_apc_callback, t, 0);
      CloseHandle(t);
    }
  }
}
static void aioSubmit(const AioJob& j) {
  Aio& a = aio();
  {
    std::lock_guard<std::mutex> lk(a.mu);
    if (!a.started) {
      a.started = true;
      uint8_t* base = sr::kernel::getGuestBase();
      const size_t size = sr::kernel::getGuestSize();
      std::thread([base, size] {
        SetThreadDescription(GetCurrentThread(), L"SR async read");
        sr::kernel::setGuestMemory(base, size);
        Aio& w = aio();
        for (;;) {
          AioJob job;
          {
            std::unique_lock<std::mutex> lk2(w.mu);
            w.cv.wait(lk2, [&] { return !w.q.empty(); });
            job = w.q.front();
            w.q.pop_front();
          }
          aioComplete(job);
        }
      }).detach();
      SR_LOG_BOOT(sr::LogSeverity::Info, "[ASYNC_READ] worker started (SR_ASYNC_READ=1)");
      (void)diagReadLatencyMs();
    }
    a.q.push_back(j);
  }
  a.cv.notify_one();
}
bool asyncReadPendingMapped() { return asyncReadOn(); }

uint32_t stub_NtReadFile(uint32_t r3, uint32_t r4, uint32_t r5,
                         uint32_t r6, uint32_t r7, uint32_t r8) {
  sr::hitch::Scope hitchScope(sr::hitch::kFileRead);
  auto isLikelyGuestPtr = [](uint32_t va) -> bool {
    // iter-463 SR_NTAVM_XENIA_HEAP: title buffers may live in Xenia's v00000000 /
    // v40000000 heaps (positive VAs) - a live region there is a guest pointer too.
    if (sr::kernel::xheap::enabled() && sr::kernel::xheap::owns(va)) return isGuestVAInRange(va);
    return va >= kGuestVABase && isGuestVAInRange(va);
  };
  uint32_t r9 = ExportRegistry::getCurrentArgR9();
  uint32_t r10 = ExportRegistry::getCurrentArgR10();

  // --- Convention detection ---
  // Real NT: r7=IOSB + r8=Buffer (both guest ptrs), r9=plausible length, and
  // r4/r5 are NOT (IOSB, Buffer): r4 is 0/small handle, r5 is 0 or a code VA.
  bool r4HandleLike = (r4 < 0x00100000u);                          // unified handles are small ints
  bool r5CodeLike   = (r5 >= 0x82000000u && r5 < 0x82A00000u);     // ApcRoutine lives in image code
  bool realConv = isLikelyGuestPtr(r7) && isLikelyGuestPtr(r8) &&
                  r9 != 0 && r9 < 0x10000000u &&
                  (r4HandleLike || r5CodeLike);

  uint32_t eventHandle = 0, apcRoutineVA = 0, apcContext = 0;
  uint32_t ioStatusVA = 0, bufVA = 0, len = 0, byteOffsetVA = 0;
  const char* conv = "legacy";
  if (realConv) {
    eventHandle  = r4HandleLike ? r4 : 0;
    apcRoutineVA = r5CodeLike ? r5 : 0;
    apcContext   = r6;
    ioStatusVA   = r7;
    bufVA        = r8;
    len          = r9;
    byteOffsetVA = isLikelyGuestPtr(r10) ? r10 : 0;
    conv = "nt";
  } else {
    // Legacy wrapper layout A: r4=IOSB, r5=Buffer, r6=Length, r8/r10=ByteOffset.
    ioStatusVA = r4;
    if (ioStatusVA == 0 && isLikelyGuestPtr(r7) && r5 == 0) ioStatusVA = r7;
    bufVA = r5;
    len = r6;
    // Legacy wrapper layout B ("ext"): r7=IOSB, r8=Buffer, r9=Length, r10=ByteOffset.
    if (bufVA == 0 && len == 0 && isLikelyGuestPtr(r7) && isLikelyGuestPtr(r8) && r9 != 0) {
      bufVA = r8;
      len = r9;
      ioStatusVA = r7;
      conv = "ext";
    }
    byteOffsetVA = (conv[0] == 'e') ? r10 : r8;
    if (conv[0] != 'e' &&
        (byteOffsetVA == 0 || !isLikelyGuestPtr(byteOffsetVA)) &&
        isLikelyGuestPtr(r10)) {
      byteOffsetVA = r10;
    }
  }

  cmdq::ioDiagMaybeDelay(r3);   // S3 control diagnostic; no-op unless SR_DIAG_IO_DELAY_MS
  std::FILE* f = fileHandleGet(r3);
  if (!f) {
    if (ioStatusVA && isLikelyGuestPtr(ioStatusVA)) {
      writeGuestBE32(ioStatusVA + 0, kStatusInvalidHandle);
      writeGuestBE32(ioStatusVA + 4, 0u);
    }
    STUB_LOG("NtReadFile", r3, r4, r5, r6, r7, r8, kStatusInvalidHandle);
    return kStatusInvalidHandle;
  }

  // iter-388 PLATFORM FIX -> iter-463 (f) SR_ATOMIC_READ, now default ON (=0
  // restores the old unlocked seek+fread). NT's NtReadFile with an explicit
  // ByteOffset is an ATOMIC positioned read; the old seek+fread pair on a
  // shared FILE* is not - a concurrent same-handle read can move the position
  // between the two and hand one reader the other's bytes (RAG #189 package
  // image shear; iter-463 movie-3 handle-95 reads from tids 112/113 < 1 ms
  // apart). Xenia: XFile::Read holds the per-file lock (xfile.cc:105) and the
  // host read is positioned - Win32FileHandle::Read = ReadFile with
  // OVERLAPPED.Pointer = offset (base/filesystem_win.cc:115-135). Same here:
  // per-handle lock (file_io.hpp fileHandleIoLock) + ReadFile(OVERLAPPED) on
  // the FILE*'s OS handle, then the file position = offset + bytes read
  // (xfile.cc:176-179). No offset, or an offset of -1, reads at the current
  // position (xfile.cc:114-117). Like Xenia, any other value (including
  // FILE_USE_FILE_POINTER_POSITION, 0xFFFFFFFFFFFFFFFE) is a literal offset.
  //
  // SR_READ_EOF_STATUS (default ON, needs SR_ATOMIC_READ): a read that starts at
  // or past the end returns STATUS_END_OF_FILE with 0 bytes (disc device:
  // disc_image_file.cc:26-27), any other failed host read is END_OF_FILE too
  // (HostPathFile::ReadSync, host_path_file.cc:33-43), an unmapped buffer is
  // ACCESS_VIOLATION (XFile::ReadInternal, xfile.cc:124-156), and the IOSB
  // carries the real status - {status, 0} on failure (xboxkrnl_io.cc:151-153,
  // 205-208). Old path: SUCCESS with 0 bytes.
  static const bool s_atomicRead = atomicReadOn();
  static const bool s_eofStatus = srGateDefaultOn("SR_READ_EOF_STATUS");
  uint32_t readStatus = kStatusSuccess;

  // iter-388d [IMGREAD_OVERLAP]: the package-image overwriter is a KERNEL-MODE
  // write (fread fills its buffer in the kernel; user-mode Dr watchpoints
  // never fire — the Dr0 zero-hit mystery) = another FILE READ whose buffer
  // overlaps the live image. Flag any such read loudly, with its handle/len/
  // tid/guest lr — this names the overlapping consumer directly.
  {
    static std::atomic<bool> s_imgLive{false};
    if (len == 6830004u) s_imgLive.store(true, std::memory_order_relaxed);
    if (s_imgLive.load(std::memory_order_relaxed) && len != 6830004u &&
        bufVA < 0x9245F2C4u && bufVA + len > 0x91D8AC90u) {
      static std::atomic<unsigned> s_ov{0};
      unsigned ov = s_ov.fetch_add(1, std::memory_order_relaxed) + 1;
      if (ov <= 32 || (ov & (ov - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Error,
            "[IMGREAD_OVERLAP] #%u handle=%u buf=%08X len=%u OVERLAPS live pkg "
            "image [91D8AC90,9245F2C4) tid=%u guestLR=%08X",
            ov, r3, bufVA, len, sr::kernel::getCurrentThreadHandle(),
            sr::kernel::getCurrentGuestLR());
    }
  }

  // iter-466 stutter lane: SR_ASYNC_READ (above) - an overlapped read on a non-synchronous
  // handle completes on the async worker; this thread gets STATUS_PENDING back at once.
  if (realConv && s_atomicRead && asyncReadOn() && len && !fileHandleIsSynchronous(r3) &&
      ioStatusVA && isLikelyGuestPtr(ioStatusVA) && byteOffsetVA && guestVAToHost(bufVA)) {
    const uint64_t aoff =
        (uint64_t(readGuestU32BE(byteOffsetVA)) << 32) | uint64_t(readGuestU32BE(byteOffsetVA + 4));
    // A read at/after the end is answered at once (END_OF_FILE is returned, not PENDING:
    // xboxkrnl_io.cc:167-169), as on the synchronous path below.
    LARGE_INTEGER asz{};
    HANDLE aos = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f)));
    const bool pastEnd = aos != INVALID_HANDLE_VALUE && GetFileSizeEx(aos, &asz) != 0 &&
                         aoff >= static_cast<uint64_t>(asz.QuadPart);
    if (aoff != ~0ull && !pastEnd) {
      if (eventHandle) {
        if (HANDLE he = eventHandleGet(eventHandle)) ResetEvent(he);
      }
      if (HANDLE fe = static_cast<HANDLE>(fileHandleWaitEvent(r3))) ResetEvent(fe);
      writeGuestBE32(ioStatusVA + 0, kStatusPending);
      writeGuestBE32(ioStatusVA + 4, 0u);
      AioJob job;
      job.handle = r3;
      job.bufVA = bufVA;
      job.len = len;
      job.iosbVA = ioStatusVA;
      job.eventHandle = eventHandle;
      job.issuerTid = GetCurrentThreadId();
      job.off = aoff;
      job.apc = apcRoutineVA != 0;
      aioSubmit(job);
      hitchScope.t0 = 0;  // the read is counted on the worker
      STUB_LOG("NtReadFile", r3, r4, r5, r6, r7, r8, kStatusPending);
      return kStatusPending;
    }
  }

  long long offLog = -1;  // iter-397e: -1 = sequential (current position)
  uint8_t* buf = guestVAToHost(bufVA);
  size_t n = 0;
#if SR_ORACLE
  // iter-466 cp lane (SR_SHM_WRITE_TRACK): ReadFile writes the guest buffer in kernel mode,
  // where a page the GPU tracker protected fails the read (ERROR_NOACCESS) instead of faulting
  // into its handler. Release the buffer's tracked pages first (Xenia does the same before
  // host I/O: Memory::TriggerPhysicalMemoryCallbacks).
  sr::kernel::gww::HostWriteScope gwwScope(buf ? bufVA : 0u, buf ? len : 0u);
#endif
#if defined(_WIN32)
  if (s_atomicRead) {
    std::shared_ptr<std::mutex> io = fileHandleIoLock(r3);
    std::unique_lock<std::mutex> hold;
    const uint64_t hitchLock0 = sr::hitch::begin();
    if (io) hold = std::unique_lock<std::mutex>(*io);
    const uint64_t hitchLock1 = sr::hitch::begin();
    uint64_t hitchIoOff = 0, hitchRead0 = 0, hitchRead1 = 0;
    struct SlowIo {  // iter-466 stutter: a slow read names itself ([HITCH_IO], SR_HITCH_LOG only)
      uint32_t h, len;
      const uint64_t &l0, &l1, &r0, &r1, &off;
      std::FILE*& f;
      ~SlowIo() {
        if (l0) sr::hitch::slowIo(h, len, off, l0, l1, r0, r1, f);
      }
    } slowIo{r3, len, hitchLock0, hitchLock1, hitchRead0, hitchRead1, hitchIoOff, f};
    f = fileHandleGet(r3);  // re-validate: an NtClose may have won the lock
    if (!f) {
      if (ioStatusVA && isLikelyGuestPtr(ioStatusVA)) {
        writeGuestBE32(ioStatusVA + 0, kStatusInvalidHandle);
        writeGuestBE32(ioStatusVA + 4, 0u);
      }
      return kStatusInvalidHandle;
    }
    uint64_t off = 0;
    bool haveOff = false;
    if (byteOffsetVA && isLikelyGuestPtr(byteOffsetVA)) {
      off = (uint64_t(readGuestU32BE(byteOffsetVA)) << 32) | uint64_t(readGuestU32BE(byteOffsetVA + 4));
      haveOff = off != ~0ull;
    }
    if (haveOff) {
      offLog = static_cast<long long>(off);
    } else {
      const __int64 cur = _ftelli64(f);
      off = cur > 0 ? static_cast<uint64_t>(cur) : 0ull;
    }
    HANDLE os = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f)));
    if (len == 0 || !buf || os == INVALID_HANDLE_VALUE) {
      // Zero length is success for a valid file (xfile.cc:121-123). An
      // unmapped buffer is ACCESS_VIOLATION (xfile.cc:124-156) with
      // SR_READ_EOF_STATUS; =0 keeps the old result (0 bytes, SUCCESS).
      if (len != 0 && !buf && s_eofStatus) readStatus = kStatusAccessViolationRead;
    } else {
      LARGE_INTEGER size{};
      const bool haveSize = GetFileSizeEx(os, &size) != 0;
      if (s_eofStatus && haveSize && off >= static_cast<uint64_t>(size.QuadPart)) {
        readStatus = kStatusEndOfFile;
      } else {
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(off);
        ov.OffsetHigh = static_cast<DWORD>(off >> 32);
        DWORD got = 0;
        hitchIoOff = off;
        hitchRead0 = sr::hitch::begin();
        if (const uint32_t dl = diagReadLatencyMs()) Sleep(dl);
        const BOOL rfOk = ReadFile(os, buf, len, &got, &ov);
        hitchRead1 = sr::hitch::begin();
        if (!rfOk) {
          const DWORD err = GetLastError();
          if (err == ERROR_HANDLE_EOF) {
            got = 0;
            if (s_eofStatus) readStatus = kStatusEndOfFile;
          } else {
            // Xenia HostPathFile::ReadSync: any failed host read -> END_OF_FILE.
            if (s_eofStatus) readStatus = kStatusEndOfFile;
            static std::atomic<int> s_rfErr{0};
            if (s_rfErr.fetch_add(1, std::memory_order_relaxed) < 20)
              SR_LOG_KERNEL_FILE("NtReadFile handle=%u ReadFile failed err=%lu off=%llu len=%u got=%lu",
                                 r3, (unsigned long)err, (unsigned long long)off, len, (unsigned long)got);
          }
        }
        n = got;
        if (readStatus == kStatusSuccess)
          (void)_fseeki64(f, static_cast<__int64>(off + n), SEEK_SET);
      }
    }
  } else
#endif
  {
    if (byteOffsetVA && isLikelyGuestPtr(byteOffsetVA)) {
      uint64_t off = (uint64_t(readGuestU32BE(byteOffsetVA)) << 32) | uint64_t(readGuestU32BE(byteOffsetVA + 4));
      offLog = static_cast<long long>(off);
#if defined(_WIN32)
      (void)_fseeki64(f, static_cast<__int64>(off), SEEK_SET);
#else
      (void)std::fseek(f, static_cast<long>(off), SEEK_SET);
#endif
    }
    if (buf && len) n = std::fread(buf, 1, len, f);
  }
  if (readStatus & 0x80000000u) n = 0;  // failure: IOSB {status, 0} (xboxkrnl_io.cc:205-208)
  // mem.resources step 2a (SR_MEM_RESOURCES=census): the read's bytes, noted for the registry once they landed.
  if (buf && n && sr::gfx::memres::census()) sr::gfx::memres::noteReadFile(bufVA, static_cast<uint32_t>(n));
  if (buf && n && sr::gfx::memres::resourceMode())
    sr::gfx::memres::resourceMemory().noteFileRead(bufVA, static_cast<uint32_t>(n));
  if (ioStatusVA) {
    writeGuestBE32(ioStatusVA + 0, readStatus);
    writeGuestBE32(ioStatusVA + 4, static_cast<uint32_t>(n));
  }
  // iter-388 [BIGREAD]: content probe for the package-image corruption (RAG
  // #189 arc). For multi-MB reads, sample the first dword + the known victim
  // offset (0x60FE10 = X2_BURGER001_PHYS's Havok header in the 6.8MB
  // metropolis package). Wrong AT READ TIME = the read itself is sheared
  // (non-atomic seek+fread on a shared FILE* — concurrent same-handle read
  // moved the position between our seek and fread); right here but wrong at
  // fill 1.5s later = post-read overwrite. Pure logging.
  if (buf && len >= 0x400000u && n >= 0x610000u) {
    auto rd32 = [&](size_t off) -> uint32_t {
      return (uint32_t(buf[off]) << 24) | (uint32_t(buf[off + 1]) << 16) |
             (uint32_t(buf[off + 2]) << 8) | uint32_t(buf[off + 3]);
    };
    SR_LOG_KERNEL_FILE(
        "[BIGREAD] handle=%u len=%u n=%zu off0=%08X victim@60FE10=%08X tid=%u",
        r3, len, n, rd32(0), rd32(0x60FE10),
        sr::kernel::getCurrentThreadHandle());
    // iter-388b [IMGCORRUPT]: 5ms host poll of the victim dword + sibling
    // offsets after a package-image read (Dr0 proved unreliable on late guest
    // threads). Logs the corruption transition instant + which offsets went
    // bad (bounds the overwrite range); correlate the timestamp against
    // engine events. One watcher per big read, self-terminating.
    if (n == 6830004u) {
      struct ImgWatch {
        uint8_t* buf; uint32_t v0;
        static unsigned long __stdcall run(void* p) {
          auto* w = (ImgWatch*)p;
          static const size_t offs[6] =
              {0x60FE10, 0x100000, 0x300000, 0x500000, 0x610000, 0x660000};
          uint32_t init[6];
          auto rd = [&](size_t o) -> uint32_t {
            uint32_t v; memcpy(&v, w->buf + o, 4); return _byteswap_ulong(v);
          };
          for (int i = 0; i < 6; ++i) init[i] = rd(offs[i]);
          bool dumped = false;
          for (int t = 0; t < 24000; ++t) {  // 120s @5ms
            for (int i = 0; i < 6; ++i) {
              uint32_t cur = rd(offs[i]);
              if (cur != init[i]) {
                SR_LOG_BOOT(sr::LogSeverity::Error,
                    "[IMGCORRUPT] t=+%dms off=%zX %08X -> %08X (image %p)",
                    t * 5, offs[i], init[i], cur, (void*)w->buf);
                init[i] = cur;  // keep watching for more transitions
                // iter-388e: the bulk writer is mid-loop RIGHT NOW — snapshot
                // every guest thread's PC/LR; the writer names itself.
                if (!dumped) {
                  dumped = true;
                  sr::kernel::dumpGuestThreadsNow("IMGCORRUPT_THREADS");
                }
              }
            }
            Sleep(5);
          }
          delete w;
          return 0;
        }
      };
      auto* w = new ImgWatch{buf, 0};
      HANDLE h = CreateThread(nullptr, 0, &ImgWatch::run, w, 0, nullptr);
      if (h) CloseHandle(h);
    }
  }

  // --- Completion signaling (real convention only) ---
  // Even for a synchronous completion, the NT contract signals the Event and
  // queues the APC. Xenia does both; we signal the event and queue a host
  // wake-APC to the issuing thread so its next alertable wait returns
  // STATUS_USER_APC and the engine's IO loop re-checks its completion state
  // (iter-43 wake-and-check pattern). Guest APC routine is logged, not executed.
  bool eventSignaled = false;
  if (eventHandle != 0) {
#if defined(_WIN32)
    HANDLE he = eventHandleGet(eventHandle);
    if (he) { eventSignaled = SetEvent(he) != 0; }
#endif
  }
  // Xenia queues the completion APC only for STATUS_SUCCESS (xboxkrnl_io.cc:159-165).
  if (apcRoutineVA != 0 && readStatus == kStatusSuccess) {
#if defined(_WIN32)
    QueueUserAPC(noop_apc_callback, GetCurrentThread(), 0);
#endif
  }
  // iter-463 (h) SR_READ_PENDING: the file object's own event is set after every
  // read (XFile::ReadInternal notify_completion -> async_event_->Set(),
  // xfile.cc:190-198), and a handle opened without FILE_SYNCHRONOUS_IO_* gets
  // STATUS_PENDING back unless the read hit END_OF_FILE (xboxkrnl_io.cc:167-169).
  // The IOSB above already holds the real status and count.
  uint32_t retStatus = readStatus;
  if (readPendingOn()) {
#if defined(_WIN32)
    if (HANDLE fe = static_cast<HANDLE>(fileHandleWaitEvent(r3))) SetEvent(fe);
#endif
    if (!fileHandleIsSynchronous(r3) && readStatus != kStatusEndOfFile)
      retStatus = kStatusPending;  // exactly Xenia's condition (any status but EOF)
  }

  static std::atomic<int> s_readLog{0};
  int readLogIdx = s_readLog.fetch_add(1, std::memory_order_relaxed);
  // iter-365i: always log big reads (>=64K = catalog/stream payloads) with the
  // issuing thread + guest LR — names the stream-I/O driver the FE park kills.
  if (readLogIdx < 200 || (len > 1024 && readLogIdx < 1000) || eventHandle || apcRoutineVA ||
      len >= 0x10000u || readStatus != kStatusSuccess) {
    SR_LOG_KERNEL_FILE("NtReadFile handle=%u buf=0x%08X len=%u read=%zu off=%lld (%s) event=%u%s apc=0x%08X ctx=0x%08X tid=%u lr=0x%08X st=%08X ret=%08X",
      r3, bufVA, len, n, offLog, conv, eventHandle, eventSignaled ? "(signaled)" : "", apcRoutineVA, apcContext,
      sr::kernel::getCurrentThreadHandle(), sr::kernel::getCurrentGuestPC(), readStatus, retStatus);
  }
  STUB_LOG("NtReadFile", r3, r4, r5, r6, r7, r8, retStatus);
  hitchScope.bytes = uint64_t(n);
  return retStatus;
}

// NtQueryInformationFile:
// r3=FileHandle, r4=PIO_STATUS_BLOCK, r5=FileInformation, r6=Length, r7=FileInformationClass
// We implement class 0x22 (FileNetworkOpenInformation) used by archive loaders.
static uint32_t stub_NtQueryInformationFile(uint32_t r3, uint32_t r4, uint32_t r5,
                                            uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r8;
  constexpr uint32_t kFileNetworkOpenInformation = 0x22u;
  constexpr uint32_t kFileNetworkOpenInfoSize = 56u;

  std::FILE* f = fileHandleGet(r3);
  if (!f) {
    if (r4) {
      writeGuestBE32(r4 + 0, kStatusInvalidHandle);
      writeGuestBE32(r4 + 4, 0u);
    }
    STUB_LOG("NtQueryInformationFile", r3, r4, r5, r6, r7, r8, kStatusInvalidHandle);
    return kStatusInvalidHandle;
  }
  if (r7 != kFileNetworkOpenInformation) {
    if (r4) {
      writeGuestBE32(r4 + 0, kStatusInvalidInfoClass);
      writeGuestBE32(r4 + 4, 0u);
    }
    STUB_LOG("NtQueryInformationFile", r3, r4, r5, r6, r7, r8, kStatusInvalidInfoClass);
    return kStatusInvalidInfoClass;
  }
  if (r5 == 0 || r6 < kFileNetworkOpenInfoSize || !guestVAToHost(r5)) {
    if (r4) {
      writeGuestBE32(r4 + 0, kStatusInvalidParameter);
      writeGuestBE32(r4 + 4, 0u);
    }
    STUB_LOG("NtQueryInformationFile", r3, r4, r5, r6, r7, r8, kStatusInvalidParameter);
    return kStatusInvalidParameter;
  }

  // Preserve current position while probing size. iter-463 (f): under the
  // handle's I/O lock, so a concurrent NtReadFile never sees the SEEK_END.
  std::shared_ptr<std::mutex> ioQ = atomicReadOn() ? fileHandleIoLock(r3) : nullptr;
  std::unique_lock<std::mutex> ioQHold;
  if (ioQ) {
    ioQHold = std::unique_lock<std::mutex>(*ioQ);
    // Re-validate under the lock (file_io.hpp): an NtClose that won the lock
    // first has fclosed the FILE* fetched above.
    f = fileHandleGet(r3);
    if (!f) {
      if (r4) {
        writeGuestBE32(r4 + 0, kStatusInvalidHandle);
        writeGuestBE32(r4 + 4, 0u);
      }
      return kStatusInvalidHandle;
    }
  }
#if defined(_WIN32)
  __int64 cur = _ftelli64(f);
  (void)_fseeki64(f, 0, SEEK_END);
  __int64 end = _ftelli64(f);
  if (cur >= 0) (void)_fseeki64(f, cur, SEEK_SET);
  uint64_t fileSize = (end >= 0) ? static_cast<uint64_t>(end) : 0ull;
#else
  long cur = std::ftell(f);
  (void)std::fseek(f, 0, SEEK_END);
  long end = std::ftell(f);
  if (cur >= 0) (void)std::fseek(f, cur, SEEK_SET);
  uint64_t fileSize = (end >= 0) ? static_cast<uint64_t>(end) : 0ull;
#endif

  // FILE_NETWORK_OPEN_INFORMATION layout (56 bytes):
  // 0 CreationTime(8), 8 LastAccess(8), 16 LastWrite(8), 24 ChangeTime(8),
  // 32 AllocationSize(8), 40 EndOfFile(8), 48 FileAttributes(4), 52 Unknown/Pad(4)
  for (uint32_t i = 0; i < kFileNetworkOpenInfoSize; i += 4) writeGuestBE32(r5 + i, 0u);
  writeGuestBE32(r5 + 32, static_cast<uint32_t>(fileSize >> 32));
  writeGuestBE32(r5 + 36, static_cast<uint32_t>(fileSize & 0xFFFFFFFFu));
  writeGuestBE32(r5 + 40, static_cast<uint32_t>(fileSize >> 32));
  writeGuestBE32(r5 + 44, static_cast<uint32_t>(fileSize & 0xFFFFFFFFu));
  writeGuestBE32(r5 + 48, 0x00000080u);  // FILE_ATTRIBUTE_NORMAL

  if (r4) {
    writeGuestBE32(r4 + 0, kStatusSuccess);
    writeGuestBE32(r4 + 4, kFileNetworkOpenInfoSize);
  }

  STUB_LOG("NtQueryInformationFile", r3, r4, r5, r6, r7, r8, kStatusSuccess);
  return kStatusSuccess;
}

// NtSetInformationFile:
// r3=FileHandle, r4=PIO_STATUS_BLOCK, r5=FileInformation, r6=Length, r7=FileInformationClass
// We implement class 0x0E (FilePositionInformation) for archive seeking.
static uint32_t stub_NtSetInformationFile(uint32_t r3, uint32_t r4, uint32_t r5,
                                          uint32_t r6, uint32_t r7, uint32_t r8) {
  { uint32_t st; if (xamContentTrySetInformationFile(r3, r4, r5, r6, r7, r8, &st)) return st; }  // iter-463
  (void)r8;
  constexpr uint32_t kFilePositionInformation = 0x0Eu;

  std::FILE* f = fileHandleGet(r3);
  if (!f) {
    if (r4) {
      writeGuestBE32(r4 + 0, kStatusInvalidHandle);
      writeGuestBE32(r4 + 4, 0u);
    }
    STUB_LOG("NtSetInformationFile", r3, r4, r5, r6, r7, r8, kStatusInvalidHandle);
    return kStatusInvalidHandle;
  }
  if (r7 != kFilePositionInformation) {
    // For now we only need seek semantics; keep strict behavior for other classes.
    if (r4) {
      writeGuestBE32(r4 + 0, kStatusInvalidInfoClass);
      writeGuestBE32(r4 + 4, 0u);
    }
    STUB_LOG("NtSetInformationFile", r3, r4, r5, r6, r7, r8, kStatusInvalidInfoClass);
    return kStatusInvalidInfoClass;
  }
  if (r5 == 0 || r6 < 8 || !guestVAToHost(r5)) {
    if (r4) {
      writeGuestBE32(r4 + 0, kStatusInvalidParameter);
      writeGuestBE32(r4 + 4, 0u);
    }
    STUB_LOG("NtSetInformationFile", r3, r4, r5, r6, r7, r8, kStatusInvalidParameter);
    return kStatusInvalidParameter;
  }

  uint64_t newPos = (uint64_t(readGuestU32BE(r5)) << 32) | uint64_t(readGuestU32BE(r5 + 4));
  // iter-463 (f): the position move under the handle's I/O lock.
  std::shared_ptr<std::mutex> ioS = atomicReadOn() ? fileHandleIoLock(r3) : nullptr;
  std::unique_lock<std::mutex> ioSHold;
  if (ioS) {
    ioSHold = std::unique_lock<std::mutex>(*ioS);
    f = fileHandleGet(r3);  // re-validate under the lock (file_io.hpp)
    if (!f) {
      if (r4) {
        writeGuestBE32(r4 + 0, kStatusInvalidHandle);
        writeGuestBE32(r4 + 4, 0u);
      }
      return kStatusInvalidHandle;
    }
  }
#if defined(_WIN32)
  (void)_fseeki64(f, static_cast<__int64>(newPos), SEEK_SET);
#else
  (void)std::fseek(f, static_cast<long>(newPos), SEEK_SET);
#endif

  if (r4) {
    writeGuestBE32(r4 + 0, kStatusSuccess);
    writeGuestBE32(r4 + 4, 0u);
  }
  STUB_LOG("NtSetInformationFile", r3, r4, r5, r6, r7, r8, kStatusSuccess);
  return kStatusSuccess;
}

// NtQueryDirectoryFile (Xbox 360 syscall):
//   r3=FileHandle, r4=Event, r5=ApcRoutine, r6=ApcContext,
//   r7=IoStatusBlock, r8=FileInformation buffer,
//   r9=Length (buffer size), r10=FileInformationClass
// Xbox 360 FILE_DIRECTORY_INFORMATION layout (class 1, from Xenia):
//   0  NextEntryOffset (4)
//   4  FileIndex       (4)
//   8  CreationTime    (8)
//  16  LastAccessTime  (8)
//  24  LastWriteTime   (8)
//  32  ChangeTime      (8)
//  40  EndOfFile       (8)
//  48  AllocationSize  (8)
//  56  FileAttributes  (4)
//  60  FileNameLength  (4)
//  64  FileName[]      (ANSI, variable)
// Total header = 64 bytes.
// Per-handle directory enumeration state (supports iterative enumeration)
namespace {
  struct DirEnumState {
    std::vector<ModDirEntry> entries;  // iter-463: name/size/kind, so a mod overlay can union
    size_t nextIndex = 0;
    bool loaded = false;
    std::string rule;                  // iter-464 SR_NTQDIR_XENIA: the find rule (XFile find_engine_)
  };
  std::mutex g_dirEnumMutex;
  std::unordered_map<uint32_t, DirEnumState> g_dirEnumState;

  // The directory listing (disc order) with the mod overlay's union (iter-463).
  void dirListLoad(const std::string& dirPath, DirEnumState& state) {
    state.entries.clear();
    state.loaded = true;
    state.nextIndex = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::u8path(dirPath), ec)) {
      if (ec) break;
      ModDirEntry de;
      de.name = entry.path().filename().string();
      de.hostPath = modPathU8(entry.path());
      std::error_code ec2;
      if (entry.is_regular_file(ec2)) de.size = entry.file_size(ec2);
      de.isDir = entry.is_directory(ec2);
      state.entries.push_back(std::move(de));
    }
    modOverlayMergeDir(dirPath, state.entries);  // iter-463: no-op unless a mod is enabled
  }

  // The find rule as Xenia matches it (semantics of xenia base/filesystem_wildcard.cc
  // WildcardEngine, re-implemented): the pattern is cut at '*' and '?' into literal pieces,
  // matched case-insensitively left to right - the first piece anchored at the start unless
  // the pattern starts with a wildcard, the last anchored at the end unless it ends with '*',
  // each other piece found anywhere after the previous one; a run of '?' consumes exactly
  // that many characters. An empty pattern matches everything. NOTE: "*.*" therefore needs a
  // '.' in the name, as in Xenia; the game never sends it (FindFirstFileA, sub_8246A640, turns
  // a "*.*" tail into an EMPTY mask).
  bool dirWildcardMatch(const std::string& pattern, const std::string& name) {
    if (pattern.empty()) return true;
    auto low = [](std::string x) {
      for (char& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return x;
    };
    const std::string p = low(pattern), n = low(name);
    struct Piece { std::string lit; bool first = false, last = false, exact = false; };
    std::vector<Piece> rules;
    bool first = true;
    size_t at = 0, last = 0;
    while ((at = p.find_first_of("*?", last)) != std::string::npos) {
      if (last != at) rules.push_back({p.substr(last, at - last), first, false, false});
      if (p[at] == '?') {
        size_t end = p.find_first_not_of('?', at + 1);
        const size_t count = (end == std::string::npos ? p.size() : end) - at;
        rules.push_back({std::string(count, '?'), false, false, true});
        last = at + count;
      } else {
        last = at + 1;
      }
      first = false;
    }
    if (last != p.size()) rules.push_back({p.substr(last), last == 0, true, false});
    size_t off = 0;
    for (const Piece& r : rules) {
      if (r.lit.empty()) continue;
      if (n.size() - off < r.lit.size()) return false;
      if (r.exact) { off += r.lit.size(); continue; }
      const size_t hit = n.find(r.lit, off);
      if (hit == std::string::npos) return false;
      if (r.first && hit != off) return false;
      if (r.last && hit != n.size() - r.lit.size()) return false;
      off = hit + r.lit.size();
    }
    return true;
  }
}

static void ntQueryDirClose(uint32_t handle) {
  std::lock_guard<std::mutex> lock(g_dirEnumMutex);
  g_dirEnumState.erase(handle);
}

uint32_t ntQueryDirStateCountForTest() {
  std::lock_guard<std::mutex> lock(g_dirEnumMutex);
  return static_cast<uint32_t>(g_dirEnumState.size());
}

// iter-464 PLATFORM FIX SR_NTQDIR_XENIA (default ON after natural A/B; =0 legacy diagnostic):
// NtQueryDirectoryFile as Xenia implements it (xboxkrnl_io.cc NtQueryDirectoryFile_entry
// + xfile.cc XFile::QueryDirectory): ONE X_FILE_DIRECTORY_INFORMATION record per call
// (next_entry_offset 0), the FileName mask (r10, an X_ANSI_STRING) sets the find rule
// and restarts the scan, RestartScan (the 9th argument) restarts it, length < 72 ->
// STATUS_INFO_LENGTH_MISMATCH, no match for a new rule -> STATUS_NO_SUCH_FILE, the end
// -> STATUS_NO_MORE_FILES, a name that does not fit -> STATUS_NO_SUCH_FILE ("Buffer
// overflow?"), IOSB {status, length on success}. The old path packed as many records as
// fit and ignored the mask and RestartScan; the game's FindFirstFileA/FindNextFileA
// (sub_828A7D50 / sub_828A7C10) pass 328 bytes and convert only the FIRST record of each
// call (sub_828A8640 never reads NextEntryOffset), so it saw about one entry in four.
// Live 2026-10-02 A/B: DATA-root FileStorage registers 3/12 ASTs with the old path,
// 12/12 with this path, matching the Xenia sound-menu dump by all 12 asset IDs.
// See docs/MODS.md section 12. The old "4" counted the directory too, not 4 ASTs.
// kernel/xam_content.cpp's content-package listing already follows Xenia.
static std::atomic<int> g_ntQueryDirTestMode{-1};  // difftest seam: -1 env, 0 off, 1 on
static bool ntQueryDirXeniaOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NTQDIR_XENIA");
    return !e || e[0] != '0';
  }();
  const int m = g_ntQueryDirTestMode.load(std::memory_order_relaxed);
  return m < 0 ? v : m != 0;
}
void ntQueryDirXeniaSetForTest(int mode) { g_ntQueryDirTestMode.store(mode); }

static uint32_t ntQueryDirectoryXenia(uint32_t r3, uint32_t r7, uint32_t r8, const std::string& dirPath,
                                      DirEnumState& state) {
  constexpr uint32_t kInfoLengthMismatch = 0xC0000004u;
  constexpr uint32_t kNoSuchFile = 0xC000000Fu;
  constexpr uint32_t kNoMoreFiles = 0x80000006u;
  constexpr uint32_t kHeader = 64;
  const uint32_t len = ExportRegistry::getCurrentArgR9();
  const uint32_t maskVA = ExportRegistry::getCurrentArgR10();
  if (len < 72) return kInfoLengthMismatch;  // Xenia returns before touching the IOSB
  std::string mask;
  if (maskVA && isGuestVAInRange(maskVA) && isGuestVAInRange(maskVA + 7)) {
    const uint32_t w0 = readGuestU32BE(maskVA);       // X_ANSI_STRING {u16 length, u16 max}
    const uint32_t buf = readGuestU32BE(maskVA + 4);  // char* buffer
    const uint32_t mlen = w0 >> 16;
    if (mlen && buf && isGuestVAInRange(buf) && isGuestVAInRange(buf + mlen - 1)) {
      const uint8_t* h = guestVAToHost(buf);
      if (h) mask.assign(reinterpret_cast<const char*>(h), mlen);
    }
  }
  {  // Xenia xfile.h IsValidPath(name, is_pattern=true): ASCII only, none of " + , < > |, and a
     // '*' followed by anything but '.' is refused (Xenia's 4D530819 note). A '\' in a mask is
     // only an assert there; it then matches no child name, as here.
    bool gotAsterisk = false;
    for (char ch : mask) {
      const uint8_t c = static_cast<uint8_t>(ch);
      if (c <= 31 || c >= 127) return kStatusInvalidParameter;
      if (gotAsterisk) {
        if (ch != '.') return kStatusInvalidParameter;
        gotAsterisk = false;
      }
      if (ch == '"' || ch == '+' || ch == ',' || ch == '<' || ch == '>' || ch == '|') return kStatusInvalidParameter;
      if (ch == '*') gotAsterisk = true;
    }
  }
  const bool restart = ntStackArg9() != 0;
  uint32_t st;
  size_t found = static_cast<size_t>(-1);
  if (!mask.empty()) {
    state.rule = mask;  // a new rule always restarts (xfile.cc QueryDirectory)
    dirListLoad(dirPath, state);
    for (size_t i = 0; i < state.entries.size(); ++i)
      if (dirWildcardMatch(state.rule, state.entries[i].name)) { found = i; break; }
    if (found == static_cast<size_t>(-1)) state.nextIndex = state.entries.size();
    st = found == static_cast<size_t>(-1) ? kNoSuchFile : kStatusSuccess;
  } else {
    if (restart || !state.loaded) dirListLoad(dirPath, state);
    for (size_t i = state.nextIndex; i < state.entries.size(); ++i)
      if (dirWildcardMatch(state.rule, state.entries[i].name)) { found = i; break; }
    if (found == static_cast<size_t>(-1)) state.nextIndex = state.entries.size();
    st = found == static_cast<size_t>(-1) ? kNoMoreFiles : kStatusSuccess;
  }
  if (st == kStatusSuccess) {
    const ModDirEntry& e = state.entries[found];
    const uint32_t nameLen = static_cast<uint32_t>(e.name.size());
    state.nextIndex = found + 1;  // Xenia's IterateChildren advances find_index_ before the size check
    if (kHeader + nameLen > len || !guestVAToHost(r8) || !isGuestVAInRange(r8 + kHeader + nameLen)) {
      st = kNoSuchFile;
    } else {
      const uint64_t size = e.isDir ? 0 : e.size;
      writeGuestBE32(r8 + 0, 0u);                                      // next_entry_offset
      writeGuestBE32(r8 + 4, static_cast<uint32_t>(state.nextIndex));  // file_index (find_index_)
      for (uint32_t o = 8; o < 40; o += 4) writeGuestBE32(r8 + o, 0u);  // times (not tracked: 0)
      writeGuestBE32(r8 + 40, static_cast<uint32_t>(size >> 32));
      writeGuestBE32(r8 + 44, static_cast<uint32_t>(size));
      const uint64_t alloc = (size + 511) & ~uint64_t(511);
      writeGuestBE32(r8 + 48, static_cast<uint32_t>(alloc >> 32));
      writeGuestBE32(r8 + 52, static_cast<uint32_t>(alloc));
      writeGuestBE32(r8 + 56, e.isDir ? 0x10u : 0x80u);
      writeGuestBE32(r8 + 60, nameLen);
      uint8_t* nm = guestVAToHost(r8 + kHeader);
      for (uint32_t i = 0; i < nameLen; ++i) nm[i] = static_cast<uint8_t>(e.name[i]);
    }
  }
  if (r7) {
    writeGuestBE32(r7 + 0, st);
    writeGuestBE32(r7 + 4, st == kStatusSuccess ? len : 0u);
  }
  static std::atomic<int> s_log{0};
  if (s_log.fetch_add(1, std::memory_order_relaxed) < 400)
    SR_LOG_KERNEL_FILE("[NTQDIR] h=%u dir=\"%s\" mask='%s' restart=%d len=%u -> %08X entry='%s'", r3,
                       dirPath.c_str(), mask.c_str(), restart ? 1 : 0, len, st,
                       st == kStatusSuccess ? state.entries[found].name.c_str() : "");
  STUB_LOG("NtQueryDirectoryFile", r3, 0, 0, 0, r7, r8, st);
  return st;
}
static uint32_t stub_NtQueryDirectoryFile(uint32_t r3, uint32_t r4, uint32_t r5,
                                          uint32_t r6, uint32_t r7, uint32_t r8) {
  { uint32_t st; if (xamContentTryQueryDirectoryFile(r3, r4, r5, r6, r7, r8, &st)) return st; }  // iter-463
  (void)r4; (void)r5; (void)r6;
  constexpr uint32_t kStatusNoMoreFiles = 0x80000006u;
  constexpr uint32_t kHeaderSize = 64;  // header before FileName[]
  std::string dirPath = fileHandleGetDir(r3);
  if (dirPath.empty()) {
    if (ntQueryDirXeniaOn()) {
      constexpr uint32_t kStatusNoSuchFile = 0xC000000Fu;
      if (r7) {
        writeGuestBE32(r7 + 0, kStatusNoSuchFile);
        writeGuestBE32(r7 + 4, 0u);
      }
      STUB_LOG("NtQueryDirectoryFile", r3, r4, r5, r6, r7, r8, kStatusNoSuchFile);
      return kStatusNoSuchFile;
    }
    STUB_LOG("NtQueryDirectoryFile", r3, r4, r5, r6, r7, r8, 0xC0000008u);
    return 0xC0000008u;  // INVALID_HANDLE
  }

  std::lock_guard<std::mutex> lock(g_dirEnumMutex);
  auto& state = g_dirEnumState[r3];
  if (ntQueryDirXeniaOn()) return ntQueryDirectoryXenia(r3, r7, r8, dirPath, state);  // iter-464

  // Load directory entries on first call
  if (!state.loaded) dirListLoad(dirPath, state);

  // Check if all entries have been returned
  if (state.nextIndex >= state.entries.size()) {
    if (r7) { writeGuestBE32(r7 + 0, kStatusNoMoreFiles); writeGuestBE32(r7 + 4, 0u); }
    STUB_LOG("NtQueryDirectoryFile", r3, r4, r5, r6, r7, r8, kStatusNoMoreFiles);
    return kStatusNoMoreFiles;
  }

  // r8 = FileInformation buffer VA.
  // r9 = buffer length. Use it if valid, otherwise generous default.
  uint32_t bufVA = r8;
  uint32_t r9 = ExportRegistry::getCurrentArgR9();
  uint32_t bufLen = (r9 > 0 && r9 < 0x100000u) ? r9 : 8192u;
  uint32_t ioStatusVA = r7;
  uint32_t offset = 0;
  uint32_t entryCount = 0;

  while (state.nextIndex < state.entries.size()) {
    const ModDirEntry& entry = state.entries[state.nextIndex];
    std::string name = entry.name;
    if (name.size() > 255) name = name.substr(0, 255);
    uint32_t nameLen = static_cast<uint32_t>(name.size());
    uint32_t entrySize = kHeaderSize + ((nameLen + 3) & ~3u);
    if (offset + entrySize > bufLen) break;  // buffer full; next call gets remaining
    uint32_t va = bufVA + offset;
    uint32_t nextEntryOffset = entrySize;  // will fix up for last
    writeGuestBE32(va +  0, nextEntryOffset);
    writeGuestBE32(va +  4, static_cast<uint32_t>(state.nextIndex));  // FileIndex
    writeGuestBE32(va +  8, 0u);  writeGuestBE32(va + 12, 0u);  // CreationTime
    writeGuestBE32(va + 16, 0u);  writeGuestBE32(va + 20, 0u);  // LastAccessTime
    writeGuestBE32(va + 24, 0u);  writeGuestBE32(va + 28, 0u);  // LastWriteTime
    writeGuestBE32(va + 32, 0u);  writeGuestBE32(va + 36, 0u);  // ChangeTime
    uint64_t fileSize = entry.size;
    writeGuestBE32(va + 40, static_cast<uint32_t>(fileSize >> 32));
    writeGuestBE32(va + 44, static_cast<uint32_t>(fileSize & 0xFFFFFFFF));
    writeGuestBE32(va + 48, static_cast<uint32_t>(fileSize >> 32));  // AllocationSize
    writeGuestBE32(va + 52, static_cast<uint32_t>(fileSize & 0xFFFFFFFF));
    uint32_t attrs = entry.isDir ? 0x10u : 0x80u;
    writeGuestBE32(va + 56, attrs);
    writeGuestBE32(va + 60, nameLen);
    uint8_t* host = guestVAToHost(va + kHeaderSize);
    if (host) {
      for (size_t i = 0; i < name.size(); ++i) host[i] = static_cast<uint8_t>(name[i]);
      for (size_t i = name.size(); i < ((nameLen + 3u) & ~3u); ++i) host[i] = 0;
    }
    static std::atomic<int> s_entryLog{0};
    if (s_entryLog.fetch_add(1, std::memory_order_relaxed) < 30)
      SR_LOG_KERNEL_FILE("NtQueryDirectoryFile entry[%zu]: \"%s\" size=%llu attrs=0x%X",
        state.nextIndex, name.c_str(), fileSize, attrs);
    offset += entrySize;
    entryCount++;
    state.nextIndex++;
  }
  // Fix up last entry: NextEntryOffset = 0
  if (entryCount > 0) {
    uint32_t lastOffset = 0;
    uint32_t tmpOff = 0;
    for (uint32_t i = 0; i < entryCount; i++) {
      lastOffset = tmpOff;
      tmpOff += readGuestU32BE(bufVA + tmpOff);
    }
    writeGuestBE32(bufVA + lastOffset, 0u);
  }
  if (ioStatusVA) {
    writeGuestBE32(ioStatusVA + 0, kStatusSuccess);
    writeGuestBE32(ioStatusVA + 4, offset);
  }
  static std::atomic<int> s_queryDirLogCount{0};
  if (s_queryDirLogCount.fetch_add(1, std::memory_order_relaxed) < 10)
    SR_LOG_KERNEL_FILE("NtQueryDirectoryFile handle=%u dir=\"%s\" -> %u bytes %u entries (bufLen=%u, remaining=%zu)",
      r3, dirPath.c_str(), offset, entryCount, bufLen,
      state.entries.size() - state.nextIndex);
  STUB_LOG("NtQueryDirectoryFile", r3, r4, r5, r6, r7, r8, kStatusSuccess);
  return kStatusSuccess;
}

static uint64_t windowsFileTime() {
#ifdef _WIN32
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
#else
  using namespace std::chrono;
  auto now = system_clock::now().time_since_epoch();
  auto ticks100ns = duration_cast<duration<uint64_t, std::ratio<1, 10000000>>>(now).count();
  constexpr uint64_t kUnixToFileTimeOffset = 116444736000000000ULL;
  return ticks100ns + kUnixToFileTimeOffset;
#endif
}

uint32_t stub_KeQuerySystemTime(uint32_t r3, uint32_t r4, uint32_t r5,
                                uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4;
  (void)r5;
  (void)r6;
  (void)r7;
  (void)r8;
  // iter-463 SR_TIMEBASE_50MHZ: guest system time = FILETIME at the clock origin +
  // guest ticks/5 (Xenia Clock::QueryGuestSystemTime), and - as Xenia's
  // KeQuerySystemTime_entry does - stored back into KeTimeStampBundle.system_time so a
  // direct bundle read agrees with the call. Legacy: raw host FILETIME.
  uint64_t ft;
  if (sr::timebase::enabled()) {
    ft = sr::timebase::systemTime100ns(sr::timebase::guestTicks());
    if (srTimebaseBundleLive()) writeGuest64(sr::timebase::kBundleVA + 0x08, ft);
  } else {
    ft = windowsFileTime();
  }
  if (r3) writeGuest64(r3, ft);
  STUB_LOG("KeQuerySystemTime", r3, r4, r5, r6, r7, r8, kStatusSuccess);
  return kStatusSuccess;
}

// NtDuplicateObject: Xbox 360 signature (Xenia xboxkrnl_ob.cc):
//   r3 = handle       (source handle to duplicate; -2 = NtCurrentThread)
//   r4 = new_handle_ptr (guest VA to write the new handle)
//   r5 = options       (1 = DUPLICATE_CLOSE_SOURCE, 2 = DUPLICATE_SAME_ACCESS)
// Games use NtDuplicateObject(-2) to get a real handle to the current thread.
// The returned handle must match the one from ExCreateThread so allocator key
// lookups (sub_8278C840) find the right per-thread allocator.
static uint32_t stub_NtDuplicateObject(uint32_t r3, uint32_t r4, uint32_t r5,
                                       uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r5; (void)r6; (void)r7; (void)r8;
  uint32_t srcHandle = r3;
  uint32_t outPtr = r4;

  uint32_t newHandle = 0;
  if (srcHandle == 0xFFFFFFFE) {
    newHandle = sr::kernel::getCurrentThreadHandle();
    if (newHandle == 0) {
      newHandle = sr::kernel::allocUnifiedHandle();
      sr::kernel::setCurrentThreadHandle(newHandle);
    }
    // iter-462 (SR_OB_KTHREAD): the handle names this thread's KTHREAD. When the
    // number is already mapped (our -2 duplicate returns the thread's OWN handle)
    // this is one more open, so closing the "duplicate" keeps the thread's entry.
    if (obKThreadEnabled()) kthreadRegisterHandle(newHandle, kthreadCurrent(true));
  } else {
    newHandle = sr::kernel::allocUnifiedHandle();
    if (obKThreadEnabled()) kthreadAliasHandle(newHandle, srcHandle);
    // step4/audit SR_XNOTIFY_XENIA (Astra r2 over 170216cc..f22817d3), a RETAINED INCOMPATIBILITY: a duplicate of a
    // listener handle registers no alias (on Xenia it names the same listener; DUPLICATE_CLOSE_SOURCE is ignored here
    // too). The title's only DuplicateHandle caller (ThreadGetCurrentHandleCached sub_825F70C0) duplicates the
    // current-thread pseudo-handle, so this never fires on its routes; every occurrence is logged, unsampled.
    bool xnClosed = false;
    if (xnotifyIsListener(srcHandle, &xnClosed)) {
      static std::atomic<uint32_t> s_xnDup{0};
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[IMPORT_FIXES] XNOTIFY dup #%u of a listener: %08X%s -> %08X options=%08X, no alias (the copy names "
                  "nothing) lr=%08X",
                  s_xnDup.fetch_add(1, std::memory_order_relaxed) + 1, srcHandle, xnClosed ? " (closed)" : "",
                  newHandle, r5, getCurrentGuestLR());
    }
  }

  if (outPtr != 0) {
    writeGuest32(outPtr, newHandle);
  }
  SR_LOG_KERNEL_IMPORT("NtDuplicateObject(handle=0x%08X outPtr=0x%08X options=0x%08X) -> newHandle=0x%08X",
    srcHandle, outPtr, r5, newHandle);
  return kStatusSuccess;
}

// step4/audit (logic audit 9a row 6) SR_NT_EVENT_TYPE (PLATFORM FIX, BAKED default ON 2026-10-18; =0 = the old
// auto-reset, unsignalled events and the generic NtClearEvent):
// NtCreateEvent(handle_ptr r3, obj_attributes r4, event_type r5, initial_state r6) honours the event type -
// 0 = NotificationEvent (manual reset), 1 = SynchronizationEvent (auto reset) - and the initial state, as Xenia
// 0bd090dbe does: `ev->Initialize(!event_type, !!initial_state)` (xboxkrnl_threading.cc:622-653, xevent.cc:23-33).
// XAPI CreateEventA (sub_824693D8) passes type = !bManualReset; the XMV media stream (InitStreamBuffer
// 0x8248299C) asks for a manual-reset, initially SIGNALLED event, which the old path made auto-reset and
// unsignalled. With the gate NtClearEvent also resets the event (xboxkrnl_threading.cc:717-734; it was a
// generic stub). Not covered (as before): named events (r4's name is ignored) and the guest KEVENT object Xenia
// creates beside the host event. =0: the exact old bodies. Test: difftest --import-fixes-test (E*).
static bool ntEventTypeOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NT_EVENT_TYPE");
    return !e || e[0] != '0';
  }();
  return v;
}

// NtCreateEvent: r3 = PHANDLE (guest VA to write handle). Create host event, store in table, write handle to *r3.
static uint32_t stub_NtCreateEvent(uint32_t r3, uint32_t r4, uint32_t r5,
                                    uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4;
  (void)r5;
  (void)r6;
  (void)r7;
  (void)r8;
#ifdef _WIN32
  // SR_NT_EVENT_TYPE: manual reset = (event_type == 0), initial state = (initial_state != 0); off: auto, unset.
  const bool xeniaType = ntEventTypeOn();
  HANDLE h = CreateEventA(nullptr, xeniaType && r5 == 0 ? TRUE : FALSE, xeniaType && r6 != 0 ? TRUE : FALSE,
                          nullptr);
  if (!h) {
    STUB_LOG("NtCreateEvent", r3, r4, r5, r6, r7, r8, 0xC0000017u);  // STATUS_NO_MEMORY
    return 0xC0000017u;
  }
  uint32_t guestHandle = sr::kernel::allocUnifiedHandle();
  {
    std::lock_guard<std::mutex> lock(g_eventMutex);
    g_eventHandles[guestHandle] = h;
    if (xeniaType) g_eventTyped.insert(guestHandle);   // NtClearEvent's typed lookup (Astra P2 2)
  }
  writeGuestBE32(r3, guestHandle);
  if (xeniaType) {
    // The gate's own witness (bounded; IMPORT_FIXES is a player-log milestone tag): which events the title asks
    // for and from where (the movie stream's InitStreamBuffer_sub_824828F0 asks for manual reset, signalled).
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 64 || (n & (n - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[IMPORT_FIXES] NT_EVENT #%u NtCreateEvent type=%u initial=%u -> handle=%08X manual=%d "
                  "signalled=%d lr=%08X",
                  n, r5, r6, guestHandle, r5 == 0 ? 1 : 0, r6 != 0 ? 1 : 0, sr::kernel::getCurrentGuestLR());
  }
#else
  writeGuestBE32(r3, 0);  // No host event on non-Windows; wait/set remain no-ops.
#endif
  STUB_LOG("NtCreateEvent", r3, r4, r5, r6, r7, r8, kStatusSuccess);
  return kStatusSuccess;
}

// NtCreateSemaphore: create a real Win32 semaphore, store in Nt-level handle table.
// NTSTATUS NtCreateSemaphore(PHANDLE pHandle, POBJECT_ATTRIBUTES objAttr, LONG initialCount, LONG maxCount)
static uint32_t stub_NtCreateSemaphore(uint32_t r3, uint32_t r4, uint32_t r5,
                                        uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r7; (void)r8;
#ifdef _WIN32
  LONG initialCount = static_cast<LONG>(r5);
  LONG maxCount = static_cast<LONG>(r6);
  // iter-463 (i) SR_KESEM_STATUS: Xenia creates the host semaphore with the
  // guest's count/limit as given and fails with INVALID_PARAMETER (handle 0)
  // when that is impossible (xboxkrnl_threading.cc:785-792). =0: old coercion.
  const bool xeniaCreate = keSemStatusOn();
  if (!xeniaCreate) {
    if (maxCount <= 0) maxCount = 1024;  // safe default
    if (initialCount < 0) initialCount = 0;
    if (initialCount > maxCount) initialCount = maxCount;
  }
  HANDLE h = CreateSemaphoreA(nullptr, initialCount, maxCount, nullptr);
  if (!h) {
    const uint32_t st = xeniaCreate ? kStatusInvalidParameter : 0xC0000017u;  // else STATUS_NO_MEMORY
    if (xeniaCreate && r3 && guestVAToHost(r3)) writeGuestBE32(r3, 0u);
    SR_LOG_KERNEL_IMPORT("NtCreateSemaphore(pHandle=0x%08X init=%d max=%d) host create failed -> %08X",
                         r3, (int)initialCount, (int)maxCount, st);
    STUB_LOG("NtCreateSemaphore", r3, r4, r5, r6, r7, r8, st);
    return st;
  }
  uint32_t guestHandle = sr::kernel::allocUnifiedHandle();
  {
    std::lock_guard<std::mutex> lock(g_eventMutex);
    g_eventHandles[guestHandle] = h;
  }
  writeGuestBE32(r3, guestHandle);
  static int s_logCount = 0;
  if (++s_logCount <= 20)
    SR_LOG_KERNEL_IMPORT("NtCreateSemaphore(pHandle=0x%08X init=%u max=%u) -> handle=%u host=%p",
                         r3, r5, r6, guestHandle, (void*)h);

  // iter-463: DOGMA_AUTORELEASE (SR_DOGMA_AUTORELEASE, a host timer that
  // released every init=0/max=0x3FFFFFFF semaphore 500 ms after creation) is
  // DELETED. Those 12 semaphores are the EA::Thread::Condition benaphores
  // (Semaphore::Init 0x825F6C70 <- Condition::Init 0x825FBF30); the guest
  // releases them itself (Post 0x825F6D38 -> NtReleaseSemaphore when a waiter
  // is blocked). Retirement evidence: archive/v1_waitfix e1_off1..4 (4/4 OFF
  // boot rolls, all six intro movies, PRESS START, no 824BD7A0 skip storm, no
  // CS_WEDGE on 89678CE8) and long_off (26 min tutorial + free flight, RESULT
  // SUCCESS). docs/BANDAIDS.md.
#else
  writeGuestBE32(r3, 0);
#endif
  return kStatusSuccess;
}

// NtReleaseSemaphore: release a semaphore created by NtCreateSemaphore.
// NTSTATUS NtReleaseSemaphore(HANDLE handle, LONG releaseCount, PLONG previousCount)
static uint32_t stub_NtReleaseSemaphore(uint32_t r3, uint32_t r4, uint32_t r5,
                                         uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r6; (void)r7; (void)r8;
#ifdef _WIN32
  HANDLE h = syncHandleLookup(r3);
  if (semStatusOn()) {
    // iter-463 (e): Xenia NtReleaseSemaphore_entry (xboxkrnl_threading.cc:807-832).
    // The count is passed through unmodified; the host release failing (count
    // past the maximum, or count <= 0 -> ERROR_INVALID_PARAMETER) is
    // SEMAPHORE_LIMIT_EXCEEDED, a missing handle (or a host handle that is not
    // a semaphore) is INVALID_HANDLE. *previous_count is always written.
    uint32_t st = kStatusSuccess;
    LONG prevCount = 0;
    if (h) {
      if (!ReleaseSemaphore(h, static_cast<LONG>(r4), &prevCount)) {
        const DWORD err = GetLastError();
        prevCount = 0;
        st = (err == ERROR_INVALID_HANDLE) ? kStatusInvalidHandle : kStatusSemaphoreLimitExceeded;
      }
    } else {
      st = kStatusInvalidHandle;
    }
    if (r5 && guestVAToHost(r5)) writeGuestBE32(r5, static_cast<uint32_t>(prevCount));
    // Successes: the first 20 calls. Failures have their OWN counter (first 64,
    // then powers of two), so a failure late in a roll is never hidden behind
    // the early-boot success lines - a zero of [SEM_RELEASE_FAIL] is a real zero.
    static std::atomic<int> s_semLog{0};
    const int n = s_semLog.fetch_add(1, std::memory_order_relaxed);
    if (st == kStatusSuccess) {
      if (n < 20)
        SR_LOG_KERNEL_IMPORT("NtReleaseSemaphore(handle=0x%08X count=%u pPrev=0x%08X) host=%p prev=%ld -> %08X",
                             r3, r4, r5, (void*)h, prevCount, st);
    } else {
      static std::atomic<unsigned> s_semFail{0};
      const unsigned f = s_semFail.fetch_add(1, std::memory_order_relaxed) + 1;
      if (f <= 64 || (f & (f - 1)) == 0)
        SR_LOG_KERNEL_IMPORT("[SEM_RELEASE_FAIL] #%u NtReleaseSemaphore(handle=0x%08X count=%d) host=%p -> %08X tid=%u lr=%08X",
                             f, r3, (int)r4, (void*)h, st, sr::kernel::getCurrentThreadHandle(),
                             sr::kernel::getCurrentGuestLR());
    }
    return st;
  }
  if (h) {
    LONG releaseCount = static_cast<LONG>(r4);
    if (releaseCount <= 0) releaseCount = 1;
    LONG prevCount = 0;
    ReleaseSemaphore(h, releaseCount, &prevCount);
    if (r5) writeGuestBE32(r5, static_cast<uint32_t>(prevCount));
  }
  static int s_logCount = 0;
  if (++s_logCount <= 20)
    SR_LOG_KERNEL_IMPORT("NtReleaseSemaphore(handle=0x%08X count=%u pPrev=0x%08X) host=%p",
                         r3, r4, r5, (void*)h);
#endif
  return kStatusSuccess;
}

// NtWaitForSingleObjectEx: wait on a handle (event or semaphore) with timeout.
// NTSTATUS NtWaitForSingleObjectEx(HANDLE handle, KPROCESSOR_MODE waitMode, BOOLEAN alertable, PLARGE_INTEGER timeout)
static uint32_t stub_NtWaitForSingleObjectEx(uint32_t r3, uint32_t r4, uint32_t r5,
                                              uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r7; (void)r8;
  constexpr uint32_t kStatusTimeout = 258u;
  constexpr uint32_t kStatusUserApc = 0xC0u;
  {
    uint32_t tHandle = sr::kernel::getCurrentThreadHandle();
    if (tHandle <= 2) {
      static std::atomic<uint64_t> s_mainNtWaitLog{0};
      uint64_t n = s_mainNtWaitLog.fetch_add(1, std::memory_order_relaxed);
      if (n < 5 || (n + 1) % 10000 == 0)
        SR_LOG_KERNEL_IMPORT("[MAIN_NTWAIT] handle=%u r6=0x%08X call#%" PRIu64, r3, r6, n + 1);
    }
  }
#ifdef _WIN32
  // Auto-track: if the VdSwap thread does a TIMED wait on a semaphore, register it
  // as a GPU completion semaphore so the GPU interrupt thread can signal it.
  // iter-463 (g): FORCING (releases the guest never made) - opt-in only.
#if SR_ORACLE
  const bool gpuSemForcing = gpuSemAutotrackForcing();
#endif
  // iter-463 (b): the guest's alertable flag (r5), or the old always-alertable.
  const BOOL hostAlertable = waitAlertableOn() ? (r5 != 0 ? TRUE : FALSE) : TRUE;
#if SR_ORACLE
  if (gpuSemForcing && r6 != 0) {
    DWORD rtId = g_renderThreadNativeId.load(std::memory_order_relaxed);
    if (rtId != 0 && GetCurrentThreadId() == rtId) {
      if (!isGpuCompletionSemaphore(r3)) {
        HANDLE h = eventHandleGet(r3);
        if (h) trackGpuCompletionSemaphore(r3);
      }
    }
  }

#endif
  // Thread handle wait: Xbox 360 returns STATUS_USER_APC when an APC is delivered
  // to the target thread. On Windows, WaitForSingleObject on a thread handle waits
  // for thread termination, which is wrong. Use our per-thread completion event instead.
  if (isThreadHandle(r3)) {
    HANDLE evt = getThreadCompletionEvent(r3);
    if (evt && threadWaitStatusOn()) {
      // iter-463 (c): XObject::Wait on the thread object (xobject.cc:191-228):
      // exit -> STATUS_SUCCESS, timeout -> STATUS_TIMEOUT, a host APC during an
      // alertable wait -> STATUS_USER_APC. Full guest timeout, guest alertable.
      const DWORD ms = (r6 != 0) ? guestTimeoutToMs(r6) : INFINITE;
      const DWORD result = WaitForSingleObjectEx(evt, ms, hostAlertable);
      uint32_t st = kStatusTimeout;
      if (result == WAIT_OBJECT_0) st = kStatusSuccess;
      else if (result == WAIT_IO_COMPLETION) st = kStatusUserApc;
      else if (result == WAIT_FAILED) st = 0x00000080u;  // X_STATUS_ABANDONED_WAIT_0 (xobject.cc:224-226)
      static std::atomic<int> s_thWaitLog2{0};
      const int n = ++s_thWaitLog2;
      if (n <= 30 || (n % 1000 == 0))
        SR_LOG_KERNEL_IMPORT("[NtWait] thread handle=%u -> %08X ms=%lu alertable=%d (call#%d)",
                             r3, st, (unsigned long)ms, (int)hostAlertable, n);
      return st;
    }
    if (evt) {
      // Use a 5-second cap so E630 module-init callers (which retry on STATUS_TIMEOUT)
      // eventually give up rather than spin-looping. sub_82720650's wait is bypassed
      // entirely in ppc_recomp.99.cpp, so it never reaches here during game-loop ticks.
      DWORD ms = (r6 != 0) ? 5000 : INFINITE;
      DWORD result = WaitForSingleObject(evt, ms);
      static std::atomic<int> s_thWaitLog{0};
      int n = ++s_thWaitLog;
      if (n <= 30 || (n % 1000 == 0))
        SR_LOG_KERNEL_IMPORT("[NtWait] thread handle=%u completion=%s (call#%d)",
                             r3, result == WAIT_OBJECT_0 ? "signaled" : "timeout", n);
      return (result == WAIT_OBJECT_0) ? kStatusUserApc : kStatusTimeout;
    }
  }

#if SR_ORACLE
  // GPU completion semaphores: for POLLED waits (non-infinite), return SUCCESS immediately
  // to simulate instant GPU processing. The render thread polls with a timeout; without this,
  // the ~16ms poll interval causes 130ms stalls after ring buffer wraps.
  // For INFINITE waits (r6=0, used by the main thread's game loop), do a real wait so the
  // thread properly blocks until the render thread signals via VdSwap.
  if (gpuSemForcing && r6 != 0 && isGpuCompletionSemaphore(r3)) {
    HANDLE h = syncHandleLookup(r3);
    if (h) {
      ReleaseSemaphore(h, 1, nullptr);
      WaitForSingleObject(h, 0);
    }
    return kStatusSuccess;
  }

#endif

  HANDLE h = syncHandleLookup(r3);
  if (h) {
    if (auto hook = g_ntWaitAcquiredHookForTest.load(std::memory_order_acquire)) hook(r3);   // difftest N11 only
    DWORD ms = INFINITE;
    if (r6 != 0) {
      // r6 = PLARGE_INTEGER timeout (100ns units, negative = relative).
      // iter-463 (a): full timeout unless SR_WAIT_FULL_TIMEOUT=0 (5000 ms cap).
      ms = guestTimeoutToMs(r6);
    } else {
      // r6 == 0 → INFINITE wait. The game's condvar protocol requires that
      // INFINITE waits truly block: returning STATUS_TIMEOUT corrupts the condvar's
      // spurious wakeup counter, causing the signal function to skip semaphore releases.
      // Use internal polling so we can still process APCs and avoid permanent hangs.
      //
      // /loop iter-21: FMGameRtamIni's CTOR (sub_8218F068 -> ... -> sub_8246A3D8)
      // creates a semaphore init=0 and waits infinite, but the worker that should
      // signal it sometimes never does (race-flakey, 2/3 hang). When we're on the
      // MAIN thread and the wait has blocked for >25s, escape with STATUS_USER_APC.
      // The guest's wait wrapper sub_8246A3D8 only loops on STATUS_ALERTED (0x101),
      // so USER_APC (0xC0) propagates to its caller cleanly. USER_APC doesn't
      // corrupt the condvar wakeup counter the way TIMEOUT does (per the existing
      // comment) — workers/condvars treat APC as a spurious wakeup, not a state
      // corruption. Only apply on main thread (FM ctor walker).
      // iter-463 (d) SR_NTWAIT_ESCAPE_SCOPE: forcing only, real main thread only.
      // Forcing either way: only behind the master gate (charter rule 1);
      // SR_NTWAIT_ESCAPE_SCOPE=0 restores the old handle<=2 scope inside it.
      const bool isMainThread = sr::kernel::forcingAllowed() &&
          (ntWaitEscapeScopeOn() ? sr::kernel::isMainGuestThread()
                                 : (sr::kernel::getCurrentThreadHandle() <= 2));
      // /loop iter-23: reduced escape time from 25000ms to 8000ms. Healthy
      // condvar waits should signal in milliseconds; 8s is plenty of slack
      // for legitimate slow workers but cuts each "fake escape" cycle's
      // dwell time by 3x. iter-21's 25s let walker reach END=46 in 1/3 runs
      // but spent most of the 90s budget in the wait. Shorter escape gives
      // the polling loop more chances to find data ready.
      const DWORD kEscapeMs = 8000;
      const auto startTime = GetTickCount64();

      static std::atomic<int> s_infWaitLog{0};
      int n = ++s_infWaitLog;
      if (n <= 20 || (n % 500 == 0))
        SR_LOG_KERNEL_IMPORT("[NtWait] INFINITE wait on handle=%u host=%p (blocking, call#%d)",
                             r3, (void*)h, n);
      for (;;) {
        DWORD result = WaitForSingleObjectEx(h, 200, hostAlertable);
        if (result == WAIT_OBJECT_0) {
          static int s_sigCount = 0;
          if (++s_sigCount <= 50 || s_sigCount % 500 == 0)
            SR_LOG_KERNEL_IMPORT("[NtWait] INFINITE handle=%u signaled (call#%d)", r3, s_sigCount);
          return kStatusSuccess;
        }
        if (result == WAIT_IO_COMPLETION) {
          return kStatusUserApc;
        }
        if (result == WAIT_FAILED) {
          return kStatusTimeout;
        }
        if (isMainThread && (GetTickCount64() - startTime) > kEscapeMs) {
          static std::atomic<int> s_escapeCount{0};
          int en = ++s_escapeCount;
          // iter-21: return STATUS_USER_APC after 25s blocking. iter-22 tried
          // STATUS_SUCCESS but that crashed (the guest treats SUCCESS as
          // "data is now valid" and dereferences uninitialized memory →
          // 0xC0000005 in KeTlsGetValue). USER_APC is safer: guest's wait
          // wrapper exits but caller's downstream code typically re-enters
          // the wait state rather than dereferencing protected data.
          if (en <= 20)
            SR_LOG_KERNEL_IMPORT("[NtWait] MAIN thread infinite-wait escape after %ums on handle=%u host=%p -> STATUS_USER_APC (escape#%d)",
                                 kEscapeMs, r3, (void*)h, en);
          return kStatusUserApc;
        }
      }
    }
    DWORD result = WaitForSingleObjectEx(h, ms, hostAlertable);
    if (result == WAIT_TIMEOUT) {
      static int s_logCount = 0;
      if (++s_logCount <= 50)
        STUB_LOG("NtWaitForSingleObjectEx(timeout)", r3, r4, r5, r6, r7, r8, kStatusTimeout);
      return kStatusTimeout;
    }
    if (result == WAIT_IO_COMPLETION) {
      static int s_apcCount = 0;
      if (++s_apcCount <= 50)
        SR_LOG_KERNEL_IMPORT("[NtWait] APC woke handle=%u (WAIT_IO_COMPLETION->0xC0, call#%d)", r3, s_apcCount);
      return kStatusUserApc;
    }
    static int s_sigCount = 0;
    if (++s_sigCount <= 50)
      STUB_LOG("NtWaitForSingleObjectEx(signaled)", r3, r4, r5, r6, r7, r8, kStatusSuccess);
    return kStatusSuccess;
  }
  // No handle found
  {
    // step4/audit SR_XNOTIFY_XENIA (Astra r2): a closed listener handle names no object any more, and Xenia's
    // NtWaitForSingleObjectEx answers X_STATUS_INVALID_HANDLE when its lookup fails (xboxkrnl_threading.cc:1024-1040).
    // Other unknown handles keep the old answers below.
    bool xnClosed = false;
    if (xnotifyIsListener(r3, &xnClosed) && xnClosed) return kStatusInvalidHandle;
  }
  if (r6 != 0) {
    Sleep(1);  // avoid busy-spin
    static int s_noHandleCount = 0;
    if (++s_noHandleCount <= 50)
      STUB_LOG("NtWaitForSingleObjectEx(no-handle)", r3, r4, r5, r6, r7, r8, kStatusTimeout);
    return kStatusTimeout;
  }
  // r6 == 0 means infinite wait on Xbox 360. Handle not found → can't wait.
  // Sleep longer (16ms) to avoid busy-spin when workers poll with handle=0.
  {
    static std::atomic<int> s_noHandleInfCount{0};
    int n = ++s_noHandleInfCount;
    if (n <= 20 || (n % 10000 == 0))
      SR_LOG_KERNEL_IMPORT("[NtWait] no handle for r3=0x%08X r6=0 infinite-wait (call#%d)", r3, n);
    Sleep(16);
  }
  return kStatusTimeout;
#endif
  return kStatusSuccess;
}

// KeInitializeSemaphore: create a real Win32 semaphore associated with a guest VA.
// VOID KeInitializeSemaphore(PKSEMAPHORE semaphore, LONG count, LONG limit)
static uint32_t stub_KeInitializeSemaphore(uint32_t r3, uint32_t r4, uint32_t r5,
                                            uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r6; (void)r7; (void)r8;
  // r3 = PKSEMAPHORE (guest VA)
  // r4 = Count (initial count)
  // r5 = Limit (maximum count)
#ifdef _WIN32
  LONG initialCount = static_cast<LONG>(r4);
  LONG maxCount = static_cast<LONG>(r5);
  if (maxCount <= 0) maxCount = 1024;
  if (initialCount < 0) initialCount = 0;
  if (initialCount > maxCount) initialCount = maxCount;
  HANDLE h = CreateSemaphoreA(nullptr, initialCount, maxCount, nullptr);
  if (h) keSyncHandleSet(r3, h);
  // iter-399: write the guest KSEMAPHORE header (type 5) so keGuestTruthProbe
  // never misclassifies a garbage-headed semaphore as an event.
  keWriteGuestDispatcherHeader(r3, 5u, static_cast<uint32_t>(initialCount));
  static int s_logCount = 0;
  if (++s_logCount <= 20)
    SR_LOG_KERNEL_IMPORT("KeInitializeSemaphore(obj=0x%08X count=%u limit=%u) -> host=%p",
                         r3, r4, r5, (void*)h);
#endif
  return kStatusSuccess;
}

// KeReleaseSemaphore: release a semaphore created by KeInitializeSemaphore.
// LONG KeReleaseSemaphore(PKSEMAPHORE semaphore, KPRIORITY increment, LONG adjustment, BOOLEAN wait)
static uint32_t stub_KeReleaseSemaphore(uint32_t r3, uint32_t r4, uint32_t r5,
                                         uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r7; (void)r8;
  // r3 = PKSEMAPHORE (guest VA)
  // r4 = Increment (priority boost, ignored here)
  // r5 = Adjustment (release count, typically 1)
  // r6 = Wait (if TRUE, caller will immediately wait)
#ifdef _WIN32
  HANDLE h = keSyncHandleGet(r3);
  if (!h) h = syncHandleLookup(r3);  // fallback: try all tables
  if (keSemStatusOn()) {
    // iter-463 (i): xeKeReleaseSemaphore (xboxkrnl_threading.cc:743-758) - the
    // adjustment goes to the host release unmodified (<= 0 or past the limit
    // fails and adds nothing), and the return value is the previous count.
    LONG prevCount = 0;
    if (h && !ReleaseSemaphore(h, static_cast<LONG>(r5), &prevCount)) {
      prevCount = 0;
      static std::atomic<unsigned> s_keFail{0};
      const unsigned f = s_keFail.fetch_add(1, std::memory_order_relaxed) + 1;
      if (f <= 64 || (f & (f - 1)) == 0)
        SR_LOG_KERNEL_IMPORT("[SEM_RELEASE_FAIL] #%u KeReleaseSemaphore(obj=0x%08X adj=%d) host=%p err=%lu tid=%u lr=%08X",
                             f, r3, (int)r5, (void*)h, (unsigned long)GetLastError(),
                             sr::kernel::getCurrentThreadHandle(), sr::kernel::getCurrentGuestLR());
    }
    static int s_logCount2 = 0;
    if (++s_logCount2 <= 20)
      SR_LOG_KERNEL_IMPORT("KeReleaseSemaphore(obj=0x%08X incr=%u adj=%u wait=%u) host=%p prev=%ld",
                           r3, r4, r5, r6, (void*)h, prevCount);
    return static_cast<uint32_t>(prevCount);
  }
  if (h) {
    LONG releaseCount = static_cast<LONG>(r5);
    if (releaseCount <= 0) releaseCount = 1;
    ReleaseSemaphore(h, releaseCount, nullptr);
  }
  static int s_logCount = 0;
  if (++s_logCount <= 20)
    SR_LOG_KERNEL_IMPORT("KeReleaseSemaphore(obj=0x%08X incr=%u adj=%u wait=%u) host=%p",
                         r3, r4, r5, r6, (void*)h);
#endif
  return kStatusSuccess;
}

// Generic stub: log and return 0 (success). Use for exports we don't implement yet.
[[maybe_unused]] static uint32_t stub_generic_success(uint32_t r3, uint32_t r4, uint32_t r5,
                                     uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r3;
  (void)r4;
  (void)r5;
  (void)r6;
  (void)r7;
  (void)r8;
  return kStatusSuccess;
}

// iter-318 STUB CENSUS: XamGetSystemVersion (returned 0, silently killed the
// whole FE transition) proved the class "load-bearing generic-success stub"
// exists — and KeTimeStampBundle was the same story. Every generic stub now
// logs name + caller lr + first args (calls 1-3 and powers of 2), so one
// batch yields a census of every import the boot actually consults, to be
// cross-checked against what the callers branch on. SR_STUB_CENSUS=0 mutes.
// iter-348 (charter platform fix): honest no-client socket semantics. On a
// retail console with no debug PC attached, accept/recv/connect fail with
// SOCKET_ERROR (-1) and WSAGetLastError() = WSAEWOULDBLOCK. The old
// return-0 stubs faked a connected client for the game's port-9201 debug
// service, whose packet handlers then parsed untouched buffers as commands
// (the .data-wipe root). See registerStubs() NetDll block for the full story.
static uint32_t stub_NetDll_fail_socket_error(uint32_t r3, uint32_t r4,
                                              uint32_t r5, uint32_t r6,
                                              uint32_t r7, uint32_t r8) {
  (void)r3; (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  static std::atomic<uint32_t> s_n{0};
  uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 4 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[NETDLL] no-client fail (-1) #%u lr=0x%08X", n, getCurrentGuestLR());
  // Console accept()/recv() on a blocking socket WAITS when no client is
  // attached; an instant -1 turned the debug-service loop into a hot spin
  // (~250 Hz through the handler). Brief sleep = honest blocking-ish pacing.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return 0xFFFFFFFFu;  // SOCKET_ERROR / INVALID_SOCKET (no client attached)
}
static uint32_t stub_NetDll_WSAGetLastError(uint32_t, uint32_t, uint32_t,
                                            uint32_t, uint32_t, uint32_t) {
  return 10035;  // WSAEWOULDBLOCK
}

static bool stubCensusOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_STUB_CENSUS");
    return !e || e[0] != '0';
  }();
  return v;
}
#define REG_GENERIC_STUB(nm)                                                   \
  reg.registerExport(nm, +[](uint32_t r3, uint32_t r4, uint32_t r5, uint32_t, \
                             uint32_t, uint32_t) -> uint32_t {                 \
    static std::atomic<uint32_t> s_n{0};                                       \
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;        \
    if (stubCensusOn() && (n <= 3 || (n & (n - 1)) == 0))                      \
      SR_LOG_BOOT(sr::LogSeverity::Info,                                       \
                  "[STUB_CENSUS] " nm " #%u lr=%08X r3=%08X r4=%08X r5=%08X",  \
                  n, sr::kernel::getCurrentGuestLR(), r3, r4, r5);             \
    return kStatusSuccess;                                                     \
  })

// --- XMA stubs (fake playback complete) ---
// The 16 legacy XMA fakes live in kernel/apu/xma_legacy_exports.cpp (C1, docs/RELEASE_COMPOSITION.md 2.6; Astra round 1):
// the SR_XMA_NEW=0 arm of the audio A/B, an EMULATION_SOURCES unit of the dev build and difftest, registered in
// registerStubs below under SR_ORACLE and replaced by the native decoder's exports when the gate is on.

// Returns TRUE (1) — for functions that return BOOL where 0 means failure.
static uint32_t stub_return_true(uint32_t r3, uint32_t r4, uint32_t r5,
                                 uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r3; (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  return 1;
}

// ExTerminateThread: terminates the calling thread.  On Xbox 360 this never returns.
// Run cleanup (release held CSes, signal completion event) before exiting so the main
// thread doesn't deadlock waiting on a CS or completion event held by this dying thread.
static uint32_t stub_ExTerminateThread(uint32_t r3, uint32_t r4, uint32_t r5,
                                        uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  uint32_t gHandle = sr::kernel::getCurrentThreadHandle();
  // iter-434: log the guest CALLER + a bounded stack chain — the preload
  // conveyor/FE-driver thread self-terminates ~2.5s after the last dispatch
  // (deterministic; the deferred re-posts land invisible to its exit check).
  // The lr names the guest exit-decision site.
  {
    uint32_t glr = sr::kernel::getCurrentGuestPC();
    uint32_t gr1 = sr::kernel::getCurrentGuestR1();
    char chain[200]; int cn = 0; chain[0] = 0;
    uint8_t* gbase = nullptr; size_t gsize = 0;
    getGlobalGuestMemory(&gbase, &gsize);
    if (gbase && gr1 >= 0x40000000u && (size_t)gr1 + 0x400 < gsize) {
      int words = 10;
      for (uint32_t o = 0; o < 0x300 && words > 0; o += 4) {
        const uint8_t* p = gbase + gr1 + o;
        uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                     (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        if (v >= 0x82000000u && v < 0x82990000u) {
          int w = snprintf(chain + cn, sizeof(chain) - cn, " %08X", v);
          if (w <= 0 || cn + w >= (int)sizeof(chain) - 1) break;
          cn += w; --words;
        }
      }
    }
    SR_LOG_KERNEL_IMPORT(
        "[THREAD] ExTerminateThread(exitCode=0x%08X) handle=%u lr=%08X r1=%08X chain:%s",
        r3, gHandle, glr, gr1, chain[0] ? chain : " -");
  }
  // iter-462 (SR_OB_KTHREAD): exit_status + signal_state on this thread's KTHREAD
  // (Xenia XThread::Exit) so GetExitCodeThread reports the exit code.
  if (obKThreadEnabled()) kthreadMarkExited(r3);
  releaseDeadThreadCSes(gHandle);
  signalThreadCompletion(gHandle);
  // iter-465 SR_THREAD_MEM_XENIA: the guest is done with its stack (nothing below
  // touches guest memory before the host thread ends).
  sr::kernel::threadmem::releaseCurrentStack();
#if defined(_WIN32)
  ExitThread(r3);
#else
  pthread_exit(nullptr);
#endif
  return 0;  // never reached
}

// NtYieldExecution: Xbox 360 kernel API to yield the current thread's time slice.
// On Xbox 360 this is a fast syscall that yields to another hardware thread.
// We call SwitchToThread() so we actually yield, and throttle logging to avoid flood.
static uint32_t stub_NtYieldExecution(uint32_t r3, uint32_t r4, uint32_t r5,
                                      uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r3; (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  static thread_local uint64_t s_yieldCount = 0;
  s_yieldCount++;
  // Log only at powers of 10 to avoid flood
  if (s_yieldCount == 1 || s_yieldCount == 100 || s_yieldCount == 10000 ||
      s_yieldCount == 1000000 || s_yieldCount == 10000000) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[KERNEL] NtYieldExecution count=%llu", (unsigned long long)s_yieldCount);
  }
  // iter-375 PLATFORM FIX: yield ALERTABLY.
  //
  // On the 360 this is a scheduler yield point, and the kernel delivers pending
  // APCs across it. On Windows, SwitchToThread() does NOT — APCs are delivered
  // only at an alertable wait. So every APC queued by NtQueueApcThread (the
  // condvar-signal path, VdRetrainEDRAM -> cmd43) could NEVER fire while a thread
  // sat in a yield loop, which is most of the time: the game-loop runners take the
  // yield arm of IdleYieldOrTimedWait_sub_825F7168 on every iteration
  // (dword_8208CA44 is statically 0, IDA-verified iter-366).
  //
  // That gap is what the ungated hand patch in guest/ppc/ppc_recomp.72.cpp was
  // compensating for — it fabricated a semaphore and substituted SleepEx(1, TRUE)
  // to buy an alertable window. Implementing the yield correctly here removes the
  // reason that forcing existed. SleepEx(0, TRUE) yields the remainder of the time
  // slice AND drains the APC queue, which is the console's semantics.
  //
  // ⚠️ MEASURED REGRESSION — default OFF pending attribution (iter-375).
  // Landed together with the pristine restore of sub_825F7168, and the pair took
  // feTick 18/19 -> 2 (K=2 300s). Two changes at once, so neither is attributed
  // yet; SR_YIELD_ALERTABLE=1 isolates this half. The pristine restore itself is
  // NOT optional (it deletes ungated forcing) and stays in regardless.
  // Suspicion to test: the four game-loop runners hit this every iteration, so an
  // alertable yield may be draining APCs far more often than the console does, or
  // simply costing more than SwitchToThread at that rate.
  static const bool alertable = [] {
    const char* e = std::getenv("SR_YIELD_ALERTABLE");
    return e && e[0] == '1';
  }();
#ifdef _WIN32
  if (alertable) SleepEx(0, TRUE); else SwitchToThread();
#else
  SwitchToThread();
#endif
  return kStatusSuccess;
}

static void CALLBACK noop_apc_callback(ULONG_PTR) {}

// NtQueueApcThread: queue a user-mode APC to a target thread.
// On Xbox 360, this is used by the condvar signal (sub_825FC2D0) to wake a thread
// from its wait. The APC routine itself is often a no-op (blr); the point is to
// interrupt the target thread's wait so it re-checks its work queue.
// Xbox 360 signature: NtQueueApcThread(ThreadHandle, ApcRoutine, Arg1, Arg2, Arg3)
static uint32_t stub_NtQueueApcThread(uint32_t r3, uint32_t r4, uint32_t r5,
                                      uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
#ifdef _WIN32
  HANDLE hostThread = eventHandleGet(r3);
  if (hostThread) {
    DWORD queued = QueueUserAPC(noop_apc_callback, hostThread, 0);
    // NOTE: do NOT call signalThreadCompletion here — completion should be signaled
    // when the worker thread actually finishes its work and exits (ExCreateThread
    // signals on thread exit). Signaling immediately was causing sub_825F71F8 to wake
    // too early and clear world_struct[0] before any work was done.
    static std::atomic<int> s_logCount{0};
    int n = ++s_logCount;
    if (n <= 30 || (n % 1000 == 0))
      SR_LOG_KERNEL_IMPORT("[NtQueueApcThread] handle=%u host=%p apcRoutine=0x%08X queued=%lu (call#%d)",
                           r3, (void*)hostThread, r4, queued, n);
  } else {
    static std::atomic<int> s_missCount{0};
    int n = ++s_missCount;
    if (n <= 20)
      SR_LOG_KERNEL_IMPORT("[NtQueueApcThread] handle=%u NOT FOUND (call#%d)", r3, n);
  }
#endif
  return kStatusSuccess;
}

static bool xapiThreadStartupOn();   // step4/audit SR_XAPI_THREAD_STARTUP (defined with ExCreateThread)

// File-scope thread arg struct used by guestThreadFunc (can't use __try/__except in lambdas).
struct GuestThreadArgs {
  ExCreateThreadRunnerFn fn;
  uint32_t startVA, startParam, stackVA, guestHandle;
  uint8_t* b;
  size_t sz;
  uint32_t kt = 0;   // iter-463 SR_KT_BIND_VA: the KTHREAD ExCreateThread built
  uint32_t stackAlloc = 0;   // iter-465 SR_THREAD_MEM_XENIA: released when the thread ends
  uint32_t startR4 = 0;      // step4/audit SR_XAPI_THREAD_STARTUP: r4 of the entry call (the start context)
  uint32_t logStartVA = 0;   // the thread's own start (r7) for the [THREAD] lines when the entry is the startup
};

// Worker thread body with SEH so stack overflow / other fatal exceptions don't
// abandon held CSes and deadlock the main thread permanently.
static DWORD WINAPI guestThreadFunc(LPVOID p) {
  GuestThreadArgs* a = static_cast<GuestThreadArgs*>(p);
  uint32_t gHandle = a->guestHandle;
  uint32_t sVA = a->logStartVA ? a->logStartVA : a->startVA;
  sr::kernel::setCurrentThreadHandle(gHandle);
  // iter-462 (SR_OB_KTHREAD): this host thread IS the KTHREAD ExCreateThread built.
  // iter-463 PLATFORM FIX (SR_KT_BIND_VA, default ON; =0 = the handle lookup):
  // bind by the block itself. A thread created running (flags 0) often starts
  // AFTER its creator closed the only handle (workers 0x82566760 /
  // 0x825671A0: "close ... last handle of a LIVE thread" precedes STARTING), so
  // the handle lookup missed and the thread ran on an anonymous KTHREAD with a
  // second, round-robin KPCR (cpu 3/4; Xenia's live KPCRs show both on cpu 0,
  // inherited from main) - r13 named a KTHREAD the guest never saw, and the
  // real block was never marked exited.
  if (obKThreadEnabled()) {
    if (a->kt && kthreadBindByVAEnabled()) kthreadBindCurrentVA(a->kt);
    else kthreadBindCurrent(gHandle);
  }
  // iter-465 SR_THREAD_MEM_XENIA: this thread's guest stack goes back when it ends
  // (below, or in ExTerminateThread, which never returns here).
  sr::kernel::threadmem::bindCurrentStack(a->stackAlloc);
  // Reserve 64 KB so the exception handler has stack even after a stack overflow.
  ULONG stackGuarantee = 64 * 1024;
  SetThreadStackGuarantee(&stackGuarantee);
  SR_LOG_KERNEL_IMPORT("[THREAD] handle=%u startVA=0x%08X STARTING", gHandle, sVA);
  // step4/audit SR_XAPI_THREAD_STARTUP (Astra batch-2 P3): through XapiThreadStartup a worker that returns ends in
  // ExTerminateThread, which never comes back here, so `delete a` below would be skipped. With the gate on the
  // arguments are copied and released before the guest runs; the old path keeps its order exactly.
  GuestThreadArgs local = *a;
  if (xapiThreadStartupOn()) { delete a; a = &local; }
  __try {
    sr::kernel::setThreadStartR4(a->startR4);   // step4/audit SR_XAPI_THREAD_STARTUP (0: the old call)
    a->fn(a->startVA, a->startParam, a->stackVA, a->b, a->sz);
    SR_LOG_KERNEL_IMPORT("[THREAD] handle=%u startVA=0x%08X EXITED normally", gHandle, sVA);
    // The runner calls the entry directly (no xapi startup), so a normal
    // return exits with code 0. ExTerminateThread exits never reach here.
    if (obKThreadEnabled()) kthreadMarkExited(0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    DWORD code = GetExceptionCode();
    if (code == 0xC00000FDu) _resetstkoflw();  // restore guard page after stack overflow
    SR_LOG_KERNEL_IMPORT("[THREAD] handle=%u startVA=0x%08X EXCEPTION 0x%08X, cleaning up abandoned CSes", gHandle, sVA, (unsigned)code);
    if (obKThreadEnabled()) kthreadMarkExited((uint32_t)code);
    releaseDeadThreadCSes(gHandle);
  }
  signalThreadCompletion(gHandle);
  sr::kernel::threadmem::releaseCurrentStack();   // iter-465 (Xenia XThread::FreeStack)
  if (a != &local) delete a;
  return 0;
}

// iter-30 walker-phase worker deferral. See guest_context.hpp for design rationale.
// Workers spawning startVA=0x827A7420 during the walker phase race the main
// thread inside sub_82182700; defer them until walker WALK_END.
constexpr uint32_t kDogmaWorkerStartVA = 0x827A7420u;

namespace {
struct DeferredWorker {
  GuestThreadArgs args;  // owned (will be passed to CreateThread)
};
std::atomic<bool> g_walkerPhaseActive{false};
std::mutex g_deferredWorkersMutex;
std::vector<DeferredWorker> g_deferredWorkers;
}  // namespace

void sr::kernel::beginWalkerPhase() {
  g_walkerPhaseActive.store(true, std::memory_order_release);
  SR_LOG_KERNEL_IMPORT("[EX_DEFER] walker phase begin -- deferring 0x827A7420 worker spawns");
}
bool sr::kernel::isWalkerPhaseActive() {
  return g_walkerPhaseActive.load(std::memory_order_acquire);
}
void sr::kernel::endWalkerPhase() {
  // Drain the queue under the lock, then start each thread outside.
  std::vector<DeferredWorker> drained;
  {
    std::lock_guard<std::mutex> lk(g_deferredWorkersMutex);
    drained.swap(g_deferredWorkers);
  }
  g_walkerPhaseActive.store(false, std::memory_order_release);
  SR_LOG_KERNEL_IMPORT("[EX_DEFER] walker phase end -- draining %zu deferred workers", drained.size());
  for (auto& dw : drained) {
    auto* args = new GuestThreadArgs(dw.args);
    constexpr SIZE_T kHostStackSize = 8 * 1024 * 1024;
    HANDLE h = CreateThread(nullptr, kHostStackSize, guestThreadFunc, args, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (h) {
      std::lock_guard<std::mutex> lock(g_eventMutex);
      g_eventHandles[args->guestHandle] = h;
    }
    SR_LOG_KERNEL_IMPORT("[EX_DEFER] drain spawn: handle=%u startVA=0x%08X hostHandle=%p",
                         args->guestHandle, args->startVA, (void*)h);
  }
}

// iter-43: forward decl — defined in kernel/fm_world_ptr_patcher.cpp.
// (No namespace wrapper needed: stubs.cpp is already inside namespace sr::kernel.)
void notifyFMWorkerCreated(uint32_t contextVA, uint32_t guestHandle);

// ExCreateThread: Xbox 360 kernel API
// NTSTATUS ExCreateThread(
//   PHANDLE pHandle,               // r3 - output handle pointer
//   DWORD   dwStackSize,           // r4 - stack size (0 = default)
//   PDWORD  pThreadId,             // r5 - output thread ID pointer (can be 0)
//   PVOID   pApiThreadStartup,     // r6 - xapi thread startup routine (can be 0)
//   LPTHREAD_START_ROUTINE pStartAddress, // r7 - actual thread entry point
//   PVOID   pStartContext,         // r8 - context passed to thread
//   DWORD   dwCreationFlagsMask   // r9 - creation flags (from PPCContext)
// );
// Note: r9 comes from ctx.r9 but our stub only receives r3-r8. We read r9 via stack/context.
// step4/audit (logic audit 9a row 8) SR_XAPI_THREAD_STARTUP (PLATFORM FIX, baked default ON 2026-10-20; =0 = the old
// entry, docs/LOGIC_AUDIT.md 2.0f):
// ExCreateThread's r6 is the XAPI thread startup routine. Xenia XThread::Execute (xthread.cc) calls
// startup(start_address r3, start_context r4) when it is set, else start_address(start_context). This title's
// startup is XapiThreadStartup 0x828A8C08: XapiCallThreadNotifyRoutines(1), r3 = start(ctx),
// XapiCallThreadNotifyRoutines(0), ExTerminateThread(r3). Its one registered routine is the CRT's sub_827AF280:
// on exit it frees the thread's _tiddata (_freeptd), and the exit code becomes the start routine's return.
// Off: entry = start (r7), r3 = context, as before. The thread's own start keeps naming the thread everywhere
// (the KTHREAD, the VD pump / DOGMA-worker checks, the [THREAD] lines). Returns the entry; *r3Out / *r4Out its
// first two arguments.
static bool xapiThreadStartupOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_XAPI_THREAD_STARTUP");
    return !e || e[0] != '0';
  }();
  return v;
}
static uint32_t exThreadEntryPlan(bool xenia, uint32_t xapiStartup, uint32_t startVA, uint32_t startContext,
                                  uint32_t* r3Out, uint32_t* r4Out) {
  constexpr uint32_t kCodeBase = 0x820F0000u, kCodeEnd = 0x82A00000u;
  // Xenia XThread::Execute (xthread.cc) takes the startup whenever xapi_thread_startup != 0 and passes it
  // (start_address, start_context) unchanged, whatever the start address is (Astra batch-2 P3: an r6 == r7 or
  // r7 == 0 thread used to fall back to start(context, 0)). Only the host's own check stays: the entry it calls
  // must be guest code.
  const bool xapiOk = xapiStartup >= kCodeBase && xapiStartup < kCodeEnd;
  if (xenia && xapiStartup != 0 && xapiOk) {
    *r3Out = startVA;
    *r4Out = startContext;
    return xapiStartup;
  }
  *r3Out = startContext;
  *r4Out = 0;
  return startVA;
}
uint32_t exThreadEntryPlanForTest(uint32_t r6, uint32_t r7, uint32_t r8, uint32_t* r3, uint32_t* r4) {
  return exThreadEntryPlan(xapiThreadStartupOn(), r6, r7, r8, r3, r4);
}

static uint32_t stub_ExCreateThread(uint32_t r3, uint32_t r4, uint32_t r5,
                                    uint32_t r6, uint32_t r7, uint32_t r8) {
  static std::atomic<uint32_t> s_callCount{0};
  uint32_t n = s_callCount.fetch_add(1, std::memory_order_relaxed) + 1;

  uint8_t* base = nullptr;
  size_t size = 0;
  ExCreateThreadRunnerFn runner = getExCreateThreadRunner(&base, &size);

  // r7 = start address (the actual thread function entry point)
  // r8 = start context (passed to the thread)
  // r6 = xapi thread startup (wrapper; 0 means none)
  uint32_t startVA = r7;          // actual thread entry
  uint32_t startContext = r8;     // context/parameter
  uint32_t xapiStartup = r6;     // API startup wrapper
  uint32_t stackSize = r4;       // 0 = default
  uint32_t handlePtr = r3;       // where to write output handle

  constexpr uint32_t kCodeBase = 0x820F0000u;
  constexpr uint32_t kCodeEnd  = 0x82A00000u;
  bool startInRange = (startVA >= kCodeBase && startVA < kCodeEnd);

  // If startVA is 0 but xapiStartup is set, use xapiStartup as entry
  if (!startInRange && xapiStartup >= kCodeBase && xapiStartup < kCodeEnd) {
    startVA = xapiStartup;
    startInRange = true;
  }

  uint32_t handle = sr::kernel::allocUnifiedHandle();
  if (handlePtr) writeGuestBE32(handlePtr, handle);

  // Write thread ID if requested
  if (r5) writeGuestBE32(r5, handle);

  constexpr size_t kThreadStackSize = 256 * 1024;
  // iter-388 PLATFORM FIX (SR_STACK_PHYS, default ON; =0 reverts): thread
  // stacks moved guestHeapAlloc -> guestPhysAlloc. The heap bump's forward
  // march passes through arenas the GAME's own allocators carved earlier
  // inside the bump window (the 6.8MB metropolis package image at 0x91D8AC90
  // was live game data); a late thread's 256KB stack grant landed ENTIRELY
  // INSIDE it and stack frames shredded the image (0x5FF locals over Havok
  // headers -> parse rc=1 -> setter(NULL) -> the conveyor's unsatisfiable
  // wait = THE residual shallow-death; RAG #189 arc, [IMGCORRUPT] proof).
  // Phys serves the game's pool backings from the same monotonic cursor, so
  // stacks there can never overlap a game arena. Two-allocator overlap
  // family, 4th instance (iter-364 class).
  static const bool s_stackPhys = [] {
    const char* e = std::getenv("SR_STACK_PHYS");
    return !e || e[0] != '0';
  }();
  // iter-465 physmem PLATFORM FIX (SR_THREAD_MEM_XENIA, kernel/thread_mem_xenia.hpp):
  // the stack lives where Xenia's XThread::AllocateStack puts it - the v40000000
  // heap [0x70000000, 0x7F000000), not physical memory - and goes back when the
  // thread ends. The phys stacks (never freed, placed top-down into freed space)
  // split the freed 224 MB world pool on the Warworld -> Metropolis return.
  uint32_t stackAlloc = 0;
  uint32_t stackVA = 0;
  if (sr::kernel::threadmem::enabled())
    stackVA = sr::kernel::threadmem::allocStack(static_cast<uint32_t>(kThreadStackSize), &stackAlloc);
  if (!stackVA && s_stackPhys) {
    sr::kernel::PhysKindScope pk(sr::kernel::PhysKind::kThreadStack);
    stackVA = sr::kernel::guestPhysAlloc(kThreadStackSize);
    // iter-458 H-UNIQ (Astra 058): stacks share the phys cursor - census them.
    if (stackVA) sr::kernel::physwin::noteStackGrant(stackVA, kThreadStackSize);
  }
  if (!stackVA) stackVA = guestHeapAlloc(kThreadStackSize);

  SR_LOG_KERNEL_IMPORT("[ExCreateThread] #%u pHandle=0x%08X stackSz=0x%X startVA=0x%08X context=0x%08X xapi=0x%08X inRange=%d stackVA=0x%08X",
                      n, handlePtr, stackSize, startVA, startContext, xapiStartup, (int)startInRange, stackVA);

  // iter-346: capture the VD interrupt-pump thread's context (its event lives
  // at ctx+8) so VdSwap can deliver swap-complete signaling (platform: the
  // interrupt the real GPU raises; our GPU completes instantly).
  if (startVA == 0x82110A30u) {
    g_vdPumpCtxVA.store(startContext, std::memory_order_release);
    SR_LOG_KERNEL_IMPORT("[VD_PUMP] pump thread ctx captured: 0x%08X (event @+8)", startContext);
  }

  // iter-357j PLATFORM FIX: ExCreateThread's 7th arg (r9, via the extra-args
  // channel) is creation_flags; bit 0 = CREATE_SUSPENDED (Xenia contract).
  // We had ignored it — XMV decode workers that must start suspended (creator
  // sets up shared state, then NtResumeThread) raced their own setup and the
  // player-create handshake deadlocked (creator in KeWait, workers spinning).
  const uint32_t creationFlags = ExportRegistry::getCurrentArgR9();
  const bool createSuspended = (creationFlags & 1u) != 0;

  // iter-462 PLATFORM FIX (SR_OB_KTHREAD): the thread's guest KTHREAD, built
  // before any host thread exists (both the runner and the handle-only path),
  // so ObReferenceObjectByHandle / GetExitCodeThread see it from the start.
  uint32_t kt = 0;
  if (obKThreadEnabled()) {
    kt = kthreadCreate(handle, startVA, stackVA, (uint32_t)kThreadStackSize, creationFlags);
    // iter-463 PLATFORM FIX (SR_KPCR): its KPCR, on the creator's thread so the
    // CPU follows Xenia's GetFakeCpuNumber(creation_flags >> 24) (0 = parent's).
    if (kt && kpcrEnabled())
      kpcrCreateForKThread(kt, kpcrFakeCpuNumber(uint8_t(creationFlags >> 24)),
                           stackVA + (uint32_t)kThreadStackSize, stackVA);
  }

  if (runner && base && size && startInRange && stackVA) {
    // iter-30 attempt: defer DOGMA workers during walker phase to eliminate
    // contention with main thread inside sub_82182700. Result: contention
    // eliminated (0 DOGMA_WIT events vs wave-10's 20+), BUT walker still
    // hung at FMGameRtam — that ctor legitimately needs a worker alive to
    // dequeue and signal back. Reverted. Keeping the API + flag for future
    // experiments where we might want to defer SOME workers selectively.
    (void)kDogmaWorkerStartVA;
    // step4/audit SR_XAPI_THREAD_STARTUP: the entry may be the XAPI startup with (start, context).
    uint32_t runR3 = startContext, runR4 = 0;
    // With the gate on the plan gets the ORIGINAL start address (r7): the fallback above may have made startVA the
    // startup itself (r7 out of range), and Xenia still calls startup(r7, context).
    const uint32_t runEntry = xapiThreadStartupOn()
        ? exThreadEntryPlan(true, xapiStartup, r7, startContext, &runR3, &runR4)
        : exThreadEntryPlan(false, xapiStartup, startVA, startContext, &runR3, &runR4);
    auto* args = new GuestThreadArgs{runner, runEntry, runR3, stackVA, handle, base, size};
    args->kt = kt;
    args->stackAlloc = stackAlloc;
    args->startR4 = runR4;
    if (runEntry != startVA) {
      args->logStartVA = startVA;
      static std::atomic<uint32_t> s_x{0};
      const uint32_t nx = s_x.fetch_add(1, std::memory_order_relaxed) + 1;
      if (nx <= 32 || (nx & (nx - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[IMPORT_FIXES] XAPI_STARTUP #%u ExCreateThread handle=%u: entry %08X(start=%08X, ctx=%08X)",
                    nx, handle, runEntry, startVA, startContext);
    }
    // iter-132: 32MB reservation lets workers run ~47s (like iter-113) vs 15s at 8MB.
    // iter-130's KERNEL_CRASH at 8s with 256MB was a different code state; 32MB is a
    // safe middle ground that avoids deep-recursion stack overflow at sub_82720B58.
    constexpr SIZE_T kHostStackSize = 32 * 1024 * 1024; // 32 MB host stack
    HANDLE h = CreateThread(nullptr, kHostStackSize, guestThreadFunc, args,
                            STACK_SIZE_PARAM_IS_A_RESERVATION |
                                (createSuspended ? CREATE_SUSPENDED : 0),
                            nullptr);
    if (h) {
      {
        std::lock_guard<std::mutex> lock(g_eventMutex);
        g_eventHandles[handle] = h;
      }
      // iter-463: KTHREAD -> host thread (bookkeeping only; KeResumeThread resumes through it).
      if (kt) kthreadRecordHostThread(kt, h);
    } else if (obKThreadEnabled()) {
      // The thread will never run: it must not look alive forever.
      const unsigned long err = (unsigned long)GetLastError();
      kthreadMarkHandleExited(handle, 0u);
      SR_LOG_KERNEL_IMPORT("[ExCreateThread] #%u host CreateThread failed (err=%lu): KTHREAD marked exited",
                           n, err);
    }
    markAsThreadHandle(handle);
    SR_LOG_KERNEL_IMPORT("[ExCreateThread] #%u -> THREAD CREATED: startVA=0x%08X handle=%u hostHandle=%p flags=0x%X%s", n, startVA, handle, (void*)h, creationFlags, createSuspended ? " SUSPENDED" : "");
    // iter-43: notify world-ptr patcher for FM render workers
    if (startVA == kDogmaWorkerStartVA)
      sr::kernel::notifyFMWorkerCreated(startContext, handle);
  } else {
    SR_LOG_KERNEL_IMPORT("[ExCreateThread] #%u -> handle only (runner=%p inRange=%d stackVA=0x%08X)", n, (void*)runner, (int)startInRange, stackVA);
    sr::kernel::threadmem::releaseStack(stackAlloc);   // no thread will run on it
  }
  return kStatusSuccess;
}

// KeGetCurrentProcessType: Xenia's values are IDLE 0, TITLE 1, SYSTEM 2 (kernel_state.h:54-56).
// Heap owner check sub_8246BD18 compares it with the heap's creation-time byte (+379) and
// bugchecks F4 on a mismatch; heap creation 0x8246F6E0 and AudioBuildSpeakerConfig_sub_82117DF8
// (XAudio worker thread only when ==1) branch on it too.
// iter-463 SR_PROCTYPE (kernel/kpcr.hpp, default ON): Xenia xeKeGetCurrentProcessType from the
// caller's KPCR/KTHREAD. Gate off: the old constant 0.
static uint32_t stub_KeGetCurrentProcessType(uint32_t r3, uint32_t r4, uint32_t r5,
                                             uint32_t r6, uint32_t r7, uint32_t r8) {
  if (sr::kernel::procTypeEnabled())
    return sr::kernel::keGetCurrentProcessType(ExportRegistry::getCurrentR13());
  (void)r3;
  (void)r4;
  (void)r5;
  (void)r6;
  (void)r7;
  (void)r8;
  return 0;  // UserProcess - matches game's (r30+379) so we skip KeBugCheckEx(F4)
}

// Helper to dump guest memory as string or hex
static void dumpGuestMemory(const char* label, uint32_t guestAddr) {
  if (guestAddr < 0x82000000 || guestAddr >= 0x90000000) return;
  uint8_t* host = guestVAToHost(guestAddr);
  if (!host) return;
  
  // Try as string first
  char str[64] = {0};
  int strLen = 0;
  for (int i = 0; i < 63 && host[i] >= 0x20 && host[i] < 0x7F; i++)
    str[strLen++] = (char)host[i];
  if (strLen > 4) {
    SR_LOG_KERNEL_IMPORT("[BUGCHECK]   %s @ 0x%08X = \"%s\"", label, guestAddr, str);
    return;
  }
  // Dump as hex
  SR_LOG_KERNEL_IMPORT("[BUGCHECK]   %s @ 0x%08X = %02X %02X %02X %02X %02X %02X %02X %02X",
                       label, guestAddr, host[0], host[1], host[2], host[3],
                       host[4], host[5], host[6], host[7]);
}

// KeBugCheck/KeBugCheckEx: game reports fatal condition.
// F4 (FATAL_OBJECT_TERMINATION): don't count toward halt; glue will long-return so guest continues.
// Other codes: after kBugCheckHaltThreshold total, halt the guest thread so main window stays responsive.
// lr 0x827A16E8 = CRT doexit (sub_827A1698) re-entry bugcheck; 0x829AAE78/74 are its
// _C_Termination_Done/_C_Exit_Done flags (iter-463: the host no longer writes them).
constexpr uint32_t kBugCheckHaltThreshold = 25u;
static uint32_t stub_KeBugCheck(uint32_t r3, uint32_t r4, uint32_t r5,
                                uint32_t r6, uint32_t r7, uint32_t r8) {
  static std::atomic<uint32_t> s_count{0};
  static std::atomic<uint32_t> s_haltCount{0};
  uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1u;
  uint32_t guestLr = getCurrentGuestLR();

  // DOGMA path (sub_827A1698): when caller is 0x827A16E8, read guard globals once to see what game sees
  constexpr uint32_t kDogmaBugcheckLr = 0x827A16E8u;
  constexpr uint32_t kDogmaGuard1VA = 0x829AAE78u;
  constexpr uint32_t kDogmaGuard2VA = 0x829AAE74u;
  if (guestLr == kDogmaBugcheckLr) {
    static std::atomic<uint32_t> s_dogmaLogCount{0};
    uint32_t dogmaHit = s_dogmaLogCount.fetch_add(1, std::memory_order_relaxed);
    if (dogmaHit < 3) {
      uint32_t g1 = readGuestU32BE(kDogmaGuard1VA);
      uint32_t g2 = readGuestU32BE(kDogmaGuard2VA);
      SR_LOG_KERNEL_IMPORT("[BUGCHECK] DOGMA path lr=0x827A16E8: guard1(0x%08X)=0x%08X guard2(0x%08X)=0x%08X (NOT re-applying — address used by game for data)",
                           kDogmaGuard1VA, g1, kDogmaGuard2VA, g2);
    }
    // Do NOT re-apply guards — 0x829AAE78 is used by the game to store function
    // pointers. Re-applying guard1=1 corrupts the game's dispatch table.
  }

  // Log first 10 bugchecks with full detail (guest lr = return address of caller of KeBugCheckEx)
  if (n <= 10) {
    SR_LOG_KERNEL_IMPORT("[BUGCHECK] #%u code=0x%08X p1=0x%08X p2=0x%08X p3=0x%08X p4=0x%08X p5=0x%08X",
                         n, r3, r4, r5, r6, r7, r8);
    SR_LOG_KERNEL_IMPORT("[BUGCHECK]   guest lr=0x%08X (caller of KeBugCheckEx; see docs/_attic/DOGMA_ENGINE.md for sub_827A6298 callers)", guestLr);
    dumpGuestMemory("p1", r4);
    dumpGuestMemory("p2", r5);
    dumpGuestMemory("p3", r6);
  }

  // Only count non-F4 and non-DOGMA toward halt (F4 bypassed in glue; DOGMA we re-apply guards and continue)
  bool isDogmaPath = (guestLr == kDogmaBugcheckLr);
  if (r3 != 0xF4u && !isDogmaPath) {
    uint32_t h = s_haltCount.fetch_add(1, std::memory_order_relaxed) + 1u;
    if (kBugCheckHaltThreshold != 0u && h >= kBugCheckHaltThreshold) {
      SR_LOG_KERNEL_IMPORT("[BUGCHECK] Halting guest thread after %u bugchecks (main window remains open)", h);
#ifdef _WIN32
      ExitThread(0);
#else
      std::_Exit(0);
#endif
    }
  }
  return kStatusSuccess;
}

// iter-467 p2 divergence (1), docs/NATIVE_ROADMAP.md P2.9: VdGetSystemCommandBuffer(r3 = a 0x94-byte
// system command buffer descriptor, r4 = a dword). Xenia VdGetSystemCommandBuffer_entry
// (xboxkrnl_video.cc:353-357) zeroes the descriptor and stores 0xBEEF0000 at r3 and 0xBEEF0001 at r4;
// the export is void. Its only caller, D3DDeviceSwap_sub_82112050 (call at 0x8211228C: r3 = sp+0xC0,
// r4 = sp+0x74), copies descriptor+4 to dev+0x4E80 and +8 to dev+0x4FB8 and gates its scaler / gamma
// emission on +8, and passes *r4 to VdSwap as r7. The old stub wrote neither, so Swap read stack garbage.
// SR_VD_SYSCMDBUF = Xenia's fill (BAKED default ON iter-467; =0 restores the old path below for the A/B).
// Proof (p2fix1 / r1_scan_ab, exe 98054cc00c58): off, the descriptor read +8 = 0xBF800000 in 6111 of
// 6144 sampled swaps and Swap emitted scaler/gamma display-register PM4 from it (PM4_COV pages 0x18/0x19:
// 7721 / 13793 packets in ~6400 swaps); on, +0=BEEF0000 +4=0 +8=0 r7=BEEF0001, nz8 0/14336, pages 0x18/0x19
// 5 / 263 in 15872 frames; replay 9/9 SAME, whole-run check + flicker gate PASS.
static bool vdSysCmdBufXenia() {
#if SR_ORACLE
  static const bool v = [] {
    const char* e = std::getenv("SR_VD_SYSCMDBUF");
    return !e || e[0] != '0';
  }();
  return v;
#else
  return true;
#endif
}

// Return a guest VA (for VdGetSystemCommandBuffer - r3 is return value).
static uint32_t stub_VdGetSystemCommandBuffer(uint32_t r3, uint32_t r4, uint32_t r5,
                                              uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r5; (void)r6; (void)r7; (void)r8;
#if SR_ORACLE
  // NATIVE METER (native_meter.hpp `dispreg`): the system command buffer descriptor is a display-
  // register-class answer on either path (one per call).
  sr::native::meter::add(sr::native::meter::kDisplayReg);
  sr::native::vblank::countDispreg(sr::native::vblank::Dispreg::Sysbuf, 1);  // display.registers' breakdown (host only)
  if (vdSysCmdBufXenia()) {
    if (r3 && isGuestVAInRange(r3) && isGuestVAInRange(r3 + 0x93u)) {
      if (uint8_t* p = guestVAToHost(r3)) std::memset(p, 0, 0x94);
      writeGuest32(r3, 0xBEEF0000u);
    }
    if (r4 && isGuestVAInRange(r4) && isGuestVAInRange(r4 + 3u)) writeGuest32(r4, 0xBEEF0001u);
    static int s_xlog = 0;
    if (s_xlog++ < 5)
      SR_LOG_KERNEL_IMPORT("[RENDER] VdGetSystemCommandBuffer (SR_VD_SYSCMDBUF, Xenia fill) desc=0x%08X dw=0x%08X (call #%d)",
                           r3, r4, s_xlog);
    return r3;  // Xenia's export is void: r3 keeps the caller's pointer (Swap overwrites it at once)
  }
  (void)r4;
  uint32_t va = getGfxCommandBufferVA();
  static int s_logCount = 0;
  if (s_logCount++ < 5) {
    SR_LOG_KERNEL_IMPORT("[RENDER] VdGetSystemCommandBuffer -> 0x%08X (guest call #%d)", va, s_logCount);
  }
  return va;
#else
  (void)r3; (void)r4;
  SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_DISPLAY_BAD] guest system-command-buffer path reached release");
  std::_Exit(3);
#endif
}

// VdEnableRingBufferRPtrWriteBack: tells the GPU where to write back the ring buffer
// read pointer (RPTR). r3 = physical address where RPTR should be written.
// On Xbox 360, physical-to-VA mapping is physAddr + 0x80000000.
static uint32_t stub_VdEnableRingBufferRPtrWriteBack(uint32_t r3, uint32_t r4,
    uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8) {
  uint32_t physAddr = r3;
  uint32_t va = physAddr + 0x80000000u;
#if SR_ORACLE
  g_rptrWritebackVA.store(va, std::memory_order_release);
#endif
  sr::kernel::gpuCpSetRptrWriteback(physAddr, r4);  // P2 async CP writes it here
  // iter-463 charter (burndown item 12): Xenia only RECORDS the writeback
  // address (xboxkrnl_video.cc:345-350 -> CommandProcessor
  // EnableReadPointerWriteBack) - so do we, above. The iter-176 write of that
  // VA into "device+10768" at the FAKE device 0xD0000000 is DELETED: the fake
  // device went in iter-358f, the title's real device lives behind
  // VdGlobalDevice (0xD0013000), and nothing (kernel, gfx, generated guest
  // code) reads 0xD0002A10.
  SR_LOG_KERNEL_IMPORT("[RPTR] VdEnableRingBufferRPtrWriteBack: phys=0x%08X -> VA=0x%08X (recorded for the CP)", physAddr, va);
  STUB_LOG("VdEnableRingBufferRPtrWriteBack", r3, r4, r5, r6, r7, r8, kStatusSuccess);
  return kStatusSuccess;
}

// ========== VdQueryVideoMode / VdSetDisplayMode / display stubs ==========
// Xbox 360 VIDEO_MODE struct (0x30 bytes, big-endian in guest memory):
//   +0x00  display_width
//   +0x04  display_height
//   +0x08  is_interlaced
//   +0x0C  is_widescreen
//   +0x10  is_hi_def
//   +0x14  refresh_rate
//   +0x18  video_standard (1=NTSC, 2=PAL)
//   +0x1C..+0x2C  reserved/unknown
static uint32_t stub_VdQueryVideoMode(uint32_t r3, uint32_t r4, uint32_t r5,
                                       uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  if (r3 != 0) {
    uint8_t* host = guestVAToHost(r3);
    if (host) {
      memset(host, 0, 0x30);
      writeGuest32(r3 + 0x00, 1280);   // display_width
      writeGuest32(r3 + 0x04, 720);    // display_height
      writeGuest32(r3 + 0x08, 0);      // is_interlaced = false
      writeGuest32(r3 + 0x0C, 1);      // is_widescreen = true
      writeGuest32(r3 + 0x10, 1);      // is_hi_def = true
      writeGuest32(r3 + 0x14, 60);     // refresh_rate
      writeGuest32(r3 + 0x18, 1);      // video_standard = NTSC
    }
  }
  STUB_LOG("VdQueryVideoMode", r3, r4, r5, r6, r7, r8, kStatusSuccess);
  return kStatusSuccess;
}

// VdGetCurrentDisplayInformation fills a display info struct.
// Xenia fills ~0x30 bytes at the output pointer. Key fields:
//   +0x02  front_buffer_width (uint16 BE)
//   +0x04  front_buffer_height (uint16 BE)
static uint32_t stub_VdGetCurrentDisplayInformation(uint32_t r3, uint32_t r4, uint32_t r5,
                                                     uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  if (r3 != 0) {
    uint8_t* host = guestVAToHost(r3);
    if (host) {
      memset(host, 0, 0x58);
      // Fill key fields: width=1280, height=720
      // Offset +0x00: ActualDisplayWidth (BE16)
      host[0] = (1280 >> 8) & 0xFF; host[1] = 1280 & 0xFF;
      // Offset +0x02: ActualDisplayHeight (BE16)
      host[2] = (720 >> 8) & 0xFF; host[3] = 720 & 0xFF;
      // Offset +0x04: may be aspect ratio or refresh related
      // Offset +0x08: D3DMULTISAMPLE_TYPE (0=none)
    }
  }
  STUB_LOG("VdGetCurrentDisplayInformation", r3, r4, r5, r6, r7, r8, kStatusSuccess);
  return kStatusSuccess;
}

// VdQueryVideoFlags returns flags in r3 (return value).
// Flags: 0x1=widescreen, 0x2=480i, 0x4=480p, 0x8=720p, 0x10=1080i, 0x20=1080p
// Return 720p + widescreen.
static uint32_t stub_VdQueryVideoFlags(uint32_t r3, uint32_t r4, uint32_t r5,
                                        uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r3; (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  uint32_t flags = 0x00000009; // 0x1 (widescreen) | 0x8 (720p)
  SR_LOG_KERNEL_IMPORT("VdQueryVideoFlags -> 0x%08X", flags);
  return flags;
}

// VdSetDisplayMode logs the requested mode and returns success.
static uint32_t stub_VdSetDisplayMode(uint32_t r3, uint32_t r4, uint32_t r5,
                                       uint32_t r6, uint32_t r7, uint32_t r8) {
  SR_LOG_KERNEL_IMPORT("VdSetDisplayMode(r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X)", r3, r4, r5, r6);
  (void)r7; (void)r8;
  return kStatusSuccess;
}

// iter-345c ROOT FIX (charter platform fix): VdSetGraphicsInterruptCallback.
// THE console heartbeat we never provided. The game registers its D3D-runtime
// ISR (sub_820FDBA0(source, ctx): source 0 = VSYNC -> bumps the frame counter
// and invokes the swap callback that POSTS the per-frame master dispatch
// cmd 0x2C -> sub_8271F848 -> walks ALL FM per-frame callbacks incl. the PSM
// splash dispatcher sub_822AB748). We stubbed the registration, so the entire
// per-frame chain was dark: splash sequencer roll-dependent (fired only when
// rare paths poked it), fsmPass=0, feFrames=0, FE flow dead. This implements
// the service: capture the callback and fire it at 60 Hz (source=0/vsync) from
// a host vblank thread via GuestCaller. SR_VD_ISR=0 disables for A/B.
// The registration is published through graphics_interrupt::registrationService()
// (kernel/graphics_interrupt_registration.hpp): each VD vblank and CP interrupt
// delivery holds a lease on the tuple it read, and an external setter returns
// only after every admitted delivery is done. Retail unregisters (8210651C) and
// then frees the ring state behind *(dev+2A14) and the device, so a delivery that
// read the old tuple must not still be running when the setter returns.
static std::atomic<bool> g_vdIsrThreadStarted{false};

#if SR_ORACLE
static bool vdNativeSource0Requested() {
  static const bool on = [] {
    const char* value = std::getenv("SR_NATIVE_DISPLAY_SOURCE0");
    return value && value[0] == '1';
  }();
  return on;
}

#endif

static thread_local sr::native::display::Source0Result g_vdNativeSource0Result;
static thread_local uint32_t g_vdNativeSource0Exception;
// display.registers (Astra round 1, P1): the LEASED registration's callback, so trySource0's identity check sees what
// is registered (it declines anything but the D3D ISR) instead of a hard-coded kD3DIsr.
static thread_local uint32_t g_vdNativeSource0Callback;
static void vdNativeSource0Thunk(PPCContext& ctx, uint8_t* base) {
  g_vdNativeSource0Result = sr::native::display::trySource0(
    ctx, base, g_vdNativeSource0Callback, ctx.r4.u32,
    &g_vdNativeSource0Exception);
}

// iter-467 p2 divergence (2), docs/NATIVE_ROADMAP.md P2.9: the display controller's scan-out
// registers. GpuPresentPacingUpdate_sub_82111E38 (the vblank gate's ISR callback) reads
// D1MODE_V_COUNTER (MMIO 0x7FC86530, reg 0x194C) and D1MODE_VIEWPORT_SIZE (0x7FC86584 & 0xFFF, reg
// 0x1961) and releases the gate at once only when line% = V*100/H + 1 (capped at 100) is <= its tear
// threshold, else one vblank later. Our zero-backed MMIO page read 0 for both, so the gate always
// released at once. Xenia (graphics_system.cc:273-295 ReadRegister: 0x1961 -> 0x050002D0; frame
// limiter :143-144: V_COUNTER += the display height every pass) makes V*100/H saturate, so the gate
// waits one more vblank, as the oracle does. SR_VD_SCANOUT_REGS (BAKED default ON iter-467; =0 = the A/B): the
// console path (60 Hz vblank) adds the height per vblank and reports 0x050002D0. PORT FEATURE
// (SR_HIGHFPS): the high-rate emulated display reports scanline 0, so the guest's gate keeps
// releasing at once and the feature's uncapped pacing is unchanged. Joins the in-roll A/B
// (SR_AB_TOGGLE=SR_VD_SCANOUT_REGS): its OFF windows write the old zeros. Proof (console path, paired
// in-roll A/B): gameplay (r1_scan_ab) fps 30.00 both ways, delta +0.015 +-0.017; intro movies + title
// (r6_title_ab, natural, 70 pairs) fps +0.089 +-0.560, frame_p50 +0.025 +-0.037 ms - neutral, 2.0
// vblanks/frame either way (golden Xenia ~2.48 is Xenia's own speed). NATIVE: answering display-register
// reads is emulation (NATIVE_ROADMAP section 2); the P2-B GATE item replaces 82111E38's reads.
#if SR_ORACLE
static bool vdScanoutRegsEnv() {
  static const bool v = [] {
    const char* e = std::getenv("SR_VD_SCANOUT_REGS");
    return !e || e[0] != '0';
  }();
  return v;
}

static void vdScanoutRegsTick(bool highFps) {
  if (!vdScanoutRegsEnv()) return;  // =0: the MMIO page is never touched (the pre-fix zeros)
  static const int ab = sr::abt::reg("SR_VD_SCANOUT_REGS");
  constexpr uint32_t kVCounterVA = 0x7FC86530u;   // D1MODE_V_COUNTER (reg 0x194C)
  constexpr uint32_t kViewportVA = 0x7FC86584u;   // D1MODE_VIEWPORT_SIZE (reg 0x1961)
  // Xenia: 1280 x 720 (graphics_system.cc ReadRegister 0x1961). display.registers: the one definition the host counter
  // under SR_NATIVE_VBLANK shares (kernel/native_vblank.hpp).
  constexpr uint32_t kViewport = sr::native::vblank::kViewportAnswer;
  static uint32_t s_vcount = 0;
  if (ab >= 0 && !sr::abt::on(ab)) {  // A/B OFF window: the pre-fix values
    s_vcount = 0;
    writeGuest32(kVCounterVA, 0u);
    writeGuest32(kViewportVA, 0u);
    sr::native::meter::add(sr::native::meter::kDisplayReg, 2);  // still two emulated register answers
    sr::native::vblank::countDispreg(sr::native::vblank::Dispreg::Scanout, 2);
    return;
  }
  // V += the height per console-path tick (32-bit wrap), 0 under HIGHFPS - the same function advances the host counter
  // under SR_NATIVE_VBLANK, so the two answers cannot diverge.
  s_vcount = sr::native::vblank::nextVCounter(s_vcount, highFps);
  writeGuest32(kVCounterVA, s_vcount);
  writeGuest32(kViewportVA, kViewport);
  // NATIVE METER (native_meter.hpp `dispreg`): the two scan-out registers answered by emulation this
  // vblank. 0 once the P2-B GATE item serves the guest's pacing read natively (display.registers:
  // SR_NATIVE_VBLANK skips this tick, kernel/native_vblank.hpp).
  sr::native::meter::add(sr::native::meter::kDisplayReg, 2);
  sr::native::vblank::countDispreg(sr::native::vblank::Dispreg::Scanout, 2);
  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[VD_SCANOUT] SR_VD_SCANOUT_REGS on: V_COUNTER += %u per vblank (console path), "
                "VIEWPORT_SIZE = 0x%08X%s", kViewport & 0xFFFu, kViewport,
                highFps ? " [HIGHFPS: scanline 0]" : "");
  }
}

#endif

// One vblank tick's delivery for the leased registration (vdVblankThread, after the lease, the callback lookup and
// the CPU-2 / KPCR setup; the difftest's --native-vblank-test drives it with a GuestCaller of its own).
// display.registers (SR_NATIVE_VBLANK, kernel/native_vblank.hpp; Astra round 1, P1): planTick decides from the
// leased registration itself - the host source 0 only for the retail D3D ISR 820FDBA0 with a device context (a
// native registration, or that registration in a toggled roll's ON windows); any other registration runs its own
// callback, and the per-vblank check never reads its context. Gate OFF: no tick scope, and the tick is today's.
VdTickDelivery vdVblankDeliver(GuestCaller* gc, const graphics_interrupt::Registration& registration, bool ownPcr,
                               bool highFps) {
  VdTickDelivery d;
  const auto plan = sr::native::vblank::planTick(registration.nativeSource0, registration.callbackVA,
                                                 registration.contextVA);
  d.native = plan.native;
  if (plan.native && !ownPcr) {
    d.refused = true;  // the caller stops: the host source 0 needs the CPU-2 DPC identity
    return d;
  }
  PPCFunc* fn = PPC_LOOKUP_FUNC(getGuestBase(), registration.callbackVA);
  if (!plan.native && !fn) return d;  // the caller looked it up first; not reached
  GuestArgs a{};
  a.r3 = 0;  // interrupt source: 0 = vsync
  a.r4 = registration.contextVA;
  {
    // From the scan-out answer through the source-0 delivery: the CPU-2 lock (the console's DPC serialization with
    // the pacing 82111E38), the host counter's advance (today's V_COUNTER answer, held by host code), the tick record
    // and the per-vblank check of the D3D ISR's effects, around either form.
    std::optional<sr::native::vblank::TickScope> tick;
    if (plan.scoped) tick.emplace(plan.checkDev, highFps, plan.native);
    // iter-467 p2 divergence (2): before the vblank ISR, as Xenia's frame limiter. display.registers: not written
    // under SR_NATIVE_VBLANK (a toggled roll keeps them for its OFF windows' guest 82111E38, inside the lock).
#if SR_ORACLE
    if (sr::native::vblank::scanoutAnswers()) vdScanoutRegsTick(highFps);
#endif
    KpcrDpcScope dpc(ownPcr ? gc->ctx().r13.u32 : 0u);
    if (plan.native) {
      g_vdNativeSource0Result = sr::native::display::Source0Result::Declined;
      g_vdNativeSource0Exception = 0;
      g_vdNativeSource0Callback = registration.callbackVA;
      d.okc = gc->call(vdNativeSource0Thunk, a, &d.exc);
      d.source0Completed = g_vdNativeSource0Result == sr::native::display::Source0Result::Completed;
      d.source0Result = unsigned(g_vdNativeSource0Result);
      d.source0Exception = g_vdNativeSource0Exception;
    } else {
      d.okc = gc->call(fn, a, &d.exc);
    }
  }
  return d;
}

static void vdVblankThread() {
  using namespace std::chrono;
  sr::kernel::GuestCaller gc("VD_ISR");
  if (!gc.ok()) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[VD_ISR] GuestCaller not ready; vblank service cannot start");
    std::_Exit(3);
  }
  uint8_t* gb = sr::kernel::getGuestBase();
  // iter-464 PORT FEATURE (SR_HIGHFPS, default off): the emulated display's vblank follows
  // the high-frame-rate config (kernel/highfps.hpp) so the game's own present pacing
  // (D3DPRESENT_INTERVAL_ONE: >= 1 vblank between flips) does not cap it at 60. Off keeps
  // the console's 60 Hz cadence exactly as before.
  // iter-465 polish: the rate is the LIVE one (the DISPLAY OPTIONS page can change it while the
  // game runs; highfps::liveVblankHz() == 0 = the console's 60 Hz path, as before).
  double hfHz = sr::kernel::highfps::liveVblankHz();
  std::unique_ptr<sr::kernel::highfps::VblankClock> hfClock;
  if (hfHz > 0.0) hfClock = std::make_unique<sr::kernel::highfps::VblankClock>(hfHz);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[VD_ISR] vblank thread up — %.1f Hz, source=0 (vsync)%s",
              hfHz > 0.0 ? hfHz : 60.0, hfHz > 0.0 ? " [HIGHFPS]" : "");
  uint64_t n = 0;
  auto next = steady_clock::now();
  // tools/graceful-quit (kernel/quit.hpp): the vblank ISR never parks at an import (it would hold the delivery
  // lease); the loop top below, holding nothing, is where a quit's thread cut stops this thread (no more vblanks,
  // no more `dispreg` answers).
  sr::kernel::quit::markExempt("VD_ISR");
  for (;;) {
    sr::kernel::quit::safePoint("VD_ISR");
    const double wantHz = sr::kernel::highfps::liveVblankHz();
    if (wantHz != hfHz) {
      hfHz = wantHz;
      hfClock.reset();
      if (hfHz > 0.0) hfClock = std::make_unique<sr::kernel::highfps::VblankClock>(hfHz);
      next = steady_clock::now();
      SR_LOG_BOOT(sr::LogSeverity::Info, "[VD_ISR] vblank rate -> %.1f Hz (settings menu)%s",
                  hfHz > 0.0 ? hfHz : 60.0, hfHz > 0.0 ? " [HIGHFPS]" : " (the console's)");
    }
    if (hfClock) {
      hfClock->wait();
    } else {
      next += microseconds(16667);  // 60 Hz
      std::this_thread::sleep_until(next);
    }
    // tools/graceful-quit: and right before the ISR - a cut during the wait above never delivers one more vblank.
    sr::kernel::quit::safePoint("VD_ISR");
    // Held to the end of this tick: the source-0 ISR, the source-1 deliveries and
    // the device reads below all use this registration's device.
    const auto lease = sr::kernel::graphics_interrupt::registrationService().begin();
    if (!lease) continue;  // unregistered, or a setter is retiring the registration
    const auto& registration = lease.registration();
    const uint32_t cb = registration.callbackVA;
    PPCFunc* fn = PPC_LOOKUP_FUNC(gb, cb);
    if (!fn) {
      if (registration.nativeSource0) {
        SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_DISPLAY] source0 ISR mapping missing cb=%08X", cb);
        std::_Exit(3);
      }
      continue;
    }
    // iter-345d: the ISR's vsync-enable gate reads through r13 (Xenon TLS).
    // Mirror the MAIN thread's r13 so the gate sees the real flags.
    // iter-463 SR_KPCR: this thread has its OWN KPCR (GuestCaller) and runs the
    // ISR as Xenia's vsync thread does (MarkVblank -> EmulateCPInterruptDPC(0,
    // cpu 2)): SetActiveCpu(2) + DPC impersonation. No mirroring.
    const bool ownPcr = sr::kernel::kpcrEnabled() && gc.ctx().r13.u32 != 0;
    if (!ownPcr) {
      const uint32_t mainR13 = sr::kernel::getMainGuestR13();
      if (mainR13) gc.ctx().r13.u32 = mainR13;
    } else {
      sr::kernel::kpcrSetActiveCpu(gc.ctx().r13.u32, 2u);
    }
    sr::kernel::GuestArgs a{};
    a.r3 = 0;  // interrupt source: 0 = vsync
    a.r4 = registration.contextVA;
    const sr::kernel::VdTickDelivery d = sr::kernel::vdVblankDeliver(&gc, registration, ownPcr, hfHz > 0.0);
    if (d.refused) {
      SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_DISPLAY] source0 requires owned CPU2/DPC context");
      std::_Exit(3);
    }
    const uint32_t exc = d.exc;
    const bool okc = d.okc;
    // Native registration does not arm the oracle's MMIO enable bit. A failure
    // therefore terminates explicitly; replaying the original ISR is unsafe.
    if (d.native && (!okc || !d.source0Completed)) {
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[NATIVE_DISPLAY] source0 failed result=%u exc=%08X dev=%08X",
        d.source0Result, exc ? exc : d.source0Exception, registration.contextVA);
      std::_Exit(3);
    }
    // P2 async CP: counter_ ticks per vblank (Xenia MarkVblank); source-1
    // interrupts come from the CP at each PM4_INTERRUPT, in order, so neither
    // delivery below runs (they approximated exactly that).
    sr::kernel::gpuCpVblank();
#if SR_ORACLE
    const bool cpOwnsIrq = sr::kernel::gpuCpActive();
    // iter-345f: also deliver the COMPLETION interrupt (source=1) when a
    // pending ring-op record exists at ctx+0x2A14 — on hardware the GPU
    // raises this when the command processor finishes; our fake GPU completes
    // instantly, so pending == complete. This is what signals the
    // VdInterruptPumpWait event (the pump then runs sub_82110840 = the
    // frame bottom-half). Guarded exactly like the ISR itself.
    if (okc && !cpOwnsIrq && a.r4 >= 0x10000u) {
      const uint32_t rec = readGuestU32BE(a.r4 + 0x2A14u);
      if (rec >= 0x10000u && rec < 0xC0000000u) {
        const uint32_t cbFn = readGuestU32BE(rec + 0x10u);
        if (cbFn != 0 && cbFn != 0x0BADF00Du) {
          sr::kernel::GuestArgs a1{};
          a1.r3 = 1;  // source: 1 = CP/ring completion
          a1.r4 = a.r4;
          uint32_t exc1 = 0;
          bool ok1;
          {   // iter-463: every interrupt delivery is a DPC (Xenia EmulateCPInterruptDPC)
            sr::kernel::KpcrDpcScope dpc1(ownPcr ? gc.ctx().r13.u32 : 0u);
            ok1 = gc.call(fn, a1, &exc1);
          }
          static std::atomic<uint64_t> s_c1{0};
          uint64_t c1 = s_c1.fetch_add(1, std::memory_order_relaxed) + 1;
          if (c1 <= 4 || (c1 & (c1 - 1)) == 0 || !ok1)
            SR_LOG_BOOT(ok1 ? sr::LogSeverity::Info : sr::LogSeverity::Warn,
              "[VD_ISR] completion #%llu rec=%08X cbFn=%08X %s exc=%08X",
              (unsigned long long)c1, rec, cbFn, ok1 ? "ok" : "EXC", exc1);
        }
      }
    }
    // iter-397q PLATFORM FIX (SR_CP_INT=1): deliver source-1 for every
    // PM4_INTERRUPT (0x54) packet the engine consumed — UNCONDITIONALLY, as
    // hardware does. The iter-345f block above gates on an ARMED continuation
    // (rec+0x10 != 0), but the ISR with cb16==0 still does load-bearing work:
    // it clears the per-CPU pending-interrupt bit in *(dev+0x2A14) that the
    // driver's emission path set when it wrote the 0x54. Dropping the delivery
    // (the pre-397q state: pm4_engine counted 0x54s and did nothing) jams that
    // handshake; live X-ray: the driver emits 0x54s all through FMV playback
    // (~2-3/s) and the ring doorbell goes silent after #5 while draws freeze.
    if (okc && !cpOwnsIrq) {
      // iter-398d: baked default-ON (=0 keeps the A/B) alongside
      // SR_PM4_CHAINSKIP — proven across K-runs (iter-397q evidence).
      static const bool s_cpInt = [] {
        const char* e = std::getenv("SR_CP_INT");
        return !e || e[0] != '0';
      }();
      if (s_cpInt) {
        static uint64_t s_lastCpInt = 0;
        const uint64_t now = sr::gfx::getLiveCpInterruptCount();
        uint64_t pendingInts = now - s_lastCpInt;
        s_lastCpInt = now;
        if (pendingInts > 8) pendingInts = 8;   // storm cap per 16ms tick
        for (uint64_t i = 0; i < pendingInts; i++) {
          sr::kernel::GuestArgs a1{};
          a1.r3 = 1;  // source: 1 = CP interrupt
          a1.r4 = a.r4;
          uint32_t exc1 = 0;
          bool ok1;
          {   // iter-463: every interrupt delivery is a DPC (Xenia EmulateCPInterruptDPC)
            sr::kernel::KpcrDpcScope dpc1(ownPcr ? gc.ctx().r13.u32 : 0u);
            ok1 = gc.call(fn, a1, &exc1);
          }
          static std::atomic<uint64_t> s_d{0};
          const uint64_t d = s_d.fetch_add(1, std::memory_order_relaxed) + 1;
          if (d <= 8 || (d & (d - 1)) == 0 || !ok1)
            SR_LOG_BOOT(ok1 ? sr::LogSeverity::Info : sr::LogSeverity::Warn,
              "[VD_ISR] cp-int #%llu delivered (engine total=%llu) %s exc=%08X",
              (unsigned long long)d, (unsigned long long)now,
              ok1 ? "ok" : "EXC", exc1);
          if (!ok1) break;
        }
      }
    }
#endif
    ++n;
    if (n <= 4 || (n & (n - 1)) == 0 || !okc) {
      // Diagnose the two vsync-branch gates: swapCb@ctx+0x3B10, frameCtr@+0x3B14.
      uint32_t swapCb = 0, frameCtr = 0;
      if (a.r4 >= 0x10000u) {
        swapCb = readGuestU32BE(a.r4 + 0x3B10u);
        frameCtr = readGuestU32BE(a.r4 + 0x3B14u);
      }
      SR_LOG_BOOT(okc ? sr::LogSeverity::Info : sr::LogSeverity::Warn,
        "[VD_ISR] tick #%llu cb=%08X ctx=%08X r13=%08X swapCb=%08X frameCtr=%u %s exc=%08X",
        (unsigned long long)n, cb, a.r4, gc.ctx().r13.u32, swapCb, frameCtr,
        okc ? "ok" : "EXC", exc);
    }
    // iter-397q X-RAY (1 Hz): the movie replay machine. dev+0x2A14 = interrupt
    // pump context {+0 pending mask, +4 replay-active, +16 continuation cb
    // (0x0BADF00D = poison/idle), +20 arg}; dev+12944 = command-buffer
    // recording ctx (mode B — golden menu value 0); dev+10808..10810 = mode
    // flag bytes. Read-only; names why source-1 never fires.
    // The 1 Hz x-ray counts vblanks; keep it 1 Hz at any (live) vblank rate.
    const uint64_t xrayEvery = hfHz > 0.0 ? (uint64_t)std::max(1.0, hfHz + 0.5) : 60u;
    if (a.r4 >= 0x10000u && (n % xrayEvery) == 0) {
      const uint32_t dev = a.r4;
      const uint32_t rec = readGuestU32BE(dev + 0x2A14u);
      uint32_t cb16 = 0, cb20 = 0, pend = 0, act = 0;
      if (rec >= 0x10000u && rec < 0xC0000000u) {
        pend = readGuestU32BE(rec);
        act  = readGuestU32BE(rec + 4u);
        cb16 = readGuestU32BE(rec + 0x10u);
        cb20 = readGuestU32BE(rec + 0x14u);
      }
      const uint32_t recMode = readGuestU32BE(dev + 12944u);
      const uint32_t flags   = readGuestU32BE(dev + 10808u);
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[VD_ISR] xray pumpctx=%08X pend=%08X act=%08X cb16=%08X arg=%08X "
        "recMode=%08X flags10808=%08X",
        rec, pend, act, cb16, cb20, recMode, flags);
      // iter-398: THE PUMP BLOCK. VdInterruptPumpWait(a1): a1 = one of the
      // 56-byte entries at dev+11228 (idx at dev+11192); {+0 statePtr(=pumpctx),
      // +4 epochSnapshot, +0x20 KEVENT}. Wait mode: snapshot == *(pumpctx+380)
      // ? 30ms poll (the self-present duty) : INFINITE (wait for KeSetEvent
      // from the submit walker tail). Name the live mismatch.
      const uint32_t slotIdx = readGuestU32BE(dev + 11192u);
      const uint32_t blk = dev + 11228u + 56u * (slotIdx & 7u);
      const uint32_t epoch = (rec >= 0x10000u) ? readGuestU32BE(rec + 380u) : 0;
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[VD_ISR] pumpblk idx=%u blk=%08X stateP=%08X snap=%08X epoch(+380)=%08X "
        "ev[+20]=%08X ev.sig(+24)=%08X qHead(ctx+4)=%08X",
        slotIdx, blk, readGuestU32BE(blk), readGuestU32BE(blk + 4u), epoch,
        readGuestU32BE(blk + 0x20u), readGuestU32BE(blk + 0x24u), act);
    }
  }
}

static uint32_t stub_VdSetGraphicsInterruptCallback(uint32_t r3, uint32_t r4, uint32_t r5,
                                                    uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r5; (void)r6; (void)r7; (void)r8;
#if SR_ORACLE
  static const bool s_on = [] {
    const char* e = std::getenv("SR_VD_ISR");
    return !e || e[0] != '0';
  }();
  // display.registers (SR_NATIVE_VBLANK, untoggled): the host vblank service owns source 0 for good, so the
  // registration is native and the vsync-enable bit 0x7FC86544 is never armed. A toggled roll arms it for its OFF
  // windows' guest ISR (nativeRegistration() is false there).
  // native/dreg-hang: the host source 0 admits guest ranges by arithmetic and declines every tick while a
  // page-protecting diagnostic is requested (kernel/guest_protect.hpp) - fatal for a native registration - so the
  // registration stays the guest ISR's then (SR_NATIVE_VBLANK refuses - or, defaulted, stands down - on its own; this
  // covers SR_NATIVE_DISPLAY_SOURCE0).
  const bool protectDiag = sr::kernel::guest_protect::diagnosticRequested();
  const bool nativeSource0 = s_on &&
    (vdNativeSource0Requested() || sr::native::vblank::nativeRegistration()) &&
    r3 == sr::native::display::kD3DIsr && r4 != 0 && !protectDiag;
  if (s_on && vdNativeSource0Requested() && protectDiag && r3 == sr::native::display::kD3DIsr && r4 != 0) {
    static std::atomic<bool> s_warned{false};
    if (!s_warned.exchange(true))
      SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[NATIVE_DISPLAY] SR_NATIVE_DISPLAY_SOURCE0 refused: a page-protecting diagnostic is requested "
        "(kernel/guest_protect.hpp kDiagnosticGates); the guest ISR keeps source 0");
  }
#else
  constexpr bool s_on = true;
  const bool nativeService = sr::native::vblank::nativeRegistration();
  const bool nativeSource0 = nativeService && sr::native::vblank::d3dIsrRegistration(r3, r4);
  if (r3 == sr::native::display::kD3DIsr && !nativeSource0) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_DISPLAY_BAD] invalid native ISR registration cb=%08X dev=%08X", r3, r4);
    std::_Exit(3);
  }
#endif
  // Prepare the oracle enable state before exposing an oracle registration. It runs
  // under the publication mutex, so it only writes the bit; the logging follows.
  enum class Armed { None, Native, Bit, Unmapped };
  struct Prepare { bool on; Armed armed = Armed::None; } prepare{s_on};
  namespace gint = sr::kernel::graphics_interrupt;
  const auto result = gint::registrationService().publish(
    {r3, r4, nativeSource0},
    [](const gint::Registration& next, void* context) noexcept {
      auto& p = *static_cast<Prepare*>(context);
      if (!p.on || !next.callbackVA) return;
      if (next.nativeSource0) {
        p.armed = Armed::Native;
#if SR_ORACLE
      } else if (uint8_t* bit = guestVAToHost(0x7FC86544u)) {
        bit[3] |= 1;
        sr::native::meter::add(sr::native::meter::kDisplayReg);
        sr::native::vblank::countDispreg(sr::native::vblank::Dispreg::VsyncBit, 1);
        p.armed = Armed::Bit;
      } else {
        p.armed = Armed::Unmapped;
#endif
      }
    },
    &prepare);
  if (result != gint::PublishResult::Published) {
    // Only an interrupt delivery itself can get here (it holds a lease). The tuple
    // it runs on stays published, and the SDK would go on to free the device under
    // it; stop instead of continuing on freed state.
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[VD_ISR] VdSetGraphicsInterruptCallback(cb=%08X ctx=%08X) inside an interrupt delivery "
      "rejected (%s); stopping",
      r3, r4, result == gint::PublishResult::ReentrantUnregister ? "unregister" : "retirement in progress");
    std::_Exit(3);
  }
  if (prepare.armed == Armed::Native) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[NATIVE_DISPLAY] source0 armed cb=%08X dev=%08X; native ISR owns enable state", r3, r4);
#if SR_ORACLE
  } else if (prepare.armed == Armed::Bit) {
    SR_LOG_KERNEL_IMPORT("VdSetGraphicsInterruptCallback: kernel vsync-enable bit SET @0x7FC86544");
  } else if (prepare.armed == Armed::Unmapped) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[VD_ISR] kernel page 0x7FC86544 unmapped; vsync gate cannot be armed");
#endif
  }
#if SR_ORACLE
  SR_LOG_KERNEL_IMPORT("VdSetGraphicsInterruptCallback(cb=0x%08X ctx=0x%08X)%s",
                       r3, r4, s_on ? "" : " [SR_VD_ISR=0; not fired]");
#else
  SR_LOG_KERNEL_IMPORT("VdSetGraphicsInterruptCallback(cb=0x%08X ctx=0x%08X)", r3, r4);
#endif
  if (s_on && r3 != 0 &&
      !g_vdIsrThreadStarted.exchange(true, std::memory_order_acq_rel)) {
    std::thread(vdVblankThread).detach();
  }
  return kStatusSuccess;
}

// VdGetCurrentDisplayGamma(DWORD* type r3, float* power r4) - VOID, two 4-byte out-parameters (xboxkrnl; Xenia
// src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:134-143: *type = kernel_display_gamma_type, default 2 = BT.709 / HDTV;
// *power = kernel_display_gamma_power, default 2.22222233, read only for type 3). The title's two callers, the ramp
// builders GpuBuildDisplayGammaRamp 82105278 / ...PWL 82105458, pass their frame's sp+0x58 (type, read by lwz and
// compared with 1 / 2 / 3: sRGB, BT.709, the power curve - any other value takes the BT.709 branch here) and sp+0x50
// (power, lfs, read only for type 3). Exactly those 8 bytes are written: until native/boot-gaps (2026-10-16) this stub
// wrote a 512-byte linear ramp through EACH pointer - sp+0x50..sp+0x257 of the builder's 0x8F0-byte frame (the
// builder writes every word of that span it reads before reading it; the type read back 0x04040505: the BT.709
// branch, as 2 - the same ramp). tools/difftest/swap_sdk_test.cpp's boot gaps section checks the two words and the
// canaries around them.
static uint32_t stub_VdGetCurrentDisplayGamma(uint32_t r3, uint32_t r4, uint32_t r5,
                                                uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r5; (void)r6; (void)r7; (void)r8;
  constexpr uint32_t kDisplayGammaType = 2u;        // BT.709 (Xenia's default)
  constexpr float kDisplayGammaPower = 2.22222233f;  // read by the guest only for type 3
  if (r3 != 0) writeGuest32(r3, kDisplayGammaType);
  if (r4 != 0) {
    uint32_t bits = 0;
    std::memcpy(&bits, &kDisplayGammaPower, sizeof(bits));
    writeGuest32(r4, bits);
  }
  STUB_LOG("VdGetCurrentDisplayGamma", r3, r4, r5, r6, r7, r8, kStatusSuccess);
  return kStatusSuccess;  // VOID: the callers reload r3 before any use
}

// VdInitializeRingBuffer: the game tells the GPU where the command ring buffer is.
// iter-178: r3 = MmGetPhysicalAddress(ring_buf_VA). With our identity-mapped
// stub_MmGetPhysicalAddress returning the input VA, r3 IS the ring buffer guest VA.
// Fallback: also check r6 for older code paths. Both checked before g_ringBufBase update.
static uint32_t stub_VdInitializeRingBuffer(uint32_t r3, uint32_t r4, uint32_t r5,
                                             uint32_t r6, uint32_t r7, uint32_t r8) {
#if !SR_ORACLE
  // The native submission service consumes recorded spans, never a console ring.
  // Keep the imported SDK ABI; it has no hardware state to initialize.
  (void)r3; (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  return kStatusSuccess;
#else
  SR_LOG_KERNEL_IMPORT("VdInitializeRingBuffer(r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X r7=0x%08X r8=0x%08X)",
                       r3, r4, r5, r6, r7, r8);
  // r3 = MmGetPhysicalAddress(ring_VA) — returns 0 in our build (stub_generic_success).
  // r6 was the old candidate (sometimes holds ring base from earlier code paths).
  // g_gpuRingAllocVA: set by gpu_ring_alloc_fix when sub_82467F40 allocates ring memory.
  if (vdRingFromArgOn()) {
    // r3 = the ring's PHYSICAL address, r4 = log2(size in bytes) - 3 (Xenia). The guest allocated it
    // through MmAllocatePhysicalMemoryEx, i.e. in the 0xA physical window: VA = 0xA0000000 + phys.
    const uint32_t phys = r3 & 0x1FFFFFFFu;
    const uint32_t bytes = r4 < 29u ? (1u << (r4 + 3u)) : 0u;
    if (r3 != 0 && bytes != 0) {
      const uint32_t ringVA = 0xA0000000u + phys;
      g_ringBufBase.store(ringVA, std::memory_order_release);
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[VD_RING] SR_VD_RING_ARG: ring phys=0x%08X bytes=0x%X -> VA 0x%08X (from the argument; no claim)",
                  phys, bytes, ringVA);
    } else {
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[VD_RING] SR_VD_RING_ARG: VdInitializeRingBuffer(r3=0x%08X r4=0x%08X) carries no ring "
                  "(is SR_MM_PHYSADDR on?) - g_ringBufBase unchanged", r3, r4);
    }
    return kStatusSuccess;
  }
  uint32_t ringVA = 0;
  if (r3 >= 0x82000000u && r3 < 0xD0000000u) ringVA = r3;
  else if (r6 >= 0x82000000u && r6 < 0xD0000000u) ringVA = r6;
  else {
    uint32_t allocVA = g_gpuRingAllocVA.load(std::memory_order_acquire);
    if (allocVA >= 0x82000000u && allocVA < 0xD0000000u) ringVA = allocVA;
  }

  if (ringVA != 0) {
    g_ringBufBase.store(ringVA, std::memory_order_release);
    SR_LOG_KERNEL_IMPORT("[VdInitializeRingBuffer] ring base 0x%08X -> g_ringBufBase (r3=0x%08X r6=0x%08X allocVA=0x%08X)",
                         ringVA, r3, r6, g_gpuRingAllocVA.load());
  } else {
    SR_LOG_KERNEL_IMPORT("[VdInitializeRingBuffer] no valid ring VA — g_ringBufBase unchanged (0x%08X)",
                         g_ringBufBase.load(std::memory_order_relaxed));
  }
  return kStatusSuccess;
#endif
}

// VdInitializeScalerCommandBuffer: the game passes scaler parameters.
// On real hardware, this writes GPU commands for the display scaler.
// r3 and r4 seem to be width/height related, r5/r6 flags. Return success.
static uint32_t stub_VdInitializeScalerCommandBuffer(uint32_t r3, uint32_t r4, uint32_t r5,
                                                      uint32_t r6, uint32_t r7, uint32_t r8) {
  SR_LOG_KERNEL_IMPORT("VdInitializeScalerCommandBuffer(r3=0x%08X r4=0x%08X r7=0x%08X r8=0x%08X)",
                       r3, r4, r7, r8);
  (void)r5; (void)r6;
  return kStatusSuccess;
}

// Device metadata may be mapped but inaccessible. Bounds checks on a guest VA
// do not make a direct read fault-free, so VdSwap resolves it through the OS.
static uint32_t vdSwapReadDeviceWord(uint32_t va, uint32_t offset = 0u) {
  if (!va || uint64_t(va) + offset + 4u > 0x100000000ull) return 0u;
  const uint32_t address = va + offset;
  const uint8_t* first = guestVAToHost(address);
  const uint8_t* last = guestVAToHost(address + 3u);
  uint8_t bytes[4];
  SIZE_T copied = 0;
  if (!first || !last || last - first != 3 ||
      !ReadProcessMemory(GetCurrentProcess(), first, bytes, sizeof(bytes), &copied) ||
      copied != sizeof(bytes)) return 0u;
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
         (uint32_t(bytes[2]) << 8) | bytes[3];
}

// VdSwap: game presents a frame.
// Per Xenia (xboxkrnl_video.cc): VdSwap(buffer_ptr, fetch_ptr, unk2, unk3,
//   unk4, frontbuffer_ptr, texture_format_ptr, color_space_ptr, width, height)
// r3=buffer_ptr (ring buffer), r4=fetch_ptr (D3D9 texture header),
// r5=writeback, r6=cmd buffer, r7=cmd buffer token,
// r8=frontbuffer_ptr (ptr to front buffer VA).
// Width/height are on the guest stack (params 9-10) -- read by import glue.
//
// Since we don't have a GPU command processor yet, the front buffer memory
// is uninitialized. We increment the frame counter so the host can show
// that VdSwap is being called and present a "first frame" indicator.
static uint32_t stub_VdSwap(uint32_t r3, uint32_t r4, uint32_t r5,
                             uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r6;
  sr::perf::onGuestSwap();  // iter-464 perf (observe-only; one bool test when off)
  sr::kernel::diagSwapNoteGuest();  // step 6 G6a DIAG [DIAG_SWAP] (dev builds; a no-op in release)
  incrementVdSwapCount();
  uint64_t n = getVdSwapCount();
  sr::kernel::highfps::onGuestSwap();   // iter-464 [HIGHFPS_PACE] observer (reads only)
  sr::abt::onGuestSwap();               // iter-467 native-a: SR_AB_PRESENT - the in-roll A/B flips per present
  if (sr::native::p2trace::g_on) sr::native::p2trace::onPresent();  // iter-467 native-a P2 trace (observe)
  sr::native::exec::profPlace(sr::native::exec::kPlGuestSwap);  // iter-467 SR_NATIVE_EXEC_PROF (observe-only)
  sr::native::meter::onFrame();         // iter-467 native meter (observe-only)
  sr::hitch::onGuestSwap();             // iter-466 stutter: SR_HITCH_LOG (no-op when off)
  // Reuse the watchdog's device resolution for the optional native block and
  // diagnostics; the gate-OFF export gains no additional guest dependency.
  const uint32_t deviceCell = vdSwapReadDeviceWord(0x82000948u);
  const uint32_t device = deviceCell ? vdSwapReadDeviceWord(deviceCell) : 0u;
#if SR_ORACLE
  // iter-467 p2 divergence (1) witness (observe-only, SR_VD_SYSBUF_LOG=1): the system command buffer
  // descriptor Swap passes here as r6 (= VdGetSystemCommandBuffer's r3), r7 (= its *r4), and what the
  // previous Swap copied into dev+0x4E80 (descriptor+4) / dev+0x4FB8 (descriptor+8, when non-zero).
  // descriptor+8 != 0 is the gate of Swap's scaler / gamma emission.
  {
    static const bool s_sysbufLog = [] {
      const char* e = std::getenv("SR_VD_SYSBUF_LOG");
      return e && e[0] == '1';
    }();
    if (s_sysbufLog && r6 && isGuestVAInRange(r6) && isGuestVAInRange(r6 + 0x93u)) {
      static uint64_t s_nz8 = 0, s_seen = 0;
      const uint32_t d0 = readGuestU32BE(r6), d4 = readGuestU32BE(r6 + 4u), d8 = readGuestU32BE(r6 + 8u),
                     d28 = readGuestU32BE(r6 + 28u);
      s_seen++;
      if (d8) s_nz8++;
      const uint32_t f4e80 = vdSwapReadDeviceWord(device, 0x4E80u);
      const uint32_t f4fb8 = vdSwapReadDeviceWord(device, 0x4FB8u);
      if (s_seen <= 8 || (s_seen & 2047u) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[VD_SYSBUF] swap=%llu desc=%08X +0=%08X +4=%08X +8=%08X +28=%08X r7=%08X dev+4E80=%08X "
                    "dev+4FB8=%08X nz8=%llu/%llu xenia=%d",
                    (unsigned long long)n, r6, d0, d4, d8, d28, r7, f4e80, f4fb8,
                    (unsigned long long)s_nz8, (unsigned long long)s_seen, vdSysCmdBufXenia() ? 1 : 0);
    }
  }
#endif
  // P2 async command processor: VdSwap does exactly what Xenia's VdSwap_entry
  // does (write the swap block) and nothing else — completion (fences, rptr,
  // interrupts) comes from the CP reaching the packets. Every "GPU finished"
  // shortcut below is skipped on this path.
#if SR_ORACLE
  const bool cpAsync = sr::kernel::gpuCpActive();
#else
  constexpr bool cpAsync = true;
  if (!sr::kernel::gpuCpActive()) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_DISPLAY_BAD] swap without the native submission service");
    std::_Exit(3);
  }
#endif
  // iter-463 (GPU_HANG_WD) latch (1): global -> cell -> device, resolved above.
  if (deviceCell) sr::kernel::gpuHangWdNoteDevice(device);

#if SR_ORACLE
  // iter-378 PLATFORM FIX: on real hardware the GPU sets bit 1 of the D3D
  // device's render-active byte (+10810) while rendering; the render-work
  // section of VdInterruptPumpWait_sub_82110A30 is gated on it. That byte is
  // PLATFORM-owned device state, so the platform sets it — this retires the
  // injected guest-code write in ppc_recomp.2.cpp:14217 (now master-gated),
  // which was the FOURTH GPU-signal compensation found by the iter-376 triage.
  // Device resolution is the proven stub_VdSwap path: global 0x82000948 -> ptr
  // -> device. The metadata resolution is fault-free; the device is game-owned.
  if (!cpAsync) {
    const uint32_t dev = device;
    if (dev && sr::kernel::isGuestVAInRange(dev + 10810u)) {
      uint8_t* b = sr::kernel::guestVAToHost(dev + 10810u);
      if (b && !(*b & 0x02)) {
        *b |= 0x02;
        SR_LOG_KERNEL_IMPORT("[VdSwap] render-active bit set on device 0x%08X (+10810)", dev);
      }
    }
  }

#endif

#if SR_ORACLE
  // Record the render thread on first VdSwap using native thread ID for reliability.
  DWORD expected = 0;
  DWORD nativeId = GetCurrentThreadId();
  if (g_renderThreadNativeId.compare_exchange_strong(expected, nativeId, std::memory_order_relaxed)) {
    SR_LOG_KERNEL_IMPORT("[GPU_AUTOTRACK] Render thread identified: nativeId=%lu", (unsigned long)nativeId);
  }

#endif

#if SR_ORACLE
  // r5 = writeback_ptr: guest VA where the GPU writes a completion value.
  // On real HW the GPU writes this when command processing finishes.
  // The game polls this location to know when the frame's GPU work is done.
  // Write the current swap count so the game sees the GPU as "done".
  if (r5 != 0 && !cpAsync) {
    uint8_t* wb = guestVAToHost(r5);
    if (wb) {
      // Write as big-endian uint32 (PPC convention)
      uint32_t val = static_cast<uint32_t>(n);
      wb[0] = (val >> 24) & 0xFF;
      wb[1] = (val >> 16) & 0xFF;
      wb[2] = (val >> 8) & 0xFF;
      wb[3] = val & 0xFF;
      if (n <= 5)
        SR_LOG_KERNEL_IMPORT("[VdSwap] wrote writeback=0x%08X val=%u at r5=0x%08X",
                             val, val, r5);
    }
  }

  // Main fence at 0x9480C000: the game may poll this until it reaches the "next" frame value
  // before calling NtWait(9). Write n+2 so a "wait for fence >= next" poll passes even if the
  // game expects one frame ahead (render thread stalls after VdSwap #4 otherwise).
  constexpr uint32_t kMainFenceVA = 0x9480C000u;
  constexpr uint32_t kFenceVA2   = 0x9480C004u;
  uint32_t nextVal = static_cast<uint32_t>(n + 2);
  if (!cpAsync) {
    uint32_t curFence = readGuestU32BE(kMainFenceVA);
    if (nextVal > curFence) {
      writeGuest32(kMainFenceVA, nextVal);
    }
  }
  // Also advance 0x9480C004 — the game may poll either fence location.
  if (!cpAsync) {
    uint32_t cur4 = readGuestU32BE(kFenceVA2);
    if (nextVal > cur4) {
      writeGuest32(kFenceVA2, nextVal);
    }
  }

#endif

  // r7 = cmd_buffer_token: the GPU also writes this token to signal completion.
  // Some games check this token alongside the writeback.
  // We don't know the exact semantics yet, but log it for diagnostics.
  if (n <= 5)
    SR_LOG_KERNEL_IMPORT("[VdSwap] r5=0x%08X r7=0x%08X (writeback/token)", r5, r7);

#if SR_ORACLE
  // r8 = frontbuffer_ptr: a guest VA pointing to the actual front buffer address.
  // Read the front buffer VA (big-endian uint32) from guest memory.
  uint32_t fbVA = 0;
  if (r8 != 0) {
    uint8_t* host = guestVAToHost(r8);
    if (host) {
      fbVA = (host[0] << 24) | (host[1] << 16) | (host[2] << 8) | host[3];
      // iter-169: do NOT write testFrontBufferVA back into the game's struct.
      // Writing a non-zero VA poisons the descriptor: the game's EDRAM setup
      // checks "if fbVA != 0, skip allocation" and never writes 0xF4FF0000.
      // Just substitute locally for our D3D12 present path.
      if (fbVA == 0) {
        uint32_t testFB = getTestFrontBufferVA();
        if (testFB != 0)
          fbVA = testFB;
      }
    }
  }

  if (n <= 100 || (n % 10 == 0)) {
    SR_LOG_KERNEL_IMPORT("[VdSwap] #%" PRIu64 " frontbuffer_ptr=0x%08X -> fbVA=0x%08X  r3=0x%08X r4=0x%08X",
                         n, r8, fbVA, r3, r4);
  }

  // Diagnostic: read device fields and scan for PM4 draw commands.
  //
  // iter-371: this census is the ONLY guest-side measurement of how much PM4 the
  // game actually builds per frame — (dev+40 cursor) - (dev+13508 segment start)
  // is the segment body, and dev+10780 is the guest's own fence counter. It is
  // completely independent of our PM4 parser, which makes it the arbiter when a
  // parser reading is in doubt. Capped at n<=10 it only ever described the first
  // ten swaps AT BOOT, so every later frame was unmeasured on the guest side.
  // Sample periodically instead: the whole point is to see whether the segment
  // body grows (and the fence delta exceeds 2) once the game has content.
  if (n <= 10 || (n % 30 == 0)) {
    const uint32_t actualDevVA = device;
    if (actualDevVA != 0) {
      uint32_t dev40 = readGuestU32BE(actualDevVA + 40);
      uint32_t dev44 = readGuestU32BE(actualDevVA + 44);
      uint32_t dev48 = readGuestU32BE(actualDevVA + 48);
      uint32_t dev10768 = readGuestU32BE(actualDevVA + 10768);
      uint32_t dev10780 = readGuestU32BE(actualDevVA + 10780);
      uint32_t dev13508 = readGuestU32BE(actualDevVA + 13508);
      SR_LOG_KERNEL_IMPORT("[VdSwap] dev=0x%08X +40=0x%08X +44=0x%08X +48=0x%08X +10768=0x%08X +10780=0x%08X +13508=0x%08X",
                           actualDevVA, dev40, dev44, dev48, dev10768, dev10780, dev13508);

#if SR_ORACLE
      // Scan secondary command buffer (device+40 region) for PM4 type-3 packets
      if (n == 5 && dev40 < dev44) {
        uint32_t scanStart = (dev40 > 0x100) ? (dev40 - 0x100) : 0;
        uint8_t* host = guestVAToHost(scanStart);
        if (host) {
          uint32_t scanLen = dev44 - scanStart;
          if (scanLen > 0x2000) scanLen = 0x2000;
          int type3 = 0, draws = 0, nops = 0;
          uint32_t opcodeHist[256] = {};
          for (uint32_t off = 0; off + 4 <= scanLen; ) {
            uint32_t hdr = (uint32_t(host[off])<<24)|(uint32_t(host[off+1])<<16)|(uint32_t(host[off+2])<<8)|host[off+3];
            if (hdr == 0) { nops++; off += 4; continue; }
            uint32_t type = (hdr >> 30) & 3;
            if (type == 3) {
              uint32_t opc = (hdr >> 8) & 0xFF;
              uint32_t cnt = ((hdr >> 16) & 0x3FFF) + 1;
              size_t pktSz = (1 + cnt) * 4;
              if (off + pktSz > scanLen) break;
              type3++; opcodeHist[opc]++;
              if (opc==0x22||opc==0x23||opc==0x27||opc==0x2D) draws++;
              off += static_cast<uint32_t>(pktSz);
            } else { off += 4; }
          }
          SR_LOG_KERNEL_IMPORT("[CMD_SCAN] secBuf 0x%08X-0x%08X: type3=%d draws=%d nops=%d",
            scanStart, dev44, type3, draws, nops);
          for (int i = 0; i < 256; i++)
            if (opcodeHist[i]) SR_LOG_KERNEL_IMPORT("[CMD_SCAN]   opc 0x%02X: %u", i, opcodeHist[i]);
        }
      }
#endif
    }
  }

  // Diagnostic: the 24-byte front-buffer texture fetch constant at r4.
  //
  // iter-369c: log on CHANGE, not just the first 5 frames. The console derives
  // the scanned-out surface ENTIRELY from this fetch (Xenia RequestSwapTexture),
  // and the golden trace carries a populated one:
  //   fbPhys=0x1F130000 1280x720 pitch=1280 tiled=1 fmt=6 swiz=0xA0A fetchValid=1
  // At boot ours is empty (D0..D5 = 0 except D3=0x1414), so type bits[1:0] != 2
  // (kTexture) and the decode rightly rejects it. But [r8]'s plain u32 DOES go
  // non-zero at swap ~#1700, so the open question is whether the game ever
  // populates this fetch later. A first-5-frames-only dump cannot answer that —
  // that sampling blind spot is what made the boot dumps misleading before.
  // Bounded: only distinct values, capped, so it cannot spam the hot path.
  if (r4 != 0) {
    uint8_t* fetchHost = guestVAToHost(r4);
    if (fetchHost) {
      uint32_t d[6];
      for (int i = 0; i < 6; i++)
        d[i] = (fetchHost[i*4] << 24) | (fetchHost[i*4+1] << 16) |
               (fetchHost[i*4+2] << 8) | fetchHost[i*4+3];
      static uint32_t s_lastFetch[6] = {0xFFFFFFFFu};
      static std::atomic<uint32_t> s_fetchLogs{0};
      bool changed = false;
      for (int i = 0; i < 6; i++) if (d[i] != s_lastFetch[i]) { changed = true; break; }
      if ((changed || n <= 5) && s_fetchLogs.fetch_add(1, std::memory_order_relaxed) < 40) {
        for (int i = 0; i < 6; i++) s_lastFetch[i] = d[i];
        const uint32_t type = d[0] & 3u;
        SR_LOG_KERNEL_IMPORT(
          "[VdSwap] fetch@0x%08X #%" PRIu64 ": D0=%08X D1=%08X D2=%08X D3=%08X D4=%08X D5=%08X"
          " type=%u %s",
          r4, n, d[0], d[1], d[2], d[3], d[4], d[5], type,
          type == 2u ? "VALID(kTexture)" : "INVALID — not a texture fetch");
      }
    }
  }

  // Diagnostic: dump 32 bytes around frontbuffer_ptr at r8 (first 5 frames)
  if (n <= 5 && r8 != 0) {
    uint8_t* structHost = guestVAToHost(r8);
    if (structHost) {
      uint32_t s[8];
      for (int i = 0; i < 8; i++)
        s[i] = (structHost[i*4] << 24) | (structHost[i*4+1] << 16) |
               (structHost[i*4+2] << 8) | structHost[i*4+3];
      SR_LOG_KERNEL_IMPORT("[VdSwap] struct@0x%08X: %08X %08X %08X %08X  %08X %08X %08X %08X",
        r8, s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
    }
  }

#endif

  // iter-358i: decode the D3D9 front-buffer texture fetch constant at r4 and
  // honor it when valid — Xenia's swap path derives EVERYTHING from this fetch
  // (RequestSwapTexture re-reads fetch slot 0; the PM4 payload ptr/w/h are
  // informational). Layout: xe_gpu_texture_fetch_t, xenos.h:1229-1330 —
  // per-dword LSB-first after the BE read:
  //   D0: type bits[1:0] (2 = kTexture required), pitch bits[30:22] (texels>>5),
  //       tiled bit 31
  //   D1: format bits[5:0], endianness bits[7:6], base_address bits[31:12] (>>12)
  //   D2: size_2d width bits[12:0], height bits[25:13] (both stored minus 1)
  // NOTE (observed live, iter-358i): the game passes an all-zero fetch at this
  // boot stage — it has not created its display surface yet (pre-menu). The
  // decode is inert until the surface exists; fallbacks preserved.
  uint32_t fbW = 1280, fbH = 720, fbPitchBytes = 1280 * 4, fetchFbVA = 0;
  bool fbTiled = false;
  if (r4 != 0) {
    uint8_t* f = guestVAToHost(r4);
    if (f) {
      const uint32_t d0 = readGuestU32BE(r4);
      const uint32_t d1 = readGuestU32BE(r4 + 4);
      const uint32_t d2 = readGuestU32BE(r4 + 8);
      const uint32_t fetchType = d0 & 3u;
      const uint32_t basePage = (d1 >> 12) & 0xFFFFFu;
      if (fetchType == 2u && basePage != 0u) {
        // D3D9 header carries the CPU-virtual page (Xenia translates to
        // physical; identity under our phys-window mapping).
        fetchFbVA = basePage << 12;
        fbW = (d2 & 0x1FFFu) + 1u;
        fbH = ((d2 >> 13) & 0x1FFFu) + 1u;
        const uint32_t pitchTexels = ((d0 >> 22) & 0x1FFu) << 5;
        fbPitchBytes = (pitchTexels >= fbW ? pitchTexels : fbW) * 4u;
        fbTiled = ((d0 >> 31) & 1u) != 0;
        const uint32_t format = d1 & 0x3Fu;
        static int s_fetchDecodeLog = 0;
        if (++s_fetchDecodeLog <= 8)
          SR_LOG_KERNEL_IMPORT("[VdSwap] fetch decode: fbVA=0x%08X %ux%u pitch=%uB fmt=%u tiled=%u",
                               fetchFbVA, fbW, fbH, fbPitchBytes, format, fbTiled ? 1 : 0);
      }
    }
  }
#if SR_ORACLE
  // Register the front buffer so the host can display it. Preference order:
  // the swap fetch constant (the console's source of truth) > the r8-pointed
  // front buffer VA > our test-pattern fallback. The tiled flag only describes
  // the fetch-decoded surface.
  uint32_t effectiveFB = (fetchFbVA != 0) ? fetchFbVA
                       : (fbVA != 0) ? fbVA : getTestFrontBufferVA();
  if (effectiveFB != 0) {
    setGuestFramebufferVA(effectiveFB, static_cast<int>(fbW), static_cast<int>(fbH),
                          static_cast<int>(fbPitchBytes),
                          fbTiled && effectiveFB == fetchFbVA);
  }
#endif
  // P2 async CP: Xenia VdSwap_entry (xboxkrnl_video.cc:466-548), exactly. The
  // caller reserved 64 dwords at r3 (inside the command buffer the runtime is
  // about to kick): zero them, write fetch constant 0 with the base translated
  // to PHYSICAL, the PM4_XE_SWAP packet {'SWAP', fb phys, w, h}, and type-2
  // NOP fill. The CP presents when it reaches that packet.
  if (cpAsync) {
    if (sr::native::flush::swapBlockOn() &&
        sr::native::flush::swapBlockLeaf(device, r3, r4, fbW, fbH)) {
      // The native item presents when the consumer reaches it. All export
      // observers above and the frame notification below still run once.
#if SR_ORACLE
    } else if (r3 >= 0x80000000u && guestVAToHost(r3) && guestVAToHost(r3 + 63u * 4u)) {
      for (uint32_t i = 0; i < 64u; i++) writeGuest32(r3 + i * 4u, 0);
      uint32_t idx = 0;
      uint32_t fbPhys = 0;
      if (r4 != 0 && guestVAToHost(r4)) {
        uint32_t d[6];
        for (int i = 0; i < 6; i++) d[i] = readGuestU32BE(r4 + uint32_t(i) * 4u);
        // base_address (dword1 bits 12-31) is the CPU-virtual page here; the
        // GPU works with physical addresses (Xenia GetPhysicalAddress).
        fbPhys = (d[1] & 0xFFFFF000u) & 0x1FFFFFFFu;
        d[1] = (d[1] & 0x00000FFFu) | fbPhys;
        writeGuest32(r3 + idx++ * 4u, 0x00054800u);  // type-0: 6 regs at 0x4800
        for (int i = 0; i < 6; i++) writeGuest32(r3 + idx++ * 4u, d[i]);
      }
      writeGuest32(r3 + idx++ * 4u, 0xC0036400u);  // type-3 PM4_XE_SWAP, count 4
      writeGuest32(r3 + idx++ * 4u, 0x53574150u);  // 'SWAP'
      writeGuest32(r3 + idx++ * 4u, fbPhys);
      writeGuest32(r3 + idx++ * 4u, fbW);
      writeGuest32(r3 + idx++ * 4u, fbH);
      for (; idx < 64u; idx++) writeGuest32(r3 + idx * 4u, 0x80000000u);  // type-2 NOP
      if (n <= 3)
        SR_LOG_KERNEL_IMPORT("[VdSwap] async-CP swap block at r3=0x%08X fb=0x%08X %ux%u",
                             r3, fbPhys, fbW, fbH);
    } else if (n <= 10) {
      SR_LOG_KERNEL_IMPORT("[VdSwap] async-CP: r3=0x%08X is not a writable command-buffer VA",
                           r3);
#else
    } else {
      SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_DISPLAY_BAD] native swap block refused dev=%08X block=%08X", device, r3);
      std::_Exit(3);
#endif
    }
#if SR_ORACLE
    sr::gfx::onVdSwapFrame(r3, getGfxCommandBufferVA(), effectiveFB, n);
#else
    sr::gfx::onVdSwapFrame(0, 0, 0, n);
#endif
#ifdef _WIN32
    if (n == 50) {
      void stopPeriodicImageRestore();
      stopPeriodicImageRestore();
      SR_LOG_KERNEL_IMPORT("[VdSwap] stopped periodic image restore at VdSwap #50");
    }
#endif
    return kStatusSuccess;
  }

#if SR_ORACLE
  // XDK ring buffer management passes either an absolute VA (>= 0x80000000) or a relative
  // offset from the ring base. iter-172: threshold raised from <0x10000 to <0x80000000 so
  // offsets larger than 64KB (e.g. 0x10580 at frame 109) are correctly fixed up.
  // iter-176: use g_ringBufBase (set by VdInitializeRingBuffer) instead of device+10768
  // (device+10768 now holds the RPTR writeback ptr for sub_820FD9A8's ring-wait loop).
  uint32_t wptr = r3;
  if (wptr != 0 && wptr < 0x80000000u) {
    uint32_t ringBase = g_ringBufBase.load(std::memory_order_acquire);
    if (ringBase >= 0x80000000u) {
      wptr = ringBase + r3;
      if (n <= 10 || (r3 > 0x10000 && n <= 50))
        SR_LOG_KERNEL_IMPORT("[VdSwap] WPTR fixup: r3=0x%08X + ringBase=0x%08X -> 0x%08X", r3, ringBase, wptr);
    } else {
      constexpr uint32_t kRingBufBase = 0xD0010000u;
      wptr = kRingBufBase + r3;
      if (n <= 20)
        SR_LOG_KERNEL_IMPORT("[VdSwap] WPTR fixup fallback: r3=0x%08X + hardcoded 0xD0010000 -> 0x%08X", r3, wptr);
    }
  }
  // iter-463 charter (burndown item 12, same class): the iter-176 write of
  // WPTR into "device+10780" of the FAKE device 0xD0000000 is DELETED - that
  // device went in iter-358f and nothing reads 0xD0002A1C. Xenia's VdSwap
  // writes only the swap packet at r3 (below).

  // Xenia parity (xboxkrnl_video.cc VdSwap_entry): the caller reserves 64
  // dwords AT r3 for VdSwap itself to write swap packets into — the console
  // kernel writes a type-0 SHADER_CONSTANT_FETCH_00_0 packet carrying the
  // front-buffer fetch constant (from r4), then filler. Without this, the
  // reserved block stays whatever the game left there (zeros, iter-358h
  // RING_DUMP) and every ring scan anchored at a previous r3 starts on
  // non-packet data. Write the fetch packet + type-2 NOP fill so the stream
  // is contiguous valid PM4 across the swap boundary.
  //
  // Note on addresses: Xenia rewrites the fetch's base_address from CPU-
  // virtual to physical. Our PM4 engine maps physical P at VA 0x80000000 +
  // (P & 0x1FFFFFFF), under which the VA-based page number in r4's header
  // resolves to the same host memory — so the 6 dwords are copied unmodified.
  if (wptr >= 0x80000000u && wptr < 0xF0000000u) {
    uint8_t* blk = guestVAToHost(wptr);
    uint8_t* blkEnd = guestVAToHost(wptr + 64u * 4u - 4u);
    if (blk && blkEnd && blkEnd > blk) {
      uint32_t idx = 0;
      uint8_t* fetch = (r4 != 0) ? guestVAToHost(r4) : nullptr;
      if (fetch) {
        // Type-0 header: ((count-1)<<16) | baseReg; 6 dwords at 0x4800.
        writeGuest32(wptr, 0x00054800u);
        std::memcpy(blk + 4, fetch, 24);  // 6 BE dwords, already guest order
        idx = 7;
      }
      for (; idx < 64u; idx++)
        writeGuest32(wptr + idx * 4u, 0x80000000u);  // type-2 NOP packet
      if (n <= 3)
        SR_LOG_KERNEL_IMPORT("[VdSwap] wrote reserved swap block at r3=0x%08X (%s)",
                             wptr, fetch ? "fetch+NOPs" : "NOPs only");
    }
  }

  // Parse the PM4 stream the game wrote since the previous swap.
  sr::gfx::onVdSwapFrame(wptr, getGfxCommandBufferVA(), effectiveFB, n);

  // Advance ring buffer RPTR to current WPTR.
  // This simulates the GPU instantly consuming all commands, preventing the ring buffer
  // from filling up and blocking the render thread after ~34 frames.
  // Accept WPTR from any valid guest address range (real ring, fake ring, or phys alloc).
  // iter-172: expanded from narrow 0x8C.../0xD001... to cover the physical memory pool
  // (0x80000000-0xB4000000) where VdInitializeRingBuffer's real ring buffer lives.
  bool validWptr = wptr >= 0x80000000u && wptr < 0xE0000000u;
  if (validWptr) {
    g_lastWptrFromVdSwap.store(wptr, std::memory_order_release);
  } else if (n > 1) {
    static int _badWptr = 0;
    if (_badWptr++ < 10)
      SR_LOG_KERNEL_IMPORT("[VdSwap] BAD WPTR wptr=0x%08X (r3=0x%08X) frame=%" PRIu64 " – keeping old RPTR", wptr, r3, n);
  }
  // iter-463 charter: everything below on this LEGACY synchronous path
  // (SR_PM4_CP=0 / SR_PM4_LIVE=0, a renderer A/B knob) fakes GPU completion -
  // RPTR := WPTR as if the GPU consumed everything, a release of every tracked
  // GPU semaphore, the fake 16 ms GPU IRQ thread, the VD pump KEVENT signal.
  // The console contract is the CP writing rptr as it consumes and the guest's
  // own ISR signalling (the async CP does exactly that). All master-gated, so a
  // renderer A/B never enables forcing; without SR_ALLOW_FORCING=1 the legacy
  // path parses the stream and presents but completes nothing.
  uint32_t rptrVA = g_rptrWritebackVA.load(std::memory_order_acquire);
  if (rptrVA != 0 && validWptr && sr::kernel::forcingAllowed()) {
    writeGuest32(rptrVA, wptr);
    if (n <= 3)
      SR_LOG_KERNEL_IMPORT("[RPTR] Advanced RPTR to WPTR=0x%08X at VA=0x%08X", wptr, rptrVA);
  }

#ifdef _WIN32
  if (sr::kernel::forcingAllowed()) {
    signalGpuCompletionSemaphores();
    // Start the GPU interrupt thread on first VdSwap.
    if (n == 1) {
      startGpuInterruptThread();
    }
  }
  // FIX iter-164: The FM module walker (walkFmModuleCtors_impl) calls
  // stopPeriodicImageRestore() when FMGameRenderInit completes. But the walker
  // runs on the main init thread, which is permanently blocked in sub_8218CFE8
  // (init2 never returns). Without this, the 1s periodic restore overwrites
  // 8MB of game binary every second for the entire run — destroying runtime
  // state (vtables, pointers, counters) and throttling VdSwap from 30 Hz → 2 Hz.
  if (n == 50) {
    // Forward decl: defined in kernel/image_write_watchdog.cpp.
    void stopPeriodicImageRestore();
    stopPeriodicImageRestore();
    SR_LOG_KERNEL_IMPORT("[VdSwap] stopped periodic image restore at VdSwap #50");
  }
#endif

  // NOTE: Previously we signaled the render thread event (0x83EEC7CC) each frame.
  // This caused the render thread to wake with STATUS_SUCCESS instead of STATUS_TIMEOUT,
  // which skips the render work section (sub_82111EC8 + sub_82112050 → VdSwap).
  // Now we let the render thread timeout naturally so it always enters render work.
  // The event signal is only needed on real HW where the GPU DPC signals frame-done.

  // iter-346 (charter platform fix): CONDITIONAL pump signaling — the missing
  // GPU DPC delivery. sub_82110840 (the pump bottom-half) is a lock-free ring
  // consumer draining D3D's deferred-callback queue (threadObj[0x14]/[0x16]);
  // on hardware the GPU interrupt signals the pump event when work is queued.
  // We signal ONLY when the ring is non-empty, so the timeout-poll leg (the
  // scanout work the old unconditional signal starved) still runs between
  // swaps. SR_VD_PUMPSIG=0 disables for A/B.
  {
    static const bool s_pumpSig = [] {
      const char* e = std::getenv("SR_VD_PUMPSIG");
      return !e || e[0] != '0';
    }();
    const uint32_t pctx = g_vdPumpCtxVA.load(std::memory_order_acquire);
    if (s_pumpSig && pctx >= 0x10000u && sr::kernel::forcingAllowed()) {
      const uint32_t tobj = readGuestU32BE(pctx);
      if (tobj >= 0x10000u) {
        const uint32_t pend14 = readGuestU32BE(tobj + 0x50u);
        const uint32_t pend16 = readGuestU32BE(tobj + 0x58u);
        if (pend14 != 0 || pend16 != 0) {
          stub_KeSetEvent(pctx + 8u, 0, 0, 0, 0, 0);
          static std::atomic<unsigned> s_sig{0};
          unsigned sn = s_sig.fetch_add(1, std::memory_order_relaxed) + 1;
          if (sn <= 6 || (sn & (sn - 1)) == 0)
            SR_LOG_KERNEL_IMPORT("[VD_PUMP] DPC signal #%u (ring pend14=%08X pend16=%08X)",
                                 sn, pend14, pend16);
        }
      }
    }
  }

  return kStatusSuccess;
#else
  std::_Exit(3);  // cpAsync is constant true in release
#endif
}

// Xex: return non-zero module handle so game thinks module is loaded.
static uint32_t stub_XexGetModuleHandle(uint32_t r3, uint32_t r4, uint32_t r5,
                                        uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r3;
  (void)r4;
  (void)r5;
  (void)r6;
  (void)r7;
  (void)r8;
  return 1;  // fake handle
}

// Xex: return 0 = procedure not found (game may have fallback).
static uint32_t stub_XexGetProcedureAddress(uint32_t r3, uint32_t r4, uint32_t r5,
                                            uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r3;
  (void)r4;
  (void)r5;
  (void)r6;
  (void)r7;
  (void)r8;
  return 0;
}

// KeTlsAlloc returns TLS index; 0 can mean failure.
static uint32_t stub_KeTlsAlloc(uint32_t r3, uint32_t r4, uint32_t r5,
                                uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r3; (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  uint32_t index = g_nextTlsIndex.fetch_add(1);
  if (index >= kMaxTlsSlots) {
    SR_LOG_KERNEL_IMPORT("[TLS] KeTlsAlloc: no more slots (index=%u)", index);
    return 0;  // No more slots
  }
  g_tlsValues[index] = 0;  // Initialize to 0 for this thread
  SR_LOG_KERNEL_IMPORT("[TLS] KeTlsAlloc -> index=%u", index);
  return index;
}

// KeTlsSetValue(index, value): store value at TLS index (per-thread)
static uint32_t stub_KeTlsSetValue(uint32_t r3, uint32_t r4, uint32_t r5,
                                   uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r5; (void)r6; (void)r7; (void)r8;
  uint32_t index = r3;
  uint32_t value = r4;
  if (index < kMaxTlsSlots) {
    g_tlsValues[index] = value;
    static thread_local int s_logCount = 0;
    if (s_logCount++ < 5)
      SR_LOG_KERNEL_IMPORT("[TLS] KeTlsSetValue(index=%u, value=0x%08X) OK", index, value);
    return 1;  // TRUE = success
  }
  // Module-encoded index (e.g. 262151 = module 4 slot 7) — use overflow map.
  g_tlsOverflow[index] = value;
  static thread_local int s_ovfLogCount = 0;
  if (s_ovfLogCount++ < 5)
    SR_LOG_KERNEL_IMPORT("[TLS] KeTlsSetValue(index=%u, value=0x%08X) overflow OK", index, value);
  return 1;
}

// KeTlsGetValue(index): return per-thread value at TLS index
static uint32_t stub_KeTlsGetValue(uint32_t r3, uint32_t r4, uint32_t r5,
                                   uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  uint32_t index = r3;
  if (index < kMaxTlsSlots) {
    uint32_t val = g_tlsValues[index];
    static thread_local int s_logCount = 0;
    if (s_logCount++ < 5)
      SR_LOG_KERNEL_IMPORT("[TLS] KeTlsGetValue(index=%u) -> 0x%08X", index, val);
    return val;
  }
  // Module-encoded index — look up overflow map (returns 0 if not yet set, same as XDK).
  auto it = g_tlsOverflow.find(index);
  uint32_t oval = (it != g_tlsOverflow.end()) ? it->second : 0u;
  static thread_local int s_ovfLogCount = 0;
  if (s_ovfLogCount++ < 5)
    SR_LOG_KERNEL_IMPORT("[TLS] KeTlsGetValue(index=%u) overflow -> 0x%08X", index, oval);
  return oval;
}

// RtlImageXexHeaderField(ImageBase, FieldId): return pointer to field in xex header (guest VA).
// The game's init code (sub_8246A2A8) checks if the returned pointer's offset 0 is 0.
// If offset 0 is 0, it returns success. So we return a pointer to zeroed memory.
// Legacy arm (SR_XEX_HEAP_FIELD=0) returns 0xD001D000, the zeroed xex-header block the
// XexExecutableModuleHandle hmodule points at (initKernelDataStructures; iter-463 moved it
// out of .text, where it sat at 0x82100200).
static uint32_t stub_RtlImageXexHeaderField(uint32_t r3, uint32_t r4, uint32_t r5,
                                           uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r3;
  (void)r5;
  (void)r6;
  (void)r7;
  (void)r8;
  // iter-392 (SR_XEX_HEAP_FIELD=1, default OFF pending K-A/B): HONEST lookup.
  // The real default.xex has 14 optional headers and key 0x20401 (default
  // heap size) is NOT among them — console truth is "field absent" (return
  // NULL), which makes the CRT build its default 1MB process heap
  // (sub_8246A2A8: field==NULL -> v6=0x100000 -> sub_8246C4D0). The legacy
  // pointer-to-zero return below was based on a MISREADING of that CRT code:
  // *field==0 means "title declares NO process heap" and the CRT SKIPS
  // building it entirely (dword_829AC16C stays 0) — silently deleting the
  // game's CRT heap since the earliest sessions (fake-mstate 0x82110000 /
  // dlmalloc-fragility suspect chain). Honest path is opt-in until the
  // allocator-geometry A/B proves it; runtime caller census: exactly ONE
  // call per boot, r4=0x20401, from the CRT heap init.
  // Baked default-ON after the K=4 A/B (honest arm 4/4 deep, clean FE tails,
  // CRT heap built naturally at 0x839F0000 via guest NtAVM). =0 keeps the A/B.
  static const bool s_honest = [] {
    const char* e = std::getenv("SR_XEX_HEAP_FIELD");
    return !e || e[0] != '0';
  }();
  if (s_honest) {
    SR_LOG_KERNEL_IMPORT("[XEXFIELD] key=0x%X -> ABSENT (honest: CRT builds its default heap)", r4);
    return 0;
  }
  // Legacy: pointer to our zeroed xexHeader area ([result+0]==0 -> CRT skips the heap)
  return 0xD001D000u;  // kXexHeaderStubVA in initKernelDataStructures
}

// RtlInitAnsiString(PANSI_STRING DestinationString, PCSZ SourceString)
// Initialize an ANSI_STRING structure from a null-terminated C string.
// ANSI_STRING layout: Length(2), MaximumLength(2), Buffer(4)
static uint32_t stub_RtlInitAnsiString(uint32_t r3, uint32_t r4, uint32_t r5,
                                       uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r5;
  (void)r6;
  (void)r7;
  (void)r8;
  uint8_t* destHost = guestVAToHost(r3);  // PANSI_STRING
  uint8_t* srcHost = guestVAToHost(r4);   // PCSZ (null-terminated string)
  
  if (destHost) {
    if (srcHost && r4 != 0) {
      // Calculate string length (not including null terminator)
      size_t len = std::strlen(reinterpret_cast<const char*>(srcHost));
      if (len > 65534) len = 65534;  // Clamp to USHORT max - 1
      
      // Log interesting strings (paths, etc.) - first 20 and then every 1000th
      static int strCount = 0;
      if (++strCount <= 20 || (strCount % 1000 == 0)) {
        char buf[80] = {0};
        size_t copyLen = (len < 79) ? len : 79;
        std::memcpy(buf, srcHost, copyLen);
        SR_LOG_KERNEL_IMPORT("[STRING] #%d RtlInitAnsiString: \"%s\"", strCount, buf);
      }
      
      uint16_t length = static_cast<uint16_t>(len);
      uint16_t maxLength = static_cast<uint16_t>(len + 1);
      
      // Write ANSI_STRING structure in big-endian (matching PPC_STORE_U16/U32
      // which stores bswap'd values; guest memory is big-endian).
      // PPC_LOAD_U16/U32 will byte-swap back to get correct values.
      destHost[0] = (length >> 8) & 0xFF;
      destHost[1] = length & 0xFF;
      destHost[2] = (maxLength >> 8) & 0xFF;
      destHost[3] = maxLength & 0xFF;
      destHost[4] = (r4 >> 24) & 0xFF;
      destHost[5] = (r4 >> 16) & 0xFF;
      destHost[6] = (r4 >> 8) & 0xFF;
      destHost[7] = r4 & 0xFF;
    } else {
      // Null source string - zero out the structure
      uint16_t zero16 = 0;
      uint32_t zero32 = 0;
      std::memcpy(destHost + 0, &zero16, 2);
      std::memcpy(destHost + 2, &zero16, 2);
      std::memcpy(destHost + 4, &zero32, 4);
    }
  }
  // Don't log every call - this is called very frequently
  return kStatusSuccess;
}

// ---- Real critical section implementation ----
// Xbox 360 RTL_CRITICAL_SECTION supports recursive locking (same thread can
// enter multiple times). We map each guest CS address to a host recursive_mutex.

struct HostCriticalSection {
  // iter-440j THE BOOT-IDENT STARVATION ROOT-FIX: recursive_TIMED_mutex so
  // waiters BLOCK with kernel wakeup (try_lock_for) instead of try_lock +
  // Sleep(1) polling. The poll missed the release window ~always: the
  // render-script worker (thread 50) re-enters the manager CS microseconds
  // after releasing it, while the asset thread's poll ran at >=1ms
  // granularity — on console the waiter blocks in a real wait queue and is
  // woken AT release. Frida-proven live (iter-440j): holder progressing
  // through script passes, waiter never entering = the no-movie/boot-ident
  // hold class.
  std::recursive_timed_mutex mtx;
  std::atomic<std::thread::id> owner{};
  std::atomic<uint32_t> recursionCount{0};
  // iter-438f: name the holder when a waiter stalls — guest thread handle and
  // the guest LR at the moment of the last successful Enter.
  std::atomic<uint32_t> ownerHandle{0};
  std::atomic<uint32_t> ownerEnterLr{0};
  // iter-439: host tid of the owner — direct frida_thread_backtrace target.
  std::atomic<uint32_t> ownerHostTid{0};
  // step 6 I8 (Fable ruling 21 (3)): threads inside Enter's timed wait on THIS object (a re-init with waiters is
  // logged and skipped - it keeps the graveyard path)
  std::atomic<uint32_t> waiters{0};
};

static std::mutex s_csMapMutex;
static std::unordered_map<uint32_t, std::unique_ptr<HostCriticalSection>> s_csMap;

static HostCriticalSection& getOrCreateCSLocked(uint32_t guestAddr) {
  std::lock_guard<std::mutex> lock(s_csMapMutex);
  auto& ptr = s_csMap[guestAddr];
  if (!ptr) ptr = std::make_unique<HostCriticalSection>();
  return *ptr;
}

// iter-464 perf lane, SR_CS_FASTMAP (A/B gate): EVERY RtlEnter/Leave/TryEnterCriticalSection
// looked its host CS up in s_csMap under ONE process-wide std::mutex - so all guest threads'
// CS traffic (millions of calls per second) serialized on that mutex before even reaching
// their own CS. The fast path is a lock-free open-addressed cache in front of the map
// (65536 slots, 16-probe windows; the map stays the owner and the registry every census /
// dying-thread release iterates). A miss falls back to the map and then fills the cache.
// RtlInitializeCriticalSection re-creates a CS: under the gate the old host object moves to a
// graveyard instead of being destroyed, so a thread still holding the old pointer can never
// touch freed memory (the non-gated path destroys it - a latent use-after-free).
struct CsFastSlot {
  std::atomic<uint32_t> key{0};
  std::atomic<HostCriticalSection*> cs{nullptr};
};
static constexpr uint32_t kCsFastSlots = 65536;
static CsFastSlot s_csFast[kCsFastSlots];
static std::vector<std::unique_ptr<HostCriticalSection>> s_csGraveyard;  // guarded by s_csMapMutex

// iter-466 tick: baked default ON (=0 A/B) with SR_CS_TRYFIRST (PERFORMANCE 2.8).
static bool csFastMapOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_CS_FASTMAP");
    return !e || e[0] != '0';
  }();
  return v;
}

// The slot for `key` (claiming an empty one when `claim`), or nullptr.
static CsFastSlot* csFastSlot(uint32_t key, bool claim) {
  const uint32_t h = (key * 2654435761u) >> 16;
  for (uint32_t probe = 0; probe < 16; ++probe) {
    CsFastSlot& s = s_csFast[(h + probe) & (kCsFastSlots - 1)];
    uint32_t cur = s.key.load(std::memory_order_acquire);
    if (cur == key) return &s;
    if (cur == 0) {
      if (!claim) return nullptr;
      uint32_t expected = 0;
      if (s.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel) || expected == key)
        return &s;
    }
  }
  return nullptr;
}

static HostCriticalSection& getOrCreateCS(uint32_t guestAddr) {
  if (!csFastMapOn() || guestAddr == 0) return getOrCreateCSLocked(guestAddr);
  if (CsFastSlot* s = csFastSlot(guestAddr, false)) {
    if (HostCriticalSection* cs = s->cs.load(std::memory_order_acquire)) return *cs;
  }
  HostCriticalSection& cs = getOrCreateCSLocked(guestAddr);
  if (CsFastSlot* s = csFastSlot(guestAddr, true)) {
    HostCriticalSection* none = nullptr;
    s->cs.compare_exchange_strong(none, &cs, std::memory_order_acq_rel);
  }
  return cs;
}

// iter-432 (roll-stability): census body — see the exported
// sr::kernel::dumpCsOwnedByCurrentThread wrapper near the end of this file.
// Lists every guest CS the DYING thread still owns — these are the locks the
// death orphans (the exact mechanism that turned one AV into a full-boot
// deadlock in iter-428e/432a). Log-only, fault-free.
static void dumpCsOwnedByCurrentThreadImpl(const char* tag) {
  const std::thread::id self = std::this_thread::get_id();
  int owned = 0;
  std::lock_guard<std::mutex> lock(s_csMapMutex);
  for (const auto& kv : s_csMap) {
    const auto& cs = kv.second;
    if (cs && cs->owner.load(std::memory_order_relaxed) == self) {
      SR_LOG_BOOT(::sr::LogSeverity::Error,
          "[%s] dying thread OWNS cs=0x%08X rec=%u — ORPHANED (waiters will "
          "wedge forever)",
          tag, kv.first, cs->recursionCount.load(std::memory_order_relaxed));
      ++owned;
    }
  }
  if (!owned)
    SR_LOG_BOOT(::sr::LogSeverity::Error, "[%s] dying thread owns no guest CS",
                tag);
}

// iter-190: REAL locking for out-of-range critsec VAs.
// The iter-25a/32 garbage-CS filter returned success WITHOUT locking for any
// CS whose aliased VA fell outside [0x82000000, 0xB4000000). That left real
// critsecs with NO mutual exclusion — e.g. the render pump's CS at 0xD00034D4
// (fake-device window, identity-mapped) and anything in the fallback heap
// (0x10000-0x110000). iter-189m fingered this as the lead for the
// 16s-vs-30min init variance: a DOGMA/allocator CS routed through the fake
// path → free-list races under concurrent init. The garbage-value storms that
// motivated pretend-success came from the iter-77 immortal-worker corruption,
// fixed in iter-189; in healthy runs out-of-range CS traffic is real traffic.
//
// All three stubs now key the host-mutex map on the CANONICAL VA
// (aliasGuestVA), so a CS entered via its uncached alias (0xC2xxxxxx) and via
// its cached VA (0x82xxxxxx) shares one mutex. Sentinel values (top byte
// 0xFF) key on the raw value — the alias fold would land them on legitimate
// 0x9Fxxxxxx heap addresses and cause false sharing with real CSes.
//
// Safety valves: SR_CRITSEC_FAKE_SUCCESS=1 restores the legacy behavior
// wholesale; the lock-free OOR registry below is capacity-bounded (4096
// slots, 8-probe windows) and falls back to pretend-success for NEW keys
// once saturated, so a corruption storm minting garbage CS values stays
// bounded (see docs/BANDAIDS.md).
static bool csLegacyFakeSuccessMode() {
  static const bool v = [] {
    const char* e = std::getenv("SR_CRITSEC_FAKE_SUCCESS");
    return e && e[0] == '1';
  }();
  // iter-463 charter: pretend-success is forcing; only under SR_ALLOW_FORCING.
  return v && sr::kernel::forcingAllowed();
}

// iter-463 charter: what an OOR CS does when the lock-free registry is full.
// Forcing on: the legacy pretend-success (storm cap). Forcing off: the console
// behaviour - the CS is a real lock - so the caller routes the key through the
// ordinary s_csMap path instead. A key refused by the registry is refused
// forever (slots are never freed), so Enter/Leave/TryEnter agree on the path.
static bool csOorFullPretends() {
  return sr::kernel::forcingAllowed();
}
static void csOorFullFallbackLog(uint32_t r3, uint32_t key) {
  static std::atomic<uint32_t> s_n{0};
  uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed);
  if (n < 5 || (n + 1) % 100000 == 0)
    SR_LOG_KERNEL_IMPORT("[CRITSEC_OOR_FULL] r3=0x%08X (key=0x%08X) #%u registry full - REAL locking via the CS map (no pretend-success)",
                         r3, key, n + 1);
}

// Canonical map key for a guest CS pointer + out-of-range classification.
static uint32_t csCanonicalKey(uint32_t r3, bool* outOfRange) {
  bool sentinel = ((r3 >> 24) == 0xFFu);
  uint32_t key = sentinel ? r3 : sr::kernel::aliasGuestVA(r3);
  // iter-463: the top was the stale pre-iter-173 0xB4000000, which classed real
  // CSes in [0xB4000000, 0xBF000000) - guest thread stacks, phys-region objects -
  // as "garbage". The arena ends at kMinGuestSizeToCoverAllUsedVAs.
  *outOfRange = sentinel || key < 0x82000000u ||
                key >= static_cast<uint32_t>(sr::kernel::kMinGuestSizeToCoverAllUsedVAs);
  // iter-463 SR_NTAVM_XENIA_HEAP: title arenas now live in Xenia's v00000000 /
  // v40000000 heaps, so a CS there is a real one, not garbage.
  if (*outOfRange && !sentinel && sr::kernel::xheap::enabled() && sr::kernel::xheap::inRange(key))
    *outOfRange = false;
  return key;
}

// Out-of-range CS registry: lock-free open-addressed table keyed by canonical
// VA, holding the host CS for each admitted key. The garbage-CS walking storm
// calls Enter/Leave at ~2M/s with byte-soup keys; routing those through a
// mutex-guarded set (and the global s_csMap mutex) serialized init and starved
// legit CS users (found in 190c parallel soaks). Admission is bounded by table
// capacity — when a probe window is full, new keys fall back to legacy
// pretend-success (the storm-cap behavior). Admitted entries are never freed
// (bounded: 4096 slots).
struct OorCsSlot {
  std::atomic<uint32_t> key{0};                 // 0 = empty (key 0 handled as null earlier)
  std::atomic<HostCriticalSection*> cs{nullptr};
};
static OorCsSlot s_oorCsTable[4096];

// Returns the host CS for an out-of-range key, admitting it if there's room.
// nullptr → table full (storm), caller takes the legacy pretend-success path.
static HostCriticalSection* oorGetOrCreateCS(uint32_t key) {
  uint32_t h = (key * 2654435761u) >> 20;  // fibonacci hash → 12 bits
  for (uint32_t probe = 0; probe < 8; ++probe) {
    OorCsSlot& slot = s_oorCsTable[(h + probe) & 4095];
    uint32_t cur = slot.key.load(std::memory_order_acquire);
    if (cur == 0) {
      uint32_t expected = 0;
      if (!slot.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel)) {
        if (expected != key) continue;  // lost race to a different key — next probe
      }
      cur = key;
    }
    if (cur != key) continue;
    HostCriticalSection* cs = slot.cs.load(std::memory_order_acquire);
    if (!cs) {
      auto* fresh = new HostCriticalSection();
      if (slot.cs.compare_exchange_strong(cs, fresh, std::memory_order_acq_rel)) return fresh;
      delete fresh;  // another thread published first; cs now holds theirs
    }
    return slot.cs.load(std::memory_order_acquire);
  }
  static std::atomic<bool> s_capWarned{false};
  if (!s_capWarned.exchange(true)) {
    SR_LOG_KERNEL_IMPORT("[CRITSEC_OOR_CAP] out-of-range CS table saturated — garbage storm suspected, new keys fall back to pretend-success");
  }
  return nullptr;
}

// Force-recreate an admitted OOR CS (10s abandoned-owner escape). The old CS
// leaks deliberately — other threads may still be blocked inside it.
static HostCriticalSection* oorRecreateCS(uint32_t key) {
  uint32_t h = (key * 2654435761u) >> 20;
  for (uint32_t probe = 0; probe < 8; ++probe) {
    OorCsSlot& slot = s_oorCsTable[(h + probe) & 4095];
    if (slot.key.load(std::memory_order_acquire) != key) continue;
    auto* fresh = new HostCriticalSection();
    slot.cs.store(fresh, std::memory_order_release);
    return fresh;
  }
  return nullptr;
}

// Release all CSes owned by the calling thread. Called from the SEH exception handler
// of a dying worker thread to prevent permanent deadlocks in threads waiting on those CSes.
static void releaseDeadThreadCSes(uint32_t gHandle) {
  auto tid = std::this_thread::get_id();
  uint32_t count = 0;
  std::lock_guard<std::mutex> lock(s_csMapMutex);
  for (auto& [addr, csPtr] : s_csMap) {
    if (!csPtr) continue;
    if (csPtr->owner.load(std::memory_order_relaxed) != tid) continue;
    uint32_t rc = csPtr->recursionCount.exchange(0, std::memory_order_relaxed);
    csPtr->owner.store(std::thread::id{}, std::memory_order_relaxed);
    for (uint32_t i = 0; i < rc; ++i) csPtr->mtx.unlock();
    SR_LOG_KERNEL_IMPORT("[THREAD_CLEANUP] handle=%u releasing abandoned CS 0x%08X (rc=%u)", gHandle, addr, rc);
    ++count;
  }
  if (count > 0)
    SR_LOG_KERNEL_IMPORT("[THREAD_CLEANUP] handle=%u released %u abandoned CSes on exit", gHandle, count);
  // iter-190: also sweep the lock-free out-of-range CS registry.
  uint32_t oorCount = 0;
  for (auto& slot : s_oorCsTable) {
    if (slot.key.load(std::memory_order_acquire) == 0) continue;
    HostCriticalSection* cs = slot.cs.load(std::memory_order_acquire);
    if (!cs || cs->owner.load(std::memory_order_relaxed) != tid) continue;
    uint32_t rc = cs->recursionCount.exchange(0, std::memory_order_relaxed);
    cs->owner.store(std::thread::id{}, std::memory_order_relaxed);
    for (uint32_t i = 0; i < rc; ++i) cs->mtx.unlock();
    ++oorCount;
  }
  if (oorCount > 0)
    SR_LOG_KERNEL_IMPORT("[THREAD_CLEANUP] handle=%u released %u abandoned OOR CSes on exit", gHandle, oorCount);
}

// iter-32: CS-spin instrumentation. Post-WALK_END, main thread parks in a
// 2.2M-call/sec Enter+Leave loop on REAL guest CS pointers (the iter-25a
// garbage filter doesn't catch them — they're in [0x82000000, 0xB4000000)).
// We don't yet know the guest function or CS object, so we can't target a fix.
//
// Instrumentation: lock-free direct-mapped histograms keyed by hash of
// caller_lr / r3, sized at 256 slots each. Eviction-on-collision is fine —
// hot LRs/pointers will dominate. A monitor thread polls the call-rate at
// 1 Hz; when total Enter+Leave exceeds 100 k/s it logs the top-N entries and
// clears the histograms.
//
// Cost on the fast path: one atomic increment per call + a load + (in steady
// state) one atomic increment for the matched LR/r3 slot. No mutex. With ~256
// distinct hot LRs the steady-state contention is bounded.
namespace cs_spin_instr {

struct LrSlot {
  std::atomic<uint32_t> key{0};
  std::atomic<uint32_t> count{0};
};

constexpr size_t kSlots = 256;

static LrSlot g_enterLrHist[kSlots];
static LrSlot g_leaveLrHist[kSlots];
static LrSlot g_r3Hist[kSlots];

static std::atomic<uint64_t> g_enterCount{0};
static std::atomic<uint64_t> g_leaveCount{0};
static std::atomic<uint32_t> g_lastDumpedHandle{0};

static std::atomic<bool> g_monitorStarted{false};

static inline uint32_t hashKey(uint32_t k) {
  // FNV-style mix; PPC code is 4-byte aligned so the low 2 bits are useless.
  uint32_t h = k >> 2;
  h ^= h >> 16;
  h *= 0x85EBCA6Bu;
  h ^= h >> 13;
  return h;
}

// Update one slot of the histogram. Lock-free with eviction-on-collision.
static inline void recordSlot(LrSlot* hist, uint32_t key) {
  if (key == 0) return;
  size_t slot = hashKey(key) & (kSlots - 1);
  auto& s = hist[slot];
  uint32_t cur = s.key.load(std::memory_order_relaxed);
  if (cur == key) {
    s.count.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (cur == 0) {
    if (s.key.compare_exchange_strong(cur, key, std::memory_order_relaxed)) {
      s.count.store(1, std::memory_order_relaxed);
      return;
    }
    if (cur == key) {
      s.count.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  // Collision with a different hot key — evict if our count grows large vs
  // theirs. Simple policy: replace if their count is still < 64 (cold slot
  // recently claimed). Otherwise drop the sample.
  uint32_t cnt = s.count.load(std::memory_order_relaxed);
  if (cnt < 64) {
    s.key.store(key, std::memory_order_relaxed);
    s.count.store(1, std::memory_order_relaxed);
  }
}

static void recordEnter(uint32_t lr, uint32_t r3) {
  g_enterCount.fetch_add(1, std::memory_order_relaxed);
  recordSlot(g_enterLrHist, lr);
  recordSlot(g_r3Hist, r3);
}

static void recordLeave(uint32_t lr, uint32_t r3) {
  g_leaveCount.fetch_add(1, std::memory_order_relaxed);
  recordSlot(g_leaveLrHist, lr);
  recordSlot(g_r3Hist, r3);
}

static void dumpTop(LrSlot* hist, const char* label) {
  // Snapshot all slots, sort by count, log top 10.
  std::pair<uint32_t, uint32_t> snap[kSlots];
  for (size_t i = 0; i < kSlots; ++i) {
    snap[i].first = hist[i].count.load(std::memory_order_relaxed);
    snap[i].second = hist[i].key.load(std::memory_order_relaxed);
  }
  // Partial selection of top 10 (small N, not worth std::sort overhead).
  for (int round = 0; round < 10; ++round) {
    int bestIdx = round;
    for (int j = round + 1; j < (int)kSlots; ++j) {
      if (snap[j].first > snap[bestIdx].first) bestIdx = j;
    }
    if (bestIdx != round) std::swap(snap[round], snap[bestIdx]);
    if (snap[round].first == 0) break;
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[CS_SPIN] %s top#%d key=0x%08X count=%u",
      label, round + 1, snap[round].second, snap[round].first);
  }
}

static void clearHist(LrSlot* hist) {
  for (size_t i = 0; i < kSlots; ++i) {
    hist[i].key.store(0, std::memory_order_relaxed);
    hist[i].count.store(0, std::memory_order_relaxed);
  }
}

static void monitorLoop() {
  using namespace std::chrono_literals;
  uint64_t prevEnter = 0;
  uint64_t prevLeave = 0;
  uint64_t spikeCount = 0;
  while (true) {
    std::this_thread::sleep_for(1s);
    uint64_t curEnter = g_enterCount.load(std::memory_order_relaxed);
    uint64_t curLeave = g_leaveCount.load(std::memory_order_relaxed);
    uint64_t enterDelta = curEnter - prevEnter;
    uint64_t leaveDelta = curLeave - prevLeave;
    prevEnter = curEnter;
    prevLeave = curLeave;
    uint64_t total = enterDelta + leaveDelta;

    // Threshold: 100 k Enter+Leave per second is well above any normal game-
    // tick activity (we're seeing ~2.2 M/s in the spin).
    if (total < 100000) continue;
    ++spikeCount;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[CS_SPIN] spike #%llu — Enter=%llu/s Leave=%llu/s total=%llu/s (cumulative E=%llu L=%llu)",
      (unsigned long long)spikeCount,
      (unsigned long long)enterDelta, (unsigned long long)leaveDelta,
      (unsigned long long)total,
      (unsigned long long)curEnter, (unsigned long long)curLeave);
    dumpTop(g_enterLrHist, "EnterLR");
    dumpTop(g_leaveLrHist, "LeaveLR");
    dumpTop(g_r3Hist, "CSPtr");
    // Clear so next-second snapshot is fresh.
    clearHist(g_enterLrHist);
    clearHist(g_leaveLrHist);
    clearHist(g_r3Hist);
  }
}

static void startMonitorOnce() {
  bool already = g_monitorStarted.exchange(true, std::memory_order_acq_rel);
  if (already) return;
  std::thread(monitorLoop).detach();
}

}  // namespace cs_spin_instr

// step 6 I8 (joint plan I8(a), Fable ruling 21 (3)), SR_CS_REINIT_REUSE (default ON; =0 the old path): a re-init of a
// guest CS whose host object nobody owns and nobody waits on REUSES that object - under its own lock (a try_lock that
// succeeds, so no thread holds it), only the diagnostic owner fields are reset, and the map and the fast slot keep the
// same pointer. Nothing is created or destroyed, so no reference anywhere (a fast-slot reader, a waiter's 50 ms
// re-fetch) can meet freed memory - the property the graveyard exists for - and the graveyard stops growing (gate 3:
// +282 objects / +51 KB per reload). A concurrent user sees the re-initialized, unlocked CS, as on the console, where
// the CS is the same guest memory. Owned or waited on: logged ([CS_REINIT_HELD]) and skipped - the old path.
static bool csReinitReuseOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_CS_REINIT_REUSE");
    return !e || e[0] != '0';
  }();
  return v;
}

struct CsInitTally {   // guarded by s_csMapMutex
  uint64_t all = 0, first = 0, reused = 0, held = 0, replaced = 0;
};
static CsInitTally s_csInit;

// Under s_csMapMutex: reuse `o` for a re-init when nobody owns or waits on it. True = reused.
static bool csTryReuseLocked(HostCriticalSection& o) {
  if (o.owner.load(std::memory_order_acquire) != std::thread::id{} ||
      o.recursionCount.load(std::memory_order_acquire) != 0 || o.waiters.load(std::memory_order_acquire) != 0)
    return false;
  // owner none, so the calling thread does not hold it: a successful try_lock means NO thread holds it
  if (!o.mtx.try_lock()) return false;
  o.ownerHandle.store(0, std::memory_order_relaxed);
  o.ownerEnterLr.store(0, std::memory_order_relaxed);
  o.ownerHostTid.store(0, std::memory_order_relaxed);
  o.mtx.unlock();
  return true;
}

// Under s_csMapMutex: the counts' milestone line (player-profile milestone tag CS_INIT) and the held re-init's line.
static void csInitNoteLocked(uint32_t key, const HostCriticalSection* held, bool reusedNow = false) {
  const CsInitTally& t = s_csInit;
  if (held) {
    if (t.held <= 64 || t.held % 1000 == 0)
      SR_LOG_BOOT(::sr::LogSeverity::Warn,
                  "[CS_REINIT_HELD] n=%llu cs=0x%08X ownerHandle=0x%08X rec=%u waiters=%u - re-init of a held / "
                  "waited-on CS: the old object kept (graveyard), a new one made",
                  (unsigned long long)t.held, key, held->ownerHandle.load(std::memory_order_relaxed),
                  held->recursionCount.load(std::memory_order_relaxed),
                  held->waiters.load(std::memory_order_relaxed));
  }
  // this call's reuse is the first or a 100th: logged too, so a short roll (one load) always shows its reuses
  const bool reuseMilestone = reusedNow && (t.reused == 1 || t.reused % 100 == 0);
  if (t.all == 1 || t.all == 10 || t.all == 100 || t.all % 1000 == 0 || (held && t.held <= 64) || reuseMilestone)
    SR_LOG_BOOT(::sr::LogSeverity::Info,
                "[CS_INIT] all=%llu first=%llu reused=%llu held=%llu replaced=%llu graveyard=%zu reuse=%d",
                (unsigned long long)t.all, (unsigned long long)t.first, (unsigned long long)t.reused,
                (unsigned long long)t.held, (unsigned long long)t.replaced, s_csGraveyard.size(),
                csReinitReuseOn() ? 1 : 0);
}

static uint32_t stub_RtlInitializeCriticalSection(uint32_t r3, uint32_t r4, uint32_t r5,
                                                   uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  if (r3 != 0) {
    bool oor = false;
    uint32_t key = csCanonicalKey(r3, &oor);
    std::lock_guard<std::mutex> lock(s_csMapMutex);
    auto& slotPtr = s_csMap[key];
    ++s_csInit.all;
    const HostCriticalSection* held = nullptr;
    if (!slotPtr) {
      ++s_csInit.first;
    } else if (csReinitReuseOn() && csTryReuseLocked(*slotPtr)) {
      // I8: the same object, re-initialized under its own lock; the fast slot already holds it (or claims it here)
      ++s_csInit.reused;
      if (csFastMapOn())
        if (CsFastSlot* s = csFastSlot(key, true)) s->cs.store(slotPtr.get(), std::memory_order_release);
      csInitNoteLocked(key, nullptr, true);
      STUB_LOG("RtlInitializeCriticalSection", r3, r4, r5, r6, r7, r8, kStatusSuccess);
      return kStatusSuccess;
    } else if (csReinitReuseOn()) {
      ++s_csInit.held;
      held = slotPtr.get();
    } else {
      ++s_csInit.replaced;   // SR_CS_REINIT_REUSE=0: every re-init takes the old path
    }
    if (held) csInitNoteLocked(key, held);   // before the old object moves (its fields are read)
    if (csFastMapOn()) {
      // iter-464 SR_CS_FASTMAP: keep the old host object alive (see getOrCreateCS).
      if (slotPtr) s_csGraveyard.push_back(std::move(slotPtr));
      slotPtr = std::make_unique<HostCriticalSection>();
      if (CsFastSlot* s = csFastSlot(key, true)) s->cs.store(slotPtr.get(), std::memory_order_release);
    } else {
      slotPtr = std::make_unique<HostCriticalSection>();
    }
    if (!held) csInitNoteLocked(key, nullptr);
  }
  STUB_LOG("RtlInitializeCriticalSection", r3, r4, r5, r6, r7, r8, kStatusSuccess);
  return kStatusSuccess;
}

// iter-466 tick: SR_CS_TRYFIRST (see stub_RtlEnterCriticalSection). Baked default ON (=0 A/B).
static bool csTryFirstOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_CS_TRYFIRST");
    return !e || e[0] != '0';
  }();
  return v;
}

// The bookkeeping of a successful Enter (identical to the inline block of the timed loop).
static void csNoteAcquired(HostCriticalSection& cs, uint32_t r3) {
  cs.owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
  cs.recursionCount.fetch_add(1, std::memory_order_relaxed);
  const uint32_t tHandle = sr::kernel::getCurrentThreadHandle();
  cs.ownerHandle.store(tHandle, std::memory_order_relaxed);
  cs.ownerEnterLr.store(sr::kernel::getCurrentGuestLR(), std::memory_order_relaxed);
  cs.ownerHostTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
  if (tHandle <= 2) {
    static std::atomic<uint64_t> s_mainEnterLog{0};
    uint64_t mel = s_mainEnterLog.fetch_add(1, std::memory_order_relaxed);
    if (mel < 5 || (mel + 1) % 50000 == 0)
      SR_LOG_KERNEL_IMPORT("[MAIN_ENTER_CS] CS=0x%08X call#%" PRIu64, r3, mel + 1);
  }
}

static uint32_t stub_RtlEnterCriticalSection(uint32_t r3, uint32_t r4, uint32_t r5,
                                              uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  if (r3 == 0) {
    static std::atomic<uint64_t> s_nullCount{0};
    uint64_t n = s_nullCount++;
    if (n < 3 || (n + 1) % 10000 == 0)
      SR_LOG_KERNEL_IMPORT("RtlEnterCriticalSection: WARNING - null (r3=0), ignoring (count=%" PRIu64 ")", n + 1);
    return kStatusSuccess;
  }
  // iter-190: out-of-range CSes get REAL locking on the canonical key (see
  // csCanonicalKey above for rationale; replaces the iter-25a/32 pretend-
  // success path that left them with no mutual exclusion). Lock-free registry
  // so the garbage-CS storm (~2M calls/s) can't serialize legit CS traffic.
  bool oor = false;
  uint32_t csKey = csCanonicalKey(r3, &oor);
  HostCriticalSection* oorCs = nullptr;
  if (oor) {
    static std::atomic<uint64_t> s_oorEnterCount{0};
    uint64_t n = s_oorEnterCount.fetch_add(1, std::memory_order_relaxed);
    oorCs = csLegacyFakeSuccessMode() ? nullptr : oorGetOrCreateCS(csKey);
    if (!oorCs && csOorFullPretends()) {
      if (n < 5 || (n + 1) % 100000 == 0) {
        SR_LOG_KERNEL_IMPORT("[CRITSEC_GARBAGE_ENTER] r3=0x%08X (key=0x%08X) #%" PRIu64 " — legacy mode, returning success", r3, csKey, n + 1);
      }
      return kStatusSuccess;
    }
    if (!oorCs) { csOorFullFallbackLog(r3, csKey); oor = false; }
    if (oor && (n < 5 || (n + 1) % 100000 == 0)) {
      SR_LOG_KERNEL_IMPORT("[CRITSEC_OOR_ENTER] r3=0x%08X (key=0x%08X) #%" PRIu64 " caller_lr=0x%08X — out-of-range, REAL locking", r3, csKey, n + 1, sr::kernel::getCurrentGuestLR());
    }
  }
  // iter-32: feed the spin-monitor histogram. Only main-thread calls — worker
  // threads have their own legitimate CS traffic that we don't want to muddy
  // the histogram with.
  if (sr::kernel::getCurrentThreadHandle() <= 2) {
    cs_spin_instr::startMonitorOnce();
    cs_spin_instr::recordEnter(sr::kernel::getCurrentGuestLR(), r3);
  }
  // iter-466 tick (SR_CS_TRYFIRST, A/B gate): an uncontended Enter took a steady_clock read
  // (QPC) for the stall timer plus try_lock_for's own deadline read before even trying the
  // lock (~2% of the guest render thread, PERFORMANCE 2.8). A plain try_lock first: when it
  // succeeds, try_lock_for would have succeeded the same way; when it fails, the timed loop
  // below runs exactly as before.
  if (csTryFirstOn()) {
    HostCriticalSection& cs = oor ? *oorCs : getOrCreateCS(csKey);
    if (cs.mtx.try_lock()) {
      csNoteAcquired(cs, r3);
      return kStatusSuccess;
    }
  }
  auto start = std::chrono::steady_clock::now();
  bool warned = false;
  bool forcedReset = false;
  while (true) {
    HostCriticalSection& cs = oor ? *oorCs : getOrCreateCS(csKey);
    // iter-440j: kernel-blocking wait with watchdog cadence (see struct note).
    // step 6 I8: the wait is counted on the object, so a re-init sees it and keeps the old path
    cs.waiters.fetch_add(1, std::memory_order_acq_rel);
    const bool acquired = cs.mtx.try_lock_for(std::chrono::milliseconds(50));
    cs.waiters.fetch_sub(1, std::memory_order_acq_rel);
    if (acquired) {
      if (csTryFirstOn()) {
        csNoteAcquired(cs, r3);
        return kStatusSuccess;
      }
      cs.owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
      cs.recursionCount.fetch_add(1, std::memory_order_relaxed);
      cs.ownerHandle.store(sr::kernel::getCurrentThreadHandle(), std::memory_order_relaxed);
      cs.ownerEnterLr.store(sr::kernel::getCurrentGuestLR(), std::memory_order_relaxed);
      cs.ownerHostTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
      {
        static std::atomic<uint64_t> s_mainEnterLog{0};
        uint32_t tHandle = sr::kernel::getCurrentThreadHandle();
        if (tHandle <= 2) {
          uint64_t mel = s_mainEnterLog.fetch_add(1, std::memory_order_relaxed);
          if (mel < 5 || (mel + 1) % 50000 == 0)
            SR_LOG_KERNEL_IMPORT("[MAIN_ENTER_CS] CS=0x%08X call#%" PRIu64, r3, mel + 1);
        }
      }
      return kStatusSuccess;
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    if (!warned && elapsed > std::chrono::seconds(2)) {
      warned = true;
      uint32_t tHandle = sr::kernel::getCurrentThreadHandle();
      SR_LOG_KERNEL_IMPORT(
          "[CRITSEC_STALL] thread=%u waiting >2s on CS guestVA=0x%08X "
          "owner=thread %u ownerHostTid=%u enterLR=0x%08X rc=%u",
          tHandle, r3, cs.ownerHandle.load(std::memory_order_relaxed),
          cs.ownerHostTid.load(std::memory_order_relaxed),
          cs.ownerEnterLr.load(std::memory_order_relaxed),
          cs.recursionCount.load(std::memory_order_relaxed));
      // iter-439 P7: snapshot ALL guest threads' live PC state at stall time —
      // the holder's park location is the whole investigation. Rate-limited:
      // at most one dump per 10s process-wide.
      {
        static std::atomic<int64_t> s_lastDump{0};
        const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
        int64_t last = s_lastDump.load(std::memory_order_relaxed);
        if (now - last >= 10 &&
            s_lastDump.compare_exchange_strong(last, now)) {
          sr::kernel::dumpGuestThreadsNow("CS_STALL_THREADS");
          // iter-439 P7: dump every HELD critical section — the deadlock's
          // full edge list (who holds what, taken from where). The wedge is
          // T50 holding CS A while deadline-retrying CS B; this names B and
          // B's owner directly.
          {
            std::lock_guard<std::mutex> lk(s_csMapMutex);
            for (auto& kv : s_csMap) {
              if (!kv.second) continue;
              const uint32_t rc =
                  kv.second->recursionCount.load(std::memory_order_relaxed);
              if (rc == 0) continue;
              SR_LOG_KERNEL_IMPORT(
                  "[CS_HELD] va=0x%08X owner=thread %u hostTid=%u rc=%u enterLR=0x%08X",
                  kv.first,
                  kv.second->ownerHandle.load(std::memory_order_relaxed),
                  kv.second->ownerHostTid.load(std::memory_order_relaxed), rc,
                  kv.second->ownerEnterLr.load(std::memory_order_relaxed));
            }
          }
        }
      }
    }
    // OOR keys are overwhelmingly garbage values whose "owner" is the storm
    // walker that never Leaves; don't make real threads pay 10s per collision.
    //
    // iter-428e: for IN-RANGE CSes the old 10s force-recreate is DELETED — it
    // was caught live manufacturing a mutual-exclusion violation: a pool
    // worker held the FMV SM's CS legitimately-long, the main thread declared
    // it "abandoned", broke the lock, and marched into the same critical
    // region; the SM's guest recursion counter stayed off-by-one for the rest
    // of the run and the movie pipeline died ([CS_PAIR] 18:44:23.784 →
    // 33.805, exactly the 10s timeout). A live-owner lock must NEVER be
    // broken. If the owner thread is genuinely gone (the longjmp-orphan
    // class), that is the bug to fix at its root — here we log loudly and
    // keep waiting, which is exactly what the console would do.
    // iter-463 charter: the OOR force-recreate breaks a held lock - forcing.
    // Master-gated; with forcing off an OOR CS waits like any other (the
    // console behaviour, Xenia xboxkrnl_rtl.cc RtlEnterCriticalSection).
    if (!forcedReset && oor && elapsed > std::chrono::seconds(2) &&
        sr::kernel::forcingAllowed()) {
      forcedReset = true;
      uint32_t tHandle = sr::kernel::getCurrentThreadHandle();
      SR_LOG_KERNEL_IMPORT("[CRITSEC_ABANDONED] thread=%u CS=0x%08X (garbage-range) >2s — force-recreating", tHandle, r3);
      oorCs = oorRecreateCS(csKey);
      if (!oorCs) return kStatusSuccess;
      continue;  // retry: next iteration fetches fresh CS and try_lock succeeds
    }
    if (elapsed > std::chrono::seconds(30) &&
        std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() % 30 == 0) {
      SR_LOG_KERNEL_IMPORT(
          "[CRITSEC_HELD_LONG] thread=%u still waiting on CS=0x%08X (%llds) — "
          "NOT breaking the lock; find the holder",
          sr::kernel::getCurrentThreadHandle(), r3,
          (long long)std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
      Sleep(900);
    }
  }
}

static uint32_t stub_RtlLeaveCriticalSection(uint32_t r3, uint32_t r4, uint32_t r5,
                                              uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  if (r3 == 0) {
    static std::atomic<uint64_t> s_nullCount{0};
    uint64_t n = s_nullCount++;
    if (n < 3 || (n + 1) % 10000 == 0)
      SR_LOG_KERNEL_IMPORT("RtlLeaveCriticalSection: WARNING - null (r3=0), ignoring (count=%" PRIu64 ")", n + 1);
    return kStatusSuccess;
  }
  // iter-190: out-of-range CSes get REAL unlocking on the canonical key,
  // matching Enter/TryEnter (replaces the iter-25a pretend-success path).
  // The owner check below already protects against unbalanced Leaves on
  // garbage values — a Leave by a non-owner is a no-op.
  bool oor = false;
  uint32_t csKey = csCanonicalKey(r3, &oor);
  HostCriticalSection* oorCs = nullptr;
  if (oor) {
    static std::atomic<uint64_t> s_oorLeaveCount{0};
    uint64_t n = s_oorLeaveCount.fetch_add(1, std::memory_order_relaxed);
    oorCs = csLegacyFakeSuccessMode() ? nullptr : oorGetOrCreateCS(csKey);
    if (!oorCs && !csOorFullPretends()) oor = false;  // table full: map path (see Enter)
    else if (!oorCs) {
      if (n < 5 || n == 100 || n == 10000 || n == 1000000 || n == 10000000) {
        uint32_t lr = sr::kernel::getCurrentGuestLR();
        SR_LOG_KERNEL_IMPORT("[GARBAGE_LEAVE_LR] #%" PRIu64 " r3=0x%08X (key=0x%08X) r4=0x%08X caller_lr=0x%08X — legacy mode",
                             n + 1, r3, csKey, r4, lr);
      }
      return kStatusSuccess;
    }
    if (oor && (n < 5 || (n + 1) % 100000 == 0)) {
      SR_LOG_KERNEL_IMPORT("[CRITSEC_OOR_LEAVE] r3=0x%08X (key=0x%08X) #%" PRIu64 " caller_lr=0x%08X — out-of-range, REAL unlocking", r3, csKey, n + 1, sr::kernel::getCurrentGuestLR());
    }
  }
  // iter-32: feed the spin-monitor histogram. See stub_RtlEnterCriticalSection
  // for the rationale; same main-thread filter applies.
  if (sr::kernel::getCurrentThreadHandle() <= 2) {
    cs_spin_instr::startMonitorOnce();
    cs_spin_instr::recordLeave(sr::kernel::getCurrentGuestLR(), r3);
  }
  auto& cs = oor ? *oorCs : getOrCreateCS(csKey);
  auto tid = std::this_thread::get_id();
  if (cs.owner.load(std::memory_order_relaxed) != tid) {
    return kStatusSuccess;
  }
  uint32_t rc = cs.recursionCount.fetch_sub(1, std::memory_order_relaxed);
  if (rc <= 1) {
    cs.owner.store(std::thread::id{}, std::memory_order_relaxed);
  }
  cs.mtx.unlock();
  return kStatusSuccess;
}

static uint32_t stub_RtlTryEnterCriticalSection(uint32_t r3, uint32_t r4, uint32_t r5,
                                                 uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  if (r3 == 0) {
    static std::atomic<uint64_t> s_nullCount{0};
    uint64_t n = s_nullCount++;
    if (n < 3 || (n + 1) % 10000 == 0)
      SR_LOG_KERNEL_IMPORT("RtlTryEnterCriticalSection: WARNING - null (r3=0), returning 0 (count=%" PRIu64 ")", n + 1);
    return 0;
  }
  // iter-303: post-init spin-storm instrument. After the natural init walks
  // complete, worker loops TryEnter at microsecond cadence forever (the CS VAs
  // alternate in pairs, so track the dominant CALLER LR, not the CS VA). The
  // known hot site is 0x825FC15C = the timed path of CS-helper sub_825FC108 —
  // which is only reachable when its deadline global (*0x8208A864/*0x82074190,
  // static+golden value 0xFFFFFFFF = "take the blocking path") reads as
  // something else at runtime, i.e. a scribble. Dump both globals with each
  // threshold hit so the storm names its own cause.
  // iter-464 perf lane: three process-wide atomic RMWs per TryEnter from every thread (the
  // storm runs ~2M/s) bounce one cache line between cores. Diagnostics are opt-in for
  // players: on in the dev log profile (every automated roll), off in the player profile;
  // SR_CS_STORM_DIAG=0/1 overrides.
  static const bool s_stormDiag = [] {
    const char* e = std::getenv("SR_CS_STORM_DIAG");
    if (e && e[0]) return e[0] != '0';
    return !sr::Log::playerProfile();
  }();
  if (s_stormDiag) {
    static std::atomic<int64_t> s_totalTry{0};
    static std::atomic<uint32_t> s_hotLR{0};
    static std::atomic<int64_t> s_hotRun{0};
    uint32_t lr = sr::kernel::getCurrentGuestLR();
    int64_t total = s_totalTry.fetch_add(1, std::memory_order_relaxed) + 1;
    bool report = false;
    if (s_hotLR.load(std::memory_order_relaxed) == lr) {
      int64_t run = s_hotRun.fetch_add(1, std::memory_order_relaxed) + 1;
      report = (run == 1000000) || (run % 200000000 == 0);
    } else if (s_hotRun.fetch_sub(1, std::memory_order_relaxed) <= 1) {
      s_hotLR.store(lr, std::memory_order_relaxed);
      s_hotRun.store(1, std::memory_order_relaxed);
    }
    if (report || total == 20000000 || total == 100000000 ||
        total == 300000000 || total == 600000000) {
      SR_LOG_KERNEL_IMPORT("[CS_SPIN_STORM] total=%" PRId64 " caller_lr=0x%08X CS=0x%08X thread=%u"
                           " deadlineGlobals *8208A864=0x%08X *82074190=0x%08X (expect FFFFFFFF; else scribbled)",
                           total, lr, r3, sr::kernel::getCurrentThreadHandle(),
                           readGuestU32BE(0x8208A864), readGuestU32BE(0x82074190));
    }
  }
  // iter-190: out-of-range CSes get a REAL try-lock on the canonical key,
  // matching Enter/Leave (replaces the iter-25a pretend-acquired path). A
  // contended TryEnter now correctly returns 0 instead of letting two
  // threads both "acquire" the same CS.
  bool oor = false;
  uint32_t csKey = csCanonicalKey(r3, &oor);
  HostCriticalSection* oorCs = nullptr;
  if (oor) {
    static std::atomic<uint64_t> s_oorTryCount{0};
    uint64_t n = s_oorTryCount.fetch_add(1, std::memory_order_relaxed);
    oorCs = csLegacyFakeSuccessMode() ? nullptr : oorGetOrCreateCS(csKey);
    if (!oorCs && !csOorFullPretends()) {
      csOorFullFallbackLog(r3, csKey);
      oor = false;  // table full: real try-lock via the map (see Enter)
    } else if (!oorCs) {
      // iter-213: log caller_lr + thread so we can identify WHICH guest spin-wait
      // is hammering this OOR key. The storm is a poll-loop whose awaited flag
      // never flips (stalled worker) — its caller_lr names the stuck subsystem.
      if (n < 8 || (n + 1) % 100000 == 0) {
        SR_LOG_KERNEL_IMPORT("[CRITSEC_GARBAGE_CS] r3=0x%08X (key=0x%08X) #%" PRIu64 " thread=%u caller_lr=0x%08X — legacy mode, returning success",
                             r3, csKey, n + 1, sr::kernel::getCurrentThreadHandle(), sr::kernel::getCurrentGuestLR());
      }
      return 1;  // pretend acquired
    }
    if (oor && (n < 5 || (n + 1) % 100000 == 0)) {
      SR_LOG_KERNEL_IMPORT("[CRITSEC_OOR_TRYENTER] r3=0x%08X (key=0x%08X) #%" PRIu64 " caller_lr=0x%08X — out-of-range, REAL try-lock", r3, csKey, n + 1, sr::kernel::getCurrentGuestLR());
    }
    // The known background spin storm (iter-185) calls TryEnter ~2M/s on the
    // same OOR keys with no Leave. Re-locking a recursive_mutex each time
    // would overflow its recursion count on long soaks; if we already own it,
    // report success without deepening.
    if (oor && oorCs->owner.load(std::memory_order_relaxed) == std::this_thread::get_id()) {
      return 1;
    }
  }
  auto& cs = oor ? *oorCs : getOrCreateCS(csKey);
  if (cs.mtx.try_lock()) {
    cs.owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
    cs.recursionCount.fetch_add(1, std::memory_order_relaxed);
    cs.ownerHandle.store(sr::kernel::getCurrentThreadHandle(), std::memory_order_relaxed);
    cs.ownerEnterLr.store(sr::kernel::getCurrentGuestLR(), std::memory_order_relaxed);
    return 1;
  }
  static std::atomic<uint64_t> s_failCount{0};
  uint64_t fc = s_failCount.fetch_add(1, std::memory_order_relaxed);
  if (fc < 5 || (fc + 1) % 50000 == 0) {
    uint32_t tHandle = sr::kernel::getCurrentThreadHandle();
    SR_LOG_KERNEL_IMPORT("[CRITSEC_TRYENTER_FAIL] thread=%u CS=0x%08X failCount=%" PRIu64, tHandle, r3, fc + 1);
  }
  return 0;
}

// MmAllocatePhysicalMemoryEx - Xbox 360 kernel function to allocate physical/GPU memory
// Xenia-confirmed signature (xboxkrnl_memory.cc):
//   r3 = flags     (0=user, 1/2=D3D)
//   r4 = region_size  (bytes — NOT pages)
//   r5 = protect_bits (PAGE_READWRITE=4, PAGE_READONLY=2, etc.)
//   r6 = min_addr_range (0 for any)
//   r7 = max_addr_range (0xFFFFFFFF for any)
//   r8 = alignment (bytes, power of 2; 0 = page-aligned)
// Returns: allocated physical address or 0 on failure
//
// iter-175 fix: was using r5 (protect_bits, typically 4) as "pages", giving
// 16KB allocations when the game asked for large GPU buffers (ring buf, EDRAM).
// iter-458 HR1 (Astra 072): requests the legacy path truncated to the per-call
// cap, and requests whose 32-bit page rounding wrapped (both still granted).
static std::atomic<uint64_t> g_mmTruncated{0}, g_mmWrapped{0};
void mmAllocSizeAnomalies(uint64_t out[2]) {
  out[0] = g_mmTruncated.load(std::memory_order_relaxed);
  out[1] = g_mmWrapped.load(std::memory_order_relaxed);
}

static uint32_t stub_MmAllocatePhysicalMemoryEx(uint32_t r3, uint32_t r4, uint32_t r5,
                                                 uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r3; (void)r5; (void)r6; (void)r7;

  // iter-458 H-FREE (SR_PHYS_FREE=1, default OFF): the real service - a free
  // list with Xenia's protect/page-size/physical-range semantics, so memory the
  // game frees (MmFreePhysicalMemory below) is reused. Contract: phys_alloc.hpp.
  if (uint32_t addr = 0; sr::kernel::physalloc::serviceAllocate(r4, r5, r6, r7, r8, addr)) {
    static int pfCount = 0;
    if (++pfCount <= 20 || (pfCount % 1000) == 0)
      SR_LOG_KERNEL_IMPORT("[PHYS_ALLOC] #%d MmAllocatePhysicalMemoryEx(SR_PHYS_FREE): r4(bytes)=0x%X r5(prot)=0x%X r6(min)=0x%X r7(max)=0x%X r8(align)=0x%X -> 0x%08X",
                           pfCount, r4, r5, r6, r7, r8, addr);
    if (addr == 0)
      sr::kernel::noteGuestAllocFailure(sr::kernel::GuestAllocApi::MmAllocatePhysicalMemoryEx, r4, r3, r5,
                                        "SR_PHYS_FREE: no free range");
    return addr;
  }

  // r4 is byte count. Round up to 4KB page boundary; cap per-call to guard
  // against a corrupt r4 overflowing the phys region (944 MB total).
  // iter-437f: the cap was 256MB and SILENTLY TRUNCATED the game's 0x12B00000
  // (299MB) DOGMA arena request — the game then sub-allocated the movie
  // planes inside an arena 43MB smaller than it believes, its pool layout
  // diverged from Xenia's (planes packed 0x43000 apart vs Xenia's 0x44000 —
  // frida-verified), and the U plane's tail overlapped V = the movie chroma
  // bottom-strip. Honor real requests; 512MB still guards the region.
  uint32_t sizeBytes = r4;
  if (sizeBytes == 0) sizeBytes = 4096;
  sizeBytes = (sizeBytes + 0xFFFu) & ~0xFFFu;  // page-align
  // iter-458 HR1 (Astra 072): these two existing size rules are NOT fixed
  // here; they are counted, exactly, so a capture can name inadequate
  // provisioning (a grant smaller than the request) instead of reading a
  // non-null return as success.
  if (r4 != 0 && sizeBytes < r4) g_mmWrapped.fetch_add(1, std::memory_order_relaxed);
  constexpr uint32_t kMaxPerCall = 512u * 1024u * 1024u;  // 512 MB
  if (sizeBytes > kMaxPerCall) {
    g_mmTruncated.fetch_add(1, std::memory_order_relaxed);
    SR_LOG_KERNEL_IMPORT("[PHYS_ALLOC] request 0x%X exceeds cap 0x%X — TRUNCATING (guest layout divergence risk)",
                         r4, kMaxPerCall);
    sizeBytes = kMaxPerCall;
  }

  // iter-449B PLATFORM FIX (SR_PHYS_ALIGN_FIX, default ON, =0 for A/B):
  // honor r8 = alignment (Xenia xboxkrnl_memory.cc signature) with the
  // console's page-alignment floor. Console Mm results are ALWAYS at least
  // 4KB-aligned; the D3DX effect compiler's pool pages (XMemAlloc attr class
  // 3 → alignment 1<<12 → sub_82468008 r8=0x1000) find their pool header via
  // *(obj & 0xFFFFF000) and silently corrupt the whole new-backend HLSL
  // compile when a page is unaligned (the black-FE root cause chain).
  static const bool s_alignFix = [] {
    const char* e = std::getenv("SR_PHYS_ALIGN_FIX");
    return !e || e[0] != '0';
  }();
  uint32_t align = 16u;
  if (s_alignFix) {
    align = r8;
    if (align == 0u || (align & (align - 1u)) != 0u || align > 0x10000u)
      align = 0x1000u;
    if (align < 0x1000u) align = 0x1000u;
  }
  // iter-465 physmem (SR_PHYS_XENIA_PLACE): Xenia's page size from the protect bits;
  // size AND alignment round up to it (xboxkrnl_memory.cc:440-449).
  if (sr::kernel::physXeniaPlaceEnabled()) {
    const uint32_t page = (r5 & 0x20000000u) ? 0x10000u : (r5 & 0x80000000u) ? 0x1000000u : 0x1000u;
    const uint64_t rs = (static_cast<uint64_t>(r4 ? r4 : 1u) + page - 1) & ~static_cast<uint64_t>(page - 1);
    if (rs <= kMaxPerCall) sizeBytes = static_cast<uint32_t>(rs);
    uint32_t al = r8 ? r8 : page;
    if (al & (al - 1u)) al = page;   // not a power of two: the page alone
    align = al < page ? page : al;
  }

  // Use dedicated physical region so all "physical" allocs are in one contiguous range
  // (helps DOGMA_PoolManager and similar expect addresses in a single pool).
  // iter-458 H-LIFO (Astra 060): for large grants, the cursor read OUTSIDE the
  // allocator before and after - an independent witness for the free's rollback
  // (another thread's concurrent grant would show up as a mismatch, not hide).
  const bool bigWitness = sizeBytes >= 0x1000000u && (sr::kernel::physLifoEnabled() ||
                                                      sr::kernel::physwin::uniqCensusEnabled());
  const size_t cursorBefore = bigWitness ? sr::kernel::guestPhysNextForTest() : 0;
  uint32_t addr = guestPhysAlloc(sizeBytes, align);
  if (bigWitness)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[PHYS_LIFO] big grant va=%08X size=0x%X align=0x%X | physNext 0x%zX -> 0x%zX lr=%08X",
                addr, sizeBytes, align, cursorBefore, sr::kernel::guestPhysNextForTest(),
                sr::kernel::getCurrentGuestLR());
  // iter-458 (Astra 043): a genuine GPU physical grant - the precise owner model.
  if (addr) sr::kernel::physwin::notePhysGrant(addr, sizeBytes);
#if SR_ORACLE
  // mem.resources 2a (memres2, SR_MEM_RESOURCES=census): the grant and the guest lr that asked (a missed page's grant)
  if (addr && sr::gfx::memres::census()) sr::gfx::memres::noteGrant(addr, sizeBytes, sr::kernel::getCurrentGuestLR());
#endif
  if (addr && sr::gfx::memres::resourceMode()) sr::gfx::memres::resourceMemory().noteGrant(addr, sizeBytes);

  static int allocCount = 0;
  if (++allocCount <= 20 || (allocCount % 1000) == 0) {
    SR_LOG_KERNEL_IMPORT("[PHYS_ALLOC] #%d MmAllocatePhysicalMemoryEx: r4(bytes)=0x%X r5(prot)=%u r8(align)=0x%X sizeRounded=0x%X alignUsed=0x%X -> 0x%08X",
                         allocCount, r4, r5, r8, sizeBytes, align, addr);
  }
  if (addr == 0)
    sr::kernel::noteGuestAllocFailure(sr::kernel::GuestAllocApi::MmAllocatePhysicalMemoryEx, r4, r3, r5,
                                      "physical region exhausted");

  return addr;  // 0 on failure, address on success
}

// NetDll_WSAWaitForMultipleEvents — proper stub.
// stub_generic_success returns 0 (= "object 0 signaled"), which makes the
// guest's poll loop spin forever (FMGameRtam observed at ~500K calls/sec
// pumping WSAWaitForMultipleEvents → WSARecvFrom → WSAResetEvent → repeat).
// Real Xbox semantics: with no socket events firing, this should return
// WSA_WAIT_TIMEOUT (0x102) after dwMilliseconds (r6). We cap the sleep to
// 16ms so guest still gets ~60Hz progress and doesn't block the walker.
// r3 = nCount, r4 = lphEvents, r5 = fWaitAll, r6 = dwMilliseconds, r7 = fAlertable
static uint32_t stub_NetDll_WSAWaitForMultipleEvents(uint32_t r3, uint32_t r4, uint32_t r5,
                                                      uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r3; (void)r4; (void)r5; (void)r7; (void)r8;
  uint32_t ms = r6;
  if (ms == 0xFFFFFFFFu) ms = 16;        // INFINITE → cap to 16ms
  else if (ms > 16) ms = 16;             // also cap finite waits to 16ms per call
  if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  return 0x102u;  // WSA_WAIT_TIMEOUT
}

// NetDll_WSARecvFrom — proper stub.
// stub_generic_success returns 0 = "received N bytes" but doesn't write any
// out-params. The guest then loops and never gets real data. Return
// SOCKET_ERROR (-1) so the guest's `if (ret == SOCKET_ERROR)` branch fires
// and it stops trying to dereference uninit recv buffers.
static uint32_t stub_NetDll_WSARecvFrom(uint32_t r3, uint32_t r4, uint32_t r5,
                                         uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r3; (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  return 0xFFFFFFFFu;  // SOCKET_ERROR — game's WSAGetLastError will return 0 (success), benign loop exit
}

// RELEASE_PLAN phase 4 row 2 (docs/FORCING_RETIREMENT.md): the console's NetDll_XNetGetTitleXnAddr
// fills the caller's XNADDR and reports the address state. The game's 'conn' state machine
// (sub_82565FD0 -> sub_825657A0 / sub_82568538 -> the thunk sub_828A8EA0) polls it from the
// video-out / net init sub_8229F460 until it answers '+isp'; the generic 0 (XNET_GET_XNADDR_PENDING)
// kept it in '~ini' forever, which the R1 100-poll cap hook used to cut. The poll leaves '~ini' on
// any TERMINAL state and never on PENDING: a real console with no cable reports
// XNET_GET_XNADDR_NONE (1), one with an address STATIC (4) / DHCP (8); Xenia's xam_net.cc
// NetDll_XNetGetTitleXnAddr_entry reports STATIC with a loopback XNADDR. We answer STATIC + Xenia's
// XNADDR on purpose, not NONE: every oracle roll and golden dump this project compares against ran
// the title's networking state machine on that answer, so the rest of it (XNetStartup, logon, QoS
// stubs) stays on the oracle's path, where NONE would steer the title down a no-network branch no
// oracle run has exercised. Fields: ina = INADDR_LOOPBACK (network order), inaOnline = 0,
// wPortOnline = 0, abEnet = CC x6, abOnline = 0 x20; return XNET_GET_XNADDR_STATIC (4).
// r3 = caller type (XNCALLER_TITLE), r4 = XNADDR* (36 bytes). Platform fix: an out-param the import fills.
static uint32_t stub_NetDll_XNetGetTitleXnAddr(uint32_t r3, uint32_t r4, uint32_t r5,
                                               uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r3; (void)r5; (void)r6; (void)r7; (void)r8;
  if (r4) {
    writeGuest32(r4 + 0, 0x7F000001u);   // ina.s_addr = htonl(INADDR_LOOPBACK): bytes 7F 00 00 01
    writeGuest32(r4 + 4, 0u);            // inaOnline
    writeGuest32(r4 + 8, 0x0000CCCCu);   // wPortOnline = 0; abEnet[0..1] = CC CC
    writeGuest32(r4 + 12, 0xCCCCCCCCu);  // abEnet[2..5]
    for (uint32_t o = 16; o < 36; o += 4) writeGuest32(r4 + o, 0u);  // abOnline[20]
  }
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 2 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[XNADDR] NetDll_XNetGetTitleXnAddr #%u lr=%08X out=%08X -> STATIC",
                n, sr::kernel::getCurrentGuestLR(), r4);
  return 4u;  // XNET_GET_XNADDR_STATIC
}

// KeEnableFpuExceptions - Xbox 360 kernel function to enable/disable FPU exceptions
// r3 = enable flag (0 = disable, non-zero = enable)
// We MUST disable FPU exceptions on the host to prevent crashes from recompiled code
static uint32_t stub_KeEnableFpuExceptions(uint32_t r3, uint32_t r4, uint32_t r5,
                                           uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
#ifdef _WIN32
  // Mask all floating-point exceptions to prevent crashes
  // This affects: invalid, denormal, divide-by-zero, overflow, underflow, inexact
  unsigned int unused = 0;
  (void)_controlfp_s(&unused, _MCW_EM, _MCW_EM);  // Set all exception mask bits (disable all exceptions)
#endif
  // Always return 0 (disabled) regardless of what guest requests - we can't let FPU exceptions crash the host
  (void)r3;
  return 0;
}

// --- Xam stubs (Xbox dashboard/profile APIs) ---

// XamUserGetSigninState: returns sign-in state for a user index.
// 0=not signed in, 1=signed in offline, 2=signed in to Xbox Live.
// We pretend user 0 is signed in offline so the game proceeds past profile checks.
static uint32_t stub_XamUserGetSigninState(uint32_t r3, uint32_t r4, uint32_t r5,
                                           uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  // r3 = dwUserIndex (0-3)
  return (r3 == 0) ? 1u : 0u;  // User 0 = signed in offline
}

// iter-449r PLATFORM FIX: XamContentCreateEnumerator + XamEnumerate were
// REG_GENERIC_STUBs (return 0, write nothing), which violated the console
// contract TWICE: the guest's phEnum slot kept its 0xFFFFFFFF init, and the
// save-flow FindFirst (sub_82645510) read an UNINITIALIZED stack dword as the
// items-returned count. Worse, FindNext (sub_82645600) never reads the count
// at all — it loops while rc==0 — so an always-0 XamEnumerate makes any roll
// whose garbage count read nonzero spin FOREVER in the boot save flow.
// Contract pinned three ways (iter-449r workflow): IDA decompile of the
// callers, reference/UnleashedRecomp/UnleashedRecomp/kernel/xam.cpp:259-292,
// and Xenia xam_content.cc/xam_enum.cc (which additionally writes
// items_returned=0 on the NO_MORE_FILES path — adopted, it kills the uninit
// read regardless of which check a caller makes). Faithful fresh-console
// semantics: creation SUCCEEDS with an empty set; exhaustion is signaled by
// XamEnumerate returning ERROR_NO_MORE_FILES.
static uint32_t stub_XamContentCreateEnumerator(uint32_t r3, uint32_t r4,
                                                uint32_t r5, uint32_t r6,
                                                uint32_t r7, uint32_t r8) {
  // r3=dwUserIndex r4=DeviceID r5=dwContentType r6=dwContentFlags r7=cItem
  // r8=pcbBuffer OUT; the 7th arg phEnum rides in r9, which the 6-reg
  // ExportFn signature drops — fetch it from the per-call extra-args channel
  // (ppc_import_glue populates it for every import; precedent: NtReadFile).
  (void)r6;
  const uint32_t phEnum = ExportRegistry::getCurrentArgR9();
  if (r3 != 0) return 0xFFFFFFFFu;  // no such user (guest tests nonzero only)
  uint32_t handle;
  {
    std::lock_guard<std::mutex> lk(g_xamEnumMutex);
    handle = g_xamEnumNextHandle++;
    g_xamEnums[handle] = 0;  // fresh console: zero save packages
  }
  constexpr uint32_t kXContentDataSize = 0x134;  // sizeof(XCONTENT_DATA)
  if (r8 && isGuestVAInRange(r8)) writeGuest32(r8, kXContentDataSize * r7);
  if (phEnum && isGuestVAInRange(phEnum)) writeGuest32(phEnum, handle);
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = ++s_n;
  if (n <= 4)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[XAM] XamContentCreateEnumerator #%u user=%u dev=%08X "
                "type=%u cItem=%u -> h=%08X (phEnum=%08X pcb=%08X) lr=%08X",
                n, r3, r4, r5, r7, handle, phEnum, r8,
                sr::kernel::getCurrentGuestLR());
  return 0;  // ERROR_SUCCESS — empty set still succeeds (Xenia/reference)
}

static uint32_t stub_XamEnumerate(uint32_t r3, uint32_t r4, uint32_t r5,
                                  uint32_t r6, uint32_t r7, uint32_t r8) {
  // r3=hEnum r4=dwFlags r5=pvBuffer r6=cbBuffer r7=pcItemsReturned
  // r8=pOverlapped (guest wrapper sub_828A8258 passes flags=0, ovl=0).
  (void)r4; (void)r5; (void)r6;
  constexpr uint32_t kErrorNoMoreFiles = 0x12;  // win32 ERROR_NO_MORE_FILES
  bool known;
  {
    std::lock_guard<std::mutex> lk(g_xamEnumMutex);
    known = g_xamEnums.find(r3) != g_xamEnums.end();
  }
  // Empty/exhausted is the only live case (no save registry yet). Write the
  // count even on the failure return (Xenia oracle: *items_returned = 0);
  // the buffer stays untouched. MANDATORY nonzero return at exhaustion:
  // FindNext never reads the count and loops while rc==0.
  if (r7 && isGuestVAInRange(r7)) writeGuest32(r7, 0);
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = ++s_n;
  if (n <= 4 || !known)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[XAM] XamEnumerate #%u h=%08X%s buf=%08X cb=%u pcItems=%08X "
                "ovl=%08X -> NO_MORE_FILES lr=%08X",
                n, r3, known ? "" : " (UNKNOWN)", r5, r6, r7, r8,
                sr::kernel::getCurrentGuestLR());
  return kErrorNoMoreFiles;
}

// XamUserGetName: writes a username string for the given user index.
// r3 = dwUserIndex, r4 = pUserName (guest VA), r5 = cbUserName (buffer size)
static uint32_t stub_XamUserGetName(uint32_t r3, uint32_t r4, uint32_t r5,
                                     uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r6; (void)r7; (void)r8;
  // iter-464 platform fix (SR_XAM_USERNAME, default ON): Xenia's semantics + the player's
  // gamertag (kernel/xam_profile.cpp). =0 keeps the old stub below.
  if (sr::kernel::xamUserNameEnabled()) return sr::kernel::xamUserGetName(r3, r4, r5);
  if (r3 != 0) return 1u;  // ERROR_NO_SUCH_USER for indices != 0
  uint8_t* host = guestVAToHost(r4);
  if (host && r5 >= 8) {
    const char* name = "Player";
    std::memcpy(host, name, 7);  // includes null terminator
  }
  return 0;  // success
}

// XamInputGetCapabilities: r3=userIndex, r4=flags, r5=pCapabilities
// Returns ERROR_SUCCESS for user 0, ERROR_DEVICE_NOT_CONNECTED for others.
static uint32_t stub_XamInputGetCapabilities(uint32_t r3, uint32_t r4, uint32_t r5,
                                             uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r6; (void)r7; (void)r8;
  constexpr uint32_t kErrorDeviceNotConnected = 0x048F;
  if (r3 != 0 && !(r3 == 1 && devToolsFreecamRouting())) return kErrorDeviceNotConnected;
  // iter-464 input lane: player 1's pad was unplugged (kernel/host_input.hpp) - the same
  // "not connected" XamInputGetState reports, so the game's newly-connected path
  // (InputXenonPad_Update_sub_825FFD40 -> this call) sees one consistent controller.
  if (r3 == 0 && sr::kernel::hostinput::sdlPadEnabled() && !sr::kernel::hostinput::player1Connected()) {
    if (r5 && isGuestVAInRange(r5)) std::memset(guestVAToHost(r5), 0, 20);
    return kErrorDeviceNotConnected;
  }
  if (r5 && isGuestVAInRange(r5)) {
    uint8_t* caps = guestVAToHost(r5);
    std::memset(caps, 0, 20);
    caps[0] = 1;  // Type = XINPUT_DEVTYPE_GAMEPAD
    caps[1] = 1;  // SubType = XINPUT_DEVSUBTYPE_GAMEPAD
    // Gamepad at offset 4: wButtons = 0xF3FF (all buttons supported)
    caps[4] = 0xF3; caps[5] = 0xFF;
    // Triggers at offset 6-7: 0xFF each
    caps[6] = 0xFF; caps[7] = 0xFF;
  }
  return 0;
}

// ── iter-450S: KEYBOARD-AS-GAMEPAD (platform fix, default ON) ───────────────
// The console's controller surface, fed by a REAL host device: SDL's live
// keyboard state (updated by the app's main-thread event pump; reading the
// state array cross-thread is the reference's own precedent, xam.cpp:404-460)
// is OR-merged into the pad state the guest polls. This is NOT the banned
// iter-449t class — nothing is synthesized, no timers, no auto-press; only
// keys a human is physically holding, and ONLY while the game window has
// focus (guest_context.hpp hostWindowFocused — background keystrokes can
// never leak into an unattended roll). SR_KB_PAD=0 disables for A/B.
// iter-464 bgruns: automated runs (SR_INSTANCE_ID) read the HARNESS keyboard
// channel instead (no focus needed) and ignore the real keyboard and pads
// unless SR_HARNESS_REAL_INPUT=1 - kernel/host_automation.hpp.
// Mapping (reference model): Enter=START, Esc/Backspace=BACK, arrows=DPAD,
// Z/Space=A, X=B, C=X, V=Y, Q/E=LB/RB, WASD=left stick.
#ifdef _WIN32
// ── iter-450UJ: THE KEYBOARD-MERGE OBSERVER (SR_KB_PAD_OBS, default OFF) ────
// C-450Sb ("does Enter naturally reach the guest?") is UNMEASURABLE today, and
// the reason is an instrument gap, not a result: `[INPUT]` samples only at poll
// 1/100/1000/every-10000, and mergeKeyboardPad returns SILENTLY when the gate is
// off, the window is unfocused, or SDL has no keyboard state — three very
// different facts that all look identical in the log (all of them produce
// btn=0x0000). This records which one happened.
//
// ⛔ OBSERVATION ONLY, stated precisely rather than absolutely:
//   • Gate OFF preserves input VALUES, the SDL sample count and order, the
//     guest writes, and merge behaviour — but it is not literally zero work:
//     the observer gate itself is still evaluated once per poll.
//   • Observer ON adds bounded diagnostic branching and logging cost. Any
//     enabled logger has a timing cost; claiming otherwise would be false.
//   • Neither state takes an extra SDL_GetKeyboardState sample — the observer
//     reads the pointer the merge already fetched.
// It writes no guest memory beyond the Xam result the stub already writes,
// synthesises no input, and does not touch input_recorder.
static bool kbObsEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_KB_PAD_OBS");
    return e && e[0] == '1';   // DEFAULT OFF
  }();
  return v;
}

struct KbPadObs {
  bool gateOn = false;
  bool focused = false;
  bool sdlOk = false;
  bool harness = false;   // iter-464 bgruns: the harness channel was read (src=harness)
  uint16_t mappedMask = 0;
  // 0 = merged, 1 = gate disabled, 2 = unfocused, 3 = SDL state unavailable,
  // 4 = the merge never ran at all (replay supplied the state).
  // ⚠ The DEFAULT is 4, not 1: mergeKeyboardPad sets this on every one of its
  // exit paths, so a value of 4 can only survive when the function was never
  // called. Defaulting to 1 mislabelled a replay bypass as a disabled gate.
  uint8_t reject = 4;
};

// iter-466 controls2: the keyboard / mouse drove the game's actions this poll (the action layer;
// they are no longer in the pad report) - the mouse cursor sleeps on it like on a controller.
static bool s_kbActionsHeld = false;

static void mergeKeyboardPad(input_recorder::GamepadState& gps,
                             KbPadObs* obs = nullptr) {
  s_kbActionsHeld = false;
  static const bool s_kbPad = [] {
    const char* e = std::getenv("SR_KB_PAD");
    return !e || e[0] != '0';
  }();
  if (obs) obs->gateOn = s_kbPad;
  if (!s_kbPad) {
    // ⚠ Gate off AND observer off reads focus ZERO times — the original short
    // circuit is preserved exactly. With the observer on we take one read, only
    // so the rejection can be described.
    if (obs) { obs->focused = hostWindowFocused(); obs->reject = 1; }
    return;
  }
  // ⛔ EXACTLY ONE FOCUS SAMPLE, used for BOTH the decision and the report.
  // Reading hostWindowFocused() twice (once to populate the observation, once
  // to decide) let focus change between the two atomic loads and could emit
  // self-contradicting evidence — focus=1 alongside REJECTED:unfocused. The
  // observation must describe the decision that was actually taken.
  const bool focused = hostWindowFocused();
  if (obs) obs->focused = focused;
  // ── iter-464 bgruns: THE HARNESS KEYBOARD (host_automation.hpp) ─────────────
  // On an automated run the harness channel IS the keyboard: no focus needed, so
  // the window stays in the background. Its scancode bytes go through the same
  // mapping table as SDL's state below, so a harness key reaches the guest
  // exactly as that physical key would. Real keys are merged too ONLY when the
  // run allows real input (players always; automated runs only with
  // SR_HARNESS_REAL_INPUT=1) - input isolation for background rolls.
  namespace A = sr::kernel::automation;
  namespace HI = sr::kernel::hostinput;
  const A::Policy& pol = A::policy();
  // ── iter-464 controls lane: THE PC CONTROL SCHEME (kernel/pc_controls.hpp) ──────────────
  // Players: the designed keyboard + mouse layout (game actions, mouse-look, the Dodge key);
  // rolls: the harness channel keeps the iter-450S TEST keymap (sr_drive's key names) unless
  // its flags select the player keymap. SR_PC_CONTROLS=0 = the code below, unchanged (A/B).
  namespace PC = sr::kernel::pccontrols;
  if (PC::enabled()) {
    const uint8_t* hk = pol.harnessInput ? A::harnessKeys() : nullptr;
    const uint8_t* rk = nullptr;
    if (pol.realInput && focused) rk = sdlKeyboardBytes();
    PC::Pad pd;
    pd.buttons = gps.wButtons;
    pd.lt = gps.bLeftTrigger; pd.rt = gps.bRightTrigger;
    pd.lx = gps.sThumbLX; pd.ly = gps.sThumbLY; pd.rx = gps.sThumbRX; pd.ry = gps.sThumbRY;
    bool anyHeld = false;
    const uint16_t added = PC::mergePoll(&pd, hk, rk, pol.realInput && focused, &anyHeld);
    if (hk) A::harnessNoteMerge(added);
    if (obs) {
      obs->harness = hk != nullptr;
      obs->sdlOk = rk != nullptr;
      obs->reject = (hk || rk) ? 0 : (pol.realInput ? (focused ? 3 : 2) : 5);
      obs->mappedMask = added;
    }
    gps.wButtons = pd.buttons;
    gps.bLeftTrigger = pd.lt; gps.bRightTrigger = pd.rt;
    gps.sThumbLX = pd.lx; gps.sThumbLY = pd.ly; gps.sThumbRX = pd.rx; gps.sThumbRY = pd.ry;
    if (anyHeld) HI::noteKeyboardUse();
    s_kbActionsHeld = anyHeld;
    return;
  }
  // iter-464 input lane: a poll where the keyboard changed the report makes the keyboard
  // player 1's device (kernel/host_input.hpp noteKeyboardUse) - what the prompt glyphs follow.
  // CONTRACT for the controls lane (docs/INPUT_GLYPHS.md): its mergePoll path reports the
  // same through its *anyHeld.
  const int16_t lx0 = gps.sThumbLX, ly0 = gps.sThumbLY;
  uint16_t b = 0;
  if (pol.harnessInput) {
    const uint8_t* hk = A::harnessKeys();
    if (hk) {
      const uint16_t hb = A::mapKeysToPad(hk, &gps.sThumbLX, &gps.sThumbLY, &gps.sThumbRX, &gps.sThumbRY);
      A::harnessNoteMerge(hb);
      b |= hb;
      if (obs) { obs->harness = true; obs->reject = 0; }
    } else if (obs) {
      obs->reject = 3;   // channel requested but unavailable ([AUTOMATION] logged why)
    }
  }
  // iter-466 controls2 (USER DECISION 2026-10-06, "no virtual presses"): the real keyboard no
  // longer maps to pad buttons here; with SR_PC_CONTROLS=0 it does nothing. Only the harness
  // TEST keymap above (sr_drive's key names - test tooling) still lands in the pad report.
  if (pol.realInput) {
    if (obs && !obs->harness) obs->reject = 6;
  } else if (obs && !obs->harness && obs->reject == 4) {
    obs->reject = 5;   // automated run, real keyboard ignored, no harness channel
  }
  if (obs) obs->mappedMask = b;
  gps.wButtons |= b;
  if (b || gps.sThumbLX != lx0 || gps.sThumbLY != ly0) sr::kernel::hostinput::noteKeyboardUse();
}
#endif

#ifdef _WIN32
// ── iter-464 DT-2: the dev tools' host input (docs/DEVTOOLS.md 6.4) ─────────
// F6 toggles the free camera (only with the `freecam` tool on); F7, while the free camera is on and the
// `cheats` tool too, warps Superman to the camera ("warp camera"); F8 (`cheats`) toggles
// infinite_stamina. None of the three is in the keyboard merge. Each SUBMITS a command; the exe runs it on the game's own tick thread
// (kernel/devtools_levelskip.cpp).
//
// iter-464 (after bgruns): a dev-tool key is read from the SAME two sources as the keyboard merge -
// the harness keyboard channel (automated runs; no focus needed) and the real keyboard (focused
// window, only when the run allows real input). So an automated roll presses F6 / F7 / F8 / IJKL /
// R / F through the channel, and the user's own typing never reaches a background roll.
static bool devHostKeyDown(int sc) {
  namespace A = sr::kernel::automation;
  const A::Policy& pol = A::policy();
  if (pol.harnessInput)
    if (const uint8_t* hk = A::harnessKeys()) if (hk[sc]) return true;
  if (pol.realInput && hostWindowFocused())
    if (const uint8_t* k = sdlKeyboardBytes()) if (k[sc]) return true;
  return false;
}

static void devToolsHotkeys() {
  static bool s_prev6 = false, s_prev7 = false, s_prev8 = false, s_prev10 = false, s_stamina = false;
  const bool freecamTool = devToolAllowed("freecam"), cheatsTool = devToolAllowed("cheats");
  const bool skipTool = devToolAllowed("levelskip");
  if (!freecamTool && !cheatsTool && !skipTool) return;   // gate off: not even a key read
  // iter-465 playthrough: F10 (tool levelskip) = SKIP CURRENT MISSION (`skip_mission`: passes the
  // objective the game has active, story or side, with the plan's own `pass` stores)
  const bool down10 = skipTool && devHostKeyDown(SDL_SCANCODE_F10);
  if (down10 && !s_prev10) devToolsSubmit("skip_mission", nullptr);
  s_prev10 = down10;
  // F11 (tool levelskip) = SKIP CHAPTER (`skip_chapter`: on to the next chapter through its story objective)
  static bool s_prev11 = false;
  const bool down11 = skipTool && devHostKeyDown(SDL_SCANCODE_F11);
  if (down11 && !s_prev11) devToolsSubmit("skip_chapter", nullptr);
  s_prev11 = down11;
  if (!freecamTool && !cheatsTool) return;
  const bool down6 = devHostKeyDown(SDL_SCANCODE_F6);
  const bool down7 = devHostKeyDown(SDL_SCANCODE_F7);
  const bool down8 = devHostKeyDown(SDL_SCANCODE_F8);
  if (freecamTool) {
    if (down6 && !s_prev6) devToolsSubmit(devToolsFreecamRouting() ? "freecam off" : "freecam on", nullptr);
    if (down7 && !s_prev7 && devToolsFreecamRouting()) devToolsSubmit("warp camera", nullptr);
  }
  // F8 (tool cheats): infinite_stamina on / off, alternately (the readout has the real state)
  if (cheatsTool) {
    if (down8 && !s_prev8 && devToolsSubmit(s_stamina ? "infinite_stamina off" : "infinite_stamina on", nullptr))
      s_stamina = !s_stamina;
  }
  // iter-467 native-a: F2 (tool cheats) = the PRESET warp `warp X Y Z` from SR_DEVTOOLS_WARP ("X Y Z", or
  // "X Y Z;X Y Z;..." - each press takes the next point, the last one repeats). Unset: F2 does nothing. F2 because
  // nothing else binds it (F12 is the front end's screenshot key; F6-F11 are dev tools, F9 the capture hotkey).
  // The command channel has no automated line input, and the harness keyboard knows no F2, so an automated roll
  // fires the same preset with SR_DEVTOOLS_WARP_ON_MARK=1 at the harness channel's MARK (the tape's `mark dump`,
  // kHarnessFlagDumpMark), once - every arm of a diagnostic roll gets the same point (the freeze oracle's sea
  // scene, save_gate_drift). The warp itself is the existing `warp X Y Z` ([DEVTOOL] act=cheats.warp: before /
  // target / after).
  if (cheatsTool) {
    static bool s_prev2 = false, s_markDone = false;
    static unsigned s_warpNext = 0;
    const bool down2 = devHostKeyDown(SDL_SCANCODE_F2);
    const bool mark = !s_markDone && devToolsWarpOnMark() &&
                      (sr::kernel::automation::harnessFlags() & sr::kernel::automation::kHarnessFlagDumpMark) != 0;
    if (mark) s_markDone = true;
    if ((down2 && !s_prev2) || mark) {
      const char* src = mark ? "harness mark" : "F2";
      static const std::string s_presets = devToolsWarpPresets();  // SR_DEVTOOLS_WARP, read in kernel/devtools.cpp
      std::vector<std::string> pts;
      for (size_t a = 0; a <= s_presets.size();) {
        size_t b = s_presets.find(';', a);
        if (b == std::string::npos) b = s_presets.size();
        if (b > a) pts.push_back(s_presets.substr(a, b - a));
        a = b + 1;
      }
      if (pts.empty()) {
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[DEVTOOL] %s: SR_DEVTOOLS_WARP is not set - no preset warp", src);
      } else {
        const std::string line = "warp " + pts[std::min<size_t>(s_warpNext, pts.size() - 1)];
        std::string err;
        const uint64_t t = devToolsSubmit(line, &err);
        SR_LOG_BOOT(sr::LogSeverity::Info, "[DEVTOOL] %s preset warp #%u: \"%s\" -> ticket %llu%s%s", src, s_warpNext + 1,
                    line.c_str(), (unsigned long long)t, t ? "" : " refused: ", t ? "" : err.c_str());
        ++s_warpNext;
      }
    }
    s_prev2 = down2;
  }
  s_prev6 = down6;
  s_prev7 = down7;
  s_prev8 = down8;
}

// While the retail debug camera is on, guest CONTROLLER 2 is the host's pad 0 + keyboard: the dev
// switch put controller slot 1 in the DebugContext and the dev flew the camera with a second pad
// (InputCtx_DebugContext_Bind_sub_8229AFA0: sticks move / look, RT / LT up / down, RB / LB speed,
// A / B look rate, START pause, LB+START resume, R3 single step, X target lock, L3 entity info).
// The keyboard adds IJKL = right stick (look) and R / F = RT / LT to the usual merge (WASD = left
// stick, Q / E = LB / RB, Z / X / C / V = A / B / X / Y, Enter = START). Controller 1 reads neutral.
static uint32_t freecamPadState(uint32_t r5) {
  static std::atomic<uint32_t> s_packet{0};
  input_recorder::GamepadState gps{};
  XINPUT_STATE xs = {};
  // input isolation (host_automation.hpp): an automated run never reads the user's real pad
  if (sr::kernel::automation::policy().realInput && XInputGetState(0, &xs) == ERROR_SUCCESS) {
    gps.wButtons = xs.Gamepad.wButtons;
    gps.bLeftTrigger = xs.Gamepad.bLeftTrigger;
    gps.bRightTrigger = xs.Gamepad.bRightTrigger;
    gps.sThumbLX = xs.Gamepad.sThumbLX;
    gps.sThumbLY = xs.Gamepad.sThumbLY;
    gps.sThumbRX = xs.Gamepad.sThumbRX;
    gps.sThumbRY = xs.Gamepad.sThumbRY;
  }
  mergeKeyboardPad(gps);
  if (devHostKeyDown(SDL_SCANCODE_I)) gps.sThumbRY = 32767;
  else if (devHostKeyDown(SDL_SCANCODE_K)) gps.sThumbRY = -32767;
  if (devHostKeyDown(SDL_SCANCODE_J)) gps.sThumbRX = -32767;
  else if (devHostKeyDown(SDL_SCANCODE_L)) gps.sThumbRX = 32767;
  if (devHostKeyDown(SDL_SCANCODE_R)) gps.bRightTrigger = 255;
  if (devHostKeyDown(SDL_SCANCODE_F)) gps.bLeftTrigger = 255;
  if (r5 && isGuestVAInRange(r5)) {
    uint8_t* p = guestVAToHost(r5);
    writeGuestBE32(r5 + 0, s_packet.fetch_add(1, std::memory_order_relaxed));
    p[4] = uint8_t(gps.wButtons >> 8);
    p[5] = uint8_t(gps.wButtons & 0xFF);
    p[6] = gps.bLeftTrigger;
    p[7] = gps.bRightTrigger;
    auto s16 = [&](int off, int16_t v) {
      p[off] = uint8_t(uint16_t(v) >> 8);
      p[off + 1] = uint8_t(uint16_t(v) & 0xFF);
    };
    s16(8, gps.sThumbLX);
    s16(10, gps.sThumbLY);
    s16(12, gps.sThumbRX);
    s16(14, gps.sThumbRY);
  }
  return 0;
}
#endif

// XamInputGetState: r3=userIndex, r4=flags, r5=pState (XINPUT_STATE)
// Reads real controller state via XInput and writes big-endian to guest memory.
// Guest XINPUT_STATE layout (same fields as PC, all multi-byte values BE):
//   +0  dwPacketNumber  BE32
//   +4  wButtons        BE16
//   +6  bLeftTrigger    u8
//   +7  bRightTrigger   u8
//   +8  sThumbLX        BE16
//   +10 sThumbLY        BE16
//   +12 sThumbRX        BE16
//   +14 sThumbRY        BE16
static uint32_t stub_XamInputGetState(uint32_t r3, uint32_t r4, uint32_t r5,
                                      uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r6; (void)r7; (void)r8;
  constexpr uint32_t kErrorDeviceNotConnected = 0x048F;
#ifdef _WIN32
  if (r3 == 1 && devToolsFreecamRouting()) return freecamPadState(r5);   // DT-2 freecam
#endif
  if (r3 != 0) return kErrorDeviceNotConnected;

#ifdef _WIN32
  devToolsHotkeys();   // DT-2: F6 = freecam on/off (a no-op unless the freecam tool is on)
  static std::atomic<uint32_t> s_callCount{0};
  uint32_t call = s_callCount.fetch_add(1, std::memory_order_relaxed);

  input_recorder::GamepadState gps{};
  // iter-462: replay and record are separate calls - the old single tick() ran
  // here AND after the merge, so a recording wrote two entries per poll (an
  // empty state, then the real one).
  bool replayed = input_recorder::replayTick(call, gps);
  // iter-450UJ: evaluated ONCE per poll and reused for both the struct hand-off
  // and the logging decision, so the two can never disagree.
  const bool kbObsOn = kbObsEnabled();
  KbPadObs kbObs{};
  uint16_t kbPreMerge = 0;
  bool kbMerged = false;

  // iter-464 input lane: player 1's pad was unplugged - the console's "controller removed"
  // until a device is used again (kernel/host_input.hpp). The state is zeroed as Xenia's
  // XamInputGetState_entry does before it returns X_ERROR_DEVICE_NOT_CONNECTED. A replay
  // tape still owns the report (it recorded a connected pad).
  namespace HI = sr::kernel::hostinput;
  const bool sdlPad = HI::sdlPadEnabled();
  if (!replayed && sdlPad && !HI::player1Connected()) {
    if (r5 && isGuestVAInRange(r5)) std::memset(guestVAToHost(r5), 0, 16);
    static std::atomic<uint32_t> s_ncLog{0};
    const uint32_t nc = s_ncLog.fetch_add(1, std::memory_order_relaxed) + 1;
    if (nc <= 4 || nc % 1000 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[INPUT] XamInputGetState user 0 -> NOT CONNECTED (player 1's pad unplugged) "
                  "#%u poll=%u", nc, call);
    return kErrorDeviceNotConnected;
  }

  if (!replayed) {
    if (sdlPad) {
      // SDL game controllers (hot-plug, every pad family): player 1's active pad.
      HI::PadState ps;
      if (HI::readActivePad(&ps)) {
        gps.wButtons      = ps.buttons;
        gps.bLeftTrigger  = ps.lt;
        gps.bRightTrigger = ps.rt;
        gps.sThumbLX      = ps.lx;
        gps.sThumbLY      = ps.ly;
        gps.sThumbRX      = ps.rx;
        gps.sThumbRY      = ps.ry;
      }
    } else {
      // SR_SDL_PAD=0: the pre-iter-464 path (XInput slot 0 only, no hot-plug).
      XINPUT_STATE xs = {};
      // iter-464 bgruns: input isolation - an automated run does not read the user's real
      // controller (host_automation.hpp; SR_HARNESS_REAL_INPUT=1 opts back in).
      DWORD res = sr::kernel::automation::policy().realInput ? XInputGetState(0, &xs)
                                                             : ERROR_DEVICE_NOT_CONNECTED;

      if (res == ERROR_SUCCESS) {
        gps.wButtons      = xs.Gamepad.wButtons;
        gps.bLeftTrigger  = xs.Gamepad.bLeftTrigger;
        gps.bRightTrigger = xs.Gamepad.bRightTrigger;
        gps.sThumbLX      = xs.Gamepad.sThumbLX;
        gps.sThumbLY      = xs.Gamepad.sThumbLY;
        gps.sThumbRX      = xs.Gamepad.sThumbRX;
        gps.sThumbRY      = xs.Gamepad.sThumbRY;
      }
    }
    // iter-449t: the ancient "synthesise Start+A after 180 calls" arm is
    // DELETED — it was ungated FORCING (fabricated input the user never
    // pressed) hiding in an import stub since before the charter, and it
    // was the KILLER OF THE TITLE MOVIE: the guest's skip-action check
    // (ev187 -> sub_8229AF18 -> input query) read the phantom Start+A
    // ~2.8s into movie "0100" (the only skippable movie in the intro
    // chain — the ladder climber arms PSM+553 for exactly that name) and
    // executed it, freezing the starfield frame for the rest of the roll.
    // No controller now means what the console meant: neutral input.
    //
    // iter-450S: the keyboard merge happens BEFORE the record tick so
    // SR_INPUT_RECORD tapes capture the merged pad+keyboard state and
    // SR_INPUT_REPLAY stays deterministic.
    // iter-450UJ: the observer rides the EXISTING merge — same single
    // SDL_GetKeyboardState, same order, same semantics. `preMerge` is snapshotted
    // here so the log can separate controller buttons from keyboard-OR'd ones.
    kbPreMerge = gps.wButtons;
    mergeKeyboardPad(gps, kbObsOn ? &kbObs : nullptr);
    kbMerged = true;
    // iter-463 phase 2 (DEVTOOL `levelskip`, default OFF; docs/DEVTOOLS.md 3): a plan's `pad`
    // step presses its buttons here, as a player would (the MSTobj03 save prompt). Always 0
    // unless levelskip queued a press; the stub's own XINPUT_STATE store below carries it.
    gps.wButtons |= devToolsVirtualPadButtons();
    // iter-464 PLATFORM (docs/MOUSE_CURSOR.md): the in-game mouse cursor. iter-466 controls2: it
    // calls the menus' own functions (kernel/mouse_ui.cpp, after the Apt pump) and presses
    // nothing here; real pad / keyboard input puts it to sleep.
    {
      const bool sticks = gps.sThumbLX > 7849 || gps.sThumbLX < -7849 ||
                          gps.sThumbLY > 7849 || gps.sThumbLY < -7849 ||
                          gps.sThumbRX > 8689 || gps.sThumbRX < -8689 ||
                          gps.sThumbRY > 8689 || gps.sThumbRY < -8689 ||
                          gps.bLeftTrigger > 30 || gps.bRightTrigger > 30;
      mouseUiNoteOtherInput(gps.wButtons != 0 || sticks || s_kbActionsHeld);
    }
    // iter-465 modlib (kernel/mod_library.hpp): while the mod library overlay is up the game
    // reads a neutral pad - what a 360 title read while the system Guide was open - and after
    // it closes, until everything is released, so the B / Esc that closed it never reaches the
    // menu underneath. Inert (one atomic load) while the library has never been opened.
    {
      const bool held = gps.wButtons != 0 || gps.bLeftTrigger > 30 || gps.bRightTrigger > 30 ||
                        gps.sThumbLX > 7849 || gps.sThumbLX < -7849 || gps.sThumbLY > 7849 ||
                        gps.sThumbLY < -7849 || gps.sThumbRX > 8689 || gps.sThumbRX < -8689 ||
                        gps.sThumbRY > 8689 || gps.sThumbRY < -8689;
      if (sr::kernel::modlib::blockGameInput(held)) gps = input_recorder::GamepadState{};
    }
    // iter-465 viewer: the COSTUME VIEWER turns / zooms the suit with the right stick the game reads
    sr::kernel::costumeViewerNotePad(gps.sThumbRX, gps.sThumbRY);
    input_recorder::recordTick(call, gps);
  }

  // DT-2 freecam: the host pad flies the debug camera through controller 2; controller 1 reads
  // neutral meanwhile (Superman stands still). Always false unless the freecam tool turned it on.
  if (devToolsFreecamRouting()) gps = input_recorder::GamepadState{};
  const bool wroteGuest = (r5 && isGuestVAInRange(r5));
  if (wroteGuest) {
    uint8_t* p = guestVAToHost(r5);
    writeGuestBE32(r5 + 0, call);  // dwPacketNumber
    p[4] = uint8_t(gps.wButtons >> 8);
    p[5] = uint8_t(gps.wButtons & 0xFF);
    p[6] = gps.bLeftTrigger;
    p[7] = gps.bRightTrigger;
    auto writeS16BE = [&](int offset, int16_t v) {
      p[offset + 0] = uint8_t(uint16_t(v) >> 8);
      p[offset + 1] = uint8_t(uint16_t(v) & 0xFF);
    };
    writeS16BE(8,  gps.sThumbLX);
    writeS16BE(10, gps.sThumbLY);
    writeS16BE(12, gps.sThumbRX);
    writeS16BE(14, gps.sThumbRY);
  }

  if (call == 1 || call == 100 || call == 1000 || call % 10000 == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[INPUT] XamInputGetState call #%u btn=0x%04X LT=%u RT=%u%s",
      call, gps.wButtons, gps.bLeftTrigger, gps.bRightTrigger,
      replayed ? " [replay]" : "");

  // ── iter-450UJ [KBPAD_OBS] ────────────────────────────────────────────────
  // Triggers, per the ruling: the first observation, EVERY focus-state change,
  // EVERY mapped-mask change, and EVERY nonzero mapped-mask sample. Unchanged
  // neutral polls are throttled so a held key is never lost in the noise.
  if (kbObsOn) {
    static bool s_first = true;
    static bool s_lastFocus = false;
    static uint16_t s_lastMask = 0;
    static uint8_t s_lastReject = 0xFF;
    static uint64_t s_neutral = 0;
    const char* why = kbObs.reject == 0 ? "merged"
                    : kbObs.reject == 1 ? "REJECTED:gate-disabled"
                    : kbObs.reject == 2 ? "REJECTED:unfocused"
                    : kbObs.reject == 3 ? "REJECTED:sdl-state-unavailable"
                    : kbObs.reject == 5 ? "REJECTED:real-input-isolated"
                    : kbObs.reject == 6 ? "REJECTED:no-virtual-pad (SR_PC_CONTROLS=0)"
                                        : "BYPASSED:replay";
    if (s_first)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[KBPAD_OBS] ENABLED — observing the existing keyboard merge "
          "(no extra SDL sample; no guest write beyond the Xam result; "
          "input_recorder untouched; this logger itself costs bounded "
          "diagnostic time). Enter=START(0x0010).");
    const bool focusChanged = (kbObs.focused != s_lastFocus);
    const bool maskChanged = (kbObs.mappedMask != s_lastMask);
    const bool rejectChanged = (kbObs.reject != s_lastReject);
    const bool interesting = s_first || focusChanged || maskChanged ||
                             rejectChanged || kbObs.mappedMask != 0;
    if (interesting || (++s_neutral % 4000) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[KBPAD_OBS] poll=%u %s gate=%u focus=%u sdl=%u mappedMask=0x%04X "
          "preMergeBtn=0x%04X finalBtn=0x%04X guestWritten=%u merged=%u src=%s%s%s",
          call, why, kbObs.gateOn ? 1u : 0u, kbObs.focused ? 1u : 0u,
          kbObs.sdlOk ? 1u : 0u, kbObs.mappedMask, kbPreMerge, gps.wButtons,
          wroteGuest ? 1u : 0u, kbMerged ? 1u : 0u,
          kbObs.harness ? (kbObs.sdlOk ? "harness+sdl" : "harness")
                        : (kbObs.sdlOk ? "sdl" : "none"),
          focusChanged && !s_first ? " [FOCUS-CHANGED]" : "",
          maskChanged && !s_first ? " [MASK-CHANGED]" : "");
    s_first = false;
    s_lastFocus = kbObs.focused;
    s_lastMask = kbObs.mappedMask;
    s_lastReject = kbObs.reject;
  }
  return 0;
#else
  return kErrorDeviceNotConnected;
#endif
}

// iter-464 input lane: XamInputSetState(userIndex r3, flags r4 (unused, as in Xenia),
// pVibration r5 = XINPUT_VIBRATION { BE16 wLeftMotorSpeed; BE16 wRightMotorSpeed; }).
// Was REG_GENERIC_STUB (return 0, rumble dropped). The speeds go to player 1's SDL pad
// (kernel/host_input.cpp tick() applies them on the main thread). Return values follow
// Xenia xam_input.cc XamInputSetState_entry: bad user -> DEVICE_NOT_CONNECTED, null
// pointer -> BAD_ARGUMENTS.
static uint32_t stub_XamInputSetState(uint32_t r3, uint32_t r4, uint32_t r5,
                                      uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r6; (void)r7; (void)r8;
  constexpr uint32_t kErrorDeviceNotConnected = 0x048F;
  constexpr uint32_t kErrorBadArguments = 0x00A0;
  if (r3 != 0) return kErrorDeviceNotConnected;
  if (!r5 || !isGuestVAInRange(r5)) return kErrorBadArguments;
  const uint8_t* v = guestVAToHost(r5);
  const uint16_t left = uint16_t((v[0] << 8) | v[1]);
  const uint16_t right = uint16_t((v[2] << 8) | v[3]);
  namespace HI = sr::kernel::hostinput;
  if (HI::sdlPadEnabled() && !HI::player1Connected()) return kErrorDeviceNotConnected;
  HI::setVibration(left, right);
  static std::atomic<uint32_t> s_n{0};
  static std::atomic<uint32_t> s_last{0xFFFFFFFFu};
  const uint32_t packed = (uint32_t(left) << 16) | right;
  if (s_last.exchange(packed, std::memory_order_relaxed) != packed) {
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16 || n % 500 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[INPUT] XamInputSetState vibration L=%u R=%u (change #%u) lr=%08X", left,
                  right, n, sr::kernel::getCurrentGuestLR());
  }
  return 0;
}

// ---------------------------------------------------------------------------
// iter-351: XNotify system-notification subsystem (PLATFORM FIX, not forcing).
//
// The real 360 dashboard delivers system notifications the guest polls via
// XNotifyGetNext to sequence its boot. Superman's boot-flow driver sub_822B5910
// (ticked every frame from FeMasterTick) advances ONLY on XN_SYS_UI (id=9,
// param=0) = "system UI closed": with its flow object ready (DAT_8296eda0+0x100
// == 0) it calls Function_822B4BC8 -> Press-Start -> the frontend Apt movie
// load. The previous stub was a GLOBAL one-shot that faked only
// XN_SYS_SIGNINCHANGED (id=10, param=1) and NEVER delivered (9,0), so the boot
// flow never started and aptCount stayed 0 (the whole menu wall was downstream).
//
// Implement the real per-listener queue and deliver (9,0) the way the console
// does when the launch/sign-in blade closes at boot. Verified against a shipping
// 360 recomp (UnleashedRecomp kernel/xam.cpp, which enqueues (9,0) on every
// system-UI close). The guest still makes every decision FROM the notification —
// we only implement the platform's delivery side. SR_XNOTIFY_BOOTUI=0 A/Bs off.
// ---------------------------------------------------------------------------
namespace {
struct XNotifyListener {
  uint32_t handle;
  uint64_t areas;
  std::deque<std::pair<uint32_t, uint32_t>> queue;  // (id, param) FIFO
  uint32_t maxVersion = 0xFFFFFFFFu;                // SR_XNOTIFY_XENIA: the listener's max_version (r4)
  void* waitEvent = nullptr;  // SR_XNOTIFY_XENIA: the host manual-reset event behind the handle (Xenia wait_handle_)
  // SR_XNOTIFY_XENIA: NtClose dropped the guest handle. Xenia keeps the object in KernelState::notify_listeners_
  // (kernel_state.cc:1039) and keeps broadcasting to it (:1080), and UnregisterNotifyListener has no caller, so the
  // listener stays: it still queues and signals (a wait that already holds its event wakes), but no lookup by handle
  // finds it any more.
  bool closed = false;
};
std::mutex g_xnMutex;
std::vector<XNotifyListener> g_xnListeners;
uint32_t g_xnNextHandle = 0x00058000u;
// The system-UI state our XAM model last reported (XN_SYS_UI 9: 1 = a blade is up, 0 = closed): Xenia's
// XamState::IsUIActive, which RegisterNotifyListener primes the first System listener with. Guarded by g_xnMutex.
bool g_xnUiActive = false;
// SR_XNOTIFY_XENIA: KernelState's has_notified_startup_ / _live_startup_ / _xmp_startup_ (guarded by g_xnMutex).
bool g_xnPrimedSystem = false, g_xnPrimedLive = false, g_xnPrimedXmp = false;

// step4/audit (logic audit 9a row 4) SR_XNOTIFY_XENIA (PLATFORM FIX, baked default ON 2026-10-20; =0 = the old queue,
// docs/LOGIC_AUDIT.md 2.0f): the notification queue the way Xenia 0bd090dbe runs it.
//  - A listener keeps only notifications its area mask and max_version admit (XNotifyListener::EnqueueNotification,
//    xnotifylistener.cc:38-51; key = local_id:16 | version:9 | mask_index:6).
//  - Creating a listener primes it (KernelState::RegisterNotifyListener, kernel_state.cc:1037-1067). The FIRST
//    System listener gets XN_SYS_UI(9, IsUIActive = g_xnUiActive), XN_SYS_SIGNINCHANGED(0xA, 1) and
//    XN_SYS_STORAGEDEVICESCHANGED(0xB, 0). The first Live listener gets XN_LIVE_CONNECTIONCHANGED
//    (0x02000001, X_ONLINE_S_LOGON_DISCONNECTED 0x001510F1) and XN_LIVE_LINK_STATE_CHANGED (0x02000003, 0). The
//    first XMP listener gets XN_XMP_PLAYBACKCONTROLLERCHANGED (0x0A000003, IsTitleInPlaybackControl from the XMP
//    state, 1 at boot).
//  - XNotifyGetNext writes *param = 0 and *id = 0 before the lookup and returns 0 with no id pointer
//    (xam_notify.cc:56-94).
// Off: the old path exactly - every notification to every listener, XN_SYS_SIGNINCHANGED(0xA, 1) 1.5 s after the
// first listener and nothing else primed. The title creates 6 listeners: areas 0x01 (System) and 0x2F (System |
// Live | Friends | Custom | XMP), max_version 1.
bool xnotifyXeniaOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_XNOTIFY_XENIA");
    return !e || e[0] != '0';
  }();
  return v;
}

// Caller holds g_xnMutex.
void xnEnqueueTo(XNotifyListener& l, uint32_t id, uint32_t param) {
  if (xnotifyXeniaOn()) {
    const uint32_t maskIndex = (id >> 25) & 0x3Fu, version = (id >> 16) & 0x1FFu;
    if ((l.areas & (1ull << maskIndex)) == 0) return;
    if (version > l.maxVersion) return;
  }
  l.queue.emplace_back(id, param);
#ifdef _WIN32
  if (l.waitEvent) SetEvent(static_cast<HANDLE>(l.waitEvent));   // xnotifylistener.cc:50 wait_handle_->Set()
#endif
}

// Broadcast one notification to every live listener. System notifications are
// broadly relevant; area-mask filtering can be tightened once the boot path is
// verified (delivering (9,0) to an uninterested listener is harmless).
// SR_XNOTIFY_XENIA: filtered per listener as on Xenia (xnEnqueueTo).
void xnEnqueue(uint32_t id, uint32_t param) {
  std::lock_guard<std::mutex> lk(g_xnMutex);
  if (id == 0x09u) g_xnUiActive = param != 0;   // our XAM UI model's state: what XN_SYS_UI last said
  for (auto& l : g_xnListeners) xnEnqueueTo(l, id, param);
}

// DIAGNOSTIC (default OFF): fabricate periodic XN_SYS_UI(9,0) to force-drive the
// boot-flow poll (sub_822B5910) in isolation, for testing WITHOUT the FE movie.
// This delivers a notification the console did NOT send (no blade actually closed)
// -> borderline forcing, so it is opt-in only. The FAITHFUL (9,0) delivery is
// XamShowSigninUI-triggered (stub_XamShowSigninUI below), which fires exactly when
// the guest itself opens+dismisses the sign-in blade (after DoBootFlowProfileLogin
// arms DAT_8296eda0+0x100=0). Verified (batch b3rg3c1vx) that the periodic form
// feeds sub_822B5910 the id=9/param=0 it polls for.
bool xnForceBootUiOn() {
  static const bool v = [] {
    // iter-463 charter: fabricates a notification the console never sent =
    // forcing, so the env gate sits under the master gate.
    const char* e = std::getenv("SR_XNOTIFY_FORCE_BOOTUI");
    return sr::kernel::forcingAllowed() && e && e[0] == '1';
  }();
  return v;
}

// At launch the dashboard reports a signed-in profile; deliver the matching
// XN_SYS_SIGNINCHANGED once (mirrors XamUserGetSigninState returning signed-in).
// The (9,0) system-UI-close notification is NOT fabricated here — it rides on the
// guest's own XamShowSigninUI call (the faithful trigger).
void xnBootDeliveryThread() {
  using namespace std::chrono_literals;
  std::this_thread::sleep_for(1500ms);
  // SR_XNOTIFY_XENIA: the sign-in notification is primed at the first System listener's creation instead.
  if (!xnotifyXeniaOn()) xnEnqueue(0x0Au, 0x01u);   // XN_SYS_SIGNINCHANGED, user 0 signed in
  if (!xnForceBootUiOn()) return;
  for (int i = 0; i < 60; ++i) {    // DIAGNOSTIC only (SR_XNOTIFY_FORCE_BOOTUI=1)
    xnEnqueue(0x09u, 0x00u);
    std::this_thread::sleep_for(1000ms);
  }
}

void xnEnsureBootDelivery() {
  static std::atomic<bool> started{false};
  if (!started.exchange(true))
    std::thread(xnBootDeliveryThread).detach();
}
}  // namespace

void xnotifyBroadcastXenia(uint32_t id, uint32_t param) {
  if (xnotifyXeniaOn()) xnEnqueue(id, param);
}

// SR_XNOTIFY_XENIA (Astra merge review P2): the state change runs under the queue lock and its notification is
// enqueued in the same step, so two concurrent changes publish in the order they happened and the last notification
// is the current state (lock order queue -> the caller's state lock, as the priming path takes queue -> XMP).
// Gate off: the change alone, nothing enqueued.
void xnotifyPublishXenia(uint32_t id, uint32_t (*mutate)(void*), void* ctx) {
  if (!xnotifyXeniaOn()) {
    (void)mutate(ctx);
    return;
  }
  std::lock_guard<std::mutex> lk(g_xnMutex);
  const uint32_t param = mutate(ctx);
  if (id == 0x09u) g_xnUiActive = param != 0;
  for (auto& l : g_xnListeners) xnEnqueueTo(l, id, param);
}

namespace {
std::atomic<bool> g_xnFailNextEventForTest{false};
}
void xnotifyFailNextEventForTest() { g_xnFailNextEventForTest.store(true); }
void* xnotifyListenerEventForTest(uint32_t handle) {
#ifdef _WIN32
  return eventHandleGet(handle);
#else
  (void)handle;
  return nullptr;
#endif
}

void xnotifyResetForTest() {
  std::vector<uint32_t> handles;
  std::vector<void*> closedEvents;
  {
    std::lock_guard<std::mutex> lk(g_xnMutex);
    for (const auto& l : g_xnListeners) {
      if (!l.closed) handles.push_back(l.handle);
      else if (l.waitEvent) closedEvents.push_back(l.waitEvent);
    }
    g_xnListeners.clear();
    g_xnUiActive = false;
    g_xnPrimedSystem = g_xnPrimedLive = g_xnPrimedXmp = false;
  }
  g_xnFailNextEventForTest.store(false);
#ifdef _WIN32
  // the open listeners' events go with their table entries; a closed listener's event has no entry left
  for (uint32_t h : handles) eventHandleClose(h);
  for (void* e : closedEvents) CloseHandle(static_cast<HANDLE>(e));
#endif
}

// NtClose of a listener handle (Xenia: the XNotifyListener object goes; its handle is invalid afterwards).
static bool xnotifyIsListener(uint32_t handle, bool* closed) {
  if (!xnotifyXeniaOn()) return false;
  std::lock_guard<std::mutex> lk(g_xnMutex);
  for (const auto& e : g_xnListeners)
    if (e.handle == handle && e.waitEvent) {
      *closed = e.closed;
      return true;
    }
  return false;
}

// One step under the queue lock, with the table lock nested inside it (queue -> table; nothing takes them the other
// way round): find the listener, refuse a second close, mark it closed and drop its handle mapping. Its host event
// stays with the listener, which keeps receiving broadcasts (Xenia: NtClose drops a handle reference only).
static int xnotifyListenerClose(uint32_t handle) {
  if (!xnotifyXeniaOn()) return 0;
  uint32_t n = 0;
  {
    std::lock_guard<std::mutex> lk(g_xnMutex);
    XNotifyListener* l = nullptr;
    for (auto& e : g_xnListeners)
      if (e.handle == handle) { l = &e; break; }
    if (!l || !l->waitEvent) return 0;
    if (l->closed) return 2;
    l->closed = true;
#ifdef _WIN32
    {
      std::lock_guard<std::mutex> tl(g_eventMutex);
      g_eventHandles.erase(handle);
      g_eventTyped.erase(handle);
    }
#endif
    static uint32_t s_closes = 0;   // guarded by g_xnMutex
    n = ++s_closes;
  }
  // every close up to 256 (the session bound is measured from this line), then powers of two
  if (n <= 256 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[IMPORT_FIXES] XNOTIFY close #%u: listener %08X's handle closed; the listener stays registered "
                "(Xenia) lr=%08X", n, handle, getCurrentGuestLR());
  return 1;
}

// XamNotifyCreateListener(qwAreas): create a real listener with its area mask.
static uint32_t stub_XamNotifyCreateListener(uint32_t r3, uint32_t r4, uint32_t r5,
                                             uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r5; (void)r6; (void)r7; (void)r8;
  uint32_t handle;
  const bool xenia = xnotifyXeniaOn();
  // SR_XNOTIFY_XENIA (Astra batch-2 P2): the listener is a waitable object, as Xenia's XNotifyListener (an XObject
  // with a manual-reset wait_handle_, xnotifylistener.cc:23-28): a unified handle whose Nt wait table entry is that
  // event, set on enqueue and reset when the last notification is taken. Taken BEFORE the queue lock (lock order).
  uint32_t xeniaHandle = 0;
  void* xeniaEvent = nullptr;
#ifdef _WIN32
  if (xenia) {
    // Astra merge review P3: the event first. Without it there is no listener: 0 comes back, and nothing is
    // registered or primed (the first-listener flags stay unspent) and no handle is allocated.
    HANDLE ev = g_xnFailNextEventForTest.exchange(false) ? nullptr : CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) {
      const unsigned long err = GetLastError();
      static std::atomic<uint32_t> s_f{0};
      if (s_f.fetch_add(1, std::memory_order_relaxed) < 8)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[IMPORT_FIXES] XNOTIFY XamNotifyCreateListener(areas=%08X): no wait event (error %lu) -> 0 lr=%08X",
                    r3, err, getCurrentGuestLR());
      return 0;
    }
    xeniaHandle = allocUnifiedHandle();
    {
      std::lock_guard<std::mutex> lock(g_eventMutex);
      g_eventHandles[xeniaHandle] = ev;   // not in g_eventTyped: NtClearEvent refuses it, as LookupObject<XEvent>
    }
    xeniaEvent = ev;
  }
#endif
  {
    std::lock_guard<std::mutex> lk(g_xnMutex);
    XNotifyListener l;
    l.handle = xenia && xeniaHandle ? xeniaHandle : g_xnNextHandle++;
    l.waitEvent = xeniaEvent;
    l.areas = (uint64_t(r4) << 32) | r3;  // qwAreas: r3=low, r4=high (PPC64 reg pair)
    if (xenia) {
      // XamNotifyCreateListener(qword mask r3, max_version r4) (xam_notify.cc): the 64-bit mask is r3 alone (the
      // glue passes its low word; every area bit is there) and r4 is max_version, capped at 10. Astra batch-2 P3:
      // bits 32-63 of r3 are lost here (the import glue hands exports 32-bit registers); this title's masks are 0x01
      // and 0x2F, and the defined areas are bits 0-5. Not changed: it needs the glue to pass the full register.
      l.areas = r3;
      l.maxVersion = r4 > 10 ? 10 : r4;
    }
    handle = l.handle;
    g_xnListeners.push_back(std::move(l));
    if (xenia) {
      XNotifyListener& nl = g_xnListeners.back();
      if (!g_xnPrimedSystem && (nl.areas & 0x01u)) {
        g_xnPrimedSystem = true;
        // XN_SYS_UI with xam_state()->IsUIActive() (Astra batch-2 P2: the current state, not a constant 0)
        xnEnqueueTo(nl, 0x09u, g_xnUiActive ? 1u : 0u);
        xnEnqueueTo(nl, 0x0Au, 1u);          // XN_SYS_SIGNINCHANGED
        xnEnqueueTo(nl, 0x0Bu, 0u);          // XN_SYS_STORAGEDEVICESCHANGED
      }
      if (!g_xnPrimedLive && (nl.areas & 0x02u)) {
        g_xnPrimedLive = true;
        xnEnqueueTo(nl, 0x02000001u, 0x001510F1u);   // XN_LIVE_CONNECTIONCHANGED: logon disconnected
        xnEnqueueTo(nl, 0x02000003u, 0u);            // XN_LIVE_LINK_STATE_CHANGED
      }
      if (!g_xnPrimedXmp && (nl.areas & 0x20u)) {
        g_xnPrimedXmp = true;
        // XN_XMP_PLAYBACKCONTROLLERCHANGED with IsTitleInPlaybackControl() (Astra batch-2 P2: the XMP state, e.g. an
        // override the title set before its first XMP listener). Lock order: queue -> XMP state.
        xnEnqueueTo(nl, 0x0A000003u, xmpTitleInPlaybackControl() ? 1u : 0u);
      }
    }
  }
  static int s_logCount = 0;
  if (++s_logCount <= 6)
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[XAM] XamNotifyCreateListener(areas=0x%08X%08X) -> handle=0x%08X", r4, r3, handle);
  if (xenia) {   // step4/audit SR_XNOTIFY_XENIA witness (IMPORT_FIXES is a player-log milestone tag)
    size_t primed = 0;
    {
      std::lock_guard<std::mutex> lk(g_xnMutex);
      for (auto& e : g_xnListeners) if (e.handle == handle) { primed = e.queue.size(); break; }
    }
    static std::atomic<uint32_t> s_w{0};
    const uint32_t n = s_w.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16 || (n & (n - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[IMPORT_FIXES] XNOTIFY #%u XamNotifyCreateListener(areas=%08X, max_version=%u) -> %08X primed=%u lr=%08X",
        n, r3, r4, handle, (unsigned)primed, getCurrentGuestLR());
  }
  // SR_XNOTIFY_XENIA primes at creation (above); the delivery thread then only serves the forcing diagnostic.
  if (!xenia || xnForceBootUiOn()) xnEnsureBootDelivery();  // start delivering launch notifications now a listener exists
  return handle;
}

// XNotifyGetNext(hNotification, dwMsgFilter, pdwId, pParam): pop one matching
// notification from THAT listener's queue (per-listener, not a global one-shot).
static uint32_t stub_XNotifyGetNext(uint32_t r3, uint32_t r4, uint32_t r5,
                                    uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r7; (void)r8;
  uint32_t id = 0, param = 0;
  bool got = false;
  if (xnotifyXeniaOn()) {
    // Xenia (xam_notify.cc:56-65): the out-params are zeroed first; without an id pointer nothing is dequeued.
    if (r6 && isGuestVAInRange(r6)) writeGuestBE32(r6, 0);
    if (!r5 || !isGuestVAInRange(r5)) return 0;
    writeGuestBE32(r5, 0);
  }
  {
    std::lock_guard<std::mutex> lk(g_xnMutex);
    XNotifyListener* l = nullptr;
    for (auto& e : g_xnListeners) if (e.handle == r3 && !e.closed) { l = &e; break; }   // a closed handle finds nothing
    if (l) {
      auto it = l->queue.end();
      if (r4 != 0) {  // filtered fetch: first entry whose id == filter
        for (auto i = l->queue.begin(); i != l->queue.end(); ++i)
          if (i->first == r4) { it = i; break; }
      } else if (!l->queue.empty()) {
        it = l->queue.begin();
      }
      if (it != l->queue.end()) { id = it->first; param = it->second; l->queue.erase(it); got = true; }
#ifdef _WIN32
      // xnotifylistener.cc:66-68 / 83-85: the wait handle is reset when the last notification is taken
      if (got && l->queue.empty() && l->waitEvent) ResetEvent(static_cast<HANDLE>(l->waitEvent));
#endif
    }
  }
  if (!got) return 0;  // FALSE — no matching notification
  if (r5 && isGuestVAInRange(r5)) writeGuestBE32(r5, id);
  if (r6 && isGuestVAInRange(r6)) writeGuestBE32(r6, param);
  static std::atomic<int> s_log{0};
  if (s_log.fetch_add(1, std::memory_order_relaxed) < 16)
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[XAM] XNotifyGetNext(h=%08X filt=%08X) -> id=%u param=%u", r3, r4, id, param);
  if (xnotifyXeniaOn()) {   // step4/audit SR_XNOTIFY_XENIA witness: each delivered notification (bounded)
    static std::atomic<uint32_t> s_w{0};
    const uint32_t n = s_w.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 32 || (n & (n - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[IMPORT_FIXES] XNOTIFY #%u XNotifyGetNext(h=%08X, filter=%08X) -> id=%08X param=%08X lr=%08X", n, r3, r4,
        id, param, getCurrentGuestLR());
  }
  return 1;  // TRUE
}

// XamShowSigninUI(cPanes, dwFlags): the sign-in blade. The boot-flow arming path
// (Function_822AA878, invoked by the FE ActionScript fscommand DoBootFlowProfileLogin)
// sets DAT_8296eda0+0x100=0 then calls this to open the blade; the driver sub_822B5910
// then waits for the blade-close notification XN_SYS_UI(id=9,param=0) to run START.
// We can't render a real blade, so model the console emitting (9,0) on dismissal
// (instant close). Faithful platform delivery — the guest still made the decision to
// open it. (UnleashedRecomp does the same for its system-UI stubs.)
static uint32_t stub_XamShowSigninUI(uint32_t r3, uint32_t r4, uint32_t r5,
                                     uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[XAM] XamShowSigninUI(panes=%u) -> instant close, emit XN_SYS_UI(9,0)", r3);
  xnEnqueue(0x09u, 0x00u);  // system UI closed -> unblocks the armed boot flow
  return 0;  // ERROR_SUCCESS
}

// iter-355 (PLATFORM FIX, not forcing): the boot-flow (sub_822B5910) is a MULTI-STAGE
// system-UI sequence. It advances past sign-in (stub_XamShowSigninUI emits XN_SYS_UI
// (9,0)) but then STALLS on the NEXT blade it opens, because these three were
// REG_GENERIC_STUB and returned WITHOUT delivering the console's blade-close
// notification XN_SYS_UI(id=9,param=0). Model each as an instant console-close, exactly
// like the sign-in blade. The guest still decides to open each blade; we only implement
// the platform's close event (matches UnleashedRecomp's system-UI stubs).

// XamShowDeviceSelectorUI(user, contentType, contentFlags, ullTotalRequested[64b],
// pDeviceId, pOverlapped): the "select storage device" blade. Return a valid HDD device
// id + emit the close notification. (The u64 ullTotalRequested occupies one PPC reg
// pair, so pDeviceId lands in r8.)
static uint32_t stub_XamShowDeviceSelectorUI(uint32_t r3, uint32_t r4, uint32_t r5,
                                             uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r7;
  uint32_t pDeviceId = r8;
  if (pDeviceId && isGuestVAInRange(pDeviceId))
    writeGuestBE32(pDeviceId, 0x00000001);  // a valid attached HDD device id
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[XAM] XamShowDeviceSelectorUI(user=%u,pDev=%08X) -> device=1, close, emit XN_SYS_UI(9,0)",
    r3, pDeviceId);
  xnEnqueue(0x09u, 0x00u);
  return 0;
}

// ── iter-462 XAM storage, part A (PLATFORM FIX, SR_XAM_CONTENT default ON) ──────────
// Ported from Xenia (BSD-3): xam_ui.cc:142-161,514-571 (headless device selector),
// kernel_state.cc:1128-1170 (deferred overlapped), xam_content_device.cc. Spec and
// evidence: docs/CUSTOM_FRONTEND.md section 8, archive/v1_frontend/xam_storage/notes.md.
// The stub above read pDeviceId from r8 - on PPC64 a u64 argument takes ONE GPR, so r8 is
// the XOVERLAPPED (mgr+0x33C) - and returned 0, while DeviceSelectorTask_invokeXam
// (sub_82645D20) treats anything but 997 as failure: the storage library then raised the
// "No_Device_Warning" popup and the front end looped on "NO STORAGE DEVICE SELECTED".
// SR_XAM_CONTENT=0 restores the old stubs for A/B.
static bool xamContentEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_XAM_CONTENT");
    return !e || e[0] != '0';
  }();
  return v;
}

constexpr uint32_t kXamDeviceHdd = 1;
constexpr uint32_t kXErrIoPending = 997;
constexpr uint32_t kXErrInvalidParameter = 0x57;
constexpr uint32_t kXErrDeviceNotConnected = 0x48F;
constexpr uint32_t kXErrInsufficientBuffer = 0x7A;
static const char16_t kXamHddName[] = u"Hard Drive";

// XOVERLAPPED (0x1C): +0 InternalLow (result), +4 InternalHigh (length), +8 context,
// +0xC hEvent, +0x10 completion routine, +0x14 routine context, +0x18 extended error.
static void xoverlappedComplete(uint32_t ovl, uint32_t result, uint32_t extErr, uint32_t length) {
  if (!ovl || !isGuestVAInRange(ovl)) return;
  writeGuestBE32(ovl + 0x04, length);
  writeGuestBE32(ovl + 0x18, extErr);
  writeGuestBE32(ovl + 0x00, result);  // last: the guest polls [0] != 997
  const uint32_t hEvent = readGuestU32BE(ovl + 0x0C);
  const uint32_t routine = readGuestU32BE(ovl + 0x10);
  if (hEvent || routine) {
    static std::atomic<int> s_n{0};
    if (s_n.fetch_add(1, std::memory_order_relaxed) < 8)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[XAM] overlapped %08X has hEvent=%08X routine=%08X - not signalled (unused by this title)",
        ovl, hEvent, routine);
  }
}

// The console's system-UI blade opens (XN_SYS_UI 9,1) and closes (9,0) around the
// operation; Xenia's headless dispatch sends the close ~100 ms after completion. The
// timer thread touches only the host notification queue.
static void xnSystemUiCloseLater() {
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    xnEnqueue(0x09u, 0x00u);
  }).detach();
}

static uint32_t stub_XamShowDeviceSelectorUI_v2(uint32_t r3, uint32_t r4, uint32_t r5,
                                                uint32_t r6, uint32_t r7, uint32_t r8) {
  if (!xamContentEnabled()) return stub_XamShowDeviceSelectorUI(r3, r4, r5, r6, r7, r8);
  (void)r4; (void)r6;
  const uint32_t user = r3, flags = r5, pDeviceId = r7, ovl = r8;
  if (!ovl || !isGuestVAInRange(ovl)) return kXErrInvalidParameter;
  if ((user >= 4 && user != 0xFFu) || (flags & 0x83F00008u) || !pDeviceId ||
      !isGuestVAInRange(pDeviceId)) {
    writeGuestBE32(ovl + 0x18, kXErrInvalidParameter);
    return kXErrInvalidParameter;
  }
  writeGuestBE32(ovl + 0x00, kXErrIoPending);
  xnEnqueue(0x09u, 0x01u);                       // blade opens
  writeGuestBE32(pDeviceId, kXamDeviceHdd);      // headless: the first device, the HDD
  xoverlappedComplete(ovl, 0, 0, 0);
  xnSystemUiCloseLater();                        // blade closes
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[XAM] XamShowDeviceSelectorUI(user=%u flags=%08X pDev=%08X ovl=%08X) -> device=%u, "
    "overlapped complete, XN_SYS_UI(9,1) now + (9,0) in 100 ms, rc=997",
    user, flags, pDeviceId, ovl, kXamDeviceHdd);
  return kXErrIoPending;
}

// XamContentGetDeviceState(id r3, pOverlapped r4).
static uint32_t stub_XamContentGetDeviceState(uint32_t r3, uint32_t r4, uint32_t, uint32_t,
                                              uint32_t, uint32_t) {
  const bool known = r3 == kXamDeviceHdd;
  if (r4) {
    xoverlappedComplete(r4, known ? 0u : 0x65Bu, known ? 0u : kXErrDeviceNotConnected, 0);
    return kXErrIoPending;
  }
  return known ? 0u : kXErrDeviceNotConnected;
}

// XamContentGetDeviceData(id r3, XDEVICE_DATA* r4): 0x50 bytes - id, type, u64 total,
// u64 free, 28 UTF-16BE name characters. Space reported from the save root's volume.
static uint32_t stub_XamContentGetDeviceData(uint32_t r3, uint32_t r4, uint32_t, uint32_t,
                                             uint32_t, uint32_t) {
  if (r3 != kXamDeviceHdd) return kXErrDeviceNotConnected;
  if (!r4 || !isGuestVAInRange(r4)) return kXErrInvalidParameter;
  uint8_t* p = guestVAToHost(r4);
  std::memset(p, 0, 0x50);
  std::error_code ec;
  const auto sp = std::filesystem::space(std::filesystem::current_path(ec), ec);
  uint64_t total = ec ? 0x100000000ull : uint64_t(sp.capacity);
  uint64_t avail = ec ? 0x80000000ull : uint64_t(sp.available);
  // iter-463: with real saves on, report the volume that holds the content root.
  if (xamContentFsEnabled()) xamContentSpace(&total, &avail);
  auto be64 = [&](size_t off, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[off + i] = uint8_t(v >> (56 - 8 * i));
  };
  writeGuestBE32(r4 + 0x0, kXamDeviceHdd);
  writeGuestBE32(r4 + 0x4, 1u);  // XCONTENTDEVICETYPE_HDD
  be64(0x08, total);
  be64(0x10, avail);
  for (size_t i = 0; kXamHddName[i] && i < 27; ++i) {
    p[0x18 + 2 * i] = uint8_t(uint16_t(kXamHddName[i]) >> 8);
    p[0x18 + 2 * i + 1] = uint8_t(kXamHddName[i]);
  }
  return 0;
}

// XamContentGetDeviceName(id r3, wchar* r4, cch r5).
static uint32_t stub_XamContentGetDeviceName(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t,
                                             uint32_t, uint32_t) {
  if (r3 != kXamDeviceHdd) return kXErrDeviceNotConnected;
  const uint32_t len = uint32_t(sizeof(kXamHddName) / sizeof(kXamHddName[0])) - 1;
  if (r5 < len + 1) return kXErrInsufficientBuffer;
  if (!r4 || !isGuestVAInRange(r4)) return kXErrInvalidParameter;
  uint8_t* p = guestVAToHost(r4);
  for (uint32_t i = 0; i <= len; ++i) {
    const uint16_t c = i < len ? uint16_t(kXamHddName[i]) : 0;
    p[2 * i] = uint8_t(c >> 8);
    p[2 * i + 1] = uint8_t(c);
  }
  return 0;
}

// XamShowMessageBoxUIEx(...): a modal message box. Console delivers XN_SYS_UI(9,0) on
// dismissal; the boot-flow waits for it. Model instant close (default button).
static uint32_t stub_XamShowMessageBoxUIEx(uint32_t r3, uint32_t r4, uint32_t r5,
                                           uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r3; (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[XAM] XamShowMessageBoxUIEx -> instant close, emit XN_SYS_UI(9,0)");
  xnEnqueue(0x09u, 0x00u);
  return 0;
}

// XamShowDirtyDiscErrorUI(user): the "disc unreadable" blade. Console delivers
// XN_SYS_UI(9,0) on close. Model instant close (our disc always reads fine).
static uint32_t stub_XamShowDirtyDiscErrorUI(uint32_t r3, uint32_t r4, uint32_t r5,
                                             uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[XAM] XamShowDirtyDiscErrorUI(user=%u) -> instant close, emit XN_SYS_UI(9,0)", r3);
  xnEnqueue(0x09u, 0x00u);
  return 0;
}

// XamUserGetXUID: r3=dwUserIndex, r4=pXuid (8-byte output)
// Returns 0 on success, 1 on error.
static uint32_t stub_XamUserGetXUID(uint32_t r3, uint32_t r4, uint32_t r5,
                                    uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r5; (void)r6; (void)r7; (void)r8;
  if (r3 != 0) return 1;
  if (r4 && isGuestVAInRange(r4)) {
    writeGuestBE32(r4 + 0, 0x00090000); // high word of fake XUID
    writeGuestBE32(r4 + 4, 0x00000001); // low word
  }
  static int s_logCount = 0;
  if (s_logCount++ < 3)
    SR_LOG_BOOT(LogSeverity::Info, "[XAM] XamUserGetXUID(user=%u) -> XUID=0x0009000000000001", r3);
  return 0;
}

// ExGetXConfigSetting: r3=category, r4=setting, r5=buffer, r6=bufSize, r7=requiredSize
static uint32_t stub_ExGetXConfigSetting(uint32_t r3, uint32_t r4, uint32_t r5,
                                         uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r8;
  uint32_t category = r3;
  uint32_t setting  = r4;
  uint32_t bufVA    = r5;
  uint32_t bufSize  = r6;
  uint32_t reqVA    = r7;

  uint32_t value = 0;
  uint32_t valueSize = 4;
  bool known = false;

  if (category == 0x03) { // XCONFIG_USER_SETTINGS
    known = true;
    switch (setting) {
      case 0x01: value = 0;          break; // TimeZoneBias (UTC)
      case 0x02: value = 0x55544300; break; // TimeZoneStdName "UTC\0"
      case 0x03: value = 0x55544300; break; // TimeZoneDltName "UTC\0"
      case 0x04: value = 0;          break; // TimeZoneStdDate
      case 0x05: value = 0;          break; // TimeZoneDltDate
      case 0x06: value = 0;          break; // TimeZoneStdBias
      case 0x07: value = 0;          break; // TimeZoneDltBias
      case 0x09: value = 1;          break; // Language (1=English)
      case 0x0A:                             // VideoFlags
        value = 0x00010000  // XC_VIDEO_FLAGS_WIDESCREEN
              | 0x00020000  // XC_VIDEO_FLAGS_HDTV_720p
              | 0x00080000  // XC_VIDEO_FLAGS_HDTV_480p
              | 0x00400000; // XC_VIDEO_FLAGS_60Hz
        break;
      case 0x0B: value = 0x00010000; break; // AudioFlags (stereo)
      case 0x0C: value = 0;          break; // RetailFlags
      default:   value = 0;          break;
    }
  } else if (category == 0x02) { // XCONFIG_SECURED_SETTINGS
    known = true;
    switch (setting) {
      case 0x01: value = 0x00000001; break; // AVRegion (NTSC-U)
      case 0x02: value = 0x000000FF; break; // GameRegion (all)
      case 0x03: value = 0x00000001; break; // DVDRegion (region 1)
      case 0x04: value = 0;          break; // ResetKey
      case 0x05: value = 0;          break; // SystemFlags
      default:   value = 0;          break;
    }
  }

  if (bufVA && bufSize >= 4 && isGuestVAInRange(bufVA)) {
    writeGuestBE32(bufVA, value);
  }
  if (reqVA && isGuestVAInRange(reqVA)) {
    writeGuestBE32(reqVA, valueSize);
  }

  static std::atomic<int> s_logCount{0};
  if (s_logCount.fetch_add(1, std::memory_order_relaxed) < 20) {
    SR_LOG_KERNEL_IMPORT("ExGetXConfigSetting(cat=%u set=%u buf=0x%08X sz=%u) -> val=0x%08X%s",
      category, setting, bufVA, bufSize, value, known ? "" : " (unknown)");
  }
  return kStatusSuccess;
}

// RtlTimeToTimeFields: r3=PLARGE_INTEGER time, r4=PTIME_FIELDS output
static uint32_t stub_RtlTimeToTimeFields(uint32_t r3, uint32_t r4, uint32_t r5,
                                         uint32_t r6, uint32_t r7, uint32_t r8) {
  (void)r5; (void)r6; (void)r7; (void)r8;
  if (!r3 || !isGuestVAInRange(r3) || !r4 || !isGuestVAInRange(r4)) {
    STUB_LOG("RtlTimeToTimeFields", r3, r4, r5, r6, r7, r8, kStatusSuccess);
    return kStatusSuccess;
  }
  uint32_t hi = readGuestU32BE(r3);
  uint32_t lo = readGuestU32BE(r3 + 4);
  uint64_t ft = (uint64_t(hi) << 32) | lo;

#ifdef _WIN32
  FILETIME filetime;
  filetime.dwHighDateTime = static_cast<DWORD>(ft >> 32);
  filetime.dwLowDateTime  = static_cast<DWORD>(ft & 0xFFFFFFFF);
  SYSTEMTIME st;
  if (!FileTimeToSystemTime(&filetime, &st)) {
    GetSystemTime(&st);
  }
  uint8_t* p = guestVAToHost(r4);
  auto writeBE16 = [&](int off, uint16_t v) {
    p[off]     = uint8_t(v >> 8);
    p[off + 1] = uint8_t(v & 0xFF);
  };
  writeBE16(0,  st.wYear);
  writeBE16(2,  st.wMonth);
  writeBE16(4,  st.wDay);
  writeBE16(6,  st.wHour);
  writeBE16(8,  st.wMinute);
  writeBE16(10, st.wSecond);
  writeBE16(12, st.wMilliseconds);
  writeBE16(14, st.wDayOfWeek);
#else
  uint8_t* p = guestVAToHost(r4);
  std::memset(p, 0, 16);
#endif
  STUB_LOG("RtlTimeToTimeFields", r3, r4, r5, r6, r7, r8, kStatusSuccess);
  return kStatusSuccess;
}

#if SR_ORACLE && defined(_WIN32)
// step 6 G6a DIAG (dev; SR_DIAG_WAIT=0 turns both off). The wrappers wrap whatever each export is registered to at the
// end of registerStubs.
// - [DIAG_WAIT]: per guest thread, per 120 swaps, the host ms each wait export blocked on one wait object - the KEVENT /
//   KSEMAPHORE VA, an Nt handle, or a multiple wait's first object - with the class of that object's last signaller:
//   isr / swap (the CP thread inside a guest ISR or the swap's present, kernel/gpu_submit.hpp diagCpCallback) or guest
//   (a guest thread; its tid).
// - [DIAG_WAKE]: per wait object, per 120 swaps, the latency from the signal that woke a wait to the wait's return
//   (our signal -> wake path: the host event and the scheduler). A signal stamps its object with the QPC; a wait that
//   returns takes the latest stamp of its object(s) inside [t0, t1] as the waking signal.
namespace diagwait {
enum : int { kKeWait, kNtWait, kKeWaitMulti, kKeSet, kKeRelSem, kNtRelSem, kNtSetEvt, kExports };
std::atomic<ExportFn> g_orig[kExports];
constexpr uint32_t kSigSlots = 1024;
// One record per slot in one 64-bit word, so a reader never pairs one object with another's signaller: obj (32 bits),
// cls (3 bits), tid (29 bits; Windows thread ids are far below 2^29).
struct Signaller { std::atomic<uint64_t> rec{0}; };
Signaller g_sig[kSigSlots];
uint64_t sigRecord(uint32_t obj, uint32_t cls, uint32_t tid) {
  return uint64_t(obj) << 32 | uint64_t(cls & 7u) << 29 | (tid & 0x1FFFFFFFu);
}
uint32_t sigSlot(uint32_t obj) { return (obj * 0x9E3779B1u) >> 22; }
const char* className(uint32_t c) { return c == 4 ? "isr" : c == 3 ? "swap" : "guest"; }

// [DIAG_WAKE]'s stamp: obj and the QPC's low 32 bits in one 64-bit word (one store, one load).
std::atomic<uint64_t> g_wakeStamp[kSigSlots];
uint64_t wakeStamp(uint32_t obj, uint64_t qpc) { return uint64_t(obj) << 32 | uint32_t(qpc); }
// The ticks from the stamped signal to t1, or -1 when the stamp is another object's or not inside [t0, t1]. Waits of
// 2^31 ticks or more are skipped (the low 32 bits could alias).
int64_t wakeTicks(uint64_t stamp, uint32_t obj, uint64_t t0, uint64_t t1) {
  if (uint32_t(stamp >> 32) != obj || t1 < t0 || t1 - t0 >= (1ull << 31)) return -1;
  const uint32_t intoWait = uint32_t(stamp) - uint32_t(t0);
  const uint32_t span = uint32_t(t1 - t0);
  if (intoWait > span) return -1;
  return int64_t(span - intoWait);
}
constexpr int kWakeObjs = 64;
constexpr int kWakeBuckets = 20;   // bucket b: wakes of [2^b - 1, 2^(b+1) - 1) us; the last one is open-ended
struct WakeHist {
  std::atomic<uint32_t> obj{0}, n{0}, maxUs{0};
  std::atomic<uint64_t> sumUs{0};
  std::atomic<uint32_t> b[kWakeBuckets]{};
};
WakeHist g_wake[kWakeObjs];
int wakeBucket(uint32_t us) {
  const uint64_t v = uint64_t(us) + 1u;
  int b = 0;
  while (b + 1 < kWakeBuckets && (v >> (b + 1))) ++b;
  return b;
}
void noteWake(uint32_t obj, uint32_t us) {
  for (int i = 0; i < kWakeObjs; ++i) {   // linear probe from the object's slot; a full table drops the sample
    WakeHist& h = g_wake[(sigSlot(obj) + uint32_t(i)) % kWakeObjs];
    uint32_t cur = h.obj.load(std::memory_order_relaxed);
    if (cur == 0 && h.obj.compare_exchange_strong(cur, obj, std::memory_order_relaxed)) cur = obj;
    if (cur != obj) continue;
    h.n.fetch_add(1, std::memory_order_relaxed);
    h.sumUs.fetch_add(us, std::memory_order_relaxed);
    uint32_t mx = h.maxUs.load(std::memory_order_relaxed);
    while (us > mx && !h.maxUs.compare_exchange_weak(mx, us, std::memory_order_relaxed)) {
    }
    h.b[wakeBucket(us)].fetch_add(1, std::memory_order_relaxed);
    return;
  }
}
// The bucket that holds the q-th quantile's sample, as its upper bound in us (never above the window's max).
uint32_t wakeQuantileUs(const uint32_t (&b)[kWakeBuckets], uint32_t n, double q, uint32_t maxUs) {
  const uint32_t want = uint32_t(double(n) * q);
  uint32_t seen = 0;
  for (int k = 0; k + 1 < kWakeBuckets; ++k) {
    seen += b[k];
    if (seen > want) return (std::min)((2u << k) - 1u, maxUs);
  }
  return maxUs;
}
// The first thread to see a new 120-swap window prints the last one's top 6 objects by wakes and clears the table
// (a wake noted during the clear may land in either window); 4000 lines a run at most.
std::atomic<uint64_t> g_wakeWindow{~0ull};
void flushWake(uint64_t window) {
  uint64_t prev = g_wakeWindow.load(std::memory_order_relaxed);
  if (prev == window || !g_wakeWindow.compare_exchange_strong(prev, window, std::memory_order_relaxed)) return;
  if (prev == ~0ull) return;
  struct Row { uint32_t obj, n, maxUs; uint64_t sumUs; uint32_t b[kWakeBuckets]; };
  Row rows[kWakeObjs];
  int m = 0;
  for (WakeHist& h : g_wake) {
    const uint32_t o = h.obj.load(std::memory_order_relaxed);
    if (!o) continue;
    Row r{o, h.n.exchange(0, std::memory_order_relaxed), h.maxUs.exchange(0, std::memory_order_relaxed),
          h.sumUs.exchange(0, std::memory_order_relaxed), {}};
    for (int k = 0; k < kWakeBuckets; ++k) r.b[k] = h.b[k].exchange(0, std::memory_order_relaxed);
    if (r.n) rows[m++] = r;
  }
  std::sort(rows, rows + m, [](const Row& a, const Row& b) { return a.n > b.n; });
  static std::atomic<uint32_t> s_lines{0};
  for (int i = 0; i < m && i < 6; ++i) {
    if (s_lines.fetch_add(1, std::memory_order_relaxed) >= 4000) break;
    const Row& r = rows[i];
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[DIAG_WAKE] swaps=%llu obj=%08X wakes=%u mean=%.1f p50<=%u p95<=%u max=%u us (%.3f ms/frame)",
                (unsigned long long)(prev * 120u), r.obj, r.n, double(r.sumUs) / double(r.n),
                wakeQuantileUs(r.b, r.n, 0.5, r.maxUs), wakeQuantileUs(r.b, r.n, 0.95, r.maxUs), r.maxUs,
                double(r.sumUs) / 120e3);
  }
}

struct Entry { uint32_t obj, waits; double ms; };
struct ThreadWaits { std::vector<Entry> e; uint64_t window = ~0ull; };
thread_local ThreadWaits t_waits;
// The window's top 3 objects that blocked at least 60 ms (0.5 ms a frame); 4000 lines a run at most.
void flush(ThreadWaits& w) {
  std::sort(w.e.begin(), w.e.end(), [](const Entry& a, const Entry& b) { return a.ms > b.ms; });
  static std::atomic<uint32_t> s_lines{0};
  for (size_t i = 0; i < w.e.size() && i < 3 && w.e[i].ms >= 60.0; ++i) {
    if (s_lines.fetch_add(1, std::memory_order_relaxed) >= 4000) break;
    const uint64_t rec = g_sig[sigSlot(w.e[i].obj)].rec.load(std::memory_order_relaxed);
    const bool known = uint32_t(rec >> 32) == w.e[i].obj;
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[DIAG_WAIT] swaps=%llu tid=%lu handle=%u obj=%08X waits=%u blocked=%.1f ms (%.2f ms/frame) "
                "last_signaller=%s tid=%u",
                (unsigned long long)(w.window * 120u), GetCurrentThreadId(), sr::kernel::getCurrentThreadHandle(),
                w.e[i].obj, w.e[i].waits, w.e[i].ms, w.e[i].ms / 120.0,
                known ? className(uint32_t(rec >> 29) & 7u) : "unknown", known ? uint32_t(rec) & 0x1FFFFFFFu : 0u);
  }
  w.e.clear();
}
void noteWait(uint32_t obj, double ms) {
  ThreadWaits& w = t_waits;
  const uint64_t window = getVdSwapCount() / 120u;
  if (window != w.window) {
    if (w.window != ~0ull) flush(w);
    w.window = window;
    flushWake(window);
  }
  for (Entry& x : w.e)
    if (x.obj == obj) {
      x.ms += ms;
      ++x.waits;
      return;
    }
  if (w.e.size() < 64) w.e.push_back({obj, 1u, ms});
}
double qpcPerUs() {
  static const double v = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return double(f.QuadPart) / 1e6;
  }();
  return v;
}
template <int kIdx>
uint32_t timedWait(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8) {
  LARGE_INTEGER t0, t1;
  QueryPerformanceCounter(&t0);
  const uint32_t rc = g_orig[kIdx].load(std::memory_order_relaxed)(r3, r4, r5, r6, r7, r8);
  QueryPerformanceCounter(&t1);
  const uint64_t a = uint64_t(t0.QuadPart), z = uint64_t(t1.QuadPart);
  // KeWaitForMultipleObjects(r3 count, r4 PVOID Object[]): the first object names the wait; the wake is the latest
  // stamped signal among its first 8 objects (for a wait-any that undercounts only when two were set during it).
  uint32_t obj = r3;
  int64_t wake = -1;
  if (kIdx == kKeWaitMulti) {
    obj = r4 && isGuestVAInRange(r4) ? readGuestU32BE(r4) : 0u;
    for (uint32_t i = 0; i < r3 && i < 8u && r4 && isGuestVAInRange(r4 + 4u * i); ++i) {
      const uint32_t o = readGuestU32BE(r4 + 4u * i);
      const int64_t t = wakeTicks(g_wakeStamp[sigSlot(o)].load(std::memory_order_relaxed), o, a, z);
      if (t >= 0 && (wake < 0 || t < wake)) wake = t;
    }
  } else {
    wake = wakeTicks(g_wakeStamp[sigSlot(r3)].load(std::memory_order_relaxed), r3, a, z);
  }
  if (wake >= 0 && obj) noteWake(obj, uint32_t((std::min)(double(wake) / qpcPerUs(), 4.0e9)));
  noteWait(obj, double(z - a) / (qpcPerUs() * 1000.0));
  return rc;
}
template <int kIdx>
uint32_t signalled(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8) {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  g_sig[sigSlot(r3)].rec.store(sigRecord(r3, sr::kernel::diagCpCallback(), GetCurrentThreadId()),
                               std::memory_order_relaxed);
  g_wakeStamp[sigSlot(r3)].store(wakeStamp(r3, uint64_t(t.QuadPart)), std::memory_order_relaxed);
  return g_orig[kIdx].load(std::memory_order_relaxed)(r3, r4, r5, r6, r7, r8);
}
void wrap(ExportRegistry& reg, const char* name, int idx, ExportFn wrapper) {
  const ExportFn cur = reg.getExport(name);
  if (!cur) return;
  g_orig[idx].store(cur, std::memory_order_relaxed);
  reg.registerExport(name, wrapper);
}
void install(ExportRegistry& reg) {
  static const bool on = [] {
    const char* e = std::getenv("SR_DIAG_WAIT");
    return !e || e[0] != '0';
  }();
  if (!on) return;
  wrap(reg, "KeWaitForSingleObject", kKeWait, &timedWait<kKeWait>);
  wrap(reg, "NtWaitForSingleObjectEx", kNtWait, &timedWait<kNtWait>);
  wrap(reg, "KeWaitForMultipleObjects", kKeWaitMulti, &timedWait<kKeWaitMulti>);
  wrap(reg, "KeSetEvent", kKeSet, &signalled<kKeSet>);
  wrap(reg, "KeReleaseSemaphore", kKeRelSem, &signalled<kKeRelSem>);
  wrap(reg, "NtReleaseSemaphore", kNtRelSem, &signalled<kNtRelSem>);
  wrap(reg, "NtSetEvent", kNtSetEvt, &signalled<kNtSetEvt>);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[DIAG_WAIT] on: KeWaitForSingleObject / NtWaitForSingleObjectEx / "
              "KeWaitForMultipleObjects timed; KeSetEvent / Ke-, NtReleaseSemaphore / NtSetEvent name the signaller "
              "and stamp [DIAG_WAKE] (SR_DIAG_WAIT=0 off)");
}
}  // namespace diagwait
#endif

// step 6 G6a test seams (tools/difftest/diag_g6a_test.cpp): the [DIAG_WAIT] record and the [DIAG_WAKE] arithmetic.
#if SR_ORACLE && defined(_WIN32)
uint64_t srTestDiagSigRecord(uint32_t obj, uint32_t cls, uint32_t tid) { return diagwait::sigRecord(obj, cls, tid); }
int64_t srTestDiagWakeTicks(uint32_t stampObj, uint64_t stampQpc, uint32_t obj, uint64_t t0, uint64_t t1) {
  return diagwait::wakeTicks(diagwait::wakeStamp(stampObj, stampQpc), obj, t0, t1);
}
int srTestDiagWakeBucket(uint32_t us) { return diagwait::wakeBucket(us); }
#endif

void registerStubs() {
#ifdef _WIN32
  // Disable all FPU exceptions at startup to prevent crashes from recompiled guest code
  unsigned int unused = 0;
  (void)_controlfp_s(&unused, _MCW_EM, _MCW_EM);
#endif
  auto& reg = ExportRegistry::instance();
  reg.registerExport("NtAllocateVirtualMemory", stub_NtAllocateVirtualMemory);
  reg.registerExport("KeQueryPerformanceCounter", stub_KeQueryPerformanceCounter);
  reg.registerExport("KeQueryPerformanceFrequency", stub_KeQueryPerformanceFrequency);
  reg.registerExport("KeQuerySystemTime", stub_KeQuerySystemTime);
  reg.registerExport("KeInitializeEvent", stub_KeInitializeEvent);
  reg.registerExport("KeSetEvent", stub_KeSetEvent);
  reg.registerExport("KeResetEvent", stub_KeResetEvent);
  reg.registerExport("KeWaitForSingleObject", stub_KeWaitForSingleObject);
  reg.registerExport("RtlAllocateHeap", stub_RtlAllocateHeap);
  reg.registerExport("DbgPrint", stub_DbgPrint);
  reg.registerExport("KeDelayExecutionThread", stub_KeDelayExecutionThread);
  reg.registerExport("NtClose", stub_NtClose);
  reg.registerExport("NtCreateFile", stub_NtCreateFile);
  reg.registerExport("NtOpenFile", stub_NtOpenFile);
  reg.registerExport("NtReadFile", stub_NtReadFile);

  // Sync object stubs: events and semaphores backed by real Win32 objects
  reg.registerExport("NtCreateEvent", stub_NtCreateEvent);
  REG_GENERIC_STUB("NtClearEvent");
#ifdef _WIN32
  // step4/audit SR_NT_EVENT_TYPE (baked default ON): NtClearEvent resets the event like Xenia's xeNtClearEvent
  // (xboxkrnl_threading.cc:717-734): LookupObject<XEvent> - an event handle -> Reset, STATUS_SUCCESS; anything else
  // (unknown, closed, a semaphore or thread handle) -> STATUS_INVALID_HANDLE (Astra P2 2). The table lock is held
  // through the reset, so a concurrent NtClose cannot close or recycle the HANDLE under it (Astra P2 1; Xenia holds
  // an object_ref for the call). Off: the generic stub above stays registered.
  if (ntEventTypeOn())
    reg.registerExport("NtClearEvent", +[](uint32_t r3, uint32_t, uint32_t, uint32_t, uint32_t,
                                           uint32_t) -> uint32_t {
      uint32_t rc = 0xC0000008u;  // STATUS_INVALID_HANDLE
      {
        std::lock_guard<std::mutex> lock(g_eventMutex);
        auto it = g_eventHandles.find(r3);
        if (it != g_eventHandles.end() && it->second && g_eventTyped.count(r3))
          rc = ResetEvent(it->second) ? kStatusSuccess : 0xC0000008u;
      }
      static std::atomic<uint32_t> s_n{0};
      const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 16 || (n & (n - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info, "[IMPORT_FIXES] NT_EVENT #%u NtClearEvent handle=%08X -> %08X lr=%08X",
                    n, r3, rc, sr::kernel::getCurrentGuestLR());
      return rc;
    });
#endif
  reg.registerExport("NtCreateSemaphore", stub_NtCreateSemaphore);
  reg.registerExport("NtReleaseSemaphore", stub_NtReleaseSemaphore);
  reg.registerExport("NtDuplicateObject", stub_NtDuplicateObject);
  reg.registerExport("NtWaitForSingleObjectEx", stub_NtWaitForSingleObjectEx);
  reg.registerExport("NtQueryInformationFile", stub_NtQueryInformationFile);
  reg.registerExport("NtQueryDirectoryFile", stub_NtQueryDirectoryFile);
  REG_GENERIC_STUB("NtQueryVolumeInformationFile");
  REG_GENERIC_STUB("NtQueryFullAttributesFile");
  reg.registerExport("NtSetInformationFile", stub_NtSetInformationFile);
  REG_GENERIC_STUB("NtFlushBuffersFile");
  REG_GENERIC_STUB("NtWriteFile");
  reg.registerExport("NtFreeVirtualMemory", stub_NtFreeVirtualMemory);
  REG_GENERIC_STUB("NtQueryVirtualMemory");

  // Ke/Kf - spinlocks, semaphores, critical regions
  reg.registerExport("KeInitializeSemaphore", stub_KeInitializeSemaphore);
  reg.registerExport("KeReleaseSemaphore", stub_KeReleaseSemaphore);
  REG_GENERIC_STUB("KeWaitForMultipleObjects");
#ifdef _WIN32
  // iter-463 SR_KE_WAITMULTI (default ON): the real wait-any / wait-all (stub_KeWaitForMultipleObjects).
  if (keWaitMultiOn()) reg.registerExport("KeWaitForMultipleObjects", stub_KeWaitForMultipleObjects);
#endif
  REG_GENERIC_STUB("KeEnterCriticalRegion");
  REG_GENERIC_STUB("KeLeaveCriticalRegion");
  // iter-463 PLATFORM FIX (SR_KE_SPINLOCK + SR_KPCR, default ON): Xenia's
  // spinlocks (CAS 0 -> caller r13) and KPCR IRQL, plus KeSetAffinityThread
  // (SR_KPCR). Gates off = the exact REG_GENERIC_STUB body (kernel/kpcr.cpp).
  registerKpcrExports(reg);

  // Ex/Rtl/Ob
  reg.registerExport("ExCreateThread", stub_ExCreateThread);
  reg.registerExport("ExGetXConfigSetting", stub_ExGetXConfigSetting);
  REG_GENERIC_STUB("ExRegisterTitleTerminateNotification");
  reg.registerExport("ExTerminateThread", stub_ExTerminateThread);
  reg.registerExport("RtlInitializeCriticalSection", stub_RtlInitializeCriticalSection);
  reg.registerExport("RtlInitializeCriticalSectionAndSpinCount", stub_RtlInitializeCriticalSection);
  reg.registerExport("RtlEnterCriticalSection", stub_RtlEnterCriticalSection);
  reg.registerExport("RtlLeaveCriticalSection", stub_RtlLeaveCriticalSection);
  reg.registerExport("RtlTryEnterCriticalSection", stub_RtlTryEnterCriticalSection);
  reg.registerExport("RtlInitAnsiString", stub_RtlInitAnsiString);
  reg.registerExport("RtlImageXexHeaderField", stub_RtlImageXexHeaderField);
  // iter-462 PLATFORM FIX (SR_OB_KTHREAD, default ON): the console contract -
  // *out = the thread's guest KTHREAD (kernel/kthread_objects.hpp). The generic
  // stub returned success WITHOUT writing *out, so GetExitCodeThread
  // (sub_82469E98) read a stale stack slot as the KTHREAD and reported live
  // EAThreadPool workers as exited -> FixThreads cloned them onto one ThreadInfo
  // (archive/v1_eapool/). =0 keeps the exact REG_GENERIC_STUB body below.
  reg.registerExport("ObReferenceObjectByHandle",
                     +[](uint32_t r3, uint32_t r4, uint32_t r5, uint32_t, uint32_t,
                         uint32_t) -> uint32_t {
    if (obKThreadEnabled()) return obReferenceObjectByHandle(r3, r4, r5);
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (stubCensusOn() && (n <= 3 || (n & (n - 1)) == 0))
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[STUB_CENSUS] ObReferenceObjectByHandle #%u lr=%08X r3=%08X r4=%08X r5=%08X",
                  n, sr::kernel::getCurrentGuestLR(), r3, r4, r5);
    return kStatusSuccess;
  });
  // No refcount: KTHREAD blocks are never freed, so dereference stays a no-op.
  REG_GENERIC_STUB("ObDereferenceObject");
  REG_GENERIC_STUB("ObCreateSymbolicLink");
  REG_GENERIC_STUB("ObDeleteSymbolicLink");

  // Mm, NtYield, KiApc
  reg.registerExport("MmAllocatePhysicalMemoryEx", stub_MmAllocatePhysicalMemoryEx);
  // iter-458 H-FREE: gate OFF = exactly the former generic stub (census log,
  // success, nothing freed). Gate ON = the block returns to the allocator.
  reg.registerExport("MmFreePhysicalMemory", +[](uint32_t r3, uint32_t r4, uint32_t r5,
                                                 uint32_t, uint32_t, uint32_t) -> uint32_t {
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (stubCensusOn() && (n <= 3 || (n & (n - 1)) == 0))
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[STUB_CENSUS] MmFreePhysicalMemory #%u lr=%08X r3=%08X r4=%08X r5=%08X",
                  n, sr::kernel::getCurrentGuestLR(), r3, r4, r5);
    sr::kernel::physalloc::serviceFree(r4);   // no-op with SR_PHYS_FREE off
    // iter-458 H-LIFO (Astra 058, SR_PHYS_LIFO_FREE=1, default OFF): reclaim the
    // freed block only when it is the most recent live bump grant; unknown,
    // duplicate and non-top frees are rejected and named. The ownership
    // registry follows: a reclaimed grant is gone, a rejected non-top free is
    // RELEASED-BUT-RETAINED (the guest let go; our allocator kept the pages).
    // With only SR_PHYS_UNIQ_CENSUS=1 every free is a release the census records.
    // Astra 059: reclaimed / not-top registry updates happen INSIDE
    // guestPhysLifoFree under the allocator lock; only reporting follows here.
    // Unknown (heap-routed) and gate-OFF releases name VAs no allocator reuses.
    {
      const size_t cursorBefore = sr::kernel::guestPhysNextForTest();
      const sr::kernel::PhysLifoFree lr = sr::kernel::guestPhysLifoFree(r4);
      sr::kernel::physLifoPostFreeHook(r4);   // test seam; no-op in the game
      if (lr == sr::kernel::PhysLifoFree::kReclaimed)
        sr::kernel::physwin::uniqLifetimeReport("reclaimed");
      else if (lr == sr::kernel::PhysLifoFree::kNotTop)
        sr::kernel::physwin::uniqLifetimeReport("released-retained");
      else if ((lr == sr::kernel::PhysLifoFree::kUnknown ||
                lr == sr::kernel::PhysLifoFree::kNotEnabled) &&
               (sr::kernel::physwin::uniqCensusEnabled() || sr::kernel::txc::enabled() ||
                sr::kernel::physwin::gpuOwnerWindowEnabled()))
        // iter-460: also for the GPU window choice (physwin::gpuWindow ignores
        // released grants): the game's 299 MB probe is freed 9 ms after its
        // allocation and never touched again on the console (Xenia oracle,
        // docs/astra/058); our allocator keeps its pages, the registry no
        // longer lets it claim them.
        sr::kernel::physwin::markPhysGrantReleased(r4);
      if (lr != sr::kernel::PhysLifoFree::kNotEnabled) {
        static std::atomic<uint32_t> s_lf{0};
        const uint32_t k = s_lf.fetch_add(1, std::memory_order_relaxed) + 1;
        const size_t cursorAfter = sr::kernel::guestPhysNextForTest();
        if (k <= 32 || (k & (k - 1)) == 0 || cursorBefore >= cursorAfter + 0x1000000u)
          SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[PHYS_LIFO] free #%u va=%08X -> %s | physNext 0x%zX -> 0x%zX | retained 0x%zX in %u "
              "extents (reuse=%d) lr=%08X", k, r4,
              sr::kernel::physLifoFreeName(lr), cursorBefore, cursorAfter,
              sr::kernel::physLifoRetainedBytes(), sr::kernel::physLifoRetainedExtents(),
              sr::kernel::physLifoReuseEnabled() ? 1 : 0, sr::kernel::getCurrentGuestLR());
      }
    }
    return kStatusSuccess;
  });
  REG_GENERIC_STUB("MmGetPhysicalAddress");
  // iter-463 SR_MM_PHYSADDR (kernel/mm_phys.hpp): overrides the line above only when its gate is on.
  sr::kernel::registerMmPhysExport(reg);
  REG_GENERIC_STUB("MmQueryAddressProtect");
  reg.registerExport("MmQueryAllocationSize", stub_MmQueryAllocationSize);  // iter-395 real impl
  REG_GENERIC_STUB("MmSetAddressProtect");
  reg.registerExport("NtYieldExecution", stub_NtYieldExecution);
  REG_GENERIC_STUB("KiApcNormalRoutineNop");

  // Ke/Ex/Nt not yet registered
  REG_GENERIC_STUB("DbgBreakPoint");
  REG_GENERIC_STUB("HalReturnToFirmware");
  reg.registerExport("KeBugCheck", stub_KeBugCheck);
  reg.registerExport("KeBugCheckEx", stub_KeBugCheck);
  reg.registerExport("KeEnableFpuExceptions", stub_KeEnableFpuExceptions);
  reg.registerExport("KeGetCurrentProcessType", stub_KeGetCurrentProcessType);
  REG_GENERIC_STUB("KeLockL2");
  // iter-463 PLATFORM FIX (SR_KE_RESUME_THREAD, kernel/kthread_objects.hpp; default ON since the iter-463 rolls, was OFF until
  // rolled): KeResumeThread(KTHREAD*) as Xenia (xboxkrnl_threading.cc KeResumeThread_entry).
  // The XAudio lib creates its worker 0x82117810 CREATE_SUSPENDED (flags 0x10000001) and resumes
  // it with KeResumeThread from AudioBuildSpeakerConfig (lr 0x82117C88); the generic stub left
  // it suspended forever, so XAudioServiceSync's KeWaitForMultipleObjects never completed
  // (archive/v1_audio/rolls/a_all_1). Gate off: the generic stub (returns 0, resumes nothing).
  reg.registerExport("KeResumeThread",
    +[](uint32_t r3, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) -> uint32_t {
      if (!keResumeThreadEnabled()) return 0;
      const long prev = kthreadResumeHost(r3);
      static std::atomic<uint32_t> s_n{0};
      if (s_n.fetch_add(1, std::memory_order_relaxed) < 64)
        SR_LOG_KERNEL_IMPORT("[KE_RESUME] kthread=0x%08X prev=%ld lr=%08X -> %s", r3, prev,
                             sr::kernel::getCurrentGuestPC(),
                             prev < 0 ? "INVALID_HANDLE" : "SUCCESS");
      return prev < 0 ? 0xC0000008u : kStatusSuccess;
    });
  // KeSetAffinityThread: registerKpcrExports (above).
  REG_GENERIC_STUB("KeSetBasePriorityThread");
  reg.registerExport("KeTlsAlloc", stub_KeTlsAlloc);
  REG_GENERIC_STUB("KeTlsFree");
  reg.registerExport("KeTlsGetValue", stub_KeTlsGetValue);
  reg.registerExport("KeTlsSetValue", stub_KeTlsSetValue);
  REG_GENERIC_STUB("KeUnlockL2");
  reg.registerExport("NtQueueApcThread", stub_NtQueueApcThread);
  // iter-357j PLATFORM FIX: real suspend/resume — the XMV player's
  // creator/worker handshake uses CREATE_SUSPENDED + NtResumeThread.
  reg.registerExport("NtResumeThread",
    +[](uint32_t r3, uint32_t r4, uint32_t, uint32_t, uint32_t,
        uint32_t) -> uint32_t {
      HANDLE h = nullptr;
      {
        std::lock_guard<std::mutex> lock(g_eventMutex);
        auto it = g_eventHandles.find(r3);
        if (it != g_eventHandles.end()) h = it->second;
      }
      DWORD prev = h ? ResumeThread(h) : (DWORD)-1;
      SR_LOG_KERNEL_IMPORT("[NtResumeThread] handle=%u host=%p prev=%ld", r3,
                           (void*)h, (long)prev);
      if (r4 && prev != (DWORD)-1) writeGuestBE32(r4, prev);
      return h ? kStatusSuccess : 0xC0000008u;  // STATUS_INVALID_HANDLE
    });
  reg.registerExport("NtSuspendThread",
    +[](uint32_t r3, uint32_t r4, uint32_t, uint32_t, uint32_t,
        uint32_t) -> uint32_t {
      HANDLE h = nullptr;
      {
        std::lock_guard<std::mutex> lock(g_eventMutex);
        auto it = g_eventHandles.find(r3);
        if (it != g_eventHandles.end()) h = it->second;
      }
      DWORD prev = h ? SuspendThread(h) : (DWORD)-1;
      // iter-436 STABILITY CAMPAIGN: the handle=0 INFINITE SPIN variant
      // (16.6M calls / 1.8GB log in one roll — a worker-exit race leaves a
      // zeroed handle field that the guest suspends in a tight loop). The
      // unbudgeted per-call log WAS the 1.8GB; budget it and, on the spin
      // signature, capture the caller (guest lr + r1 chain) so the looping
      // guest function names itself. First occurrence + every 100000th.
      if (!h) {
        static std::atomic<uint64_t> s_null{0};
        const uint64_t nn = s_null.fetch_add(1, std::memory_order_relaxed) + 1;
        if (nn <= 8 || nn % 100000 == 0) {
          SR_LOG_KERNEL_IMPORT(
              "[NtSuspendThread] NULL-HANDLE #%llu handle=%u lr=%08X r1=%08X tid=%u",
              (unsigned long long)nn, r3, sr::kernel::getCurrentGuestPC(),
              sr::kernel::getCurrentGuestR1(), sr::kernel::getCurrentThreadHandle());
        }
      } else {
        SR_LOG_KERNEL_IMPORT("[NtSuspendThread] handle=%u host=%p prev=%ld", r3,
                             (void*)h, (long)prev);
      }
      if (r4 && prev != (DWORD)-1) writeGuestBE32(r4, prev);
      return h ? kStatusSuccess : 0xC0000008u;
    });

  // Rtl helpers
  REG_GENERIC_STUB("RtlCompareMemoryUlong");
  // iter-318 census fix: was a generic NO-OP — the guest's fill (e.g. a
  // framebuffer clear at 0xC003F390 len 0x320 pattern 0x80000000) silently
  // never happened. Real BE pattern store now.
  reg.registerExport("RtlFillMemoryUlong",
                     +[](uint32_t dest, uint32_t len, uint32_t pattern,
                         uint32_t, uint32_t, uint32_t) -> uint32_t {
                       for (uint32_t o = 0; o + 4 <= len; o += 4)
                         writeGuestBE32(dest + o, pattern);
                       return kStatusSuccess;
                     });
  REG_GENERIC_STUB("RtlMultiByteToUnicodeN");
  REG_GENERIC_STUB("RtlNtStatusToDosError");
  reg.registerExport("RtlRaiseException", stub_RtlRaiseException);
  REG_GENERIC_STUB("RtlTimeFieldsToTime");
  reg.registerExport("RtlTimeToTimeFields", stub_RtlTimeToTimeFields);
  REG_GENERIC_STUB("RtlUnicodeToMultiByteN");
  reg.registerExport("RtlUnwind", stub_RtlUnwind);

  // Vd* (graphics)
  reg.registerExport("VdGetSystemCommandBuffer", stub_VdGetSystemCommandBuffer);
  REG_GENERIC_STUB("VdCallGraphicsNotificationRoutines");
  REG_GENERIC_STUB("VdEnableDisableClockGating");
  reg.registerExport("VdEnableRingBufferRPtrWriteBack", stub_VdEnableRingBufferRPtrWriteBack);
  reg.registerExport("VdGetCurrentDisplayGamma", stub_VdGetCurrentDisplayGamma);
  reg.registerExport("VdGetCurrentDisplayInformation", stub_VdGetCurrentDisplayInformation);
  REG_GENERIC_STUB("VdInitializeEngines");
  reg.registerExport("VdInitializeRingBuffer", stub_VdInitializeRingBuffer);
  reg.registerExport("VdInitializeScalerCommandBuffer", stub_VdInitializeScalerCommandBuffer);
  reg.registerExport("VdIsHSIOTrainingSucceeded", stub_return_true);  // MUST return 1 (TRUE) or game thinks GPU is dead
  REG_GENERIC_STUB("VdPersistDisplay");
  reg.registerExport("VdQueryVideoFlags", stub_VdQueryVideoFlags);
  reg.registerExport("VdQueryVideoMode", stub_VdQueryVideoMode);
  REG_GENERIC_STUB("VdRetrainEDRAM");
  REG_GENERIC_STUB("VdRetrainEDRAMWorker");
  reg.registerExport("VdSetDisplayMode", stub_VdSetDisplayMode);
  reg.registerExport("VdSetGraphicsInterruptCallback", stub_VdSetGraphicsInterruptCallback);  // iter-345c: real vblank service
  REG_GENERIC_STUB("VdSetSystemCommandBufferGpuIdentifierAddress");
  REG_GENERIC_STUB("VdShutdownEngines");
  reg.registerExport("VdSwap", stub_VdSwap);

  // XAudio, XGet, XMA, XMsg, XNetLogon, XNotify
  REG_GENERIC_STUB("XAudioGetVoiceCategoryVolume");
  REG_GENERIC_STUB("XAudioGetVoiceCategoryVolumeChangeMask");
  // iter-22: real render-driver client implementation in kernel/audio_render_driver.cpp.
  // Captures (callbackArg, callbackFnVA) from the registration descriptor and spawns
  // a host audio thread that invokes the callback at ~5 ms cadence (Xbox 360's audio
  // frame rate). Without this the game registers a render driver client but the
  // callback never fires, so FMSoundSystem mixer/decode work never advances.
  extern uint32_t stub_XAudioRegisterRenderDriverClient(uint32_t, uint32_t, uint32_t,
                                                        uint32_t, uint32_t, uint32_t);
  extern uint32_t stub_XAudioUnregisterRenderDriverClient(uint32_t, uint32_t, uint32_t,
                                                          uint32_t, uint32_t, uint32_t);
  reg.registerExport("XAudioRegisterRenderDriverClient", stub_XAudioRegisterRenderDriverClient);
  // Imported and called once per mixed frame (lr 821209D4 via XAudioRenderClientSubmitFrame
  // 0x8211B680). Generic stub unless SR_AUDIO_OUT (registerAudioExports below).
  REG_GENERIC_STUB("XAudioSubmitRenderDriverFrame");
  reg.registerExport("XAudioUnregisterRenderDriverClient", stub_XAudioUnregisterRenderDriverClient);
  // iter-463 SR_AUDIO_OUT (kernel/apu/audio_system.hpp, default ON): XAudioSubmitRenderDriverFrame
  // + XAudioGetVoiceCategoryVolume(ChangeMask) = the port of Xenia's AudioSystem / xboxkrnl_audio.
  sr::kernel::apu::registerAudioExports(reg);
  REG_GENERIC_STUB("XGetAVPack");
  REG_GENERIC_STUB("XGetLanguage");
  // iter-318 census fix: was a generic no-op — callers received an ALL-ZERO
  // XVIDEO_MODE (width 0, height 0, refresh 0) and the display/FMV paths
  // branch on it (census: called from 0x822EB180 FE TU + 0x82611C70 render
  // TU with r5=0x2C = sizeof(XVIDEO_MODE)). Report real 720p widescreen.
  reg.registerExport("XGetVideoMode",
                     +[](uint32_t out, uint32_t, uint32_t, uint32_t, uint32_t,
                         uint32_t) -> uint32_t {
                       if (out) {
                         writeGuestBE32(out + 0x00, 1280);       // dwDisplayWidth
                         writeGuestBE32(out + 0x04, 720);        // dwDisplayHeight
                         writeGuestBE32(out + 0x08, 0);          // fIsInterlaced
                         writeGuestBE32(out + 0x0C, 1);          // fIsWideScreen
                         writeGuestBE32(out + 0x10, 1);          // fIsHiDef
                         writeGuestBE32(out + 0x14, 0x42700000); // RefreshRate 60.0f
                         writeGuestBE32(out + 0x18, 1);          // VideoStandard NTSC-M
                         for (uint32_t o = 0x1C; o < 0x2C; o += 4)
                           writeGuestBE32(out + o, 0);
                       }
                       return kStatusSuccess;
                     });
#if SR_ORACLE
  // The 16 legacy XMA fakes: the SR_XMA_NEW=0 A/B (kernel/apu/xma_legacy_exports.cpp, an EMULATION_SOURCES unit). C1
  // (docs/RELEASE_COMPOSITION.md 2.6): a disabled fallback component, out of the release configuration, where
  // registerXmaExports below registers the native exports unconditionally (kernel/apu/xma_api.hpp).
  sr::kernel::apu::registerXmaLegacyExports(reg);
#endif  // SR_ORACLE
  // iter-463 SR_XMA_NEW (kernel/apu/xma_device.hpp): the native port of Xenia's "new" XMA decoder
  // replaces the 16 legacy fakes above only when its gate is on (always, in the release configuration).
  sr::kernel::apu::registerXmaExports(reg);
  REG_GENERIC_STUB("XMsgCancelIORequest");
  REG_GENERIC_STUB("XMsgInProcessCall");
  REG_GENERIC_STUB("XMsgStartIORequest");
  // iter-325: XMsgStartIORequestEx(0xFA, 0x7001A) is the FE's transition
  // send — the splash stepper fires it when the counter passes 5 (natural
  // since iter-324b, 64+×/run) and on hardware the XAM-app receiver's job
  // is the FE-SM state-4 write to [[0x829761E0]+4] (iter-318 contract).
  // Deliver it here at the send site until a real XAM message pump exists.
  // SR_XMSG_7001A=0 disables.
  reg.registerExport("XMsgStartIORequestEx",
    +[](uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t,
        uint32_t) -> uint32_t {
      static std::atomic<uint32_t> s_n{0};
      const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
      if (stubCensusOn() && (n <= 3 || (n & (n - 1)) == 0))
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[STUB_CENSUS] XMsgStartIORequestEx #%u lr=%08X r3=%08X r4=%08X r5=%08X",
                    n, sr::kernel::getCurrentGuestLR(), r3, r4, r5);
      // iter-357j CHARTER FIX: default OFF (was default-ON). iter-356 proved
      // 0xFA/0x7001A is the XMP MUSIC PLAYER app message, NOT an FE
      // interface-state request — and the XMV movie player itself sends it at
      // every movie start (sub_824704B0), so the old default would fire a
      // spurious SetInterfaceState kick the moment the first splash plays.
      static const bool s_deliver = [] {
        const char* e = std::getenv("SR_XMSG_7001A");
        return e && e[0] == '1';
      }();
      // Defined in fe_flow_watch.cpp (consumer side).
      extern std::atomic<bool>     g_xmsg7001A_pending;
      extern std::atomic<uint32_t> g_xmsg7001A_payload;
      if (s_deliver && r3 == 0xFAu && r4 == 0x0007001Au) {
        // iter-330 CORRECTED RECEIVER (sub_824704B0 decompile): the message
        // payload at r6 is 12 bytes {area, state, flags} — the splash sends
        // {2, 4, 0} = an INTERFACE-STATE-CHANGE request. The title-side
        // receiver equivalent is SetInterfaceState_sub_821745D0(auiMod,
        // state, mode) — the iter-297g PROVEN contract (state 4 = golden
        // menu idle). Publish the request; the fe_flow_watch kick thread
        // performs the guest call (GuestCaller needs a host-owned thread).
        uint32_t area = 2, state = 4, flags = 0;
        if (r6 && isGuestVAInRange(r6)) {
          area = readGuestU32BE(r6);
          state = readGuestU32BE(r6 + 4u);
          flags = readGuestU32BE(r6 + 8u);
        }
        g_xmsg7001A_payload.store((area << 16) | (state & 0xFFFFu),
                                  std::memory_order_release);
        g_xmsg7001A_pending.store(true, std::memory_order_release);
        static std::atomic<uint32_t> s_kicks{0};
        const uint32_t k = s_kicks.fetch_add(1) + 1;
        if (k <= 8 || (k & (k - 1)) == 0)
          SR_LOG_BOOT(sr::LogSeverity::Info,
            "[XMSG_7001A] #%u queued: area=%u state=%u flags=%u -> SetInterfaceState",
            k, area, state, flags);
      }
      return kStatusSuccess;
    });
  REG_GENERIC_STUB("XNetLogonGetMachineID");
  REG_GENERIC_STUB("XNetLogonGetTitleID");
  reg.registerExport("XNotifyGetNext", stub_XNotifyGetNext);

  // Xam*
  reg.registerExport("XamAlloc", stub_XamAlloc);  // iter-437f: REAL signature (was RtlAllocateHeap — see stub)
  REG_GENERIC_STUB("XamContentClose");
  // iter-449r PLATFORM FIX: real out-param-writing implementations (see the
  // stubs by stub_XamUserGetSigninState for the pinned contract).
  reg.registerExport("XamContentCreateEnumerator",
                     stub_XamContentCreateEnumerator);
  REG_GENERIC_STUB("XamContentCreateEx");
  REG_GENERIC_STUB("XamContentDelete");
  REG_GENERIC_STUB("XamContentGetCreator");
  // iter-462 XAM storage part A (SR_XAM_CONTENT, default ON): the dummy HDD, as Xenia.
  if (xamContentEnabled()) {
    reg.registerExport("XamContentGetDeviceData", stub_XamContentGetDeviceData);
    reg.registerExport("XamContentGetDeviceName", stub_XamContentGetDeviceName);
    reg.registerExport("XamContentGetDeviceState", stub_XamContentGetDeviceState);
  } else {
    REG_GENERIC_STUB("XamContentGetDeviceData");
    REG_GENERIC_STUB("XamContentGetDeviceName");
    REG_GENERIC_STUB("XamContentGetDeviceState");
  }
  REG_GENERIC_STUB("XamContentSetThumbnail");
  reg.registerExport("XamEnumerate", stub_XamEnumerate);
  REG_GENERIC_STUB("XamFree");
  REG_GENERIC_STUB("XamGetExecutionId");
  // iter-317h ROOT FIX (not a band-aid): XamGetSystemVersion returning 0
  // (generic-success) sent sub_82470700 (the splash-done -> FE state-2
  // transition, sole sender of XMsg 0x7001A via sub_824704B0(2,..)) down
  // its OLD-KERNEL fallback, which bails 0x65B when the pending-transition
  // global DAT_82976AF4 is unset -- the FE transition NEVER fired and the
  // title clean-exited after the splash sequence (the mass .bss zero at
  // guestLR 0x827AC4E0). Return a real dash version 2.0.17559.0: the
  // (ver & 0xFFFF00) >= 0x8A100 check takes the modern path, which calls
  // sub_824704B0(2,4,0) UNCONDITIONALLY.
  reg.registerExport("XamGetSystemVersion",
                     +[](uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                         uint32_t) -> uint32_t {
                       static std::atomic<uint32_t> s_n{0};
                       const uint32_t n =
                           s_n.fetch_add(1, std::memory_order_relaxed) + 1;
                       if (n <= 4)
                         SR_LOG_BOOT(sr::LogSeverity::Info,
                                     "[STUB_CENSUS] XamGetSystemVersion #%u "
                                     "lr=%08X -> 0x20449700 (FE-transition "
                                     "version gate)",
                                     n, sr::kernel::getCurrentGuestLR());
                       return 0x20449700u;
                     });
  reg.registerExport("XamInputGetCapabilities", stub_XamInputGetCapabilities);
  reg.registerExport("XamInputGetState", stub_XamInputGetState);
  reg.registerExport("XamInputSetState", stub_XamInputSetState);
  REG_GENERIC_STUB("XamLoaderLaunchTitle");
  REG_GENERIC_STUB("XamLoaderTerminateTitle");
  reg.registerExport("XamNotifyCreateListener", stub_XamNotifyCreateListener);
  reg.registerExport("XamShowDeviceSelectorUI", stub_XamShowDeviceSelectorUI_v2);  // iter-462: r7/r8, 997, overlapped (SR_XAM_CONTENT=0: the iter-355 stub)  // iter-355: emit XN_SYS_UI(9,0) close
  reg.registerExport("XamShowDirtyDiscErrorUI", stub_XamShowDirtyDiscErrorUI);  // iter-355: emit XN_SYS_UI(9,0) close
  reg.registerExport("XamShowMessageBoxUIEx", stub_XamShowMessageBoxUIEx);      // iter-355: emit XN_SYS_UI(9,0) close
  reg.registerExport("XamShowSigninUI", stub_XamShowSigninUI);
  reg.registerExport("XamUserGetName", stub_XamUserGetName);
  reg.registerExport("XamUserGetSigninState", stub_XamUserGetSigninState);
  reg.registerExport("XamUserGetXUID", stub_XamUserGetXUID);
  REG_GENERIC_STUB("XamUserReadProfileSettings");

  // Xex
  REG_GENERIC_STUB("XexCheckExecutablePrivilege");
  reg.registerExport("XexGetModuleHandle", stub_XexGetModuleHandle);
  reg.registerExport("XexGetProcedureAddress", stub_XexGetProcedureAddress);

  // CRT / exception (guest may call these)
  REG_GENERIC_STUB("__C_specific_handler");
  reg.registerExport("_vsnprintf", stub_vsnprintf);
  reg.registerExport("sprintf", stub_sprintf);

  // NetDll (network - all stub)
  REG_GENERIC_STUB("NetDll_WSACleanup");
  REG_GENERIC_STUB("NetDll_WSACloseEvent");
  REG_GENERIC_STUB("NetDll_WSACreateEvent");
  REG_GENERIC_STUB("NetDll_WSAGetLastError");
  REG_GENERIC_STUB("NetDll_WSAGetOverlappedResult");
  REG_GENERIC_STUB("NetDll_WSARecv");
  reg.registerExport("NetDll_WSARecvFrom", stub_NetDll_WSARecvFrom);
  REG_GENERIC_STUB("NetDll_WSAResetEvent");
  REG_GENERIC_STUB("NetDll_WSASetEvent");
  REG_GENERIC_STUB("NetDll_WSAStartup");
  reg.registerExport("NetDll_WSAWaitForMultipleEvents", stub_NetDll_WSAWaitForMultipleEvents);
  REG_GENERIC_STUB("NetDll_XNetCleanup");
  REG_GENERIC_STUB("NetDll_XNetConnect");
  REG_GENERIC_STUB("NetDll_XNetCreateKey");
  REG_GENERIC_STUB("NetDll_XNetDnsLookup");
  REG_GENERIC_STUB("NetDll_XNetDnsRelease");
  REG_GENERIC_STUB("NetDll_XNetGetConnectStatus");
  REG_GENERIC_STUB("NetDll_XNetGetEthernetLinkStatus");
  reg.registerExport("NetDll_XNetGetTitleXnAddr", stub_NetDll_XNetGetTitleXnAddr);  // phase 4 row 2
  REG_GENERIC_STUB("NetDll_XNetQosRelease");
  REG_GENERIC_STUB("NetDll_XNetQosServiceLookup");
  REG_GENERIC_STUB("NetDll_XNetRegisterKey");
  REG_GENERIC_STUB("NetDll_XNetStartup");
  REG_GENERIC_STUB("NetDll_XNetUnregisterKey");
  // iter-348 ROOT FIX (charter platform fix — THE .data-wipe root): the
  // game's remote-debug/capture service (port 0x23F1=9201, magic 0xBC614E,
  // service obj ctor sub_8263CF70, packet loop sub_8263CCD0, config parser
  // sub_8263C750) accepts a client, then RESIZES its shared tri-buffer with
  // atoi()-parsed sizes from the received text. On console with no debug PC,
  // accept() returns -1 forever and none of that code runs. Our old
  // generic-success stubs returned 0: the accept wrapper Function_825677B0
  // checks `ret != -1` → 0 = PHANTOM CLIENT; recv "succeeded" with an
  // UNTOUCHED buffer → the handler parsed stale stack as commands → garbage
  // sizes → resize → memset(buf, 0, ptr-as-size≈2.2GB) flattening .data
  // (splash names 0x8291363C + service descriptors 0x8295C62C — the wipe).
  // Honest no-client semantics: accept/connect/recv/recvfrom/send/sendto
  // fail with SOCKET_ERROR (-1); select reports no ready fds; the rest keep
  // benign success (bind/listen/close succeed on console regardless).
  reg.registerExport("NetDll_accept", stub_NetDll_fail_socket_error);
  REG_GENERIC_STUB("NetDll_bind");
  REG_GENERIC_STUB("NetDll_closesocket");
  reg.registerExport("NetDll_connect", stub_NetDll_fail_socket_error);
  REG_GENERIC_STUB("NetDll_getpeername");
  REG_GENERIC_STUB("NetDll_getsockname");
  REG_GENERIC_STUB("NetDll_getsockopt");
  REG_GENERIC_STUB("NetDll_ioctlsocket");
  REG_GENERIC_STUB("NetDll_listen");
  reg.registerExport("NetDll_recv", stub_NetDll_fail_socket_error);
  reg.registerExport("NetDll_recvfrom", stub_NetDll_fail_socket_error);
  REG_GENERIC_STUB("NetDll_select");
  reg.registerExport("NetDll_send", stub_NetDll_fail_socket_error);
  reg.registerExport("NetDll_sendto", stub_NetDll_fail_socket_error);
  REG_GENERIC_STUB("NetDll_setsockopt");
  REG_GENERIC_STUB("NetDll_shutdown");
  REG_GENERIC_STUB("NetDll_socket");
  reg.registerExport("NetDll_WSAGetLastError", stub_NetDll_WSAGetLastError);
  // iter-463 (SR_XAM_CONTENT_FS, default ON): real save storage. MUST stay the last
  // line - it replaces the XamContent*/NtWriteFile/RtlNtStatusToDosError stubs above.
  // iter-463 (SR_XAM_PROFILE, default ON): XamUserReadProfileSettings with Xenia's
  // profile defaults (replaces the generic stub above).
  registerXamProfileExports(reg);
  registerXamContentExports(reg);
  // step4/audit (logic audit 9a): Xenia 0bd090dbe's answers for XamUserGetXUID / ExGetXConfigSetting /
  // XamGetSystemVersion / the XMP app, each only when its own gate is on (kernel/import_fixes.cpp).
  registerImportFixes(reg);
#if SR_ORACLE && defined(_WIN32)
  diagwait::install(reg);  // step 6 G6a DIAG [DIAG_WAIT] / [DIAG_WAKE]: wraps the final registrations above
#endif
}

// iter-432 (roll-stability): exported wrapper for the guest-thread death
// handler (guest_harness.cpp) — body lives with the CS map above.
void dumpCsOwnedByCurrentThread(const char* tag) {
  dumpCsOwnedByCurrentThreadImpl(tag);
}

// step 6 I8: RtlInitializeCriticalSection's counts (kernel/cs_reinit.hpp)
CsInitCounts csInitCounts() {
  std::lock_guard<std::mutex> lock(s_csMapMutex);
  return {s_csInit.all, s_csInit.first, s_csInit.reused, s_csInit.held, s_csInit.replaced, s_csGraveyard.size()};
}
#if SR_ORACLE
uintptr_t csHostIdentityForTest(uint32_t guestCs) {
  bool oor = false;
  const uint32_t key = csCanonicalKey(guestCs, &oor);
  std::lock_guard<std::mutex> lock(s_csMapMutex);
  const auto it = s_csMap.find(key);
  return it == s_csMap.end() ? 0 : reinterpret_cast<uintptr_t>(it->second.get());
}
#endif

// iter-463 (GPU_HANG_WD (g)): every guest CS with a live owner. The map lock
// is only TRIED (the hang watchdog must never wait on it); the lock-free
// out-of-range table is always listed. Log-only, reads host atomics only.
bool dumpCsHeldTable(const char* tag) {
  int held = 0;
  auto line = [&](uint32_t key, const HostCriticalSection& cs, const char* where) {
    if (cs.owner.load(std::memory_order_relaxed) == std::thread::id{}) return;
    ++held;
    SR_LOG_BOOT(::sr::LogSeverity::Error,
                "[%s] cs=0x%08X (%s) ownerHandle=0x%08X hostTid=%u rec=%u enterLR=0x%08X", tag,
                key, where, cs.ownerHandle.load(std::memory_order_relaxed),
                cs.ownerHostTid.load(std::memory_order_relaxed),
                cs.recursionCount.load(std::memory_order_relaxed),
                cs.ownerEnterLr.load(std::memory_order_relaxed));
  };
  const bool locked = s_csMapMutex.try_lock();
  if (locked) {
    for (const auto& kv : s_csMap)
      if (kv.second) line(kv.first, *kv.second, "map");
    s_csMapMutex.unlock();
  }
  for (const OorCsSlot& slot : s_oorCsTable) {
    const uint32_t key = slot.key.load(std::memory_order_acquire);
    const HostCriticalSection* cs = key ? slot.cs.load(std::memory_order_acquire) : nullptr;
    if (cs) line(key, *cs, "oor");
  }
  SR_LOG_BOOT(::sr::LogSeverity::Error, "[%s] %d held guest CS%s", tag, held,
              locked ? "" : " (map lock busy - out-of-range table only)");
  return locked;
}

}  // namespace kernel
}  // namespace sr
