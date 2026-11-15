// iter-464 controls lane: the two new air moves (port feature, SR_PC_MOVES default OFF).
// Design + verification: docs/PC_CONTROLS.md section 6. Contracts [V] (IDA + golden dumps):
//   flight mover (vt 0x8200ED88): +0x44 nudge duration (-1 idle), +0x48 elapsed, +0x60 move-def
//   index, +0x64 owner (PMSY, vt 0x82008958; +4 = the actor), +0x90 slot (0 boost, 1 brake,
//   2 hover, 3 land, 4 default);
//   actor: +0x10 rotation quaternion (x, y, z, w), +0x20 position (feet), +0x70 PRXY
//   (+0x0E high nibble 1 = keyframed, +0x0F bit 2 = active, +4 = hkCharacterProxy, velocity +0x10);
//   move-def table *0x8296DF58 = {defs, count}; uint64_t(def) +8 = char* name.
//   Vec3ComputeNormalizedDir_sub_821EAA18(mover r3, f1 = duration, f2 = distance, r6 = &dir
//   (world, 16-byte aligned), r7 = stopOnCollide, r8 = relative, r9 = profile): the nudge start
//   that ScriptCmd_ApplyNudge_sub_8221B118 -> sub_8220A350 reaches; MoveToPointUpdate_sub_821FABC0
//   (the last step of LocoFly_Update) then sets the proxy velocity to dist/dur toward the target
//   each tick until the duration ends (or a collision, with stopOnCollide).
#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "collision_watch.hpp"
#include "pc_guest_access.hpp"
#include "pc_moves.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace sr::kernel::pcmoves {

namespace {
using pcguest::Mem;

constexpr uint32_t kMoveDefTable = 0x8296DF58u;
constexpr uint32_t kCooldownPolls = 15;      // 0.5 s at the 30 Hz sim
constexpr uint32_t kRequestLifePolls = 3;    // a request older than this is dropped
constexpr float kStrafeDashMinSpeed = 22.4f; // m/s (50 MPH, the game's airbrake threshold)

std::atomic<uint32_t> g_reqPoll{0};
std::atomic<int> g_reqStrafe{0}, g_reqThrust{0};
std::atomic<uint32_t> g_lastStart{0};
std::atomic<uint8_t> g_lastKind{kNone};

float norm3(const float v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

bool defNameIs(const Mem& m, uint32_t defIdx, const char* const* names) {
  const uint32_t tbl = m.u32(kMoveDefTable);
  if (!tbl || defIdx >= m.u32(uint64_t(tbl) + 4)) return false;
  const uint32_t def = m.u32(uint64_t(m.u32(tbl)) + 4ull * defIdx);
  const uint32_t s = m.u32(uint64_t(def) + 8);
  if (!collwatch::inWindow(s, 24)) return false;
  char buf[24];
  for (int i = 0; i < 24; ++i) buf[i] = static_cast<char>(m.u8(uint64_t(s) + i));
  buf[23] = 0;
  for (const char* const* n = names; *n; ++n)
    if (std::strcmp(buf, *n) == 0) return true;
  return false;
}

}  // namespace

bool enabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PC_MOVES");
    return e && e[0] == '1';   // DEFAULT OFF (port feature, docs/MODS.md)
  }();
  return v;
}

Kind lastKind() { return static_cast<Kind>(g_lastKind.load(std::memory_order_relaxed)); }
uint32_t lastStartPoll() { return g_lastStart.load(std::memory_order_relaxed); }

void requestAirDodge(int strafe, int thrust, uint32_t poll) {
  g_reqStrafe.store(strafe, std::memory_order_relaxed);
  g_reqThrust.store(thrust, std::memory_order_relaxed);
  g_reqPoll.store(poll ? poll : 1, std::memory_order_release);
}

