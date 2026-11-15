// iter-464 controls lane: the game-state readers and hooks of the PC control scheme.
// docs/PC_CONTROLS.md sections 3 (context), 4 (mouse-look), 5 (Dodge frame), 6 (air moves).
//
// Contracts [V] (IDA + golden dumps, docs/PC_CONTROLS.md 2-6):
//   *0x8296D884 = IgcContextManager: +28 slot count, +32 slots (20 B each: +0 context, +4 current
//     type, +8 requested type (0-3 InGameConfigA-D, 4 Interface, 5 Mim, 6 Reveal, 7 Debug,
//     8 Igc), +16 invertX applied, +17 invertY applied). *0x8296E8C8 + 12 = the active pad index.
//   On-foot camera (vt 0x82005DC8; OnFootCamera_Tick_sub_821855B8 = vt slot 4, r3 = camera):
//     +32 position, +96 follow point, +64 follow reference, +160 pitch, +164 yaw (rad),
//     +192 idle timer, +628/+632 pitch min/max, +900 the followed actor (Superman).
//   LocoLoco_Update_sub_8221D2C8 / LocoFly_Update_sub_82224A90 (vt slot 2 of the LOCO / FLY
//     movers, r3 = mover): mover +0x64 = owner (vt 0x82008958), owner +4 = the actor.
//   Actor (vt 0x82009798): +0x10 rotation quaternion (x, y, z, w), +0x84 -> +0x24 name.
//   Flight move records: def +0xC u16[5] record indices (boost, brake, hover, land, default) =
//     mover slot +0x90; pool *0x8296E06C, 128 B each: +0x40/+0x44 rotVel threshold min/max,
//     +0x48/+0x4C yaw rate/max, +0x50/+0x54 pitch rate/max (rad/s).
// Oracle retains the flat-reader precondition. Release PC features use scoped
// native arena admission for every range, excluding protected hardware/diagnostics.
// The hooks call the game's own function once, unconditionally.
#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "collision_watch.hpp"
#include "pc_guest_access.hpp"
#include "pc_controls_game.hpp"
#include "pc_moves.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

extern "C" void __imp__sub_821855B8(PPCContext& __restrict ctx, uint8_t* base);  // OnFootCamera_Tick
extern "C" void __imp__sub_8221D2C8(PPCContext& __restrict ctx, uint8_t* base);  // LocoLoco_Update
extern "C" void __imp__sub_82224A90(PPCContext& __restrict ctx, uint8_t* base);  // LocoFly_Update

