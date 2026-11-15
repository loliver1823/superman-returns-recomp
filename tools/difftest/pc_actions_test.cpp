// iter-466 controls2: difftest --pc-actions-test. THE PARITY TEST of the action layer
// (kernel/pc_actions.hpp, docs/PC_CONTROLS.md 2.6).
//
// A synthetic input manager in guest memory holds a context of the game's OWN command objects,
// built by the game's own constructors (sub_82723718 EventButton, sub_82725950 EventButtonRelease,
// sub_82724D68 EventButtonMapped, sub_82723070 EventAxis, sub_82725290 EventAxisMapped,
// sub_82724800 EventAlternative, sub_82724160 EventCombo, sub_82723C28 EventDualButton) with the
// binders' own arguments (sub_8229B630 + InputCtx_BindConfigA sub_8229E308). One input sequence
// (deterministic, every control, holds of 1-12 ticks, axis values on and around the dead zone) is
// played twice:
//   PAD: the sequence is the game's InputState; each tick the game's own Update (recompiled code,
//        vtable slot 6; the composites call their children through the vtables) runs on every
//        command the way InputContext_UpdateCommands_sub_82722DB8 does, wrapped in the action
//        layer's before / after halves with an idle PC frame.
//   PC:  the InputState is idle; the same sequence is the PC's ControlFrame, published to the
//        action layer, which evaluates its mirror and merges.
// Every tick, every event id: what the consumers read (IsActive's field and the value field) must
// be identical between the two runs. A third check: in the PC run, right after beforeDispatch the
// commands hold exactly the idle pad's values (the merge is fully put back).
//
// step4/pad-triggers (2026-10-20): the same parity on the REAL pad layout. The game's pad driver
// (InputXenonPad_Update_sub_825FFD40) gives each InputState 14 DIGITAL controls as bits (+0x20,
// count +0x14) and the 2 triggers as ANALOG bytes (+0x24, count +0x18: LT / RT = controls 14 / 15);
// InputState_GetButton_sub_825F7B08 answers 14 / 15 with that 0..255 byte. The 16-bit fixture above
// never had an analog slot, so a layer that latched only the bits passed it while a real controller's
// triggers (LLOCK 44-46, POWER_ACTIVATE 59 / 60, THROW 69 / 75, the LT / RT duals) were overwritten
// with "not fired" in the game. The invariant checked: with no PC input, what the consumers read
// equals the game's own result for every command, on trigger values around both binder thresholds
// (1 and 42).
//
// Astra P2 / P3 (2026-10-20): the controller can go away. InputContext_UpdateCommands calls Update only while the
// InputState says a controller is present (+8 == 1) and the commands' Reset (vtable slot 7) otherwise. The game run
// below does the same, on a presence sequence that drops the controller for 1-25 ticks (from the context's first
// tick, and with triggers / buttons held across the gap): what the consumers read equals the game's own, every tick,
// for a controller, for keys, and for a controller and keys at once (the game's run on the merged input: buttons
// OR'd, the larger trigger pull, the PC's axis when its magnitude is at least the pad's). A layout the pad driver
// never builds (analog bytes null, or n + na wrapping in 32 bits) is rejected: nothing is written that tick and the
// mirrors reseed from the game's result.
#include "../../kernel/pc_actions.hpp"
#include "../../kernel/guest_context.hpp"
#include "ppc/ppc_recomp_shared.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace K = sr::kernel;
namespace PA = sr::kernel::pcactions;

