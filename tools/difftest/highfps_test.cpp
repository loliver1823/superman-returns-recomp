// iter-464 PORT FEATURE high frame rate (kernel/highfps.cpp, docs/HIGH_FRAMERATE.md).
//
//   --highfps-test
//     C*  the SR_HIGHFPS config mapping (configFromEnv): off / monitor / fixed N / uncapped,
//         the vblank and present-sync choices, the overrides, unparsable input = off
//     R*  the REAL FGGameRender mgr ctor GameKernelMgrDerivedCtor_sub_82166178 (mode 1 =
//         variable step) and GameKernelMgr_SetTickRate_sub_82720418: rate -> period
//         (30 Hz -> 33,333,334 ns, the golden FGGameSim period), and the override is an exact
//         pass-through with SR_HIGHFPS unset
//     S*  the REAL EA::StateStream group: RenderFrameBegin_82787650 / RenderFrameEnd_82787730
//         (sim side) and StateStreamGroup_BeginRead_sub_82787520 / EndRead_sub_827876A8
//         (render side): the bracketing frames, t = (time-cur)/(next-cur), the read stamp
//     P*  pacing: a 30 Hz FIXED sim mgr and a render mgr at 30/60/120/144/240 Hz, both run
//         by the REAL GameKernelLoop TimeGate / AdvanceTickTime / PostTickStats on a
//         simulated 1 ms guest clock (KeTimeStampBundle ms), publishing and reading through
//         the REAL StateStream. Measures what the player would see: presents per second,
//         the displayed game time per present against the wall clock, held frames.
//
// Run: build/difftest.exe --highfps-test   (exit code = failed checks)
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/export_registry.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/highfps.hpp"
#include "kernel/sr_timebase.hpp"
#include "kernel/stubs.hpp"

PPC_EXTERN_FUNC(sub_82166178);   // GameKernelMgrDerivedCtor (FGGameRender mgr; the override)
PPC_EXTERN_FUNC(sub_827209D8);   // GameKernelMgr_Ctor (base; FGGameSim builds with it)
PPC_EXTERN_FUNC(sub_82720418);   // GameKernelMgr_SetTickRate
PPC_EXTERN_FUNC(sub_827206B8);   // GameKernelLoop_TimeGate
PPC_EXTERN_FUNC(sub_82720740);   // GameKernelLoop_AdvanceTickTime
PPC_EXTERN_FUNC(sub_82720858);   // GameKernelLoop_PostTickStats
PPC_EXTERN_FUNC(sub_825FC088);   // EAThread_MutexCtor
PPC_EXTERN_FUNC(sub_82787650);   // RenderFrameBegin (StateStream group, stamp)
PPC_EXTERN_FUNC(sub_82787730);   // RenderFrameEnd
PPC_EXTERN_FUNC(sub_82787520);   // StateStreamGroup_BeginRead
PPC_EXTERN_FUNC(sub_827876A8);   // StateStreamGroup_EndRead
PPC_EXTERN_FUNC(sub_824680C0);   // GuestClockMs (KeTimeStampBundle ms; splice reads bundle+0x10 when the 50 MHz gate is on)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace K = sr::kernel;
namespace TB = sr::timebase;
namespace HF = sr::kernel::highfps;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}

constexpr uint32_t kStack = 0x70780000u;
constexpr uint32_t kGroup = 0x70700000u;     // EA::StateStream::Group (0x70 bytes)
constexpr uint32_t kRecArr = 0x70700100u;    // 4 record pointers
constexpr uint32_t kRec0 = 0x70700200u;      // 4 records x 0x40
constexpr uint32_t kRecData = 0x70701000u;   // the records' (empty) handle tables
constexpr uint32_t kRenderMgr = 0x70702000u; // GameKernelLoop mgr, 0xB0 bytes
constexpr uint32_t kSimMgr = 0x70703000u;
constexpr int kRecords = 4;                  // the golden group 0 has 4 streams

uint8_t* base() { return K::getGuestBase(); }
uint64_t rd64(uint32_t va) {
  const uint8_t* p = K::guestVAToHost(va);
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  return v;
}
void wr64(uint32_t va, uint64_t v) {
  uint8_t* p = K::guestVAToHost(va);
  for (int i = 0; i < 8; i++) p[i] = uint8_t(v >> (56 - 8 * i));
}
float rdF(uint32_t va) {
  const uint32_t b = K::readGuestU32BE(va);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}
PPCContext mk() {
  PPCContext c{};
  c.r1.u64 = kStack;
  return c;
}

