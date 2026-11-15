// step4/audit (logic audit 9a, docs/LOGIC_AUDIT.md 2.0): the import fixes against Xenia 0bd090dbe's answers.
//
// Drives the REAL exports through the export registry after the real registerStubs() (so the wiring - which
// handler wins - is tested, not a helper body). Every gate it covers is baked default ON in the exe (batch 1
// 2026-10-18, batches 2 and 3 2026-10-20), and the test leaves an unset gate unset, so the plain run checks the exe's
// defaults; a gate the caller sets to 0 is that gate's MUTANT run:
//
//   build/difftest.exe --import-fixes-test                         the defaults (all on): exit = failed checks (expect 0)
//   SR_XAM_XUID_ABI=0      ... --import-fixes-test   -> X1 X2 X3 X4 FAIL (old r4-as-out-pointer stub)
//   SR_NT_EVENT_TYPE=0     ... --import-fixes-test   -> E1 E3 E5-E8 FAIL (auto-reset, initial state ignored, generic NtClearEvent)
//   SR_XCONFIG_XENIA=0     ... --import-fixes-test   -> C1..C7 FAIL (old table / sizes / statuses)
//   SR_XAM_SYSVER_XENIA=0  ... --import-fixes-test   -> V1 FAIL (0x20449700)
//   SR_XMP_APP_XENIA=0     ... --import-fixes-test   -> M1..M7 FAIL (generic XMsgInProcessCall writes nothing)
//   SR_NTSTATUS_DOSERR=0   ... --import-fixes-test   -> D1 FAIL (generic table: every status -> 0)
// batch 2 (2026-10-18, baked 2026-10-20):
//   SR_RTL_TIMEFIELDS_XENIA=0 ... -> T1 T2 T4 FAIL (generic RtlTimeFieldsToTime: FALSE, nothing written)
//   SR_RTL_MBCS_XENIA=0       ... -> U1 U2 FAIL (generic converters write nothing)
//   SR_XNOTIFY_XENIA=0        ... -> N1 N3-N14 FAIL (no priming, out-params untouched, no filter, no wait handle,
//                                    no broadcast, no wake after a close, no ordered publication, no failed create,
//                                    no refused second close; N2 holds either way: with the gate off nothing is
//                                    primed)
//   SR_XAPI_THREAD_STARTUP=0  ... -> S1 S2 S3 FAIL (the entry stays the start routine)
// batch 3 (2026-10-20, baked the same day):
//   SR_IMPORT_FIX_ATTRS=0     ... -> A1-A6 FAIL (the generic stub: SUCCESS for every path, the info unwritten).
//   L1-L3: the optional-data alias is DELETED (no gate). Before the bake its =0 arm failed L1 (objtiv1.AST opened
//   objtiv.AST).
//   A1-A8 and L1-L3 run on an isolated guest file system: a temp content root with its own disc tree and one mod
//   (setContentRootForTest / modOverlayResetForTest), queried only through guest paths (d:\, game:\).
// Optional oracle compare: SR_XCONFIG_ORACLE_FILE=<a Xenia-written xconfig.settings> adds C8 (the whole 6680-byte
// XConfigData image our ExGetXConfigSetting answers from equals the file byte for byte).
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/export_registry.hpp"
#include "kernel/file_io.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/import_fixes.hpp"
#include "kernel/mod_overlay.hpp"
#include "kernel/stubs.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace K = sr::kernel;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}

uint8_t* g_base = nullptr;
constexpr size_t kGuestSize = 0x100000000ull;
constexpr uint32_t kOut = 0x90700000u;        // 64-byte scratch out-params
constexpr uint32_t kBuf = 0x90700100u;        // 256-byte buffers
constexpr uint32_t kArgs = 0x90700400u;       // message argument blocks
constexpr uint32_t kTimeout = 0x90700500u;    // LARGE_INTEGER 0 = poll

K::ExportFn getExp(const char* name) { return K::ExportRegistry::instance().getExport(name); }
uint32_t call(const char* name, uint32_t a3 = 0, uint32_t a4 = 0, uint32_t a5 = 0, uint32_t a6 = 0,
              uint32_t a7 = 0, uint32_t a8 = 0) {
  K::ExportRegistry::setCurrentExtraArgs(0, 0);
  return getExp(name)(a3, a4, a5, a6, a7, a8);
}
uint8_t* host(uint32_t va) { return K::guestVAToHost(va); }
void fill(uint32_t va, uint8_t b, uint32_t n) { std::memset(host(va), b, n); }
uint64_t rd64(uint32_t va) { return (uint64_t(K::readGuestU32BE(va)) << 32) | K::readGuestU32BE(va + 4); }
uint16_t rd16(uint32_t va) { const uint8_t* p = host(va); return uint16_t((p[0] << 8) | p[1]); }
bool allBytes(uint32_t va, uint8_t b, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) if (host(va)[i] != b) return false;
  return true;
}

struct Ctx {
  PPCContext c{};
  explicit Ctx(uint32_t stack) {
    K::setGuestMemory(g_base, kGuestSize);
    c.r1.u32 = stack;
    c.r14.u32 = 0x829C0000u;
    K::setCurrentGuestContext(&c, offsetof(PPCContext, lr), offsetof(PPCContext, ctr),
                              offsetof(PPCContext, r3), offsetof(PPCContext, r1),
                              offsetof(PPCContext, r31));
  }
  ~Ctx() { K::setCurrentGuestContext(nullptr, 0); }
};

// Every gate here is baked default ON: an unset gate is left unset (the plain run checks the defaults, 2026-10-20;
// until then this set each unset gate to 1).
void showGate(const char* gate) {
  const char* v = std::getenv(gate);
  std::printf("  gate %s=%s\n", gate, v ? v : "(unset: the baked default, ON)");
}
}  // namespace