namespace {
constexpr uint32_t kArena = 0x90B00000u, kArenaSize = 0x20000u;
constexpr uint32_t kMgr = kArena, kMapInfo = kArena + 0x100, kPad = kArena + 0x200;
constexpr uint32_t kCtx = kArena + 0x300, kArr = kArena + 0x400;
constexpr uint32_t kState = kArena + 0x1000, kBits = kArena + 0x1100, kAxes = kArena + 0x1200;
constexpr uint32_t kAnalog = kArena + 0x1300;   // the InputState's analog bytes (the triggers)
constexpr uint32_t kObjs = kArena + 0x2000, kSp = kArena + kArenaSize - 0x200;
constexpr uint32_t kInputMgrPtr = 0x829A2C04u, kActivePadPtr = 0x8296E8C8u;
constexpr uint32_t kCount = 192;

struct Vt { uint32_t va; uint32_t slots[10]; };
// The eight class vtables (slots 0-9, golden sound_gameplay; difftest has no .rdata image).
const Vt kVts[] = {
    {0x8208CE5Cu, {0x827236B8, 0x82723130, 0x82723138, 0x82723150, 0x82723160, 0x82723188, 0x82723518, 0x827231F0, 0x82723600, 0x8231C140}},
    {0x8208CF34u, {0x82723BC8, 0x827237B8, 0x827237C0, 0x827237C8, 0x827237D0, 0x827237F8, 0x82723A60, 0x82723838, 0x82723B20, 0x82723850}},
    {0x8208D00Cu, {0x82724100, 0x82723CF8, 0x82723D00, 0x82723D08, 0x82723D20, 0x82723D48, 0x82723D70, 0x82723E48, 0x82724040, 0x8231C140}},
    {0x8208D0ECu, {0x82724798, 0x82724320, 0x82724328, 0x82724330, 0x82724348, 0x82724370, 0x82724660, 0x82724398, 0x82724720, 0x8231C140}},
    {0x8208D1C4u, {0x82724D00, 0x82724960, 0x82724968, 0x82724990, 0x827249B8, 0x827249E8, 0x82724A18, 0x82724AB0, 0x82724C88, 0x8231C140}},
    {0x8208D29Cu, {0x82725230, 0x82724E28, 0x82724E30, 0x82724E38, 0x82724E40, 0x82724E68, 0x827250B8, 0x82724E80, 0x82725178, 0x82724E98}},
    {0x8208D374u, {0x827258F0, 0x827253A8, 0x827253B0, 0x827253C8, 0x827253D8, 0x827253F0, 0x82725740, 0x82725408, 0x82725828, 0x8231C140}},
    {0x8208D44Cu, {0x82725DF8, 0x827259F0, 0x827259F8, 0x82725A00, 0x82725A08, 0x82725A30, 0x82725C90, 0x82725A70, 0x82725D50, 0x82725A80}},
};

int pass = 0, fail = 0;
void check(bool ok, const std::string& what) {
  if (!ok || std::getenv("SR_PCA_TEST_VERBOSE")) std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
  (ok ? pass : fail)++;
}
uint32_t r32(uint32_t va) { return PPC_LOAD_U32(va); }
void w32(uint32_t va, uint32_t v) { K::writeGuestBE32(va, v); }

uint32_t g_next = kObjs;
uint8_t* g_base = nullptr;
uint32_t alloc() { const uint32_t o = g_next; g_next += 48; std::memset(K::guestVAToHost(o), 0, 48); return o; }

PPCContext regs(uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0, uint32_t r7 = 0,
                uint32_t r8 = 0, uint32_t r9 = 0, uint32_t r10 = 0) {
  PPCContext c{};
  c.r1.u64 = kSp;
  c.r3.u64 = r3; c.r4.u64 = r4; c.r5.u64 = r5; c.r6.u64 = r6;
  c.r7.u64 = r7; c.r8.u64 = r8; c.r9.u64 = r9; c.r10.u64 = r10;
  c.lr = 0x82123450;
  return c;
}
uint32_t button(uint32_t idx, uint32_t rep, uint32_t thr) { const uint32_t o = alloc(); auto c = regs(o, idx, rep, thr); sub_82723718(c, g_base); return o; }
uint32_t release(uint32_t idx, uint32_t rep, uint32_t thr) { const uint32_t o = alloc(); auto c = regs(o, idx, rep, thr); sub_82725950(c, g_base); return o; }
uint32_t mapped(uint32_t idx, uint32_t rep, uint32_t thr, uint32_t mx, uint32_t curve) { const uint32_t o = alloc(); auto c = regs(o, idx, rep, thr, mx, curve); sub_82724D68(c, g_base); return o; }
uint32_t axis(uint32_t ax, uint32_t rep, uint32_t mode, uint32_t neg, uint32_t dz) { const uint32_t o = alloc(); auto c = regs(o, ax, rep, mode, neg, dz); sub_82723070(c, g_base); return o; }
uint32_t axisMapped(uint32_t ax, uint32_t rep, uint32_t mode, uint32_t neg, uint32_t dz, uint32_t mx, uint32_t curve) { const uint32_t o = alloc(); auto c = regs(o, ax, rep, mode, neg, dz, mx, curve); sub_82725290(c, g_base); return o; }
uint32_t alt(uint32_t a, uint32_t b) { const uint32_t o = alloc(); auto c = regs(o, a, b); sub_82724800(c, g_base); return o; }
uint32_t combo(uint32_t a, uint32_t b, uint32_t window, uint32_t rep) { const uint32_t o = alloc(); auto c = regs(o, a, b, window, rep); sub_82724160(c, g_base); return o; }
uint32_t dual(uint32_t a, uint32_t thrA, uint32_t b, uint32_t thrB, uint32_t rep) { const uint32_t o = alloc(); auto c = regs(o, a, thrA, b, thrB, rep); sub_82723C28(c, g_base); return o; }

// The game's binders' arguments for a representative context (event id -> command).
void buildContext() {
  for (uint32_t i = 0; i < kCount; ++i) w32(kArr + 4 * i, 0);
  auto put = [](uint32_t id, uint32_t cmd) { w32(kArr + 4 * id, cmd); };
  put(1, button(4, 0, 1));            // START
  put(2, button(5, 0, 1));            // BACK
  put(3, button(0, 1, 1));            // UP (held)
  put(7, button(10, 0, 1));           // A
  put(15, button(6, 1, 42));          // L3
  put(27, axis(0, 1, 2, 1, 250));     // L_XAXIS
  put(28, axis(1, 1, 2, 0, 250));     // L_YAXIS
  put(29, axis(2, 1, 2, 1, 250));
  put(30, axis(3, 1, 2, 0, 250));
  put(31, button(13, 0, 42));         // TOGGLE_MOVEMENT_MODE
  put(32, button(13, 1, 42));         // _HELD
  put(33, release(13, 0, 42));        // _RELEASED
  put(39, button(8, 1, 42));          // FLIGHT_HOVER
  put(42, button(8, 0, 42));          // FLIGHT_MANEUVER
  put(43, release(8, 0, 42));
  put(44, button(14, 0, 1));          // LLOCK_TOGGLE (LT)
  put(45, button(14, 1, 1));          // LLOCK_HOLD
  put(46, release(14, 0, 1));         // LLOCK_RELEASE
  put(47, button(7, 10, 1));          // LLOCK_CYCLE (R3, every 10 ticks)
  put(51, axisMapped(0, 1, 2, 1, 250, 1000, 0));   // TARGETER_XAXIS
  put(53, axisMapped(1, 1, 2, 0, 250, 1000, 3));   // LOCO_MOVE (config A)
  put(54, axisMapped(0, 1, 2, 1, 250, 1000, 3));   // LOCO_STRAFE
  const uint32_t yaw = axisMapped(2, 1, 2, 1, 250, 1000, 3);   // shared: 38 FLIGHT_YAW + 55 CAM_YAW
  put(38, yaw);
  put(55, yaw);
  put(58, button(9, 1, 42));          // SUPERSPEED
  put(59, button(15, 1, 1));          // POWER_ACTIVATE (binder: (15, 1, 1))
  put(60, release(15, 0, 1));         // POWER_RELEASE
  put(61, mapped(0, 1, 127, 255, 0)); // SELECT_HEATVISION
  put(62, mapped(2, 1, 127, 255, 0));
  put(63, mapped(3, 1, 127, 255, 0));
  put(64, button(9, 1, 42));          // DASH
  put(65, button(10, 0, 1));          // GRAB
  put(66, button(12, 0, 1));          // ATTACK
  put(67, button(12, 1, 1));          // ATTACK_HELD
  put(69, alt(button(15, 0, 42), button(10, 0, 1)));   // THROW (binder order: RT child first)
  put(70, button(11, 1, 1));          // BLOCK
  put(71, button(11, 0, 1));          // DROP
  put(74, release(12, 0, 1));         // ATTACK_RELEASED
  put(75, combo(release(15, 1, 42), release(10, 1, 1), 0, 1));        // THROW_RELEASED
  put(76, release(11, 0, 1));         // BLOCK_RELEASED
  put(77, combo(button(10, 0, 1), button(12, 0, 1), 3, 0));           // GRAB_AND_ATTACK
  put(78, combo(button(10, 1, 42), button(12, 1, 42), 3, 1));         // _HELD
  put(79, combo(button(13, 1, 1), button(11, 1, 1), 3, 1));           // JUMP_AND_THROW
  put(80, combo(button(10, 1, 1), combo(button(12, 1, 1), button(11, 1, 1), 3, 1), 3, 1));
  put(81, button(4, 0, 1));           // PAUSE
  put(82, button(5, 0, 1));           // MAP_VIEW
  put(117, button(0, 6, 1));          // a repeating menu key (interface-style)
  put(107, dual(8, 1, 15, 1, 0));     // LB + RT
  put(108, dual(14, 1, 9, 1, 0));     // LT + RB
  put(109, dual(14, 1, 15, 1, 0));    // LT + RT
  put(168, dual(6, 1, 7, 1, 0));      // L3 + R3
  put(187, alt(button(4, 0, 1), button(10, 0, 1)));   // FMV_SKIP
}

// The consumer-visible result of each command (IsActive's field, value field).
struct Seen { uint8_t fired; int32_t value; };
Seen seen(uint32_t cmd) {
  const uint32_t vt = r32(cmd);
  const uint8_t* p = K::guestVAToHost(cmd);
  switch (PA::kindOfVtable(vt)) {
    case PA::Kind::Button: case PA::Kind::Release: case PA::Kind::Mapped: return {p[16], p[17]};
    case PA::Kind::Axis: return {p[28], int16_t(p[16] << 8 | p[17])};
    case PA::Kind::AxisMapped: return {p[32], int16_t(p[16] << 8 | p[17])};
    case PA::Kind::Alternative: return {uint8_t(p[12] || p[13]), 0};
    case PA::Kind::Combo: return {p[21], 0};
    case PA::Kind::Dual: return {p[30], 0};
    default: return {0, 0};
  }
}

// The InputState layout under test: 16 digital bits (the original fixture) or the pad driver's 14 bits +
// 2 analog trigger bytes, the triggers' values from g_trig (per tick; 0 = released).
bool g_padLayout = false;
uint8_t g_trig[2] = {};
void setLayout(bool pad) {
  g_padLayout = pad;
  w32(kState + 0x14, pad ? 14 : 16);
  w32(kState + 0x18, pad ? 2 : 0);
  w32(kState + 0x24, pad ? kAnalog : 0);
}
void writeState(const PA::ControlFrame& f) {
  uint8_t* bits = K::guestVAToHost(kBits);
  const uint16_t b = g_padLayout ? uint16_t(f.buttons & 0x3FFF) : f.buttons;
  bits[0] = uint8_t(b & 0xFF);
  bits[1] = uint8_t(b >> 8);
  if (g_padLayout) {
    uint8_t* an = K::guestVAToHost(kAnalog);
    an[0] = g_trig[0];
    an[1] = g_trig[1];
  }
  for (int a = 0; a < 4; ++a) w32(kAxes + 4 * a, uint32_t(int32_t((f.axisDriven >> a) & 1 ? f.axis[a] : 0)));
}

// The game's InputContext_UpdateCommands: Update (vtable slot 6) on every non-null entry while a controller is
// present (InputState +8 == 1), Reset (slot 7) otherwise.
void gameUpdate() {
  const bool present = r32(kState + 8) == 1;
  for (uint32_t i = 0; i < kCount; ++i) {
    const uint32_t cmd = r32(kArr + 4 * i);
    if (!cmd) continue;
    const uint32_t fn = r32(r32(cmd) + (present ? 24 : 28));
    PPCFunc* f = PPC_LOOKUP_FUNC_SAFE(g_base, fn);
    if (!f) { check(false, "update function mapped"); return; }
    auto c = regs(cmd, kState);
    f(c, g_base);
  }
}

std::vector<PA::ControlFrame> sequence(int ticks, uint32_t seed = 12345u) {
  std::vector<PA::ControlFrame> seq(ticks);
  uint32_t lcg = seed;
  auto rnd = [&](uint32_t n) { lcg = lcg * 1664525u + 1013904223u; return (lcg >> 8) % n; };
  int hold[20] = {};
  PA::ControlFrame cur;
  static const int16_t kVals[] = {0, 250, -250, 251, -251, 600, -600, 1000, -1000, 400};
  // the context exists before the player touches anything (5 idle ticks), as in the game: a new
  // context's first tick seeds the mirror from the game's own result
  for (int t = 5; t < ticks; ++t) {
    for (int c = 0; c < 16; ++c) {
      if (--hold[c] > 0) continue;
      hold[c] = 1 + int(rnd(12));
      if (rnd(3) == 0) cur.buttons ^= uint16_t(1u << c);
    }
    for (int a = 0; a < 4; ++a) {
      if (--hold[16 + a] > 0) continue;
      hold[16 + a] = 1 + int(rnd(10));
      cur.axis[a] = kVals[rnd(10)];
      cur.axisDriven |= uint8_t(1u << a);
    }
    seq[t] = cur;
  }
  return seq;
}

using Record = std::vector<std::vector<Seen>>;   // [tick][event id]

enum class Mode { Game, PadWithLayer, PcWithLayer, BothWithLayer };

// Per tick trigger values for the pad layout ([tick][LT, RT]); empty = from the frame's bits 14 / 15 (255 / 0).
std::vector<std::array<uint8_t, 2>> g_trigSeq;
// Per tick controller presence (InputState +8); empty = always present.
std::vector<uint8_t> g_presentSeq;
// BothWithLayer: the PC's frames (the pad plays the run's sequence).
const std::vector<PA::ControlFrame>* g_pcSeq = nullptr;

Record run(const std::vector<PA::ControlFrame>& seq, Mode mode, bool* restoredOk) {
  PA::resetForTest();
  g_next = kObjs;
  buildContext();
  Record rec(seq.size(), std::vector<Seen>(kCount));
  std::vector<Seen> own(kCount);
  const PA::ControlFrame idle;
  const bool layer = mode != Mode::Game;
  const bool viaPc = mode == Mode::PcWithLayer;
  *restoredOk = true;
  for (size_t t = 0; t < seq.size(); ++t) {
    const PA::ControlFrame& padIn = viaPc ? idle : seq[t];
    for (int k = 0; k < 2; ++k)
      g_trig[k] = !g_trigSeq.empty() && !viaPc ? g_trigSeq[t][k]
                                               : uint8_t((padIn.buttons >> (14 + k)) & 1u ? 255 : 0);
    writeState(padIn);
    w32(kState + 8, g_presentSeq.empty() || g_presentSeq[t] ? 1 : 0);
    if (layer) {
      PA::publish(viaPc ? seq[t] : mode == Mode::BothWithLayer ? (*g_pcSeq)[t] : idle);
      PA::beforeDispatch(g_base);
      // the merge is fully put back: the commands hold the game's own values again
      for (uint32_t i = 0; t > 0 && i < kCount; ++i) {
        const uint32_t cmd = r32(kArr + 4 * i);
        if (!cmd) continue;
        const Seen s = seen(cmd);
        if (s.fired != own[i].fired || s.value != own[i].value) *restoredOk = false;
      }
      PA::noteContextUpdate(g_base, kCtx, kState);   // what the UpdateCommands hook latches
    }
    gameUpdate();
    for (uint32_t i = 0; i < kCount; ++i) {
      const uint32_t cmd = r32(kArr + 4 * i);
      own[i] = cmd ? seen(cmd) : Seen{0, 0};
    }
    if (layer) PA::afterDispatch(g_base);
    for (uint32_t i = 0; i < kCount; ++i) {
      const uint32_t cmd = r32(kArr + 4 * i);
      rec[t][i] = cmd ? seen(cmd) : Seen{0, 0};
    }
  }
  return rec;
}

int compare(const Record& want, const Record& got, const char* label, int* firedTicks) {
  std::map<uint32_t, int> mism;
  for (size_t t = 0; t < want.size(); ++t)
    for (uint32_t i = 0; i < kCount; ++i) {
      if (firedTicks && want[t][i].fired) ++*firedTicks;
      if (want[t][i].fired != got[t][i].fired || want[t][i].value != got[t][i].value) {
        if (mism[i]++ < 3)
          std::printf("  %s mismatch t=%zu ev=%u %s: pad (%u, %d) got (%u, %d)\n", label, t, i,
                      PA::eventName(i), want[t][i].fired, want[t][i].value, got[t][i].fired,
                      got[t][i].value);
      }
    }
  for (uint32_t i = 0; i < kCount; ++i) {
    if (!r32(kArr + 4 * i)) continue;
    check(mism.find(i) == mism.end(), std::string(label) + ": event " + std::to_string(i) + " " +
                                          PA::eventName(i) + " equals the game's own, every tick");
  }
  return int(mism.size());
}

// Trigger values on and around both binder thresholds (1: LLOCK, POWER; 42: THROW), holds of 1-9 ticks.
void rebuildTrig(size_t ticks) {
  static const uint8_t kTrig[] = {0, 0, 1, 2, 20, 41, 42, 43, 128, 255, 255};
  g_trigSeq.assign(ticks, {0, 0});
  uint32_t lcg = 777u;
  auto rnd = [&](uint32_t n) { lcg = lcg * 1664525u + 1013904223u; return (lcg >> 8) % n; };
  int hold[2] = {};
  std::array<uint8_t, 2> cur{0, 0};
  for (size_t t = 5; t < ticks; ++t) {
    for (int k = 0; k < 2; ++k) {
      if (--hold[k] > 0) continue;
      hold[k] = 1 + int(rnd(9));
      cur[k] = kTrig[rnd(sizeof kTrig)];
    }
    g_trigSeq[t] = cur;
  }
}

// The presence sequence: no controller on the context's first 3 ticks, then gaps of 1-25 ticks every 40-200.
std::vector<uint8_t> presence(size_t ticks) {
  std::vector<uint8_t> p(ticks, 1);
  uint32_t lcg = 4242u;
  auto rnd = [&](uint32_t n) { lcg = lcg * 1664525u + 1013904223u; return (lcg >> 8) % n; };
  for (size_t t = 0; t < 3 && t < ticks; ++t) p[t] = 0;
  for (size_t t = 40 + rnd(160); t < ticks; t += 40 + rnd(160))
    for (size_t e = t + 1 + rnd(25); t < e && t < ticks; ++t) p[t] = 0;
  return p;
}

// Reconnects with a control held on both sides of the gap: (buttons, triggers) counts.
std::array<int, 2> heldAcrossGaps(const std::vector<PA::ControlFrame>& seq, const std::vector<uint8_t>& p,
                                  const std::vector<std::array<uint8_t, 2>>& trig) {
  std::array<int, 2> n{0, 0};
  size_t last = 0;   // the last present tick
  for (size_t t = 1; t < p.size(); ++t) {
    if (p[t] && !p[t - 1] && p[last]) {
      for (int c = 0; c < 14; ++c) n[0] += ((seq[last].buttons & seq[t].buttons) >> c) & 1u;
      for (int k = 0; k < 2 && !trig.empty(); ++k) n[1] += trig[last][k] > 1 && trig[t][k] > 1;
    }
    if (p[t]) last = t;
  }
  return n;
}

// Astra P3: a malformed analog layout (na > 0 with the bytes null, or n + na wrapping in 32 bits) is rejected. The PC
// holds UP (event 3, a held button: the layer writes its value 255 every tick, the idle pad's is 0); on tick 5 the
// layout is malformed (the game's own Update on it is not modelled: its result is what the commands hold), after it
// well-formed again.
void malformedCase(const char* label, uint32_t na, uint32_t bytes) {
  PA::resetForTest();
  g_next = kObjs;
  buildContext();
  setLayout(true);
  g_trig[0] = g_trig[1] = 0;
  w32(kState + 8, 1);
  PA::ControlFrame pc;
  pc.buttons = 1u << 0;
  const PA::ControlFrame idle;
  const uint32_t up = r32(kArr + 4 * 3);
  const std::string what = std::string("malformed layout (") + label + ")";
  for (int t = 0; t < 8; ++t) {
    writeState(idle);
    PA::publish(pc);
    PA::beforeDispatch(g_base);
    const bool bad = t == 5;
    if (bad) { w32(kState + 0x18, na); w32(kState + 0x24, bytes); }
    PA::noteContextUpdate(g_base, kCtx, kState);
    if (bad) setLayout(true);
    else gameUpdate();
    std::vector<Seen> own(kCount);
    for (uint32_t i = 0; i < kCount; ++i) {
      const uint32_t cmd = r32(kArr + 4 * i);
      own[i] = cmd ? seen(cmd) : Seen{0, 0};
    }
    PA::afterDispatch(g_base);
    if (t == 4) check(seen(up).value == 255, what + ": before it, the PC's held UP reaches the consumers");
    if (t == 5 || t == 6) {
      bool same = true;
      for (uint32_t i = 0; i < kCount; ++i) {
        const uint32_t cmd = r32(kArr + 4 * i);
        if (cmd && (seen(cmd).fired != own[i].fired || seen(cmd).value != own[i].value)) same = false;
      }
      check(same, what + (t == 5 ? ": the latch is rejected, every command keeps the game's own result"
                                 : ": the next tick the mirrors reseed from the game's result"));
    }
    if (t == 7) check(seen(up).value == 255, what + ": then the merge resumes");
  }
}
}  // namespace

