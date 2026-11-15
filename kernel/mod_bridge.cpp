// iter-463 MOD FRAMEWORK: the Apt FSCommand -> host bridge (contract: mod_bridge.hpp,
// docs/CUSTOM_FRONTEND.md sections 0 and 9, docs/MODS.md "Host bridge").
//
// Charter: registered in docs/OVERRIDES.json as platform-fix. The console's Apt runtime
// handed fscommand() to a host-registered handler; this is that host. It is inert for
// retail content (0 FSCommands in ui.AST): every command that does not start with "SR_"
// runs the game's own body unchanged. SR_* commands are handled whenever they arrive,
// independent of which mod (or which hand-placed ui.AST) issued them: the game's own
// body would look an SR_* name up in the Apt registry, miss, and dereference the null
// result (a fatal host AV under the VEH), so there is no natural behaviour to preserve.
// SR_Quit / SR_Log call no guest function and write no guest state: an SR_* command
// returns to the interpreter exactly as a registry handler would (sub_82682DF8 ignores the
// result). SR_GetSettings and SR_SetSettings DO call guest code: the bridge's return
// channel answers the mod's own request (SR_GetSettings with the store, SR_SetSettings
// with the value as stored) by calling the AS method FlowMgr.SR_OnHostSettings(packed),
// which assigns _global.SRSettings. It writes a guest stack frame below r1 (back chain +
// the method name and target path strings), then calls AptAS2AllocIntObj and the
// executor invoke - the call the game's natives use to talk to AS (sub_822B15F8 ->
// AptExecutor vt+0x20 "FlowMgr"/"CppShowPopup") - on the guest thread that is already
// inside the interpreter. Only mod content reaches it (retail issues 0 FSCommands).
#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "mod_bridge.hpp"
#include "achievements.hpp"
#include "devtools.hpp"
#include "devtools_menu.hpp"
#include "host_pages.hpp"
#include "mod_costume_data.hpp"
#include "costume_viewer.hpp"
#include "map_markers.hpp"
#include "mod_manager.hpp"
#include "mod_library.hpp"    // lane E L-a: SR_Achievements opens the ACHIEVEMENTS page
#include "game_options.hpp"   // lane E L-b: the OPTIONS screen's GAME tab
#include "xam_content.hpp"
#include "../app/log.hpp"

#include <SDL3/SDL.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#endif

