#pragma once
// iter-464 controls lane: the two NEW air moves (docs/PC_CONTROLS.md section 6), a PORT FEATURE
// (docs/MODS.md "Port features"): gameplay additions the original does not have. Gated by
// SR_PC_MOVES (default OFF); natural rolls never run them. Kernel-only header.
//
//   HOVER DODGE      Dodge while hovering (or in slow flight): a 6 m burst sideways / backwards
//                    in 0.2 s, out of the hover.
//   AIR STRAFE DASH  Dodge + A/D while flying at speed: a 10 m sidestep in 0.25 s that keeps the
//                    forward velocity.
// Both are built from the game's OWN systems: the move is the game's nudge (the primitive behind
// the Lua ApplyNudge the designers use for the ram-attack recoil and Down To Earth -
// Vec3ComputeNormalizedDir_sub_821EAA18 on Superman's flight mover, consumed by the mover's own
// MoveToPointUpdate_sub_821FABC0), so velocity goes through the character proxy and the game's
// collision; the flight/hover animation blend reads the resulting local velocity (lean).
#include <cstdint>

struct PPCContext;

namespace sr::kernel::pcmoves {

bool enabled();   // SR_PC_MOVES (default OFF)

enum Kind : uint8_t { kNone = 0, kHoverDodge = 1, kStrafeDash = 2 };

// pc_controls (the pad poll): the Dodge key's press edge while airborne. strafe / thrust = the
// movement keys (-1, 0, +1; +strafe = right, +thrust = forward), in Superman's frame.
void requestAirDodge(int strafe, int thrust, uint32_t poll);

// pc_controls_game.cpp (Superman's LocoFly_Update, BEFORE the game's body): starts a pending
// request when every guard passes. Restores ctx before returning.
void onSupermanFly(PPCContext& ctx, uint8_t* base, uint32_t mover, uint32_t actor, uint32_t poll);

// The last move started (for the harness read-back): kind and the poll it started on.
Kind lastKind();
uint32_t lastStartPoll();

// ── pure helpers (difftest --pc-controls-test) ─────────────────────────────────────────────
// Rotates v by the unit quaternion q = (x, y, z, w).
void quatRotate(const float q[4], const float v[3], float out[3]);
// The nudge for each move: world direction (unit) + distance (m) + duration (s). False when the
// inputs give no valid move (e.g. a strafe dash without a strafe key).
struct Nudge { float dir[3]; float dist, dur; };
bool planHoverDodge(const float q[4], int strafe, int thrust, Nudge* out);
bool planStrafeDash(const float q[4], const float vel[3], int strafe, Nudge* out);
// Superman's right = this sign x the game's local X (-1: the world is right-handed, local +Z
// forward; docs/PC_CONTROLS.md 6.3).
extern float g_localRightSign;

}  // namespace sr::kernel::pcmoves
