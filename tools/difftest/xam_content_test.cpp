// iter-463 PLATFORM FIX: XAM content packages = real save storage
// (kernel/xam_content.{hpp,cpp}, SR_XAM_CONTENT_FS default ON, sub-gate
// SR_NTSTATUS_DOSERR default ON (baked 2026-10-18) - the test checks that default, then keeps it
// on through the seam for the GetLastError checks unless the env names it).
//
// Drives the REAL recompiled guest wrappers through the import glue and the
// export registry (so the wiring is tested, not just the bodies):
//   sub_828A8218  XContentCreateEx wrapper (user, root, data, flags, pDisp, pLic, ovl)
//   sub_824687F8  CreateFileA          sub_824692B0  WriteFile
//   sub_824685C8  ReadFile             sub_828A7D50  FindFirstFileA
//   sub_82469588  DeleteFileA          sub_82645510  SaveContentFindFirst (the
//                                                    save manager's real consumer)
// against a fresh SR_CONTENT_DIR under the host temp directory.
//
// Run: build/difftest.exe --xam-content-test                    (exit = failed checks)
//      SR_XAM_CONTENT_FS=0 build/difftest.exe --xam-content-test  (the old stubs: the
//      gate-on checks FAIL - that is the attribution / mutation check)
//      SR_NTSTATUS_DOSERR=0 ...                                  (GetLastError checks FAIL)
//      SR_NTSTATUS_DOSERR=1 ...                                  (T0b skipped; all else passes)
//      (T0b checks the default: ON since the step4/audit batch-1 bake)
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/export_registry.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/stubs.hpp"
#include "kernel/xam_content.hpp"
#include "kernel/devtools.hpp"

PPC_EXTERN_FUNC(sub_828A8218);   // XContentCreateEx wrapper
PPC_EXTERN_FUNC(sub_824687F8);   // CreateFileA
PPC_EXTERN_FUNC(sub_824692B0);   // WriteFile
PPC_EXTERN_FUNC(sub_824685C8);   // ReadFile
PPC_EXTERN_FUNC(sub_828A7D50);   // FindFirstFileA
PPC_EXTERN_FUNC(sub_82469588);   // DeleteFileA
PPC_EXTERN_FUNC(sub_82645510);   // SaveContentFindFirst(mgr, outName**)

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

// Scratch guest memory (difftest's flat 4 GB, all committed). Above 0x82000000 so
// NtReadFile's real-convention detector (kGuestVABase) sees guest pointers.
constexpr uint32_t kStack = 0x90100000u;
constexpr uint32_t kPcr = 0x90200000u, kPcrThread = 0x90201000u;   // GetLastError = [[r13+256]+352]
constexpr uint32_t kRoot = 0x90300000u;      // "rmcsave"
constexpr uint32_t kPath = 0x90300100u;      // path strings
constexpr uint32_t kXcd = 0x90301000u;       // XCONTENT_DATA
constexpr uint32_t kDisp = 0x90302000u, kLic = 0x90302004u, kIsCreator = 0x90302008u;
constexpr uint32_t kXuidOut = 0x90302010u, kOvl = 0x90302040u;
constexpr uint32_t kBuf = 0x90310000u, kRead = 0x90320000u, kFind = 0x90330000u;
constexpr uint32_t kEnumBuf = 0x90340000u;
constexpr uint32_t kPcb = 0x90350000u, kPh = 0x90350004u, kCount = 0x90350008u, kDone = 0x9035000Cu;
constexpr uint32_t kMgr = 0x90400000u, kOutSlot = 0x90401000u;
constexpr uint32_t kSaveSize = 88 + 96 * 4;   // header + 96 * N (N = 4)

const char16_t kDisplay[] = u"Superman Returns: The Videogame";
const char* const kFile = "Superman Returns";

void putStr(uint32_t va, const std::string& s) {
  uint8_t* p = K::guestVAToHost(va);
  std::memcpy(p, s.c_str(), s.size() + 1);
}
void zero(uint32_t va, size_t n) { std::memset(K::guestVAToHost(va), 0, n); }
void putXcd(uint32_t va, uint32_t dev, uint32_t type, const std::u16string& disp,
            const std::string& file) {
  zero(va, 0x134);
  K::writeGuestBE32(va, dev);
  K::writeGuestBE32(va + 4, type);
  uint8_t* p = K::guestVAToHost(va);
  for (size_t i = 0; i < disp.size() && i < 127; ++i) {
    p[8 + 2 * i] = uint8_t(uint16_t(disp[i]) >> 8);
    p[8 + 2 * i + 1] = uint8_t(disp[i]);
  }
  for (size_t i = 0; i < file.size() && i < 42; ++i) p[0x108 + i] = uint8_t(file[i]);
}
void saveXcd(const std::string& file = kFile) { putXcd(kXcd, 1, 1, kDisplay, file); }

uint32_t gle() { return K::readGuestU32BE(kPcrThread + 352); }
void setGle(uint32_t v) { K::writeGuestBE32(kPcrThread + 352, v); }

K::ExportFn getExp(const char* name) { return K::ExportRegistry::instance().getExport(name); }

// Run a guest function (or an export) on a context the import glue can see:
// getCurrentGuestR1() must name this frame so the 9th stack argument is found.
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
  x.c.lr = 0;
  fn(x.c, K::getGuestBase());
  return x.c.r3.u32;
}
// An export called the way the import glue calls it: r9/r10 via the extra-args
// channel, the 9th argument at [r1+0x54].
uint32_t exportCall(const char* name, uint32_t a3, uint32_t a4 = 0, uint32_t a5 = 0,
                    uint32_t a6 = 0, uint32_t a7 = 0, uint32_t a8 = 0, uint32_t a9 = 0,
                    uint32_t a10 = 0, uint32_t stack9 = 0) {
  Ctx x;
  K::writeGuestBE32(kStack + 0x54, stack9);
  K::ExportRegistry::setCurrentExtraArgs(a9, a10);
  return getExp(name)(a3, a4, a5, a6, a7, a8);
}

// The guest's own mount: sub_828A8218(user 0, "rmcsave", data, flags).
uint32_t mount(uint32_t flags) { return guest(sub_828A8218, 0, kRoot, kXcd, flags, 0, 0, 0); }
uint32_t unmount() { return exportCall("XamContentClose", kRoot, 0); }
uint32_t createFile(const std::string& path, uint32_t access, uint32_t disp) {
  putStr(kPath, path);
  setGle(0xDEADBEEFu);
  return guest(sub_824687F8, kPath, access, 0, 0, disp, 0x80);
}
void ntClose(uint32_t h) { exportCall("NtClose", h); }