int runImportFixesTest() {
  std::printf("[IMPORT_FIXES_TEST] import answers vs Xenia 0bd090dbe (logic audit 9a)\n");
  for (const char* g : {"SR_XAM_XUID_ABI", "SR_NT_EVENT_TYPE", "SR_XCONFIG_XENIA", "SR_XAM_SYSVER_XENIA",
                        "SR_XMP_APP_XENIA", "SR_NTSTATUS_DOSERR", "SR_RTL_TIMEFIELDS_XENIA", "SR_RTL_MBCS_XENIA",
                        "SR_XNOTIFY_XENIA", "SR_XAPI_THREAD_STARTUP"})
    showGate(g);
  showGate("SR_IMPORT_FIX_ATTRS");   // batch 3 (baked 2026-10-20)
  g_base = K::getGuestBase();
  K::registerStubs();
  Ctx mainCtx(0x90100000u);

  // ── X: XamUserGetXUID(user r3, type_mask r4, pxuid r5) - xam_user.cc:31-66 ───────────────────────────────
  {
    fill(0, 0xAB, 16);                 // the null page the old stub wrote into (r4 = 7 / 2)
    fill(kOut, 0xCC, 8);
    uint32_t st = call("XamUserGetXUID", 0, 7, kOut);
    std::printf("       X1: st=%08X xuid=%016llX null-page-intact=%d\n", st, (unsigned long long)rd64(kOut),
                allBytes(0, 0xAB, 16) ? 1 : 0);
    check(st == 0 && rd64(kOut) == 0x0009000000000001ull && allBytes(0, 0xAB, 16),
          "X1 user 0, mask 7: X_E_SUCCESS, *r5 = our profile XUID, nothing written at r4");
    fill(kOut, 0xCC, 8);
    st = call("XamUserGetXUID", 1, 7, kOut);
    std::printf("       X2: st=%08X xuid=%016llX\n", st, (unsigned long long)rd64(kOut));
    check(st == 0x80070525u && rd64(kOut) == 0, "X2 user 1 (not signed in): X_E_NO_SUCH_USER, *xuid = 0");
    fill(kOut, 0xCC, 8);
    st = call("XamUserGetXUID", 4, 7, kOut);
    check(st == 0x80070057u && rd64(kOut) == 0, "X3 user 4: X_E_INVALIDARG, *xuid = 0");
    st = call("XamUserGetXUID", 0, 7, 0);
    check(st == 0x80070057u, "X4 NULL xuid pointer: X_E_INVALIDARG");
    fill(kOut, 0xCC, 8);
    st = call("XamUserGetXUID", 0, 2, kOut);
    check(st == 0 && rd64(kOut) == 0x0009000000000001ull, "X5 mask 2 (online): X_E_SUCCESS (type 3 & 2)");
  }

#ifdef _WIN32
  // ── E: NtCreateEvent type / initial state, NtClearEvent - xboxkrnl_threading.cc:622-653, 717-734 ────────
  {
    K::writeGuestBE32(kTimeout, 0);
    K::writeGuestBE32(kTimeout + 4, 0);
    auto create = [](uint32_t type, uint32_t initial) {
      K::writeGuestBE32(kOut, 0);
      call("NtCreateEvent", kOut, 0, type, initial);
      return K::readGuestU32BE(kOut);
    };
    auto poll = [](uint32_t h) { return call("NtWaitForSingleObjectEx", h, 1, 0, kTimeout); };
    const uint32_t hm = create(0, 1);  // NotificationEvent (manual reset), signalled
    const uint32_t w1 = poll(hm), w2 = poll(hm);
    std::printf("       E1: manual+signalled poll %08X then %08X\n", w1, w2);
    check(hm != 0 && w1 == 0 && w2 == 0, "E1 type 0 initial 1: signalled and stays signalled (manual reset)");
    const uint32_t cs = call("NtClearEvent", hm);
    const uint32_t w3 = poll(hm);
    std::printf("       E2: NtClearEvent %08X, poll %08X\n", cs, w3);
    check(cs == 0 && w3 == 0x102u, "E2 NtClearEvent resets it: the next poll is STATUS_TIMEOUT");
    const uint32_t ha = create(1, 1);  // SynchronizationEvent (auto reset), signalled
    const uint32_t a1 = poll(ha), a2 = poll(ha);
    std::printf("       E3: auto+signalled poll %08X then %08X\n", a1, a2);
    check(a1 == 0 && a2 == 0x102u, "E3 type 1 initial 1: one wait consumes the signal (auto reset)");
    const uint32_t hu = create(0, 0);
    check(poll(hu) == 0x102u, "E4 type 0 initial 0: starts unsignalled");
    check(call("NtClearEvent", 0x0007FFF0u) == 0xC0000008u, "E5 NtClearEvent(unknown handle): INVALID_HANDLE");
    // E6 (Astra P2 2): the table also holds semaphores - Xenia's LookupObject<XEvent> refuses them.
    K::writeGuestBE32(kOut, 0);
    call("NtCreateSemaphore", kOut, 0, 1, 1);
    const uint32_t hs = K::readGuestU32BE(kOut);
    const uint32_t cs6 = call("NtClearEvent", hs);
    const uint32_t ws6 = poll(hs);
    std::printf("       E6: semaphore %08X NtClearEvent %08X, then poll %08X\n", hs, cs6, ws6);
    check(hs != 0 && cs6 == 0xC0000008u && ws6 == 0,
          "E6 NtClearEvent(semaphore handle): INVALID_HANDLE, the semaphore untouched (count 1 still taken)");
    call("NtClose", hs);
    // E7: a closed event handle is no longer an event.
    const uint32_t hc = create(0, 1);
    call("NtClose", hc);
    check(call("NtClearEvent", hc) == 0xC0000008u, "E7 NtClearEvent(closed event handle): INVALID_HANDLE");
    // E8 (Astra P2 1): the clear holds the table lock through the reset, so a concurrent close + create cannot
    // hand it a recycled host HANDLE. Thread A: create h1 (published), close it, create h2 (signalled, never
    // published), require h2 still signalled. Thread B: NtClearEvent on whatever A published. Pre-fix code (lookup,
    // unlock, ResetEvent) could reset h2 when Windows reused h1's HANDLE value for it.
    {
      std::atomic<uint32_t> published{0};
      std::atomic<bool> stop{false};
      std::atomic<uint32_t> bad{0}, oddRc{0}, clears{0};
      std::thread b([&] {
        while (!stop.load(std::memory_order_relaxed)) {
          const uint32_t rc = call("NtClearEvent", published.load(std::memory_order_relaxed));
          if (rc != 0 && rc != 0xC0000008u) oddRc.fetch_add(1);
          clears.fetch_add(1, std::memory_order_relaxed);
        }
      });
      auto createAt = [](uint32_t va, uint32_t type, uint32_t initial) {
        K::writeGuestBE32(va, 0);
        call("NtCreateEvent", va, 0, type, initial);
        return K::readGuestU32BE(va);
      };
      for (int i = 0; i < 20000; ++i) {
        const uint32_t h1 = createAt(kOut + 0x20, 0, 1);
        published.store(h1, std::memory_order_relaxed);
        call("NtClose", h1);
        const uint32_t h2 = createAt(kOut + 0x28, 0, 1);
        if (poll(h2) != 0) bad.fetch_add(1);
        call("NtClose", h2);
      }
      stop.store(true);
      b.join();
      std::printf("       E8: 20000 rounds, %u clears raced, %u fresh events found reset, %u other results\n",
                  clears.load(), bad.load(), oddRc.load());
      check(bad.load() == 0 && oddRc.load() == 0 && clears.load() > 0,
            "E8 concurrent close / create / clear: no fresh event reset through a recycled HANDLE");
    }
    call("NtClose", hm);
    call("NtClose", ha);
    call("NtClose", hu);
  }
#endif

  // ── C: ExGetXConfigSetting - xboxkrnl_xconfig.cc:21-58, XConfig::SetDefaults xconfig.cc:96-122 ──────────
  {
    auto get = [](uint32_t cat, uint32_t set, uint32_t buf, uint32_t size, uint32_t req) {
      return call("ExGetXConfigSetting", cat, set, buf, size, req);
    };
    fill(kBuf, 0xCC, 16);
    fill(kOut, 0xEE, 4);
    uint32_t st = get(2, 2, kBuf, 16, kOut);
    std::printf("       C1: st=%08X av=%08X req=%u next2=%02X%02X\n", st, K::readGuestU32BE(kBuf), rd16(kOut),
                host(kOut)[2], host(kOut)[3]);
    check(st == 0 && K::readGuestU32BE(kBuf) == 0x00400100u && rd16(kOut) == 4 && host(kOut)[2] == 0xEE &&
              allBytes(kBuf + 4, 0xCC, 12),
          "C1 SECURED_AV_REGION (2,2): NTSC-M 0x00400100, 4 bytes only, *required = u16 4");
    fill(kBuf, 0xCC, 4);
    st = get(3, 0x0E, kBuf, 4, kOut);
    check(st == 0 && host(kBuf)[0] == 103 && host(kBuf)[1] == 0xCC && rd16(kOut) == 1,
          "C2 USER_COUNTRY (3,0x0E): one byte 103, *required = 1");
    fill(kBuf, 0xCC, 4);
    K::writeGuestBE32(kOut, 0xEEEEEEEEu);
    st = get(3, 0x0A, kBuf, 2, kOut);
    check(st == 0xC0000023u && allBytes(kBuf, 0xCC, 4) && rd16(kOut) == 0,
          "C3 USER_VIDEO_FLAGS with a 2-byte buffer: BUFFER_TOO_SMALL, nothing copied, *required = 0");
    fill(kBuf, 0xCC, 64);
    bool tz = get(3, 0x01, kBuf, 4, 0) == 0 && K::readGuestU32BE(kBuf) == 0;
    tz = tz && get(3, 0x02, kBuf + 4, 4, 0) == 0 && std::memcmp(host(kBuf + 4), "GMT", 4) == 0;
    tz = tz && get(3, 0x03, kBuf + 8, 4, 0) == 0 && std::memcmp(host(kBuf + 8), "BST", 4) == 0;
    tz = tz && get(3, 0x04, kBuf + 12, 4, 0) == 0 && K::readGuestU32BE(kBuf + 12) == 0x0A050002u;
    tz = tz && get(3, 0x05, kBuf + 16, 4, 0) == 0 && K::readGuestU32BE(kBuf + 16) == 0x03050001u;
    tz = tz && get(3, 0x07, kBuf + 20, 4, 0) == 0 && K::readGuestU32BE(kBuf + 20) == 0xFFFFFFC4u;
    check(tz, "C4 time zone (3,1..7): Xenia's GMT London - bias 0, GMT / BST, 0A050002 / 03050001, DST bias -60");
    K::writeGuestBE32(kOut, 0xEEEEEEEEu);
    const uint32_t st5 = get(3, 0x09, 0, 4, kOut);
    std::printf("       C5a: st=%08X required=%04X next2=%02X%02X\n", st5, rd16(kOut), host(kOut)[2], host(kOut)[3]);
    check(st5 == 0xC00000F1u && rd16(kOut) == 0 && host(kOut)[2] == 0xEE && host(kOut)[3] == 0xEE,
          "C5a NULL buffer with a size: STATUS_INVALID_PARAMETER_3, and *required = WORD 0 (only 2 bytes written)");
    st = get(3, 0x09, 0, 0, kOut);
    check(st == 0 && rd16(kOut) == 4, "C5b NULL buffer, size 0: success, *required = 4 (size query)");
    fill(kBuf, 0xCC, 8);
    K::writeGuestBE32(kOut, 0xEEEEEEEEu);
    st = get(3, 0x99, kBuf, 8, kOut);
    check(st == 0 && allBytes(kBuf, 0xCC, 8) && rd16(kOut) == 0,
          "C6 unknown setting (3,0x99): success, nothing copied, *required = 0");
    fill(kBuf, 0xCC, 8);
    st = get(3, 0x09, kBuf, 4, 0);
    const uint32_t lang = K::readGuestU32BE(kBuf);
    st |= get(3, 0x0A, kBuf, 4, 0);
    const uint32_t vflags = K::readGuestU32BE(kBuf);
    st |= get(3, 0x0C, kBuf, 4, 0);
    const uint32_t retail = K::readGuestU32BE(kBuf);
    std::printf("       C7: language=%u video_flags=%08X retail=%08X\n", lang, vflags, retail);
    check(st == 0 && lang == 1 && vflags == 0 && retail == 0x40,
          "C7 language 1 (English), video flags 0 (RatioNormal), retail flags 0x40 (DashboardInitialized)");
    if (const char* f = std::getenv("SR_XCONFIG_ORACLE_FILE")) {
      std::ifstream in(f, std::ios::binary);
      std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      uint32_t n = 0;
      const uint8_t* img = K::importFixesXConfigImageForTest(&n);
      size_t firstDiff = n;
      for (size_t i = 0; i < n && i < file.size(); ++i)
        if (file[i] != img[i]) { firstDiff = i; break; }
      std::printf("       C8: file %zu bytes, image %u bytes, first difference at %zd\n", file.size(), n,
                  firstDiff == n ? -1 : (std::ptrdiff_t)firstDiff);
      check(file.size() == n && firstDiff == n, "C8 the XConfigData image equals the oracle's xconfig.settings");
    }
  }

  // ── V: XamGetSystemVersion - xam_info.cc:359-367 ─────────────────────────────────────────────────────────
  check(call("XamGetSystemVersion") == 0, "V1 XamGetSystemVersion: 0 (Xenia's answer)");

  // ── M: XMP app 0xFA - xmp_app.cc:348-427, audio_media_player.h:87-135 ─────────────────────────────────
  {
    auto getController = [](uint32_t* controller, uint32_t* locked) {
      K::writeGuestBE32(kArgs, 2);                // client Game
      K::writeGuestBE32(kArgs + 4, kOut);         // *controller
      K::writeGuestBE32(kArgs + 8, kOut + 4);     // *locked
      K::writeGuestBE32(kOut, 0xCCCCCCCCu);
      K::writeGuestBE32(kOut + 4, 0xCCCCCCCCu);
      const uint32_t st = call("XMsgInProcessCall", 0xFA, 0x0007001Bu, kArgs, 0);
      *controller = K::readGuestU32BE(kOut);
      *locked = K::readGuestU32BE(kOut + 4);
      return st;
    };
    auto setController = [](uint32_t client, uint32_t request, uint32_t locked) {
      K::writeGuestBE32(kArgs + 0x20, client);
      K::writeGuestBE32(kArgs + 0x24, request);
      K::writeGuestBE32(kArgs + 0x28, locked);
      return call("XMsgStartIORequestEx", 0xFA, 0x0007001Au, 0, kArgs + 0x20, 12);
    };
    uint32_t c = 0, l = 0;
    uint32_t st = getController(&c, &l);
    std::printf("       M1: st=%08X controller=%08X locked=%08X\n", st, c, l);
    check(st == 0 && c == 0 && l == 0, "M1 boot state: controller Game (0), not locked (title in control)");
    setController(2, 0, 1);            // XMPOverrideBackgroundMusic: (Game, Game, locked)
    st = getController(&c, &l);
    check(st == 0 && c == 0 && l == 1, "M2 after Set(Game, Game, locked=1): controller 0, locked 1 (override)");
    setController(2, 4, 0);            // XMPRestoreBackgroundMusic: (Game, Restore, 0)
    st = getController(&c, &l);
    check(st == 0 && c == 0 && l == 0, "M3 after Set(Game, Restore, 0): controller Game, locked 0");
    setController(2, 1, 0);            // controller User
    st = getController(&c, &l);
    check(st == 0 && c == 1 && l == 1, "M4 after Set(Game, User, 0): controller User (1), locked 1");
    setController(2, 4, 0);
    // M6 (Astra P2 5): one handler behind both transports - Set through XMsgInProcessCall (buffer r5), Get
    // through XMsgStartIORequestEx (buffer r6).
    K::writeGuestBE32(kArgs + 0x20, 2);
    K::writeGuestBE32(kArgs + 0x24, 1);    // controller User
    K::writeGuestBE32(kArgs + 0x28, 0);
    const uint32_t stSet = call("XMsgInProcessCall", 0xFA, 0x0007001Au, kArgs + 0x20, 12);
    K::writeGuestBE32(kArgs, 2);
    K::writeGuestBE32(kArgs + 4, kOut);
    K::writeGuestBE32(kArgs + 8, kOut + 4);
    K::writeGuestBE32(kOut, 0xCCCCCCCCu);
    K::writeGuestBE32(kOut + 4, 0xCCCCCCCCu);
    const uint32_t stGet = call("XMsgStartIORequestEx", 0xFA, 0x0007001Bu, 0, kArgs, 12);
    std::printf("       M6: set(in-process) %08X get(io-request) %08X controller=%08X locked=%08X\n", stSet, stGet,
                K::readGuestU32BE(kOut), K::readGuestU32BE(kOut + 4));
    check(stSet == 0 && stGet == 0 && K::readGuestU32BE(kOut) == 1 && K::readGuestU32BE(kOut + 4) == 1,
          "M6 Set via XMsgInProcessCall and Get via XMsgStartIORequestEx reach the same XMP handler");
    setController(2, 4, 0);
#ifdef _WIN32
    // M7 (Astra P2 4): an overlapped request completes at once - result / length / extended error / context,
    // its event set - and the call answers X_ERROR_IO_PENDING.
    {
      K::writeGuestBE32(kOut + 0x30, 0);
      call("NtCreateEvent", kOut + 0x30, 0, 0, 0);   // manual reset, unsignalled
      const uint32_t he = K::readGuestU32BE(kOut + 0x30);
      constexpr uint32_t kOvl = kArgs + 0x40;
      fill(kOvl, 0xCC, 0x1C);
      K::writeGuestBE32(kOvl + 0x0C, he);
      K::writeGuestBE32(kOvl + 0x10, 0);
      K::writeGuestBE32(kArgs + 0x20, 2);
      K::writeGuestBE32(kArgs + 0x24, 0);
      K::writeGuestBE32(kArgs + 0x28, 1);
      const uint32_t st7 = call("XMsgStartIORequestEx", 0xFA, 0x0007001Au, kOvl, kArgs + 0x20, 12);
      K::writeGuestBE32(kTimeout, 0);
      K::writeGuestBE32(kTimeout + 4, 0);
      const uint32_t w7 = call("NtWaitForSingleObjectEx", he, 1, 0, kTimeout);
      std::printf("       M7: rc=%u ovl result=%08X length=%08X ext=%08X ctx=%08X event poll=%08X\n", st7,
                  K::readGuestU32BE(kOvl), K::readGuestU32BE(kOvl + 4), K::readGuestU32BE(kOvl + 0x18),
                  K::readGuestU32BE(kOvl + 8), w7);
      check(st7 == 997 && K::readGuestU32BE(kOvl) == 0 && K::readGuestU32BE(kOvl + 4) == 0 &&
                K::readGuestU32BE(kOvl + 0x18) == 0 && K::readGuestU32BE(kOvl + 8) != 0xCCCCCCCCu && w7 == 0,
            "M7 overlapped Set: X_ERROR_IO_PENDING, the XOVERLAPPED completed (0 / 0 / 0, context) and its event set");
      call("NtClose", he);
      setController(2, 4, 0);
    }
#endif
    // M5: the boot calls registerStubs() twice (runHarness, runGuestHarness). A second registration must leave the
    // XMP pair answering once (no chaining to itself: that would recurse until the stack overflows).
    K::registerStubs();
    setController(2, 0, 1);
    st = getController(&c, &l);
    check(st == 0 && c == 0 && l == 1, "M5 after a second registerStubs(): Set / Get still answer (no self-chaining)");
    setController(2, 4, 0);
  }

  // ── D: RtlNtStatusToDosError (SR_NTSTATUS_DOSERR, xam_content.cpp) - xboxkrnl_error.cc:968-1008 ────────
  {
    const uint32_t a = call("RtlNtStatusToDosError", 0xC0000034u), b = call("RtlNtStatusToDosError", 0x80000006u);
    std::printf("       D1: 0xC0000034 -> %u, 0x80000006 -> %u\n", a, b);
    check(a == 2 && b == 0x12, "D1 OBJECT_NAME_NOT_FOUND -> 2, NO_MORE_FILES -> 0x12 (Xenia's table)");
  }

  // ── batch 2 T: RtlTimeFieldsToTime(fields r3, *time r4) - xboxkrnl_rtl.cc:724-749 ───────────────────────
  {
    auto fields = [](uint32_t va, unsigned y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned sec,
                     unsigned ms) {
      const unsigned v[8] = {y, mo, d, h, mi, sec, ms, 6};
      for (int i = 0; i < 8; ++i) {
        host(va + 2 * i)[0] = uint8_t(v[i] >> 8);
        host(va + 2 * i)[1] = uint8_t(v[i]);
      }
    };
    auto ref = [](unsigned y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned sec, unsigned ms) {
      SYSTEMTIME st{};
      st.wYear = WORD(y); st.wMonth = WORD(mo); st.wDay = WORD(d); st.wHour = WORD(h);
      st.wMinute = WORD(mi); st.wSecond = WORD(sec); st.wMilliseconds = WORD(ms);
      FILETIME ft{};
      SystemTimeToFileTime(&st, &ft);
      return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    fill(kOut, 0xCC, 8);
    fields(kBuf, 1970, 1, 1, 0, 0, 0, 0);
    uint32_t st = call("RtlTimeFieldsToTime", kBuf, kOut);
    std::printf("       T1: rc=%u time=%016llX\n", st, (unsigned long long)rd64(kOut));
    check(st == 1 && rd64(kOut) == 0x019DB1DED53E8000ull, "T1 1970-01-01: TRUE, FILETIME 116444736000000000 (BE 64-bit)");
    fill(kOut, 0xCC, 8);
    fields(kBuf, 2026, 10, 25, 1, 0, 0, 0);   // a DST cutover the London default asks for
    st = call("RtlTimeFieldsToTime", kBuf, kOut);
    check(st == 1 && rd64(kOut) == ref(2026, 10, 25, 1, 0, 0, 0),
          "T2 2026-10-25 01:00: TRUE and the FILETIME Windows' SystemTimeToFileTime gives");
    fill(kOut, 0xCC, 8);
    fields(kBuf, 2026, 2, 29, 0, 0, 0, 0);
    const uint32_t bad1 = call("RtlTimeFieldsToTime", kBuf, kOut);
    fields(kBuf, 2026, 13, 1, 0, 0, 0, 0);
    const uint32_t bad2 = call("RtlTimeFieldsToTime", kBuf, kOut);
    fields(kBuf, 1600, 12, 31, 0, 0, 0, 0);
    const uint32_t bad3 = call("RtlTimeFieldsToTime", kBuf, kOut);
    fields(kBuf, 2026, 1, 1, 24, 0, 0, 0);
    const uint32_t bad4 = call("RtlTimeFieldsToTime", kBuf, kOut);
    check(bad1 == 0 && bad2 == 0 && bad3 == 0 && bad4 == 0 && allBytes(kOut, 0xCC, 8),
          "T3 2026-02-29, month 13, year 1600, hour 24: FALSE, nothing written");
    fields(kBuf, 2024, 2, 29, 23, 59, 59, 999);
    st = call("RtlTimeFieldsToTime", kBuf, kOut);
    check(st == 1 && rd64(kOut) == ref(2024, 2, 29, 23, 59, 59, 999),
          "T4 2024-02-29 23:59:59.999 (a leap day): TRUE, the Windows FILETIME");
  }

  // ── batch 2 U: RtlMultiByteToUnicodeN / RtlUnicodeToMultiByteN - xboxkrnl_rtl.cc:350-395 ─────────────────
  {
    fill(kBuf, 0xCC, 32);
    std::memcpy(host(kBuf + 64), "ABCDEFG", 7);
    K::writeGuestBE32(kOut, 0xEEEEEEEEu);
    uint32_t st = call("RtlMultiByteToUnicodeN", kBuf, 10, kOut, kBuf + 64, 7);
    const bool wide = rd16(kBuf) == 'A' && rd16(kBuf + 8) == 'E' && host(kBuf + 10)[0] == 0xCC;
    std::printf("       U1: rc=%u written=%u wide=%d\n", st, K::readGuestU32BE(kOut), wide ? 1 : 0);
    check(st == 0 && wide && K::readGuestU32BE(kOut) == 10,
          "U1 MultiByteToUnicodeN(dest 10 bytes, \"ABCDEFG\"): 5 BE code units A..E, *written = 10");
    const uint16_t src[4] = {'A', 'B', 0x0100, 'C'};
    for (int i = 0; i < 4; ++i) { host(kBuf + 96 + 2 * i)[0] = uint8_t(src[i] >> 8); host(kBuf + 96 + 2 * i)[1] = uint8_t(src[i]); }
    fill(kBuf + 128, 0xCC, 8);
    K::writeGuestBE32(kOut, 0xEEEEEEEEu);
    st = call("RtlUnicodeToMultiByteN", kBuf + 128, 4, kOut, kBuf + 96, 8);
    std::printf("       U2: rc=%u written=%u bytes=%.4s\n", st, K::readGuestU32BE(kOut), (const char*)host(kBuf + 128));
    check(st == 0 && std::memcmp(host(kBuf + 128), "AB?C", 4) == 0 && host(kBuf + 132)[0] == 0xCC &&
              K::readGuestU32BE(kOut) == 4,
          "U2 UnicodeToMultiByteN(dest 4, 4 code units): \"AB?C\" (>= 256 -> '?'), *written = 4");
  }

  // ── batch 2 N: notifications - kernel_state.cc:1037-1067, xnotifylistener.cc:38-51, xam_notify.cc:56-94 ──────
  {
    auto next = [](uint32_t h, uint32_t* id, uint32_t* param) {
      K::writeGuestBE32(kOut, 0xEEEEEEEEu);
      K::writeGuestBE32(kOut + 4, 0xEEEEEEEEu);
      const uint32_t got = call("XNotifyGetNext", h, 0, kOut, kOut + 4);
      *id = K::readGuestU32BE(kOut);
      *param = K::readGuestU32BE(kOut + 4);
      return got;
    };
    const uint32_t hAll = call("XamNotifyCreateListener", 0x2F, 1);   // System | Live | Friends | Custom | XMP
    const uint32_t expect[6][2] = {{9, 0}, {0xA, 1}, {0xB, 0}, {0x02000001u, 0x001510F1u}, {0x02000003u, 0},
                                   {0x0A000003u, 1}};
    bool primed = true;
    for (const auto& e : expect) {
      uint32_t id = 0, param = 0;
      const uint32_t got = next(hAll, &id, &param);
      std::printf("       N1: got=%u id=%08X param=%08X\n", got, id, param);
      primed = primed && got == 1 && id == e[0] && param == e[1];
    }
    uint32_t id = 1, param = 1;
    const uint32_t empty = next(hAll, &id, &param);
    check(primed && empty == 0 && id == 0 && param == 0,
          "N1 the first System / Live / XMP listener is primed with Xenia's 6 startup notifications, then empty "
          "(and an empty fetch zeroes *id / *param)");
    const uint32_t hSys = call("XamNotifyCreateListener", 0x01, 1);
    check(next(hSys, &id, &param) == 0, "N2 a second System listener gets no startup notifications");
    K::writeGuestBE32(kOut + 4, 0xEEEEEEEEu);
    const uint32_t noId = call("XNotifyGetNext", hSys, 0, 0, kOut + 4);
    check(noId == 0 && K::readGuestU32BE(kOut + 4) == 0, "N3 no id pointer: 0, and *param = 0");

    // Astra batch-2 P2 (2026-10-20): filtering, filtered dequeue, the listener's wait handle, close, concurrent
    // delivery, the XMP controller broadcast and priming from the current state. Each case starts from a fresh
    // queue (the seam drops every listener and the first-listener flags).
    auto fetch = [](uint32_t h, uint32_t filter, uint32_t* id, uint32_t* param) {
      K::writeGuestBE32(kOut, 0xEEEEEEEEu);
      K::writeGuestBE32(kOut + 4, 0xEEEEEEEEu);
      const uint32_t got = call("XNotifyGetNext", h, filter, kOut, kOut + 4);
      *id = K::readGuestU32BE(kOut);
      *param = K::readGuestU32BE(kOut + 4);
      return got;
    };
    auto drain = [&](uint32_t h) { uint32_t i, p, n = 0; while (fetch(h, 0, &i, &p)) ++n; return n; };
    K::xnotifyResetForTest();
    const uint32_t hP = call("XamNotifyCreateListener", 0x01, 1);   // primes System (3)
    drain(hP);
    K::xnotifyBroadcastXenia(0x0A000003u, 1);   // XMP area (mask index 5): not in 0x01
    K::xnotifyBroadcastXenia(0x0002000Bu, 7);   // System, version 2 > max_version 1
    K::xnotifyBroadcastXenia(0x0000000Bu, 8);   // System, version 0: kept
    uint32_t fid = 0, fparam = 0;
    const uint32_t g1 = fetch(hP, 0, &fid, &fparam);
    const uint32_t g2 = fetch(hP, 0, &id, &param);
    check(g1 == 1 && fid == 0x0Bu && fparam == 8 && g2 == 0,
          "N4 a listener keeps only its areas (0x01: no XMP 0x0A000003) and versions <= max_version (no 0x0002000B)");
    K::xnotifyBroadcastXenia(0x0Bu, 1);
    K::xnotifyBroadcastXenia(0x0Au, 2);
    const uint32_t g3 = fetch(hP, 0x0Au, &fid, &fparam);
    const uint32_t g4 = fetch(hP, 0, &id, &param);
    check(g3 == 1 && fid == 0x0Au && fparam == 2 && g4 == 1 && id == 0x0Bu && param == 1 && drain(hP) == 0,
          "N5 a filtered XNotifyGetNext takes the first matching id and leaves the others queued in order");
#ifdef _WIN32
    K::writeGuestBE32(kTimeout, 0);
    K::writeGuestBE32(kTimeout + 4, 0);
    auto pollL = [](uint32_t h) { return call("NtWaitForSingleObjectEx", h, 1, 0, kTimeout); };
    const uint32_t wEmpty = pollL(hP);
    K::xnotifyBroadcastXenia(0x0Bu, 3);
    K::xnotifyBroadcastXenia(0x0Bu, 4);
    const uint32_t wQueued = pollL(hP);
    fetch(hP, 0, &id, &param);
    const uint32_t wOne = pollL(hP);
    fetch(hP, 0, &id, &param);
    const uint32_t wDrained = pollL(hP);
    std::printf("       N6: empty %08X queued %08X one-left %08X drained %08X\n", wEmpty, wQueued, wOne, wDrained);
    check(wEmpty == 0x102u && wQueued == 0 && wOne == 0 && wDrained == 0x102u,
          "N6 the listener handle is waitable: signalled while a notification is queued, reset after the last one");
    const uint32_t hC = call("XamNotifyCreateListener", 0x01, 1);
    K::xnotifyBroadcastXenia(0x0Bu, 5);
    const uint32_t wBefore = pollL(hC);   // signalled: (0x0B, 5) is queued
    call("NtClose", hC);
    K::xnotifyBroadcastXenia(0x0Bu, 6);   // after the close: must not reach (or crash on) the closed listener
    const uint32_t gClosed = fetch(hC, 0, &id, &param);
    const uint32_t wClosed = pollL(hC);
    std::printf("       N7: wait before close %08X, get after close %u, wait after close %08X\n", wBefore, gClosed,
                wClosed);
    // (a closed listener handle names nothing: STATUS_INVALID_HANDLE, as Xenia's lookup failure - Astra r2)
    check(wBefore == 0 && gClosed == 0 && wClosed == 0xC0000008u,
          "N7 NtClose ends the listener: XNotifyGetNext finds nothing, the handle no longer waits on its queue");
#endif
    drain(hP);
    std::atomic<uint32_t> taken{0};
    std::atomic<bool> stop{false};
    std::thread consumer([&] {
      K::setGuestMemory(g_base, kGuestSize);   // a side thread binds guest memory before any guest access
      uint32_t i, p;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while ((!stop.load() || taken.load() < 4000) && std::chrono::steady_clock::now() < deadline)
        if (fetch(hP, 0, &i, &p)) taken.fetch_add(1);
    });
    std::thread pa([] { for (int i = 0; i < 2000; ++i) K::xnotifyBroadcastXenia(0x0Bu, 100); });
    std::thread pb([] { for (int i = 0; i < 2000; ++i) K::xnotifyBroadcastXenia(0x0Au, 200); });
    pa.join();
    pb.join();
    stop.store(true);
    consumer.join();
    const uint32_t left = drain(hP);
    std::printf("       N8: taken %u, left %u\n", taken.load(), left);
    check(taken.load() == 4000 && left == 0, "N8 4000 notifications from two producers, one consumer: each taken once");
    K::xnotifyResetForTest();
    const uint32_t hX = call("XamNotifyCreateListener", 0x20, 1);   // the first XMP listener: primed with 1
    const uint32_t gx0 = fetch(hX, 0, &fid, &fparam);
    const bool primedOne = gx0 == 1 && fid == 0x0A000003u && fparam == 1;
    auto setXmp = [](uint32_t client, uint32_t request, uint32_t locked) {
      K::writeGuestBE32(kArgs + 0x20, client);
      K::writeGuestBE32(kArgs + 0x24, request);
      K::writeGuestBE32(kArgs + 0x28, locked);
      return call("XMsgStartIORequestEx", 0xFA, 0x0007001Au, 0, kArgs + 0x20, 12);
    };
    setXmp(2, 0, 1);   // override: the title is not in control
    const uint32_t gx1 = fetch(hX, 0, &fid, &fparam);
    const bool lockedZero = gx1 == 1 && fid == 0x0A000003u && fparam == 0;
    setXmp(2, 4, 0);   // restore
    const uint32_t gx2 = fetch(hX, 0, &fid, &fparam);
    check(primedOne && lockedZero && gx2 == 1 && fid == 0x0A000003u && fparam == 1,
          "N9 XMPSetPlaybackController broadcasts 0x0A000003 with IsTitleInPlaybackControl (override 0, restore 1)");
    K::xnotifyResetForTest();
    setXmp(2, 0, 1);                           // an override BEFORE the first XMP listener
    K::xnotifyBroadcastXenia(0x09u, 1);        // a system UI is up (no listener yet: only the state changes)
    const uint32_t hS = call("XamNotifyCreateListener", 0x21, 1);   // System | XMP
    uint32_t ui = 0xFF, xmpState = 0xFF;
    for (int i = 0; i < 4 && fetch(hS, 0, &fid, &fparam); ++i) {
      if (fid == 0x09u) ui = fparam;
      if (fid == 0x0A000003u) xmpState = fparam;
    }
    std::printf("       N10: primed XN_SYS_UI=%u XMP controller=%u\n", ui, xmpState);
    check(ui == 1 && xmpState == 0,
          "N10 priming reads the current state: UI active 1 (a blade is up), title in control 0 (the override)");
    setXmp(2, 4, 0);

    // Astra merge review of 0492b836 (2026-10-20): close during a wait, concurrent XMP setters, a failed event.
#ifdef _WIN32
    // N11 (Astra reviews of 0492b836 / 170216cc): Xenia keeps a closed listener in KernelState::notify_listeners_ and
    // keeps broadcasting to it; NtClose drops the handle only. A wait that already holds the listener's event (the
    // test hook fires after NtWaitForSingleObjectEx's lookup, before it waits) is woken by a broadcast after the
    // close. The close answers 0; the closed handle then finds nothing and no longer waits.
    K::xnotifyResetForTest();
    const uint32_t hW = call("XamNotifyCreateListener", 0x01, 1);
    drain(hW);
    static std::atomic<uint32_t> s_acquired{0};
    s_acquired.store(0);
    K::ntWaitAcquiredHookForTest([](uint32_t h) { s_acquired.store(h); });
    constexpr uint32_t kTimeout2s = kArgs + 0xC0;   // LARGE_INTEGER -2 s (relative, 100 ns units)
    K::writeGuestBE32(kTimeout2s, 0xFFFFFFFFu);
    K::writeGuestBE32(kTimeout2s + 4, 0xFECED300u);
    std::atomic<uint32_t> wAcross{0xFFFFFFFFu};
    std::atomic<long long> wMs{-1};
    std::thread waiter([&] {
      K::setGuestMemory(g_base, kGuestSize);
      const auto t0 = std::chrono::steady_clock::now();
      wAcross.store(call("NtWaitForSingleObjectEx", hW, 1, 0, kTimeout2s));
      wMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    });
    const auto dl11 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (s_acquired.load() != hW && std::chrono::steady_clock::now() < dl11) std::this_thread::yield();
    const bool acquired = s_acquired.load() == hW;
    K::ntWaitAcquiredHookForTest(nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));   // the waiter is now blocked in its kernel wait
    const uint32_t c11 = call("NtClose", hW);
    K::xnotifyBroadcastXenia(0x0Bu, 9);   // reaches the still-registered listener: its event wakes the wait
    waiter.join();
    const uint32_t g11 = fetch(hW, 0, &id, &param);
    const uint32_t w11 = pollL(hW);
    std::printf("       N11: acquired %d, close %08X, wait across the close %08X after %lld ms, get after %u, wait "
                "after %08X\n", acquired ? 1 : 0, c11, wAcross.load(), wMs.load(), g11, w11);
    check(acquired && c11 == 0 && wAcross.load() == 0 && wMs.load() >= 40 && wMs.load() < 1500 && g11 == 0 &&
              w11 == 0xC0000008u,
          "N11 NtClose during a wait (Xenia): the listener stays registered, a broadcast after the close wakes the "
          "wait that holds its event; the closed handle finds nothing, and a new wait on it is STATUS_INVALID_HANDLE");
    // N14: two threads close one listener handle at once, 200 times. Exactly one close answers 0 and the other
    // STATUS_INVALID_HANDLE, and the listener's event stays a live object (a second close never reaches the
    // table's CloseHandle).
    K::xnotifyResetForTest();
    int bad14 = 0;
    for (int i = 0; i < 200; ++i) {
      const uint32_t h14 = call("XamNotifyCreateListener", 0x01, 1);
      const HANDLE ev14 = static_cast<HANDLE>(K::xnotifyListenerEventForTest(h14));
      std::atomic<int> go{0};
      std::atomic<uint32_t> ra{1}, rb{1};
      auto closer = [&](std::atomic<uint32_t>* out) {
        K::setGuestMemory(g_base, kGuestSize);
        go.fetch_add(1);
        while (go.load() < 2) {
        }
        out->store(call("NtClose", h14));
      };
      std::thread ta(closer, &ra), tb(closer, &rb);
      ta.join();
      tb.join();
      const bool oneEach = (ra.load() == 0 && rb.load() == 0xC0000008u) || (ra.load() == 0xC0000008u && rb.load() == 0);
      const bool alive = ev14 != nullptr && WaitForSingleObject(ev14, 0) != WAIT_FAILED;
      if (!oneEach || !alive) ++bad14;
    }
    std::printf("       N14: 200 double closes, bad %d\n", bad14);
    check(bad14 == 0,
          "N14 two concurrent closes of one listener handle: one 0, one STATUS_INVALID_HANDLE, the event stays live");
#endif
    // N12: two threads set the XMP controller in lockstep, 2000 rounds (one override, one restore per round), with a
    // test-only spin of up to 200 us between each state change and its publication. Each round must enqueue exactly
    // two notifications and the last one must be the current state (publishing after the XMP lock loses that).
    K::xnotifyResetForTest();
    const uint32_t hX2 = call("XamNotifyCreateListener", 0x20, 1);
    drain(hX2);
    std::atomic<int> gen{0}, doneN{0};
    std::atomic<bool> quit{false};
    auto setter = [&](uint32_t buf, uint32_t request, uint32_t locked) {
      K::setGuestMemory(g_base, kGuestSize);
      int seen = 0;
      for (;;) {
        int g;
        while ((g = gen.load()) == seen && !quit.load()) std::this_thread::yield();
        if (quit.load()) return;
        seen = g;
        K::writeGuestBE32(buf, 2);
        K::writeGuestBE32(buf + 4, request);
        K::writeGuestBE32(buf + 8, locked);
        call("XMsgStartIORequestEx", 0xFA, 0x0007001Au, 0, buf, 12);
        doneN.fetch_add(1);
      }
    };
    std::thread sa(setter, kArgs + 0x60, 0u, 1u);   // override: not in control (0)
    std::thread sb(setter, kArgs + 0x80, 4u, 0u);   // restore: in control (1)
    int stale = 0, miscount = 0;
    K::xmpPublishSpinForTest(200);   // up to 200 us between each change and its publication
    for (int r = 1; r <= 2000; ++r) {
      doneN.store(0);
      gen.store(r);
      while (doneN.load() < 2) std::this_thread::yield();
      uint32_t last = 0xFFu, n = 0, i, pr;
      while (fetch(hX2, 0, &i, &pr)) {
        last = pr;
        ++n;
      }
      if (n != 2) ++miscount;
      if (last != (K::xmpTitleInPlaybackControl() ? 1u : 0u)) ++stale;
    }
    quit.store(true);
    sa.join();
    sb.join();
    K::xmpPublishSpinForTest(0);
    std::printf("       N12: 2000 lockstep rounds: stale last notification %d, rounds without exactly two %d\n", stale,
                miscount);
    check(stale == 0 && miscount == 0,
          "N12 concurrent XMPSetPlaybackController calls publish in state order: the last notification is the current "
          "state every round");
    setXmp(2, 4, 0);
    // N13: a listener whose wait event cannot be made is not created: 0, nothing registered, the priming flags kept
    // for the next System listener.
    K::xnotifyResetForTest();
    K::xnotifyFailNextEventForTest();
    const uint32_t hFail = call("XamNotifyCreateListener", 0x01, 1);
    const uint32_t hNext = call("XamNotifyCreateListener", 0x01, 1);
    const uint32_t primedNext = drain(hNext);
    std::printf("       N13: failed create -> %08X, next listener %08X primed %u\n", hFail, hNext, primedNext);
    check(hFail == 0 && hNext != 0 && primedNext == 3,
          "N13 no wait event -> no listener: 0 back, nothing registered, the next System listener is still primed");
    K::xnotifyResetForTest();
  }

  // ── batch 2 S: ExCreateThread's XAPI startup - xthread.cc (startup(start, ctx)) ────────────────────────────
  {
    uint32_t r3 = 0, r4 = 0;
    const uint32_t e1 = K::exThreadEntryPlanForTest(0x828A8C08u, 0x82566760u, 0x00001234u, &r3, &r4);
    std::printf("       S1: entry=%08X r3=%08X r4=%08X\n", e1, r3, r4);
    const bool viaStartup = e1 == 0x828A8C08u && r3 == 0x82566760u && r4 == 0x00001234u;
    const uint32_t e2 = K::exThreadEntryPlanForTest(0, 0x82566760u, 0x00001234u, &r3, &r4);
    const bool noStartup = e2 == 0x82566760u && r3 == 0x00001234u && r4 == 0;
    K::setThreadStartR4(0x55u);
    const uint32_t t1 = K::takeThreadStartR4(), t2 = K::takeThreadStartR4();
    check(viaStartup && noStartup && t1 == 0x55u && t2 == 0,
          "S1 r6 set: entry = XapiThreadStartup(r3 = start, r4 = context); r6 = 0: start(context); the r4 handoff "
          "is taken once");
    // Astra batch-2 P3: r6 == r7 and r7 = 0 still take the startup with (r7, r8), as Xenia does
    const uint32_t e3 = K::exThreadEntryPlanForTest(0x828A8C08u, 0x828A8C08u, 0x00001234u, &r3, &r4);
    const bool same = e3 == 0x828A8C08u && r3 == 0x828A8C08u && r4 == 0x00001234u;
    const uint32_t e4 = K::exThreadEntryPlanForTest(0x828A8C08u, 0, 0x00001234u, &r3, &r4);
    check(same && e4 == 0x828A8C08u && r3 == 0 && r4 == 0x00001234u,
          "S2 the startup is taken whenever r6 != 0 (r6 == r7, r7 = 0 included) with r3 = r7, r4 = r8");
  }
#ifdef _WIN32
  // ── S3 (Astra batch-2 P2): the REAL ExCreateThread -> guestThreadFunc -> runner wiring, with a recording runner ─
  {
    static std::atomic<uint32_t> s_entry{0}, s_r3{0}, s_r4{0};
    static HANDLE s_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    uint8_t* prevBase = nullptr;
    size_t prevSize = 0;
    const K::ExCreateThreadRunnerFn prev = K::getExCreateThreadRunner(&prevBase, &prevSize);
    K::registerExCreateThreadRunner(
        [](uint32_t startVA, uint32_t startParam, uint32_t, uint8_t*, size_t) {
          s_entry = startVA;
          s_r3 = startParam;
          s_r4 = K::takeThreadStartR4();
          SetEvent(s_done);
        },
        g_base, kGuestSize);
    K::writeGuestBE32(0x90100000u + 0x54u, 0);   // the 9th argument (creation flags) at [r1 + 0x54]: run at once
    K::writeGuestBE32(kOut, 0);
    call("ExCreateThread", kOut, 0, 0, 0x828A8C08u, 0x82400000u, 0x00005678u);
    const DWORD w = WaitForSingleObject(s_done, 5000);
    const uint32_t th = K::readGuestU32BE(kOut);
    std::printf("       S3: wait=%lu entry=%08X r3=%08X r4=%08X handle=%u\n", (unsigned long)w, s_entry.load(),
                s_r3.load(), s_r4.load(), th);
    check(w == WAIT_OBJECT_0 && s_entry == 0x828A8C08u && s_r3 == 0x82400000u && s_r4 == 0x00005678u,
          "S3 ExCreateThread(r6 = XapiThreadStartup): the thread's first call is startup(r3 = start, r4 = context)");
    if (th) call("NtClose", th);
    K::registerExCreateThreadRunner(prev, prevBase, prevSize);
  }
#endif

  // ── batch 3 A / L: NtQueryFullAttributesFile and NtCreateFile on an ISOLATED guest file system ────────────────
  // (Astra batch-3 review: host temp paths are read as DVD paths when %TEMP% is on D:.) The test builds its own
  // content root (a disc tree + one mod) under the temp folder and points resolveGuestPath / the overlay at it, so
  // every query is a guest path (d:\, game:\) that cannot reach the repo's disc or the host's drives.
  // A: Xenia 0bd090dbe xboxkrnl_io.cc:474-513 with the disc device's entries (disc_image_device.cc:118-161,
  //    vfs/entry.cc:21-30): exact attributes, sizes, allocation and times.
  // L: NtCreateFile without the deleted optional-data alias (xboxkrnl_io.cc:47-111).
#ifdef _WIN32
  {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path R = fs::temp_directory_path(ec) / ("sr_vfs_" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(R, ec);
    const fs::path disc = R / "Superman Returns (USA)";
    const fs::path mod = R / "mods" / "m";
    auto put = [&](const fs::path& f, const std::string& bytes) {
      fs::create_directories(f.parent_path(), ec);
      std::ofstream(f, std::ios::binary) << bytes;
    };
    put(disc / "DATA" / "probe.AST", std::string(1000, 'x'));
    put(disc / "DATA" / "objtiv.AST", "AST0");
    put(disc / "DATA" / "shadow.AST", std::string(3000, 'd'));
    put(disc / "DATA" / "sub" / "s.bin", "abc");
    fs::create_directories(disc / "DATA" / "empty", ec);
    put(mod / "DATA" / "modonly.AST", "MODONLY!");
    put(mod / "DATA" / "shadow.AST", std::string(700, 'm'));
    put(mod / "DATA" / "moddir" / "x.txt", "x");
    const auto rootU8raw = R.generic_u8string();
    const std::string rootU8(reinterpret_cast<const char*>(rootU8raw.data()), rootU8raw.size());
    K::setContentRootForTest(rootU8 + "/");
    K::modOverlayResetForTest(rootU8, {"m"});

    constexpr uint32_t kOa = kBuf, kAnsi = kBuf + 0x10, kStr = kBuf + 0x20, kInfo = kOut;
    auto setOa = [&](const std::string& guestPath) {
      fill(kBuf, 0, 256);
      K::writeGuestBE32(kOa + 0, 0);       // RootDirectory
      K::writeGuestBE32(kOa + 4, kAnsi);   // ObjectName (ANSI_STRING*)
      K::writeGuestBE32(kOa + 8, 0x40);    // OBJ_CASE_INSENSITIVE
      const uint32_t n = uint32_t(guestPath.size());
      K::writeGuestBE32(kAnsi, (n << 16) | (n + 1));
      K::writeGuestBE32(kAnsi + 4, kStr);
      std::memcpy(host(kStr), guestPath.data(), n);
    };
    auto attrs = [&](const std::string& guestPath) {
      fill(kInfo, 0xEE, 56);
      setOa(guestPath);
      return call("NtQueryFullAttributesFile", kOa, kInfo);
    };
    struct Info { uint64_t c, a, w, ch, alloc, eof; uint32_t attr; };
    auto info = [&]() {
      return Info{rd64(kInfo + 0), rd64(kInfo + 8), rd64(kInfo + 16), rd64(kInfo + 24), rd64(kInfo + 32),
                  rd64(kInfo + 40), K::readGuestU32BE(kInfo + 48)};
    };
    auto hostTimes = [](const fs::path& q, uint64_t& c, uint64_t& a, uint64_t& w) {
      WIN32_FILE_ATTRIBUTE_DATA d{};
      if (!GetFileAttributesExW(q.c_str(), GetFileExInfoStandard, &d)) return false;
      auto ft = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
      c = ft(d.ftCreationTime); a = ft(d.ftLastAccessTime); w = ft(d.ftLastWriteTime);
      return true;
    };
    auto show = [&](const char* tag, uint32_t st) {
      const Info v = info();
      std::printf("       %s: %08X attr %08X eof %llu alloc %llu c %016llX a %016llX w %016llX ch %016llX\n", tag, st,
                  v.attr, (unsigned long long)v.eof, (unsigned long long)v.alloc, (unsigned long long)v.c,
                  (unsigned long long)v.a, (unsigned long long)v.w, (unsigned long long)v.ch);
      return v;
    };
    constexpr uint64_t k1970 = 116444736000000000ull;   // Xenia: 10000 * 11644473600000
    constexpr uint32_t kIsoFile = 0x80u;   // NORMAL: the retail ISO's 14 file entries (lane B b2/census/iso_attrs.json)
    auto disc1970 = [&](const Info& v) { return v.c == k1970 && v.a == k1970 && v.w == k1970 && v.ch == k1970; };

    const uint32_t a1 = attrs("d:\\DATA\\no_such_file.AST");
    check(a1 == 0xC000000Fu && allBytes(kInfo, 0xEE, 56),
          "A1 a disc path nothing resolves: STATUS_NO_SUCH_FILE, the info untouched (Xenia's failed ResolvePath)");
    const uint32_t a1b = attrs("C:\\Windows");
    check(a1b == 0xC000000Fu && allBytes(kInfo, 0xEE, 56),
          "A1b a host drive path (no Xenia device): NO_SUCH_FILE, although the host folder exists");
    const uint32_t a2 = attrs("d:\\DATA\\probe.AST");
    Info v = show("A2", a2);
    check(a2 == 0 && v.attr == (kIsoFile | 0x01u) && v.eof == 1000 && v.alloc == 1024 && disc1970(v),
          "A2 a disc file: SUCCESS, the ISO file attribute | READONLY, end 1000, allocation 1024, all times 1970");
    const uint32_t a2b = attrs("game:\\DATA\\probe.AST\\");
    v = info();
    check(a2b == 0 && v.attr == (kIsoFile | 0x01u) && v.eof == 1000 && disc1970(v),
          "A2b game:\\ and a trailing separator (Xenia's split_path drops empty parts): the same file");
    const uint32_t a3 = attrs("d:\\DATA");
    v = show("A3", a3);
    check(a3 == 0 && v.attr == 0x11u && v.eof == 2048 && v.alloc == 2048 && disc1970(v),
          "A3 a disc folder: SUCCESS, DIRECTORY | READONLY, its directory table length 2048 (one sector), 1970");
    const uint32_t a3b = attrs("d:\\DATA\\");
    v = info();
    check(a3b == 0 && v.attr == 0x11u && v.eof == 2048 && disc1970(v),
          "A3b the folder with a trailing separator: the same answer");
    const uint32_t a3c = attrs("d:\\");
    v = show("A3c", a3c);
    check(a3c == 0 && v.attr == 0x10u && v.eof == 0 && v.alloc == 0 && v.c == 0 && v.a == 0 && v.w == 0 && v.ch == 0,
          "A3c the disc root: the device's root entry - DIRECTORY only, size, allocation and times 0");
    const uint32_t a3d = attrs("d:\\DATA\\empty");
    v = info();
    check(a3d == 0 && v.attr == 0x11u && v.eof == 0 && v.alloc == 0 && disc1970(v),
          "A3d an empty disc folder: directory table length 0");
    const uint32_t a4 = attrs("ShaderDumpxe:\\CompareBackEnds");
    check(a4 == 0xC000000Fu,
          "A4 the D3DX compiler's probe ShaderDumpxe:\\CompareBackEnds: NO_SUCH_FILE (its compare-back-ends debug mode "
          "stays off, as on Xenia)");
    const uint32_t a5 = attrs(std::string("game:\\caf") + char(0xE9) + ".bin");
    check(a5 == 0xC0000033u, "A5 a path that is not ASCII: STATUS_OBJECT_NAME_INVALID (Xenia's IsValidPath)");
    const uint32_t a5b = attrs("game:\\DATA\\a|b.bin");
    check(a5b == 0xC0000033u, "A5b a path with a character IsValidPath refuses ('|'): STATUS_OBJECT_NAME_INVALID");
    const uint32_t a6 = attrs("game:\\DATA\\sr_attrs_no_such_file.bin");
    check(a6 == 0xC000000Fu, "A6 a game: path no disc folder holds: NO_SUCH_FILE");
    uint64_t hc = 0, ha = 0, hw = 0;
    const uint32_t a7 = attrs("d:\\DATA\\modonly.AST");
    v = show("A7", a7);
    const bool t7 = hostTimes(mod / "DATA" / "modonly.AST", hc, ha, hw);
    check(a7 == 0 && t7 && v.attr == (kIsoFile | 0x01u) && v.eof == 8 && v.alloc == 512 && v.c == hc && v.a == ha &&
              v.w == hw && v.ch == hw && hc != 0,
          "A7 a file only the mod adds: SUCCESS, the disc file attributes, end 8, allocation 512, the mod file's times");
    const uint32_t a7b = attrs("d:\\DATA\\shadow.AST");
    v = info();
    const bool t7b = hostTimes(mod / "DATA" / "shadow.AST", hc, ha, hw);
    check(a7b == 0 && t7b && v.attr == (kIsoFile | 0x01u) && v.eof == 700 && v.alloc == 1024 && v.w == hw,
          "A7b a disc file the mod shadows: the mod's size (700, not 3000) and times");
    const uint32_t a8 = attrs("d:\\DATA\\moddir");
    v = show("A8", a8);
    const bool t8 = hostTimes(mod / "DATA" / "moddir", hc, ha, hw);
    check(a8 == 0 && t8 && v.attr == 0x11u && v.eof == 2048 && v.alloc == 2048 && v.c == hc && v.a == ha &&
              v.w == hw && hc != 0,
          "A8 a folder only the mod provides: DIRECTORY | READONLY, table length 2048, the backing folder's times "
          "(not zero)");
    std::printf("       A: %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X\n", a1, a1b, a2,
                a2b, a3, a3b, a3c, a3d, a4, a5, a5b, a6, a7, a7b, a8);

    // L: the title's own spelling (d:\DATA\..., ENGINE_MAP 5: "d:\DATA\objtiv1.AST").
    constexpr uint32_t kIosb = kOut + 0x20;
    auto open = [&](const std::string& guestPath, uint32_t* handle) {
      fill(kOut, 0xEE, 0x40);
      setOa(guestPath);
      const uint32_t st = call("NtCreateFile", kOut, 0x80100080u, kOa, kIosb, 0, 0);
      *handle = K::readGuestU32BE(kOut);
      return st;
    };
    K::modOverlayResetForTest(rootU8, {});   // no mod: the alias question is the disc's alone
    uint32_t h1 = 0, h2 = 0, h3 = 0;
    const uint32_t l1 = open("d:\\DATA\\objtiv1.AST", &h1);
    check(l1 == 0xC0000034u && h1 == 0xEEEEEEEEu,
          "L1 d:\\DATA\\objtiv1.AST (not on the retail disc): OBJECT_NAME_NOT_FOUND, no handle - no alias to "
          "objtiv.AST");
    const uint32_t l2 = open("d:\\DATA\\objtiv.AST", &h2);
    check(l2 == 0 && h2 != 0xEEEEEEEEu && h2 != 0, "L2 d:\\DATA\\objtiv.AST (on the disc): SUCCESS with a handle");
    if (l2 == 0 && h2 != 0xEEEEEEEEu) call("NtClose", h2);
    if (l1 == 0 && h1 != 0xEEEEEEEEu) call("NtClose", h1);
    const uint32_t l3 = open("d:\\DATA\\sr_no_such.AST", &h3);
    check(l3 == 0xC0000034u, "L3 a DATA file with no alias and no disc copy: OBJECT_NAME_NOT_FOUND");
    std::printf("       L: %08X %08X %08X (handles %08X %08X)\n", l1, l2, l3, h1, h2);

    K::setContentRootForTest("");
    K::modOverlayResetForTest(rootU8, {});
    fs::remove_all(R, ec);
  }
#endif

  std::printf("[IMPORT_FIXES_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