// ------------------------------------------------------------------ C: config ---------------
void configTests() {
  std::printf("[C] SR_HIGHFPS config mapping\n");
  using HF::Mode;
  auto c = HF::configFromEnv(nullptr, nullptr, nullptr, nullptr, nullptr, 60.0);
  check(c.mode == Mode::Off && c.vblankHz == 60.0 && c.syncInterval == 1 && !c.paceLog,
        "C1 unset = off: console 60 Hz vblank, sync 1, no pace log");
  c = HF::configFromEnv("0", "240", "240", "0", nullptr, 60.0);
  check(c.mode == Mode::Off && c.renderHz == 30.0 && c.vblankHz == 60.0 && c.syncInterval == 1,
        "C2 =0 = off, and the overrides do nothing while off");
  c = HF::configFromEnv("monitor", nullptr, nullptr, nullptr, nullptr, 144.0);
  check(c.mode == Mode::Monitor && c.renderHz == 144.0 && c.vblankHz == 288.0 && c.syncInterval == 1 && c.paceLog,
        "C3 monitor @144: render 144, vblank 288 (2x so INTERVAL_ONE never binds), vsync on");
  c = HF::configFromEnv("1", nullptr, nullptr, nullptr, nullptr, 60.0);
  check(c.mode == Mode::Monitor && c.renderHz == 60.0 && c.vblankHz == 120.0, "C4 =1 = monitor");
  c = HF::configFromEnv("60", nullptr, nullptr, nullptr, nullptr, 60.0);
  check(c.mode == Mode::Fixed && c.renderHz == 60.0 && c.vblankHz == 120.0 && c.syncInterval == 1 && !c.tearing,
        "C5 60 on a 60 Hz display: render 60, vblank 120, vsync on");
  c = HF::configFromEnv("120", nullptr, nullptr, nullptr, nullptr, 60.0);
  check(c.mode == Mode::Fixed && c.renderHz == 120.0 && c.vblankHz == 240.0 && c.syncInterval == 0 && c.tearing,
        "C6 120 on a 60 Hz display: render 120, vblank 240, vsync off + tearing");
  c = HF::configFromEnv("120", nullptr, nullptr, nullptr, nullptr, 144.0);
  check(c.syncInterval == 1 && !c.tearing, "C7 120 on a 144 Hz display: vsync on");
  c = HF::configFromEnv("uncapped", nullptr, nullptr, nullptr, nullptr, 60.0);
  check(c.mode == Mode::Uncapped && c.renderHz == HF::kMaxHz && c.vblankHz == HF::kMaxHz &&
            c.syncInterval == 0 && c.tearing,
        "C8 uncapped: render 1000, vblank 1000, sync 0 + tearing");
  c = HF::configFromEnv("30", nullptr, nullptr, nullptr, nullptr, 60.0);
  check(c.mode == Mode::Fixed && c.renderHz == 30.0 && c.vblankHz == 60.0 && c.syncInterval == 1,
        "C9 30 = the console rates");
  c = HF::configFromEnv("120", "90", "500", "1", "0", 60.0);
  check(c.renderHz == 90.0 && c.vblankHz == 500.0 && c.syncInterval == 1 && !c.tearing && !c.paceLog,
        "C10 overrides: render, vblank, sync, pace");
  c = HF::configFromEnv("fast please", nullptr, nullptr, nullptr, nullptr, 60.0);
  check(c.mode == Mode::Off, "C11 unparsable = off (the console behaviour)");
  c = HF::configFromEnv("5000", nullptr, nullptr, nullptr, "1", 0.0);
  check(c.renderHz == HF::kMaxHz && c.displayHz == 60.0 && c.paceLog,
        "C12 clamps: 5000 -> 1000, an unknown display rate -> 60; pace=1 logs");

  // Precedence: env > automated roll > SR_PORT_FEATURES=0 > player setting > default.
  HF::Source src;
  const char* m = HF::resolveMode("120", "run0", "0", "monitor", src);
  check(std::strcmp(m, "120") == 0 && src == HF::Source::Env, "C13 SR_HIGHFPS wins over everything");
  m = HF::resolveMode("0", nullptr, nullptr, "monitor", src);
  check(std::strcmp(m, "0") == 0 && src == HF::Source::Env, "C14 SR_HIGHFPS=0 turns a player's setting off (A/B)");
  m = HF::resolveMode(nullptr, "run0", nullptr, "120", src);
  check(std::strcmp(m, "0") == 0 && src == HF::Source::AutomatedRoll,
        "C15 an automated roll (SR_INSTANCE_ID) never runs the port feature");
  m = HF::resolveMode(nullptr, nullptr, "0", "120", src);
  check(std::strcmp(m, "0") == 0 && src == HF::Source::PortFeaturesOff, "C16 SR_PORT_FEATURES=0 = off");
  m = HF::resolveMode(nullptr, nullptr, nullptr, "uncapped", src);
  check(std::strcmp(m, "uncapped") == 0 && src == HF::Source::Player, "C17 the player's target_fps");
  m = HF::resolveMode(nullptr, nullptr, nullptr, nullptr, src);
  check(std::strcmp(m, HF::kPlayerDefault) == 0 && src == HF::Source::Default, "C18 no setting = the player default");
  // iter-465 player: a PORT FEATURE roll (SR_PORT_FEATURES=1) resolves like a player.
  m = HF::resolveMode(nullptr, "run0", "1", nullptr, src);
  check(std::strcmp(m, HF::kPlayerDefault) == 0 && src == HF::Source::Default,
        "C18b SR_INSTANCE_ID + SR_PORT_FEATURES=1 = the player default");
  m = HF::resolveMode(nullptr, "run0", "1", "monitor", src);
  check(std::strcmp(m, "monitor") == 0 && src == HF::Source::Player, "C18c a PORT FEATURE roll reads its own setting");
  // iter-465 polish: the display refresh with vsync on (120 tore on the owner's 60 Hz TV).
  check(std::strcmp(HF::kPlayerDefault, "monitor") == 0, "C18d the player default is the display refresh (monitor)");
  char v[32];
  check(HF::parseTargetFps("# PC settings\r\nres_scale = 2\r\n  target_fps =  120  # cap\r\n", v, sizeof v) &&
            std::strcmp(v, "120") == 0,
        "C19 display.ini: target_fps parsed (CRLF, comments, spaces)");
  check(!HF::parseTargetFps("target_fps=\nfoo=1\n", v, sizeof v) && v[0] == 0, "C20 empty / absent key = no setting");
  check(HF::parseTargetFps("target_fps=60\ntarget_fps=monitor\n", v, sizeof v) && std::strcmp(v, "monitor") == 0,
        "C21 the last target_fps wins");

  // The settings UI store (setPlayerTargetFps rewrites display.ini through withTargetFps).
  check(HF::validTargetFps("monitor") && HF::validTargetFps("UNCAPPED") && HF::validTargetFps("off") &&
            HF::validTargetFps("0") && HF::validTargetFps("30") && HF::validTargetFps("144") &&
            HF::validTargetFps("1000"),
        "C22 valid values: monitor, uncapped, off, 0, 30, 144, 1000");
  check(!HF::validTargetFps(nullptr) && !HF::validTargetFps("") && !HF::validTargetFps("fast") &&
            !HF::validTargetFps("120fps") && !HF::validTargetFps("0.5") && !HF::validTargetFps("5000") &&
            !HF::validTargetFps("nan"),
        "C23 invalid values refused: empty, words, units, < 1, > 1000, nan");
  const std::string other = "# display\r\nres_scale=2\r\ndisplay_mode = borderless\r\n";
  std::string w = HF::withTargetFps(other, "120");
  check(w == other + "target_fps=120\r\n", "C24 no key: appended with the file's own CRLF, other lines byte for byte");
  w = HF::withTargetFps("res_scale=auto\n  target_fps = 60  # mine\nwindow_width=1920\n", "monitor");
  check(w == "res_scale=auto\ntarget_fps=monitor\nwindow_width=1920\n", "C25 existing key replaced in place, neighbours kept");
  w = HF::withTargetFps("", "uncapped");
  check(w == "target_fps=uncapped\n", "C26 empty file: one line");
  w = HF::withTargetFps("res_scale=2", "60");
  check(w == "res_scale=2\ntarget_fps=60\n" && HF::parseTargetFps(w.c_str(), v, sizeof v) && std::strcmp(v, "60") == 0,
        "C27 no trailing newline: a line break first; the result parses back");

  // iter-465 polish: VSYNC (display.ini vsync = on | off, default on).
  bool on = false;
  check(HF::parseVsync("res_scale=2\r\n vsync = OFF # tear\r\n", on) && !on, "C28 vsync=OFF parsed (CRLF, case, comment)");
  check(HF::parseVsync("vsync=on\nvsync=0\nvsync=1\n", on) && on, "C28b the last vsync line wins (1 = on)");
  on = true;
  check(!HF::parseVsync("target_fps=60\nvsync=maybe\n", on) && on, "C28c no / unparsable vsync key = not set");
  w = HF::withIniKey("res_scale=2\r\ntarget_fps=120\r\n", "vsync", "off");
  check(w == "res_scale=2\r\ntarget_fps=120\r\nvsync=off\r\n", "C29 withIniKey appends with the file's CRLF");
  w = HF::withIniKey(w, "vsync", "on");
  check(w == "res_scale=2\r\ntarget_fps=120\r\nvsync=on\r\n", "C29b withIniKey replaces in place");
  HF::Config vc = HF::configFromEnv("120", nullptr, nullptr, nullptr, nullptr, 60.0);
  HF::applyVsync(vc, true);
  check(vc.syncInterval == 1 && !vc.tearing && vc.renderHz == 120.0,
        "C30 120 on a 60 Hz display, VSYNC ON: sync 1, no tearing (the render loop still 120)");
  HF::applyVsync(vc, false);
  check(vc.syncInterval == 0 && vc.tearing, "C30b VSYNC OFF: sync 0 with tearing");
  vc = HF::configFromEnv("monitor", nullptr, nullptr, nullptr, nullptr, 60.0);
  HF::applyVsync(vc, true);
  check(vc.mode == HF::Mode::Monitor && vc.renderHz == 60.0 && vc.syncInterval == 1 && !vc.tearing,
        "C31 the player default (monitor + vsync on) on a 60 Hz display: 60 Hz render loop, sync 1, no tearing");
  vc = HF::configFromEnv("0", nullptr, nullptr, nullptr, nullptr, 60.0);
  HF::applyVsync(vc, false);
  check(vc.mode == HF::Mode::Off && vc.syncInterval == 1, "C32 off stays the console path whatever vsync says");
}

