// iter-463 PLATFORM FIX: XamUserReadProfileSettings. Contract, gate and references:
// kernel/xam_profile.hpp.
//
// Ported from xenia-canary @0b4e6c4 (BSD-3; Copyright Ben Vanik and Xenia contributors):
// kernel/xam/xam_user.cc XamUserReadProfileSettingsEx, user_settings.{h,cc}
// (known_settings, default_setting_values, GetDefaultSetting), user_profile.h
// (X_USER_PROFILE_SETTING) and xam.h (X_USER_READ_PROFILE_SETTINGS).
#include "xam_profile.hpp"

#include "export_registry.hpp"
#include "guest_context.hpp"
#include "xam_content.hpp"
#include "../app/log.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace sr {
namespace kernel {

namespace {
std::atomic<int> g_testMode{-1};
ExportFn g_prev = nullptr;

bool envOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_XAM_PROFILE");
    return !e || e[0] != '0';
  }();
  return v;
}

constexpr uint32_t kErrSuccess = 0;
constexpr uint32_t kErrInvalidParameter = 0x57;
constexpr uint32_t kErrInsufficientBuffer = 0x7A;
constexpr uint32_t kErrFunctionFailed = 0x65B;
constexpr uint32_t kErrIoPending = 997;
constexpr uint32_t kENoSuchUser = 0x80070525u;   // X_HRESULT_FROM_WIN32(0x525)
constexpr uint32_t kSettingSize = 40;            // X_USER_PROFILE_SETTING
constexpr uint32_t kHeaderSize = 8;              // X_USER_READ_PROFILE_SETTINGS

// user_settings.h known_settings (115 ids, in Xenia's order).
constexpr uint32_t kKnown[] = {
    0x10040000u, 0x10040001u, 0x10040002u, 0x10040003u, 0x10040004u, 0x10040005u,
    0x10040006u, 0x10040007u, 0x10040008u, 0x1004000Cu, 0x1004000Du, 0x1004000Eu,
    0x10040012u, 0x10040013u, 0x10040015u, 0x10040018u, 0x1004001Du, 0x1004001Eu,
    0x10040022u, 0x10040023u, 0x10040024u, 0x10040026u, 0x10040027u, 0x10040028u,
    0x10040029u, 0x10040038u, 0x10040039u, 0x1004003Au, 0x1004003Bu, 0x1004003Cu,
    0x1004003Du, 0x1004003Eu, 0x1004003Fu, 0x10040040u, 0x10040041u, 0x10040042u,
    0x10040043u, 0x10040044u, 0x10040045u, 0x10040046u, 0x10040047u, 0x10040048u,
    0x10040049u, 0x1004004Au, 0x1004004Bu, 0x1004004Cu, 0x1004004Du, 0x1004004Eu,
    0x1004004Fu, 0x10040050u, 0x10040051u, 0x10040052u, 0x10040053u, 0x10040057u,
    0x10042000u, 0x10042001u, 0x10042002u, 0x10042003u, 0x10042004u, 0x10042005u,
    0x10042006u, 0x10042007u, 0x10042008u, 0x10042009u, 0x1004200Au, 0x1004200Bu,
    0x1004200Cu, 0x1004200Du, 0x100403EAu, 0x100403EBu, 0x100403ECu, 0x100403EDu,
    0x100403EEu, 0x100403EFu, 0x100403F0u, 0x100403F1u, 0x100403F3u, 0x40520041u,
    0x41040040u, 0x41900042u, 0x43E80043u, 0x43E803FAu, 0x406403FBu, 0x406403FCu,
    0x406403FDu, 0x406403FEu, 0x4064000Fu, 0x40640010u, 0x402C0011u, 0x40C80049u,
    0x40C8004Bu, 0x406403E8u, 0x410003F6u, 0x5004000Bu, 0x5004004Cu, 0x5004004Du,
    0x6010004Au, 0x6020004Au, 0x603403F2u, 0x61180050u, 0x63E80051u, 0x60620054u,
    0x60A803F5u, 0x601003F7u, 0x601003F8u, 0x601003F9u, 0x63E80044u, 0x63E80045u,
    0x61000046u, 0x63E83FFFu, 0x63E83FFEu, 0x63E83FFDu, 0x700803F4u, 0x70080049u,
    0x7008004Fu,
};
bool isKnown(uint32_t id) {
  return std::find(std::begin(kKnown), std::end(kKnown), id) != std::end(kKnown);
}
uint8_t keyType(uint32_t id) { return uint8_t(id >> 28); }
uint32_t keySize(uint32_t id) { return (id >> 16) & 0xFFFu; }
constexpr uint8_t kTypeWString = 4, kTypeBinary = 6;