namespace sr {
namespace kernel {

namespace {
namespace fs = std::filesystem;
void (*g_quitForTest)() = nullptr;
std::atomic<uint32_t> g_cmdCount{0};
int g_mixerTestMode = -1;   // -1 read the guest, 0 absent, 1 present (difftest)

// Fault-free guest C-string read (the harness VEH makes any host AV fatal).
std::string readGuestCString(uint32_t va, size_t maxLen = 255) {
  std::string s;
  if (va == 0) return s;
  // One readability check per 4 KB page (VirtualQuery is not free), not per byte.
  uint32_t checkedUpTo = va;
  for (size_t i = 0; i < maxLen; ++i) {
    const uint32_t a = va + uint32_t(i);
    if (a >= checkedUpTo) {
      if (!isGuestVAInRange(a)) break;
      const uint32_t pageEnd = (a | 0xFFFu) + 1u;
      const uint8_t* p = guestVAToHost(a);
      if (!p || !hostRangeReadable(p, pageEnd - a)) break;
      checkedUpTo = pageEnd;
    }
    const uint8_t c = *guestVAToHost(a);
    if (c == 0) break;
    s.push_back(char(c));
  }
  return s;
}

bool readGuestU32(uint32_t va, uint32_t* out) {
  if (!isGuestVAInRange(va)) return false;
  const uint8_t* p = guestVAToHost(va);
  if (!p || !hostRangeReadable(p, 4)) return false;
  *out = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  return true;
}

bool guestRangeWritable(uint32_t va, size_t bytes) {
  if (!bytes || uint64_t(va) + bytes > 0x100000000ull || !isGuestVAInRange(va) ||
      !isGuestVAInRange(va + uint32_t(bytes - 1))) return false;
  auto* start = guestVAToHost(va);
  if (!start) return false;
#ifdef _WIN32
  uintptr_t cursor = reinterpret_cast<uintptr_t>(start);
  const uintptr_t end = cursor + bytes;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(cursor), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
      return false;
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (regionEnd <= cursor) return false;
    cursor = regionEnd;
  }
  return true;
#else
  // The current supported host is Windows. Do not guess write permissions on
  // another host; its implementation must provide the same fault-free check.
  return false;
#endif
}

bool costumeRequestNodeReady(uint32_t wrapper) {
  uint32_t root = 0, begin = 0, end = 0;
  if (!readGuestU32(wrapper + 4u, &root) || !root || root > 0xFFFFFFF8u || (root & 3u) ||
      !readGuestU32(root, &begin) || !readGuestU32(root + 4u, &end) ||
      !begin || begin >= end || ((begin | end) & 3u) || end - begin > 0x7FFFFFFFu ||
      !isGuestVAInRange(begin) || !isGuestVAInRange(end - 1) ||
      !hostRangeReadable(guestVAToHost(begin), end - begin)) return false;
  // Fault-free dry read of the exact search path taken by 821947D8. This only
  // validates the pointers/type; 821E6F90 still resolves and writes its own node.
  // FNV-1 (case-sensitive) of SetCostumeNum, not the flow-name hash31.
  constexpr uint32_t target = 0x2E196D85u;
  uint32_t lo = begin, hi = end;
  while (lo < hi) {
    const uint32_t mid = lo + 4u * ((hi - lo) >> 3);
    uint32_t node = 0, hash = 0, flags = 0;
    if (!readGuestU32(mid, &node) || !node || node > 0xFFFFFFF4u || (node & 3u) ||
        !readGuestU32(node, &hash) || !readGuestU32(node + 8u, &flags)) return false;
    if (hash == target)
      return ((flags >> 17) & 0x7Fu) == 0 && !(flags & 0x10000u) && guestRangeWritable(node + 4u, 4);
    if (hash < target) lo = mid + 4;
    else hi = mid;
  }
  return false;
}

// Shown in the Fortress COSTUMES description field for a suit of another animation family.
constexpr const char* kNextStartNote =
    "Superman wears this suit from the next start of the game: it moves with different animations.";

// Authored costume UI -> the SAME game-owned request used by RecordAdapter's
// SetActiveCostume (822B29E0). Only the stable-id -> appended DAS-slot lookup is
// new. There is no direct entity replacement, host scheduling or save mutation.
bool requestCostumeSlot(PPCContext* ctx, int slot, std::string* reason);

// iter-465 charswitch (owner request): WEAR switches without the retail "NEW COSTUME SELECTED!"
// popup, for every suit (the popup's close path only returns to the screen: sub_822B18E0 takes no
// callback). SR_COSTUME_POPUP=1 brings the retail popup back.
bool costumePopupWanted() {
  const char* gate = std::getenv("SR_COSTUME_POPUP");
  return gate && gate[0] == '1';
}

bool requestCostume(PPCContext* ctx, const std::string& id, std::string* reason) {
  auto refuse = [&](const char* message) { *reason = message; return false; };
  ModCostumeInfo costume;
  if (!modNativeCostumesEnabled()) return refuse("Native costume extension is not enabled.");
  if (!modCostumeFindActive(id, &costume)) return refuse("This suit is not available in the running game.");
  // A whole character on its own animation project (preloaded at boot with the animation memory
  // to hold it) is switched to at once by the actor flow's character switch, from any family.
  const bool character = (costume.character || !costume.animationProject.empty()) && modCharacterSwitchEnabled() &&
                         modCharacterAnimBytes() > 0;
  if (!character && modCostumeFamily(costume) != modCostumeSessionFamily()) {
    // A suit animates on its family's banks, and this session loaded another family's at start:
    // the choice is remembered and the next start loads its banks (no stretched or squashed body).
    const bool kept = modCostumeNoteWorn(costume.id);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[COSTUME] UI request %s: family %s, session %s -> %s", id.c_str(),
                modCostumeFamily(costume).c_str(), modCostumeSessionFamily().c_str(),
                kept ? "worn from the next start" : "not remembered");
    return refuse(kept ? kNextStartNote : "This suit could not be remembered.");
  }
  const bool queued = requestCostumeSlot(ctx, costume.slot, reason);
  if (queued) modCostumeNoteWorn(costume.id);
  *reason = queued ? costume.description : "The character is not ready for a costume change.";
  SR_LOG_BOOT(sr::LogSeverity::Info, "[COSTUME] UI request %s slot=%d -> %s", id.c_str(), costume.slot,
              queued ? "queued through retail DAS setter" : "refused by retail flow");
  return queued;
}

// The SAME game-owned request RecordAdapter.SetActiveCostume (822B29E0) makes: the actor flow's
// DAS SetCostumeNum := slot through the retail setter; the retail popup only if wanted.
bool requestCostumeSlot(PPCContext* ctx, int slot, std::string* reason) {
  auto refuse = [&](const char* message) { *reason = message; return false; };
  if (!modNativeCostumesEnabled()) return refuse("Native costume extension is not enabled.");
  if (!ctx || !g_dispatchTable) return refuse("No game interpreter is available.");
  uint8_t* base = getGuestBase();
  auto* find = PPC_LOOKUP_FUNC_SAFE(base, 0x821F6850u);
  auto* set = PPC_LOOKUP_FUNC_SAFE(base, 0x821E6F90u);
  if (!find || !set) return refuse("The costume request functions are unavailable.");
  // sub_821F6588 is the getter for this global. Validate everything the retail
  // FindFlowByName scans before calling it (the VEH cannot catch a bad read).
  uint32_t manager = 0, slots = 0;
  if (!readGuestU32(0x8296DF74u, &manager) || !manager ||
      !readGuestU32(manager + 0x20u, &slots) || !slots)
    return refuse("The character flow is not loaded.");
  for (uint32_t i = 0; i < 8; ++i) {
    uint32_t flow = 0, hash = 0;
    if (!readGuestU32(slots + 4 * i, &flow) || (flow && !readGuestU32(flow + 0x4Cu, &hash)))
      return refuse("The character flow is not readable.");
  }
  if (ctx->r1.u32 < 0x2000u) return refuse("The game stack is unavailable.");
  const uint32_t frame = (ctx->r1.u32 - 0x200u) & ~0xFu;
  if (!guestRangeWritable(frame, 0x200))
    return refuse("The game stack is unavailable.");
  const PPCContext saved = *ctx;
  writeGuestBE32(frame, saved.r1.u32);
  std::memcpy(guestVAToHost(frame + 0x80u), "SupermenActorWorldFlow", sizeof("SupermenActorWorldFlow"));
  std::memcpy(guestVAToHost(frame + 0xA0u), "SetCostumeNum", sizeof("SetCostumeNum"));
  ctx->r1.u32 = frame;
  ctx->r3.u64 = manager;
  ctx->r4.u64 = frame + 0x80u;
  find(*ctx, base);
  uint32_t wrapper = 0;
  const uint32_t flow = ctx->r3.u32;
  const bool ready = flow && readGuestU32(flow + 104u, &wrapper) && wrapper && costumeRequestNodeReady(wrapper);
  bool queued = false;
  if (ready) {
    ctx->r1.u32 = frame;
    ctx->r3.u64 = wrapper;
    ctx->r4.u64 = frame + 0xA0u;
    ctx->r5.s64 = slot;
    set(*ctx, base);
    queued = ctx->r3.u32 != 0;  // setter found its DAS node; not visual proof
  }
  // Retail SetActiveCostume's own confirmation, the same call it makes after its DAS write:
  // sub_822B18E0(*0x8296EE90 FlowMgr, off_82913718 "Costume_Wearing_MessageBox_Body",
  // off_82913714 "..._Title") -> FlowMgr_CppShowPopup ("NEW COSTUME SELECTED!").
  uint32_t flowMgr = 0, body = 0, title = 0;
  auto* popup = PPC_LOOKUP_FUNC_SAFE(base, 0x822B18E0u);
  if (queued && costumePopupWanted() && popup && readGuestU32(0x8296EE90u, &flowMgr) && flowMgr &&
      readGuestU32(0x82913718u, &body) && body && readGuestU32(0x82913714u, &title) && title) {
    ctx->r1.u32 = frame;
    ctx->r3.u64 = flowMgr;
    ctx->r4.u64 = body;
    ctx->r5.u64 = title;
    popup(*ctx, base);
  }
  *ctx = saved;
  return queued;
}

// The save flow keeps the id of the C++ popup it waits for at saveFlow+16
// (SaveFlow_ShowLoadCreateProfilePopup_sub_822B4C50: *(a1+16) = sub_822B15F8(...)).
std::string pendingPopupNote() {
  uint32_t flow = 0, id = 0;
  if (!readGuestU32(0x8296EE8Cu, &flow) || flow == 0) return " saveFlow=null";
  if (!readGuestU32(flow + 16u, &id)) return " saveFlow+16=unreadable";
  char b[64];
  std::snprintf(b, sizeof b, " saveFlow+16=%u(0x%08X)", id, id);
  return b;
}

// ── the universal options store (mod_bridge.hpp) ─────────────────────────────────────
struct Opt { const char* key; int shift; int bits; int maxv; };
// Order = the file's line order; maxv inclusive.
constexpr Opt kOpts[] = {
    {"music_volume", 0, 4, 10}, {"sfx_volume", 4, 4, 10}, {"control_scheme", 8, 2, 3},
    {"invert_x", 10, 1, 1},     {"invert_y", 11, 1, 1},   {"auto_center", 12, 1, 1},
    {"vibration", 13, 1, 1},    {"apply_to_loaded_saves", 14, 1, 1},
};
constexpr int kNumOpts = 8;
constexpr int kGameOpts = 7;   // the first 7 come from the game; the last is host-owned

bool parseInt(const std::string& s, long* out) {
  if (s.empty() || s.size() > 11) return false;
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (!end || *end != 0) return false;
  *out = v;
  return true;
}
std::string trimWs(const std::string& s) {
  const char* ws = " \t\r\n";
  const size_t b = s.find_first_not_of(ws);
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(ws) - b + 1);
}
// false when the file cannot be opened; have[i] = key i present with a value in range.
bool readStore(int (&vals)[kNumOpts], bool (&have)[kNumOpts]) {
  for (int i = 0; i < kNumOpts; ++i) {
    vals[i] = 0;
    have[i] = false;
  }
  std::ifstream f(modSettingsPath());
  if (!f) return false;
  std::string line;
  while (std::getline(f, line)) {
    line = trimWs(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = trimWs(line.substr(0, eq));
    long v = 0;
    if (!parseInt(trimWs(line.substr(eq + 1)), &v)) continue;
    for (int i = 0; i < kNumOpts; ++i)
      if (k == kOpts[i].key && v >= 0 && v <= kOpts[i].maxv) {
        vals[i] = int(v);
        have[i] = true;
      }
  }
  return true;
}
std::string packedText(int32_t p) {
  if (p < 0) return "none";
  char b[180];
  char vol[32] = "volumes=unknown";
  if (p & int32_t(kModSettingsVolumesKnownBit))
    std::snprintf(vol, sizeof vol, "music=%d sfx=%d", p & 15, (p >> 4) & 15);
  std::snprintf(b, sizeof b,
                "%s scheme=%d invX=%d invY=%d autoCenter=%d vibration=%d applyToLoaded=%d",
                vol, (p >> 8) & 3, (p >> 10) & 1, (p >> 11) & 1, (p >> 12) & 1, (p >> 13) & 1,
                (p >> 14) & 1);
  return b;
}
}  // namespace

