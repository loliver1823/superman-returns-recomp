// step4/audit (RELEASE_PLAN 6.9a, docs/LOGIC_AUDIT.md 2.0): import answers made equal to Xenia 0bd090dbe's.
//
// PLATFORM FIXES, each behind its own gate. Batch 1 (the four below + SR_NT_EVENT_TYPE + SR_NTSTATUS_DOSERR) is
// BAKED: default ON since 2026-10-18 (docs/LOGIC_AUDIT.md 2.0d), =0 restores the old answer - with a gate =0 its
// export keeps exactly the registration it had before (nothing here is registered). Batch 2's gates are default
// OFF until their A/B. Each fix is the oracle's answer for a row the logic audit found the game reads:
//   SR_XAM_XUID_ABI      XamUserGetXUID(user r3, type_mask r4, pxuid r5)   audit row 3
//   SR_XCONFIG_XENIA     ExGetXConfigSetting = Xenia's XConfig table       audit row 1
//   SR_XAM_SYSVER_XENIA  XamGetSystemVersion = 0 (Xenia's kStub answer)    audit row 2
//   SR_XMP_APP_XENIA     XMP app 0xFA: XMPSetPlaybackController (0x7001A) state, XMPGetPlaybackController
//                        (0x7001B) answer - the path the game takes with a version-0 dashboard (row 2)
// The event-type fix (SR_NT_EVENT_TYPE, audit rows 6 + 9) lives with the event table in kernel/stubs.cpp.
// Tests: build/difftest.exe --import-fixes-test (each gate's checks FAIL with that gate =0: the mutant).
#include "import_fixes.hpp"