// ------------------------------------------------------------------ R: the render mgr -------
void buildRenderMgr(uint32_t mgr, double hz) {
  std::memset(K::guestVAToHost(mgr), 0, 0xB0);
  PPCContext c = mk();
  c.r3.u64 = mgr;
  c.r4.u64 = 2;            // StartService_FGGameRender_sub_82177F90(2)
  c.f1.f64 = hz;
  sub_82166178(c, base());
}

void buildSimMgr(uint32_t mgr, double hz) {
  std::memset(K::guestVAToHost(mgr), 0, 0xB0);
  PPCContext c = mk();
  c.r3.u64 = mgr;
  c.f1.f64 = hz;
  c.r4.u64 = 0;
  c.r5.u64 = 0;            // step mode 0 = FIXED (StartService_FGGameSim_sub_821780C0)
  c.r6.u64 = 0;
  sub_827209D8(c, base());
}

void renderMgrTests() {
  std::printf("[R] the real FGGameRender mgr ctor + SetTickRate\n");
  char buf[200];
  // The guest computes the period in SINGLE precision: (float)(1e9f / rate) + 0.5f, then
  // fctidz. 30 Hz -> 33,333,334 is the golden FGGameSim value; 60 -> 16,666,668, 120 ->
  // 8,333,334, 144 -> 6,944,445 (float rounding, not 1e9/rate).
  const struct { double hz; int64_t period; } cases[] = {
      {30.0, 33333334}, {60.0, 16666668}, {120.0, 8333334}, {144.0, 6944445}, {1000.0, 1000000}};
  for (const auto& k : cases) {
    buildRenderMgr(kRenderMgr, k.hz);
    const int64_t period = int64_t(rd64(kRenderMgr + 0x48));
    const uint32_t mode = K::readGuestU32BE(kRenderMgr + 0x34);
    const float rate = rdF(kRenderMgr + 0x3C);
    std::snprintf(buf, sizeof buf, "R1 ctor(%.0f Hz): period %lld ns (want %lld), rate %.1f, step mode %u (1 = variable)",
                  k.hz, (long long)period, (long long)k.period, rate, mode);
    check(period == k.period && mode == 1 && rate == float(k.hz), buf);
  }
  buildSimMgr(kSimMgr, 30.0);
  std::snprintf(buf, sizeof buf, "R2 FGGameSim base ctor(30 Hz, mode 0): period %lld (golden 33333334), mode %u",
                (long long)rd64(kSimMgr + 0x48), K::readGuestU32BE(kSimMgr + 0x34));
  check(rd64(kSimMgr + 0x48) == 33333334ull && K::readGuestU32BE(kSimMgr + 0x34) == 0, buf);
  PPCContext c = mk();
  c.r3.u64 = kRenderMgr;
  c.f1.f64 = 120.0;
  sub_82720418(c, base());
  check(rd64(kRenderMgr + 0x48) == 8333334ull, "R3 SetTickRate(120) alone: period 8,333,334 ns");
}