fs::path modSettingsPath() { return xamContentRoot() / "settings" / "game_options.ini"; }

int32_t modSettingsLoadPacked() {
  int vals[kNumOpts];
  bool have[kNumOpts];
  if (!readStore(vals, have)) return kModSettingsNone;
  int32_t p = 0;
  for (int i = 0; i < kNumOpts; ++i) {
    int v = vals[i];
    if (!have[i]) {
      if (i < 2) continue;                          // volumes: optional (unknown)
      if (i < kGameOpts) return kModSettingsNone;   // an incomplete store is no store
      v = 1;                                        // apply_to_loaded_saves default
    }
    p |= int32_t(v) << kOpts[i].shift;
  }
  if (have[0] && have[1]) p |= int32_t(kModSettingsVolumesKnownBit);
  return p;
}

bool modSettingsSavePacked(int32_t packed, bool volumesReadable, int32_t* canonical) {
  if (canonical) *canonical = kModSettingsNone;
  if (packed < 0 || packed >= (1 << 14)) return false;
  int vals[kNumOpts];
  for (int i = 0; i < kGameOpts; ++i) {
    vals[i] = (packed >> kOpts[i].shift) & ((1 << kOpts[i].bits) - 1);
    if (vals[i] > kOpts[i].maxv) return false;
  }
  int old[kNumOpts];
  bool have[kNumOpts];
  readStore(old, have);
  vals[kNumOpts - 1] = have[kNumOpts - 1] ? old[kNumOpts - 1] : 1;
  // Volumes the game could not read (no audio mixer: the getters return a cached 0) are
  // never stored: keep the file's, or leave them unknown.
  bool writeVol = true;
  if (!volumesReadable) {
    writeVol = have[0] && have[1];
    vals[0] = old[0];
    vals[1] = old[1];
  }
  // The value the file holds once written (= modSettingsLoadPacked() afterwards), known
  // before the write so the caller can keep it for the session when the write fails.
  if (canonical) {
    int32_t c = 0;
    for (int i = 0; i < kNumOpts; ++i) {
      if (i < 2 && !writeVol) continue;
      c |= int32_t(vals[i]) << kOpts[i].shift;
    }
    if (writeVol) c |= int32_t(kModSettingsVolumesKnownBit);
    *canonical = c;
  }
  const fs::path path = modSettingsPath();
  // iter-466: keys this writer does not own (PC options such as traffic_density /
  // population_density, kernel/mod_density.hpp) are kept, in their order, after ours.
  std::vector<std::string> foreign;
  {
    std::ifstream in(path);
    std::string line;
    while (in && std::getline(in, line)) {
      const std::string t = trimWs(line);
      const size_t eq = t.find('=');
      if (t.empty() || t[0] == '#' || t[0] == ';' || eq == std::string::npos) continue;
      const std::string k = trimWs(t.substr(0, eq));
      bool ours = false;
      for (int i = 0; i < kNumOpts; ++i) ours = ours || k == kOpts[i].key;
      if (!ours && !k.empty()) foreign.push_back(t);
    }
  }
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path tmp = path.parent_path() / "game_options.ini.tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << "# Superman Returns recomp - universal game options (custom_frontend mod,\n"
         "# docs/CUSTOM_FRONTEND.md section 0). Written whenever an options screen closes,\n"
         "# read at boot. Volumes 0..10 (absent = not known yet), control_scheme 0..3,\n"
         "# the rest 0/1. apply_to_loaded_saves=1: re-apply these after a save's own\n"
         "# options load (0 = the save's options stay, console behaviour).\n";
    for (int i = 0; i < kNumOpts; ++i) {
      if (i < 2 && !writeVol) continue;
      f << kOpts[i].key << '=' << vals[i] << '\n';
    }
    for (const auto& l : foreign) f << l << '\n';
    f.flush();
    if (!f) return false;
  }
  fs::rename(tmp, path, ec);   // std::filesystem::rename replaces an existing file
  if (ec) fs::remove(tmp, ec);
  return !ec;
}