namespace sr::kernel::pccontrols {

namespace {
using pcguest::Mem;

constexpr uint32_t kIgcMgrPtr = 0x8296D884u;
constexpr uint32_t kActivePadPtr = 0x8296E8C8u;
constexpr uint32_t kOnFootCamVt = 0x82005DC8u;
constexpr uint32_t kFlyVt = 0x8200ED88u;
constexpr uint32_t kLocoVt = 0x8200EDF4u;
constexpr uint32_t kOwnerVt = 0x82008958u;
constexpr uint32_t kActorVt = 0x82009798u;
constexpr uint32_t kMoveDefTable = 0x8296DF58u;
constexpr uint32_t kMoveParamPool = 0x8296E06Cu;
constexpr float kPi = 3.14159265358979f;

std::atomic<int> g_readable{-1};
std::atomic<uint32_t> g_poll{0};
// published by the hooks (the sim thread; the poll runs earlier in the same sim tick)
std::atomic<uint32_t> g_camPoll{0}, g_cam{0};
std::atomic<uint32_t> g_flyPoll{0}, g_flySlot{0};
std::atomic<uint32_t> g_actor{0};
std::atomic<float> g_flyYaw{0}, g_flyPitch{0};
// on-foot mouse-look: published by the poll, consumed by this tick's camera update
std::atomic<float> g_pendYaw{0}, g_pendPitch{0};
std::atomic<uint32_t> g_pendPoll{0};
std::atomic<uint32_t> g_lookWrites{0};

bool hooksOn(uint8_t* base) {
  int r = g_readable.load(std::memory_order_relaxed);
  if (r < 0) {
    if (!enabled()) { g_readable.store(0); return false; }
    char why[200] = {};
    r = pcguest::ready(base, why, sizeof why) ? 1 : 0;
    g_readable.store(r);
    SR_LOG_BOOT(r ? sr::LogSeverity::Info : sr::LogSeverity::Warn,
                "[PC_CONTROLS] game readers %s (%s)", r ? "ON" : "OFF", why);
  }
  return r == 1;
}

bool isSuperman(const Mem& m, uint32_t actor) {
  if (!actor || m.u32(actor) != kActorVt) return false;
  if (actor == g_actor.load(std::memory_order_relaxed)) return m.ok();
  const uint32_t s = m.u32(uint64_t(m.u32(uint64_t(actor) + 0x84)) + 0x24);
  static const char kName[] = "SUPERMAN";
  if (!collwatch::inWindow(s, 9)) return false;
  for (int i = 0; i < 8; ++i)
    if (m.u8(uint64_t(s) + i) != static_cast<uint8_t>(kName[i])) return false;
  if (m.u8(uint64_t(s) + 8) != 0) return false;
  if (!m.ok()) return false;
  g_actor.store(actor, std::memory_order_relaxed);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_CONTROLS] Superman actor=0x%08X", actor);
  return m.ok();
}

uint32_t actorOfMover(const Mem& m, uint32_t mover) {
  const uint32_t owner = m.u32(uint64_t(mover) + 0x64);
  return m.u32(owner) == kOwnerVt ? m.u32(uint64_t(owner) + 4) : 0;
}

float pcCamLoadBEf(const Mem& m, uint64_t va) { return m.f32(va); }
bool pcCamStoreBEf(const Mem& m, uint64_t va, float value) { return m.putFloat(va, value); }

// The flight's full-deflection yaw / pitch rates for the mover's current record and speed
// (sub_8220E0A0 / sub_8220D230: lerp(rate, rateMax) over rotVel threshold min..max).
void publishFlightRates(const Mem& m, uint32_t mover, uint32_t actor) {
  const uint32_t tbl = m.u32(kMoveDefTable);
  const uint32_t defIdx = m.u32(uint64_t(mover) + 0x60), slot = m.u32(uint64_t(mover) + 0x90);
  if (!tbl || defIdx >= m.u32(uint64_t(tbl) + 4) || slot > 4) return;
  const uint32_t def = m.u32(uint64_t(m.u32(tbl)) + 4ull * defIdx);
  const uint32_t hw = m.u32(uint64_t(def) + 0x0C + 2 * (slot & ~1u));
  const uint32_t rec = (slot & 1) ? (hw & 0xFFFF) : (hw >> 16);
  if (rec == 0xFFFF) return;
  const uint64_t r = uint64_t(m.u32(kMoveParamPool)) + uint64_t(rec) * 0x80;
  float v[3];
  m.vec3(uint64_t(m.u32(uint64_t(m.u32(uint64_t(actor) + 0x70)) + 4)) + 0x10, v);
  const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  const float lo = m.f32(r + 0x40), hi = m.f32(r + 0x44);
  float t = hi > lo ? (speed - lo) / (hi - lo) : 1.0f;
  t = t < 0 ? 0 : (t > 1 ? 1 : t);
  const float yaw = m.f32(r + 0x48) + t * (m.f32(r + 0x4C) - m.f32(r + 0x48));
  const float pitch = m.f32(r + 0x50) + t * (m.f32(r + 0x54) - m.f32(r + 0x50));
  if (!m.ok()) return;
  if (yaw > 0.01f && yaw < 20.0f) g_flyYaw.store(yaw, std::memory_order_relaxed);
  if (pitch > 0.01f && pitch < 20.0f) g_flyPitch.store(pitch, std::memory_order_relaxed);
}

void unit2(float* v) {
  const float n = std::sqrt(v[0] * v[0] + v[1] * v[1]);
  if (n > 1e-4f) { v[0] /= n; v[1] /= n; } else { v[0] = 0; v[1] = 0; }
}
}  // namespace

bool footMouseLookEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PC_MOUSELOOK");
    return !e || e[0] != '0';   // default ON: it writes only while a mouse moves
  }();
  return v;
}

void publishFootLook(float dYaw, float dPitch, uint32_t poll) {
  g_pendYaw.store(dYaw, std::memory_order_relaxed);
  g_pendPitch.store(dPitch, std::memory_order_relaxed);
  g_pendPoll.store(poll, std::memory_order_release);
}

// docs/PC_CONTROLS.md 5.2: the camera's right on the ground plane (stick +X) for its forward
// (fx, fz) is (-fz, fx) in the game's right-handed y-up world ([V] golden: the on-foot camera's
// yaw +164 = atan2(fwd.x, fwd.z) = 51.57 deg = Superman's facing, local +Z forward).
static float g_camRightSign = 1.0f;

