// sub_82176300 override (observe wrapper around the game's MAIN LOOP).
//
// sub_82176300 is `while(*(gameState+0x18)==0) sub_828A7978();` - the game loop,
// +0x18 its QUIT flag (iter-294). The iter-2x "init3-wait bypass" that set the
// flag and returned (skipping the loop and running GameShutdown) and its env
// arm SR_RUN_REAL_GAMELOOP=0 were deleted in iter-463 (charter: the =0 mode
// wrote the quit flag for the guest). The real loop always runs.

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "../app/log.hpp"
#include <atomic>
#include <cstdlib>

// The REAL recompiled body of Function_82176300 (this file strong-overrides the
// weak symbol; __imp__ is always the real lifted code).
extern "C" void __imp__sub_82176300(PPCContext& __restrict ctx, uint8_t* base);

void sub_82176300(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t paramVA = ctx.r3.u32;
  static std::atomic<unsigned> s_callCount{0};
  unsigned n = s_callCount.fetch_add(1, std::memory_order_relaxed);

  // iter-294 TRUNK FIX: Function_82176300 is NOT an "init3 wait to skip" — it IS
  // the game's main loop:  while(*(param+0x18)==0) { Function_828A7978(); }  where
  // Function_828A7978 pumps a frame (worker-queue drain sub_828EE050). The sibling
  // Function_821863A0 that this bypass hands control to is the SHUTDOWN/teardown
  // (it destroys the 4 module managers DAT_8296d8e4/dc/e0/d8 via vt[5] and nulls
  // them) — the E630 walk / splash activity seen "in the game tick" was the game
  // TEARING ITSELF DOWN. So the bypass has been skipping the entire game loop and
  // running teardown. The REAL loop runs so the
  // per-frame pump actually executes (the game can advance splash -> menu). If the
  // worker pump stalls, the main thread parks IN the loop (diagnosable) rather than
  // the game shutting down.
  {
    // iter-463 charter: OBSERVE-ONLY. The host used to write 0 to the quit flag
    // [gameState+0x18] here ("stack garbage" theory). IDA: Init2 (sub_8218CFE8)
    // zeroes it itself, and every roll logged preValue=0x00, so the write was
    // inert - but it would have erased a genuine early quit. The game owns the
    // flag; we only report it.
    uint8_t preFlag = 0xFF;
    if (const uint8_t* qf = sr::kernel::guestVAToHost(paramVA + 0x18)) preFlag = *qf;
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[GAMELOOP] call#%u running REAL game loop __imp__sub_82176300 (gameState=0x%08X, quit-flag@0x%08X preValue=0x%02X, untouched)",
      n + 1, paramVA, paramVA + 0x18, preFlag);
    if (preFlag != 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[GAMELOOP] quit flag already set (0x%02X) before the loop - the game will exit it at once (guest decision, not cleared)",
        preFlag);
    __imp__sub_82176300(ctx, base);
    // Reaching here means the game set its quit flag — i.e. the game itself decided
    // to exit (or a spurious quit). Log it; do NOT fall through to the old bypass.
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[GAMELOOP] REAL game loop RETURNED (quit flag set) gameState=0x%08X", paramVA);
    return;
  }
}