// ── the mod options store (mod_bridge.hpp; settings/mod_options.ini) ─────────────────
namespace {
struct ModOpt { const char* key; int def; uint32_t bit; };
constexpr ModOpt kModOpts[] = {
    {"autosave_without_asking", 1, kModOptAutosaveBit},
    {"devtools_notice_ack", 0, kModOptNoticeAckBit},
};
std::mutex g_modOptMu;
std::set<std::string> g_modOptMounted;
int32_t g_saveFlowSlotForTest = -2;
std::mutex g_slotMu;
int32_t g_slot = -1;         // the current save slot (-1 = none: a new playthrough)
int32_t g_pendingSlot = -1;  // latched at an overwrite prompt (saveFlow+0x54 becomes 3 after it)

const ModOpt* findModOpt(const char* key) {
  if (!key) return nullptr;
  for (const ModOpt& o : kModOpts)
    if (std::strcmp(o.key, key) == 0) return &o;
  return nullptr;
}
// key -> 0/1 for the keys present in the file (missing file: empty map, all defaults).
void readModOpts(int (&vals)[sizeof(kModOpts) / sizeof(kModOpts[0])]) {
  constexpr size_t n = sizeof(kModOpts) / sizeof(kModOpts[0]);
  for (size_t i = 0; i < n; ++i) vals[i] = kModOpts[i].def;
  std::ifstream f(modOptionsPath());
  if (!f) return;
  std::string line;
  while (std::getline(f, line)) {
    line = trimWs(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = trimWs(line.substr(0, eq));
    long v = 0;
    if (!parseInt(trimWs(line.substr(eq + 1)), &v) || (v != 0 && v != 1)) continue;
    for (size_t i = 0; i < n; ++i)
      if (k == kModOpts[i].key) vals[i] = int(v);
  }
}
}  // namespace

fs::path modOptionsPath() { return xamContentRoot() / "settings" / "mod_options.ini"; }

int modOptionGet(const char* key) {
  const ModOpt* o = findModOpt(key);
  if (!o) return -1;
  std::lock_guard<std::mutex> lk(g_modOptMu);
  int vals[sizeof(kModOpts) / sizeof(kModOpts[0])];
  readModOpts(vals);
  return vals[o - kModOpts];
}

bool modOptionSet(const char* key, int value) {
  const ModOpt* o = findModOpt(key);
  if (!o || (value != 0 && value != 1)) return false;
  std::lock_guard<std::mutex> lk(g_modOptMu);
  constexpr size_t n = sizeof(kModOpts) / sizeof(kModOpts[0]);
  int vals[n];
  readModOpts(vals);
  vals[o - kModOpts] = value;
  const fs::path path = modOptionsPath();
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path tmp = path.parent_path() / "mod_options.ini.tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << "# Superman Returns recomp - mod / player options (custom_frontend mod,\n"
         "# docs/CUSTOM_FRONTEND.md section 0). Written by the in-game menus.\n"
         "# autosave_without_asking=1: the game's save prompt is answered by the mod (save to\n"
         "#   the current slot) and the end of every chapter saves too; 0 = the game asks, as\n"
         "#   on a console. The end-of-chapter save follows the value at launch.\n"
         "# devtools_notice_ack=1: the one-time dev-tools notice was confirmed.\n";
    for (size_t i = 0; i < n; ++i) f << kModOpts[i].key << '=' << vals[i] << '\n';
    f.flush();
    if (!f) return false;
  }
  fs::rename(tmp, path, ec);
  if (ec) fs::remove(tmp, ec);
  return !ec;
}

int32_t modOptionsPacked() {
  int32_t p = 0;
  {
    std::lock_guard<std::mutex> lk(g_modOptMu);
    int vals[sizeof(kModOpts) / sizeof(kModOpts[0])];
    readModOpts(vals);
    for (size_t i = 0; i < sizeof(kModOpts) / sizeof(kModOpts[0]); ++i)
      if (vals[i]) p |= int32_t(kModOpts[i].bit);
  }
  if (devToolsAnyDevtoolClass()) p |= int32_t(kModOptDevtoolsOnBit);
  if (modOptionMounted("autosave_without_asking")) p |= int32_t(kModOptAutosaveMountedBit);
  if (devToolAllowed("levelskip")) {
    const std::string st = devToolsLevelskipStatus().state;
    if (st == "armed" || st == "running") p |= int32_t(kModOptLevelskipBit);
  }
  return p;
}

void modOptionNoteMounted(const char* key) {
  std::lock_guard<std::mutex> lk(g_modOptMu);
  g_modOptMounted.insert(key ? key : "");
}
bool modOptionMounted(const char* key) {
  std::lock_guard<std::mutex> lk(g_modOptMu);
  return g_modOptMounted.count(key ? key : "") != 0;
}

