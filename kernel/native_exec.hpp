#pragma once

// iter-467 NATIVE strand A: the native executor - pipelined fences (SR_NATIVE_EXEC, default OFF).
//
// With S1's native records the CP thread is the native executor's front end. Today it also drains
// (translates the queued draws into D3D12 batches) synchronously before every guest-visible effect
// (EVENT_WRITE_SHD / MEM_WRITE / REG_TO_MEM fences, scratch write-backs, INTERRUPT, XE_SWAP) and at
// every indirect-buffer end, so parse and drain add up on one thread (fp3: 9.26 ms/frame, the drain
// 5.73 of it). Under SR_NATIVE_EXEC the drain runs on its own thread ("SR native exec", the executor)
// and the effects become stream items it performs in order:
//   - a memory store (fence / MEM_WRITE / REG_TO_MEM / scratch write-back) is performed only after
//     every draw and resolve ahead of it has been drained, in stream order;
//   - XE_SWAP's drain + present run on the executor;
//   - INTERRUPT (the guest ISR runs on the CP thread's GuestCaller) and an unsatisfied WAIT_REG_MEM
//     first wait for the executor to finish everything pushed so far (cp.flush = sync).
// Native record path only: SR_NATIVE_FLUSH on, no live-trace recording. The PM4 oracle path
// (SR_NATIVE_FLUSH=0) and the .xtr replay keep the synchronous drains, byte-identical.
//
//   SR_NATIVE_EXEC=1            the executor (default OFF; in-roll A/B: SR_AB_TOGGLE=SR_NATIVE_EXEC)
//   SR_NATIVE_EXEC_CHECK=1      fence order (each effect: every earlier item executed) and data stability
//                               (hash of each sampled draw's index + vertex-fetch ranges at push vs at
//                               execute, by class: command-buffer page / other) -> [NATIVE_EXEC] BAD
//   SR_NATIVE_EXEC_CHECK_EVERY  sample every Nth draw for the data hash (default 8; command-buffer draws
//                               are always hashed)
//   SR_NATIVE_EXEC_LAG_US       CHECK only: the executor sleeps this long per item (the widest window for
//                               the guest to rewrite data the executor has not read yet)
//
// C1 (docs/RELEASE_COMPOSITION.md 2.2): the executor is a default-OFF experiment on the emulated CP's drain, a
// disabled path the default build never takes. The release configuration (SR_ORACLE=0, kernel/sr_oracle.hpp)
// compiles kernel/native_exec.cpp out (CMakeLists.txt EMULATION_SOURCES) and gfx/null_renderer.cpp's executor
// thread with it; this API is then inline no-ops answering what the shipping code sees with the gate unset
// (deferring() false, nothing counted, no profile). The dev build and difftest keep the experiment whole.

#include <cstdint>

#include "sr_oracle.hpp"

namespace sr::gfx {
struct GuestDrawCommand;
}