// ------------------------------------------------------------------ L: live changes ---------
// iter-465 polish: the DISPLAY OPTIONS page's live path. applyLive re-resolves the config; the
// render loop's own post-tick (GameKernelLoop_PostTickStats, overridden in highfps.cpp) then calls
// the game's own GameKernelMgr_SetTickRate on the FGGameRender mgr the global 0x8296D8DC names (a
// real mgr built by the game's own ctor here; the test's thread plays the render loop).
bool waitRate(float want) {
  PPCContext c = mk();
  c.r3.u64 = kRenderMgr;
  sub_82720858(c, base());
  return rdF(kRenderMgr + 0x3C) == want;
}

void liveTests() {
  std::printf("[L] live changes from the settings menu\n");
  char buf[220];
  const uint32_t savedGlobal = K::readGuestU32BE(0x8296D8DCu);
  buildRenderMgr(kRenderMgr, 30.0);
  K::writeGuestBE32(0x8296D8DCu, kRenderMgr);
  check(HF::applyLive("60", true) && waitRate(60.0f), "L1 applyLive(60, vsync on): the mgr's rate becomes 60");
  std::snprintf(buf, sizeof buf, "L1b period %lld ns (the game's own setter: 16,666,668), sync %u, tearing %d, vblank %.0f",
                (long long)rd64(kRenderMgr + 0x48), HF::presentSyncInterval(), HF::wantTearing() ? 1 : 0,
                HF::liveVblankHz());
  check(rd64(kRenderMgr + 0x48) == 16666668ull && HF::presentSyncInterval() == 1 && !HF::wantTearing() &&
            HF::liveVblankHz() == 120.0,
        buf);
  check(HF::applyLive("uncapped", false) && waitRate(1000.0f) && HF::presentSyncInterval() == 0 && HF::wantTearing() &&
            HF::liveVblankHz() == 1000.0,
        "L2 UNLIMITED + VSYNC OFF: 1000 Hz loop, sync 0 with tearing, vblank 1000");
  check(HF::applyLive("30", true) && waitRate(30.0f) && rd64(kRenderMgr + 0x48) == 33333334ull &&
            HF::presentSyncInterval() == 1 && !HF::wantTearing() && HF::liveVblankHz() == 60.0,
        "L3 30 (CONSOLE): the game's own 30 Hz period 33,333,334, vblank 60, sync 1");
  const HF::Config lc = HF::liveConfig();
  check(lc.mode == HF::Mode::Fixed && lc.targetHz == 30.0, "L4 liveConfig follows the last change");
  check(!HF::applyLive("fast", true), "L5 an invalid value is refused");
  K::writeGuestBE32(0x8296D8DCu, savedGlobal);
}

