#pragma once
// iter-462 PLATFORM FIX (SR_OB_KTHREAD, default ON; =0 reproduces the generic
// stub exactly): guest KTHREAD objects + ObReferenceObjectByHandle.
//
// The console kernel gives every thread a guest-visible X_KTHREAD. The game's
// GetExitCodeThread (sub_82469E98) calls ObReferenceObjectByHandle(h,
// ExThreadObjectType, &obj) and reads obj+4 (the dispatch header's
// signal_state, low byte) and obj+0x140 (exit_status) to decide whether the
// thread still runs (0x103 STILL_ACTIVE). Our ObReferenceObjectByHandle was a
// REG_GENERIC_STUB that returned success WITHOUT writing *obj, so the guest
// read a stale stack slot as the KTHREAD and reported live EAThreadPool
// workers as exited; EAThreadPool_FixThreads (0x82606100) then NtClosed the
// live worker and started a clone on the SAME ThreadInfo (up to 19 at once),
// and the clones' mbQuit stores landed in the freed ThreadInfo (the
// 0x89674600 -> 0x89014600 pool free-list corruption). Evidence:
// archive/v1_eapool/.
//
// Reference (BSD-3, ported semantics): xenia-canary
//   src/xenia/kernel/xboxkrnl/xboxkrnl_ob.cc:245-283 (ObReferenceObjectByHandle)
//   src/xenia/kernel/xthread.cc:180-200 (InitializeGuestObject),
//   480-489 (Exit), 520-527 (Terminate); xthread.h:259-372 (X_KTHREAD layout).
//
// This is NOT forcing: no guest function is called or overridden; the guest's
// own GetExitCodeThread gets the answer the console kernel gives it.
//
// Kernel-only header: do NOT include from guest/ (and never add these
// declarations to guest_context.hpp - that header rebuilds all guest TUs).
#include <cstdint>

