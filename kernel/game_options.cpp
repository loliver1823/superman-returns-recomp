// lane E L-b (step 5): THE GAME TAB of OPTIONS - the host half. Contract: game_options.hpp.
#include "game_options.hpp"

#include "apt_call.hpp"
#include "guest_context.hpp"
#include "host_automation.hpp"
#include "mod_bridge.hpp"
#include "mod_library.hpp"   // the overlay's open state: the screen's close calls the opener back
#include "../app/log.hpp"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace sr {
namespace kernel {
namespace gameopts {

namespace {

// The packed fields (kernel/mod_bridge.hpp), one per game row.
struct Field {
  const char* id;
  int shift, bits, maxv;
  bool toggle;
};
const Field kFields[] = {
    {kInvertY, 11, 1, 1, true},  {kAutoCenter, 12, 1, 1, true}, {kVibration, 13, 1, 1, true},
    {kScheme, 8, 2, 3, false},   {kMusic, 0, 4, 10, false},     {kSfx, 4, 4, 10, false},
};
constexpr int32_t kGameBits = (1 << 14) - 1;   // bits 0-13: the game's own options
// The console's options before any change (ProfileSettings_applyOptions with the Xenia profile defaults,
// docs/CUSTOM_FRONTEND.md 0): auto-centre forced on, vibration on, scheme 0, no inversion; volumes 10 / 10.
constexpr int32_t kDefaults = 10 | (10 << 4) | (1 << 12) | (1 << 13);

const Field* field(const std::string& id) {
  for (const Field& f : kFields)
    if (id == f.id) return &f;
  return nullptr;
}

std::mutex g_mu;
int32_t g_live = -1;      // the game's options as last reported (bits 0-13), -1 = not yet
bool g_liveVolumes = false;   // ... and its volume bits were the game's (the mixer existed)
int32_t g_shown = -1;     // the value the game should have now (live + the screen's previews)
std::string g_target = "_global.SRFrontEnd";
Pending g_pending;
bool g_awaitClose = false;   // the screen was opened: SR_OptionsClosed is due once the overlay is gone
int g_closeWhat = 0;         // 0 back to the menu, 1 VIEW PAD LAYOUT, 2 DEV TOOLS
int g_volTest = -1;
int32_t g_storeTest = INT32_MIN;
bool g_storeFailTest = false;

int32_t storePacked() {
  if (g_storeTest != INT32_MIN) return g_storeTest;
  return modSettingsLoadPacked();
}

// The base value: the game's live options, else the store's, else the console's defaults. Volumes the game did
// not report come from the store (or the defaults).
int32_t baseLocked() {
  const int32_t st = storePacked();
  const int32_t stGame = st >= 0 ? (st & kGameBits) : kDefaults;
  if (g_live < 0) return stGame;
  if (g_liveVolumes) return g_live;
  return (g_live & ~0xFF) | (stGame & 0xFF);
}

bool readU32(uint32_t va, uint32_t* out) {
  if (!isGuestVAInRange(va)) return false;
  const uint8_t* p = guestVAToHost(va);
  if (!p || !hostRangeReadable(p, 4)) return false;
  *out = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  return true;
}

// The game's mixer exists (g_mu held: the test seam is read here, the guest globals are read-only).
bool volumesAvailableLocked() {
  if (g_volTest >= 0) return g_volTest != 0;
  // the volume getters read the audio mixer (sub_822AD1D0: dword_82976054 && dword_82976070), as mod_bridge's
  // SR_SetSettings check
  uint32_t mixer = 0, names = 0;
  readU32(0x82976054u, &mixer);
  readU32(0x82976070u, &names);
  return mixer != 0 && names != 0;
}

// Queue the game's options = packed (bits 0-13); stored = the store's new value after an APPLY (-1 = a preview).
void queueLocked(int32_t packed, int32_t stored) {
  const int32_t st = storePacked();
  int32_t full = packed & kGameBits;
  // bit 14 apply_to_loaded_saves: the store's (default 1); bit 15: the volumes are applied only while the game's
  // mixer exists (ApplyStore leaves them alone otherwise, as it does for a store without volumes)
  full |= (st < 0 || (st & int32_t(kModSettingsApplyToLoadedBit))) ? int32_t(kModSettingsApplyToLoadedBit) : 0;
  if (volumesAvailableLocked()) full |= int32_t(kModSettingsVolumesKnownBit);
  g_pending.apply = true;
  g_pending.packed = full;
  if (stored >= 0) g_pending.stored = stored;   // an APPLY's store survives a later preview in the same pump
  g_pending.target = g_target;
}

}  // namespace

bool isGameOption(const std::string& id) { return field(id) != nullptr; }
bool isAutosave(const std::string& id) { return id == kAutosave; }

bool enabled() { return automation::portFeaturesOn(); }

bool volumesAvailable() {
  std::lock_guard<std::mutex> lk(g_mu);
  return volumesAvailableLocked();
}

int32_t packWith(int32_t packed, const std::string& id, const std::string& v) {
  const Field* f = field(id);
  if (!f || packed < 0) return -1;
  int n = -1;
  if (f->toggle) {
    if (v == "on") n = 1;
    else if (v == "off") n = 0;
  } else {
    char* end = nullptr;
    const long x = std::strtol(v.c_str(), &end, 10);
    if (end && end != v.c_str() && *end == '\0' && x >= 0 && x <= f->maxv) n = int(x);
  }
  if (n < 0) return -1;
  const int32_t mask = ((1 << f->bits) - 1) << f->shift;
  return (packed & ~mask) | (int32_t(n) << f->shift);
}

std::string unpack(int32_t packed, const std::string& id) {
  const Field* f = field(id);
  if (!f || packed < 0) return {};
  const int n = (packed >> f->shift) & ((1 << f->bits) - 1);
  if (f->toggle) return n ? "on" : "off";
  return std::to_string(n);
}

std::string value(const std::string& id) {
  if (isAutosave(id)) return modOptionGet("autosave_without_asking") == 0 ? "off" : "on";
  std::lock_guard<std::mutex> lk(g_mu);
  return unpack(baseLocked(), id);
}

bool preview(const std::string& id, const std::string& v) {
  if (!enabled()) return false;
  if (isAutosave(id)) return v == "on" || v == "off";   // takes effect when stored (at the next save prompt)
  std::lock_guard<std::mutex> lk(g_mu);
  const int32_t p = packWith(g_shown >= 0 ? g_shown : baseLocked(), id, v);
  if (p < 0) return false;
  if (p == g_shown) return true;
  g_shown = p;
  queueLocked(p, -1);
  return true;
}

bool commit(const std::string& id, const std::string& v, bool store, std::string* err) {
  if (!enabled()) {
    if (err) *err = "port features are off";
    return false;
  }
  if (isAutosave(id)) {
    if (v != "on" && v != "off") return false;
    if (store && !modOptionSet("autosave_without_asking", v == "on" ? 1 : 0)) {
      if (err) *err = "could not be saved";
      return false;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_pending.fetchModOptions = true;
    g_pending.target = g_target;
    return true;
  }
  std::lock_guard<std::mutex> lk(g_mu);
  // the committed baseline with THIS row only (the review's P2-1): other rows' previews are not stored by it, and a
  // write that fails changes neither the store nor the baseline (REVERT then sends the real stored value back)
  const int32_t p = packWith(baseLocked(), id, v);
  const int32_t shown = packWith(g_shown >= 0 ? g_shown : baseLocked(), id, v);
  if (p < 0 || shown < 0) {
    if (err) *err = "bad value";
    return false;
  }
  int32_t stored = -1;
  if (store) {
    // the store an options screen's capture writes (mod_bridge SR_SetSettings), so the next NEW GAME / load
    // re-applies this value; the game's volumes only while its mixer exists
    const bool vol = volumesAvailableLocked();
    int32_t canonical = kModSettingsNone;
    bool ok = false;
    if (g_storeTest != INT32_MIN) {   // the test seam: an in-memory store (that can refuse)
      ok = !g_storeFailTest;
      if (ok) {
        canonical = (p & kGameBits) | int32_t(kModSettingsVolumesKnownBit) | int32_t(kModSettingsApplyToLoadedBit);
        g_storeTest = canonical;
      }
    } else {
      ok = modSettingsSavePacked(p & kGameBits, vol, &canonical);
    }
    if (!ok) {
      if (err) *err = "could not be saved";
      return false;
    }
    stored = canonical;
  }
  // the baseline advances by this row; the game keeps showing every row's current value (other previews included)
  g_live = p;
  g_liveVolumes = true;
  g_shown = shown;
  queueLocked(shown, stored);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[GAME_OPTIONS] %s = %s (%s)", id.c_str(), v.c_str(),
              store ? "stored, the game's setters next" : "the game's setters next, not stored");
  return true;
}

bool requestPadLayout(std::string* msg) {
  if (!enabled()) {
    if (msg) *msg = "not available";
    return false;
  }
  std::lock_guard<std::mutex> lk(g_mu);
  g_closeWhat = 1;
  if (msg) *msg = "the game's pad layout";
  return true;
}

bool requestDevTools(std::string* msg) {
  if (!enabled()) {
    if (msg) *msg = "not available";
    return false;
  }
  std::lock_guard<std::mutex> lk(g_mu);
  g_closeWhat = 2;
  if (msg) *msg = "dev tools";
  return true;
}

namespace {
int takeCloseCallLocked(bool overlayOpen) {
  if (!g_awaitClose || overlayOpen) return -1;
  g_awaitClose = false;
  const int what = g_closeWhat;
  g_closeWhat = 0;
  return what;
}
}  // namespace

int takeCloseCallForTest(bool overlayOpen) {
  std::lock_guard<std::mutex> lk(g_mu);
  return takeCloseCallLocked(overlayOpen);
}

void noteOpen(const std::string& ctx) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_target = ctx == "IG" ? "_global.SRPause" : "_global.SRFrontEnd";
  g_shown = -1;
  g_pending = Pending{};
  g_awaitClose = true;
  g_closeWhat = 0;
}

void noteLive(int32_t packed, bool volumesReadable) {
  if (packed < 0) return;
  std::lock_guard<std::mutex> lk(g_mu);
  g_live = packed & kGameBits;
  g_liveVolumes = volumesReadable;
  g_shown = -1;
}

void afterAptPump(PPCContext& ctx, uint8_t* base) {
  if (!enabled()) return;
  Pending p;
  int closeWhat = -1;
  std::string closeTarget;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_pending.apply && !g_pending.fetchModOptions && !g_awaitClose) return;
    p = g_pending;
    g_pending = Pending{};
    closeWhat = takeCloseCallLocked(modlib::isOpen());
    closeTarget = g_target;
  }
  if (p.apply) {
    const bool ok = aptcall::callPath(ctx, base, p.target.c_str(), "SR_GameOptions",
                                      {aptcall::Arg::Int(p.packed), aptcall::Arg::Int(p.stored)});
    SR_LOG_BOOT(sr::LogSeverity::Info, "[GAME_OPTIONS] %s.SR_GameOptions(%d, %d) %s (invY=%d autoCenter=%d vib=%d "
                "scheme=%d music=%d sfx=%d volumesKnown=%d)", p.target.c_str(), p.packed, p.stored,
                ok ? "called" : "NOT called (no executor)", (p.packed >> 11) & 1, (p.packed >> 12) & 1,
                (p.packed >> 13) & 1, (p.packed >> 8) & 3, p.packed & 15, (p.packed >> 4) & 15, (p.packed >> 15) & 1);
  }
  if (p.fetchModOptions) {
    const bool ok = aptcall::callPath(ctx, base, p.target.c_str(), "FetchModOptions", {});
    SR_LOG_BOOT(sr::LogSeverity::Info, "[GAME_OPTIONS] %s.FetchModOptions() %s", p.target.c_str(),
                ok ? "called" : "NOT called (no executor)");
  }
  if (closeWhat >= 0) {
    // after the screen's last SR_GameOptions (the REVERT of a close without APPLY above), the opener takes over again
    const bool ok = aptcall::callPath(ctx, base, closeTarget.c_str(), "SR_OptionsClosed", {aptcall::Arg::Int(closeWhat)});
    SR_LOG_BOOT(sr::LogSeverity::Info, "[GAME_OPTIONS] %s.SR_OptionsClosed(%d: %s) %s", closeTarget.c_str(), closeWhat,
                closeWhat == 1 ? "VIEW PAD LAYOUT" : closeWhat == 2 ? "DEV TOOLS" : "back to the menu",
                ok ? "called" : "NOT called (no executor)");
  }
}

