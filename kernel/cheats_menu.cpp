// iter-465 charswitch: CHEATS - the host half. Contract: cheats_menu.hpp.
#include "cheats_menu.hpp"

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "host_automation.hpp"
#include "host_pages.hpp"
#include "../app/log.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace sr {
namespace kernel {

namespace {

using Row = HostPageRow;
using Page = HostPage;
using Out = HostPageOut;

std::string g_ctx = "IG";

// The cheat manager (*0x8296DF30): +0 the cheat array (CheatDefs order), +4 its count; a cheat:
// +0 buttons*, +4 length, +12 code, +16 ON byte. The world flow manager *0x8296DF74; the
// objective-status world *0x8296DF5C.
constexpr uint32_t G_CHEAT_MGR = 0x8296DF30u, G_FLOW_MGR = 0x8296DF74u, G_OBJ_WORLD = 0x8296DF5Cu;
// "objectiveActivated", the string the matcher hands to sub_826CD320 (0x82237508: lis/addi).
constexpr uint32_t S_OBJECTIVE_ACTIVATED = 0x8200A07Cu;
// The save's unlock bitfields, for the [CHEATS] state line (the game's own attribute names).
constexpr uint32_t S_COSTUME = 0x8200DFC8u, S_TROPHIES = 0x8200DFD8u, S_VIDEOS = 0x8200DFECu,
                   S_POWERUPS = 0x8200DFFCu;

std::mutex g_mu;
std::vector<int> g_requests;   // the codes CHEATS rows asked for, run one per UI tick
std::string g_note;    // shown on the page: the game did not take a code

uint32_t rd(uint32_t va) {
  if (va < 0x10000u || !isGuestVAInRange(va) || !hostRangeReadable(guestVAToHost(va), 4)) return 0;
  return readGuestU32BE(va);
}
uint8_t rd8(uint32_t va) {
  if (va < 0x10000u || !isGuestVAInRange(va) || !hostRangeReadable(guestVAToHost(va), 1)) return 0;
  return *guestVAToHost(va);
}

// One guest call on the game's own UI-tick thread, as a callee of the hooked tick: every register
// saved, the arguments set, r1 moved 0x200 below the caller's frame, every register restored after
// (the devtools fcCall pattern). Returns the callee's r3.
uint32_t call(PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext& __restrict, uint8_t*), uint32_t r3,
              uint32_t r4 = 0, uint32_t r5 = 0) {
  const PPCContext saved = ctx;
  ctx.r1.u64 = (saved.r1.u32 - 0x200u) & ~0xFull;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  fn(ctx, base);
  const uint32_t ret = ctx.r3.u32;
  ctx = saved;
  return ret;
}

// The cheat's index in the manager's array (what the matcher passes to sub_82236840), -1 if none.
int cheatIndex(uint32_t mgr, int code) {
  const uint32_t arr = rd(mgr), count = rd(mgr + 4u);
  if (!arr || count > 64) return -1;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t node = rd(arr + 4u * i);
    if (node && int(rd(node + 12u)) == code) return int(i);
  }
  return -1;
}

// ON bits of every cheat by code (bit n = code n), read fault-free.
uint32_t onMask(uint32_t mgr) {
  const uint32_t arr = rd(mgr), count = rd(mgr + 4u);
  uint32_t m = 0;
  if (!arr || count > 64) return 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t node = rd(arr + 4u * i), code = node ? rd(node + 12u) : 0;
    if (code < 32 && rd8(node + 16u)) m |= 1u << code;
  }
  return m;
}

// A BaseWorldFlow DAS attribute's value the way the matcher reads objectiveActivated:
// sub_82194E48(*(flow+104), sub_826CD320(name)) -> +4. -1 when the flow or the attribute is missing.
int64_t flowValue(PPCContext& ctx, uint8_t* base, uint32_t nameVA) {
  const uint32_t flow = call(ctx, base, sub_821F68E8, rd(G_FLOW_MGR));
  if (!flow) return -1;
  const uint32_t das = rd(flow + 104u);
  if (!das) return -1;
  const uint32_t key = call(ctx, base, sub_826CD320, nameVA);
  const uint32_t rec = call(ctx, base, sub_82194E48, das, key);
  return rec ? int64_t(rd(rec + 4u)) : -1;
}

// The matcher's own condition (CheatMgr_FeedPadButton_sub_82237360, 0x822374F4..0x82237560), in its
// order: the world flow's objectiveActivated exists and is 0, then objectiveStatus bit 6 (WARobj02)
// through WorldFlow_IsObjectiveStatusBitSet_sub_82224EE0(*0x8296DF5C, 6).
bool gameTakesCheats(PPCContext& ctx, uint8_t* base, int64_t* objActive, int* warDone) {
  *objActive = flowValue(ctx, base, S_OBJECTIVE_ACTIVATED);
  *warDone = -1;
  if (*objActive != 0) return false;
  *warDone = (call(ctx, base, sub_82224EE0, rd(G_OBJ_WORLD), 6u) & 0xFFu) ? 1 : 0;
  return *warDone == 1;
}