#include "achievements.hpp"
#include "export_registry.hpp"
#include "guest_context.hpp"
#include "xam_content.hpp"
#include "stubs.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace sr {
namespace kernel {
namespace {

bool gateDefaultOn(const char* name) {
  const char* e = std::getenv(name);
  return !e || e[0] != '0';
}
// One predicate per gate with its literal name, so scripts/audit_forcing.py can see each fix's gate.
bool xamXuidAbiOn() {   // baked: default ON, =0 = the old answer
  static const bool v = [] { const char* e = std::getenv("SR_XAM_XUID_ABI"); return !e || e[0] != '0'; }();
  return v;
}
bool xconfigXeniaOn() {   // baked: default ON, =0 = the old answer
  static const bool v = [] { const char* e = std::getenv("SR_XCONFIG_XENIA"); return !e || e[0] != '0'; }();
  return v;
}
bool sysverXeniaOn() {   // baked: default ON, =0 = the old answer
  static const bool v = [] { const char* e = std::getenv("SR_XAM_SYSVER_XENIA"); return !e || e[0] != '0'; }();
  return v;
}
bool xmpAppXeniaOn() {   // baked: default ON, =0 = the old answer
  static const bool v = [] { const char* e = std::getenv("SR_XMP_APP_XENIA"); return !e || e[0] != '0'; }();
  return v;
}
// batch 2 (2026-10-18; baked 2026-10-20, docs/LOGIC_AUDIT.md 2.0f)
bool rtlTimeFieldsXeniaOn() {   // baked: default ON, =0 = the old answer
  static const bool v = [] { const char* e = std::getenv("SR_RTL_TIMEFIELDS_XENIA"); return !e || e[0] != '0'; }();
  return v;
}
bool rtlMbcsXeniaOn() {   // baked: default ON, =0 = the old answer
  static const bool v = [] { const char* e = std::getenv("SR_RTL_MBCS_XENIA"); return !e || e[0] != '0'; }();
  return v;
}

constexpr uint32_t kXeSuccess = 0;
constexpr uint32_t kXeInvalidArg = 0x80070057u;   // X_E_INVALIDARG
constexpr uint32_t kXeNoSuchUser = 0x80070525u;   // X_E_NO_SUCH_USER
constexpr uint32_t kStatusInvalidParameter3 = 0xC00000F1u;
constexpr uint32_t kStatusBufferTooSmall = 0xC0000023u;

bool guestRange(uint32_t va, uint32_t len) {
  return va != 0 && isGuestVAInRange(va) && (len == 0 || isGuestVAInRange(va + len - 1)) &&
         uint64_t(va) + len <= 0x100000000ull;
}
void ifxStoreBE16(uint32_t va, uint16_t v) {
  if (uint8_t* p = guestVAToHost(va)) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
}
void ifxStoreBE64(uint32_t va, uint64_t v) {
  writeGuestBE32(va, uint32_t(v >> 32));
  writeGuestBE32(va + 4, uint32_t(v));
}

// ── SR_XAM_XUID_ABI ──────────────────────────────────────────────────────────────────────────────────────────
// Xenia xam_user.cc:31-66: (user_index, type_mask, xuid_ptr). NULL ptr -> X_E_INVALIDARG; *xuid = 0; user >= 4 ->
// X_E_INVALIDARG; not signed in -> X_E_NO_SUCH_USER; signed in: profile type 3 (local | online) & mask != 0 ->
// *xuid = the profile's XUID, X_E_SUCCESS. The old stub took r4 (the type mask, 7 or 2 at the game's two callers)
// as the out pointer: it wrote the XUID into the guest null page and left the caller's buffer uninitialised
// (RefreshSignedInUser_sub_825655C8 then compared and stored stack garbage). Our signed-in profile is user 0
// (stub_XamUserGetSigninState) with our profile's XUID kXamUser0Xuid, the one the save packages are filed under.
uint32_t xamUserGetXuid(uint32_t user, uint32_t typeMask, uint32_t pxuid) {
  if (!guestRange(pxuid, 8)) return kXeInvalidArg;
  ifxStoreBE64(pxuid, 0);
  if (user >= 4) return kXeInvalidArg;
  if (user != 0) return kXeNoSuchUser;               // only user 0 is signed in (XamUserGetSigninState)
  constexpr uint32_t kProfileType = 1 | 2;           // Xenia UserProfile::type(): local | online
  if ((kProfileType & typeMask) == 0) return kXeNoSuchUser;
  ifxStoreBE64(pxuid, kXamUser0Xuid);
  return kXeSuccess;
}
uint32_t exp_XamUserGetXUID(uint32_t user, uint32_t typeMask, uint32_t pxuid, uint32_t, uint32_t, uint32_t) {
  const uint32_t rc = xamUserGetXuid(user, typeMask, pxuid);
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 8 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[IMPORT_FIXES] XUID #%u XamUserGetXUID(user=%u, mask=%u, out=%08X) -> %08X lr=%08X", n, user,
                typeMask, pxuid, rc, getCurrentGuestLR());
  return rc;
}

// ── SR_XCONFIG_XENIA ─────────────────────────────────────────────────────────────────────────────────────────
struct XConfigField {
  uint16_t category;
  uint16_t setting;
  uint32_t offset;   // absolute in XConfigData
  uint32_t size;
};
#include "xconfig_xenia_table.inc"

const XConfigField* findField(uint16_t category, uint16_t setting) {
  for (const XConfigField& f : kXConfigFields)
    if (f.category == category && f.setting == setting) return &f;
  return nullptr;
}

// Xenia XConfig::SetDefaults (xconfig.cc:96-122) into a big-endian XConfigData image. Every value below is the
// oracle machine's own xconfig.settings too (sha256 447f0c9e..., decoded by tools/logic_audit/gen_xconfig_table.py
// --decode): Xenia wrote it at its first run and never changed it.
struct XConfigImage {
  uint8_t bytes[kXConfigDataSize] = {};
  void setBytes(uint16_t cat, uint16_t set, const void* src, uint32_t n) {
    const XConfigField* f = findField(cat, set);
    if (f) std::memcpy(bytes + f->offset, src, n < f->size ? n : f->size);
  }
  void setU32(uint16_t cat, uint16_t set, uint32_t v) {
    const uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    setBytes(cat, set, b, 4);
  }
  void setU8(uint16_t cat, uint16_t set, uint8_t v) { setBytes(cat, set, &v, 1); }
  XConfigImage() {
    setU32(0x02, 0x02, 0x00400100u);       // SECURED_AV_REGION = X_AV_REGION::NTSCM
    setU32(0x03, 0x09, 1);                 // USER_LANGUAGE = kEnglish
    setU8(0x03, 0x0E, 103);                // USER_COUNTRY = kUnitedStates
    setU32(0x03, 0x0B, 0x00010001u);       // USER_AUDIO_FLAGS = DolbyDigital | DolbyProLogic
    setU32(0x03, 0x13, 0x050002D0u);       // USER_AV_COMPOSITE_SCREENSZ = XHDTVResolution[1] 1280x720
    setU32(0x03, 0x14, 0x050002D0u);       // USER_AV_COMPONENT_SCREENSZ = XHDTVResolution[1]
    setU32(0x03, 0x15, 0x02D00240u);       // USER_AV_VGA_SCREENSZ = XVGAResolution[3] 720x576
    setU32(0x03, 0x0C, 0x00000040u);       // USER_RETAIL_FLAGS = DashboardInitialized
    setU32(0x03, 0x0A, 0x00000000u);       // USER_VIDEO_FLAGS = RatioNormal
    setU8(0x03, 0x0F, 0x03);               // USER_PC_FLAGS = XBLAllowed | XBLMembershipCreationAllowed
    setU32(0x03, 0x16, 0x000000FFu);       // USER_PC_GAME = NoGameRestrictions
    setU32(0x03, 0x1F, 0x3F333333u);       // USER_MUSIC_VOLUME = 0.7f
    // kTimezones[0x19] "GMT London" (xconfig.h:272-...): bias 0, GMT / BST, US-style EU dates, DST bias -60.
    setU32(0x03, 0x01, 0);
    setBytes(0x03, 0x02, "GMT", 4);
    setBytes(0x03, 0x03, "BST", 4);
    const uint8_t stdDate[4] = {0x0A, 0x05, 0x00, 0x02}, dltDate[4] = {0x03, 0x05, 0x00, 0x01};
    setBytes(0x03, 0x04, stdDate, 4);
    setBytes(0x03, 0x05, dltDate, 4);
    setU32(0x03, 0x06, 0);
    setU32(0x03, 0x07, 0xFFFFFFC4u);       // -60
    // IPTV_SERVICE_PROVIDER_NAME = u"Xenia TV", UTF-16 big-endian.
    const char* tv = "Xenia TV";
    uint8_t name[16] = {};
    for (int i = 0; tv[i]; ++i) name[2 * i + 1] = uint8_t(tv[i]);
    setBytes(0x09, 0x01, name, 16);
  }
};
const XConfigImage& xconfigImage() {
  static const XConfigImage img;
  return img;
}

// Xenia xboxkrnl_xconfig.cc:21-58: (WORD category r3, WORD setting r4, buffer r5, WORD buffer_size r6,
// PWORD required_size r7). buffer NULL with a size -> STATUS_INVALID_PARAMETER_3; a buffer smaller than the
// setting -> STATUS_BUFFER_TOO_SMALL (nothing copied, *required = 0); else exactly the setting's bytes and
// *required (u16) = its size; an unknown setting has size 0 (nothing copied, success). The old stub answered
// SECURED ids one off (AV region at id 1), wrote 4 bytes for 1/2/8-byte settings, wrote requiredSize as a u32
// into the WORD, never refused a small buffer and differed in many USER defaults (time zone UTC without DST,
// VideoFlags 0x004B0000, retail flags 0, country 0, ...).
uint32_t exp_ExGetXConfigSetting(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t) {
  const uint16_t category = uint16_t(r3), setting = uint16_t(r4), bufSize = uint16_t(r6);
  const XConfigField* f = findField(category, setting);
  const uint32_t size = f ? f->size : 0;
  uint32_t status = kXeSuccess;
  if (!r5 && bufSize) {
    status = kStatusInvalidParameter3;
  } else if (r5) {
    if (bufSize < size) {
      status = kStatusBufferTooSmall;
    } else if (size) {
      if (guestRange(r5, size)) std::memcpy(guestVAToHost(r5), xconfigImage().bytes + f->offset, size);
      else status = kStatusInvalidParameter3;   // host safety; Xenia would fault
    }
  }
  // Every path, errors included, ends in the entry wrapper's write (xboxkrnl_xconfig.cc:46-57): *required_size =
  // a WORD, 0 unless the call succeeded (Astra P2 3: the NULL-buffer error used to return before it).
  const uint16_t required = status == kXeSuccess ? uint16_t(size) : 0;
  if (r7 && guestRange(r7, 2)) ifxStoreBE16(r7, required);
  static std::atomic<uint32_t> s_n{0};
  if (s_n.fetch_add(1, std::memory_order_relaxed) < 32)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[IMPORT_FIXES] XCONFIG ExGetXConfigSetting(%u, 0x%02X, buf=%08X size=%u) -> %08X required=%u lr=%08X",
                category, setting, r5, bufSize, status, required, getCurrentGuestLR());
  return status;
}

