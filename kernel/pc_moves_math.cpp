// iter-464 controls lane: the pure geometry of the new air moves (kernel/pc_moves.hpp), split
// from pc_moves.cpp so difftest --pc-controls-test runs it without the guest.
#include "pc_moves.hpp"

#include <cmath>

namespace sr::kernel::pcmoves {

// [V] golden sound_gameplay (docs/PC_CONTROLS.md 6.3): the world is right-handed, y up; local +Z
// is forward (the camera init rotates (0,0,1) by the actor quaternion); Superman at yaw 51.57 deg
// has local +X = (0.62, 0, -0.78) = minus his right (-fz, fx). So his right is local -X.
float g_localRightSign = -1.0f;

namespace {
float norm3(const float v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
}  // namespace

void quatRotate(const float q[4], const float v[3], float o[3]) {
  // v' = v + 2w (q x v) + 2 q x (q x v)
  const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
  const float tx = 2.0f * (qy * v[2] - qz * v[1]);
  const float ty = 2.0f * (qz * v[0] - qx * v[2]);
  const float tz = 2.0f * (qx * v[1] - qy * v[0]);
  o[0] = v[0] + qw * tx + (qy * tz - qz * ty);
  o[1] = v[1] + qw * ty + (qz * tx - qx * tz);
  o[2] = v[2] + qw * tz + (qx * ty - qy * tx);
}

bool planHoverDodge(const float q[4], int strafe, int thrust, Nudge* out) {
  // sideways when a strafe key is held (with S: diagonally back), else straight back.
  // Superman's local frame: +Z forward ([V] the game's ram recoil nudges along -Z), +X = side.
  float local[3] = {g_localRightSign * float(strafe), 0.0f, (strafe == 0 || thrust < 0) ? -1.0f : 0.0f};
  if (strafe != 0 && thrust < 0) local[2] = -1.0f;
  float w[3];
  quatRotate(q, local, w);
  w[1] = 0.0f;   // a burst out of the hover stays level
  const float n = norm3(w);
  if (!(n > 1e-3f)) return false;
  for (int i = 0; i < 3; ++i) out->dir[i] = w[i] / n;
  out->dist = 6.0f;   // 30 m/s for 6 ticks: ~1.9x the hover top speed (15.6 m/s)
  out->dur = 0.2f;
  return true;
}

bool planStrafeDash(const float q[4], const float vel[3], int strafe, Nudge* out) {
  if (strafe == 0) return false;
  const float speed = norm3(vel);
  if (!(speed > 1e-3f)) return false;
  const float vh[3] = {vel[0] / speed, vel[1] / speed, vel[2] / speed};
  const float local[3] = {g_localRightSign * float(strafe), 0.0f, 0.0f};
  float s[3];
  quatRotate(q, local, s);
  // the side vector perpendicular to the travel direction, so the dash adds no forward speed
  const float d = s[0] * vh[0] + s[1] * vh[1] + s[2] * vh[2];
  for (int i = 0; i < 3; ++i) s[i] -= d * vh[i];
  const float sn = norm3(s);
  if (!(sn > 1e-3f)) return false;
  constexpr float T = 0.25f, L = 10.0f;   // +40 m/s sideways for 8 ticks
  float D[3];
  for (int i = 0; i < 3; ++i) D[i] = vel[i] * T + (s[i] / sn) * L;
  const float dn = norm3(D);
  for (int i = 0; i < 3; ++i) out->dir[i] = D[i] / dn;
  out->dist = dn;
  out->dur = T;
  return true;
}

}  // namespace sr::kernel::pcmoves