void logState(PPCContext& ctx, uint8_t* base, uint32_t mgr, const char* why) {
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[CHEATS] state (%s): on=%08X costumeUnlocked=%08llX trophiesUnlocked=%08llX videosUnlocked=%08llX "
              "powerUpsUnlocked=%08llX",
              why, onMask(mgr), (unsigned long long)flowValue(ctx, base, S_COSTUME),
              (unsigned long long)flowValue(ctx, base, S_TROPHIES), (unsigned long long)flowValue(ctx, base, S_VIDEOS),
              (unsigned long long)flowValue(ctx, base, S_POWERUPS));
}

bool portFeaturesOn() {
  static const bool on =
      automation::portFeaturesFor(std::getenv("SR_INSTANCE_ID"), std::getenv("SR_PORT_FEATURES"));
  return on;
}

bool queueCode(int code) {
  if (!portFeaturesOn()) return false;
  std::lock_guard<std::mutex> lk(g_mu);
  g_requests.push_back(code);
  g_note.clear();
  SR_LOG_BOOT(sr::LogSeverity::Info, "[CHEATS] code %d chosen (state before %d)", code, modCheatState(code));
  return true;
}

struct Cheat { int code; const char* label; };
const Cheat kPage0[] = {{1, "INFINITE STAMINA"}, {2, "INFINITE CITY HEALTH"}, {4, "ALL POWER UPGRADES"}};
const Cheat kPage1[] = {{6, "FAST CITY"}, {7, "LOW GRAVITY"}, {8, "BIZARRO UNLEASHED"}};
// SUPERMAN_SKIN1..5: the retail joke disguises (sub_82236840 cases 9-13)
const char* const kDisguises[] = {"NONE", "PHONE BOOTH", "VENDING MACHINE", "VENDING MACHINE 2", "PLANTER", "ICE CHEST"};

std::string onOff(int code) {
  const int s = modCheatState(code);
  return s < 0 ? "-" : (s ? "ON" : "OFF");
}

// A row's choice closes the page (the Apt page's own back; in game the OPTIONS CHEATS tab is the route since lane E L-c); the next UI tick
// applies the cheat through the game's own functions (cheatsMenuOnUiTick).
void enter(int code, Out& o) {
  queueCode(code);
  o.action = 0;
}

Row switchRow(const Cheat& c) {
  Row r;
  r.label = std::string(c.label) + ": " + onOff(c.code);
  r.kind = DevMenuKind::Toggle;
  const int code = c.code;
  r.press = [code](Out& o) { enter(code, o); };
  r.adjust = [code](Out& o, int) { enter(code, o); };
  return r;
}

int disguiseOn() {
  for (int i = 0; i < 5; ++i)
    if (modCheatState(9 + i) == 1) return i + 1;
  return 0;
}

const char* const kHow =
    "Choosing a cheat applies it the way the game's own button code does. The game takes cheats between"
    " missions, after Warworld, and plays a sound when it does.";

Page pageMain() {
  Page p;
  p.id = "gamecheats";
  p.title = "CHEATS";
  p.body = std::string("UNLOCK ALL SUITS gives this save every suit, trophy and theater item (kept when you save)."
                       " The others switch on and off.\n") + kHow;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_note.empty()) p.body = g_note + "\n\n" + p.body;
  }
  Row all;
  const bool done = modCheatState(3) == 1;
  all.label = done ? "UNLOCK ALL SUITS: DONE" : "UNLOCK ALL SUITS";
  all.kind = DevMenuKind::Action;
  all.press = [done](Out& o) {
    if (done) { o.page = "gamecheats"; o.focus = 0; o.action = 2; return; }   // entering it again only clears the flag
    enter(3, o);
  };
  p.rows.push_back(all);
  for (const auto& c : kPage0) p.rows.push_back(switchRow(c));
  Row more;
  more.label = "MORE CHEATS";
  more.press = [](Out& o) { o.page = "gamecheats:1"; o.focus = 0; };
  p.rows.push_back(more);
  return p;
}

Page pageMore() {
  Page p;
  p.id = "gamecheats:1";
  p.title = "CHEATS";
  p.body = kHow;
  for (const auto& c : kPage1) p.rows.push_back(switchRow(c));
  Row dis;
  dis.label = std::string("DISGUISE: ") + kDisguises[disguiseOn()];
  dis.kind = DevMenuKind::Toggle;
  auto step = [](Out& o, int delta) {
    const int now = disguiseOn();
    const int next = (now + (delta < 0 ? 5 : 1)) % 6;
    if (now) queueCode(8 + now);    // the game's own OFF for the current one
    if (next) queueCode(8 + next);
    o.action = 0;
  };
  dis.adjust = step;
  dis.press = [step](Out& o) { step(o, 1); };
  p.rows.push_back(dis);
  Row back;
  back.label = "BACK";
  back.press = [](Out& o) { o.page = "gamecheats"; o.focus = 4; };
  p.rows.push_back(back);
  p.back = [](Out& o) { o.page = "gamecheats"; o.focus = 4; };
  return p;
}

}  // namespace