// ── the current-save-slot tracker ────────────────────────────────────────────────────
int32_t modSlotEvent(const std::string& kind, int32_t sf, int32_t* eventSlot) {
  std::lock_guard<std::mutex> lk(g_slotMu);
  const bool valid = sf >= 0 && sf <= 2;
  int32_t ev = -1;   // the slot this event itself names (step5 E14a)
  if (kind == "new") {
    g_slot = -1;
    g_pendingSlot = -1;
  } else if (kind == "load") {
    if (valid) { g_slot = sf; ev = sf; }
    g_pendingSlot = -1;
  } else if (kind == "overwrite") {
    if (valid) g_pendingSlot = sf;
  } else if (kind == "save") {
    if (valid) { g_slot = sf; ev = sf; }                                   // a save to an empty slot keeps +0x54
    else if (g_pendingSlot >= 0) { g_slot = g_pendingSlot; ev = g_pendingSlot; }   // overwrite-confirmed: +0x54 = 3
    g_pendingSlot = -1;
  }
  if (eventSlot) *eventSlot = ev;
  return g_slot;
}
int32_t modSlotCurrent() {
  std::lock_guard<std::mutex> lk(g_slotMu);
  return g_slot;
}
void modSlotResetForTest() {
  std::lock_guard<std::mutex> lk(g_slotMu);
  g_slot = -1;
  g_pendingSlot = -1;
}
void modBridgeSetSaveFlowSlotForTest(int32_t v) { g_saveFlowSlotForTest = v; }

void modBridgeSetQuitHandlerForTest(void (*fn)()) { g_quitForTest = fn; }
void modBridgeSetAudioMixerForTest(int mode) { g_mixerTestMode = mode; }

ModBridgeResult modBridgeDispatch(const std::string& cmd, const std::string& args,
                                  int32_t* reply) {
  std::vector<ModReply> rs;
  const ModBridgeResult r = modBridgeDispatchEx(cmd, args, &rs);
  if (!rs.empty() && !rs[0].args.empty() && !rs[0].args[0].isString) {
    if (reply) *reply = rs[0].args[0].i;
    return ModBridgeResult::ReplyInt;
  }
  return r;
}