// ── SR_XAM_SYSVER_XENIA ──────────────────────────────────────────────────────────────────────────────────────
// Xenia xam_info.cc:359-367 returns 0 (kStub). The iter-317h value 0x20449700 was set for an FE-transition theory
// that iter-356 / iter-357j corrected (0x7001A is the XMP music player's message, not an FE request); its readers
// (IDA: XMPOverride/RestoreBackgroundMusic sub_82470628 / sub_82470700, the WSAStartup / XNetStartup wrappers
// sub_8246F3B0 / sub_828A8D30) then take Xenia's "old dashboard" branches. Pair with SR_XMP_APP_XENIA so the old
// branch's XMsgInProcessCall(0xFA, 0x7001B) gets Xenia's answer instead of the generic stub's unwritten out-params.
uint32_t exp_XamGetSystemVersion(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 8 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[IMPORT_FIXES] SYSVER #%u XamGetSystemVersion -> 0 lr=%08X", n,
                getCurrentGuestLR());
  return 0;
}

// ── SR_XMP_APP_XENIA ─────────────────────────────────────────────────────────────────────────────────────────
// Xenia apu/audio_media_player.h:115-135 + kernel/xam/apps/xmp_app.cc:348-427: the media player's XMP client
// (default Game = 2), playback controller (default Game = 0) and override flag (default false).
//   0x7001A XMPSetPlaybackController {client, request, locked}: Restore (4) -> client Game, controller Game;
//           else client = client, controller = request; override = locked. X_E_SUCCESS.
//   0x7001B XMPGetPlaybackController {client, *controller, *locked}: *controller = controller,
//           *locked = !IsTitleInPlaybackControl() where IsTitleInPlaybackControl = !override &&
//           (client == Game && controller == Game || title rendering (false here)). X_E_SUCCESS.
// The kXNotificationXmpPlaybackControllerChanged broadcast (xmp_app.cc:388, 0x0A000003 with
// IsTitleInPlaybackControl) reaches the notification queue when SR_XNOTIFY_XENIA is on (Astra batch-2 P2); with it
// off the old queue gets nothing here, as before.
constexpr uint32_t kXmpApp = 0xFAu, kXmpSetController = 0x0007001Au, kXmpGetController = 0x0007001Bu;
constexpr uint32_t kXmpClientGame = 2, kXmpControllerGame = 0, kXmpControllerRestore = 4;
struct XmpState {
  std::mutex m;
  uint32_t client = kXmpClientGame;
  uint32_t controller = kXmpControllerGame;
  bool override_ = false;
};
XmpState& xmp() {
  static XmpState s;
  return s;
}
// Test seam (difftest N12): a spin of up to this many microseconds after an XMP state change, before the call returns
// to its publication. 0 (always, outside the test) = none.
std::atomic<uint32_t> g_xmpPublishSpinUsForTest{0};
void xmpTestSpin() {
  const uint32_t maxUs = g_xmpPublishSpinUsForTest.load(std::memory_order_relaxed);
  if (!maxUs) return;
  thread_local uint32_t x = 0x9E3779B9u ^ static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&x));
  x = x * 1664525u + 1013904223u;
  const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds((x >> 8) % maxUs);
  while (std::chrono::steady_clock::now() < until) {
  }
}
// Caller holds xmp().m.
bool xmpTitleInControlLocked() {
  return !xmp().override_ && xmp().client == kXmpClientGame && xmp().controller == kXmpControllerGame;
}
ExportFn g_prevStartIORequestEx = nullptr;
ExportFn g_prevInProcessCall = nullptr;
constexpr uint32_t kXErrIoPending = 997;   // X_ERROR_IO_PENDING