namespace sr {
namespace kernel {

// ── layout (X_KTHREAD, xthread.h; all fields big-endian) ────────────────────
constexpr uint32_t kKThreadSize = 0xAB0u;           // static_assert_size(X_KTHREAD, 0xAB0)
constexpr uint32_t kKThreadStride = 0xB00u;
constexpr uint32_t kKThreadArenaLo = 0xD0020000u;   // harness-private carveout
constexpr uint32_t kKThreadArenaHi = 0xD00E0000u;   //   (guest_harness.cpp PHYS_UNIFY regions);
                                                    //   [D00E0000,D0100000) = KPCRs (kpcr.hpp)
constexpr uint32_t kKtOffType = 0x00u;              // u8 header.type
constexpr uint32_t kKtOffSignalState = 0x04u;       // be32 header.signal_state
constexpr uint32_t kKtOffWaitList = 0x08u;          // X_LIST_ENTRY header.wait_list
constexpr uint32_t kKtOffMutantsList = 0x10u;       // X_LIST_ENTRY mutants_list
constexpr uint32_t kKtOffStackBase = 0x5Cu;
constexpr uint32_t kKtOffStackLimit = 0x60u;
constexpr uint32_t kKtOffTerminated = 0xBEu;        // u8
constexpr uint32_t kKtOffExitStatus = 0x140u;
constexpr uint32_t kKtOffThreadId = 0x14Cu;
constexpr uint32_t kKtOffStartAddress = 0x150u;
constexpr uint32_t kKtOffCreationFlags = 0x16Cu;
constexpr uint8_t kDispatcherThread = 6u;           // X_DISPATCHER_FLAGS::DISPATCHER_THREAD

// The ExThreadObjectType kernel DATA import cell (guest_context.cpp SR_DATA_IMPORTS
// resolves it to 0xD0016000). The guest loads the type pointer from here.
constexpr uint32_t kExThreadObjectTypeCell = 0x82000780u;

constexpr uint32_t kCurrentThreadPseudoHandle = 0xFFFFFFFEu;
constexpr uint32_t kObStatusInvalidHandle = 0xC0000008u;
constexpr uint32_t kObStatusObjectTypeMismatch = 0xC0000024u;

// SR_OB_KTHREAD (default ON). Every function below is inert while it is off.
bool obKThreadEnabled();
// Test seam: -1 = follow the env, 0 = force off, 1 = force on.
void obKThreadSetEnabledForTest(int mode);

// ExCreateThread: allocate + initialise the thread's KTHREAD (before any host
// thread exists) and map `handle` to it with one open. Returns the block VA
// (0 when the gate is off).
uint32_t kthreadCreate(uint32_t handle, uint32_t startVA, uint32_t stackVA,
                       uint32_t stackSize, uint32_t creationFlags);
// guestThreadFunc: bind this host thread to the KTHREAD mapped for `handle`.
void kthreadBindCurrent(uint32_t handle);
// iter-463 SR_KT_BIND_VA (default ON): bind to the block ExCreateThread built,
// independent of whether its handle is still open when the thread starts.
bool kthreadBindByVAEnabled();
void kthreadBindByVASetForTest(int mode);   // -1 = env, 0 = off, 1 = on
void kthreadBindCurrentVA(uint32_t kt);
// This thread's KTHREAD. When unbound: binds to the block mapped for the
// thread's current handle, else (createIfMissing) creates an anonymous block
// (main/host threads). Returns 0 if none and !createIfMissing.
uint32_t kthreadCurrent(bool createIfMissing);
// NtDuplicateObject(-2): the handle names `kt`. A handle already mapped gains
// one more open (our -2 duplicate returns the SAME handle number), so a closed
// duplicate never drops a live thread's KTHREAD.
void kthreadRegisterHandle(uint32_t handle, uint32_t kt);
// NtDuplicateObject(src): a fresh handle naming the same KTHREAD (one open).
void kthreadAliasHandle(uint32_t newHandle, uint32_t srcHandle);
// NtClose: drop one open; the mapping is erased only when the count hits 0.
// The block itself is never freed (same lifetime policy as thread stacks).
void kthreadCloseHandle(uint32_t handle);
// Exit sites (ExTerminateThread / guestThreadFunc): idempotent, acts on this
// thread's KTHREAD. exit_status, terminated, release fence, then signal_state.
void kthreadMarkExited(uint32_t code);
// ExCreateThread whose host CreateThread failed: the thread will never run,
// so it must not look alive forever (FixThreads would never replace it).
void kthreadMarkHandleExited(uint32_t handle, uint32_t code);

// The export body (gate ON). Xenia semantics: *out = 0 first; -2 = current
// thread; unknown handle -> 0xC0000008; type mismatch against the live
// ExThreadObjectType cell -> 0xC0000024 (any type accepted while the cell is
// unresolved, i.e. 0); else *out = KTHREAD VA, STATUS_SUCCESS.
uint32_t obReferenceObjectByHandle(uint32_t handle, uint32_t typeVA, uint32_t outVA);

// iter-463 SR_KE_RESUME_THREAD (audio arc; default ON since the iter-463 rolls, =0 = generic stub): KeResumeThread takes a
// KTHREAD pointer, not a handle (Xenia xboxkrnl_threading.cc KeResumeThread_entry:
// GetNativeObject<XThread>(thread_ptr) -> XThread::Resume, else X_STATUS_INVALID_HANDLE).
// ExCreateThread records the host thread for each KTHREAD it builds (a duplicated handle, so a
// guest NtClose of the creator's handle cannot invalidate it); the export resumes through it.
bool keResumeThreadEnabled();
void keResumeThreadSetEnabledForTest(int mode);   // -1 = env, 0 = off, 1 = on
void kthreadRecordHostThread(uint32_t kt, void* hostThread);   // takes ownership of a dup
// Returns the previous suspend count (DWORD semantics), or -1 when `kt` names no host thread.
long kthreadResumeHost(uint32_t kt);

// iter-465 physmem: true when va is a KTHREAD block this module built (the arena,
// or an overflow block once the arena is full).
bool kthreadIsBlockVA(uint32_t va);

// Diagnostics / tests (no guest writes).
uint32_t kthreadLookup(uint32_t handle);      // 0 when unmapped
uint32_t kthreadOpenCount(uint32_t handle);   // 0 when unmapped
uint32_t kthreadArenaCursorForTest(uint32_t newCursor);   // returns the old cursor

}  // namespace kernel
}  // namespace sr
