// iter-464 bgruns: AUTOMATED RUNS NEVER TAKE THE FOREGROUND (host test infrastructure).
//
// The user works on the same PC while rolls run. Before this, every automated roll brought the
// game window to the front: SDL activated it on creation, and tools/sr_drive.ps1 had to
// SetForegroundWindow + SendInput before every key because the keyboard-as-gamepad merge
// (kernel/stubs.cpp mergeKeyboardPad, iter-450S) is focus-gated by design.
//
// THE POLICY (computed once from the environment; players set none of these):
//   automated        = SR_INSTANCE_ID is non-empty. Every roll harness sets it (run_recomp_x,
//                      sr_drive, replay_ab, hle_replay, devtools/skip_run, native_replay_check);
//                      a player never does (docs/MODS.md, kernel/devtools.hpp use the same rule).
//   windowBackground = SR_WINDOW_BACKGROUND (1/0), else `automated` OR a trace-replay mode
//                      (SR_GPU_TRACE_REPLAY / SR_HLE_TRACE_REPLAY: always tool-driven - replay_ab,
//                      hle_replay, sweep_driver, the mutation tests). The window is shown without
//                      activation (SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN) and placed at the
//                      BOTTOM of the z-order; nothing raises or activates it later. The
//                      process opts out of Windows 11 background throttling (an occluded
//                      window loses its 1 ms timer resolution and may run at EcoQoS), so a
//                      background roll runs like a foreground one.
//   harnessInput     = SR_HARNESS_INPUT (1/0), else `automated`. A focus-independent keyboard: the
//                      named file mapping Local\SR_HARNESS_INPUT_<instance> holds 512 SDL-scancode
//                      bytes the harness writes (nonzero = held). mergeKeyboardPad reads it through
//                      the SAME mapping table it applies to SDL's keyboard state, so a harness key
//                      reaches the guest exactly as that physical key would - no focus needed.
//   realInput        = SR_HARNESS_REAL_INPUT (1/0), else NOT `automated`. INPUT ISOLATION: an
//                      automated run ignores the real host keyboard (incl. the F9 trace hotkey)
//                      and real XInput pads, so a background roll never reacts to the user's own
//                      typing or controller. sr_drive -ForegroundInput sets it to 1.
//
// ⛔ NOT FORCING: nothing here writes guest memory, calls guest code or synthesises input. The
// harness channel is a keyboard: the guest sees keys only while the harness holds them, through
// the unchanged XamInputGetState path. It exists only on automated runs.
#pragma once

#include <cstdint>
#include <string>