// The XMP app's handler for the controller pair, reached from BOTH transports as in Xenia (Astra P2 5): the
// AppManager's DispatchMessageSync (XMsgInProcessCall: buffer = arg1 r5) and DispatchMessageAsync
// (XMsgStartIORequestEx: buffer r6) both call XmpApp::DispatchMessageSync(message, buffer, length)
// (app_manager.cc:42-60). False for every other message: those keep the previous registration.
bool xmpDispatch(uint32_t message, uint32_t buffer, uint32_t* result) {
  if (message != kXmpSetController && message != kXmpGetController) return false;
  if (!guestRange(buffer, 12)) {   // host safety: Xenia would fault on a bad buffer
    *result = kXeInvalidArg;
    return true;
  }
  if (message == kXmpSetController) {
    struct SetArgs {
      uint32_t client, request, locked;
    } args{readGuestU32BE(buffer), readGuestU32BE(buffer + 4), readGuestU32BE(buffer + 8)};
    // xmp_app.cc:388: BroadcastNotification(kXNotificationXmpPlaybackControllerChanged, IsTitleInPlaybackControl()).
    // Astra merge review P2: the change and its notification are one step under the queue lock (lock order
    // queue -> XMP, as when priming), so two setters' notifications arrive in their state order and the last one is
    // the current state. With SR_XNOTIFY_XENIA off: the change alone under the XMP lock, as before.
    xnotifyPublishXenia(0x0A000003u, [](void* p) -> uint32_t {
      const SetArgs& a = *static_cast<const SetArgs*>(p);
      uint32_t inControl = 1;
      {
      std::lock_guard<std::mutex> lk(xmp().m);
      if (a.request == kXmpControllerRestore) {
        xmp().client = kXmpClientGame;
        xmp().controller = kXmpControllerGame;
      } else {
        xmp().client = a.client;
        xmp().controller = a.request;
      }
      xmp().override_ = a.locked != 0;
      static std::atomic<uint32_t> s_n{0};
      const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 16 || (n & (n - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[IMPORT_FIXES] XMP #%u SetPlaybackController(client=%u, request=%u, locked=%u) -> client=%u "
                    "controller=%u override=%d lr=%08X",
                    n, a.client, a.request, a.locked, xmp().client, xmp().controller, xmp().override_ ? 1 : 0,
                    getCurrentGuestLR());
      inControl = xmpTitleInControlLocked() ? 1u : 0u;
      }
      xmpTestSpin();   // difftest N12 only: widens the gap between the change and its publication
      return inControl;
    }, &args);
  } else {
    const uint32_t pController = readGuestU32BE(buffer + 4), pLocked = readGuestU32BE(buffer + 8);
    uint32_t controller, locked;
    {
      std::lock_guard<std::mutex> lk(xmp().m);
      controller = xmp().controller;
      locked = xmpTitleInControlLocked() ? 0u : 1u;
    }
    if (guestRange(pController, 4)) writeGuestBE32(pController, controller);
    if (guestRange(pLocked, 4)) writeGuestBE32(pLocked, locked);
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16 || (n & (n - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[IMPORT_FIXES] XMP #%u GetPlaybackController -> controller=%u locked=%u lr=%08X", n, controller,
                  locked, getCurrentGuestLR());
  }
  *result = kXeSuccess;
  return true;
}

// Xenia KernelState::CompleteOverlappedImmediate (kernel_state.cc:1091-1133): length = result ? 0xFFFFFFFF : 0,
// extended error = result, InternalContext = the calling thread's handle, then the event is set and a completion
// routine is queued as an APC. XOVERLAPPED: +0 result, +4 length, +8 context, +0xC hEvent, +0x10 routine,
// +0x14 routine context, +0x18 extended error. The result word is written last (callers poll [0] != 997).
// Not reproduced: the completion-routine APC (logged; the title's XMP calls pass no overlapped at all -
// XmpXMsg7001A_sub_824704B0 hands XMsgStartIORequestEx r5 = 0).
void xmpCompleteOverlapped(uint32_t ovl, uint32_t result) {
  if (!guestRange(ovl, 0x1C)) return;
  writeGuestBE32(ovl + 0x04, result ? 0xFFFFFFFFu : 0u);
  writeGuestBE32(ovl + 0x18, result);
  writeGuestBE32(ovl + 0x08, getCurrentThreadHandle());
  writeGuestBE32(ovl + 0x00, result);
  const uint32_t hEvent = readGuestU32BE(ovl + 0x0C), routine = readGuestU32BE(ovl + 0x10);
  bool set = false;
  if (xmpAppXeniaOn()) set = hEvent && ntEventSetByHandle(hEvent);   // (registered only under this gate)
  static std::atomic<uint32_t> s_n{0};
  if (s_n.fetch_add(1, std::memory_order_relaxed) < 8)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[IMPORT_FIXES] XMP overlapped %08X completed: result=%08X hEvent=%08X %s routine=%08X%s", ovl,
                result, hEvent, hEvent ? (set ? "set" : "NOT an event") : "-", routine,
                routine ? " (APC not queued)" : "");
}

uint32_t exp_XMsgStartIORequestEx(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8) {
  uint32_t result = kXeSuccess;
  if (r3 != kXmpApp || !xmpDispatch(r4, r6, &result))
    return g_prevStartIORequestEx ? g_prevStartIORequestEx(r3, r4, r5, r6, r7, r8) : kXeSuccess;
  // xam_msg.cc:48-67: an overlapped request is completed at once and the call answers X_ERROR_IO_PENDING (Astra
  // P2 4). Xenia also sets the thread's last error to 0 then; the one caller (XmpXMsg7001A) only tests
  // GetLastError() >= 0 after a success, so it is not reproduced.
  if (r5) {
    xmpCompleteOverlapped(r5, result);
    result = kXErrIoPending;
  }
  return result;
}

uint32_t exp_XMsgInProcessCall(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8) {
  uint32_t result = kXeSuccess;
  if (r3 == kXmpApp && xmpDispatch(r4, r5, &result)) return result;
  return g_prevInProcessCall ? g_prevInProcessCall(r3, r4, r5, r6, r7, r8) : kXeSuccess;
}

// ── batch 2: SR_RTL_TIMEFIELDS_XENIA (logic audit row 7) ─────────────────────────────────────────────────────
// Xenia xboxkrnl_rtl.cc:724-749: X_TIME_FIELDS = 8 BE WORDs {year, month, day, hour, minute, second, milliseconds,
// weekday}. Any field out of range, or a date that does not exist (date::year_month_day::ok), -> FALSE and nothing
// written; else *time (BE 64-bit) = the FILETIME (100 ns since 1601-01-01 UTC) and TRUE. The weekday is ignored.
// The old registration was a generic stub (FALSE, nothing written), so the time-zone DST-cutover helper
// sub_828A8778 (GetTimeZoneInformation's) never found a cutover: local time = UTC + bias on ours, while Xenia's
// London default (SR_XCONFIG_XENIA) is BST in summer.
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {   // H. Hinnant's days_from_civil: days since 1970-01-01
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = unsigned(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + int64_t(doe) - 719468;
}
uint16_t readBE16(uint32_t va) {
  const uint8_t* p = guestVAToHost(va);
  return p ? uint16_t((p[0] << 8) | p[1]) : 0;
}
// Bounded witness counter for the batch-2 lines: calls 1..cap, then powers of two.
bool b2WitnessDue(std::atomic<uint32_t>& counter, uint32_t cap, uint32_t* nOut) {
  const uint32_t n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  *nOut = n;
  return n <= cap || (n & (n - 1)) == 0;
}
uint32_t rtlTimeFieldsBody(uint32_t r3, uint32_t r4, unsigned* f) {
  if (!guestRange(r3, 16)) return 0;   // host safety; Xenia would fault
  for (int i = 0; i < 7; ++i) f[i] = readBE16(r3 + 2 * i);
  const unsigned year = f[0], month = f[1], day = f[2], hour = f[3], minute = f[4], second = f[5], ms = f[6];
  if (year < 1601 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59 ||
      ms > 999)
    return 0;
  static const unsigned kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (day > kDays[month - 1] + ((month == 2 && leap) ? 1u : 0u)) return 0;
  if (!guestRange(r4, 8)) return 0;    // host safety; Xenia would fault
  const int64_t days = daysFromCivil(year, month, day) - daysFromCivil(1601, 1, 1);
  const uint64_t ft = uint64_t((((days * 24 + hour) * 60 + minute) * 60 + second) * 1000 + ms) * 10000ull;
  ifxStoreBE64(r4, ft);
  return 1;
}
uint32_t exp_RtlTimeFieldsToTime(uint32_t r3, uint32_t r4, uint32_t, uint32_t, uint32_t, uint32_t) {
  unsigned f[7] = {0, 0, 0, 0, 0, 0, 0};
  const uint32_t rc = rtlTimeFieldsBody(r3, r4, f);
  static std::atomic<uint32_t> s_n{0};
  uint32_t n = 0;
  if (b2WitnessDue(s_n, 16, &n))
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[IMPORT_FIXES] TIMEFIELDS #%u RtlTimeFieldsToTime(%04u-%02u-%02u %02u:%02u:%02u.%03u) -> %u lr=%08X", n,
                f[0], f[1], f[2], f[3], f[4], f[5], f[6], rc, getCurrentGuestLR());
  return rc;
}

// ── batch 2: SR_RTL_MBCS_XENIA (logic audit rows 11, 12) ───────────────────────────────────────────────────
// Xenia xboxkrnl_rtl.cc:350-395 (kSketchy: a byte <-> UTF-16 code unit copy, not a code-page conversion):
//   RtlMultiByteToUnicodeN(dest r3, dest_len r4 bytes, *written r5, src r6, src_len r7): n = min(dest_len / 2,
//     src_len) code units dest[i] = src[i]; *written = n * 2; 0.
//   RtlUnicodeToMultiByteN(dest r3, dest_len r4, *written r5, src r6, src_len r7 bytes): n = min(src_len / 2,
//     dest_len) bytes dest[i] = src[i] < 256 ? src[i] : '?'; *written = n; 0.
// The old registrations were generic stubs: nothing converted, *written untouched, 0 - the XAPI MultiByteToWideChar
// / WideCharToMultiByte wrappers (0x824699B8 / 0x824696E4) saw success with untouched buffers.
uint32_t exp_RtlMultiByteToUnicodeN(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t) {
  uint32_t n = r4 >> 1;
  if (r7 < n) n = r7;
  if (n && (!guestRange(r3, n * 2) || !guestRange(r6, n))) return 0;   // host safety; Xenia would fault
  for (uint32_t i = 0; i < n; ++i) ifxStoreBE16(r3 + 2 * i, *guestVAToHost(r6 + i));
  if (r5 && guestRange(r5, 4)) writeGuestBE32(r5, n << 1);
  static std::atomic<uint32_t> s_n{0};
  uint32_t k = 0;
  if (b2WitnessDue(s_n, 8, &k))
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[IMPORT_FIXES] MBCS #%u RtlMultiByteToUnicodeN(dest_len=%u, src_len=%u) -> %u units lr=%08X", k, r4,
                r7, n, getCurrentGuestLR());
  return 0;
}
uint32_t exp_RtlUnicodeToMultiByteN(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t) {
  uint32_t n = r7 >> 1;
  if (r4 < n) n = r4;
  if (n && (!guestRange(r3, n) || !guestRange(r6, n * 2))) return 0;   // host safety; Xenia would fault
  for (uint32_t i = 0; i < n; ++i) {
    const uint16_t c = readBE16(r6 + 2 * i);
    *guestVAToHost(r3 + i) = c < 256 ? uint8_t(c) : uint8_t('?');
  }
  if (r5 && guestRange(r5, 4)) writeGuestBE32(r5, n);
  static std::atomic<uint32_t> s_n{0};
  uint32_t k = 0;
  if (b2WitnessDue(s_n, 8, &k))
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[IMPORT_FIXES] MBCS #%u RtlUnicodeToMultiByteN(dest_len=%u, src_len=%u) -> %u bytes lr=%08X", k, r4,
                r7, n, getCurrentGuestLR());
  return 0;
}

}  // namespace