std::vector<CheatRow> cheatsMenuRows() {
  std::vector<CheatRow> v;
  if (!portFeaturesOn()) return v;
  std::string note;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    note = g_note;
  }
  const std::string tail = std::string(" ") + kHow + (note.empty() ? std::string() : "\n\n" + note);
  {
    CheatRow r;
    const bool done = modCheatState(3) == 1;
    r.id = "cheat_3";
    r.section = "UNLOCKS";
    r.label = done ? "UNLOCK ALL SUITS: DONE" : "UNLOCK ALL SUITS";
    r.verb = done ? "DONE" : "UNLOCK";
    r.desc = "Gives this save every suit, trophy and theater item (kept when you save)." + tail;
    v.push_back(r);
  }
  auto sw = [&](const Cheat& c) {
    CheatRow r;
    r.id = "cheat_" + std::to_string(c.code);
    r.section = "SWITCHES";
    r.label = std::string(c.label) + ": " + onOff(c.code);
    r.verb = modCheatState(c.code) == 1 ? "TURN OFF" : "TURN ON";
    r.desc = "The game's own cheat, switched on and off as its button code does." + tail;
    v.push_back(r);
  };
  for (const auto& c : kPage0) sw(c);
  for (const auto& c : kPage1) sw(c);
  {
    CheatRow r;
    r.id = "cheat_disguise";
    r.section = "SWITCHES";
    r.label = std::string("DISGUISE: ") + kDisguises[disguiseOn()];
    r.verb = "NEXT";
    r.desc = "The game's own joke disguises, one after the other, then none." + tail;
    v.push_back(r);
  }
  return v;
}

bool cheatsMenuRun(const std::string& id, std::string* msg) {
  if (!portFeaturesOn() || id.compare(0, 6, "cheat_") != 0) {
    if (msg) *msg = "not available";
    return false;
  }
  if (id == "cheat_disguise") {
    const int now = disguiseOn();
    const int next = (now + 1) % 6;
    if (now) queueCode(8 + now);   // the game's own OFF for the current one
    if (next) queueCode(8 + next);
    if (msg) *msg = std::string("disguise: ") + kDisguises[next];
    return true;
  }
  const int code = std::atoi(id.c_str() + 6);
  bool known = code == 3;
  for (const auto& c : kPage0) known = known || c.code == code;
  for (const auto& c : kPage1) known = known || c.code == code;
  if (!known) {
    if (msg) *msg = "not available";
    return false;
  }
  if (code == 3 && modCheatState(3) == 1) {   // entering it again only clears the flag (the page's rule)
    if (msg) *msg = "all suits are already unlocked";
    return true;
  }
  queueCode(code);
  if (msg) *msg = "sent to the game";
  return true;
}

bool cheatsMenuOwnsPage(const std::string& page) { return page == "gamecheats" || page == "gamecheats:1"; }

void cheatsMenuCommand(const std::string& args, std::vector<ModReply>* out) {
  hostPagesCommand(args, [](const std::string& id) { return id == "gamecheats:1" ? pageMore() : pageMain(); },
                   out, &g_ctx, "CHEATS");
}

void cheatsMenuOnUiTick(PPCContext& ctx, uint8_t* base) {
  if (!portFeaturesOn()) return;
  const uint32_t mgr = rd(G_CHEAT_MGR);
  if (!mgr) return;
  // the [CHEATS] state line whenever a cheat's ON flag changes, however it was entered (the pad's
  // own code or a CHEATS row) - the parity record (archive/v1_charswitch/cheat_parity.py)
  static uint32_t s_last = 0xFFFFFFFFu;
  static int s_settle = -1;
  const uint32_t m = onMask(mgr);
  if (s_last != 0xFFFFFFFFu && m != s_last) {
    logState(ctx, base, mgr, "changed");
    s_settle = 60;   // and once more ~2 s later, after the effect's own follow-up writes
  }
  s_last = m;
  if (s_settle > 0 && --s_settle == 0) logState(ctx, base, mgr, "settled");
  int code = 0;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_requests.empty()) { code = g_requests.front(); g_requests.erase(g_requests.begin()); }
  }
  if (!code) return;
  const int idx = cheatIndex(mgr, code);
  int64_t objActive = -1;
  int warDone = -1;
  if (idx < 0 || !gameTakesCheats(ctx, base, &objActive, &warDone)) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[CHEATS] code %d refused by the game's own check (index %d, objectiveActivated %lld, "
                "WARobj02 %d)", code, idx, (long long)objActive, warDone);
    std::lock_guard<std::mutex> lk(g_mu);
    g_note = "The game did not take the last cheat: it takes cheats between missions, after Warworld.";
    return;
  }
  const int before = modCheatState(code);
  call(ctx, base, sub_82236840, mgr, uint32_t(idx), 1u);   // the matcher's own call, same arguments
  SR_LOG_BOOT(sr::LogSeverity::Info, "[CHEATS] code %d: sub_82236840(mgr, %d, 1): %d -> %d", code, idx, before,
              modCheatState(code));
}

}  // namespace kernel
}  // namespace sr