namespace sr::kernel::automation {

struct Policy {
  bool automated = false;
  bool windowBackground = false;
  bool harnessInput = false;
  bool realInput = true;
};

// Pure (tests drive it): each argument is the env value, nullptr = unset. `traceReplay` = the
// process replays a GPU/HLE trace instead of booting the game.
Policy computePolicy(const char* instanceId, bool traceReplay, const char* windowBackground,
                     const char* harnessInput, const char* realInput);

// The process-wide policy, computed once from the environment.
const Policy& policy();

// iter-465 player: THE PORT-FEATURE SWITCH (docs/PLAYER_BUILD.md). The player-facing port
// features (front end, autosave, high frame rate, resolution scale auto, device glyphs, the
// costume picker and its cloth capacity, ...) are ON for a player - the exe started with no
// SR_INSTANCE_ID - and OFF for automated / natural rolls. SR_PORT_FEATURES overrides both ways:
// =0 turns them off for a player (A/B), =1 turns them on in an automated roll (a PORT FEATURE
// roll, tagged natural=false). A feature's own gate (SR_HIGHFPS, SR_RES_SCALE, SR_GLYPHS, ...)
// still wins when it is set. Pure (tests drive it): nullptr = unset.
bool portFeaturesFor(const char* instanceId, const char* portFeatures);
// The process-wide answer, computed once from the environment.
bool portFeaturesOn();

// "Local\SR_HARNESS_INPUT_<id>" with the id sanitised to [A-Za-z0-9_.-] ("default" when empty).
std::string harnessChannelName(const char* instanceId);

// Channel layout v1 (little-endian host; the harness side lives in tools/sr_drive_core.ps1).
constexpr uint32_t kHarnessMagic = 0x49485253u;   // "SRHI"
constexpr uint32_t kHarnessVersion = 1;
constexpr uint32_t kHarnessOffMagic = 0x00;
constexpr uint32_t kHarnessOffVersion = 0x04;
constexpr uint32_t kHarnessOffPid = 0x08;
constexpr uint32_t kHarnessOffPollCount = 0x0C;   // merges that read the channel
constexpr uint32_t kHarnessOffLastMask = 0x10;    // pad mask the channel's keys mapped to, last merge
constexpr uint32_t kHarnessOffKeys = 0x40;        // uint8_t keys[512], SDL scancode-indexed
constexpr uint32_t kHarnessKeyCount = 512;
constexpr uint32_t kHarnessMapBytes = 4096;

// Once, early (main, before the window): with windowBackground, opts the process out of
// Windows' background throttling (HighQoS + timer resolution honoured while occluded;
// SR_BG_HIGH_QOS=0 skips it); with harnessInput, creates the channel mapping (idempotent,
// zeroes the keys, stamps magic/version/pid). Logs one [AUTOMATION] line either way.
void initAutomation();
// The 512 scancode bytes, or nullptr when the channel is off / could not be created.
const uint8_t* harnessKeys();
// Book-keeping for the harness: one merge read the channel and mapped it to `mask`.
void harnessNoteMerge(uint16_t mask);

// The iter-450S keyboard->pad mapping (reference model), shared by both keyboard sources:
// Enter=START, Esc/Backspace=BACK, arrows=DPAD, Q/E=LB/RB, Z/Space=A, X=B, C=X, V=Y,
// WASD=left stick. `keys` is SDL-scancode-indexed. Returns the button mask; writes the stick
// only for a held W/S (LY) or A/D (LX) key.
// iter-464 controls lane: this is now the harness's TEST keymap (tools/sr_drive.ps1 key names);
// players get kernel/pc_controls.hpp's layout.
uint16_t mapKeysToPad(const uint8_t* keys, int16_t* thumbLX, int16_t* thumbLY, int16_t* thumbRX = nullptr,
                      int16_t* thumbRY = nullptr);   // I/J/K/L = right stick (iter-465)

// ── iter-464 controls lane: the channel's additive extension (docs/PC_CONTROLS.md 8) ─────────
// The layout version stays 1 (tools/sr_drive_core.ps1 checks it); a harness that never writes
// these offsets sees the v1 behaviour exactly (the flags read 0 = test keymap, no mouse).
constexpr uint32_t kHarnessOffFlags = 0x14;      // u32, harness-written
constexpr uint32_t kHarnessFlagPlayerKeymap = 1u;  // the channel's keys use the PLAYER keymap
constexpr uint32_t kHarnessFlagMouse = 2u;         // the mouse block below is live
// iter-467 hooks: the harness's DUMP MARK (tools/controls/harness_play.py `mark dump`). With
// SR_PRESENT_DUMP_FROM_MARK=1 the every-present dump window opens at the first present after it
// (gfx/d3d12_backend.cpp dumpBackBuffer) - a place in the game, not a present number.
constexpr uint32_t kHarnessFlagDumpMark = 4u;
constexpr uint32_t kHarnessOffMouse = 0x300;     // i32 dxTotal, i32 dyTotal, i32 wheelTotal, u32 buttons
constexpr uint32_t kHarnessOffReadback = 0x320;  // 24 B, game-written (pc_controls readback)
constexpr uint32_t kHarnessReadbackBytes = 24;
struct HarnessMouse {
  int32_t dxTotal = 0, dyTotal = 0, wheelTotal = 0;  // cumulative (consumers diff them)
  uint32_t buttons = 0;
};
// The channel's flags (0 when the channel is off).
uint32_t harnessFlags();
// The mouse block (false when the channel is off or kHarnessFlagMouse is clear).
bool harnessMouse(HarnessMouse* out);
// Publishes the game's read-back block (no-op when the channel is off).
void harnessWriteReadback(const void* bytes, uint32_t n);

}  // namespace sr::kernel::automation