// ------------------------------------------------------------------ S: StateStream ----------
void buildGroup() {
  std::memset(K::guestVAToHost(kGroup), 0, 0x70);
  PPCContext c = mk();
  c.r3.u64 = kGroup;
  c.r4.u64 = 0;
  c.r5.u64 = 1;
  sub_825FC088(c, base());   // the group's CS (+0, depth +0x20), as the game builds mutexes
  K::writeGuestBE32(kGroup + 0x38, 0xFFFFFFFFu);
  K::writeGuestBE32(kGroup + 0x3C, 0xFFFFFFFFu);
  K::writeGuestBE32(kGroup + 0x50, 0);
  K::writeGuestBE32(kGroup + 0x54, kRecords);
  K::writeGuestBE32(kGroup + 0x60, kRecArr);
  for (int i = 0; i < kRecords; i++) {
    const uint32_t r = kRec0 + 0x40u * uint32_t(i);
    std::memset(K::guestVAToHost(r), 0, 0x40);
    K::writeGuestBE32(kRecArr + 4u * uint32_t(i), r);
    K::writeGuestBE32(r + 0x08, kRecData + 0x100u * uint32_t(i));
    K::writeGuestBE32(r + 0x0C, 3);   // initial state (the stream ctor's 3)
    K::writeGuestBE32(r + 0x18, 0);   // no handles
  }
}

void publish(int64_t stamp) {
  PPCContext c = mk();
  c.r3.u64 = kGroup;
  c.r4.u64 = uint64_t(stamp);
  sub_82787650(c, base());
  c = mk();
  c.r3.u64 = kGroup;
  sub_82787730(c, base());
}

struct Read { bool any; int64_t readStamp; float t; int32_t cur, next; int64_t curStamp, nextStamp; };
Read beginRead(int64_t time) {
  PPCContext c = mk();
  c.r3.u64 = kGroup;
  c.r4.u64 = uint64_t(time);
  sub_82787520(c, base());
  Read r{};
  r.any = (c.r3.u32 & 0xFF) != 0;
  r.readStamp = int64_t(rd64(kGroup + 0x30));
  r.t = rdF(kGroup + 0x2C);
  r.cur = int32_t(K::readGuestU32BE(kGroup + 0x3C));
  r.next = int32_t(K::readGuestU32BE(kGroup + 0x38));
  r.curStamp = r.cur >= 0 ? int64_t(rd64(K::readGuestU32BE(kRecArr + 4u * uint32_t(r.cur)))) : -1;
  r.nextStamp = r.next >= 0 ? int64_t(rd64(K::readGuestU32BE(kRecArr + 4u * uint32_t(r.next)))) : -1;
  return r;
}
void endRead() {
  PPCContext c = mk();
  c.r3.u64 = kGroup;
  sub_827876A8(c, base());
}

