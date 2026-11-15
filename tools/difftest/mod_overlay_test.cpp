// iter-463 MOD FRAMEWORK: kernel file-layer asset overlay (kernel/mod_overlay.{hpp,cpp},
// docs/MODS.md).
//
// Drives the REAL recompiled guest wrappers (CreateFileA sub_824687F8, ReadFile
// sub_824685C8) and the NtOpenFile / NtQueryDirectoryFile / NtQueryInformationFile
// exports through the export registry, so the kernel wiring is tested, not just
// the overlay's bodies. Mods live under a fresh temp content root; the disc tree
// is the repo's own "Superman Returns (USA)/" (read, never written).
//
// Run: build/difftest.exe --mod-overlay-test   (exit = failed checks)
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/export_registry.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/stubs.hpp"
#include "kernel/file_io.hpp"
#include "kernel/mod_overlay.hpp"
#include "kernel/mod_bridge.hpp"
#include "kernel/xam_profile.hpp"
#include "kernel/devtools.hpp"
#include "kernel/devtools_menu.hpp"
#include "kernel/host_pages.hpp"
#include "kernel/mod_menu.hpp"
#include "kernel/mod_library.hpp"   // iter-466: the PC SETTINGS hand-off (M19l)
#include "kernel/host_automation.hpp"   // lane E L-b: M19n (the hand-off follows port features)
#include "kernel/mod_manager.hpp"

PPC_EXTERN_FUNC(sub_824687F8);   // CreateFileA
PPC_EXTERN_FUNC(sub_824685C8);   // ReadFile
PPC_EXTERN_FUNC(sub_826D39A0);   // Apt FSCommand handler (strong def: kernel/mod_bridge.cpp)
PPC_EXTERN_FUNC(sub_828A7D50);   // FindFirstFileA
PPC_EXTERN_FUNC(sub_828A7C10);   // FindNextFileA

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace K = sr::kernel;
namespace fs = std::filesystem;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}

constexpr uint32_t kStack = 0x90100000u;
constexpr uint32_t kPcr = 0x90200000u, kPcrThread = 0x90201000u;
constexpr uint32_t kPath = 0x90300100u;
constexpr uint32_t kOa = 0x90300800u, kAnsi = 0x90300810u, kHandle = 0x90300820u, kIosb = 0x90300830u;
constexpr uint32_t kInfo = 0x90300900u;
constexpr uint32_t kRead = 0x90320000u, kDone = 0x9035000Cu, kDirBuf = 0x90360000u;
constexpr uint32_t kFindData = 0x90361000u;

void putStr(uint32_t va, const std::string& s) { std::memcpy(K::guestVAToHost(va), s.c_str(), s.size() + 1); }
int g_quits = 0;
void countQuit() { ++g_quits; }
void zero(uint32_t va, size_t n) { std::memset(K::guestVAToHost(va), 0, n); }

struct Ctx {
  PPCContext c{};
  Ctx() {
    c.r1.u32 = kStack;
    c.r13.u32 = kPcr;
    c.r14.u32 = 0x829C0000u;
    K::setCurrentGuestContext(&c, offsetof(PPCContext, lr), offsetof(PPCContext, ctr),
                              offsetof(PPCContext, r3), offsetof(PPCContext, r1),
                              offsetof(PPCContext, r31));
  }
  ~Ctx() { K::setCurrentGuestContext(nullptr, 0); }
};
using GuestFn = void (*)(PPCContext&, uint8_t*);
uint32_t guest(GuestFn fn, uint32_t a3, uint32_t a4 = 0, uint32_t a5 = 0, uint32_t a6 = 0,
               uint32_t a7 = 0, uint32_t a8 = 0, uint32_t a9 = 0) {
  Ctx x;
  x.c.r3.u32 = a3; x.c.r4.u32 = a4; x.c.r5.u32 = a5; x.c.r6.u32 = a6;
  x.c.r7.u32 = a7; x.c.r8.u32 = a8; x.c.r9.u32 = a9; x.c.r10.u32 = 0;
  fn(x.c, K::getGuestBase());
  return x.c.r3.u32;
}
uint32_t exportCall(const char* name, uint32_t a3, uint32_t a4 = 0, uint32_t a5 = 0, uint32_t a6 = 0,
                    uint32_t a7 = 0, uint32_t a8 = 0, uint32_t a9 = 0, uint32_t a10 = 0) {
  Ctx x;
  K::ExportRegistry::setCurrentExtraArgs(a9, a10);
  return K::ExportRegistry::instance().getExport(name)(a3, a4, a5, a6, a7, a8);
}

// The guest's CreateFileA(OPEN_EXISTING, GENERIC_READ) + ReadFile of up to n bytes.
std::string guestRead(const std::string& path, uint32_t n = 64) {
  putStr(kPath, path);
  const uint32_t h = guest(sub_824687F8, kPath, 0x80000000u, 1, 0, 3, 0x80);
  if (h == 0xFFFFFFFFu) return "<open failed>";
  zero(kRead, n);
  K::writeGuestBE32(kDone, 0);
  const uint32_t ok = guest(sub_824685C8, h, kRead, n, kDone, 0);
  const uint32_t got = K::readGuestU32BE(kDone);
  exportCall("NtClose", h);
  if (!ok) return "<read failed>";
  return std::string(reinterpret_cast<const char*>(K::guestVAToHost(kRead)), got);
}

// NtOpenFile(path) -> handle (0 on failure).
uint32_t ntOpen(const std::string& path) {
  putStr(kPath, path);
  K::writeGuestBE32(kAnsi, (uint32_t(path.size()) << 16) | uint32_t(path.size() + 1));
  K::writeGuestBE32(kAnsi + 4, kPath);
  K::writeGuestBE32(kOa + 0, 0);
  K::writeGuestBE32(kOa + 4, kAnsi);
  K::writeGuestBE32(kOa + 8, 0x40);
  K::writeGuestBE32(kHandle, 0);
  const uint32_t st = exportCall("NtOpenFile", kHandle, 0x80000000u, kOa, kIosb, 1, 0);
  return st == 0 ? K::readGuestU32BE(kHandle) : 0;
}

struct Listed { std::string name; uint64_t size; uint32_t attrs; };
// iter-464: list a folder the way the GAME does (the owner's mod-system research, not published,
// finding 1): FindFirstFileA (sub_828A7D50 -> sub_8246A640) passes a 328-byte buffer and the
// mask after the last '\' ("*.*" is sent as an EMPTY mask), FindNextFileA (sub_828A7C10 ->
// sub_8246A7A8) passes 328 bytes, no mask, RestartScan 0; both convert ONLY THE FIRST record of
// each call (sub_828A8640 never reads NextEntryOffset). The iter-463 helper read 4 KB and
// followed NextEntryOffset, so M4f passed while the game could not see the file.
constexpr uint32_t kMaskAnsi = 0x90300880u, kMaskBuf = 0x903008A0u;

struct QueryResult {
  uint32_t status = 0;
  uint32_t iosbStatus = 0;
  uint32_t information = 0;
  uint32_t nextEntryOffset = 0;
  std::string name;
};

QueryResult ntQueryOne(uint32_t h, const char* mask, bool restart, uint32_t length = 328) {
  uint32_t maskVA = 0;
  if (mask) {
    const size_t n = std::strlen(mask);
    putStr(kMaskBuf, mask);
    K::writeGuestBE32(kMaskAnsi, (uint32_t(n) << 16) | uint32_t(n));
    K::writeGuestBE32(kMaskAnsi + 4, kMaskBuf);
    maskVA = kMaskAnsi;
  }
  zero(kDirBuf, 328);
  K::writeGuestBE32(kIosb, 0x11223344u);
  K::writeGuestBE32(kIosb + 4, 0x55667788u);
  K::writeGuestBE32(kStack + 0x54, restart ? 1u : 0u);
  QueryResult q;
  q.status = exportCall("NtQueryDirectoryFile", h, 0, 0, 0, kIosb, kDirBuf, length, maskVA);
  q.iosbStatus = K::readGuestU32BE(kIosb);
  q.information = K::readGuestU32BE(kIosb + 4);
  if (q.status == 0) {
    q.nextEntryOffset = K::readGuestU32BE(kDirBuf);
    const uint32_t nl = K::readGuestU32BE(kDirBuf + 60);
    q.name.assign(reinterpret_cast<const char*>(K::guestVAToHost(kDirBuf + 64)), std::min(nl, 264u));
  }
  return q;
}

std::vector<Listed> ntList(uint32_t h, const std::string& mask = "") {
  std::vector<Listed> out;
  putStr(kMaskBuf, mask);
  K::writeGuestBE32(kMaskAnsi, (uint32_t(mask.size()) << 16) | uint32_t(mask.size()));
  K::writeGuestBE32(kMaskAnsi + 4, kMaskBuf);
  K::writeGuestBE32(kStack + 0x54, 0);  // RestartScan (the 9th argument's stack slot)
  for (int guard = 0; guard < 1024; ++guard) {
    zero(kDirBuf, 328);
    const uint32_t st = exportCall("NtQueryDirectoryFile", h, 0, 0, 0, kIosb, kDirBuf, 328,
                                   guard == 0 ? kMaskAnsi : 0);
    if (st & 0x80000000u) break;  // NO_MORE_FILES / NO_SUCH_FILE / errors
    const uint32_t va = kDirBuf;   // the first record only
    const uint64_t size = (uint64_t(K::readGuestU32BE(va + 40)) << 32) | K::readGuestU32BE(va + 44);
    const uint32_t attrs = K::readGuestU32BE(va + 56);
    const uint32_t nl = K::readGuestU32BE(va + 60);
    out.push_back({std::string(reinterpret_cast<const char*>(K::guestVAToHost(va + 64)), nl > 255 ? 255 : nl),
                   size, attrs});
  }
  return out;
}

// Exercise the actual retail FindFirstFileA / FindNextFileA wrappers. This is
// the wiring that exposed the old multi-record host implementation: the guest
// converts only record zero from each 328-byte NtQueryDirectoryFile call.
std::vector<Listed> guestFindList(const std::string& pattern) {
  std::vector<Listed> out;
  putStr(kPath, pattern);
  zero(kFindData, 328);
  const uint32_t h = guest(sub_828A7D50, kPath, kFindData);
  if (h == 0xFFFFFFFFu) return out;
  for (int guard = 0; guard < 1024; ++guard) {
    const uint64_t size = (uint64_t(K::readGuestU32BE(kFindData + 28)) << 32) |
                          K::readGuestU32BE(kFindData + 32);
    const uint32_t attrs = K::readGuestU32BE(kFindData);
    const char* name = reinterpret_cast<const char*>(K::guestVAToHost(kFindData + 44));
    size_t nl = 0;
    while (nl < 260 && name[nl]) ++nl;
    out.push_back({std::string(name, nl), size, attrs});
    zero(kFindData, 328);
    if (!guest(sub_828A7C10, h, kFindData)) break;
  }
  exportCall("NtClose", h);
  return out;
}
const Listed* find(const std::vector<Listed>& v, const std::string& n) {
  for (const auto& x : v)
    if (_stricmp(x.name.c_str(), n.c_str()) == 0) return &x;
  return nullptr;
}

