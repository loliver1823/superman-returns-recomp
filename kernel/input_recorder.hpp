#pragma once
// Input recording/replay for XamInputGetState.
//
// Controlled via environment variables (set before launching the harness):
//   SR_INPUT_RECORD=<path>   — record all input to a .inpr file (version 2)
//   SR_INPUT_REPLAY=<path>   — replay input from a .inpr file (version 1 or 2)
//
// Version 1 (tools/gen_input_tape.py): FileHeader + N * GamepadState, one entry
// per XamInputGetState poll, replayed by poll count from boot.
// Version 2 (iter-462, what SR_INPUT_RECORD writes): FileHeader + N * TimedEntry.
// Each entry carries the host milliseconds since the ANCHOR - the guest's own
// request of the in-game HUD package ("screens/hud/main_hud"), i.e. the start
// of gameplay - or, before the anchor, since boot with `afterAnchor` clear.
// Replay of a v2 tape passes input through (controller, keyboard, sr_drive)
// until the anchor fires, then serves the recorded state for the elapsed time
// since it, and neutral input after the tape ends. The game's simulation runs
// on elapsed time, so a time-indexed tape survives a different frame rate
// better than a poll-indexed one; it is still a replay against a
// non-deterministic game, not a guarantee.
// Host-only tool: nothing here writes guest memory except through the normal
// XamInputGetState result, exactly as a controller would.

#include <cstdint>
#include <string>

namespace input_recorder {

enum class Mode { Off, Record, Replay };

struct GamepadState {
    uint16_t wButtons;
    uint8_t  bLeftTrigger;
    uint8_t  bRightTrigger;
    int16_t  sThumbLX;
    int16_t  sThumbLY;
    int16_t  sThumbRX;
    int16_t  sThumbRY;
};

// Call once at startup after Log::init().
void init();

// Call once at shutdown before Log::shutdown().
void shutdown();

// Replay: call from the XamInputGetState stub BEFORE reading the controller.
//   Returns true if `state` was overwritten from the tape (the caller skips the
//   controller and keyboard). Record/Off: returns false and does nothing.
bool replayTick(uint32_t callNum, GamepadState& state);

// Record: call AFTER the controller + keyboard merge with the final state.
void recordTick(uint32_t callNum, const GamepadState& state);

// The gameplay anchor (the guest requested the main HUD). Idempotent.
void anchor();

Mode getMode();

} // namespace input_recorder