void stateStreamTests() {
  std::printf("[S] the real EA::StateStream group\n");
  char buf[220];
  buildGroup();
  Read r = beginRead(1000);
  check(!r.any, "S1 nothing published: BeginRead returns 0");
  endRead();
  constexpr int64_t P = 33333334;
  publish(0);
  publish(P);
  publish(2 * P);
  r = beginRead(P + P / 4);
  std::snprintf(buf, sizeof buf, "S2 time = P + P/4: cur %lld next %lld t %.4f read %lld",
                (long long)r.curStamp, (long long)r.nextStamp, r.t, (long long)r.readStamp);
  check(r.any && r.curStamp == P && r.nextStamp == 2 * P && std::fabs(r.t - 0.25f) < 1e-4f &&
            r.readStamp == P + P / 4, buf);
  endRead();
  r = beginRead(5 * P);
  std::snprintf(buf, sizeof buf, "S3 time past the newest frame: cur %lld, no next, t %.3f, read stamp snaps back to %lld",
                (long long)r.curStamp, r.t, (long long)r.readStamp);
  check(r.any && r.curStamp == 2 * P && r.next == -1 && r.t == 0.f && r.readStamp == 2 * P, buf);
  endRead();
  // Fresh streams carry stamp 0 like the first frame, so the second write (pick = the FIRST
  // minimum stamp among state 2/3) recycled frame 0: the oldest ready frame is now P.
  r = beginRead(-5);
  std::snprintf(buf, sizeof buf, "S4 time before the oldest frame: next promoted to cur %lld, read %lld, t %.3f "
                "(frame 0 was recycled by the 2nd write: fresh streams also stamp 0)",
                (long long)r.curStamp, (long long)r.readStamp, r.t);
  check(r.any && r.curStamp == P && r.next == -1 && r.readStamp == P && r.t == 0.f, buf);
  endRead();
  // A 5th publish recycles the OLDEST ready record (pick = smallest stamp among state 2/3).
  publish(3 * P);
  publish(4 * P);
  r = beginRead(int64_t(3.5 * double(P)));
  std::snprintf(buf, sizeof buf, "S5 after 5 publishes into 4 records: cur %lld next %lld t %.3f (frame 0 recycled)",
                (long long)r.curStamp, (long long)r.nextStamp, r.t);
  check(r.curStamp == 3 * P && r.nextStamp == 4 * P && std::fabs(r.t - 0.5f) < 1e-3f, buf);
  // While cur/next are held (state 0), the writer must not take them.
  publish(5 * P);
  const int64_t held0 = int64_t(rd64(K::readGuestU32BE(kRecArr + 4u * uint32_t(r.cur))));
  const int64_t held1 = int64_t(rd64(K::readGuestU32BE(kRecArr + 4u * uint32_t(r.next))));
  check(held0 == 3 * P && held1 == 4 * P, "S6 the records being read are never the write target");
  endRead();
}

// ------------------------------------------------------------------ P: pacing ---------------
struct PaceResult {
  double presentsPerSec = 0, holdFrac = 0, errP95Ms = 0, stepP50Ms = 0, lagMeanMs = 0;
  double renderTicksPerSec = 0, simTicksPerSec = 0;
  size_t n = 0;
};

// A private KeTimeStampBundle: the guest clock sub_824680C0 reads *(*(0x820007D0) + 0x10), and
// pointing the cell here keeps any live host ticker (which writes 0xD0012000) out of the model.
constexpr uint32_t kBundle = 0x70705000u;
void setClockMs(uint64_t ms) { K::writeGuestBE32(kBundle + 0x10, uint32_t(ms)); }

bool timeGate(uint32_t mgr) {
  PPCContext c = mk();
  c.r3.u64 = mgr;
  sub_827206B8(c, base());
  return (c.r3.u32 & 0xFF) != 0;
}
void advance(uint32_t mgr) { PPCContext c = mk(); c.r3.u64 = mgr; sub_82720740(c, base()); }
void postTick(uint32_t mgr) { PPCContext c = mk(); c.r3.u64 = mgr; sub_82720858(c, base()); }