void writeHost(const fs::path& p, const std::string& s) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  if (std::FILE* f = _wfopen(p.c_str(), L"wb")) {
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
  }
}
std::string readHostHead(const fs::path& p, size_t n) {
  std::string s(n, '\0');
  size_t got = 0;
  if (std::FILE* f = _wfopen(p.c_str(), L"rb")) { got = std::fread(s.data(), 1, n, f); std::fclose(f); }
  s.resize(got);
  return s;
}
}  // namespace

int runModOverlayTest() {
  // iter-465 modlib: these cases drive the retired Apt MODS pages, which SR_MOD_LIBRARY=0 keeps
  // reachable (the default opens the mod library overlay; --mod-library-test covers that).
  _putenv_s("SR_MOD_LIBRARY", "0");
  std::printf("[MOD_TEST] kernel file-layer asset overlay (mods/<name>/)\n");
  std::error_code ec;
  const fs::path root = fs::temp_directory_path(ec) / ("sr_mod_test_" + std::to_string(GetCurrentProcessId()));
  fs::remove_all(root, ec);
  const fs::path mods = root / "mods";
  writeHost(mods / "a" / "DATA" / "text.AST", "MODA");
  writeHost(mods / "b" / "DATA" / "TEXT.ast", "MODB-longer");
  writeHost(mods / "b" / "DATA" / "newfile.bin", "NEW");
  writeHost(mods / "a" / "DATA" / "_workinprogress" / "SCRPRESSSTART.ast", "X");
  writeHost(mods / "b" / "DATA" / "onlymod" / "f.txt", "F");

  K::registerStubs();
  zero(kPcr, 0x400);
  K::writeGuestBE32(kPcr + 256u, kPcrThread);
  K::ntQueryDirXeniaSetForTest(1);  // iter-464 SR_NTQDIR_XENIA (M4i checks the old path too)

  const std::string disc = K::contentRootDir() + "Superman Returns (USA)/";
  const std::string discText = readHostHead(disc + "DATA/text.AST", 64);
  std::printf("  disc = %s (text.AST head %zu bytes)\n", disc.c_str(), discText.size());
  check(!K::contentRootDir().empty() && discText.size() == 64, "M0 the disc tree is present (repo content root)");

  // ── M1: nothing enabled = the natural path ───────────────────────────────
  K::modOverlayResetForTest(root.generic_string(), {});
  check(!K::modOverlayActive(), "M1a no mod enabled -> overlay inactive");
  check(guestRead("d:\\DATA\\text.AST") == discText, "M1b CreateFileA+ReadFile d:\\DATA\\text.AST = the disc bytes");
  check(K::modOverlayResolveFile(disc + "DATA/text.AST").empty(), "M1c resolve returns \"\" when inactive");

  // ── M2: priority = list order ────────────────────────────────────────────
  K::modOverlayResetForTest(root.generic_string(), {"a", "b"});
  check(K::modOverlayActive() && K::modOverlayEnabled().size() == 2, "M2a {a,b} mounted");
  check(guestRead("d:\\DATA\\text.AST") == "MODA", "M2b a first: the guest reads a's text.AST");
  check(guestRead("d:\\DATA\\TEXT.AST") == "MODA", "M2c case-insensitive: d:\\DATA\\TEXT.AST -> a's file");
  check(guestRead("d:\\DATA\\ui.AST", 8) == readHostHead(disc + "DATA/ui.AST", 8), "M2d an unshadowed file still reads from the disc");
  check(guestRead("d:\\DATA\\newfile.bin") == "NEW", "M2e a mod-only file is added");
  K::modOverlayResetForTest(root.generic_string(), {"b", "a"});
  check(guestRead("d:\\DATA\\text.AST") == "MODB-longer", "M2f {b,a}: b wins");

  // ── M3: NtQueryInformationFile reports the shadowing file's size ─────────
  {
    const uint32_t h = ntOpen("d:\\DATA\\text.AST");
    zero(kInfo, 56);
    const uint32_t st = h ? exportCall("NtQueryInformationFile", h, kIosb, kInfo, 56, 0x22) : 1;
    const uint32_t eof = K::readGuestU32BE(kInfo + 44);
    std::printf("       M3: h=%u st=%08X eof=%u\n", h, st, eof);
    check(h && st == 0 && eof == 11u, "M3 NtQueryInformationFile(0x22) EndOfFile = 11 (b's file)");
    if (h) exportCall("NtClose", h);
  }

  // ── M4: NtQueryDirectoryFile lists the union ─────────────────────────────
  {
    const uint32_t h = ntOpen("d:\\DATA\\");
    const auto v = h ? ntList(h) : std::vector<Listed>{};
    if (h) exportCall("NtClose", h);
    const Listed* t = find(v, "text.AST");
    const Listed* n = find(v, "newfile.bin");
    const Listed* u = find(v, "ui.AST");
    const Listed* o = find(v, "onlymod");
    std::printf("       M4: %zu entries text=%llu new=%d ui=%llu onlymod=%d\n", v.size(),
                t ? (unsigned long long)t->size : 0ull, n ? 1 : 0, u ? (unsigned long long)u->size : 0ull, o ? 1 : 0);
    check(t && t->size == 11u && t->name == "text.AST", "M4a shadowed entry: disc name kept, b's size (11)");
    check(n && n->size == 3u && n->attrs == 0x80u, "M4b mod-only file appended");
    check(u && u->size == 89595420ull, "M4c unshadowed entry keeps the disc size");
    check(o && o->attrs == 0x10u, "M4d mod-only folder appended as a directory");
    size_t discN = 0;
    for (const auto& e : fs::directory_iterator(fs::path(disc) / "DATA", ec)) { (void)e; ++discN; }
    check(v.size() == discN + 2 && v[discN].name == "newfile.bin" && v[discN + 1].name == "onlymod",
          "M4e the disc entries first (disc order), then the mod-only names sorted");
  }
  {
    K::modOverlayResetForTest(root.generic_string(), {"a", "b"});
    const uint32_t h = ntOpen("d:\\DATA\\_workinprogress\\");
    const auto v = h ? ntList(h) : std::vector<Listed>{};
    if (h) exportCall("NtClose", h);
    check(find(v, "SCRPRESSSTART.ast") && find(v, "GAMECONFIG"), "M4f d:\\DATA\\_workinprogress lists disc + a's SCRPRESSSTART.ast");
    const uint32_t h2 = ntOpen("d:\\DATA\\onlymod\\");
    const auto v2 = h2 ? ntList(h2) : std::vector<Listed>{};
    if (h2) exportCall("NtClose", h2);
    check(h2 && v2.size() == 1 && v2[0].name == "f.txt", "M4g a folder only a mod has opens and lists its file");
    check(guestRead("d:\\DATA\\onlymod\\f.txt") == "F", "M4h ...and its file reads");
    // iter-464 SR_NTQDIR_XENIA: the masks the game's storage scans use, and the old path's
    // packing (the game converts only the first record of each call).
    const uint32_t h3 = ntOpen("d:\\DATA\\_workinprogress\\");
    const auto v3 = h3 ? ntList(h3, "__ASSET__*") : std::vector<Listed>{};
    if (h3) exportCall("NtClose", h3);
    check(v3.size() == 1 && v3[0].name == "__ASSET__GAMECONFIG", "M4i mask __ASSET__* -> only __ASSET__GAMECONFIG");
    const uint32_t h3b = ntOpen("d:\\DATA\\_workinprogress\\");
    const auto v3b = h3b ? ntList(h3b, "__ASSET__*.*") : std::vector<Listed>{};
    if (h3b) exportCall("NtClose", h3b);
    check(h3b && v3b.empty(),
          "M4i2 mask __ASSET__*.* needs a '.' after the prefix (Xenia's WildcardEngine): __ASSET__GAMECONFIG does not match");
    const uint32_t h4 = ntOpen("d:\\DATA\\_workinprogress\\");
    const auto v4 = h4 ? ntList(h4, "*.zzz") : std::vector<Listed>{};
    if (h4) exportCall("NtClose", h4);
    check(h4 && v4.empty(), "M4j a mask that matches nothing -> no entry (STATUS_NO_SUCH_FILE)");
    const uint32_t h6 = ntOpen("d:\\DATA\\");
    const auto v6 = h6 ? ntList(h6, "*.ast") : std::vector<Listed>{};
    if (h6) exportCall("NtClose", h6);
    size_t astN = 0;
    for (const auto& e : fs::directory_iterator(fs::path(disc) / "DATA", ec))
      if (_stricmp(e.path().extension().string().c_str(), ".ast") == 0) ++astN;
    check(h6 && v6.size() == astN + 0 && astN == 12, "M4l d:\\DATA with mask *.ast: all 12 retail .AST files, one per call");

    const auto guestAst = guestFindList("d:\\DATA\\*.ast");
    bool guestAstComplete = guestAst.size() == astN;
    for (const auto& e : fs::directory_iterator(fs::path(disc) / "DATA", ec)) {
      if (_stricmp(e.path().extension().string().c_str(), ".ast") == 0 &&
          !find(guestAst, e.path().filename().string())) {
        guestAstComplete = false;
      }
    }
    check(guestAstComplete,
          "M4m the real guest FindFirstFileA/FindNextFileA wrappers return all 12 retail .AST files");

    const uint32_t stateBase = K::ntQueryDirStateCountForTest();
    const uint32_t h7 = ntOpen("d:\\DATA\\");
    const QueryResult first = h7 ? ntQueryOne(h7, "*.ast", false) : QueryResult{};
    const QueryResult second = h7 ? ntQueryOne(h7, nullptr, false) : QueryResult{};
    const QueryResult restarted = h7 ? ntQueryOne(h7, nullptr, true) : QueryResult{};
    const QueryResult newRule = h7 ? ntQueryOne(h7, "ui.AST", false) : QueryResult{};
    check(h7 && first.status == 0 && second.status == 0 && first.name != second.name &&
              restarted.status == 0 && restarted.name == first.name && newRule.status == 0 &&
              _stricmp(newRule.name.c_str(), "ui.AST") == 0,
          "M4n cursor advances; RestartScan and a new mask both restart the directory query");
    check(first.nextEntryOffset == 0 && first.iosbStatus == 0 && first.information == 328,
          "M4o one record per call, NextEntryOffset 0, IOSB {SUCCESS, caller length}");
    if (h7) exportCall("NtClose", h7);
    check(K::ntQueryDirStateCountForTest() == stateBase,
          "M4p NtClose retires the per-handle directory cursor state");

    const uint32_t h8 = ntOpen("d:\\DATA\\");
    const QueryResult noMatch = h8 ? ntQueryOne(h8, "*.zzz", false) : QueryResult{};
    const QueryResult afterNoMatch = h8 ? ntQueryOne(h8, nullptr, false) : QueryResult{};
    check(h8 && noMatch.status == 0xC000000Fu && noMatch.iosbStatus == noMatch.status &&
              noMatch.information == 0 && afterNoMatch.status == 0x80000006u &&
              afterNoMatch.iosbStatus == afterNoMatch.status && afterNoMatch.information == 0,
          "M4q unmatched initial mask -> NO_SUCH_FILE; exhausted cursor -> NO_MORE_FILES");
    if (h8) exportCall("NtClose", h8);

    const uint32_t h9 = ntOpen("d:\\DATA\\");
    const QueryResult shortBuffer = h9 ? ntQueryOne(h9, nullptr, false, 71) : QueryResult{};
    const QueryResult invalidMask = h9 ? ntQueryOne(h9, "a*b", false) : QueryResult{};
    check(h9 && shortBuffer.status == 0xC0000004u && shortBuffer.iosbStatus == 0x11223344u &&
              shortBuffer.information == 0x55667788u && invalidMask.status == 0xC000000Du &&
              invalidMask.iosbStatus == 0x11223344u && invalidMask.information == 0x55667788u,
          "M4r Xenia early errors: short buffer / invalid mask do not touch IOSB");
    if (h9) exportCall("NtClose", h9);

    const QueryResult badHandle = ntQueryOne(0x7FFFFFFFu, nullptr, false);
    check(badHandle.status == 0xC000000Fu && badHandle.iosbStatus == badHandle.status &&
              badHandle.information == 0,
          "M4s Xenia invalid directory handle -> NO_SUCH_FILE with IOSB updated");

    K::ntQueryDirXeniaSetForTest(0);
    const uint32_t h5 = ntOpen("d:\\DATA\\_workinprogress\\");
    const auto v5 = h5 ? ntList(h5) : std::vector<Listed>{};
    if (h5) exportCall("NtClose", h5);
    K::ntQueryDirXeniaSetForTest(1);
    std::printf("       M4k: old listing, game-style reads: %zu of 3 entries seen\n", v5.size());
    check(v5.size() < 3 && !find(v5, "SCRPRESSSTART.ast"),
          "M4k SR_NTQDIR_XENIA=0 (old packing): the game-style reader misses SCRPRESSSTART.ast (the bug)");
  }

  // ── M5: safety and config ────────────────────────────────────────────────
  check(K::modOverlayResolveFile(disc + "DATA/../DATA/text.AST").empty(), "M5a '..' in a path never reaches a mod");
  check(K::modOverlayResolveFile("C:/elsewhere/DATA/text.AST").empty(), "M5b a path outside the disc tree never reaches a mod");
  K::modOverlayResetForTest(root.generic_string(), {"missing", "../a", "b"});
  check(K::modOverlayEnabled().size() == 1 && K::modOverlayEnabled()[0] == "b",
        "M5c a missing folder and a non-plain name are skipped, the rest mount");
  check(K::modEnabled("B") && !K::modEnabled("a"), "M5d modEnabled is case-insensitive and exact");
  {
    const auto l = K::modParseEnabledList("\xEF\xBB\xBF" "custom_frontend  # the menu\r\n\n# off\n  hd_pack\r\nCUSTOM_FRONTEND\n");
    check(l.size() == 2 && l[0] == "custom_frontend" && l[1] == "hd_pack",
          "M5e enabled.txt: BOM, comments, CRLF, blanks, duplicate dropped, order kept");
  }

  // ── M6: the boot decision (the user's switch is mods/enabled.txt) ────────
  {
    const std::string r = root.generic_string() + "/";
    writeHost(mods / "enabled.txt", "# my mods\r\nb\r\na  # second\r\n");
    std::string src;
    auto l = K::modComputeEnabledList(r, nullptr, nullptr, nullptr, &src);
    check(l == std::vector<std::string>{"b", "a"} && src == "mods/enabled.txt",
          "M6a no env: mods/enabled.txt is read (order kept)");
    l = K::modComputeEnabledList(r, nullptr, nullptr, "7", &src);
    check(l.empty(), "M6b automated roll (SR_INSTANCE_ID): enabled.txt ignored");
    l = K::modComputeEnabledList(r, "@enabled.txt", nullptr, "7", &src);
    check(l == std::vector<std::string>{"b", "a"} && src == "mods/enabled.txt (SR_MODS=@enabled.txt)",
          "M6h automated roll with SR_MODS=@enabled.txt: the player's file is read (the roll tests the user switch)");
    l = K::modComputeEnabledList(r, "a", nullptr, "7", &src);
    check(l == std::vector<std::string>{"a"} && src == "SR_MODS", "M6c SR_MODS replaces the file (rolls use it)");
    l = K::modComputeEnabledList(r, "0", nullptr, nullptr, &src);
    check(l.empty(), "M6d SR_MODS=0 = none");
    l = K::modComputeEnabledList(r, nullptr, "1", nullptr, &src);
    check(l == std::vector<std::string>{"custom_frontend", "b", "a"}, "M6e SR_CUSTOM_FRONTEND=1 puts custom_frontend first");
    writeHost(mods / "enabled.txt", "custom_frontend\nb\n");
    l = K::modComputeEnabledList(r, nullptr, "0", nullptr, &src);
    check(l == std::vector<std::string>{"b"}, "M6f SR_CUSTOM_FRONTEND=0 removes it from the file's list");
    fs::remove(mods / "enabled.txt", ec);
    l = K::modComputeEnabledList(r, nullptr, nullptr, nullptr, &src);
    check(l.empty() && src == "none", "M6g no file, no env = no mods");
    l = K::modComputeEnabledList(r, "@enabled.txt", nullptr, "7", &src);
    check(l.empty(), "M6i SR_MODS=@enabled.txt with no file = no mods");
  }

  // ── M7: the names the retail disc does not hold (sound1, objtiv1, fmvN) ──
  // step4/audit batch 3 (baked 2026-10-20): the host's optional-data alias is deleted. As on Xenia and the console,
  // those opens fail, and a mod's sound.AST answers sound.AST only. A mod that ships a REAL objtiv1.AST
  // (a mission mod) is served under that name (M7f).
  {
    const std::string fail = "<open failed>";
    writeHost(mods / "a" / "DATA" / "sound.AST", "SNDA");
    writeHost(mods / "a" / "DATA" / "fmv.AST", "FMVA");
    K::modOverlayResetForTest(root.generic_string(), {});
    // With a disc tree (M0), the disc's sound.AST must open, so the refusal below is not a broken fixture; without one
    // (the build worker) only the refusal is checked.
    const std::string discSound = readHostHead(disc + "DATA/sound.AST", 16);
    check((discSound.empty() || guestRead("d:\\DATA\\sound.AST", 16) == discSound) &&
              guestRead("d:\\DATA\\sound1.AST", 16) == fail,
          "M7a no mod: sound1.AST does not open (no alias, as on Xenia); the disc's sound.AST does");
    K::modOverlayResetForTest(root.generic_string(), {"a"});
    check(guestRead("d:\\DATA\\sound.AST") == "SNDA", "M7b the direct open of sound.AST reads the mod's copy");
    check(guestRead("d:\\DATA\\sound1.AST") == fail, "M7c no alias: sound1.AST does not open, even with a mod's sound.AST");
    check(guestRead("d:\\DATA\\fmv3.AST") == fail, "M7d no alias: fmv3.AST does not open (the mod ships fmv.AST only)");
    check(guestRead("d:\\DATA\\objtiv1.AST", 16) == fail, "M7e no alias: objtiv1.AST does not open");
    writeHost(mods / "a" / "DATA" / "objtiv1.AST", "OBJ1");
    K::modOverlayResetForTest(root.generic_string(), {"a"});
    check(guestRead("d:\\DATA\\objtiv1.AST") == "OBJ1",
          "M7f a mod's REAL objtiv1.AST (a mission mod's layout) is served under its own name");
    fs::remove(mods / "a" / "DATA" / "objtiv1.AST", ec);
  }

  // ── M8: difftest itself is isolated from the player's mods/enabled.txt ─────
  check(std::getenv("SR_MODS") != nullptr, "M8 difftest's entry point pins SR_MODS (0 unless the caller named mods)");

  // ── M9: the FSCommand bridge (kernel/mod_bridge.cpp) ──────────────────────
  {
    // The bridge is not tied to a mod name (review finding 2): an SR_* command from ANY
    // content - no mod, a renamed copy of custom_frontend - is handled on the host,
    // because the game's own body would null-deref on the registry miss.
    K::modBridgeSetQuitHandlerForTest(countQuit);
    g_quits = 0;
    K::modOverlayResetForTest(root.generic_string(), {});
    putStr(kPath, "SR_Log");
    putStr(kPath + 0x40, "no mod");
    uint32_t rc0 = guest(sub_826D39A0, kPath, kPath + 0x40);
    check(g_quits == 0 && rc0 == 0, "M9a no mod enabled: SR_Log is still handled on the host (no registry lookup)");
    fs::create_directories(mods / "my_menu", ec);
    K::modOverlayResetForTest(root.generic_string(), {"my_menu"});
    putStr(kPath, "SR_Quit");
    putStr(kPath + 0x40, "");
    guest(sub_826D39A0, kPath, kPath + 0x40);
    check(g_quits == 1, "M9b a mod under another name (my_menu): SR_Quit reaches the host");
    g_quits = 0;
    fs::create_directories(mods / "custom_frontend", ec);
    K::modOverlayResetForTest(root.generic_string(), {"custom_frontend"});
    check(K::modBridgeDispatch("GAME_Cmd", "") == K::ModBridgeResult::NotOurs &&
          K::modBridgeDispatch("sr_quit", "") == K::ModBridgeResult::NotOurs && g_quits == 0,
          "M9c non-SR_ commands (case-sensitive prefix) go to the game's own handler");
    // The real strong def, entered the way sub_82682DF8 enters it (r3 = cmd, r4 = args).
    putStr(kPath, "SR_Log");
    putStr(kPath + 0x40, "hello");
    guest(sub_826D39A0, kPath, kPath + 0x40);
    check(g_quits == 0, "M9d SR_Log through sub_826D39A0: handled on the host, no quit");
    putStr(kPath, "SR_Nope");
    const uint32_t rc = guest(sub_826D39A0, kPath, 0);
    check(g_quits == 0 && rc == 0, "M9e an unknown SR_ command (null args) is ignored, returns 0");
    putStr(kPath, "SR_Quit");
    putStr(kPath + 0x40, "");
    guest(sub_826D39A0, kPath, kPath + 0x40);
    check(g_quits == 1, "M9f SR_Quit through sub_826D39A0 requests exactly one quit");
    K::modBridgeSetQuitHandlerForTest(nullptr);
  }

  // ── M10: the universal options store (SR_SetSettings / SR_GetSettings) ─────────
  {
    const fs::path croot = root / "content";
    fs::create_directories(croot, ec);
#ifdef _WIN32
    _wputenv_s(L"SR_CONTENT_DIR", croot.wstring().c_str());
#endif
    const fs::path ini = croot / "settings" / "game_options.ini";
    check(K::modSettingsPath() == ini, "M10a the store lives at <content root>/settings/game_options.ini");
    int32_t reply = 7;
    check(K::modBridgeDispatch("SR_GetSettings", "", &reply) == K::ModBridgeResult::ReplyInt &&
              reply == K::kModSettingsNone,
          "M10b no file: SR_GetSettings replies -1 (no store)");
    // music 7, sfx 5, scheme 2, X 1, Y 0, auto-center 1, vibration 1
    const int32_t packed = 7 + 5 * 16 + 2 * 256 + 1024 + 0 + 4096 + 8192;
    const int32_t hostBits = int32_t(K::kModSettingsApplyToLoadedBit | K::kModSettingsVolumesKnownBit);
    K::modBridgeSetAudioMixerForTest(1);   // the game's audio mixer exists: volumes are real
    int32_t setReply = 0;
    const bool setIsReply = K::modBridgeDispatch("SR_SetSettings", "ScrAudioOpt FE " + std::to_string(packed),
                                                 &setReply) == K::ModBridgeResult::ReplyInt;
    const std::string txt = readHostHead(ini.generic_string(), 4096);
    check(txt.find("music_volume=7\n") != std::string::npos &&
              txt.find("sfx_volume=5\n") != std::string::npos &&
              txt.find("control_scheme=2\n") != std::string::npos &&
              txt.find("invert_x=1\n") != std::string::npos &&
              txt.find("invert_y=0\n") != std::string::npos &&
              txt.find("auto_center=1\n") != std::string::npos &&
              txt.find("vibration=1\n") != std::string::npos &&
              txt.find("apply_to_loaded_saves=1\n") != std::string::npos,
          "M10c SR_SetSettings '<tag> <packed>' writes every key (apply_to_loaded_saves defaults to 1)");
    reply = 0;
    K::modBridgeDispatch("SR_GetSettings", "", &reply);
    check(reply == (packed | hostBits) && setIsReply && setReply == reply,
          "M10d SR_GetSettings (and SR_SetSettings' own reply) = the stored value + bits 14/15");
    // An invalid capture gets NO reply, so the AS copy _global.SRSettings (written only by
    // the reply) keeps its last good value.
    int32_t badReply = 12345;
    const bool badNoReply =
        K::modBridgeDispatch("SR_SetSettings", "11", &badReply) == K::ModBridgeResult::Handled &&   // music 11
        K::modBridgeDispatch("SR_SetSettings", "ScrAudioOpt NaN", &badReply) == K::ModBridgeResult::Handled &&   // an undefined getter
        K::modBridgeDispatch("SR_SetSettings", "-5", &badReply) == K::ModBridgeResult::Handled;
    check(K::modSettingsLoadPacked() == (packed | hostBits) && badNoReply && badReply == 12345,
          "M10e invalid values (music 11, NaN, negative) are rejected with no reply and the store is kept");
    writeHost(ini, txt.substr(0, txt.find("\napply_to_loaded_saves=") + 1) + "apply_to_loaded_saves=0\n");
    K::modBridgeDispatch("SR_SetSettings", std::to_string(packed + 1));   // music 8
    const int32_t after = K::modSettingsLoadPacked();
    check(after == ((packed + 1) | int32_t(K::kModSettingsVolumesKnownBit)),
          "M10f a hand-set apply_to_loaded_saves=0 survives the next capture");
    // The file write fails (the temp name is taken by a directory: the same outcome as a
    // locked or read-only game_options.ini or a full disk): the store is unchanged, and the
    // reply is the value the file WOULD hold - the new capture with the file's
    // apply_to_loaded_saves=0 - so the session keeps both (iter-463 settings review 2).
    {
      const fs::path blocker = croot / "settings" / "game_options.ini.tmp";
      fs::create_directories(blocker, ec);
      int32_t failReply = 0;
      const bool failIsReply = K::modBridgeDispatch("SR_SetSettings", "ScrGameplayOpt FE " + std::to_string(packed + 2),
                                                    &failReply) == K::ModBridgeResult::ReplyInt;
      check(failIsReply && failReply == ((packed + 2) | int32_t(K::kModSettingsVolumesKnownBit)) &&
                K::modSettingsLoadPacked() == after,
            "M10k a failed store write: the file is kept, the reply = the capture + the file's apply_to_loaded_saves=0");
      fs::remove_all(blocker, ec);
    }
    // canonical == what the file holds after a successful write
    {
      int32_t canon = 0;
      const bool w = K::modSettingsSavePacked(packed + 1, true, &canon);
      check(w && canon == K::modSettingsLoadPacked(), "M10l modSettingsSavePacked's canonical value == the value read back");
    }
    // No audio mixer (the front end in this build): the getters return a cached 0, so the
    // volumes of a capture are not taken - the file's stay, or stay absent.
    K::modBridgeSetAudioMixerForTest(0);
    K::modBridgeDispatch("SR_SetSettings", "ScrAudioOpt FE " + std::to_string(0 + 0 * 16 + 1 * 256 + 8192));
    const int32_t kept = K::modSettingsLoadPacked();
    check((kept & 255) == 8 + 5 * 16 && ((kept >> 8) & 3) == 1 && (kept & int32_t(K::kModSettingsVolumesKnownBit)),
          "M10i no audio mixer: the capture's 0 volumes are ignored, the stored 8/5 kept, the scheme taken");
    fs::remove(ini, ec);
    K::modBridgeDispatch("SR_SetSettings", std::to_string(1 * 256 + 8192));
    const int32_t unk = K::modSettingsLoadPacked();
    const std::string t2 = readHostHead(ini.generic_string(), 4096);
    check(unk >= 0 && !(unk & int32_t(K::kModSettingsVolumesKnownBit)) && ((unk >> 8) & 3) == 1 &&
              t2.find("music_volume") == std::string::npos,
          "M10j no mixer and no earlier store: volumes are left unknown (no key, bit 15 clear)");
    K::modBridgeSetAudioMixerForTest(-1);
    writeHost(ini, "music_volume=3\nsfx_volume=3\n");
    check(K::modSettingsLoadPacked() == K::kModSettingsNone, "M10g an incomplete store reads as no store (-1)");
    // SR_GetSettings through the real strong def: no AptExecutor in difftest -> the reply is
    // logged as not delivered, nothing is called, the handler returns 0.
    putStr(kPath, "SR_GetSettings");
    putStr(kPath + 0x40, "");
    const uint32_t rc = guest(sub_826D39A0, kPath, kPath + 0x40);
    check(rc == 0, "M10h SR_GetSettings through sub_826D39A0 without an executor: no call, returns 0");
#ifdef _WIN32
    _wputenv_s(L"SR_CONTENT_DIR", L"");
#endif
  }

  // ── M11: XamUserReadProfileSettings (kernel/xam_profile.cpp, Xenia port) ──────
  {
    constexpr uint32_t kIds = 0x90310000u, kSize = 0x90310040u, kBuf = 0x90310100u,
                       kOvl = 0x90310400u;
    const uint32_t ids[4] = {0x10040023u, 0x10040024u, 0x10040002u, 0x10040003u};
    for (int i = 0; i < 4; ++i) K::writeGuestBE32(kIds + 4 * i, ids[i]);
    K::writeGuestBE32(kSize, 0);
    const uint32_t q = K::xamReadProfileSettings(0, 0, 0, 0, 4, kIds, kSize, 0, 0);
    check(q == 0x7A && K::readGuestU32BE(kSize) == 8 + 4 * 40,
          "M11a size query: X_ERROR_INSUFFICIENT_BUFFER and *pSize = 8 + 4*40 (the title's 4 ids)");
    zero(kBuf, 0x200);
    zero(kOvl, 0x20);
    K::writeGuestBE32(kOvl, 997);
    const uint32_t r = K::xamReadProfileSettings(0, 0, 0, 0, 4, kIds, kSize, kBuf, kOvl);
    const uint32_t cnt = K::readGuestU32BE(kBuf), ptr = K::readGuestU32BE(kBuf + 4);
    // what ProfileSettings_applyOptions_sub_822A45D0 reads: *(ptr + 72/112/152)
    const uint32_t move = K::readGuestU32BE(ptr + 72), inv = K::readGuestU32BE(ptr + 112),
                   vib = K::readGuestU32BE(ptr + 152);
    check(r == 997 && K::readGuestU32BE(kOvl) == 0 && cnt == 4 && ptr == kBuf + 8 && move == 0 &&
              inv == 0 && vib == 3,
          "M11b overlapped read: 997, ovl completed 0, 4 settings, vibration 3 (Xenia default), "
          "movement/Y inversion 0");
    check(K::readGuestU32BE(ptr + 0) == 1 && K::readGuestU32BE(ptr + 0x10) == ids[0] &&
              *K::guestVAToHost(ptr + 0x18) == 1 && K::readGuestU32BE(ptr + 0x08) == 0 &&
              K::readGuestU32BE(ptr + 0x0C) == 0xFFFFFFFFu,
          "M11c X_USER_PROFILE_SETTING layout: source DEFAULT, id @0x10, type INT32 @0x18, "
          "user_index over xuid -1 @0x08");
    K::writeGuestBE32(kIds + 4, 0x10040099u);   // not a known setting: dropped from the count
    zero(kBuf, 0x200);
    K::xamReadProfileSettings(0, 0, 0, 0, 4, kIds, kSize, kBuf, 0);
    check(K::readGuestU32BE(kBuf) == 3 && K::readGuestU32BE(kBuf + 8 + 2 * 40 + 0x20) == 3,
          "M11d an unknown id is dropped from the count (Xenia), later slots move up");
    K::writeGuestBE32(kIds + 4, ids[1]);
    check(K::xamReadProfileSettings(0, 1, 0, 0, 4, kIds, kSize, kBuf, 0) == 0x65B &&
              K::xamReadProfileSettings(0, 0, 0, 0, 0, kIds, kSize, kBuf, 0) == 0x57,
          "M11e user 1 (not signed in) -> X_ERROR_FUNCTION_FAILED; 0 settings -> invalid parameter");
  }

  // ── M12-M16: iter-464 fe lane (autosave setting, option folders, the save-slot tracker,
  // the Dev Tools menu host, XamUserGetName) ─────────────────────────────────────────────
  {
    const fs::path croot = root / "content464";
    fs::remove_all(croot, ec);
    fs::create_directories(croot / "settings", ec);
#ifdef _WIN32
    _wputenv_s(L"SR_CONTENT_DIR", croot.wstring().c_str());
#endif
    // M12: the mod options store
    check(K::modOptionsPath() == croot / "settings" / "mod_options.ini",
          "M12a the mod options live at <save root>/settings/mod_options.ini");
    check(K::modOptionGet("autosave_without_asking") == 1 && K::modOptionGet("devtools_notice_ack") == 0 &&
              K::modOptionGet("nope") == -1,
          "M12b no file: autosave_without_asking defaults to 1 (ON), devtools_notice_ack to 0, unknown -1");
    int32_t r = 0;
    check(K::modBridgeDispatch("SR_GetModOptions", "", &r) == K::ModBridgeResult::ReplyInt && (r & 3) == 1,
          "M12c SR_GetModOptions replies the packed options (bit 0 autosave = 1)");
    K::modBridgeDispatch("SR_SetModOption", "autosave_without_asking 0", &r);
    const std::string t = readHostHead(K::modOptionsPath(), 4096);
    check((r & 1) == 0 && K::modOptionGet("autosave_without_asking") == 0 &&
              t.find("autosave_without_asking=0\n") != std::string::npos &&
              t.find("devtools_notice_ack=0\n") != std::string::npos,
          "M12d SR_SetModOption 'autosave_without_asking 0' stores it (every key written) and replies bit 0 = 0");
    check(!K::modOptionSet("autosave_without_asking", 2) && !K::modOptionSet("nope", 1) &&
              K::modOptionGet("autosave_without_asking") == 0,
          "M12e a value other than 0/1 or an unknown key is rejected, the store is kept");
    writeHost(K::modOptionsPath(), "# hand edit\nautosave_without_asking = 1\r\ndevtools_notice_ack=7\n");
    check(K::modOptionGet("autosave_without_asking") == 1 && K::modOptionGet("devtools_notice_ack") == 0,
          "M12f hand edits: spaces / CRLF accepted, an out-of-range value falls back to the default");

    // M13: option folders (mods/<mod>/@<option>/) follow the option at boot
    writeHost(mods / "opt" / "DATA" / "x.bin", "MOD");
    writeHost(mods / "opt" / "@autosave_without_asking" / "DATA" / "x.bin", "OPTION");
    writeHost(mods / "opt" / "@no_such_option" / "DATA" / "y.bin", "NEVER");
    K::modOptionSet("autosave_without_asking", 1);
    K::modOverlayResetForTest(root.generic_string(), {"opt"});
    check(K::modOverlayResolveFile(disc + "DATA/x.bin").find("@autosave_without_asking") != std::string::npos &&
              K::modOptionMounted("autosave_without_asking"),
          "M13a option on: mods/opt/@autosave_without_asking/ is mounted and wins over the mod's own file");
    check(K::modOverlayResolveFile(disc + "DATA/y.bin").empty(), "M13b an unknown option's folder never mounts");
    {
      std::vector<K::ModDirEntry> v;
      K::modOverlayMergeDir(disc, v);
      bool at = false;
      for (const auto& e : v) at = at || (!e.name.empty() && e.name[0] == '@');
      check(!at, "M13c the disc root listing never shows an option folder");
    }
    check(guestRead("d:\\DATA\\x.bin") == "OPTION", "M13d the guest reads the option folder's file (CreateFileA+ReadFile)");
    K::modOptionSet("autosave_without_asking", 0);
    K::modOverlayResetForTest(root.generic_string(), {"opt"});
    check(K::modOverlayResolveFile(disc + "DATA/x.bin").find("@autosave") == std::string::npos &&
              guestRead("d:\\DATA\\x.bin") == "MOD",
          "M13e option off at boot: the folder is not mounted, the mod's own file answers");
    K::modOverlayResetForTest(root.generic_string(), {});

    // M14: the current-save-slot tracker (SR_SaveEvent; saveFlow+0x54 through the test seam)
    K::modSlotResetForTest();
    // an event gets no reply (its sites bind no reply method); a query answers the slot
    bool eventsSilent = true;
    auto ev = [&](const char* kind, int32_t sf) {
      K::modBridgeSetSaveFlowSlotForTest(sf);
      int32_t s = -9;
      if (std::string(kind) != "query")
        eventsSilent = eventsSilent && K::modBridgeDispatch("SR_SaveEvent", kind, &s) == K::ModBridgeResult::Handled;
      K::modBridgeDispatch("SR_SaveEvent", "query", &s);
      return s;
    };
    check(ev("query", 0) == -1, "M14a boot: no current slot");
    check(ev("load", 1) == 1 && ev("query", 0) == 1, "M14b a LOAD of slot 1 makes it current");
    check(ev("new", 1) == -1, "M14c NEW GAME: a new playthrough has no slot (even with a stale +0x54)");
    check(ev("save", 0) == 0, "M14d a save to an empty slot: +0x54 is the slot");
    ev("overwrite", 2);
    check(ev("save", 3) == 2, "M14e overwrite prompt latched slot 2, then +0x54 = 3 after the confirmed save: slot 2");
    check(ev("save", 3) == 2, "M14f a later +0x54 = 3 with no latch keeps the slot");
    check(ev("load", 7) == 2 && ev("query", 0) == 2, "M14g an out-of-range +0x54 never becomes the slot");
    check(eventsSilent, "M14h events (new/load/save/overwrite) are not answered; only a query replies");
    K::modBridgeSetSaveFlowSlotForTest(-2);

    // M15: the Dev Tools menu (host half; kernel/devtools_menu.cpp)
    const fs::path plans = root / "plans";
    writeHost(plans / "b_warworld.txt", "# menu: WARWORLD ARENA (WAROBJ01)\nwhen won 2 do snapshot X\n");
    writeHost(plans / "a_mst2.txt", "# skipplan skipplan/1: target 1 MSTobj02 (MST02TriggerEnt) from chapter 1\nwhen won 0 do snapshot Y\n");
#ifdef _WIN32
    _wputenv_s(L"SR_DEVTOOLS_PLANS", plans.wstring().c_str());
#endif
    K::devToolsResetForTest(0);
    K::devMenuResetForTest();
    auto menu = [&](const std::string& a) {
      std::vector<K::ModReply> out;
      K::modBridgeDispatchEx("SR_DevMenu", a, &out);
      return K::devMenuDecodeForTest(out);
    };
    K::DevMenuPageForTest p = menu("open main FE");
    check(p.page == "main" && p.action == 1 && p.labels.size() == 4 && p.labels[0] == "DEV TOOLS: OFF" &&
              p.kinds[0] == 1 && p.labels[1] == "LEVEL SKIP",
          "M15a open main: 4 rows, DEV TOOLS: OFF (a toggle), LEVEL SKIP, shown");
    check(!K::devToolsAnyDevtoolClass() && !K::devToolsTainted(), "M15b opening the menu changes nothing");
    p = menu("press main 1");
    check(p.page == "notice", "M15c LEVEL SKIP while off -> the notice first");
    p = menu("press notice 1");
    check(p.page == "main" && !K::devToolsTainted() && K::modOptionGet("devtools_notice_ack") == 0,
          "M15d CANCEL on the notice: back to main, nothing on, not acknowledged");
    p = menu("press main 0");
    check(p.page == "notice" && p.labels.size() == 2 && p.labels[0] == "ENABLE DEV TOOLS",
          "M15e DEV TOOLS: OFF -> the one-time notice (ENABLE DEV TOOLS / CANCEL)");
    p = menu("press notice 0");
    check(p.page == "main" && p.labels[0] == "DEV TOOLS: ON" && K::devToolAllowed("devmenu") &&
              K::devToolsTainted() && K::modOptionGet("devtools_notice_ack") == 1 &&
              std::string(K::devToolsTitleSuffix()).find("dev-tool-assisted") != std::string::npos,
          "M15f ENABLE: devmenu on, the session tainted (title suffix), the notice acknowledged for good");
    p = menu("press main 1");
    check(p.page == "levelskip:0" && p.labels.size() == 2 && p.labels[0] == "MSTOBJ02" &&
              p.labels[1] == "WARWORLD ARENA (WAROBJ01)",
          "M15g LEVEL SKIP lists the plans (file order; '# menu:' label, else the planner's target)");
    p = menu("press levelskip:0 1");
    const K::DevToolsSkipStatus st = K::devToolsLevelskipStatus();
    check(K::devToolAllowed("levelskip") && st.state == "armed" && st.plan == "b_warworld.txt" &&
              p.labels.size() == 2 && p.labels[0] == "STOP LEVEL SKIP" &&
              p.body.find("ARMED: WARWORLD ARENA") != std::string::npos,
          "M15h picking a plan switches levelskip on and arms it (it starts at the next active tick)");
    p = menu("press levelskip:0 0");
    check(K::devToolsLevelskipStatus().state != "armed" && p.labels[0] != "STOP LEVEL SKIP",
          "M15i STOP LEVEL SKIP disarms it, the plan list is back");
    p = menu("back levelskip:0");
    check(p.page == "main" && p.focus == 1, "M15j B on LEVEL SKIP: main, LEVEL SKIP focused");
    p = menu("press main 3");
    check(p.page == "status" && p.body.find("dev-tool assisted") != std::string::npos &&
              p.body.find("Autosave without asking:") != std::string::npos,
          "M15k STATUS: the session class, acts, level skip, save slot and autosave readouts");
    p = menu("press main 2");
    check(p.page == "cheats:0" && p.labels.empty() == false && p.body.find("No cheats") != std::string::npos,
          "M15l CHEATS with no registered rows: the extension point explains itself");
    {
      static int s_on = 0, s_speed = 100;
      K::DevMenuItemDef tg;
      tg.id = "test.toggle";
      tg.label = "PAUSE TIMERS";
      tg.kind = K::DevMenuKind::Toggle;
      tg.tool = "freecam";
      tg.get = [] { return s_on; };
      tg.set = [](int v) { s_on = v; return true; };
      K::devMenuRegister(tg);
      K::DevMenuItemDef sl;
      sl.id = "test.slider";
      sl.label = "Flight speed";
      sl.kind = K::DevMenuKind::Slider;
      sl.get = [] { return s_speed; };
      sl.set = [](int v) { s_speed = v; return true; };
      sl.min = 50;
      sl.max = 200;
      sl.step = 25;
      sl.unit = "%";
      K::devMenuRegister(sl);
      p = menu("open cheats IG");
      check(p.labels.size() == 2 && p.labels[0] == "PAUSE TIMERS: OFF" && p.labels[1] == "FLIGHT SPEED: 100%" &&
                p.kinds[1] == 2,
            "M15m registered rows (DT-2 extension point): a toggle and a slider with their values");
      p = menu("adjust cheats:0 1 1");
      check(p.action == 2 && s_speed == 125 && p.labels[1] == "FLIGHT SPEED: 125%",
            "M15n RIGHT on the slider: +step, the row updates in place (action 2)");
      menu("adjust cheats:0 1 -1");
      menu("adjust cheats:0 1 -1");
      p = menu("adjust cheats:0 1 -1");
      check(s_speed == 50 && p.labels[1] == "FLIGHT SPEED: 50%", "M15o the slider stops at its minimum");
      p = menu("press cheats:0 0");
      check(s_on == 1 && p.labels[0] == "PAUSE TIMERS: ON" && K::devToolAllowed("freecam"),
            "M15p A on a toggle flips it and switches the row's tool on");
    }
    p = menu("back main");
    check(p.action == 0 && p.labels.empty(), "M15q B on main closes the menu (back to the caller)");
    menu("open main FE");
    p = menu("press main 0");
    check(p.page == "main" && p.labels[0] == "DEV TOOLS: OFF" && !K::devToolAllowed("devmenu") &&
              !K::devToolAllowed("levelskip") && !K::devToolAllowed("freecam") && K::devToolsTainted() &&
              K::devToolsRunClassJson().find("dev-tool-assisted") != std::string::npos,
          "M15r DEV TOOLS: ON -> OFF switches every tool off; the session stays marked dev-tool assisted");
    p = menu("press main 0");
    check(p.page == "main" && K::devToolAllowed("devmenu"), "M15s once acknowledged, enabling again needs no notice");
    K::devToolsResetForTest(0);
    K::devMenuResetForTest();
#ifdef _WIN32
    _wputenv_s(L"SR_DEVTOOLS_PLANS", L"");
#endif

    // M16: XamUserGetName (Xenia semantics) and the player's gamertag
    writeHost(croot / "settings" / "profile.ini", "# test\ngamertag=Clark Kent\n");
    constexpr uint32_t kName = 0x90310600u;
    zero(kName, 32);
    check(K::xamUserGetName(4, kName, 16) == 0x57, "M16a user index >= 4: X_ERROR_INVALID_PARAMETER");
    putStr(kName, "XYZ");
    check(K::xamUserGetName(1, kName, 16) == 0x525 && *K::guestVAToHost(kName) == 0 &&
              *K::guestVAToHost(kName + 1) == 'Y',
          "M16b user 1 (not signed in): X_ERROR_NO_SUCH_USER, only byte 0 cleared");
    zero(kName, 32);
    check(K::xamUserGetName(0, kName, 16) == 0 &&
              std::string(reinterpret_cast<const char*>(K::guestVAToHost(kName))) == "Clark Kent",
          "M16c user 0: the gamertag from settings/profile.ini");
    zero(kName, 32);
    putStr(kName + 4, "Q");
    check(K::xamUserGetName(0, kName, 4) == 0 &&
              std::string(reinterpret_cast<const char*>(K::guestVAToHost(kName))) == "Cla" &&
              *K::guestVAToHost(kName + 4) == 'Q',
          "M16d a short buffer: truncated to len-1 characters + NUL (copy_truncating), nothing past it");
#ifdef _WIN32
    _wputenv_s(L"SR_CONTENT_DIR", L"");
#endif
  }


  // M17: the MODS menu (host half; kernel/mod_menu.cpp, docs/MODS.md "The MODS menu"), driven
  // through the bridge's SR_Menu command exactly as the Apt renderer sends it. The fallback
  // catalog runs over a temp content root with its own fake disc tree and mods folder.
  {
    const fs::path m17 = root / "m17";
    const fs::path mdir = m17 / "mods";
    const fs::path discRoot = m17 / "Superman Returns (USA)";
    const fs::path list = mdir / "enabled.txt";
    fs::create_directories(mdir, ec);
    writeHost(discRoot / "DATA" / "text.AST", "DISC");
    writeHost(discRoot / "DATA" / "ui.AST", "DISC");
    writeHost(discRoot / "DATA" / "baseworl.AST", "DISC");
    const fs::path c17 = m17 / "content";
    fs::create_directories(c17, ec);
#ifdef _WIN32
    _wputenv_s(L"SR_CONTENT_DIR", c17.wstring().c_str());   // fresh mod_options.ini: autosave = 1
#endif
    int restarts = 0;
    static int* s_restarts = nullptr;
    s_restarts = &restarts;
    K::modMenuResetForTest();
    K::modMenuSetRestartHandlerForTest([] { ++*s_restarts; });
    auto use = [&](const std::string& lf, const std::vector<std::string>& boot) {
      K::modMenuSetCatalog(K::modMenuMakeFallbackCatalog(mdir.generic_string(), lf, boot));
    };
    auto menu = [&](const std::string& a) {
      std::vector<K::ModReply> out;
      K::modBridgeDispatchEx("SR_Menu", a, &out);
      return K::hostPageDecodeForTest(out);
    };
    auto readList = [&]() { return readHostHead(list, 4096); };

    // a) nothing installed: a short empty state; the actual path belongs on help
    use(list.generic_string(), {});
    K::HostPageForTest p = menu("open mods FE");
    check(p.page == "mods:0" && p.title == "MODS" && p.labels.size() == 3 && p.labels[0] == "OPEN MODS FOLDER" &&
              p.labels[1] == "CHECK AGAIN" && p.labels[2] == "HOW TO INSTALL MODS" &&
              p.body.find("No mods installed") != std::string::npos &&
              p.body.find("m17") == std::string::npos && p.body.find("superman-recomp") == std::string::npos && p.action == 1,
          "M17a short empty state: folder, refresh and help actions; release name, no development path");
    p = menu("press mods:0 2");
    std::string helpPath = p.body;
    helpPath.erase(std::remove(helpPath.begin(), helpPath.end(), '\n'), helpPath.end());
    check(p.page == "modhow" && helpPath.find("m17\\mods") != std::string::npos &&
              p.labels[0] == "OPEN MODS FOLDER", "M17a2 help shows the dynamically selected folder");
    p = menu("back mods:0");
    check(p.action == 0, "M17b B on the first MODS page closes the menu (back to the caller)");

    // b) install: a port feature (hidden), two mods that change the same file, a broken folder, a zip
    writeHost(mdir / "pf" / "mod.json", "{\"name\": \"Custom front end\", \"port_feature\": true, \"x\": [1, {\"y\": \"]\"}]}");
    writeHost(mdir / "pf" / "DATA" / "ui.AST", "PF");
    writeHost(mdir / "pf" / "@autosave_without_asking" / "DATA" / "baseworl.AST", "PFOPT");
    writeHost(mdir / "alpha" / "mod.json",
              "{\"name\": \"Alpha Pack\", \"author\": \"Tester\", \"version\": \"1.2\", \"description\": \"Line one.\\nLine two.\"}");
    writeHost(mdir / "alpha" / "DATA" / "text.AST", "A");
    writeHost(mdir / "alpha" / "DATA" / "baseworl.AST", "A");
    writeHost(mdir / "beta" / "DATA" / "text.AST", "B");
    writeHost(mdir / "beta" / "DATA" / "ui.AST", "B");
    writeHost(mdir / "beta" / "DATA" / "new.bin", "B");
    writeHost(mdir / "empty_mod" / "README.txt", "read me");
    writeHost(mdir / "zipmod.zip", "PK");
    writeHost(list, "# my own note\npf\nalpha\n# beta\n");
    use(list.generic_string(), {"pf", "alpha"});
    p = menu("open mods FE");
    check(p.labels.size() == 5 && p.labels[0] == "ALPHA PACK: ON - CONFLICT" && p.labels[1] == "BETA: OFF - CONFLICT" &&
              p.labels[2] == "EMPTY MOD: OFF - FAILED" && p.labels[3] == "MORE MODS" &&
              p.labels[4] == "HOW TO INSTALL MODS" && p.kinds[0] == 1 && p.body.find("4 mods installed") != std::string::npos,
          "M17c the list: load order, ON/OFF, badges, the port feature hidden, MORE MODS, HOW TO INSTALL MODS");
    p = menu("press mods:0 3");
    check(p.page == "mods:3" && p.labels.size() == 2 && p.labels[0] == "ZIPMOD: OFF - FAILED",
          "M17d MORE MODS: the next page (a dropped .zip is listed as FAILED)");
    p = menu("press mods:3 0");
    check(p.page == "mod:zipmod" && p.body.find("unzip it") != std::string::npos,
          "M17e the .zip's details say to unzip it");
    p = menu("open mod:alpha FE");
    check(p.title == "ALPHA PACK" && p.body.find("Version 1.2 by Tester.") != std::string::npos &&
              p.body.find("Line one.\nLine two.") != std::string::npos &&
              p.body.find("Status: CONFLICT - DATA/baseworl.AST is also changed by AUTOSAVE WITHOUT ASKING (PORT FEATURE OPTION) (that one wins)") != std::string::npos &&
              p.body.find("It is loaded now.") != std::string::npos && p.labels.size() == 4 &&
              p.labels[0] == "ENABLED: ON" && p.labels[1] == "LOAD ORDER: 1 OF 4" && p.labels[2] == "WHAT IT CHANGES (2)" &&
              p.labels[3] == "CONFLICTS (1)",
          "M17f details: version/author/description, the status reason (a port feature's option folder wins), rows");

    // c) switch beta on from the list (LEFT/RIGHT): saved, pending restart, the footer changes
    p = menu("adjust mods:0 1 1");
    std::string txt = readList();
    check(p.action == 1 && p.focus == 1 && p.labels[1] == "BETA: ON - CONFLICT" && p.labels[4] == "RESTART TO APPLY" &&
              txt.find("# my own note\n") == 0 && txt.find("\nbeta\n") != std::string::npos &&
              txt.find("# empty_mod\n") != std::string::npos,
          "M17g LEFT/RIGHT on a list row switches it on: enabled.txt rewritten (player's comment kept), RESTART TO APPLY");
    p = menu("open mod:beta FE");
    check(p.labels[0] == "ENABLED: ON" && p.labels[1] == "LOAD ORDER: 2 OF 4" && p.labels[2] == "WHAT IT CHANGES (3)" &&
              p.labels[3] == "CONFLICTS (2)" && p.body.find("It loads after a restart.") != std::string::npos,
          "M17h beta: ON at the next launch, 2 conflicts (alpha's text.AST, the port feature's ui.AST)");
    p = menu("press mod:beta 3");
    check(p.page == "modconf:beta:0" && p.body.find("DATA/text.AST is also changed by ALPHA PACK. That version is used") != std::string::npos &&
              p.body.find("CUSTOM FRONT END (PORT FEATURE)") != std::string::npos,
          "M17i CONFLICTS names the other mod, the target and who wins");
    p = menu("adjust mod:beta 1 -1");
    txt = readList();
    check(p.page == "mod:beta" && p.labels[1] == "LOAD ORDER: 1 OF 4" && txt.find("pf\nbeta\nalpha\n") != std::string::npos,
          "M17j LEFT on LOAD ORDER moves beta up: the file order changes, the port feature keeps its place");
    p = menu("press mod:beta 3");
    check(p.body.find("DATA/text.AST is also changed by ALPHA PACK. This mod's version is used.") != std::string::npos,
          "M17k after the move beta wins text.AST");
    p = menu("press mod:beta 2");
    check(p.page == "modfiles:beta:0" && p.body.find("Adds DATA/new.bin") != std::string::npos &&
              p.body.find("Replaces DATA/text.AST") != std::string::npos,
          "M17l WHAT IT CHANGES: Replaces (on the disc) / Adds (new)");
    p = menu("back modfiles:beta:0");
    check(p.page == "mod:beta" && p.focus == 2, "M17m B goes back to the details page");
    p = menu("back mod:beta");
    check(p.page == "mods:0" && p.focus == 0, "M17n B on details goes back to the list, focus on that mod");
    p = menu("adjust mod:beta 1 -1");
    check(p.page == "mod:beta", "M17o moving the first mod up is refused (the page is re-shown)");

    // d) RESTART: confirm page, then one restart request (the exit path relaunches)
    p = menu("press mods:0 4");
    check(p.page == "modrestart" && p.labels.size() == 2 && p.labels[0] == "RESTART NOW", "M17p RESTART TO APPLY -> confirm");
    p = menu("press modrestart 0");
    check(p.action == 0 && restarts == 1 && K::modMenuRestartRequested(), "M17q RESTART NOW: one quit request, restart flagged");
    K::modMenuResetForTest();
    K::modMenuSetRestartHandlerForTest([] { ++*s_restarts; });

    // e) switch alpha off; switching beta back off restores the boot list -> nothing pending
    use(list.generic_string(), {"pf", "alpha"});
    p = menu("adjust mods:0 0 1");   // beta (now first) off
    p = menu("open mods FE");
    check(p.labels[0] == "BETA: OFF - CONFLICT" && p.labels[4] == "HOW TO INSTALL MODS",
          "M17r beta off again: the ON list equals what this process mounted, so nothing is pending");

    // f) a session-only catalog (SR_MODS=a;b): no file written, the restart carries the list
    writeHost(list, "pf\nalpha\n");
    const std::string before = readList();
    use("", {"pf", "alpha"});
    p = menu("open mods FE");
    check(p.body.find("changes last until the game closes") != std::string::npos, "M17s session mode says so");
    p = menu("adjust mods:0 1 1");   // beta on (session)
    check(readList() == before && K::modMenuCatalog()->sessionListForRestart() == "pf;alpha;beta",
          "M17t session mode: enabled.txt untouched, the restart list = pf;alpha;beta");

    // g) extension points: per-mod OPTIONS, the costume picker
    use(list.generic_string(), {"pf", "alpha"});
    static int s_opt = 0;
    K::DevMenuItemDef od;
    od.id = "alpha.hard";
    od.label = "Hard mode";
    od.kind = K::DevMenuKind::Toggle;
    od.get = [] { return s_opt; };
    od.set = [](int v) { s_opt = v; return true; };
    K::modMenuRegisterOption("alpha", od);
    p = menu("open mod:alpha FE");
    check(!p.labels.empty() && p.labels.back() == "OPTIONS", "M17u a registered option adds OPTIONS to the details");
    p = menu("press mod:alpha " + std::to_string(p.labels.size() - 1));
    check(p.page == "modopts:alpha:0" && p.labels.size() == 1 && p.labels[0] == "HARD MODE: OFF", "M17v the OPTIONS page");
    p = menu("adjust modopts:alpha:0 0 1");
    check(s_opt == 1 && p.action == 2 && p.labels[0] == "HARD MODE: ON", "M17w an option toggles in place");
    p = menu("open costumes IG");
    check(p.body.find("No costume packs") != std::string::npos && K::modMenuCostumeCount() == 0,
          "M17x the COSTUMES slot with nothing registered");
    static int s_sel = 0;
    K::CostumeDef cd;
    cd.id = "test.red_son";
    cd.label = "RED SON";
    cd.source = "alpha";
    cd.current = [] { return false; };
    cd.select = [] { ++s_sel; return true; };
    K::modMenuRegisterCostume(cd);
    p = menu("open costumes IG");
    check(p.labels.size() == 1 && p.labels[0] == "RED SON", "M17y a registered costume is listed");
    p = menu("press costumes:0 0");
    check(s_sel == 1, "M17z selecting it calls the provider once");

    // h) the router: other pages still reach the Dev Tools menu; SR_MODS=@<file>
    p = menu("open main FE");
    check(p.page == "main" && p.title == "DEV TOOLS", "M17aa SR_Menu routes non-mod pages to the Dev Tools menu");
    {
      std::string src, lf;
      const std::string at = "@" + list.generic_string();
      const auto l = K::modComputeEnabledList(m17.generic_string(), at.c_str(), nullptr, "roll1", &src, &lf);
      check(l == std::vector<std::string>{"pf", "alpha"} && lf == list.generic_string() &&
                src.find("SR_MODS=@<file>") != std::string::npos,
            "M17ab SR_MODS=@<file> reads that list in a roll and names it as the list file");
      std::string lf2 = "x";
      K::modComputeEnabledList(m17.generic_string(), "a;b", nullptr, "roll1", &src, &lf2);
      check(lf2.empty(), "M17ac SR_MODS=a;b has no list file (the menu keeps changes for the session)");
    }
    K::modMenuSetRestartHandlerForTest(nullptr);
    K::modMenuResetForTest();
#ifdef _WIN32
    _wputenv_s(L"SR_CONTENT_DIR", L"");
#endif
  }

  // M18: relocation and release branding. No original disc writes or Explorer launch.
  {
    const fs::path install = root / fs::path(u8"Jeux/M\u00f6ds/\u8d85\u4eba/Superman Returns");
    const fs::path decoy = root / "unrelated-working-directory";
    fs::create_directories(install / "Superman Returns (USA)", ec);
    fs::create_directories(decoy / "Superman Returns (USA)", ec);
    const std::string expected = K::modPathU8(install) + "/";
    check(K::contentRootForLocations(K::modPathU8(install / "SupermanReturns.exe"), K::modPathU8(decoy)) == expected,
          "M18a relocated release: spaces and Unicode, exe-side assets win over unrelated CWD");
    check(K::contentRootForLocations(K::modPathU8(install / "bin" / "SupermanReturns.exe"), "") == expected,
          "M18b the bin/build layout also finds the release install root");
    check(K::contentRootForLocations("", K::modPathU8(install)) == expected &&
              K::contentRootForLocations("", K::modPathU8(root / "missing" )).empty(),
          "M18c working-directory fallback and missing assets (no invented development path)");
    const fs::path longInstall = root / std::string(95, 'x') / "Superman Returns";
    fs::create_directories(longInstall / "Superman Returns (USA)", ec);
    check(K::contentRootForLocations(K::modPathU8(longInstall / "SupermanReturns.exe"), "") == K::modPathU8(longInstall) + "/",
          "M18d discovery is not limited to a short project directory name");

    K::ModBootEnv env;
    env.contentRoot = K::modPathU8(install);
    env.hasSrMods = true;
    env.srMods = "@player";
    env.hasPort = true;
    env.srPortFeatures = "0";
    env.automated = true;
    env.allowStateWrite = false;
    K::modOverlayResetForTestEnv(env);
    K::modMenuResetForTest();
#ifdef _WIN32
    const char* oldGate = std::getenv("SR_MOD_MANAGER");
    const bool hadGate = oldGate != nullptr;
    const std::string savedGate = oldGate ? oldGate : "";
    _putenv_s("SR_MOD_MANAGER", "1");
#endif
    const auto catalog = K::modMenuMakeModcoreCatalog();
    check(catalog != nullptr, "M18e relocation test uses the real mod-manager menu adapter");
    if (!catalog) {
      K::modOverlayResetForTest(root.generic_string(), {});
      fs::remove_all(root, ec);
      return g_fail;
    }
    K::modMenuSetCatalog(catalog);
    auto menu = [&](const std::string& a) {
      std::vector<K::ModReply> out;
      K::modBridgeDispatchEx("SR_Menu", a, &out);
      return K::hostPageDecodeForTest(out);
    };
    const auto v = catalog->view();
    check(fs::u8path(v.modsDir) == install / "mods" && v.autoEnableNew && !v.canRescan,
          "M18f UI folder matches the actual manager root, not the cached repo root");
    auto p = menu("open mods FE");
    check(p.labels.size() == 3 && p.labels[1] == "RESTART TO APPLY" &&
              p.body.find("New mods start enabled") != std::string::npos &&
              p.body.find("CHECK AGAIN") == std::string::npos && p.body.find("superman-recomp") == std::string::npos,
          "M18g manager help is honest: automatic enable, restart; no fake CHECK AGAIN");
    p = menu("press mods:0 1");
    check(p.page == "modrestart", "M18h restart action asks for confirmation, never restarts immediately");
    static std::string opened;
    opened.clear();
    K::modMenuSetFolderHandlerForTest([](const std::string& path) { opened = path; return true; });
    p = menu("press mods:0 0");
    check(p.page == "mods:0" && fs::u8path(opened) == install / "mods",
          "M18i OPEN MODS FOLDER passes the exact relocated Unicode path to the opener");
    K::modMenuSetFolderHandlerForTest([](const std::string&) { return false; });
    p = menu("press mods:0 0");
    check(p.page == "modfoldererror" && p.body.find("could not be opened") != std::string::npos,
          "M18j folder-open failure is visible, not a silent no-op");
    p = menu("press modfoldererror 0");
    std::string flat = p.body;
    flat.erase(std::remove(flat.begin(), flat.end(), '\n'), flat.end());
    check(p.page == "modhow" && flat.find(v.modsDir) != std::string::npos,
          "M18k help wraps but preserves the exact relocated path");
    K::modMenuSetCatalog(K::modMenuMakeFallbackCatalog("mods", "", {}));
    K::modMenuSetFolderHandlerForTest([](const std::string&) { opened = "UNSAFE"; return true; });
    opened.clear();
    p = menu("press mods:0 0");
    check(p.page == "modfoldererror" && opened.empty(), "M18l relative/unresolved paths never launch a folder opener");
    const fs::path unicodeDiscFile = install / "Superman Returns (USA)" / "DATA" / "relocation.bin";
    writeHost(unicodeDiscFile, "UNICODE DISC");
    env.srMods = "0";
    K::modOverlayResetForTestEnv(env);
    check(guestRead(K::modPathU8(unicodeDiscFile)) == "UNICODE DISC",
          "M18m real guest CreateFileA/ReadFile reads a Unicode installation path (not only path discovery)");
    const uint32_t unicodeDir = ntOpen(K::modPathU8(unicodeDiscFile.parent_path()));
    const auto unicodeFiles = unicodeDir ? ntList(unicodeDir) : std::vector<Listed>{};
    if (unicodeDir) exportCall("NtClose", unicodeDir);
    check(unicodeDir && find(unicodeFiles, "relocation.bin"),
          "M18n real NtOpenFile/NtQueryDirectoryFile enumerates the Unicode install directory");
    const fs::path unicodeMods = install / "mods";
    writeHost(unicodeMods / "relocatedpack" / "mod.json", "{\"name\":\"Relocated pack\"}");
    writeHost(unicodeMods / "relocatedpack" / "DATA" / "relocation.bin", "UNICODE MOD");
    env.srMods = "relocatedpack";
    K::modOverlayResetForTestEnv(env);
    check(guestRead(K::modPathU8(unicodeDiscFile)) == "UNICODE MOD" &&
              K::modOverlayResolveFile(K::modPathU8(unicodeDiscFile)) ==
                  K::modPathU8(unicodeMods / "relocatedpack" / "DATA" / "relocation.bin"),
          "M18o the real managed overlay opens its Unicode-path file without falling back to the disc");
    K::modMenuResetForTest();
    p = menu("open mod:relocatedpack FE");
    check(p.title == "RELOCATED PACK" && p.labels.size() >= 3 && p.labels[2] == "WHAT IT CHANGES (1)",
          "M18p real manager menu walks the installed Unicode-path pack for its file details");
    K::modOverlayResetForTest(K::modPathU8(install), {"relocatedpack"});
    check(guestRead(K::modPathU8(unicodeDiscFile)) == "UNICODE MOD",
          "M18q legacy overlay also opens the relocated Unicode-path mod");
    const fs::path unicodeList = unicodeMods / "enabled.txt";
    writeHost(unicodeList, "relocatedpack\n");
    K::modMenuSetCatalog(K::modMenuMakeFallbackCatalog(K::modPathU8(unicodeMods), K::modPathU8(unicodeList), {"relocatedpack"}));
    p = menu("adjust mods:0 0 1");
    check(p.labels[0] == "RELOCATED PACK: OFF" && readHostHead(unicodeList, 4096).find("# relocatedpack") != std::string::npos,
          "M18r legacy menu writes only its own Unicode-path enabled.txt");
    K::modMenuResetForTest();
#ifdef _WIN32
    _putenv_s("SR_MOD_MANAGER", hadGate ? savedGate.c_str() : "");
#endif
  }

  // M19 (iter-465 polish): DISPLAY OPTIONS - routed by page id, every row writes the one store
  // (settings/display.ini under the save root) and the page shows what the store holds.
  // iter-466 gfx: what ships is the PC SETTINGS overlay (M19l); these rows test the Apt page
  // kept as the developer A/B, so the block runs with SR_PC_SETTINGS=0.
  {
    const fs::path c19 = root / "m19" / "content";
    fs::create_directories(c19, ec);
#ifdef _WIN32
    _wputenv_s(L"SR_CONTENT_DIR", c19.wstring().c_str());
    _putenv_s("SR_PC_SETTINGS", "0");
#endif
    const fs::path ini = c19 / "settings" / "display.ini";
    auto menu = [&](const std::string& a) {
      std::vector<K::ModReply> out;
      K::modBridgeDispatchEx("SR_Menu", a, &out);
      return K::hostPageDecodeForTest(out);
    };
    int restarts = 0;
    static int* s_restarts19 = nullptr;
    s_restarts19 = &restarts;
    K::modMenuSetRestartHandlerForTest([] { ++*s_restarts19; });
    K::HostPageForTest p = menu("open display FE");
    check(p.page == "display" && p.title == "DISPLAY OPTIONS" && p.labels.size() == 5 &&
              p.labels[0].rfind("RESOLUTION: ", 0) == 0 && p.labels[1].rfind("DISPLAY: ", 0) == 0 &&
              p.labels[2].rfind("FRAME RATE: DISPLAY (", 0) == 0 && p.labels[3] == "VSYNC: ON" &&
              p.kinds.size() == 5 && p.kinds[0] == 1 && p.kinds[3] == 1 && p.action == 1,
          "M19a open display: 5 rows (RESOLUTION, DISPLAY, FRAME RATE = DISPLAY, VSYNC ON, size), toggles");
    p = menu("adjust display 3 1");
    check(p.labels[3] == "VSYNC: OFF" && readHostHead(ini, 4096).find("vsync=off") != std::string::npos,
          "M19b VSYNC: RIGHT -> OFF, stored (vsync=off)");
    p = menu("adjust display 2 1");
    check(p.labels[2] == "FRAME RATE: UNLIMITED" && readHostHead(ini, 4096).find("target_fps=uncapped") != std::string::npos,
          "M19c FRAME RATE: DISPLAY -> UNLIMITED, stored");
    p = menu("adjust display 2 1");
    check(p.labels[2] == "FRAME RATE: 30 (CONSOLE)" && readHostHead(ini, 4096).find("target_fps=30") != std::string::npos,
          "M19d FRAME RATE wraps to 30 (CONSOLE)");
    p = menu("adjust display 2 -1");
    check(p.labels[2] == "FRAME RATE: UNLIMITED", "M19e LEFT steps back");
    const std::string before = p.labels[1];
    p = menu("adjust display 1 1");
    check(p.labels[1] != before && readHostHead(ini, 4096).find("display_mode = ") != std::string::npos,
          "M19f DISPLAY mode cycles and is stored");
    p = menu("adjust display 0 1");
    p = menu("adjust display 0 1");
    check(p.labels[0] == "RESOLUTION: 2X - RESTART" && p.body.find("restart") != std::string::npos &&
              readHostHead(ini, 4096).find("res_scale = 2") != std::string::npos,
          "M19g RESOLUTION AUTO -> 1X -> 2X: stored, the label and body name the restart");
    p = menu("press display 0");
    check(p.page == "displayrestart" && p.labels.size() == 2 && p.labels[0] == "RESTART NOW",
          "M19h A on a pending RESOLUTION: the RESTART page");
    p = menu("press displayrestart 0");
    check(restarts == 1 && p.action == 0, "M19i RESTART NOW: the relaunch is requested and the menu closes");
    p = menu("open display IG");
    p = menu("back display");
    check(p.action == 0, "M19j B closes the page (back to the caller's panel)");
    p = menu("open mods FE");
    check(p.page.rfind("mods", 0) == 0, "M19k the MODS pages still route to the MODS menu");
    K::modMenuSetRestartHandlerForTest(nullptr);
    K::modMenuResetForTest();
#ifdef _WIN32
    _putenv_s("SR_PC_SETTINGS", "");
#endif
    // M19l (iter-466, what ships): DISPLAY OPTIONS hands off to the PC SETTINGS overlay - the Apt
    // menu closes back to its caller (SR_DTEnd action 0) and the overlay gets a Settings request.
    {
      std::string drained;
      while (K::modlib::takeOpenRequest(&drained)) {
      }
      std::vector<K::ModReply> out;
      K::modBridgeDispatchEx("SR_Menu", "open display IG", &out);
      std::string ctx;
      const bool req = K::modlib::takeOpenRequest(&ctx);
      check(out.size() == 1 && out[0].method == "SR_DTEnd" && req && ctx == "IG" &&
                K::modlib::openPage() == K::modlib::OverlayPage::Settings,
            "M19l open display: the PC SETTINGS overlay is requested (IG) and the Apt menu closes");
      check(!K::modlib::takeOpenRequest(&ctx), "M19m one request per open");
      // lane E L-b: action 3 = handed to the overlay (the Apt menu shows nothing and calls nobody back; the
      // overlay's close calls the opener's SR_OptionsClosed, kernel/game_options.hpp)
      // ... and only with port features on (the callback's pump); off (a front end mounted by SR_MODS in an
      // automated roll): action 0, the caller's menu at once (the review's P1-1; the suite runs both ways)
      const int want = K::automation::portFeaturesOn() ? 3 : 0;
      check(out.size() == 1 && out[0].args.size() == 3 && out[0].args[2].i == want,
            K::automation::portFeaturesOn()
                ? "M19n the hand-off is SR_DTEnd action 3 (the screen's close calls the opener back)"
                : "M19n port features off: the hand-off is SR_DTEnd action 0 (the caller's menu at once)");
    }
#ifdef _WIN32
    _wputenv_s(L"SR_CONTENT_DIR", L"");
#endif
  }

  K::modOverlayResetForTest(root.generic_string(), {});
  fs::remove_all(root, ec);
  std::printf("[MOD_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