namespace sr::native::exec {

// Effect items (GuestDrawCommand::effectKind).
enum Kind : uint8_t { kNone = 0, kStores = 1, kSwap = 2 };
// Store ops in an item (drawParams, 4 dwords each: op, addr, value, flags).
enum StoreOp : uint32_t { kOpEvShd = 0x58, kOpMemWrite = 0x3D, kOpRegToMem = 0x3E, kOpScratch = 0x578 };
constexpr uint32_t kStoresPerItem = 20;
enum PlaceRole { kPlCp = 0, kPlExec = 1, kPlGuestSwap = 2, kPlRender = 3 };

#if SR_ORACLE
bool gateOn();      // SR_NATIVE_EXEC=1 (fixed at startup)
bool checkOn();     // SR_NATIVE_EXEC_CHECK=1
uint32_t lagUs();   // SR_NATIVE_EXEC_LAG_US (CHECK only)
void lagSpin();     // busy-waits lagUs() (a sleep would round up to the OS timer, ~1 ms)
// The executor takes effects now: gate on, the in-roll toggle on, the native record path on, no live
// trace armed. Read at the point of use (each effect / IB end).
bool deferring();
// SR_NATIVE_EXEC_FRONTEND=1 (experiment, default OFF): the hardware front-end order - scratch
// write-backs, INTERRUPT and MEM_WRITE happen when the CP reaches them (the micro-engine performs them
// ahead of the 3D pipeline); only EVENT_WRITE_SHD fences (end of pipe), REG_TO_MEM and the swap wait
// for the draws ahead. Xenia's order (everything after the prior draws are translated) is the default.
bool frontend();

// ---- CHECK: data stability (CP at push, executor at execute) ----
// Fills c.execChk* when this draw is sampled (CP thread, emitDraw; usedFetch = the VS's vertex fetch
// constant slots as a 96-bit mask).
void chkAtPush(sr::gfx::GuestDrawCommand& c, const uint32_t usedFetch[3]);
// Executor, before the drain reads the draw's memory.
void chkAtExecute(const sr::gfx::GuestDrawCommand& c, uint64_t itemsBetween);
// Effect order: the effect pushed as item `seq` executes with `consumed` items consumed before it.
void chkEffect(uint64_t seq, uint64_t consumed, uint8_t kind);

// The executor thread (CHECK hooks and the lag run only there).
void markExecutorThread();
bool onExecutorThread();

// Counters.
void noteEffectPushed(uint8_t kind, uint32_t stores);
void noteSync(uint64_t waitUs, bool timedOut);
// Why the CP is about to sync (counted by the next noteSync): 1 WAIT_REG_MEM on a queued store's word,
// 2 INTERRUPT, 3 COND_WRITE, 0 other (an A/B OFF window, a synchronous caller).
void syncReason(uint32_t r);
void noteDrainCall(uint32_t items);
void report(const char* why);

// SR_NATIVE_EXEC_PROF=1 (observe-only, default OFF): where the executor's and the CP's time goes, and
// where the busy threads run (logical CPU / physical core / L3 of each sample; the guest thread that
// calls VdSwap = the guest render tick). [NATIVE_EXEC_PROF] every 2048 guest swaps.
bool profOn();
uint64_t profNow();  // QPC ticks
void profExecIter(uint64_t lockTicks, uint64_t busyTicks, uint32_t depth);  // executor, one drain
void profExecIdle(uint64_t spinTicks, uint64_t sleepTicks, bool spinHit);   // executor, one empty spell
void profReserveWait(uint64_t ticks);                                        // CP, queue full
void profPlace(int role);  // CP: per IB; executor: per drain; guest: per VdSwap (reports); render: per frame
#else
// SR_ORACLE=0: what the shipping code sees with SR_NATIVE_EXEC unset - no executor, no CHECK, no profile.
inline constexpr bool gateOn() { return false; }
inline constexpr bool checkOn() { return false; }
inline constexpr uint32_t lagUs() { return 0u; }
inline void lagSpin() {}
inline constexpr bool deferring() { return false; }
inline constexpr bool frontend() { return false; }
inline void chkAtPush(sr::gfx::GuestDrawCommand&, const uint32_t[3]) {}
inline void chkAtExecute(const sr::gfx::GuestDrawCommand&, uint64_t) {}
inline void chkEffect(uint64_t, uint64_t, uint8_t) {}
inline void markExecutorThread() {}
inline constexpr bool onExecutorThread() { return false; }
inline void noteEffectPushed(uint8_t, uint32_t) {}
inline void noteSync(uint64_t, bool) {}
inline void syncReason(uint32_t) {}
inline void noteDrainCall(uint32_t) {}
inline void report(const char*) {}
inline constexpr bool profOn() { return false; }
inline constexpr uint64_t profNow() { return 0u; }
inline void profExecIter(uint64_t, uint64_t, uint32_t) {}
inline void profExecIdle(uint64_t, uint64_t, bool) {}
inline void profReserveWait(uint64_t) {}
inline void profPlace(int) {}
#endif

}  // namespace sr::native::exec