int runPcActionsTest() {
  g_base = K::getGuestBase();
  if (!g_base || !K::hostRangeReadable(K::guestVAToHost(kArena), kArenaSize)) {
    std::puts("FAIL pc-actions test needs the runner's full guest mapping");
    return 1;
  }
  fail += PA::selfTest();
  // save what the fixture overwrites
  std::vector<uint8_t> oldArena(K::guestVAToHost(kArena), K::guestVAToHost(kArena) + kArenaSize);
  const uint32_t oldMgr = r32(kInputMgrPtr), oldPad = r32(kActivePadPtr);
  std::vector<std::vector<uint32_t>> oldVts;
  for (const Vt& v : kVts) {
    std::vector<uint32_t> o;
    for (int i = 0; i < 10; ++i) { o.push_back(r32(v.va + 4 * i)); w32(v.va + 4 * i, v.slots[i]); }
    oldVts.push_back(o);
  }
  // the dispatch table the composites' child calls go through (vtable slots 2 and 6)
  std::vector<PPCFunc*> table(K::kDispatchTableEntryCount, nullptr);
  PPCFunc** savedTable = K::g_dispatchTable;
  auto map = [&](uint32_t va, PPCFunc* f) { table[(va - PPC_CODE_BASE) >> 2] = f; };
  map(0x82723A60, sub_82723A60); map(0x827237C0, sub_827237C0);
  map(0x82725C90, sub_82725C90); map(0x827259F8, sub_827259F8);
  map(0x827250B8, sub_827250B8); map(0x82724E30, sub_82724E30);
  map(0x82723518, sub_82723518); map(0x82723138, sub_82723138);
  map(0x82725740, sub_82725740); map(0x827253B0, sub_827253B0);
  map(0x82724A18, sub_82724A18); map(0x82724968, sub_82724968);
  map(0x82724660, sub_82724660); map(0x82724328, sub_82724328);
  map(0x82723D70, sub_82723D70); map(0x82723D00, sub_82723D00);
  // Reset (slot 7): InputContext_UpdateCommands with no controller
  map(0x827231F0, sub_827231F0); map(0x82723838, sub_82723838);
  map(0x82723E48, sub_82723E48); map(0x82724398, sub_82724398);
  map(0x82724AB0, sub_82724AB0); map(0x82724E80, sub_82724E80);
  map(0x82725408, sub_82725408); map(0x82725A70, sub_82725A70);
  K::g_dispatchTable = table.data();
  // the synthetic input manager: player 1 = slot 0, live, the context above
  std::memset(K::guestVAToHost(kArena), 0, 0x2000);
  w32(kMgr + 4, 1); w32(kMgr + 12, kMapInfo);
  K::guestVAToHost(kMapInfo + 16)[0] = 1;
  w32(kMapInfo + 12, 0); w32(kMapInfo + 20, kCtx);
  w32(kPad + 12, 0);
  w32(kCtx + 4, kCount); w32(kCtx + 8, kArr);
  w32(kState + 0x20, kBits); w32(kState + 0x28, kAxes);
  setLayout(false);
  w32(kInputMgrPtr, kMgr); w32(kActivePadPtr, kPad);

  w32(kState + 8, 1);   // a controller is present (UpdateCommands updates, not resets)
  const auto seq = sequence(3000);
  bool r0 = true, r1 = true, r2 = true;
  const Record game = run(seq, Mode::Game, &r0);
  const Record padLayer = run(seq, Mode::PadWithLayer, &r1);
  const Record pcLayer = run(seq, Mode::PcWithLayer, &r2);
  int firedTicks = 0;
  compare(game, padLayer, "controller with the layer on", &firedTicks);
  check(firedTicks > 2000, "the sequence exercises the commands (" + std::to_string(firedTicks) + " fired event-ticks)");
  compare(game, pcLayer, "keys through the layer", nullptr);
  check(r1 && r2, "the merge is put back before the game's next evaluation");

  // the REAL pad layout (14 bits + the 2 analog trigger bytes): a controller's triggers with the layer on
  setLayout(true);
  {
    rebuildTrig(seq.size());
    bool q0 = true, q1 = true;
    const Record gameA = run(seq, Mode::Game, &q0);
    const Record padA = run(seq, Mode::PadWithLayer, &q1);
    int firedA = 0;
    compare(gameA, padA, "pad layout: controller (analog triggers) with the layer on", &firedA);
    check(q1, "pad layout: the merge is put back before the game's next evaluation");
    // the trigger commands themselves fire in the game's own run (the fixture exercises them)
    for (uint32_t ev : {44u, 45u, 46u, 59u, 60u, 69u, 75u, 107u, 108u, 109u}) {
      int n = 0;
      for (size_t t = 0; t < gameA.size(); ++t) n += gameA[t][ev].fired;
      check(n > 0, "pad layout: the game's own " + std::string(PA::eventName(ev)) + " fires on the trigger sequence (" +
                       std::to_string(n) + " ticks)");
    }
    // keys on the pad layout: the PC frame's LT / RT bits answer like a fully pulled trigger
    g_trigSeq.clear();
    bool q2 = true, q3 = true;
    const Record gameB = run(seq, Mode::Game, &q2);
    const Record pcB = run(seq, Mode::PcWithLayer, &q3);
    compare(gameB, pcB, "pad layout: keys through the layer", nullptr);
    check(q3, "pad layout: the keys' merge is put back");

    // Astra P2: the controller goes away and comes back, with triggers / buttons held across the gap
    rebuildTrig(seq.size());
    g_presentSeq = presence(seq.size());
    int gaps = 0;
    for (size_t t = 1; t < g_presentSeq.size(); ++t) gaps += g_presentSeq[t] && !g_presentSeq[t - 1];
    const auto held = heldAcrossGaps(seq, g_presentSeq, g_trigSeq);
    check(gaps > 10 && held[0] > 0 && held[1] > 0,
          "presence: " + std::to_string(gaps) + " reconnects, " + std::to_string(held[0]) + " buttons and " +
              std::to_string(held[1]) + " triggers held across a gap");
    bool p0 = true, p1 = true, p2 = true, p3 = true;
    const Record gameC = run(seq, Mode::Game, &p0);
    const Record padC = run(seq, Mode::PadWithLayer, &p1);
    compare(gameC, padC, "pad layout, disconnects: controller with the layer on", nullptr);
    check(p1, "pad layout, disconnects: the merge is put back");
    // the reconnect edge itself: the game's own LLOCK_TOGGLE / POWER_ACTIVATE fire on a reconnect with the trigger held
    int edges = 0;
    for (size_t t = 1; t < gameC.size(); ++t)
      if (g_presentSeq[t] && !g_presentSeq[t - 1]) edges += gameC[t][44].fired + gameC[t][59].fired;
    check(edges > 0, "pad layout, disconnects: the game fires a held trigger again on reconnect (" +
                         std::to_string(edges) + " LLOCK_TOGGLE / POWER_ACTIVATE)");
    g_trigSeq.clear();
    const Record gameD = run(seq, Mode::Game, &p2);
    const Record pcD = run(seq, Mode::PcWithLayer, &p3);
    compare(gameD, pcD, "pad layout, disconnects: keys through the layer", nullptr);
    check(p3, "pad layout, disconnects: the keys' merge is put back");

    // a controller and keys at once: the layer on (pad, PC) vs the game's own run on the merged input
    const auto pcSeq = sequence(int(seq.size()), 999u);
    rebuildTrig(seq.size());
    const auto padTrig = g_trigSeq;
    std::vector<PA::ControlFrame> merged(seq.size());
    std::vector<std::array<uint8_t, 2>> mergedTrig(seq.size());
    for (size_t t = 0; t < seq.size(); ++t) {
      PA::ControlFrame& c = merged[t];
      c.buttons = uint16_t((seq[t].buttons | pcSeq[t].buttons) & 0x3FFF);
      for (int a = 0; a < 4; ++a) {
        const bool padOn = (seq[t].axisDriven >> a) & 1u, pcOn = (pcSeq[t].axisDriven >> a) & 1u;
        const int16_t pv = padOn ? seq[t].axis[a] : 0;
        c.axis[a] = pcOn && std::abs(int(pcSeq[t].axis[a])) >= std::abs(int(pv)) ? pcSeq[t].axis[a] : pv;
        if (padOn || pcOn) c.axisDriven |= uint8_t(1u << a);
      }
      for (int k = 0; k < 2; ++k) mergedTrig[t][k] = (pcSeq[t].buttons >> (14 + k)) & 1u ? 255 : padTrig[t][k];
    }
    for (int gapsOn = 0; gapsOn < 2; ++gapsOn) {   // always present, then the presence sequence
      if (gapsOn) g_presentSeq = presence(seq.size());
      else g_presentSeq.clear();
      const std::string lab = gapsOn ? "pad layout, disconnects: controller + keys at once"
                                     : "pad layout: controller + keys at once";
      bool m0 = true, m1 = true;
      g_trigSeq = mergedTrig;
      const Record gameM = run(merged, Mode::Game, &m0);
      g_trigSeq = padTrig;
      g_pcSeq = &pcSeq;
      const Record bothM = run(seq, Mode::BothWithLayer, &m1);
      g_pcSeq = nullptr;
      int firedM = 0;
      compare(gameM, bothM, lab.c_str(), &firedM);
      check(m1 && firedM > 2000, lab + ": the merge is put back (" + std::to_string(firedM) + " fired event-ticks)");
    }
    g_presentSeq.clear();
    g_trigSeq.clear();
  }
  malformedCase("analog bytes null", 2, 0);
  malformedCase("n + na wraps", 0xFFFFFFF4u, kAnalog);
  setLayout(false);
  w32(kState + 8, 1);

  // restore
  K::g_dispatchTable = savedTable;
  w32(kInputMgrPtr, oldMgr); w32(kActivePadPtr, oldPad);
  for (size_t k = 0; k < oldVts.size(); ++k)
    for (int i = 0; i < 10; ++i) w32(kVts[k].va + 4 * i, oldVts[k][i]);
  std::memcpy(K::guestVAToHost(kArena), oldArena.data(), kArenaSize);
  PA::resetForTest();
  std::printf("pc-actions test: %d passed, %d failed\n", pass, fail);
  return fail ? 1 : 0;
}