GameView readGameView() {
  GameView v;
  v.poll = g_poll.fetch_add(1, std::memory_order_relaxed) + 1;
  uint8_t* base = getGuestBase();
  if (!base || !hooksOn(base)) return v;
  const Mem m{base};
  const uint32_t mgr = m.u32(kIgcMgrPtr);
  const uint32_t n = m.u32(uint64_t(mgr) + 28), slots = m.u32(uint64_t(mgr) + 32);
  if (!mgr || !slots || n == 0 || n > 8) return v;
  uint32_t pad = 0;
  if (const uint32_t ap = m.u32(kActivePadPtr)) {
    const uint32_t p = m.u32(uint64_t(ap) + 12);
    if (p < n) pad = p;
  }
  const uint64_t slot = uint64_t(slots) + 20ull * pad;
  const uint32_t type = m.u32(uint64_t(slot) + 8);
  v.valid = true;
  v.igcType = type <= 8 ? static_cast<int>(type) : -1;
  if (type <= 3) { v.ctx = Context::Gameplay; v.config = static_cast<int>(type); }
  else if (type == 5) { v.ctx = Context::MiniGame; v.config = static_cast<int>(m.u32(uint64_t(slot) + 12) & 3); }
  else if (type == 8) v.ctx = Context::Cinematic;
  else if (type == 4 || type == 6 || type == 7) v.ctx = Context::Menu;
  else v.ctx = Context::Unknown;
  v.invertX = m.u8(uint64_t(slot) + 16) != 0;
  v.invertY = m.u8(uint64_t(slot) + 17) != 0;

  // what ran LAST tick (the hooks stamp the poll sequence; this poll is one later)
  v.air = v.poll - g_flyPoll.load(std::memory_order_relaxed) <= 2;
  v.hover = v.air && g_flySlot.load(std::memory_order_relaxed) == 2;
  v.footCam = !v.air && v.poll - g_camPoll.load(std::memory_order_relaxed) <= 2;
  v.flyYawRate = g_flyYaw.load(std::memory_order_relaxed);
  v.flyPitchRate = g_flyPitch.load(std::memory_order_relaxed);

  // the Dodge frame: the on-foot camera's view direction and Superman's facing
  const uint32_t cam = g_cam.load(std::memory_order_relaxed);
  const uint32_t actor = cam ? m.u32(uint64_t(cam) + 900) : 0;
  if (v.footCam && cam && m.u32(cam) == kOnFootCamVt && isSuperman(m, actor)) {
    float cp[3], fp[3], q[4];
    m.vec3(uint64_t(cam) + 32, cp);
    m.vec3(uint64_t(cam) + 96, fp);
    for (int i = 0; i < 4; ++i) q[i] = m.f32(uint64_t(actor) + 0x10 + 4 * i);
    DodgeFrame& f = v.dodge;
    f.camFwd[0] = fp[0] - cp[0];
    f.camFwd[1] = fp[2] - cp[2];
    unit2(f.camFwd);
    // right-handed, y up: right = (-fz, fx) for forward (fx, fz); g_camRightSign calibrates
    f.camRight[0] = -f.camFwd[1] * g_camRightSign;
    f.camRight[1] = f.camFwd[0] * g_camRightSign;
    const float lz[3] = {0, 0, 1}, lx[3] = {1, 0, 0};
    float wf[3], wr[3];
    pcmoves::quatRotate(q, lz, wf);
    pcmoves::quatRotate(q, lx, wr);
    f.face[0] = wf[0]; f.face[1] = wf[2];
    f.faceRight[0] = wr[0] * pcmoves::g_localRightSign;
    f.faceRight[1] = wr[2] * pcmoves::g_localRightSign;
    unit2(f.face);
    unit2(f.faceRight);
    f.valid = (f.camFwd[0] != 0 || f.camFwd[1] != 0) && (f.face[0] != 0 || f.face[1] != 0);
  }
  if (!m.ok()) { GameView refused; refused.poll = v.poll; return refused; }
  return v;
}

}  // namespace sr::kernel::pccontrols

namespace PC = sr::kernel::pccontrols;