ModBridgeResult modBridgeDispatchEx(const std::string& cmd, const std::string& args,
                                    std::vector<ModReply>* replies, PPCContext* ctx) {
  auto replyInt = [&](const char* method, int32_t v) {
    if (replies) replies->push_back({method, {ModArg::Int(v)}});
    return ModBridgeResult::ReplyInt;
  };
  if (cmd.size() < 3 || cmd.compare(0, 3, "SR_") != 0) return ModBridgeResult::NotOurs;
  if (cmd == "SR_Map") {   // iter-466 map: the useful pause map (kernel/map_markers.hpp, docs/MAP.md)
    mapmk::mapCommand(trimWs(args), replies);
    return ModBridgeResult::ReplyInt;
  }
  if (cmd == "SR_CostumeViewer") {   // iter-465 viewer: the COSTUME VIEWER page's host model
    const std::string a = trimWs(args);
    costumeViewerCommand(a);
    if (a != "open") return ModBridgeResult::Handled;
    // the page asks whether to re-lay out (FlowMgr.SR_ViewerOn) and gets each suit's CHARACTER /
    // COMMENTS lines (FlowMgr.SR_ViewerInfo id, character, comment)
    if (replies) {
      replies->push_back({"SR_ViewerOn", {ModArg::Int(costumeViewerEnabled() ? 1 : 0)}});
      for (const auto& info : costumeViewerInfo())
        replies->push_back({"SR_ViewerInfo", {ModArg::Str(info.id), ModArg::Str(info.character), ModArg::Str(info.comment)}});
    }
    return ModBridgeResult::ReplyInt;
  }
  if (cmd == "SR_CostumeWear") {
    std::string reason;
    const bool queued = requestCostume(ctx, args, &reason);
    if (replies) replies->push_back({"SR_CostumeResult", {ModArg::Str(args), ModArg::Int(queued ? 1 : 0),
                                                         ModArg::Str(reason)}});
    return ModBridgeResult::ReplyInt;
  }
  if (cmd == "SR_CostumeWorn") {
    // A retail Fortress record the player WEARs (ScrCostumes HandleInput SQUARE on an unlocked
    // record, sent just before the retail RecordAdapter.SetActiveCostume(index) call): only
    // remembered as the worn suit; the retail call makes the change.
    // The reply says whether the retail call may make the change now: only in a session that
    // loaded the retail animation family; otherwise the suit is worn from the next start.
    long idx = -1;
    const std::string id = "retail/" + std::to_string(parseInt(trimWs(args), &idx) ? idx + 1 : 0);
    const bool ok = idx >= 0 && idx <= 2 && modCostumeNoteWorn(id);
    const bool now = ok && modCostumeSessionFamily() == kRetailCostumeFamily;
    std::string why;
    if (now && !costumePopupWanted() && requestCostumeSlot(ctx, static_cast<int>(idx) + 1, &why)) {
      SR_LOG_BOOT(sr::LogSeverity::Info, "[COSTUME] retail record %s worn -> switched (no popup)", args.c_str());
      if (replies) replies->push_back({"SR_CostumeResult", {ModArg::Str(id), ModArg::Int(2), ModArg::Str(std::string())}});
      return ModBridgeResult::ReplyInt;
    }
    SR_LOG_BOOT(sr::LogSeverity::Info, "[COSTUME] retail record %s worn -> %s", args.c_str(),
                !ok ? "not remembered" : now ? "remembered, retail switch now" : "remembered, worn from the next start");
    if (replies) replies->push_back({"SR_CostumeResult", {ModArg::Str(id), ModArg::Int(now || !ok ? 1 : 0),
                                                         ModArg::Str(now || !ok ? std::string() : kNextStartNote)}});
    return ModBridgeResult::ReplyInt;
  }
  if (cmd.compare(0, 6, "SR_Mod") == 0) {
    std::vector<ModMenuCall> calls;
    modMenuCommand(cmd, args, &calls);
    if (replies) for (const auto& call : calls) {
      ModReply reply;
      reply.method = call.method;
      for (const auto& arg : call.args)
        reply.args.push_back(arg.isString ? ModArg::Str(arg.s) : ModArg::Int(arg.i));
      replies->push_back(std::move(reply));
    }
    return calls.empty() ? ModBridgeResult::Handled : ModBridgeResult::ReplyInt;
  }
  const uint32_t n = g_cmdCount.fetch_add(1, std::memory_order_relaxed) + 1;
  if (cmd == "SR_Quit") {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[SR_FSCMD] #%u SR_Quit '%s' -> SDL_EVENT_QUIT (the window-close exit path)", n, args.c_str());
    if (g_quitForTest) {
      g_quitForTest();
    } else {
      SDL_Event e{};
      e.type = SDL_EVENT_QUIT;
      if (!SDL_PushEvent(&e))   // SDL3: bool (false = failed or filtered)
        SR_LOG_BOOT(sr::LogSeverity::Error, "[SR_FSCMD] SDL_PushEvent(SDL_EVENT_QUIT) failed: %s",
                    SDL_GetError());
    }
    return ModBridgeResult::Quit;
  }
  if (cmd == "SR_Achievements") {
    // lane E L-a: the ACHIEVEMENTS page (gfx/achievements_ui.hpp), a host page over the paused game; the pause menu's
    // ACHIEVEMENTS row sends it (L-c). args = "[IG|FE]" (default IG). No reply: the menu under the page stays as it
    // is (the game reads a neutral pad meanwhile) and has its focus back when the page closes.
    const std::string where = trimWs(args) == "FE" ? "FE" : "IG";
    modlib::requestOpenPage(modlib::OverlayPage::Achievements, where);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SR_FSCMD] #%u SR_Achievements '%s' -> open (%s)", n, args.c_str(),
                where.c_str());
    return ModBridgeResult::Handled;
  }
  if (cmd == "SR_Log") {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[SR_FSCMD] #%u SR_Log '%s'%s", n, args.c_str(),
                pendingPopupNote().c_str());
    return ModBridgeResult::Handled;
  }
  if (cmd == "SR_SetSettings") {
    // args = "[<tag> ]<packed>": the value is the last space-separated token (the AS side
    // prefixes the screen and interface state that captured it, for the log).
    const std::string a = trimWs(args);
    const size_t sp = a.find_last_of(' ');
    long v = 0;
    const bool parsed = parseInt(sp == std::string::npos ? a : a.substr(sp + 1), &v) && v >= 0 &&
                        v < (1 << 14);
    // The volume getters read the audio mixer (sub_822AD1D0: dword_82976054 &&
    // dword_82976070, else a cached float that is 0 until the mixer existed). Read-only.
    uint32_t mixer = 0, names = 0;
    readGuestU32(0x82976054u, &mixer);
    readGuestU32(0x82976070u, &names);
    const int tm = g_mixerTestMode;
    const bool volOk = tm < 0 ? (mixer != 0 && names != 0) : tm != 0;
    int32_t canonical = kModSettingsNone;
    const bool ok = parsed && modSettingsSavePacked(int32_t(v), volOk, &canonical);
    // valid = the value passed validation (canonical >= 0); !ok then means the file write
    // failed (a locked or read-only game_options.ini, a full disk).
    const bool valid = parsed && canonical >= 0;
    const int32_t stored = ok ? modSettingsLoadPacked() : canonical;
    if (valid) gameopts::noteLive(int32_t(v), volOk);   // lane E L-b: the GAME tab shows the captured options
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[SR_FSCMD] #%u SR_SetSettings '%s' -> %s (%s) audioMixer=%08X/%08X%s", n,
                args.c_str(),
                ok ? "stored" : (valid ? "NOT STORED (file write failed; kept for this session)"
                                       : "REJECTED"),
                valid ? packedText(stored).c_str()
                      : (parsed ? "value out of range" : "not a packed value"),
                mixer, names, volOk ? "" : " (no mixer: volumes not taken)");
    // The reply is the ONLY writer of the AS copy _global.SRSettings (the capture sends the
    // snapshot without assigning it): the canonical value with the host-owned bits 14/15
    // and the kept volumes, from the file, or - when the write failed - the same value
    // computed in memory, so the session keeps the player's choices and the ini's
    // apply_to_loaded_saves. An invalid capture (a getter returned undefined -> "NaN", or a
    // value out of range) gets no reply: _global.SRSettings keeps its last good value.
    if (!valid) return ModBridgeResult::Handled;
    return replyInt("SR_OnHostSettings", stored);
  }
  if (cmd == "SR_GameOptionsLive") {
    // lane E L-b (kernel/game_options.hpp): the game's own options, read by its getters right before the custom
    // front end opens OPTIONS (srfrontend.aptasm OpenOptions, srpause.aptasm OpenDisplay); shown by the GAME tab,
    // never stored. No reply.
    // args = "<FE|IG> <packed>": the value is the last token (as SR_SetSettings)
    const std::string a = trimWs(args);
    const size_t sp = a.find_last_of(' ');
    long v = 0;
    const bool parsed = parseInt(sp == std::string::npos ? a : a.substr(sp + 1), &v) && v >= 0 && v < (1 << 14);
    uint32_t mixer = 0, names = 0;
    readGuestU32(0x82976054u, &mixer);
    readGuestU32(0x82976070u, &names);
    const int tm = g_mixerTestMode;
    const bool volOk = tm < 0 ? (mixer != 0 && names != 0) : tm != 0;
    if (parsed) gameopts::noteLive(int32_t(v), volOk);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SR_FSCMD] #%u SR_GameOptionsLive '%s' -> %s (%s)%s", n, args.c_str(),
                parsed ? "noted" : "REJECTED", parsed ? packedText(int32_t(v)).c_str() : "not a packed value",
                volOk ? "" : " (no mixer: volumes not the game's)");
    return ModBridgeResult::Handled;
  }
  if (cmd == "SR_GetSettings") {
    const int32_t p = modSettingsLoadPacked();
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[SR_FSCMD] #%u SR_GetSettings -> %d (%s) from \"%s\"", n,
                p, packedText(p).c_str(), modSettingsPath().generic_string().c_str());
    return replyInt("SR_OnHostSettings", p);
  }
  if (cmd == "SR_GetModOptions") {
    const int32_t p = modOptionsPacked();
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[SR_FSCMD] #%u SR_GetModOptions -> %d (autosave=%d noticeAck=%d devtoolsOn=%d autosaveLuaMounted=%d levelskip=%d) "
                "from \"%s\"",
                n, p, (p >> 0) & 1, (p >> 1) & 1, (p >> 8) & 1, (p >> 9) & 1, (p >> 10) & 1,
                modOptionsPath().generic_string().c_str());
    return replyInt("SR_OnHostModOptions", p);
  }
  if (cmd == "SR_SetModOption") {
    // args = "<key> <0|1>"
    const std::string a = trimWs(args);
    const size_t sp = a.find(' ');
    const std::string key = sp == std::string::npos ? a : a.substr(0, sp);
    long v = -1;
    const bool parsed = sp != std::string::npos && parseInt(trimWs(a.substr(sp + 1)), &v);
    const bool ok = parsed && modOptionSet(key.c_str(), int(v));
    const int32_t p = modOptionsPacked();
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[SR_FSCMD] #%u SR_SetModOption '%s' -> %s (packed %d)", n, args.c_str(),
                ok ? "stored" : "REJECTED", p);
    return replyInt("SR_OnHostModOptions", p);
  }
  if (cmd == "SR_SaveEvent") {
    // args = new | load | save | overwrite | query. The slot the save flow used comes from
    // saveFlow+0x54 (SaveTask sub_822B4D08 / LoadTask sub_822B4ED8 store it), read fault-free.
    const std::string kind = trimWs(args);
    int32_t sf = -1;
    if (g_saveFlowSlotForTest != -2) {
      sf = g_saveFlowSlotForTest;
    } else {
      uint32_t flow = 0, v = 0;
      if (readGuestU32(0x8296EE8Cu, &flow) && flow && readGuestU32(flow + 0x54u, &v)) sf = int32_t(v);
    }
    const int32_t before = modSlotCurrent();
    int32_t eventSlot = -1;
    const int32_t slot = (kind == "query") ? before : modSlotEvent(kind, sf, &eventSlot);
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[SR_FSCMD] #%u SR_SaveEvent '%s' saveFlow+0x54=%d -> slot %d (was %d)", n,
                kind.c_str(), sf, slot, before);
    // step5 E14a: the achievements follow the save (NEW GAME clears, LOAD replaces, SAVE writes the sidecar), on the
    // slot THIS event names - never the tracker's remembered one (an out-of-range LOAD must not reload the old slot)
    if (kind != "query") sr::ach::onSaveEvent(kind, eventSlot);
    // Only a query is answered (the event sites - ScrPressStart, ScrSaveLoad, Main_Hud - bind no
    // reply method); the answer goes to FlowMgr.SR_OnHostSlot, bound around the query only.
    if (kind != "query") return ModBridgeResult::Handled;
    return replyInt("SR_OnHostSlot", slot);
  }
  // iter-464 modmenu: the host-driven pages (kernel/host_pages.hpp). SR_Menu routes by page id:
  // the MODS menu (kernel/mod_menu.cpp) or the Dev Tools menu (kernel/devtools_menu.cpp);
  // SR_DevMenu is the older name of the same command.
  if (cmd == "SR_Menu" || cmd == "SR_DevMenu") {
    std::vector<ModReply> local;
    std::vector<ModReply>* out = replies ? replies : &local;
    const size_t before = out->size();
    menuPagesCommand(trimWs(args), out);
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[SR_FSCMD] #%u %s '%s' -> %zu call(s)", n, cmd.c_str(), args.c_str(),
                out->size() - before);
    return ModBridgeResult::ReplyInt;
  }
  SR_LOG_BOOT(sr::LogSeverity::Warn, "[SR_FSCMD] #%u unknown '%s' '%s' ignored", n,
              cmd.c_str(), args.c_str());
  return ModBridgeResult::Handled;
}

