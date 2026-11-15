#pragma once
// iter-463 PLATFORM FIX: a per-thread guest KPCR (real r13) and the Ke/Kf
// spinlock + IRQL exports.
//
// SR_KPCR (default ON; =0 keeps r13 = 0 on every thread, exactly as before).
//   The console kernel gives every thread a Processor Control Region and keeps
//   its address in r13. Guest code reads it inline:
//     +0x100 prcb.current_thread (13 functions: RtlGet/SetLastError, the D3D
//            ring waits' KTHREAD+0x58 read, XAudio's audio-thread identity
//            test in XAudioWaitVoiceIdleUnlessAudioThread_sub_82118600, ...)
//     +0x10C prcb.current_cpu     (10 functions: KeGetCurrentProcessorNumber,
//            the GPU ISR's per-CPU pending bit, XAudio's per-CPU worker slot)
//     +0x150 prcb.dpc_active      (3 functions: last-error writes are skipped
//            inside a DPC)
//   With r13 = 0 all threads shared ONE "KPCR" at guest address 0: GetLastError
//   was process-global, every thread passed XAudio's audio-thread test, and the
//   Ke/Kf spinlocks (owner = r13) could exclude nothing.
//
// SR_KE_SPINLOCK (default ON; needs SR_KPCR; =0 or SR_KPCR=0 keeps the generic
//   stubs): KeAcquireSpinLockAtRaisedIrql / KeReleaseSpinLockFromRaisedIrql /
//   KfAcquireSpinLock / KfReleaseSpinLock / KeRaiseIrqlToDpcLevel / KfLowerIrql
//   with Xenia semantics (CAS 0 -> r13 on the lock word, IRQL in KPCR+0x18).
//
// Reference (BSD-3, ported semantics), xenia-canary @0bd090dbe (the oracle
// build the user runs):
//   src/xenia/kernel/xthread.h:114-212  (X_KPRCB / X_KPCR layout)
//   src/xenia/kernel/xthread.cc:157-178 (GetFakeCpuNumber), 377-409 (the KPCR
//     fields XThread::Create writes), 824-847 (SetAffinity / SetActiveCpu)
//   src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc:360-385
//     (KeSetAffinityThread), 1184-1350 (spinlocks, IRQL)
//   src/xenia/kernel/kernel_state.cc:1382-1438 (DPC impersonation for the
//     graphics interrupt)
//
// NOT forcing: no guest function is called or overridden. The guest's own code
// reads the structures the console kernel provides.
//
// Kernel-only header (never add these to guest_context.hpp: every guest TU
// includes that one).
#include <cstdint>