bool rangeOk(uint32_t va, uint32_t len) {
  if (va == 0) return false;
  const uint64_t end = uint64_t(va) + (len ? len - 1 : 0);
  if (end > 0xFFFFFFFFull) return false;
  return isGuestVAInRange(va) && isGuestVAInRange(uint32_t(end));
}
uint32_t rd32(uint32_t va) { return rangeOk(va, 4) ? readGuestU32BE(va) : 0; }
void wr32(uint32_t va, uint32_t v) {
  if (rangeOk(va, 4)) writeGuestBE32(va, v);
}

// One log line per call up to 8, then powers of two (count-safe: "#n" is the call count).
void logCall(uint32_t user, uint32_t count, uint32_t idsVA, uint32_t rc, uint32_t size,
             uint32_t ovl) {
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = s_n.fetch_add(1) + 1;
  if (n > 8 && (n & (n - 1)) != 0) return;
  char ids[64] = "";
  for (uint32_t i = 0, o = 0; i < count && i < 4 && o + 10 < sizeof ids; ++i)
    o += uint32_t(std::snprintf(ids + o, sizeof ids - o, "%s%08X", i ? "," : "", rd32(idsVA + 4 * i)));
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[XAM_PROFILE] #%u XamUserReadProfileSettings user=%u count=%u ids=[%s] size=%u "
              "ovl=%08X -> %u",
              n, user, count, ids, size, ovl, rc);
}
}  // namespace

bool xamProfileEnabled() {
  const int m = g_testMode.load(std::memory_order_relaxed);
  return m < 0 ? envOn() : m != 0;
}
void xamProfileSetEnabledForTest(int mode) { g_testMode.store(mode); }

uint32_t xamReadProfileSettings(uint32_t titleId, uint32_t userIndex, uint32_t xuidCount,
                                uint32_t xuidsVA, uint32_t settingCount, uint32_t idsVA,
                                uint32_t pSizeVA, uint32_t bufferVA, uint32_t overlappedVA) {
  (void)titleId;   // title settings (0x63E83FFx) have no stored value here: defaults either way
  // must have 1 to 32 settings; the size pointer must be valid
  if (settingCount < 1 || settingCount > 32) return kErrInvalidParameter;
  if (!rangeOk(pSizeVA, 4) || !rangeOk(idsVA, 4 * settingCount)) return kErrInvalidParameter;
  const uint32_t bufferSize = rd32(pSizeVA);
  if (bufferSize && !bufferVA) return kErrInvalidParameter;

  uint32_t headerSize = 0, dataSize = 0;
  for (uint32_t i = 0; i < settingCount; ++i) {
    headerSize += kSettingSize;
    const uint32_t id = rd32(idsVA + 4 * i);
    if (keyType(id) == kTypeWString || keyType(id) == kTypeBinary) dataSize += keySize(id);
  }
  if (xuidsVA) {
    headerSize *= xuidCount;
    dataSize *= xuidCount;
  }
  headerSize += kHeaderSize;
  const uint32_t needed = headerSize + dataSize;
  if (!bufferVA || bufferSize < needed) {
    if (!bufferSize) wr32(pSizeVA, needed);
    return kErrInsufficientBuffer;
  }
  if (!rangeOk(bufferVA, needed)) return kErrInvalidParameter;

  // run(): only user 0 is signed in (stub_XamUserGetSigninState / kXamUser0Xuid).
  uint32_t rc = kErrSuccess, ext = 0;
  bool userOk = userIndex == 0;
  if (xuidsVA) {
    const uint64_t x = (uint64_t(rd32(xuidsVA)) << 32) | rd32(xuidsVA + 4);
    userOk = x == kXamUser0Xuid;
  }
  if (!userOk) {
    rc = kErrFunctionFailed;
    ext = kENoSuchUser;
  } else {
    uint8_t* out = guestVAToHost(bufferVA);
    std::memset(out, 0, headerSize);
    const uint32_t settingsVA = bufferVA + kHeaderSize;
    uint32_t outCount = settingCount;
    uint32_t cur = settingsVA;
    for (uint32_t n = 0; n < settingCount; ++n) {
      const uint32_t id = rd32(idsVA + 4 * n);
      if (!isKnown(id)) {   // Xenia: dropped from the count, the slot is not consumed
        --outCount;
        continue;
      }
      // GetDefaultSetting: CONTROLLER_VIBRATION 3, every other value 0 / empty.
      const uint32_t value = id == 0x10040003u ? 3u : 0u;
      wr32(cur + 0x00, 1);                       // source = DEFAULT
      if (xuidsVA) {
        wr32(cur + 0x08, uint32_t(kXamUser0Xuid >> 32));
        wr32(cur + 0x0C, uint32_t(kXamUser0Xuid));
      } else {
        wr32(cur + 0x08, userIndex);             // xuid = -1, then user_index over its high half
        wr32(cur + 0x0C, 0xFFFFFFFFu);
      }
      wr32(cur + 0x10, id);                      // setting_id
      *guestVAToHost(cur + 0x18) = keyType(id);  // X_USER_DATA.type
      wr32(cur + 0x20, value);                   // X_USER_DATA.data (s32 / low bytes)
      cur += kSettingSize;
    }
    wr32(bufferVA + 0, outCount);
    wr32(bufferVA + 4, settingsVA);
  }

  if (!overlappedVA) {
    logCall(userIndex, settingCount, idsVA, rc, bufferSize, 0);
    return rc;
  }
  // CompleteOverlappedDeferredEx: result, extended error, length 0; the guest spins on
  // [ovl+0] == 997 (ProfileSettings_asyncRead_sub_822A4548), so it is completed first.
  if (rangeOk(overlappedVA, 0x1C)) {
    wr32(overlappedVA + 0x04, 0);
    wr32(overlappedVA + 0x18, ext);
    wr32(overlappedVA + 0x00, rc);
  }
  logCall(userIndex, settingCount, idsVA, kErrIoPending, bufferSize, overlappedVA);
  return kErrIoPending;
}