bool xmpTitleInPlaybackControl() {
  std::lock_guard<std::mutex> lk(xmp().m);
  return xmpTitleInControlLocked();
}

void xmpPublishSpinForTest(uint32_t maxUs) { g_xmpPublishSpinUsForTest.store(maxUs, std::memory_order_relaxed); }

void registerImportFixes(ExportRegistry& reg) {
  const bool xuid = xamXuidAbiOn(), xcfg = xconfigXeniaOn(), sysver = sysverXeniaOn(), xmpApp = xmpAppXeniaOn();
  if (xuid) reg.registerExport("XamUserGetXUID", exp_XamUserGetXUID);
  if (xcfg) reg.registerExport("ExGetXConfigSetting", exp_ExGetXConfigSetting);
  if (sysver) reg.registerExport("XamGetSystemVersion", exp_XamGetSystemVersion);
  if (xmpApp) {
    // registerStubs() runs twice at boot (runHarness, then runGuestHarness): never chain to ourselves. Today both
    // originals are re-registered earlier in the same registerStubs(), so this only guards a future order change.
    const ExportFn startIo = reg.getExport("XMsgStartIORequestEx"), inProc = reg.getExport("XMsgInProcessCall");
    if (startIo != exp_XMsgStartIORequestEx) g_prevStartIORequestEx = startIo;
    if (inProc != exp_XMsgInProcessCall) g_prevInProcessCall = inProc;
    reg.registerExport("XMsgStartIORequestEx", exp_XMsgStartIORequestEx);
    reg.registerExport("XMsgInProcessCall", exp_XMsgInProcessCall);
  }
  // The boot line names every gate of the batch, including the two that live elsewhere (SR_NT_EVENT_TYPE in
  // kernel/stubs.cpp, SR_NTSTATUS_DOSERR in kernel/xam_content.cpp). IMPORT_FIXES is a player-log milestone tag
  // (app/log.cpp), so a player-profile roll keeps these lines too; none is written while every gate is off.
  const bool ntEvent = gateDefaultOn("SR_NT_EVENT_TYPE"), dosErr = gateDefaultOn("SR_NTSTATUS_DOSERR");
  // batch 2 (baked; the two kernel/stubs.cpp gates are read here through gateDefaultOn, as batch 1's are)
  const bool timeFields = rtlTimeFieldsXeniaOn(), mbcs = rtlMbcsXeniaOn(), xnotify = gateDefaultOn("SR_XNOTIFY_XENIA"),
             xapiStart = gateDefaultOn("SR_XAPI_THREAD_STARTUP");
  if (timeFields) reg.registerExport("RtlTimeFieldsToTime", exp_RtlTimeFieldsToTime);
  if (mbcs) {
    reg.registerExport("RtlMultiByteToUnicodeN", exp_RtlMultiByteToUnicodeN);
    reg.registerExport("RtlUnicodeToMultiByteN", exp_RtlUnicodeToMultiByteN);
  }
  if (timeFields || mbcs || xnotify || xapiStart)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[IMPORT_FIXES] batch 2: SR_RTL_TIMEFIELDS_XENIA=%d SR_RTL_MBCS_XENIA=%d SR_XNOTIFY_XENIA=%d "
                "SR_XAPI_THREAD_STARTUP=%d",
                timeFields ? 1 : 0, mbcs ? 1 : 0, xnotify ? 1 : 0, xapiStart ? 1 : 0);
  // batch 3 (baked 2026-10-20; the ATTRS gate lives in kernel/xam_content.cpp). Its second item, kernel/stubs.cpp's
  // optional-data alias in NtCreateFile, was DELETED with its gate; the line says so, so a log names its tree.
  const bool attrs = gateDefaultOn("SR_IMPORT_FIX_ATTRS");
  SR_LOG_BOOT(sr::LogSeverity::Info, "[IMPORT_FIXES] batch 3: SR_IMPORT_FIX_ATTRS=%d, optional-data alias deleted",
              attrs ? 1 : 0);
  if (xuid || xcfg || sysver || xmpApp || ntEvent || dosErr)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[IMPORT_FIXES] Xenia 0bd090dbe answers: SR_XAM_XUID_ABI=%d SR_XCONFIG_XENIA=%d "
                "SR_XAM_SYSVER_XENIA=%d SR_XMP_APP_XENIA=%d SR_NT_EVENT_TYPE=%d SR_NTSTATUS_DOSERR=%d",
                xuid ? 1 : 0, xcfg ? 1 : 0, sysver ? 1 : 0, xmpApp ? 1 : 0, ntEvent ? 1 : 0, dosErr ? 1 : 0);
  // step5 E14a: XMsgStartIORequest XGI 0xFB / XUserWriteAchievements 0xB0008 recorded, the old answer kept
  // (kernel/achievements.hpp; SR_ACHIEVEMENTS, default ON). After the generic stub above (registerStubs).
  sr::ach::registerAchievementExports(reg);
}

const uint8_t* importFixesXConfigImageForTest(uint32_t* size) {
  if (size) *size = kXConfigDataSize;
  return xconfigImage().bytes;
}

}  // namespace kernel
}  // namespace sr
