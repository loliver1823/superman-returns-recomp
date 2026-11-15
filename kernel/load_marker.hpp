#pragma once

// step 4 (2026-10-20, Fable ruling 12: the stuck load closes as a KNOWN ISSUE): every load screen leaves a
// BEGIN / DONE pair in EVERY log profile, so a player log shows a load that never finished as a [LOAD_BEGIN] with
// no [LOAD_DONE] after it.
//
//   [LOAD_BEGIN] #<n> mode=<SHIELD|BIZARRO|ENGAGING> lr=<guest caller>
//       the load screen goes up: FmuiEnterLoadingScreen_sub_8214F1B0 (the Lua native ShowLoadingScreen) turned the
//       holder's shown byte (*(ui+0x54)+0x18) from 0 to non-zero
//   [LOAD_DONE] #<n> hidden <s> s after [LOAD_BEGIN] #<b> lr=<guest caller>
//       the load is over: FmuiHideLoadingScreenWorker_sub_8214E788 (the Lua native HideLoadingScreen, also bound as
//       Show / HideObjectiveLoadingScreen) turned the shown byte from non-zero to 0 (and set the sequencer state 7)
//   [LOAD_OFF] #<n> screen off <s> s after [LOAD_BEGIN] #<b> (the pause / START gate opens)
//       the load screen's active flag (LoadScreenRenderObj +0x44, the FrontendUpdateGate term 'v1+0x44') went from
//       set to clear in its vtable+0x24 sub_82317728, called by the sequencer's state 7 on the next tick
// Every marker is a TRANSITION (the flag read before and after the game's call): a call that changes nothing writes
// nothing, so a vtable slot shared by other objects or a per-frame call cannot flood the log. Each tag also has a
// backstop cap (the first 512 lines, then powers of two).
//
// A stuck load (step 4, lane D's fe_reload12c_b load 10: the HUD up, the world never drawn, START ignored) is
// expected to show BEGIN without DONE (HideLoadingScreen never reached) or DONE without OFF (the sequencer stopped).
//
// Log-only (docs/OVERRIDES.json class observe): the overrides read guest memory, call the game's body exactly once,
// unconditionally, and write nothing the guest sees. The tags are kMilestoneTags (app/log.cpp), kept by the player
// profile; a few lines per load. Release builds included.

#include <cstdint>

namespace sr::kernel::loadmark {

const char* modeName(uint32_t mode);   // FmuiEnterLoadingScreen's r4: 0 SHIELD, 1 BIZARRO, 2 ENGAGING

// The line writers. Each returns its event's 1-based count; a line is written for counts 1-512, then for every
// power of two (a backstop: the player profile keeps milestone tags whole); never for count 0 (a wrapped counter).
// [LOAD_DONE] / [LOAD_OFF] name the latest [LOAD_BEGIN] by one snapshot of its id and time (beginSnapshot).
uint32_t noteBegin(uint32_t mode, uint32_t lr);
uint32_t noteDone(uint32_t lr);
uint32_t noteOff(uint32_t lr);
bool logged(uint32_t n);   // the cap rule: is count n written?

// The latest [LOAD_BEGIN]'s count and steady-clock time (ns; 0 = none yet), published together by noteBegin.
struct BeginSnapshot {
  uint32_t id;
  int64_t ns;
};
BeginSnapshot beginSnapshot();

// The overrides' decisions: each flag read BEFORE and AFTER the game's own call; only a transition writes a line
// (true = counted). BEGIN: shown 0 -> non-zero; DONE: shown non-zero -> 0; OFF: +0x44 set -> clear.
bool onShown(uint8_t before, uint8_t after, uint32_t mode, uint32_t lr);
bool onHidden(uint8_t before, uint8_t after, uint32_t lr);
bool onScreenOff(uint8_t before, uint8_t after, uint32_t lr);

void resetForTest();
void setCountsForTest(uint32_t begin, uint32_t done, uint32_t off);   // the next event counts each + 1 (wraps)

}  // namespace sr::kernel::loadmark