// ── OnFootCamera_Tick (vt 0x82005DC8 slot 4): r3 = the on-foot orbit camera, f1 = dt ─────────
// Observe: stamps that the on-foot camera runs (mouse-look target, Dodge frame).
// PORT FEATURE (SR_PC_MOUSELOOK, default ON; writes only when the poll published mouse motion
// for THIS tick): adds the mouse's yaw / pitch to the camera's own angles, exactly as the
// game's own input path does for a stick (sub_82160690: +164 yaw, +160 pitch clamped to
// [+628, +632], idle timer +192 = 0, follow reference +64 = +96), but 1:1 - no rate cap, no
// hold ramp (CameraAxisInputSmoothUpdate_sub_82160A70). Then the game's tick runs unchanged.
PPC_FUNC(sub_821855B8) {
  const uint32_t cam = ctx.r3.u32;
  if (PC::hooksOn(base)) {
    const PC::Mem m{base};
    if (m.u32(cam) == PC::kOnFootCamVt) {
      const uint32_t poll = PC::g_poll.load(std::memory_order_relaxed);
      PC::g_camPoll.store(poll, std::memory_order_relaxed);
      PC::g_cam.store(cam, std::memory_order_relaxed);
      if (PC::footMouseLookEnabled() &&
          PC::g_pendPoll.load(std::memory_order_acquire) == poll && poll != 0) {
        const float dy = PC::g_pendYaw.exchange(0.0f, std::memory_order_relaxed);
        const float dp = PC::g_pendPitch.exchange(0.0f, std::memory_order_relaxed);
        PC::g_pendPoll.store(0, std::memory_order_relaxed);
        // every field touched lies in [cam, cam + 0x280): inside the readable window
        if ((dy != 0.0f || dp != 0.0f) && sr::kernel::collwatch::inWindow(cam, 0x280) && m.range(cam, 0x280)) {
          float yaw = PC::pcCamLoadBEf(m, uint64_t(cam) + 164) + dy;
          while (yaw > PC::kPi) yaw -= 2.0f * PC::kPi;
          while (yaw < -PC::kPi) yaw += 2.0f * PC::kPi;
          float pitch = PC::pcCamLoadBEf(m, uint64_t(cam) + 160) + dp;
          const float lo = PC::pcCamLoadBEf(m, uint64_t(cam) + 628), hi = PC::pcCamLoadBEf(m, uint64_t(cam) + 632);
          if (lo < hi) pitch = pitch < lo ? lo : (pitch > hi ? hi : pitch);
          PC::pcCamStoreBEf(m, uint64_t(cam) + 164, yaw);
          PC::pcCamStoreBEf(m, uint64_t(cam) + 160, pitch);
          PC::pcCamStoreBEf(m, uint64_t(cam) + 192, 0.0f);
          m.copy(uint64_t(cam) + 64, uint64_t(cam) + 96, 16);
          const uint32_t w = PC::g_lookWrites.fetch_add(1, std::memory_order_relaxed) + 1;
          if (w <= 3 || w % 3000 == 0)
            SR_LOG_BOOT(sr::LogSeverity::Info,
                        "[PC_CONTROLS] mouse-look on foot #%u: yaw %+.4f pitch %+.4f rad -> "
                        "(%.3f, %.3f)", w, dy, dp, yaw, pitch);
        }
      }
    }
  }
  __imp__sub_821855B8(ctx, base);
}

// ── LocoLoco_Update (LOCO mover vt 0x8200EDF4 slot 2): r3 = mover. Observe only. ────────────
PPC_FUNC(sub_8221D2C8) {
  if (PC::hooksOn(base)) {
    const PC::Mem m{base};
    const uint32_t mover = ctx.r3.u32;
    if (m.u32(mover) == PC::kLocoVt) (void)PC::isSuperman(m, PC::actorOfMover(m, mover));
  }
  __imp__sub_8221D2C8(ctx, base);
}

// ── LocoFly_Update (FLY mover vt 0x8200ED88 slot 2): r3 = mover, f1 = dt ────────────────────
// Observe: Superman is airborne (+ hover slot, flight rates for the mouse-look stick law).
// PORT FEATURE (SR_PC_MOVES, default OFF): a pending Dodge request starts a new air move
// through the game's own nudge (kernel/pc_moves.cpp), BEFORE the game's body consumes it.
PPC_FUNC(sub_82224A90) {
  if (PC::hooksOn(base)) {
    const PC::Mem m{base};
    const uint32_t mover = ctx.r3.u32;
    if (m.u32(mover) == PC::kFlyVt) {
      const uint32_t actor = PC::actorOfMover(m, mover);
      if (PC::isSuperman(m, actor)) {
        const uint32_t poll = PC::g_poll.load(std::memory_order_relaxed);
        const uint32_t slot = m.u32(uint64_t(mover) + 0x90);
        if (m.ok()) {
          PC::g_flyPoll.store(poll, std::memory_order_relaxed);
          PC::g_flySlot.store(slot, std::memory_order_relaxed);
          PC::publishFlightRates(m, mover, actor);
          sr::kernel::pcmoves::onSupermanFly(ctx, base, mover, actor, poll);
        }
      }
    }
  }
  __imp__sub_82224A90(ctx, base);
}