namespace {
uint32_t exp_XamUserReadProfileSettings(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                                        uint32_t r7, uint32_t r8) {
  if (!xamProfileEnabled()) return g_prev ? g_prev(r3, r4, r5, r6, r7, r8) : 0u;
  const uint32_t pSize = ExportRegistry::getCurrentArgR9();
  const uint32_t buffer = ExportRegistry::getCurrentArgR10();
  const uint32_t r1 = getCurrentGuestR1();
  const uint32_t ovl = (r1 && isGuestVAInRange(r1 + 0x54u)) ? readGuestU32BE(r1 + 0x54u) : 0u;
  return xamReadProfileSettings(r3, r4, r5, r6, r7, r8, pSize, buffer, ovl);
}
}  // namespace

void registerXamProfileExports(ExportRegistry& reg) {
  if (!envOn()) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[XAM_PROFILE] SR_XAM_PROFILE=0: XamUserReadProfileSettings stays the generic stub");
    return;
  }
  g_prev = reg.getExport("XamUserReadProfileSettings");
  reg.registerExport("XamUserReadProfileSettings", exp_XamUserReadProfileSettings);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[XAM_PROFILE] XamUserReadProfileSettings: Xenia profile defaults "
              "(vibration 3, others 0)");
}

bool xamUserNameEnabled() {
  static const bool on = [] {
    const char* e = std::getenv("SR_XAM_USERNAME");
    return !e || e[0] != '0';
  }();
  return on;
}

std::string xamProfileGamertag() {
  std::ifstream f(xamContentRoot() / "settings" / "profile.ini");
  std::string line;
  while (f && std::getline(f, line)) {
    if (line.rfind("gamertag=", 0) != 0) continue;
    std::string v = line.substr(9);
    while (!v.empty() && (v.back() == 0x0D || v.back() == 0x20 || v.back() == 0x09)) v.pop_back();
    bool ok = !v.empty() && v.size() <= 15;
    for (char c : v) ok = ok && c >= 0x20 && c < 0x7F;
    if (ok) return v;
  }
  return "Player";
}

uint32_t xamUserGetName(uint32_t userIndex, uint32_t bufferVA, uint32_t bufferLen) {
  constexpr uint32_t kErrInvalidParameter = 0x57;
  constexpr uint32_t kErrNoSuchUser = 0x525;
  if (userIndex >= 4) return kErrInvalidParameter;
  if (userIndex != 0) {   // only user 0 is signed in (stub_XamUserGetSigninState)
    if (bufferVA && isGuestVAInRange(bufferVA)) *guestVAToHost(bufferVA) = 0;
    return kErrNoSuchUser;
  }
  static const std::string name = xamProfileGamertag();
  const uint32_t n = std::min<uint32_t>(bufferLen, uint32_t(name.size()) + 1);
  if (n && bufferVA && isGuestVAInRange(bufferVA) && isGuestVAInRange(bufferVA + n - 1)) {
    uint8_t* d = guestVAToHost(bufferVA);
    std::memcpy(d, name.data(), n - 1);
    d[n - 1] = 0;
  }
  return 0;
}

}  // namespace kernel
}  // namespace sr