// Discrete-time model of the two GameKernelLoop threads (each spins on its TimeGate, as
// GameKernelLoop_Run does), stepping a 0.05 ms host clock that the guest sees through the
// KeTimeStampBundle millisecond. simWorkMs / renderWorkMs = each tick's busy time;
// simJitterMs adds a deterministic pseudo-random extra to the sim's work.
PaceResult runPacing(double renderHz, double simWorkMs, double renderWorkMs, double simJitterMs,
                     double seconds) {
  buildGroup();
  buildRenderMgr(kRenderMgr, renderHz);
  buildSimMgr(kSimMgr, 30.0);
  const double stepMs = 0.05;
  double simBusyUntil = 0, renderBusyUntil = 0;
  bool simPending = false;            // a frame opened (RenderFrameBegin) and not yet ended
  double simEndAt = 0;
  struct Present { double wall; int64_t shown; int64_t newest; };
  std::vector<Present> presents;
  bool renderPending = false;
  double renderPresentAt = 0;
  int64_t renderShown = 0;
  uint32_t lcg = 12345;
  int64_t newestPublished = -1;
  const uint32_t rt0 = 0, st0 = 0;
  (void)rt0; (void)st0;
  uint32_t renderTicks = 0, simTicks = 0;
  for (double w = 0; w < seconds * 1000.0; w += stepMs) {
    setClockMs(uint64_t(w));
    // sim thread
    if (simPending && w >= simEndAt) {
      PPCContext c = mk();
      c.r3.u64 = kGroup;
      sub_82787730(c, base());                          // RenderFrameEnd: publish
      newestPublished = int64_t(rd64(kGroup + 0x48));
      postTick(kSimMgr);                                 // sim time += 1/30 s
      simPending = false;
      simBusyUntil = w;
    }
    if (!simPending && w >= simBusyUntil && timeGate(kSimMgr)) {
      advance(kSimMgr);
      PPCContext c = mk();
      c.r3.u64 = kGroup;
      c.r4.u64 = rd64(kSimMgr + 0x50);                  // FeMasterTick: stamp = sim time
      sub_82787650(c, base());                           // RenderFrameBegin
      lcg = lcg * 1103515245u + 12345u;
      const double jitter = simJitterMs * double((lcg >> 8) & 0xFFFF) / 65535.0;
      simEndAt = w + simWorkMs + jitter;
      simPending = true;
      simTicks++;
    }
    // render thread
    if (renderPending && w >= renderPresentAt) {
      presents.push_back({w, renderShown, newestPublished});
      postTick(kRenderMgr);                              // render time += dt (elapsed)
      renderPending = false;
      renderBusyUntil = w;
    }
    if (!renderPending && w >= renderBusyUntil && timeGate(kRenderMgr)) {
      advance(kRenderMgr);
      const Read r = beginRead(int64_t(rd64(kRenderMgr + 0x50)));
      if (r.any) {
        wr64(kRenderMgr + 0x50, uint64_t(r.readStamp));  // FSGameRenderMain_Tick: std r11,0x50
        renderShown = r.readStamp;
        endRead();
        renderPresentAt = w + renderWorkMs;
        renderPending = true;
      } else {
        postTick(kRenderMgr);
      }
      renderTicks++;
    }
  }
  PaceResult out;
  // steady state: drop the first second
  std::vector<Present> s;
  for (const auto& p : presents) if (p.wall >= 1000.0) s.push_back(p);
  if (s.size() < 3) return out;
  const double dur = (s.back().wall - s.front().wall) / 1000.0;
  out.n = s.size();
  out.presentsPerSec = double(s.size() - 1) / dur;
  out.renderTicksPerSec = double(renderTicks) / seconds;
  out.simTicksPerSec = double(simTicks) / seconds;
  std::vector<double> err, step;
  size_t holds = 0;
  double lagSum = 0;
  for (size_t i = 1; i < s.size(); i++) {
    const double dShown = double(s[i].shown - s[i - 1].shown) * 1e-6;
    const double dWall = s[i].wall - s[i - 1].wall;
    if (s[i].shown == s[i - 1].shown) holds++;
    err.push_back(std::fabs(dShown - dWall));
    step.push_back(dShown);
    lagSum += double(s[i].newest - s[i].shown) * 1e-6;
  }
  std::sort(err.begin(), err.end());
  std::sort(step.begin(), step.end());
  out.holdFrac = double(holds) / double(err.size());
  out.errP95Ms = err[size_t(0.95 * double(err.size() - 1))];
  out.stepP50Ms = step[step.size() / 2];
  out.lagMeanMs = lagSum / double(err.size());
  return out;
}