// The replies: FlowMgr.<method>(args...) through the AptExecutor, the call the game's own
// natives make (sub_822B15F8 calls FlowMgr.CppShowPopup this way; sub_822A0C00 calls
// listener.LoadComplete): r3 = *g_AptExecutor (0x8296EB80), r4 = method name, r5 = 0,
// r6 = target path "FlowMgr" (resolved from _level0 = MAIN, loaded all session; not the
// SaveLoadAdapter listener, which points at an unloaded ScrPressStart while an in-game
// options screen is up), r7 = the argument count, r8..r10 = AS values: ints from
// AptAS2AllocIntObj_sub_82693028, strings from AptObjectRecycleOrAlloc_sub_8269A210 (the call
// sub_822B15F8 makes for popup titles; it copies the C string: AptNamedObjectCtor_sub_826C4860
// -> AptStringAssignCStr_sub_8268F3E8), call vt[+0x20]. Runs on its own guest stack frame
// below the caller's r1 (back chain + the strings); r1 is restored afterwards. No executor ->
// logged, nothing called. The mod defines each FlowMgr.SR_* method in the movie that sends
// the command, right before it.
namespace {
bool deliverOne(PPCContext& ctx, uint8_t* base, const ModReply& r) {
  constexpr uint32_t kExecutorGlobal = 0x8296EB80u;
  constexpr uint32_t kAllocIntObj = 0x82693028u;
  constexpr uint32_t kAllocStrObj = 0x8269A210u;
  uint32_t exec = 0, vt = 0, fn = 0;
  const bool haveExec = readGuestU32(kExecutorGlobal, &exec) && exec &&
                        readGuestU32(exec, &vt) && vt && readGuestU32(vt + 0x20u, &fn) && fn;
  // No dispatch table (difftest links leaves directly) -> nothing can be called.
  const bool haveTable = sr::kernel::g_dispatchTable != nullptr;
  PPCFunc* invoke = (haveExec && haveTable) ? PPC_LOOKUP_FUNC_SAFE(base, fn) : nullptr;
  PPCFunc* allocInt = invoke ? PPC_LOOKUP_FUNC_SAFE(base, kAllocIntObj) : nullptr;
  PPCFunc* allocStr = invoke ? PPC_LOOKUP_FUNC_SAFE(base, kAllocStrObj) : nullptr;
  size_t strBytes = 0;
  for (const ModArg& a : r.args) strBytes += a.isString ? ((a.s.size() + 16) & ~size_t(15)) : 0;
  const size_t nameLen = r.method.size();
  if (!invoke || !allocInt || !allocStr || nameLen > 60 || r.args.size() > 3 || strBytes > 0x700 ||
      ctx.r1.u32 < 0x2000u) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[SR_FSCMD] reply FlowMgr.%s(%zu args) NOT delivered: executor=%08X vt+0x20=%08X", r.method.c_str(),
                r.args.size(), exec, fn);
    return false;
  }
  const uint32_t savedR1 = ctx.r1.u32;
  const uint32_t frame = (savedR1 - 0x900u) & ~0xFu;
  writeGuestBE32(frame, savedR1);                         // back chain
  const uint32_t nameVA = frame + 0xC0u;
  std::memcpy(guestVAToHost(nameVA), r.method.c_str(), nameLen + 1);
  const uint32_t pathVA = frame + 0xB0u;                  // target path "FlowMgr"
  std::memcpy(guestVAToHost(pathVA), "FlowMgr", 8);
  uint32_t strVA = frame + 0x100u;
  uint32_t vals[3] = {0, 0, 0};
  bool ok = true;
  for (size_t i = 0; i < r.args.size() && ok; ++i) {
    ctx.r1.u32 = frame;
    if (r.args[i].isString) {
      std::memcpy(guestVAToHost(strVA), r.args[i].s.c_str(), r.args[i].s.size() + 1);
      ctx.r3.u64 = strVA;
      strVA += uint32_t((r.args[i].s.size() + 16) & ~size_t(15));
      allocStr(ctx, base);
    } else {
      ctx.r3.s64 = r.args[i].i;
      allocInt(ctx, base);
    }
    vals[i] = ctx.r3.u32;
    ok = vals[i] != 0;
  }
  if (ok) {
    ctx.r1.u32 = frame;
    ctx.r3.u64 = exec;
    ctx.r4.u64 = nameVA;
    ctx.r5.u64 = 0;
    ctx.r6.u64 = pathVA;
    ctx.r7.u64 = r.args.size();
    ctx.r8.u64 = vals[0];
    ctx.r9.u64 = vals[1];
    ctx.r10.u64 = vals[2];
    invoke(ctx, base);
  }
  ctx.r1.u32 = savedR1;
  return ok;
}
}  // namespace

