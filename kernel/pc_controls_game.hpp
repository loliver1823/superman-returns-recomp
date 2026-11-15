#pragma once
// iter-464 controls lane: what the PC control scheme reads from the GAME, and the one thing it
// writes (mouse-look on foot, a port feature). docs/PC_CONTROLS.md sections 3, 4 and 6.
// Kernel-only header.
#include "pc_controls_map.hpp"

namespace sr::kernel::pccontrols {

struct GameView {
  bool valid = false;        // the input-context reader found the IgcContextManager
  Context ctx = Context::Unknown;
  int igcType = -1;          // player 1's requested input-context type (-1 = no reading)
  int config = 0;            // controller configuration 0-3 (A-D) of player 1's slot
  bool invertX = false;      // the game's X INVERSION option as applied to player 1
  bool invertY = false;      // the game's Y INVERSION option
  bool air = false;          // Superman's flight mover ran last tick
  bool hover = false;        // ... in its hover slot
  bool footCam = false;      // the on-foot orbit camera ran last tick (mouse-look on foot)
  float flyYawRate = 0, flyPitchRate = 0;   // the flight's full-deflection rates now (rad/s)
  DodgeFrame dodge;          // camera + facing frame for the ground evade
  uint32_t poll = 0;         // this poll's sequence number
};
// Called once per pad poll on the polling (sim) thread. Observe-only reads.
GameView readGameView();

// SR_PC_MOUSELOOK: the on-foot camera takes the mouse as angles (default ON for players; it
// writes only while the mouse moves - automated runs have no real mouse). =0 off.
bool footMouseLookEnabled();
// The poll's mouse-look for the on-foot camera, in radians (+yaw / +pitch in the game's own
// sense after its inversion options). Consumed by the camera's next tick (this sim tick).
void publishFootLook(float dYaw, float dPitch, uint32_t poll);

}  // namespace sr::kernel::pccontrols