std::vector<uint8_t> readHost(const fs::path& p) {
  std::vector<uint8_t> v;
  if (std::FILE* f = _wfopen(p.c_str(), L"rb")) {
    uint8_t b[4096];
    size_t n;
    while ((n = std::fread(b, 1, sizeof(b), f)) > 0) v.insert(v.end(), b, b + n);
    std::fclose(f);
  }
  return v;
}
void writeHost(const fs::path& p, const std::string& s) {
  if (std::FILE* f = _wfopen(p.c_str(), L"wb")) {
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
  }
}

fs::path g_root;
fs::path pkgDir(const std::string& name = kFile) {
  return g_root / "0009000000000001" / "454107ED" / "00000001" / name;
}
fs::path hdrFile(const std::string& name = kFile) {
  return g_root / "0009000000000001" / "454107ED" / "Headers" / "00000001" / (name + ".header");
}

// Enumerate through the exports (the guest wrapper shape: cItem 1, cb 0x134).
struct EnumRes { uint32_t createRc, pcb, rc1, n1, rc2, n2; };
EnumRes enumerateOnce(uint32_t dev) {
  EnumRes r{};
  K::writeGuestBE32(kPh, 0xFFFFFFFFu);
  r.createRc = exportCall("XamContentCreateEnumerator", 0, dev, 1, 0, 1, kPcb, kPh);
  r.pcb = K::readGuestU32BE(kPcb);
  const uint32_t h = K::readGuestU32BE(kPh);
  zero(kEnumBuf, 0x134);
  K::writeGuestBE32(kCount, 0xCCCCCCCCu);
  r.rc1 = exportCall("XamEnumerate", h, 0, kEnumBuf, 0x134, kCount, 0);
  r.n1 = K::readGuestU32BE(kCount);
  std::vector<uint8_t> first(K::guestVAToHost(kEnumBuf), K::guestVAToHost(kEnumBuf) + 0x134);
  K::writeGuestBE32(kCount, 0xCCCCCCCCu);
  r.rc2 = exportCall("XamEnumerate", h, 0, kEnumBuf + 0x200, 0x134, kCount, 0);
  r.n2 = K::readGuestU32BE(kCount);
  std::memcpy(K::guestVAToHost(kEnumBuf), first.data(), first.size());
  ntClose(h);
  return r;
}
// Every item of one enumeration (cItem 1 per call): {file name, display name ASCII}.
struct Item { std::string file, display; uint32_t type; };
std::vector<Item> enumerateAll() {
  std::vector<Item> out;
  K::writeGuestBE32(kPh, 0xFFFFFFFFu);
  if (exportCall("XamContentCreateEnumerator", 0, 1, 1, 0, 1, kPcb, kPh) != 0) return out;
  const uint32_t h = K::readGuestU32BE(kPh);
  for (int guard = 0; guard < 64; ++guard) {
    zero(kEnumBuf, 0x134);
    if (exportCall("XamEnumerate", h, 0, kEnumBuf, 0x134, kCount, 0) != 0) break;
    const uint8_t* e = K::guestVAToHost(kEnumBuf);
    Item it;
    it.type = K::readGuestU32BE(kEnumBuf + 4);
    for (uint32_t i = 0; i < 42 && e[0x108 + i]; ++i) it.file.push_back(char(e[0x108 + i]));
    for (uint32_t i = 0; i < 128; ++i) {
      const uint16_t c = uint16_t((e[8 + 2 * i] << 8) | e[8 + 2 * i + 1]);
      if (!c) break;
      it.display.push_back(c < 0x80 ? char(c) : '?');
    }
    out.push_back(it);
  }
  ntClose(h);
  return out;
}
const Item* findItem(const std::vector<Item>& v, const std::string& file) {
  for (const Item& i : v)
    if (i.file == file) return &i;
  return nullptr;
}
void writeHostBytes(const fs::path& p, const std::vector<uint8_t>& b) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  if (std::FILE* f = _wfopen(p.c_str(), L"wb")) {
    if (!b.empty()) std::fwrite(b.data(), 1, b.size(), f);
    std::fclose(f);
  }
}
// A structurally valid 0x148-byte header for `file` (display name `disp`).
std::vector<uint8_t> makeHeader(const std::string& file, const std::string& disp, uint32_t type = 1) {
  std::vector<uint8_t> h(0x148, 0);
  h[3] = 1;
  h[7] = uint8_t(type);
  for (size_t i = 0; i < disp.size() && i < 127; ++i) h[8 + 2 * i + 1] = uint8_t(disp[i]);
  for (size_t i = 0; i < file.size() && i < 42; ++i) h[0x108 + i] = uint8_t(file[i]);
  h[0x140] = 0x45; h[0x141] = 0x41; h[0x142] = 0x07; h[0x143] = 0xED;
  return h;
}

bool enumDisplayMatches() {
  const uint8_t* e = K::guestVAToHost(kEnumBuf);
  const uint8_t* x = K::guestVAToHost(kXcd);
  return std::memcmp(e + 8, x + 8, 42 * 2) == 0 && std::memcmp(e + 0x108, x + 0x108, 42) == 0 &&
         K::readGuestU32BE(kEnumBuf) == 1u && K::readGuestU32BE(kEnumBuf + 4) == 1u;
}

// The full guest save: mount CREATE_ALWAYS, CreateFileA CREATE_ALWAYS, WriteFile,
// NtClose, XamContentClose. Returns true when every step reported success.
bool guestSave(uint8_t seed) {
  saveXcd();
  if (mount(2) != 0) return false;
  const uint32_t h = createFile("rmcsave:\\Superman Returns", 0xC0000000u, 2);
  if (h == 0xFFFFFFFFu) { unmount(); return false; }
  uint8_t* p = K::guestVAToHost(kBuf);
  for (uint32_t i = 0; i < kSaveSize; ++i) p[i] = uint8_t(seed + i * 7);
  K::writeGuestBE32(kDone, 0);
  const uint32_t ok = guest(sub_824692B0, h, kBuf, kSaveSize, kDone, 0);
  ntClose(h);
  return ok == 1 && K::readGuestU32BE(kDone) == kSaveSize && unmount() == 0;
}
bool hostSaveMatches(uint8_t seed) {
  const auto v = readHost(pkgDir() / kFile);
  if (v.size() != kSaveSize) return false;
  for (uint32_t i = 0; i < kSaveSize; ++i)
    if (v[i] != uint8_t(seed + i * 7)) return false;
  return true;
}