void modBridgeDeliver(PPCContext& ctx, uint8_t* base, const std::vector<ModReply>& replies) {
  size_t okN = 0;
  for (const ModReply& r : replies) okN += deliverOne(ctx, base, r) ? 1 : 0;
  // iter-466 map: the pause map's per-tick position updates stay out of the log (every frame while
  // the player pans); a failed delivery is still logged by deliverOne
  bool quiet = !replies.empty();
  for (const ModReply& r : replies) quiet = quiet && (r.method == "SR_MapXY" || r.method == "SR_MapArt" || r.method == "SR_MapRtXY");
  if (quiet) return;
  // One summary line per command; the single-int form keeps the wording the scenarios match.
  if (replies.size() == 1 && !replies[0].args.empty() && !replies[0].args[0].isString) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[SR_FSCMD] reply FlowMgr.%s(%d) %s", replies[0].method.c_str(),
                replies[0].args[0].i, okN ? "delivered" : "NOT delivered");
  } else if (!replies.empty()) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[SR_FSCMD] reply %zu call(s) (first FlowMgr.%s) %zu delivered", replies.size(),
                replies[0].method.c_str(), okN);
  }
}

void modBridgeReplyToFlowMgr(PPCContext& ctx, uint8_t* base, const char* method, int32_t value) {
  modBridgeDeliver(ctx, base, {ModReply{method, {ModArg::Int(value)}}});
}


// ---- iter-465 CHEATS page (kernel/cheats_menu.hpp): read-only state of the game's own cheats ----
// The retail cheat manager (*0x8296DF30, built from preload CheatDefs by
// ParseCheatConfigNode_sub_821F89A0): +0 node pointer array, +4 count; a node holds +12 its code
// (ParseCheatCodeName_sub_821E7890: 1 INFINITE_STAMINA .. 13 SUPERMAN_SKIN5) and +16 its ON byte.
// Fault-free reads only; the page enters a cheat by pressing its buttons (cheats_menu.cpp).
int modCheatState(int code) {
  uint32_t mgr = 0, arr = 0, count = 0;
  if (!readGuestU32(0x8296DF30u, &mgr) || !mgr || !readGuestU32(mgr, &arr) || !arr ||
      !readGuestU32(mgr + 4u, &count) || count > 64) return -1;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t node = 0, c = 0, w = 0;
    if (readGuestU32(arr + 4u * i, &node) && node && readGuestU32(node + 12u, &c) && int(c) == code)
      return readGuestU32(node + 16u, &w) ? ((w >> 24) ? 1 : 0) : -1;  // +16 is a byte (BE high byte)
  }
  return -1;
}

}  // namespace kernel
}  // namespace sr

extern "C" void __imp__sub_826D39A0(PPCContext& __restrict ctx, uint8_t* base);

// sub_826D39A0(cmd r3, args r4): the Apt FSCommand handler (*0x829A1A90).
void sub_826D39A0(PPCContext& __restrict ctx, uint8_t* base) {
  // Retail content never gets here (0 FSCommands); a non-SR_ command runs the game's body.
  const std::string cmd = sr::kernel::readGuestCString(ctx.r3.u32);
  if (cmd.compare(0, 3, "SR_") != 0) {        // not a bridge command: the game's own body
    __imp__sub_826D39A0(ctx, base);
    return;
  }
  const std::string args = sr::kernel::readGuestCString(ctx.r4.u32, 1023);
  std::vector<sr::kernel::ModReply> replies;
  if (sr::kernel::modBridgeDispatchEx(cmd, args, &replies, &ctx) == sr::kernel::ModBridgeResult::ReplyInt)
    sr::kernel::modBridgeDeliver(ctx, base, replies);
  ctx.r3.u64 = 0;
}