void onSupermanFly(PPCContext& ctx, uint8_t* base, uint32_t mover, uint32_t actor, uint32_t poll) {
  if (!enabled()) return;
  const uint32_t rp = g_reqPoll.load(std::memory_order_acquire);
  if (!rp) return;
  g_reqPoll.store(0, std::memory_order_relaxed);   // consumed (started or refused)
  const int strafe = g_reqStrafe.load(std::memory_order_relaxed);
  const int thrust = g_reqThrust.load(std::memory_order_relaxed);
  const Mem m{base};
  const char* refuse = nullptr;
  static const char* const kDefs[] = {"SupermanFlight", "SupermanFlightLock", nullptr};
  const uint32_t slot = m.u32(uint64_t(mover) + 0x90);
  const uint32_t prxy = m.u32(uint64_t(actor) + 0x70);
  const float nDur = m.f32(uint64_t(mover) + 0x44), nEl = m.f32(uint64_t(mover) + 0x48);
  float vel[3];
  m.vec3(uint64_t(m.u32(uint64_t(prxy) + 0x04)) + 0x10, vel);
  const float speed = norm3(vel);
  float q[4];
  for (int i = 0; i < 4; ++i) q[i] = m.f32(uint64_t(actor) + 0x10 + 4 * i);
  if (poll - rp > kRequestLifePolls) refuse = "stale request";
  else if (!defNameIs(m, m.u32(uint64_t(mover) + 0x60), kDefs)) refuse = "not SupermanFlight/FlightLock (carrying, airbrake, dash ...)";
  else if (nDur > 0.0f && nEl < nDur) refuse = "a nudge is already running";
  else if (!(m.u8(uint64_t(prxy) + 0x0F) & 4) || (m.u8(uint64_t(prxy) + 0x0E) >> 4) == 1) refuse = "proxy inactive / keyframed";
  else if (poll - g_lastStart.load(std::memory_order_relaxed) < kCooldownPolls) refuse = "cooldown";
  else if (!(speed == speed) || std::fabs(norm3(q) - 1.0f) > 0.01f) refuse = "bad actor state";

  if (!m.ok()) refuse = "unavailable guest state";

  Kind kind = kNone;
  Nudge nd{};
  if (!refuse) {
    if (slot == 2 || (slot == 4 && speed <= kStrafeDashMinSpeed)) {
      kind = planHoverDodge(q, strafe, thrust, &nd) ? kHoverDodge : kNone;
      if (kind == kNone) refuse = "no direction";
    } else if (slot == 4) {
      kind = planStrafeDash(q, vel, strafe, &nd) ? kStrafeDash : kNone;
      if (kind == kNone) refuse = "strafe dash needs A or D";
    } else {
      refuse = slot == 0 ? "superspeed (no strafe by design)" : "brake / land";
    }
  }
  if (refuse) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_MOVES] refused: %s (slot=%u speed=%.1f poll=%u)",
                refuse, slot, speed, poll);
    return;
  }

  // The game's own nudge start, called the way ScriptCmd_ApplyNudge's chain calls it. The
  // direction lives in a scratch frame below the guest stack pointer; the whole register file
  // is restored afterwards, so the game's LocoFly_Update runs on exactly its own arguments.
  const PPCContext saved = ctx;
  if (ctx.r1.u32 < 0x100u) return;
  const uint32_t frame = (ctx.r1.u32 - 0x100u) & ~0xFu;
  if (!m.range(frame, 0x100u)) return;
  m.put32(frame, ctx.r1.u32);   // back chain
  const uint32_t dirVA = frame + 0x80u;
  m.putFloat(dirVA + 0, nd.dir[0]);
  m.putFloat(dirVA + 4, nd.dir[1]);
  m.putFloat(dirVA + 8, nd.dir[2]);
  m.putFloat(dirVA + 12, 0.0f);
  if (!m.ok()) return;
  ctx.r1.u64 = frame;
  ctx.r3.u64 = mover;
  ctx.f1.f64 = nd.dur;
  ctx.f2.f64 = nd.dist;
  ctx.r6.u64 = dirVA;
  ctx.r7.u64 = 1;   // stopOnCollide: hitting something ends the move
  ctx.r8.u64 = 0;   // not relative: a world target (start + dir * dist)
  ctx.r9.u64 = 0;   // profile 0: constant speed (every in-game nudge)
  sub_821EAA18(ctx, base);
  ctx = saved;

  g_lastStart.store(poll, std::memory_order_relaxed);
  g_lastKind.store(kind, std::memory_order_relaxed);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PC_MOVES] %s: dir=(%.2f %.2f %.2f) dist=%.1fm dur=%.2fs speed=%.1f slot=%u keys=(%d,%d) "
              "poll=%u",
              kind == kHoverDodge ? "HOVER DODGE" : "AIR STRAFE DASH", nd.dir[0], nd.dir[1],
              nd.dir[2], nd.dist, nd.dur, speed, slot, strafe, thrust, poll);
}

}  // namespace sr::kernel::pcmoves