Pending takePendingForTest() {
  std::lock_guard<std::mutex> lk(g_mu);
  Pending p = g_pending;
  g_pending = Pending{};
  return p;
}

void resetForTest() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_live = -1;
  g_liveVolumes = false;
  g_shown = -1;
  g_target = "_global.SRFrontEnd";
  g_pending = Pending{};
  g_awaitClose = false;
  g_closeWhat = 0;
  g_volTest = -1;
  g_storeTest = INT32_MIN;
  g_storeFailTest = false;
}

void setVolumesForTest(int mode) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_volTest = mode;
}

void setStoreForTest(int32_t packed) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_storeTest = packed;
}

void setStoreFailForTest(bool fail) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_storeFailTest = fail;
}

int selfTest() {
  int fails = 0;
  auto ck = [&](bool c, const char* what) {
    if (!c) {
      std::fprintf(stderr, "FAIL: gameopts %s\n", what);
      ++fails;
    }
  };
  ck(packWith(0, kInvertY, "on") == (1 << 11), "invert Y on");
  ck(packWith(1 << 11, kInvertY, "off") == 0, "invert Y off");
  ck(packWith(0, kScheme, "3") == (3 << 8), "scheme 3");
  ck(packWith(0, kScheme, "4") == -1, "scheme 4 refused");
  ck(packWith(0, kMusic, "10") == 10, "music 10");
  ck(packWith(0, kMusic, "11") == -1, "music 11 refused");
  ck(packWith(0, kSfx, "7") == (7 << 4), "sfx 7");
  ck(packWith(0, kSfx, "x") == -1, "sfx text refused");
  ck(packWith(0, kAutosave, "on") == -1, "autosave is no packed field");
  ck(unpack(packWith(kDefaults, kVibration, "off"), kVibration) == "off", "vibration round trip");
  ck(unpack(kDefaults, kAutoCenter) == "on", "default auto-centre");
  ck(unpack(kDefaults, kMusic) == "10", "default music");
  return fails;
}

}  // namespace gameopts
}  // namespace kernel
}  // namespace sr