void pacingTests() {
  std::printf("[P] pacing: the real GameKernelLoop + StateStream on a simulated 1 ms guest clock\n");
  TB::reinitForTest(true);
  const uint32_t savedCell = K::readGuestU32BE(0x820007D0u);
  K::writeGuestBE32(0x820007D0u, kBundle);
  TB::setBundlePublished(true);
  setClockMs(0);
  {
    PPCContext c = mk();
    sub_824680C0(c, base());
    check(c.r3.u32 == 0, "P0 the guest clock reads the test's private bundle (sub_824680C0)");
    setClockMs(1234);
    c = mk();
    sub_824680C0(c, base());
    check(c.r3.u32 == 1234, "P0b ... and follows it (1234 ms)");
  }
  char buf[300];
  struct Case { double hz, simWork, renderWork, jitter; const char* what; };
  const Case cases[] = {
      {30, 10, 5, 0, "console rates"},
      {60, 10, 5, 0, ""},
      {120, 10, 3, 0, ""},
      {144, 10, 3, 0, ""},
      {240, 10, 2, 0, ""},
      {120, 10, 3, 6, "sim work jitters 10-16 ms"},
  };
  PaceResult base30{};
  for (const auto& k : cases) {
    const PaceResult r = runPacing(k.hz, k.simWork, k.renderWork, k.jitter, 6.0);
    std::snprintf(buf, sizeof buf,
                  "P render %.0f Hz %s: %.1f presents/s, shown step p50 %.2f ms, |step-wall| p95 %.2f ms, "
                  "held %.1f%%, lag %.1f ms, sim %.1f ticks/s",
                  k.hz, k.what, r.presentsPerSec, r.stepP50Ms, r.errP95Ms, 100.0 * r.holdFrac, r.lagMeanMs,
                  r.simTicksPerSec);
    if (k.hz == 30 && k.jitter == 0) base30 = r;
    // The sim must stay at 30 ticks/s whatever the render rate (it never sees the change).
    const bool simOk = std::fabs(r.simTicksPerSec - 30.0) < 0.6;
    const bool rateOk = r.presentsPerSec > 0.97 * k.hz && r.presentsPerSec < 1.03 * k.hz;
    // Smooth: at most a few percent held frames, and the shown game time tracks the wall clock
    // to within a millisecond and a half (the guest clock's own 1 ms granularity).
    const bool smooth = k.jitter == 0 ? (r.holdFrac < 0.05 && r.errP95Ms < 1.6) : (r.holdFrac < 0.15);
    check(simOk && rateOk && smooth, buf);
  }
  (void)base30;
  // Mutation check: the metric must SEE judder. A sim that cannot keep up (45 ms of work per
  // 33 ms tick) publishes late, the render clock overtakes the newest frame and snaps back:
  // held frames, and fewer sim ticks per second. If this passed as "smooth" the P checks
  // above would prove nothing.
  {
    const PaceResult r = runPacing(120, 45, 3, 0, 6.0);
    std::snprintf(buf, sizeof buf,
                  "Pm mutation (sim 45 ms/tick): %.1f presents/s, held %.1f%%, sim %.1f ticks/s - the metric sees it",
                  r.presentsPerSec, 100.0 * r.holdFrac, r.simTicksPerSec);
    check(r.holdFrac > 0.05 && r.simTicksPerSec < 25.0, buf);   // on-time cases: 0.0% held
  }
  TB::setBundlePublished(false);
  K::writeGuestBE32(0x820007D0u, savedCell);
}

}  // namespace

int runHighfpsTest() {
  std::printf("[HIGHFPS_TEST] PORT FEATURE high frame rate: config, real render mgr, real StateStream, pacing\n");
  // iter-465: the player default is now 120 (kPlayerDefault), and this test process is a
  // "player" (no SR_INSTANCE_ID). The R/P cases measure the game's own ctor and loops with the
  // override as a pass-through, so pin it OFF before config() is resolved (the A/B switch).
  if (!std::getenv("SR_HIGHFPS")) _putenv_s("SR_HIGHFPS", "0");
  K::registerStubs();
  // The image statics these functions read (difftest has no XEX image; values from the golden
  // sound_gameplay dump): CS deadlines -1 (blocking), the 0.0f BeginRead stores as t, and the
  // GameKernelLoop constants 1e9f (SetTickRate period), 0.5f (+0.5 rounding, the 0.5 s stats
  // window), 1e-9 (ns -> s for the payload dt).
  K::writeGuestBE32(0x820995F4u, 0xFFFFFFFFu);
  K::writeGuestBE32(0x8208CA48u, 0xFFFFFFFFu);
  K::writeGuestBE32(0x820013B8u, 0);
  K::writeGuestBE32(0x82074B70u, 0x4E6E6B28u);            // 1e9f
  K::writeGuestBE32(0x820037C8u, 0x3F000000u);            // 0.5f
  wr64(0x82003CA8u, 0x3E112E0BE826D695ull);               // 1e-9
  wr64(0x8208CA98u, 0x3E112E0BE826D695ull);               // 1e-9
  configTests();
  renderMgrTests();
  liveTests();
  stateStreamTests();
  pacingTests();
  std::printf("[HIGHFPS_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