namespace sr {
namespace kernel {

class ExportRegistry;

// ── layout (X_KPCR, xthread.h; all fields big-endian) ───────────────────────
constexpr uint32_t kKpcrSize = 0x2D8u;             // SystemHeapAlloc(0x2D8)
constexpr uint32_t kKpcrStride = 0x400u;
constexpr uint32_t kKpcrArenaLo = 0xD00E0000u;     // harness-private carveout (top
constexpr uint32_t kKpcrArenaHi = 0xD0100000u;     //   128 KB; KTHREADs sit below)
constexpr uint32_t kKpcrOffTlsPtr = 0x000u;
constexpr uint32_t kKpcrOffProcTypeInDpc = 0x00Cu;  // u8
constexpr uint32_t kKpcrOffIrql = 0x018u;          // u8 current_irql
constexpr uint32_t kKpcrOffPcrPtr = 0x030u;        // be64
constexpr uint32_t kKpcrOffStackBase = 0x070u;
constexpr uint32_t kKpcrOffStackEnd = 0x074u;
constexpr uint32_t kKpcrOffPrcb = 0x100u;          // prcb_data
constexpr uint32_t kKpcrOffCurrentThread = 0x100u; // prcb.current_thread
constexpr uint32_t kKpcrOffCurrentCpu = 0x10Cu;    // u8 prcb.current_cpu
constexpr uint32_t kKpcrOffDpcActive = 0x150u;     // be32 prcb.dpc_active
constexpr uint32_t kKpcrOffPrcbPtr = 0x2A8u;       // -> pcr + 0x100
constexpr uint32_t kKtOffCurrentCpu = 0xBFu;       // u8 X_KTHREAD.current_cpu
constexpr uint32_t kKtOffLastError = 0x160u;

constexpr uint32_t kKeStatusInvalidParameter = 0xC000000Du;
constexpr uint32_t kStatusInvalidHandleKe = 0xC0000008u;

// SR_KPCR (needs SR_OB_KTHREAD: the KPCR points at the thread's KTHREAD).
bool kpcrEnabled();
// SR_KE_SPINLOCK && kpcrEnabled().
bool keSpinlockEnabled();
// Test seams: -1 = follow the env, 0 = force off, 1 = force on.
void kpcrSetEnabledForTest(int mode);
void keSpinlockSetEnabledForTest(int mode);

// Xenia GetFakeCpuNumber: an explicit processor mask names the CPU (highest
// set bit); 0 inherits the creating thread's CPU, else round-robin over 0..5.
// Call on the CREATING thread.
uint8_t kpcrFakeCpuNumber(uint8_t procMask);

// ExCreateThread (creator's thread): build the new thread's KPCR next to its
// KTHREAD before the host thread exists. Returns the KPCR VA (0 when off).
uint32_t kpcrCreateForKThread(uint32_t kt, uint8_t cpu, uint32_t stackBase,
                              uint32_t stackLimit);
// This host thread's KPCR: the one already built for its KTHREAD, else a new
// one (cpu < 0 = kpcrFakeCpuNumber(0)). Binds it to the thread and returns the
// VA to put in r13 (0 when the gate is off - r13 stays 0 as before).
uint32_t kpcrBindCurrent(uint32_t stackBase, uint32_t stackLimit, int cpu);
uint32_t kpcrCurrent();                  // this thread's bound KPCR (0 = none)
uint32_t kpcrForKThread(uint32_t kt);    // 0 when none
uint8_t kpcrCpu(uint32_t pcr);
// Xenia XThread::SetActiveCpu: KPCR+0x10C and KTHREAD+0xBF.
void kpcrSetActiveCpu(uint32_t pcr, uint8_t cpu);

// DPC impersonation (Xenia KernelState::Begin/EndDPCImpersonation +
// xeKeSetCurrentProcessType around EmulateCPInterruptDPC): irql 2, dpc_active 1,
// processtype_value_in_dpc TITLE; End restores irql and sets IDLE, dpc_active 0.
struct KpcrDpcScope {
  explicit KpcrDpcScope(uint32_t pcr);
  ~KpcrDpcScope();
  KpcrDpcScope(const KpcrDpcScope&) = delete;
  KpcrDpcScope& operator=(const KpcrDpcScope&) = delete;
 private:
  uint32_t pcr_;
  uint8_t prevIrql_;
};

// The exports (Xenia bodies; the generic stub when keSpinlockEnabled() is
// false). `r13` is the calling guest context's r13 (import glue).
uint32_t keAcquireSpinLock(uint32_t lockVA, uint32_t r13, bool raiseIrql);
void keReleaseSpinLock(uint32_t lockVA, uint32_t r13, uint32_t oldIrql, bool changeIrql);
uint32_t keRaiseIrqlToDpcLevel(uint32_t r13);
void kfLowerIrql(uint32_t r13, uint32_t newIrql);
uint32_t keSetAffinityThread(uint32_t kt, uint32_t affinity, uint32_t prevOutVA);

// Registers KeAcquireSpinLockAtRaisedIrql, KeReleaseSpinLockFromRaisedIrql,
// KfAcquireSpinLock, KfReleaseSpinLock, KeRaiseIrqlToDpcLevel, KfLowerIrql,
// KeSetAffinityThread. With the gates off each keeps the exact generic-stub
// body ([STUB_CENSUS] line, returns 0).
void registerKpcrExports(ExportRegistry& reg);

// ── iter-463 SR_PROCTYPE: KeGetCurrentProcessType from the KTHREAD (default ON) ────────────
// Xenia (xthread.cc:207-217 InitializeGuestObject) copies the owning process's type into every
// KTHREAD at +0x72 (process_type_dup) and +0x73 (process_type): title threads = TITLE (1), the
// "Audio Worker" XHostThread = SYSTEM (2, kernel_state->GetSystemProcess()).
// xboxkrnl_threading.cc:429-437 xeKeGetCurrentProcessType: dpc_active ? KPCR+0xC
// (processtype_value_in_dpc) : KTHREAD(KPCR+0x100)+0x73.
// With the gate off (or SR_KPCR=0 / r13 = 0) the export returns 0 exactly as the old stub did,
// and no KTHREAD byte is written.
constexpr uint8_t kProcTypeIdle = 0, kProcTypeTitle = 1, kProcTypeSystem = 2;
constexpr uint32_t kKtOffProcessTypeDup = 0x72u, kKtOffProcessType = 0x73u;
bool procTypeEnabled();                  // SR_PROCTYPE && kpcrEnabled()
void procTypeSetEnabledForTest(int mode);
// Write +0x72/+0x73 (no-op when the gate is off or kt == 0).
void kthreadSetProcessType(uint32_t kt, uint8_t type);
// The export body (r13 = the caller's KPCR). 0 when the gate is off or r13 has no KPCR.
uint32_t keGetCurrentProcessType(uint32_t r13);

// Diagnostics / tests.
struct KeSpinStats {
  uint64_t acquires, contended, longSpins, recursions, foreignReleases, noPcr;
};
KeSpinStats keSpinStats();
uint32_t kpcrArenaCursorForTest(uint32_t newCursor);   // returns the old cursor

}  // namespace kernel
}  // namespace sr