// FindFirstFileA's WIN32_FIND_DATA: +0 attrs, +4 ftCreation, +12 ftLastAccess,
// +20 ftLastWrite, +28 sizeHigh, +32 sizeLow, +44 cFileName.
uint32_t findFirst(const std::string& path) {
  putStr(kPath, path);
  zero(kFind, 0x200);
  setGle(0xDEADBEEFu);
  return guest(sub_828A7D50, kPath, kFind);
}
}  // namespace

int runXamContentTest() {
  std::printf("[XAMC_TEST] XAM content packages / real saves (SR_XAM_CONTENT_FS)\n");
#ifdef _WIN32
  std::error_code ec;
  g_root = fs::temp_directory_path(ec) / ("sr_xamc_test_" + std::to_string(GetCurrentProcessId()));
  fs::remove_all(g_root, ec);
  _wputenv_s(L"SR_CONTENT_DIR", g_root.c_str());
#endif
  K::registerStubs();
  const bool gateOn = K::xamContentFsEnabled();
  // SR_NTSTATUS_DOSERR: baked default ON (step4/audit batch 1, 2026-10-18,
  // after the natural A/B the iter-463 review asked for). With the env unset,
  // check that default, then keep it on for the Xenia-faithful GetLastError
  // checks; an explicit env value (the =0 mutation) is honoured as-is.
  const char* dosEnv = std::getenv("SR_NTSTATUS_DOSERR");
  if (!dosEnv && gateOn) check(K::ntStatusDosErrEnabled(), "T0b SR_NTSTATUS_DOSERR is default ON (batch-1 bake)");
  const int dosMode = dosEnv ? -1 : 1;
  K::ntStatusDosErrSetEnabledForTest(dosMode);
  std::printf("  gate SR_XAM_CONTENT_FS = %s, SR_NTSTATUS_DOSERR = %s, root = %s\n",
              gateOn ? "ON" : "OFF (old stubs: failures below ARE the defect)",
              K::ntStatusDosErrEnabled() ? "ON" : "OFF",
              reinterpret_cast<const char*>(g_root.u8string().c_str()));
  check(K::xamContentRoot() == g_root, "T0 SR_CONTENT_DIR overrides the content root");

  // The guest's world: a PCR (SetLastError) and the root string.
  zero(kPcr, 0x400);
  K::writeGuestBE32(kPcr + 256u, kPcrThread);
  K::writeGuestBE32(kPcr + 336u, 0u);
  putStr(kRoot, "rmcsave");

  // ── T1: fresh root, empty enumeration ─────────────────────────────────────
  {
    const EnumRes r = enumerateOnce(1);
    std::printf("       T1: create=%u pcb=%X rc=%u n=%u\n", r.createRc, r.pcb, r.rc1, r.n1);
    check(r.createRc == 0 && r.pcb == 0x134u && r.rc1 == 0x12u && r.n1 == 0u,
          "T1 fresh root: CreateEnumerator ok, pcb=0x134, XamEnumerate = NO_MORE_FILES, 0 items");
  }

  // ── T2: the guest wrapper mounts a NEW package (CREATE_ALWAYS) ─────────────
  {
    saveXcd();
    K::writeGuestBE32(kDisp, 0xCCCCCCCCu);
    const uint32_t rc = mount(2);
    const auto hdr = readHost(hdrFile());
    std::vector<uint8_t> want(0x148, 0);
    std::memcpy(want.data(), K::guestVAToHost(kXcd), 0x134);
    // Xenia layout: xuid (0 = the creating user's) at 0x138, title_id at 0x140,
    // 0x134..0x137 and 0x144..0x147 padding.
    want[0x140] = 0x45; want[0x141] = 0x41; want[0x142] = 0x07; want[0x143] = 0xED;
    std::printf("       T2: rc=%u pkg=%d header=%zu bytes\n", rc, fs::is_directory(pkgDir(), ec) ? 1 : 0,
                hdr.size());
    check(rc == 0 && fs::is_directory(pkgDir(), ec), "T2a sub_828A8218(CREATE_ALWAYS) = 0, package dir exists");
    check(hdr == want, "T2b .header = 0x148-byte BE XCONTENT_AGGREGATE_DATA (xuid 0 @0x138, title 454107ED @0x140)");
    check(K::xamContentMountCountForTest() == 1u, "T2c one mount ('rmcsave')");

    // ── T3: CreateFileA + WriteFile + NtClose + XamContentClose ─────────────
    const uint32_t h = createFile("rmcsave:\\Superman Returns", 0xC0000000u, 2);
    const uint32_t gle1 = gle();
    uint8_t* p = K::guestVAToHost(kBuf);
    for (uint32_t i = 0; i < kSaveSize; ++i) p[i] = uint8_t(0x11 + i * 7);
    K::writeGuestBE32(kDone, 0);
    const uint32_t ok = h != 0xFFFFFFFFu ? guest(sub_824692B0, h, kBuf, kSaveSize, kDone, 0) : 0;
    const uint32_t done = K::readGuestU32BE(kDone);
    ntClose(h);
    const uint32_t crc = unmount();
    std::printf("       T3: h=%08X gle=%u write=%u done=%u close=%u\n", h, gle1, ok, done, crc);
    check(h != 0xFFFFFFFFu && gle1 == 0u, "T3a CreateFileA(rmcsave:\\Superman Returns, CREATE_ALWAYS) = handle, GLE 0");
    check(ok == 1u && done == kSaveSize, "T3b WriteFile 88+96*4 bytes = TRUE, count exact");
    check(crc == 0u && K::xamContentMountCountForTest() == 0u, "T3c XamContentClose = 0, unmounted");
    check(hostSaveMatches(0x11), "T3d host file bytes == the guest buffer");
  }

  // ── restart: forget every mount/handle/enumerator; only the disk remains ──
  K::xamContentResetForTest();

  // ── T4: enumeration after the restart carries the header's display name ───
  {
    saveXcd();
    const EnumRes r = enumerateOnce(1);
    std::printf("       T4: rc=%u n=%u next rc=%u n=%u\n", r.rc1, r.n1, r.rc2, r.n2);
    check(r.rc1 == 0u && r.n1 == 1u, "T4a after restart: XamEnumerate = 0, 1 item");
    check(enumDisplayMatches(), "T4b item = dev 1, type 1, display name (42 wchar) + file name from the .header");
    check(r.rc2 == 0x12u && r.n2 == 0u, "T4c next XamEnumerate = 0x12 (FindNext's loop ends)");
    const EnumRes r0 = enumerateOnce(0);
    check(r0.rc1 == 0u && r0.n1 == 1u, "T4d device 0 (any) also lists it");
    const EnumRes rbad = enumerateOnce(7);
    check(rbad.createRc == 0x80070057u && rbad.pcb == 0u, "T4e unknown device -> X_E_INVALIDARG, pcb 0");
  }

  // ── T5: load = mount OPEN_EXISTING + CreateFileA OPEN_EXISTING + ReadFile ──
  {
    saveXcd();
    const uint32_t rc = mount(3);
    const uint32_t h = createFile("rmcsave:\\Superman Returns", 0x80000000u, 3);
    zero(kRead, kSaveSize);
    K::writeGuestBE32(kDone, 0);
    const uint32_t ok = h != 0xFFFFFFFFu ? guest(sub_824685C8, h, kRead, kSaveSize, kDone, 0) : 0;
    const bool same = std::memcmp(K::guestVAToHost(kRead), K::guestVAToHost(kBuf), kSaveSize) == 0;
    std::printf("       T5: mount=%u h=%08X read=%u done=%u same=%d\n", rc, h, ok,
                K::readGuestU32BE(kDone), same ? 1 : 0);
    check(rc == 0u && h != 0xFFFFFFFFu, "T5a mount(OPEN_EXISTING) = 0, CreateFileA(OPEN_EXISTING) = handle");
    check(ok == 1u && K::readGuestU32BE(kDone) == kSaveSize && same, "T5b ReadFile returns the saved bytes exactly");
    // A write through a read-only handle is refused (ACCESS_DENIED -> GLE 5).
    setGle(0xDEADBEEFu);
    const uint32_t wok = h != 0xFFFFFFFFu ? guest(sub_824692B0, h, kBuf, 4, kDone, 0) : 1;
    check(wok == 0u && gle() == 5u, "T5c WriteFile on a GENERIC_READ handle = FALSE, GLE 5");
    ntClose(h);

    // ── T6: FindFirstFileA over the package (the FE save list) ─────────────
    const uint32_t fh = findFirst("rmcsave:\\*.*");
    const std::string name(reinterpret_cast<const char*>(K::guestVAToHost(kFind + 44)));
    const uint32_t sizeLow = K::readGuestU32BE(kFind + 32);
    const uint64_t ftWrite = (uint64_t(K::readGuestU32BE(kFind + 20)) << 32) | K::readGuestU32BE(kFind + 24);
    std::printf("       T6: h=%08X name='%s' size=%u ftWrite=%llX\n", fh, name.c_str(), sizeLow,
                (unsigned long long)ftWrite);
    check(fh != 0xFFFFFFFFu && name == kFile && sizeLow == kSaveSize && ftWrite != 0,
          "T6a FindFirstFileA(rmcsave:\\*.*) = 'Superman Returns', nFileSizeLow 472, real FILETIME");
    if (fh != 0xFFFFFFFFu) ntClose(fh);
    const uint32_t fn = findFirst("rmcsave:\\nomatch");
    check(fn == 0xFFFFFFFFu && gle() == 2u, "T6b mask 'nomatch' -> INVALID_HANDLE_VALUE, GLE 2 (NO_SUCH_FILE)");
    const uint32_t fc = findFirst("rmcsave:\\Superman Returns: The Videogame");
    check(fc == 0xFFFFFFFFu, "T6c the escaped display name (with ':') matches nothing");
    const uint32_t fw = findFirst("rmcsave:\\Super*");
    check(fw != 0xFFFFFFFFu && std::string(reinterpret_cast<const char*>(K::guestVAToHost(kFind + 44))) == kFile,
          "T6d wildcard 'Super*' matches");
    if (fw != 0xFFFFFFFFu) ntClose(fw);
    // ── T6e-g: NtQueryFullAttributesFile inside the package (step4/audit batch 3; Xenia's host-path device,
    // host_path_entry.cc:32-53 and host_path_device.cc:42-44) ─────────────────────────────────────────────────
    {
      constexpr uint32_t kQOa = 0x90360000u, kQAnsi = 0x90360010u, kQStr = 0x90360020u, kQInfo = 0x90360200u;
      auto q = [&](const std::string& p) {
        zero(kQOa, 0x200);
        std::memset(K::guestVAToHost(kQInfo), 0xEE, 56);
        K::writeGuestBE32(kQOa + 4, kQAnsi);
        K::writeGuestBE32(kQOa + 8, 0x40);
        const uint32_t n = uint32_t(p.size());
        K::writeGuestBE32(kQAnsi, (n << 16) | (n + 1));
        K::writeGuestBE32(kQAnsi + 4, kQStr);
        std::memcpy(K::guestVAToHost(kQStr), p.data(), n);
        return exportCall("NtQueryFullAttributesFile", kQOa, kQInfo);
      };
      auto r64 = [](uint32_t va) { return (uint64_t(K::readGuestU32BE(va)) << 32) | K::readGuestU32BE(va + 4); };
      auto ft = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
      const uint32_t s1 = q("rmcsave:\\Superman Returns");
      WIN32_FILE_ATTRIBUTE_DATA d{};
      const bool got = GetFileAttributesExW((pkgDir() / kFile).c_str(), GetFileExInfoStandard, &d) != 0;
      std::printf("       T6e: %08X attr %08X eof %llu alloc %llu c %016llX w %016llX\n", s1,
                  K::readGuestU32BE(kQInfo + 48), (unsigned long long)r64(kQInfo + 40),
                  (unsigned long long)r64(kQInfo + 32), (unsigned long long)r64(kQInfo + 0),
                  (unsigned long long)r64(kQInfo + 16));
      check(s1 == 0 && got && K::readGuestU32BE(kQInfo + 48) == 0x80u && r64(kQInfo + 40) == kSaveSize &&
                r64(kQInfo + 32) == 512 && r64(kQInfo + 0) == ft(d.ftCreationTime) &&
                r64(kQInfo + 16) == ft(d.ftLastWriteTime) && r64(kQInfo + 24) == ft(d.ftLastWriteTime),
            "T6e NtQueryFullAttributesFile(rmcsave:\\Superman Returns) = SUCCESS: NORMAL, end 472, allocation 512, "
            "the host file's times");
      const uint32_t s2 = q("rmcsave:\\");
      check(s2 == 0 && K::readGuestU32BE(kQInfo + 48) == 0x10u && r64(kQInfo + 40) == 0 && r64(kQInfo + 32) == 0 &&
                r64(kQInfo + 0) == 0 && r64(kQInfo + 8) == 0 && r64(kQInfo + 16) == 0 && r64(kQInfo + 24) == 0,
            "T6f the package root rmcsave:\\ = the device's root entry: DIRECTORY, size, allocation and times 0");
      const uint32_t s3 = q("rmcsave:\\missing.bin");
      check(s3 == 0xC000000Fu,
            "T6g a missing package path = STATUS_NO_SUCH_FILE (the attribute query; an open answers "
            "OBJECT_NAME_NOT_FOUND)");
    }

    // ── T7: NT dispositions inside the mount (Xenia VFS OpenFile) ───────────
    const uint32_t hm = createFile("rmcsave:\\missing.bin", 0x80000000u, 3);
    check(hm == 0xFFFFFFFFu && gle() == 2u, "T7a CreateFileA(OPEN_EXISTING, missing) = -1, GLE 2");
    const uint32_t hn = createFile("rmcsave:\\Superman Returns", 0xC0000000u, 1);
    check(hn == 0xFFFFFFFFu && gle() == 80u, "T7b CREATE_NEW on an existing file = -1, GLE 80 (0xC0000035)");
    const uint32_t ha = createFile("rmcsave:\\Superman Returns", 0x80000000u, 4);
    check(ha != 0xFFFFFFFFu && gle() == 183u, "T7c OPEN_ALWAYS on an existing file = handle, GLE 183");
    ntClose(ha);
    const uint32_t ht = createFile("rmcsave:\\tmp.bin", 0xC0000000u, 4);
    check(ht != 0xFFFFFFFFu && gle() == 0u && fs::exists(pkgDir() / "tmp.bin", ec),
          "T7d OPEN_ALWAYS on a new file = handle, GLE 0, host file created");
    ntClose(ht);
    const uint32_t ho = createFile("rmcsave:\\tmp.bin", 0xC0000000u, 2);
    check(ho != 0xFFFFFFFFu && gle() == 183u, "T7e CREATE_ALWAYS on an existing file = GLE 183 (overwritten)");
    ntClose(ho);
    const uint32_t hx = createFile("rmcsave:\\nothere.bin", 0x40000000u, 5);
    check(hx == 0xFFFFFFFFu && gle() == 2u, "T7f TRUNCATE_EXISTING on a missing file = -1, GLE 2");
    const uint32_t hdot = createFile("rmcsave:\\..\\evil.bin", 0x40000000u, 2);
    check(hdot == 0xFFFFFFFFu && gle() == 123u && !fs::exists(pkgDir().parent_path() / "evil.bin", ec),
          "T7g '..' escape = -1, GLE 123 (OBJECT_NAME_INVALID), nothing written outside the package");
    const uint32_t hads = createFile("rmcsave:\\a:stream", 0x40000000u, 2);
    check(hads == 0xFFFFFFFFu && gle() == 123u, "T7h NTFS stream syntax 'a:stream' = GLE 123");

    // ── T10: DeleteFileA (NtOpenFile DELETE + class 13 + NtClose) ───────────
    putStr(kPath, "rmcsave:\\tmp.bin");
    setGle(0xDEADBEEFu);
    const uint32_t del = guest(sub_82469588, kPath);
    check(del == 1u && !fs::exists(pkgDir() / "tmp.bin", ec), "T10a DeleteFileA(rmcsave:\\tmp.bin) = TRUE, host file gone");
    check(unmount() == 0u, "T5d XamContentClose after the load = 0");
    // Unmounted but known root: PATH_NOT_FOUND.
    const uint32_t hu = createFile("rmcsave:\\Superman Returns", 0x80000000u, 3);
    check(hu == 0xFFFFFFFFu && gle() == 3u, "T7i 'rmcsave:' after XamContentClose = -1, GLE 3 (0xC000003A)");
  }

  // ── T7j: OPEN_EXISTING on a missing package -> 3 (the guest maps 9) ────────
  {
    saveXcd("Nope");
    check(mount(3) == 3u && K::xamContentMountCountForTest() == 0u, "T7j sub_828A8218(OPEN_EXISTING, missing) = 3");
    saveXcd();
  }

  // ── T8: double mount / unknown close ────────────────────────────────────────
  {
    const uint32_t a = mount(3), b = mount(3);
    const uint32_t c1 = unmount(), c2 = unmount();
    std::printf("       T8: first=%u second=%u close=%u close2=%u\n", a, b, c1, c2);
    check(a == 0u && b == 0x57u, "T8a second mount of 'rmcsave' = 0x57 (symlink registered)");
    check(c1 == 0u && c2 == 2u, "T8b XamContentClose of an unknown root = 2");
  }

  // ── T9: XamContentGetCreator ────────────────────────────────────────────────
  {
    K::writeGuestBE32(kIsCreator, 0);
    const uint32_t rc = exportCall("XamContentGetCreator", 0, kXcd, kIsCreator, kXuidOut, 0);
    const uint64_t x = (uint64_t(K::readGuestU32BE(kXuidOut)) << 32) | K::readGuestU32BE(kXuidOut + 4);
    check(rc == 0u && K::readGuestU32BE(kIsCreator) == 1u && x == K::kXamUser0Xuid,
          "T9a GetCreator(existing save) = 0, isCreator 1, xuid = user 0's");
    saveXcd("Nope");
    check(exportCall("XamContentGetCreator", 0, kXcd, kIsCreator, 0, 0) == 3u, "T9b GetCreator(missing) = 3");
    check(exportCall("XamContentGetCreator", 0, kXcd, 0, 0, 0) == 0x57u, "T9c GetCreator(pIsCreator 0) = 0x57");
    saveXcd();
  }

  // ── T10b: DeleteFileA on the save itself ───────────────────────────────────
  {
    mount(3);
    putStr(kPath, "rmcsave:\\Superman Returns");
    const uint32_t del = guest(sub_82469588, kPath);
    check(del == 1u && !fs::exists(pkgDir() / kFile, ec) && fs::is_directory(pkgDir(), ec),
          "T10b DeleteFileA(rmcsave:\\Superman Returns) = TRUE, file gone, package stays");

    // ── T11: XamContentDelete ───────────────────────────────────────────────
    check(exportCall("XamContentDelete", 0, kXcd, 0) == 5u, "T11a XamContentDelete while mounted = 5");
    unmount();
    check(exportCall("XamContentDelete", 0, kXcd, 0) == 0u && !fs::exists(pkgDir(), ec),
          "T11b XamContentDelete after close = 0, package removed");
    check(fs::exists(hdrFile(), ec), "T11c the .header stays (Xenia DeleteContent)");
    const EnumRes r = enumerateOnce(1);
    check(r.rc1 == 0x12u && r.n1 == 0u, "T11d enumeration is empty again (orphan header ignored)");
    check(exportCall("XamContentDelete", 0, kXcd, 0) == 2u, "T11e XamContentDelete(missing) = 2");
  }

  // ── TM: the XamContentCreateEx disposition matrix (export, Xenia switch) ───
  {
    saveXcd("Matrix");
    auto create = [&](uint32_t flags, uint32_t ovl = 0) {
      K::writeGuestBE32(kDisp, 0xCCCCCCCCu);
      return exportCall("XamContentCreateEx", 0, kRoot, kXcd, flags, kDisp, kLic, 0, 0, ovl);
    };
    auto disp = [] { return K::readGuestU32BE(kDisp); };
    uint32_t rc;
    rc = create(3);
    check(rc == 3u && disp() == 0u, "TM1 OPEN_EXISTING(missing) = 3 (PATH_NOT_FOUND), disp 0");
    rc = create(5);
    check(rc == 3u, "TM2 TRUNCATE_EXISTING(missing) = 3");
    rc = create(1);
    check(rc == 0u && disp() == 1u && fs::is_directory(pkgDir("Matrix"), ec), "TM3 CREATE_NEW(missing) = 0, disp 1 (created)");
    unmount();
    rc = create(1);
    check(rc == 0xB7u, "TM4 CREATE_NEW(existing) = 0xB7 (ALREADY_EXISTS)");
    rc = create(4);
    check(rc == 0u && disp() == 2u, "TM5 OPEN_ALWAYS(existing) = 0, disp 2 (opened)");
    unmount();
    writeHost(pkgDir("Matrix") / "keep.bin", "old");
    rc = create(2);
    const fs::path bk = g_root / ".backup" / "0009000000000001" / "454107ED" / "00000001" / "Matrix" / "keep.bin";
    check(rc == 0u && disp() == 1u && !fs::exists(pkgDir("Matrix") / "keep.bin", ec) && fs::exists(bk, ec),
          "TM6 CREATE_ALWAYS(existing) = 0, disp 1, old package replaced (host-only .backup keeps it)");
    unmount();
    writeHost(pkgDir("Matrix") / "keep2.bin", "x");
    rc = create(5);
    check(rc == 0u && disp() == 1u && !fs::exists(pkgDir("Matrix") / "keep2.bin", ec),
          "TM7 TRUNCATE_EXISTING(existing) = 0, disp 1, emptied");
    unmount();
    saveXcd("Matrix2");
    rc = create(4);
    check(rc == 0u && disp() == 1u, "TM8 OPEN_ALWAYS(missing) = 0, disp 1");
    unmount();
    check(create(6) == 0x57u, "TM9 flags & 0xF = 6 -> 0x57");
    // Overlapped (9th argument at [r1+0x54]): 997, completion in the XOVERLAPPED.
    zero(kOvl, 0x1C);
    rc = create(3, kOvl);
    check(rc == 997u && K::readGuestU32BE(kOvl) == 0u && K::readGuestU32BE(kOvl + 4) == 2u &&
              K::readGuestU32BE(kOvl + 0x18) == 0u,
          "TM10 overlapped OPEN_EXISTING = 997; ovl result 0, InternalHigh = disposition 2");
    zero(kOvl, 0x1C);
    rc = exportCall("XamContentClose", kRoot, kOvl);
    check(rc == 997u && K::readGuestU32BE(kOvl) == 0u, "TM11 overlapped XamContentClose = 997, result 0");
    zero(kOvl, 0x1C);
    saveXcd("NotThere");
    rc = create(3, kOvl);
    check(rc == 997u && K::readGuestU32BE(kOvl) == 0x65Bu && K::readGuestU32BE(kOvl + 0x18) == 0x80070003u,
          "TM12 overlapped failure: result 0x65B, extended error HRESULT_FROM_WIN32(3)");
    saveXcd("Matrix");
    check(exportCall("XamContentCreateEx", 1, kRoot, kXcd, 3, kDisp, 0) == 0x525u, "TM13 user 1 (not signed in) = 0x525");
    saveXcd("");
    check(create(3) == 0x7Bu, "TM14 empty file name = 0x7B");
    saveXcd("..");
    check(create(2) == 0x7Bu, "TM15 file name '..' = 0x7B (host escape refused)");
    saveXcd();
  }

  // ── T12: the disc stays read-only ─────────────────────────────────────────
  {
    const uint32_t h = createFile("d:\\DATA\\probe.tmp", 0x40000000u, 2);
    bool made = false;
#ifdef _WIN32
    wchar_t exe[1024];
    if (GetModuleFileNameW(nullptr, exe, 1024)) {
      const fs::path repo = fs::path(exe).parent_path().parent_path();
      made = fs::exists(repo / "Superman Returns (USA)" / "DATA" / "probe.tmp", ec);
    }
#endif
    check(h == 0xFFFFFFFFu && !made, "T12 CreateFileA(d:\\DATA\\probe.tmp, GENERIC_WRITE, CREATE_ALWAYS) creates nothing");
  }

  // ── T13: the save manager's own FindFirst (sub_82645510) sees the save ────
  {
    const bool saved = guestSave(0x5A);
    check(saved && hostSaveMatches(0x5A), "T13a full guest save flow again (mount, create, write, close)");
    zero(kMgr, 0x400);
    K::writeGuestBE32(kMgr + 8, 0);            // user index
    K::writeGuestBE32(kMgr + 0x360, 1);        // device ref id (HDD)
    K::writeGuestBE32(kOutSlot, 0);
    const uint32_t rc = guest(sub_82645510, kMgr, kOutSlot);
    const bool nameOk = std::memcmp(K::guestVAToHost(kMgr + 0x1F0), K::guestVAToHost(kXcd + 8), 84) == 0;
    std::printf("       T13: rc=%u out=%08X name=%d dev=%u\n", rc, K::readGuestU32BE(kOutSlot), nameOk ? 1 : 0,
                K::readGuestU32BE(kMgr + 0x1E8));
    check(rc == 11u && K::readGuestU32BE(kOutSlot) == kMgr + 0x1F0 && nameOk &&
              K::readGuestU32BE(kMgr + 0x1E8) == 1u,
          "T13b SaveContentFindFirst = 11 (found), publishes mgr+0x1F0 = the display name");
    ntClose(K::readGuestU32BE(kMgr + 0x32C));
  }

  // ── T16: headers read back from disk are validated before the guest sees them ─
  {
    const size_t baseline = enumerateAll().size();
    // a) 0x148 bytes of 0xFF (bit rot / another tool): ignored, folder fallback.
    fs::create_directories(pkgDir("Garbage"), ec);
    writeHostBytes(hdrFile("Garbage"), std::vector<uint8_t>(0x148, 0xFF));
    // b) right size, file name == folder, but the display name has no NUL in 128 wchars.
    fs::create_directories(pkgDir("NoNul"), ec);
    {
      std::vector<uint8_t> h = makeHeader("NoNul", "");
      for (uint32_t i = 0; i < 128; ++i) { h[8 + 2 * i] = 0x01; h[8 + 2 * i + 1] = 0x41; }
      writeHostBytes(hdrFile("NoNul"), h);
    }
    // c) a well-formed header whose file name is not its folder's (renamed folder).
    fs::create_directories(pkgDir("Renamed"), ec);
    writeHostBytes(hdrFile("Renamed"), makeHeader("Other", "Renamed Display"));
    // d) wrong size (0x149): ignored.
    fs::create_directories(pkgDir("BigHdr"), ec);
    {
      std::vector<uint8_t> h = makeHeader("BigHdr", "Big Display");
      h.push_back(0);
      writeHostBytes(hdrFile("BigHdr"), h);
    }
    // e) a well-formed header with the licence word (0x14C): accepted.
    fs::create_directories(pkgDir("Licensed"), ec);
    {
      std::vector<uint8_t> h = makeHeader("Licensed", "Licensed Display");
      h.insert(h.end(), {0, 0, 0, 1});
      writeHostBytes(hdrFile("Licensed"), h);
    }
    // f) a folder name longer than XCONTENT_DATA.file_name (42) is skipped.
    const std::string longName(43, 'L');
    fs::create_directories(pkgDir(longName), ec);

    const std::vector<Item> items = enumerateAll();
    std::printf("       T16: %zu items:", items.size());
    for (const Item& i : items) std::printf(" [%s|%s]", i.file.c_str(), i.display.c_str());
    std::printf("\n");
    const Item* g = findItem(items, "Garbage");
    const Item* nn = findItem(items, "NoNul");
    const Item* rn = findItem(items, "Renamed");
    const Item* bh = findItem(items, "BigHdr");
    const Item* li = findItem(items, "Licensed");
    const Item* sv = findItem(items, kFile);
    check(g && g->display == "Garbage" && g->type == 1u, "T16a 0xFF-filled header ignored: folder-name fallback");
    check(nn && nn->display == "NoNul", "T16b unterminated display name (128 wchars) ignored: fallback");
    check(rn && rn->display == "Renamed" && !findItem(items, "Other"),
          "T16c header file name != folder: fallback lists the folder, never the header's name");
    check(bh && bh->display == "BigHdr", "T16d 0x149-byte header ignored: fallback");
    check(li && li->display == "Licensed Display", "T16e 0x14C-byte header (licence word) accepted");
    check(!findItem(items, longName) && items.size() == baseline + 5,
          "T16f 43-char folder skipped (baseline + the 5 valid-name folders)");
    check(sv && sv->display == "Superman Returns: The Videogame", "T16g the real save still carries its header's name");
    for (const char* n : {"Garbage", "NoNul", "Renamed", "BigHdr", "Licensed"}) {
      fs::remove_all(pkgDir(n), ec);
      fs::remove(hdrFile(n), ec);
    }
    fs::remove_all(pkgDir(longName), ec);
  }

  // ── T17: a CREATE_ALWAYS whose backup move fails loses nothing ─────────────
  {
    const fs::path bkDir = g_root / ".backup" / "0009000000000001" / "454107ED" / "00000001" / kFile;
    // A second save over T13's makes the "previous backup" this test protects.
    const bool resaved = guestSave(0x5A);
    const bool hadBackup = resaved && fs::exists(bkDir / kFile, ec);
    // Another process holds the save file open without FILE_SHARE_DELETE, so the
    // package directory cannot be renamed.
    HANDLE lock = CreateFileW((pkgDir() / kFile).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    saveXcd();
    const uint32_t rc = mount(2);
    const uint32_t mounts = K::xamContentMountCountForTest();
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    std::printf("       T17: lock=%d hadBackup=%d mount(CREATE_ALWAYS)=%u mounts=%u\n",
                lock != INVALID_HANDLE_VALUE ? 1 : 0, hadBackup ? 1 : 0, rc, mounts);
    if (rc == 0) unmount();
    check(lock != INVALID_HANDLE_VALUE && hadBackup, "T17a precondition: save locked, previous backup present");
    check(rc == 0xB7u && mounts == 0u, "T17b CREATE_ALWAYS with the package locked = 0xB7, not mounted");
    check(hostSaveMatches(0x5A) && fs::exists(bkDir / kFile, ec),
          "T17c the live save AND the previous backup both survive the failed replace");
    // Unlocked, the same replace succeeds and the fresh backup is the old save.
    const bool again = guestSave(0x5A);
    check(again && hostSaveMatches(0x5A) && readHost(bkDir / kFile).size() == kSaveSize,
          "T17d unlocked: CREATE_ALWAYS replaces, .backup holds the previous package");
  }

  // ── T18: SR_INSTANCE_ID (automated roll) without SR_CONTENT_DIR = fresh scratch ─
  {
    const char* instEnv = std::getenv("SR_INSTANCE_ID");
    const std::string savedInst = instEnv ? instEnv : "";
    _wputenv_s(L"SR_CONTENT_DIR", L"");
    _putenv_s("SR_INSTANCE_ID", "xamc test");
    const fs::path r = K::xamContentRoot();
    _putenv_s("SR_INSTANCE_ID", "");
    const fs::path persistent = K::xamContentRoot();
    _wputenv_s(L"SR_CONTENT_DIR", g_root.c_str());
    if (!savedInst.empty()) _putenv_s("SR_INSTANCE_ID", savedInst.c_str());
    const fs::path want = fs::temp_directory_path(ec) / "SupermanRecomp" /
                          ("content-xamc_test-" + std::to_string(GetCurrentProcessId()));
    std::printf("       T18: instance root=%s\n", reinterpret_cast<const char*>(r.u8string().c_str()));
    check(r == want, "T18a SR_INSTANCE_ID set, SR_CONTENT_DIR unset -> %TEMP%/SupermanRecomp/content-<id>-<pid>");
    check(persistent != r && K::xamContentRoot() == g_root, "T18b neither set -> the persistent root; SR_CONTENT_DIR wins again");
  }

  // ── T19: XamEnumerate on an unknown handle = 6, synchronously, overlapped untouched ─
  {
    K::writeGuestBE32(kOvl, 0xAAAAAAAAu);
    const uint32_t rc = exportCall("XamEnumerate", 0x0007DFFFu, 0, kEnumBuf, 0x134, 0, kOvl);
    check(rc == 6u && K::readGuestU32BE(kOvl) == 0xAAAAAAAAu,
          "T19 XamEnumerate(unknown handle, ovl) = 6 without completing the overlapped (Xenia)");
  }

  // ── T20: XamContentSetThumbnail bounds before allocating ──────────────────
  {
    saveXcd();
    check(exportCall("XamContentSetThumbnail", 0, kXcd, kBuf, 0xFFFFFFFFu, 0) == 0x57u,
          "T20a SetThumbnail(size 0xFFFFFFFF) = 0x57 (rejected before any allocation)");
    const uint32_t rc = exportCall("XamContentSetThumbnail", 0, kXcd, kBuf, 16, 0);
    check(rc == 0u && readHost(pkgDir() / "__thumbnail.png").size() == 16u,
          "T20b SetThumbnail(16 bytes) = 0, __thumbnail.png written");
  }

  // ── T14: attribution - the same calls with the gate OFF show the defect ────
  {
    K::xamContentFsSetEnabledForTest(0);
    saveXcd("MutSave");
    const uint32_t rc = mount(2);
    const bool dir = fs::exists(pkgDir("MutSave"), ec);
    const uint32_t h = createFile("rmcsave:\\MutSave", 0xC0000000u, 2);
    saveXcd();
    const EnumRes r = enumerateOnce(1);
    zero(kMgr, 0x400);
    K::writeGuestBE32(kMgr + 0x360, 1);
    const uint32_t ff = guest(sub_82645510, kMgr, kOutSlot);
    K::xamContentFsSetEnabledForTest(-1);
    std::printf("       T14m (gate OFF): mount=%u dir=%d h=%08X enum rc=%u n=%u FindFirst=%u\n", rc,
                dir ? 1 : 0, h, r.rc1, r.n1, ff);
    check(rc == 0u && !dir, "T14m-a gate OFF: XamContentCreateEx 'succeeds' but creates nothing (generic stub)");
    check(h == 0xFFFFFFFFu, "T14m-b gate OFF: CreateFileA(rmcsave:...) fails (FILE_NOT_FOUND, the run.log shape)");
    check(r.rc1 == 0x12u && r.n1 == 0u, "T14m-c gate OFF: an existing save enumerates as NO_MORE_FILES");
    check(ff == 12u, "T14m-d gate OFF: SaveContentFindFirst = 12 'no saves' although one exists");

    K::ntStatusDosErrSetEnabledForTest(0);
    mount(3);
    const uint32_t hm = createFile("rmcsave:\\missing.bin", 0x80000000u, 3);
    const uint32_t g = gle();
    unmount();
    K::ntStatusDosErrSetEnabledForTest(dosMode);
    std::printf("       T14m (DOSERR OFF): h=%08X gle=%u\n", hm, g);
    check(hm == 0xFFFFFFFFu && g == 0u, "T14m-e SR_NTSTATUS_DOSERR OFF: GetLastError is blind (0, not 2)");
    check(K::ntStatusToDosError(0x80000006u) == 0x12u && K::ntStatusToDosError(0xC0000034u) == 2u &&
              K::ntStatusToDosError(0xC000003Au) == 3u && K::ntStatusToDosError(0xC0000035u) == 0xB7u &&
              K::ntStatusToDosError(0xC0000033u) == 0x7Bu && K::ntStatusToDosError(0xC0000022u) == 5u &&
              K::ntStatusToDosError(0x80070057u) == 0x57u && K::ntStatusToDosError(0xC0010005u) == 5u &&
              K::ntStatusToDosError(0xDEAD0000u) == 317u,
          "T15 RtlNtStatusToDosError table (Xenia xboxkrnl_error.cc)");
  }

  // ── TDV (iter-463 phase 2): dev-tool taint FOLLOWS SAVES (DEVTOOLS_DESIGN 3.1 A5/B7), through
  // the real guest wrappers: the marker is keyed on the actual content write, sits BESIDE the
  // package folder (never inside it, so it cannot travel back to Xenia with an export), and a
  // run that LOADs a marked package is tainted - its next (auto)save inherits the marker.
  if (gateOn) {
    size_t nTools = 0;
    const K::DevToolInfo* tools = K::devToolTable(&nTools);
    uint32_t levelskipBit = 0;
    for (size_t i = 0; i < nTools; ++i)
      if (std::string(tools[i].name) == "levelskip") levelskipBit = 1u << i;
    const fs::path marker = K::devToolsMarkerPath(pkgDir());
    check(marker == pkgDir().parent_path() / (std::string(kFile) + ".devtool"),
          "TDV marker = <TYPE dir>/<package>.devtool, beside the package folder");
    K::devToolsResetForTest(0);
    fs::remove(marker, ec);
    check(guestSave(0x31) && !fs::exists(marker), "TDVa an untainted (natural) run's save carries NO marker");
    K::devToolsResetForTest(levelskipBit);
    check(guestSave(0x32) && fs::exists(marker) && !fs::exists(pkgDir() / (std::string(kFile) + ".devtool")),
          "TDVb a dev-tool-assisted run's save gets the marker beside (not inside) the package");
    K::devToolsResetForTest(0);
    check(!K::devToolsTainted(), "TDVc a fresh natural run starts untainted");
    const uint32_t lrc = mount(3);   // LOAD: OPEN_EXISTING
    unmount();
    check(lrc == 0u && K::devToolsTainted(), "TDVd LOAD of a marked package taints the run (devtool-derived-save)");
    fs::remove(marker, ec);
    check(guestSave(0x33) && fs::exists(marker), "TDVe the tainted run's next save (the autosave) inherits the marker");
    K::devToolsResetForTest(0);
    fs::remove(marker, ec);
    mount(3);
    unmount();
    check(!K::devToolsTainted(), "TDVf LOAD of an unmarked package leaves the run untainted");
  }

#ifdef _WIN32
  if (!std::getenv("SR_XAMC_KEEP")) fs::remove_all(g_root, ec);
#endif
  std::printf("[XAMC_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
