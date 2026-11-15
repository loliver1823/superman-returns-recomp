// iter-463 PLATFORM FIX: XAM content packages (real save storage) + the NT
// file calls inside a mounted package + RtlNtStatusToDosError.
// Contract, gates, host layout and references: kernel/xam_content.hpp.
//
// Ported from xenia-canary @0b4e6c4 (BSD-3; Copyright Ben Vanik and Xenia
// contributors): xam/xam_content.cc, xam/content_manager.cc, xam/xam_enum.cc,
// kernel/xenumerator.cc, kernel/xfile.cc, vfs/virtual_file_system.cc,
// xboxkrnl/xboxkrnl_io.cc and xboxkrnl/xboxkrnl_error.cc (the status table
// below is copied verbatim from xboxkrnl_error.cc:21-966).
//
// Everything runs synchronously on the calling guest thread; no guest calls,
// no side threads. Guest memory goes through the guest_context helpers with a
// range check first; host file-system calls use the error_code overloads only.
#include "xam_content.hpp"
#include "achievements.hpp"  // step5 E14a: the save package for the sidecar
#include "hitch_log.hpp"  // iter-466 stutter: SR_HITCH_LOG (observe-only)

#include "devtools.hpp"
#include "export_registry.hpp"
#include "file_io.hpp"
#include "guest_context.hpp"
#include "mod_overlay.hpp"   // step4/audit batch 3 SR_IMPORT_FIX_ATTRS: a file only a mod provides exists
#include "../app/log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#endif

namespace sr {
namespace kernel {
bool asyncReadPendingMapped();  // kernel/stubs.cpp (iter-466 stutter, SR_ASYNC_READ)

namespace {
namespace fs = std::filesystem;

// ── gates ────────────────────────────────────────────────────────────────────
std::atomic<int> g_fsTestMode{-1};    // -1 env, 0 off, 1 on
std::atomic<int> g_dosTestMode{-1};

bool envDefaultOn(const char* name) {
  const char* e = std::getenv(name);
  return !e || e[0] != '0';
}
bool fsEnvOn() {
  static const bool v = envDefaultOn("SR_XAM_CONTENT_FS");
  return v;
}
// SR_NTSTATUS_DOSERR: BAKED default ON 2026-10-18 (step4/audit fix batch 1, docs/LOGIC_AUDIT.md 2.0d), =0 = the
// generic answer (0). It changes GetLastError for the whole title (disc FindFirst/FindNext, optional-file misses at
// boot), not just saves, so it waited for natural A/B rolls (iter-463 review): ROG flicker gate + R0-R2 with the batch.
bool dosEnvOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NTSTATUS_DOSERR");
    return !e || e[0] != '0';
  }();
  return v;
}

// ── constants ────────────────────────────────────────────────────────────────
constexpr uint32_t kXcdSize = 0x134;   // XCONTENT_DATA
constexpr uint32_t kAggSize = 0x148;   // XCONTENT_AGGREGATE_DATA
constexpr uint32_t kOffDisplayName = 0x08, kOffFileName = 0x108, kOffPadding = 0x132;
// XCONTENT_AGGREGATE_DATA = XCONTENT_DATA (0x134) + be<u64> xuid (8-aligned:
// 0x138) + be<u32> title_id (0x140), size 0x148 (Xenia content_manager.h;
// XCONTENT_DATA_INTERNAL documents the same offsets). 0x134..0x137 and
// 0x144..0x147 are padding, kept zero.
constexpr uint32_t kOffXuid = 0x138, kOffTitle = 0x140;
constexpr uint32_t kHeaderSizeLicensed = kAggSize + 4;   // older writers append a licence word
constexpr uint32_t kFileNameMax = 42;
constexpr uint32_t kCurrentTitle = 0xFFFFFFFFu;
constexpr uint32_t kTypeSavedGame = 1, kTypeMarketplace = 2;
constexpr uint32_t kDeviceHdd = 1, kDeviceOdd = 2;
constexpr uint32_t kFlagExcludeCommon = 0x1000;
constexpr uint32_t kEnumHandleBase = 0x0007E000u;   // < 0x100000 (NtReadFile's handle heuristic)
constexpr uint32_t kDirInfoHeader = 64;              // X_FILE_DIRECTORY_INFORMATION.file_name
constexpr uint32_t kDirInfoMinLength = 72;
const char* const kThumbnailFileName = "__thumbnail.png";

// Win32 / X_ERROR
constexpr uint32_t kErrSuccess = 0, kErrFileNotFound = 2, kErrPathNotFound = 3;
constexpr uint32_t kErrAccessDenied = 5, kErrInvalidHandle = 6, kErrNoMoreFiles = 0x12;
constexpr uint32_t kErrInvalidParameter = 0x57, kErrInvalidName = 0x7B;
constexpr uint32_t kErrAlreadyExists = 0xB7, kErrIoPending = 997;
constexpr uint32_t kErrNoSuchUser = 0x525, kErrFunctionFailed = 0x65B;
constexpr uint32_t kXeInvalidArg = 0x80070057u;
// NTSTATUS
constexpr uint32_t kStSuccess = 0, kStNoMoreFiles = 0x80000006u;
constexpr uint32_t kStInvalidInfoClass = 0xC0000003u, kStInfoLengthMismatch = 0xC0000004u;
constexpr uint32_t kStInvalidHandle = 0xC0000008u, kStInvalidParameter = 0xC000000Du;
constexpr uint32_t kStNoSuchFile = 0xC000000Fu, kStAccessDenied = 0xC0000022u;
constexpr uint32_t kStNameInvalid = 0xC0000033u, kStNameNotFound = 0xC0000034u;
constexpr uint32_t kStNameCollision = 0xC0000035u, kStPathNotFound = 0xC000003Au;
constexpr uint32_t kStDiskFull = 0xC000007Fu, kStNoResources = 0xC000009Au;
constexpr uint32_t kStFileIsDirectory = 0xC00000BAu;
// NT create options / file actions (Xenia vfs::FileAction)
constexpr uint32_t kOptDirectoryFile = 0x1, kOptNonDirectoryFile = 0x40, kOptDeleteOnClose = 0x1000;
constexpr uint32_t kActSuperseded = 0, kActOpened = 1, kActCreated = 2, kActOverwritten = 3;
constexpr uint32_t kActExists = 4, kActDoesNotExist = 5;

// ── logging (first 16 per op, then powers of two) ────────────────────────────
enum Op {
  kOpCreate, kOpClose, kOpFlush, kOpDelete, kOpCreator, kOpThumb, kOpEnumCreate,
  kOpEnumerate, kOpNtCreate, kOpNtWrite, kOpNtFlush, kOpNtSetInfo, kOpNtQueryDir,
  kOpNtClose, kOpNtFullAttr, kOpDosErr, kOpRoot, kOpCount
};
std::atomic<uint32_t> g_opCount[kOpCount];
uint32_t logN(Op op) {
  const uint32_t n = g_opCount[op].fetch_add(1, std::memory_order_relaxed) + 1;
  return (n <= 16 || (n & (n - 1)) == 0) ? n : 0;
}
#define XAMC_LOG(op, fmt, ...)                                                     \
  do {                                                                             \
    if (const uint32_t n_ = logN(op))                                              \
      SR_LOG_BOOT(sr::LogSeverity::Info, "[XAM_CONTENT] #%u " fmt, n_, __VA_ARGS__); \
  } while (0)

// ── guest memory (range-checked) ─────────────────────────────────────────────
bool rangeOk(uint32_t va, uint32_t len) {
  if (va == 0) return false;
  const uint64_t end = uint64_t(va) + (len ? len - 1 : 0);
  if (end > 0xFFFFFFFFull) return false;
  return isGuestVAInRange(va) && isGuestVAInRange(uint32_t(end));
}
void wr32(uint32_t va, uint32_t v) {
  if (va && isGuestVAInRange(va)) writeGuestBE32(va, v);
}
void wr64(uint32_t va, uint64_t v) {
  wr32(va, uint32_t(v >> 32));
  wr32(va + 4, uint32_t(v));
}
uint32_t rd32(uint32_t va) { return (va && isGuestVAInRange(va)) ? readGuestU32BE(va) : 0; }
bool readBytes(uint32_t va, void* dst, uint32_t n) {
  if (!rangeOk(va, n)) return false;
  const uint8_t* p = guestVAToHost(va);
  if (!p) return false;
  std::memcpy(dst, p, n);
  return true;
}
bool writeBytes(uint32_t va, const void* src, uint32_t n) {
  if (!rangeOk(va, n)) return false;
  uint8_t* p = guestVAToHost(va);
  if (!p) return false;
  std::memcpy(p, src, n);
  return true;
}
std::string readCStr(uint32_t va, size_t maxLen) {
  std::string s;
  for (size_t i = 0; va && i < maxLen; ++i) {
    const uint32_t a = va + uint32_t(i);
    if (!isGuestVAInRange(a)) break;
    const uint8_t* p = guestVAToHost(a);
    if (!p || *p == 0) break;
    s.push_back(char(*p));
  }
  return s;
}
// The 9th argument (first stack slot, low word of the 64-bit slot) at import
// entry: [r1 + 0x54] (precedent: cs_timeout_underflow_fix.cpp HLSLS capture).
uint32_t stackArg9() {
  const uint32_t r1 = getCurrentGuestR1();
  if (r1 == 0) return 0;
  const uint32_t va = r1 + 0x54u;
  return isGuestVAInRange(va) ? readGuestU32BE(va) : 0;
}

uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void putBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
void putBe64(uint8_t* p, uint64_t v) {
  putBe32(p, uint32_t(v >> 32));
  putBe32(p + 4, uint32_t(v));
}

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(uint8_t(c)));
  return s;
}
std::string trim(const std::string& s) {
  const char* ws = " \t\r\n";
  const size_t b = s.find_first_not_of(ws);
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(ws) - b + 1);
}
std::string u8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}
std::string hex(uint64_t v, int digits) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%0*llX", digits, static_cast<unsigned long long>(v));
  return buf;
}

// ── XCONTENT_(AGGREGATE_)DATA, kept as raw big-endian bytes ─────────────────
struct Xcd {
  std::array<uint8_t, kAggSize> raw{};
  uint32_t device() const { return be32(raw.data()); }
  uint32_t type() const { return be32(raw.data() + 4); }
  uint32_t title() const { return be32(raw.data() + kOffTitle); }
  uint64_t xuid() const {
    return (uint64_t(be32(raw.data() + kOffXuid)) << 32) | be32(raw.data() + kOffXuid + 4);
  }
  // Xenia XCONTENT_DATA::file_name(): at most 42 chars, may lack a NUL.
  std::string fileName() const {
    std::string s;
    for (uint32_t i = 0; i < kFileNameMax && raw[kOffFileName + i]; ++i)
      s.push_back(char(raw[kOffFileName + i]));
    return s;
  }
};

// XCONTENT_AGGREGATE_DATA(const XCONTENT_DATA&): display name re-set (zero
// after the first NUL, always terminated), file name re-set (zero-filled),
// padding 0, xuid 0, title = the running title marker.
bool readXcd(uint32_t va, Xcd& d) {
  d.raw.fill(0);
  if (!readBytes(va, d.raw.data(), kXcdSize)) return false;
  bool ended = false;
  for (uint32_t i = 0; i < 128; ++i) {
    uint8_t* c = d.raw.data() + kOffDisplayName + 2 * i;
    if (ended) { c[0] = c[1] = 0; continue; }
    if (c[0] == 0 && c[1] == 0) ended = true;
  }
  if (!ended) d.raw[kOffDisplayName + 254] = d.raw[kOffDisplayName + 255] = 0;
  ended = false;
  for (uint32_t i = 0; i < kFileNameMax; ++i) {
    if (ended) d.raw[kOffFileName + i] = 0;
    else if (d.raw[kOffFileName + i] == 0) ended = true;
  }
  d.raw[kOffPadding] = d.raw[kOffPadding + 1] = 0;
  putBe64(d.raw.data() + kOffXuid, 0);
  putBe32(d.raw.data() + kOffTitle, kCurrentTitle);
  return true;
}

void setDisplayName(Xcd& d, const std::u16string& name) {
  for (uint32_t i = 0; i < 128; ++i) {
    const char16_t c = i < 127 && i < name.size() ? name[i] : 0;
    d.raw[kOffDisplayName + 2 * i] = uint8_t(uint16_t(c) >> 8);
    d.raw[kOffDisplayName + 2 * i + 1] = uint8_t(c);
  }
}
void setFileName(Xcd& d, const std::string& name) {
  for (uint32_t i = 0; i < kFileNameMax; ++i)
    d.raw[kOffFileName + i] = i < name.size() ? uint8_t(name[i]) : 0;
  d.raw[kOffPadding] = d.raw[kOffPadding + 1] = 0;
}

// A single host path component the guest named. Beyond Xenia (which trusts the
// name) this keeps guest-controlled names inside the package: no '.'/'..', no
// separators, drive or stream colons, wildcards, control chars, trailing
// space/dot (Windows would silently strip them) or DOS device names.
bool validHostComponent(const std::string& s) {
  if (s.empty() || s.size() > 255 || s == "." || s == "..") return false;
  for (char ch : s) {
    const uint8_t c = uint8_t(ch);
    if (c < 0x20 || c >= 0x7F) return false;
    if (std::strchr("<>:\"/\\|?*", ch)) return false;
  }
  if (s.back() == ' ' || s.back() == '.') return false;
  std::string base = s.substr(0, s.find('.'));
  for (char& c : base) c = char(std::toupper(uint8_t(c)));
  static const char* const kDos[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3",
                                     "COM4", "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1",
                                     "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8",
                                     "LPT9"};
  for (const char* dn : kDos)
    if (base == dn) return false;
  return true;
}
// Xenia IsValidPath(s, false) character set, per component.
bool xeniaValidPathChars(const std::string& s, bool isPattern) {
  for (char ch : s) {
    const uint8_t c = uint8_t(ch);
    if (c <= 31 || c >= 127) return false;
    if (ch == '"' || ch == '+' || ch == ',' || ch == '<' || ch == '>' || ch == '|') return false;
    if (!isPattern && (ch == '*' || ch == '?')) return false;
  }
  return true;
}

// Case-insensitive NT-style wildcard: '*' any run, '?' one char. "*" and "*.*"
// match everything (FsRtlIsNameInExpression's DOS "*.*" convention).
bool wildcardMatch(const std::string& pattern, const std::string& name) {
  if (pattern.empty() || pattern == "*" || pattern == "*.*") return true;
  const std::string p = lower(pattern), n = lower(name);
  size_t pi = 0, ni = 0, star = std::string::npos, mark = 0;
  while (ni < n.size()) {
    if (pi < p.size() && (p[pi] == '?' || p[pi] == n[ni])) { ++pi; ++ni; }
    else if (pi < p.size() && p[pi] == '*') { star = pi++; mark = ni; }
    else if (star != std::string::npos) { pi = star + 1; ni = ++mark; }
    else return false;
  }
  while (pi < p.size() && p[pi] == '*') ++pi;
  return pi == p.size();
}

// ── host helpers ─────────────────────────────────────────────────────────────
struct HostStat {
  bool exists = false, isDir = false;
  uint64_t size = 0, ctime = 0, atime = 0, mtime = 0;   // FILETIMEs
};
HostStat hostStat(const fs::path& p) {
  HostStat s;
#ifdef _WIN32
  WIN32_FILE_ATTRIBUTE_DATA a{};
  if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return s;
  auto ft = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  s.exists = true;
  s.isDir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  s.size = s.isDir ? 0 : (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
  s.ctime = ft(a.ftCreationTime);
  s.atime = ft(a.ftLastAccessTime);
  s.mtime = ft(a.ftLastWriteTime);
#else
  std::error_code ec;
  const auto st = fs::status(p, ec);
  if (ec || !fs::exists(st)) return s;
  s.exists = true;
  s.isDir = fs::is_directory(st);
  if (!s.isDir) s.size = fs::file_size(p, ec);
#endif
  return s;
}

std::FILE* hostOpen(const fs::path& p, const char* mode) {
#ifdef _WIN32
  wchar_t wmode[8] = {};
  for (size_t i = 0; mode[i] && i < 7; ++i) wmode[i] = wchar_t(mode[i]);
  std::FILE* f = nullptr;
  return _wfopen_s(&f, p.c_str(), wmode) == 0 ? f : nullptr;
#else
  return std::fopen(p.c_str(), mode);
#endif
}

bool writeWholeFile(const fs::path& p, const uint8_t* data, size_t n) {
  // Write-then-rename so a crash mid-write never leaves a torn file.
  fs::path tmp = p;
  tmp += ".tmp";
  std::FILE* f = hostOpen(tmp, "wb");
  if (!f) return false;
  const bool ok = (n == 0 || std::fwrite(data, 1, n, f) == n) && std::fflush(f) == 0;
  std::fclose(f);
  std::error_code ec;
  if (!ok) { fs::remove(tmp, ec); return false; }
  fs::rename(tmp, p, ec);
  if (ec) { fs::remove(tmp, ec); return false; }
  return true;
}

// ── module state (one mutex; lock order: g_xcMutex, then file_io's) ─────────
struct Mount {
  std::string rootName;   // as the guest spelled it
  fs::path pkg;
  Xcd data;               // the XCONTENT data it was mounted with (IsContentOpen)
  uint32_t license = 0;
};
struct DirEnt {
  std::string name;
  HostStat st;
};
struct XFile {
  fs::path host;
  std::string rootKey;
  bool isDir = false, writable = false, deleteOnClose = false;
  // Directory enumeration (Xenia XFile::QueryDirectory find engine).
  bool listed = false;
  std::vector<DirEnt> ents;
  size_t next = 0;
  std::string rule;       // "" = match all
};
struct Enum {
  std::vector<std::array<uint8_t, kXcdSize>> items;
  size_t next = 0;
  uint32_t perCall = 1;
};

std::mutex g_xcMutex;
std::unordered_map<std::string, Mount> g_mounts;       // key: lowercase root name
std::unordered_set<std::string> g_knownRoots;          // ever mounted (lowercase)
std::unordered_map<uint32_t, XFile> g_files;
std::unordered_map<uint32_t, Enum> g_enums;
uint32_t g_nextEnum = kEnumHandleBase;
std::atomic<uint32_t> g_liveCount{0};
std::atomic<bool> g_anyRootKnown{false};   // lock-free fast path for the NT hooks
std::string g_lastRootLogged;

void updateLive() { g_liveCount.store(uint32_t(g_files.size() + g_enums.size())); }

fs::path contentRootLocked() {
  fs::path r = xamContentRoot();
  const std::string s = u8(r);
  if (s != g_lastRootLogged) {
    g_lastRootLogged = s;
    XAMC_LOG(kOpRoot, "content root = \"%s\"", s.c_str());
  }
  return r;
}

fs::path packageRoot(const fs::path& root, uint64_t xuid, uint32_t title, uint32_t type) {
  if (title == kCurrentTitle) title = kXamContentTitleId;
  return root / hex(xuid, 16) / hex(title, 8) / hex(type, 8);
}
// ContentManager::ResolvePackagePath (non-publisher types).
fs::path packagePath(const fs::path& root, uint64_t xuid, const Xcd& d) {
  uint64_t used = (d.xuid() != ~0ull && d.xuid() != 0) ? d.xuid() : xuid;
  if (d.type() == kTypeMarketplace) used = 0;
  return packageRoot(root, used, d.title(), d.type()) / trim(d.fileName());
}
fs::path headerPath(const fs::path& root, const std::string& fileName, uint64_t xuid,
                    uint32_t title, uint32_t type) {
  if (title == kCurrentTitle) title = kXamContentTitleId;
  if (type == kTypeMarketplace) xuid = 0;
  return root / hex(xuid, 16) / hex(title, 8) / "Headers" / hex(type, 8) /
         (trim(fileName) + ".header");
}
fs::path backupPath(const fs::path& root, uint64_t xuid, const Xcd& d) {
  uint64_t used = (d.xuid() != ~0ull && d.xuid() != 0) ? d.xuid() : xuid;
  if (d.type() == kTypeMarketplace) used = 0;
  uint32_t title = d.title() == kCurrentTitle ? kXamContentTitleId : d.title();
  return root / ".backup" / hex(used, 16) / hex(title, 8) / hex(d.type(), 8) / trim(d.fileName());
}

bool sameContent(const Xcd& a, const Xcd& b) {   // XCONTENT_AGGREGATE_DATA::operator==
  return a.device() == b.device() && a.title() == b.title() && a.type() == b.type() &&
         a.fileName() == b.fileName();
}
bool isContentOpenLocked(const Xcd& d) {
  for (const auto& kv : g_mounts)
    if (sameContent(kv.second.data, d)) return true;
  return false;
}

// ContentManager::WriteContentHeaderFile.
void writeHeaderLocked(const fs::path& root, uint64_t xuid, Xcd d) {
  if (d.title() == kCurrentTitle) putBe32(d.raw.data() + kOffTitle, kXamContentTitleId);
  if (d.xuid() == ~0ull) putBe64(d.raw.data() + kOffXuid, xuid);
  const uint64_t used = (d.xuid() != ~0ull && d.xuid() != 0) ? d.xuid() : xuid;
  const fs::path hp = headerPath(root, d.fileName(), used, d.title(), d.type());
  std::error_code ec;
  fs::create_directories(hp.parent_path(), ec);
  const bool ok = writeWholeFile(hp, d.raw.data(), kAggSize);
  XAMC_LOG(kOpCreate, "header \"%s\" %s", u8(hp).c_str(), ok ? "written" : "WRITE FAILED");
}
// Reads a .header. Beyond Xenia (which trusts any file of >= 0x148 bytes):
// only an exact 0x148 (Xenia) or 0x14C (+ licence word) byte file is a header.
bool readHeader(const fs::path& hp, Xcd& d, uint32_t* license) {
  std::FILE* f = hostOpen(hp, "rb");
  if (!f) return false;
  uint8_t buf[kHeaderSizeLicensed + 1];
  const size_t n = std::fread(buf, 1, sizeof(buf), f);
  std::fclose(f);
  if (n != kAggSize && n != kHeaderSizeLicensed) return false;
  std::memcpy(d.raw.data(), buf, kAggSize);
  if (license) *license = n == kHeaderSizeLicensed ? be32(buf + kAggSize) : 0;
  return true;
}

// A header read from disk is handed to the guest's enumeration consumers, and
// sub_8263FEE0 runs Str_WideToAsciiEscaped_sub_82642388 (no bound: copies
// until a NUL, 6 bytes per wchar >= 0x100) into a 32-byte stack buffer. So a
// header is listed only when it is structurally this package's: display name
// NUL-terminated within 128 wchars (zero-filled after, as readXcd), file name
// == the folder name, content type == the listed type. Anything else (bit rot,
// a zeroed/0xFF file, another tool's layout, a renamed folder) is ignored and
// the folder-name fallback describes the package instead.
bool headerMatchesFolder(Xcd& d, const std::string& folder, uint32_t type) {
  bool ended = false;
  for (uint32_t i = 0; i < 128; ++i) {
    uint8_t* c = d.raw.data() + kOffDisplayName + 2 * i;
    if (ended) { c[0] = c[1] = 0; continue; }
    if (c[0] == 0 && c[1] == 0) ended = true;
  }
  if (!ended) return false;
  ended = false;
  for (uint32_t i = 0; i < kFileNameMax; ++i) {
    if (ended) d.raw[kOffFileName + i] = 0;
    else if (d.raw[kOffFileName + i] == 0) ended = true;
  }
  d.raw[kOffPadding] = d.raw[kOffPadding + 1] = 0;
  return d.type() == type && trim(d.fileName()) == folder;
}

// Apply a package file handle's delete-on-close, closing it through file_io
// first unless the caller already did. stub_NtClose has already run
// fileHandleClose(h), and a second close is NOT a no-op: file_io retries a
// missed handle as bswap32(h) and could fclose an unrelated live handle. The
// caller erases the entry.
void releaseFileLocked(uint32_t h, XFile& xf, bool alreadyClosed) {
  if (!alreadyClosed) fileHandleClose(h);
  if (xf.deleteOnClose) {
    std::error_code ec;
    fs::remove(xf.host, ec);   // a directory is removed only when empty
    XAMC_LOG(kOpNtClose, "h=%u delete-on-close \"%s\" -> %s", h, u8(xf.host).c_str(),
             ec ? ec.message().c_str() : "removed");
  }
}

// ContentManager::DeleteContent. `backup` (CREATE_ALWAYS / TRUNCATE_EXISTING
// replacing a package): the old package is MOVED to root/.backup/... (one
// generation, host-only, outside the enumerated tree) instead of destroyed.
// The guest never reads it back: it is a manual recovery copy for a crash
// between the replace and the guest's rewrite. Order: move the live package to
// a fresh staging name first; only once that succeeded is the previous backup
// dropped. A failed move (a file in the package held open by another process)
// leaves the live package AND the previous backup untouched and never falls
// through to remove_all - the create that follows then fails with 0xB7.
uint32_t deleteContentLocked(const fs::path& root, uint64_t xuid, const Xcd& d, bool backup) {
  if (isContentOpenLocked(d)) return kErrAccessDenied;
  const fs::path pkg = packagePath(root, xuid, d);
  std::error_code ec;
  if (backup && fs::exists(pkg, ec)) {
    const fs::path bk = backupPath(root, xuid, d);
    fs::path staging = bk;
    staging += ".staging";
    fs::remove_all(staging, ec);   // a leftover of an interrupted replace, never live data
    ec.clear();
    fs::create_directories(bk.parent_path(), ec);
    ec.clear();
    fs::rename(pkg, staging, ec);
    if (ec) {
      XAMC_LOG(kOpDelete, "backup \"%s\" -> \"%s\" FAILED (%s): package kept, nothing deleted",
               u8(pkg).c_str(), u8(staging).c_str(), ec.message().c_str());
      return kErrAccessDenied;
    }
    std::error_code ec2;
    fs::remove_all(bk, ec2);
    fs::rename(staging, bk, ec2);
    XAMC_LOG(kOpDelete, "backup \"%s\" -> \"%s\" %s", u8(pkg).c_str(),
             u8(ec2 ? staging : bk).c_str(), ec2 ? "(kept under the staging name)" : "ok");
    return kErrSuccess;
  }
  ec.clear();
  const auto removed = fs::remove_all(pkg, ec);
  return (!ec && removed != static_cast<std::uintmax_t>(-1) && removed > 0) ? kErrSuccess
                                                                           : kErrFileNotFound;
}

void mountLocked(const std::string& key, const std::string& rootName, const fs::path& pkg,
                 const Xcd& d, uint32_t license) {
  Mount m;
  m.rootName = rootName;
  m.pkg = pkg;
  m.data = d;
  m.license = license;
  g_mounts[key] = std::move(m);
  g_knownRoots.insert(key);
  g_anyRootKnown.store(true);
}

uint32_t createContentLocked(const fs::path& root, const std::string& key,
                             const std::string& rootName, uint64_t xuid, const Xcd& d) {
  if (g_mounts.count(key)) return kErrAlreadyExists;
  const fs::path pkg = packagePath(root, xuid, d);
  std::error_code ec;
  if (fs::exists(pkg, ec)) return kErrAlreadyExists;
  if (!fs::create_directories(pkg, ec) || ec) return kErrAccessDenied;
  mountLocked(key, rootName, pkg, d, 0);
  return kErrSuccess;
}

uint32_t openContentLocked(const fs::path& root, const std::string& key,
                           const std::string& rootName, uint64_t xuid, const Xcd& d,
                           uint32_t* license) {
  if (g_mounts.count(key)) return kErrAlreadyExists;
  const fs::path pkg = packagePath(root, xuid, d);
  std::error_code ec;
  if (!fs::exists(pkg, ec)) return kErrFileNotFound;
  uint32_t lic = 0;
  Xcd hdr;
  readHeader(headerPath(root, d.fileName(), xuid, kXamContentTitleId, d.type()), hdr, &lic);
  *license = lic;
  mountLocked(key, rootName, pkg, d, lic);
  return kErrSuccess;
}

uint32_t closeContentLocked(const std::string& key) {
  auto it = g_mounts.find(key);
  if (it == g_mounts.end()) return kErrFileNotFound;
  // CloseOpenedFilesFromContent: every open file handle under the root.
  for (auto fit = g_files.begin(); fit != g_files.end();) {
    if (fit->second.rootKey == key) {
      releaseFileLocked(fit->first, fit->second, /*alreadyClosed=*/false);
      fit = g_files.erase(fit);
    } else {
      ++fit;
    }
  }
  g_mounts.erase(it);
  updateLive();
  return kErrSuccess;
}

// ContentManager::ListContent (one xuid, the running title, one type).
void listContentLocked(const fs::path& root, uint32_t device, uint64_t xuid, uint32_t type,
                       std::vector<Xcd>& out) {
  const fs::path pr = packageRoot(root, xuid, kXamContentTitleId, type);
  std::error_code ec;
  std::vector<fs::path> dirs;
  for (fs::directory_iterator it(pr, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code ec2;
    if (it->is_directory(ec2) && !ec2) dirs.push_back(it->path());
  }
  std::sort(dirs.begin(), dirs.end());
  for (const fs::path& dir : dirs) {
    const fs::path fn = dir.filename();
    const std::string name = u8(fn);
    // Beyond Xenia: a folder that cannot round-trip through XCONTENT_DATA
    // (file_name holds 42 chars) or that the NT hooks would refuse is skipped.
    if (name.size() > kFileNameMax || !validHostComponent(name)) {
      XAMC_LOG(kOpEnumCreate, "skipped content folder \"%s\" (not a valid package name)",
               u8(dir).c_str());
      continue;
    }
    Xcd d;
    const fs::path hp = headerPath(root, name, xuid, kXamContentTitleId, type);
    if (readHeader(hp, d, nullptr)) {
      if (headerMatchesFolder(d, name, type)) {
        out.push_back(d);
        continue;
      }
      XAMC_LOG(kOpEnumCreate, "ignored malformed header \"%s\" (folder-name fallback used)",
               u8(hp).c_str());
      d = Xcd{};
    }
    putBe32(d.raw.data(), device);
    putBe32(d.raw.data() + 4, type);
    setDisplayName(d, fn.u16string());
    setFileName(d, name);
    putBe32(d.raw.data() + kOffTitle, kXamContentTitleId);
    putBe64(d.raw.data() + kOffXuid, xuid);
    out.push_back(d);
  }
}

// Xenia CompleteOverlappedImmediate / CompleteOverlappedDeferredEx, inline.
// XOVERLAPPED: +0 result, +4 length, +8 context, +0xC hEvent, +0x10 routine,
// +0x18 extended error. Events/routines are not signalled (this title passes
// no overlapped to any content call; logged if one ever appears).
uint32_t hresultFromWin32(uint32_t x) {
  return int32_t(x) <= 0 ? x : ((x & 0xFFFFu) | 0x80070000u);
}
void ovlComplete(uint32_t ovl, uint32_t result, uint32_t ext, uint32_t length, bool immediate) {
  if (!rangeOk(ovl, 0x1C)) return;
  if (immediate) wr32(ovl + 0x08, getCurrentThreadHandle());
  wr32(ovl + 0x04, length);
  wr32(ovl + 0x18, ext);
  wr32(ovl + 0x00, result);   // last: guests poll [0] != 997
  const uint32_t ev = rd32(ovl + 0x0C), routine = rd32(ovl + 0x10);
  if (ev || routine)
    XAMC_LOG(kOpFlush, "overlapped %08X hEvent=%08X routine=%08X not signalled", ovl, ev, routine);
}
void ovlImmediate(uint32_t ovl, uint32_t rc) {
  ovlComplete(ovl, rc, rc, rc ? 0xFFFFFFFFu : 0u, true);
}
void ovlDeferred(uint32_t ovl, uint32_t rc, uint32_t length) {
  ovlComplete(ovl, rc ? kErrFunctionFailed : kErrSuccess, hresultFromWin32(rc), length, false);
}

// ── previous exports (restored while the seam turns the gate off) ───────────
ExportFn g_prevCreateEx, g_prevClose, g_prevFlush, g_prevDelete, g_prevGetCreator,
    g_prevSetThumbnail, g_prevCreateEnum, g_prevEnumerate, g_prevNtWriteFile,
    g_prevNtFlush, g_prevNtFullAttr, g_prevDosErr;
uint32_t callPrev(ExportFn fn, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                  uint32_t r7, uint32_t r8) {
  return fn ? fn(r3, r4, r5, r6, r7, r8) : 0u;
}

// user index -> xuid, Xenia GetUserProfile: only user 0 is signed in here
// (stub_XamUserGetSigninState). Returns false for "no such user".
bool profileXuid(uint32_t user, uint64_t* xuid) {
  if (user != 0) return false;
  *xuid = kXamUser0Xuid;
  return true;
}

// ── exports ──────────────────────────────────────────────────────────────────
// XamContentCreateEx(user r3, root r4, data r5, flags r6, pDisp r7, pLicense r8,
//                    cacheSize r9, u64 contentSize r10, pOverlapped [r1+0x54]).
uint32_t exp_XamContentCreateEx(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                                uint32_t r7, uint32_t r8) {
  if (!xamContentFsEnabled()) return callPrev(g_prevCreateEx, r3, r4, r5, r6, r7, r8);
  const uint32_t user = r3, rootVA = r4, dataVA = r5, flags = r6, pDisp = r7, pLic = r8;
  const uint32_t ovl = stackArg9();
  Xcd d;
  if (!readXcd(dataVA, d)) return kErrInvalidParameter;
  if (d.raw[kOffFileName] == 0) return kErrInvalidName;
  uint64_t xuid = 0;
  if (user != 0xFEu && user != 0xFFu && !profileXuid(user, &xuid)) return kErrNoSuchUser;
  std::string rootName = readCStr(rootVA, 64);
  if (rootName.empty()) return kErrInvalidName;
  if (rootName.back() == ':') rootName.pop_back();
  const std::string key = lower(rootName);
  if (d.type() == kTypeMarketplace) xuid = 0;
  const std::string name = trim(d.fileName());
  // Beyond Xenia: the package name becomes a host directory name.
  if (key.empty() || !validHostComponent(name)) return kErrInvalidName;

  std::lock_guard<std::mutex> lk(g_xcMutex);
  // IsSymbolicLinkRegistered: a live mount, or the built-in disc links.
  if (g_mounts.count(key) || key == "game" || key == "d") return kErrInvalidParameter;
  if (ovl && pDisp) wr32(pDisp, 0);
  const fs::path root = contentRootLocked();
  const fs::path pkg = packagePath(root, xuid, d);
  std::error_code ec;
  const bool exists = fs::exists(pkg, ec);
  uint32_t rc = kErrInvalidParameter;
  uint32_t disp = 0;   // kDispositionState: 1 create, 2 open
  switch (flags & 0xF) {
    case 1:   // CREATE_NEW
      if (exists) rc = kErrAlreadyExists; else disp = 1;
      break;
    case 2:   // CREATE_ALWAYS
      if (exists) deleteContentLocked(root, xuid, d, true);
      disp = 1;
      break;
    case 3:   // OPEN_EXISTING
      if (!exists) rc = kErrPathNotFound; else disp = 2;
      break;
    case 4:   // OPEN_ALWAYS
      disp = exists ? 2 : 1;
      break;
    case 5:   // TRUNCATE_EXISTING
      if (!exists) { rc = kErrPathNotFound; }
      else { deleteContentLocked(root, xuid, d, true); disp = 1; }
      break;
    default:
      break;
  }
  uint32_t license = 0;
  if (disp == 1) {
    rc = createContentLocked(root, key, rootName, xuid, d);
    if (rc == kErrSuccess) writeHeaderLocked(root, xuid, d);
    // iter-463 phase 2 (DEVTOOLS_DESIGN 3.1 A5/B7): a tainted run's new package gets the
    // `.devtool` marker beside it. A no-op on an untainted run (the default).
    if (rc == kErrSuccess) devToolsOnSaveWrite(pkg);
    if (rc == kErrSuccess) sr::ach::notePackage(pkg);   // step5 E14a: the sidecar sits beside it
  } else if (disp == 2) {
    rc = openContentLocked(root, key, rootName, xuid, d, &license);
    // LOAD of a package carrying the marker taints this run (and its later saves).
    if (rc == kErrSuccess) devToolsOnSaveLoad(pkg);
    if (rc == kErrSuccess) sr::ach::notePackage(pkg);
  }
  if (pLic && rc == kErrSuccess) wr32(pLic, license);
  if (pDisp) wr32(pDisp, disp);
  XAMC_LOG(kOpCreate,
           "XamContentCreateEx(user=%u root='%s' dev=%u type=%u file='%s' flags=%08X ovl=%08X) "
           "-> rc=%u disp=%u pkg=\"%s\" lr=%08X",
           user, rootName.c_str(), d.device(), d.type(), name.c_str(), flags, ovl, rc, disp,
           u8(pkg).c_str(), getCurrentGuestLR());
  if (ovl) {
    ovlDeferred(ovl, rc, disp);
    return kErrIoPending;
  }
  return rc;
}

// XamContentClose(root r3, pOverlapped r4).
uint32_t exp_XamContentClose(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7,
                             uint32_t r8) {
  if (!xamContentFsEnabled()) return callPrev(g_prevClose, r3, r4, r5, r6, r7, r8);
  std::string rootName = readCStr(r3, 64);
  if (!rootName.empty() && rootName.back() == ':') rootName.pop_back();
  uint32_t rc;
  {
    std::lock_guard<std::mutex> lk(g_xcMutex);
    rc = closeContentLocked(lower(rootName));
  }
  XAMC_LOG(kOpClose, "XamContentClose('%s' ovl=%08X) -> rc=%u lr=%08X", rootName.c_str(), r4, rc,
           getCurrentGuestLR());
  if (r4) {
    ovlImmediate(r4, rc);
    return kErrIoPending;
  }
  return rc;
}

// XamContentFlush(root r3, pOverlapped r4): Xenia stub, success.
uint32_t exp_XamContentFlush(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7,
                             uint32_t r8) {
  if (!xamContentFsEnabled()) return callPrev(g_prevFlush, r3, r4, r5, r6, r7, r8);
  XAMC_LOG(kOpFlush, "XamContentFlush('%s' ovl=%08X) -> 0", readCStr(r3, 64).c_str(), r4);
  if (r4) {
    ovlImmediate(r4, kErrSuccess);
    return kErrIoPending;
  }
  return kErrSuccess;
}

// XamContentDelete(user r3, data r4, pOverlapped r5). Xenia keeps the .header.
uint32_t exp_XamContentDelete(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7,
                              uint32_t r8) {
  if (!xamContentFsEnabled()) return callPrev(g_prevDelete, r3, r4, r5, r6, r7, r8);
  Xcd d;
  if (!readXcd(r4, d)) return kErrInvalidParameter;
  uint64_t xuid = 0;
  if (r3 != 0xFEu && !profileXuid(r3, &xuid)) return kErrNoSuchUser;
  uint32_t rc;
  {
    std::lock_guard<std::mutex> lk(g_xcMutex);
    const std::string name = trim(d.fileName());
    rc = validHostComponent(name) ? deleteContentLocked(contentRootLocked(), xuid, d, false)
                                  : kErrFileNotFound;
  }
  XAMC_LOG(kOpDelete, "XamContentDelete(user=%u type=%u file='%s' ovl=%08X) -> rc=%u lr=%08X", r3,
           d.type(), d.fileName().c_str(), r5, rc, getCurrentGuestLR());
  if (r5) {
    ovlImmediate(r5, rc);
    return kErrIoPending;
  }
  return rc;
}

// XamContentGetCreator(user r3, data r4, pIsCreator r5, pXuid r6, pOverlapped r7).
uint32_t exp_XamContentGetCreator(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                                  uint32_t r7, uint32_t r8) {
  if (!xamContentFsEnabled()) return callPrev(g_prevGetCreator, r3, r4, r5, r6, r7, r8);
  if (!r5) return kErrInvalidParameter;
  uint64_t xuid = 0;
  if (!profileXuid(r3, &xuid)) return kErrNoSuchUser;
  Xcd d;
  if (!readXcd(r4, d)) return kErrInvalidParameter;
  uint32_t rc = kErrSuccess;
  bool exists = false;
  {
    std::lock_guard<std::mutex> lk(g_xcMutex);
    std::error_code ec;
    exists = validHostComponent(trim(d.fileName())) &&
             fs::exists(packagePath(contentRootLocked(), xuid, d), ec);
  }
  if (exists) {
    if (d.type() == kTypeSavedGame) {
      wr32(r5, 1);   // the user always creates saves
      if (r6) wr64(r6, xuid);
    } else {
      wr32(r5, 0);
      if (r6) wr64(r6, 0);
    }
  } else {
    rc = kErrPathNotFound;
  }
  XAMC_LOG(kOpCreator, "XamContentGetCreator(user=%u file='%s' ovl=%08X) -> rc=%u creator=%u", r3,
           d.fileName().c_str(), r7, rc, exists && d.type() == kTypeSavedGame ? 1u : 0u);
  if (r7) {
    ovlDeferred(r7, rc, 0);
    return kErrIoPending;
  }
  return rc;
}

// XamContentSetThumbnail(user r3, data r4, buf r5, size r6, pOverlapped r7).
uint32_t exp_XamContentSetThumbnail(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                                    uint32_t r7, uint32_t r8) {
  if (!xamContentFsEnabled()) return callPrev(g_prevSetThumbnail, r3, r4, r5, r6, r7, r8);
  uint64_t xuid = 0;
  if (!profileXuid(r3, &xuid)) return kErrNoSuchUser;
  Xcd d;
  if (!readXcd(r4, d)) return kErrInvalidParameter;
  // Bound and range-check BEFORE sizing the host buffer (a garbage size must
  // not become a 4 GB allocation / std::bad_alloc through the import glue).
  if (r6 > (16u << 20) || (r6 && !rangeOk(r5, r6))) return kErrInvalidParameter;
  std::vector<uint8_t> png(r6);
  if (r6 && !readBytes(r5, png.data(), r6)) return kErrInvalidParameter;
  uint32_t rc = kErrFileNotFound;
  {
    std::lock_guard<std::mutex> lk(g_xcMutex);
    if (validHostComponent(trim(d.fileName()))) {
      const fs::path pkg = packagePath(contentRootLocked(), xuid, d);
      std::error_code ec;
      fs::create_directories(pkg, ec);
      if (fs::exists(pkg, ec) && writeWholeFile(pkg / kThumbnailFileName, png.data(), png.size()))
        rc = kErrSuccess;
    }
  }
  XAMC_LOG(kOpThumb, "XamContentSetThumbnail(user=%u file='%s' size=%u ovl=%08X) -> rc=%u", r3,
           d.fileName().c_str(), r6, r7, rc);
  if (r7) {
    ovlImmediate(r7, rc);
    return kErrIoPending;
  }
  return rc;
}

// XamContentCreateEnumerator(user r3, device r4, type r5, flags r6, cItem r7,
//                            pcbBuffer r8, phEnum r9).
uint32_t exp_XamContentCreateEnumerator(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                                        uint32_t r7, uint32_t r8) {
  if (!xamContentFsEnabled()) return callPrev(g_prevCreateEnum, r3, r4, r5, r6, r7, r8);
  const uint32_t user = r3, device = r4, type = r5, flags = r6, cItem = r7, pcb = r8;
  const uint32_t phEnum = ExportRegistry::getCurrentArgR9();
  uint64_t xuid = 0;
  if (user < 4 && !profileXuid(user, &xuid)) return kErrNoSuchUser;
  if ((device != 0 && device != kDeviceHdd && device != kDeviceOdd) || !phEnum) {
    if (pcb) wr32(pcb, 0);
    return kXeInvalidArg;
  }
  if (pcb) wr32(pcb, kXcdSize * cItem);
  Enum e;
  e.perCall = cItem;
  uint32_t handle;
  size_t itemCount = 0;
  {
    std::lock_guard<std::mutex> lk(g_xcMutex);
    if (device == 0 || device == kDeviceHdd) {
      const fs::path root = contentRootLocked();
      std::vector<uint64_t> xuids;
      if (xuid) xuids.push_back(xuid);
      if (!xuid || !(flags & kFlagExcludeCommon)) xuids.push_back(0);   // common content
      std::vector<Xcd> found;
      for (uint64_t x : xuids) listContentLocked(root, kDeviceHdd, x, type, found);
      found.erase(std::unique(found.begin(), found.end(), sameContent), found.end());
      for (const Xcd& d : found) {
        std::array<uint8_t, kXcdSize> item;
        std::memcpy(item.data(), d.raw.data(), kXcdSize);
        e.items.push_back(item);
      }
    }
    // device 0 / ODD: the disc has no content directory for this title.
    handle = g_nextEnum++;
    itemCount = e.items.size();
    g_enums[handle] = std::move(e);
    updateLive();
  }
  wr32(phEnum, handle);
  XAMC_LOG(kOpEnumCreate,
           "XamContentCreateEnumerator(user=%u dev=%u type=%u flags=%08X cItem=%u) -> h=%08X "
           "items=%zu lr=%08X",
           user, device, type, flags, cItem, handle, itemCount,
           getCurrentGuestLR());
  return kErrSuccess;
}

// XamEnumerate(h r3, flags r4, buf r5, cb r6, pcItems r7, pOverlapped r8).
uint32_t exp_XamEnumerate(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7,
                          uint32_t r8) {
  if (!xamContentFsEnabled()) return callPrev(g_prevEnumerate, r3, r4, r5, r6, r7, r8);
  const uint32_t h = r3, buf = r5, pcItems = r7, ovl = r8;
  uint32_t rc, n = 0;
  size_t remaining = 0;
  {
    std::lock_guard<std::mutex> lk(g_xcMutex);
    auto it = g_enums.find(h);
    if (it == g_enums.end()) {
      // xeXamEnumerate: the handle lookup fails synchronously, before (and
      // without touching) any overlapped.
      XAMC_LOG(kOpEnumerate, "XamEnumerate(h=%08X ovl=%08X) -> 6 (unknown handle)", h, ovl);
      return kErrInvalidHandle;
    } else if (!buf) {
      rc = kErrInvalidParameter;
    } else {
      Enum& e = it->second;
      const size_t count = std::min<size_t>(e.items.size() - e.next, e.perCall);
      if (count == 0) {
        rc = kErrNoMoreFiles;
      } else if (!rangeOk(buf, uint32_t(count * kXcdSize))) {
        rc = kErrInvalidParameter;
      } else {
        // ContentEnumerator::WriteItems (does not honour cb, as Xenia).
        for (size_t i = 0; i < count; ++i)
          writeBytes(buf + uint32_t(i * kXcdSize), e.items[e.next + i].data(), kXcdSize);
        e.next += count;
        n = uint32_t(count);
        rc = kErrSuccess;
      }
      remaining = e.items.size() - e.next;
    }
  }
  XAMC_LOG(kOpEnumerate, "XamEnumerate(h=%08X buf=%08X cb=%u ovl=%08X) -> rc=%u items=%u left=%zu",
           h, buf, r6, ovl, rc, n, remaining);
  if (ovl) {   // xeXamEnumerate: an overlapped call completes through it only
    ovlDeferred(ovl, rc, n);
    return kErrIoPending;
  }
  if (pcItems) wr32(pcItems, rc == kErrSuccess ? n : 0);
  return rc;
}

// NtWriteFile(h r3, ev r4, apc r5, ctx r6, iosb r7, buf r8, len r9, pOffset r10).
uint32_t exp_NtWriteFile(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7,
                         uint32_t r8) {
  sr::hitch::Scope hitchScope(sr::hitch::kFileWrite);
  if (!xamContentFsEnabled() || g_liveCount.load() == 0)
    return callPrev(g_prevNtWriteFile, r3, r4, r5, r6, r7, r8);
  const uint32_t len = ExportRegistry::getCurrentArgR9();
  const uint32_t pOffset = ExportRegistry::getCurrentArgR10();
  uint32_t st = kStSuccess;
  size_t written = 0;
  {
    std::lock_guard<std::mutex> lk(g_xcMutex);
    auto it = g_files.find(r3);
    if (it == g_files.end()) {
      // Not a package file: keep the old generic behaviour exactly.
    } else {
      XFile& xf = it->second;
      std::FILE* f = xf.isDir ? nullptr : fileHandleGet(r3);
      // iter-463 (f): hold the handle's I/O lock (file_io.hpp) so this seek+write
      // cannot interleave with an NtReadFile on the same handle.
      std::shared_ptr<std::mutex> io = f ? fileHandleIoLock(r3) : nullptr;
      std::unique_lock<std::mutex> ioHold;
      if (io) {
        ioHold = std::unique_lock<std::mutex>(*io);
        f = fileHandleGet(r3);  // re-validate under the lock (file_io.hpp): an NtClose may have won it
      }
      if (xf.isDir) {
        st = kStInvalidParameter;
      } else if (!f) {
        st = kStInvalidHandle;
      } else if (!xf.writable) {
        st = kStAccessDenied;
      } else if (len && !rangeOk(r8, len)) {
        st = kStInvalidParameter;
      } else {
        // Position: explicit offset; -1 = end of file; -2:-1 or none = current.
        // A seek also makes a following write legal after a read on this FILE*.
        uint64_t off = ~0ull - 1;
        if (pOffset && rangeOk(pOffset, 8))
          off = (uint64_t(readGuestU32BE(pOffset)) << 32) | readGuestU32BE(pOffset + 4);
#ifdef _WIN32
        if (off == ~0ull) _fseeki64(f, 0, SEEK_END);
        else if (off == 0xFFFFFFFEFFFFFFFFull || off == ~0ull - 1) _fseeki64(f, 0, SEEK_CUR);
        else _fseeki64(f, static_cast<__int64>(off), SEEK_SET);
#else
        if (off == ~0ull) std::fseek(f, 0, SEEK_END);
        else if (off == 0xFFFFFFFEFFFFFFFFull || off == ~0ull - 1) std::fseek(f, 0, SEEK_CUR);
        else std::fseek(f, static_cast<long>(off), SEEK_SET);
#endif
        // app/log-async (docs/TOOLKIT.md 3.2.3): a save write stays synchronous on the guest thread
        // (the guest reads its status and count); one over 100 ms is named by an [IO_STALL] line.
        const auto ioT0 = std::chrono::steady_clock::now();
        if (len) written = std::fwrite(guestVAToHost(r8), 1, len, f);
        std::fflush(f);   // keeps a later fread on this FILE* legal
        sr::Log::noteIoStall("save_NtWriteFile", len,
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ioT0).count());
        st = written == len ? kStSuccess : kStDiskFull;
      }
      if (r7) {
        wr32(r7 + 0, st);
        wr32(r7 + 4, uint32_t(written));
      }
      XAMC_LOG(kOpNtWrite, "NtWriteFile(h=%u \"%s\" len=%u off=%s) -> %08X written=%zu%s", r3,
               u8(xf.host).c_str(), len, pOffset ? "given" : "cur", st, written,
               (r4 || r5) ? " (event/APC not signalled)" : "");
      return st;
    }
  }
  return callPrev(g_prevNtWriteFile, r3, r4, r5, r6, r7, r8);
}

// NtFlushBuffersFile(h r3, iosb r4).
uint32_t exp_NtFlushBuffersFile(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7,
                                uint32_t r8) {
  if (xamContentFsEnabled() && g_liveCount.load() != 0) {
    std::lock_guard<std::mutex> lk(g_xcMutex);
    auto it = g_files.find(r3);
    if (it != g_files.end()) {
      if (std::FILE* f = it->second.isDir ? nullptr : fileHandleGet(r3)) std::fflush(f);
      if (r4) {
        wr32(r4, kStSuccess);
        wr32(r4 + 4, 0);
      }
      XAMC_LOG(kOpNtFlush, "NtFlushBuffersFile(h=%u) -> 0", r3);
      return kStSuccess;
    }
  }
  return callPrev(g_prevNtFlush, r3, r4, r5, r6, r7, r8);
}

// ── NT path parsing ──────────────────────────────────────────────────────────
enum class PathKind { NotOurs, Invalid, Unmounted, Ok };
struct ParsedPath {
  std::string key;                 // lowercase root
  std::vector<std::string> comps;  // below the package root
  std::string raw;
};
// OBJECT_ATTRIBUTES (Xbox): +0 RootDirectory, +4 ANSI_STRING*, +8 Attributes.
PathKind parsePathLocked(uint32_t oa, ParsedPath& out) {
  if (!rangeOk(oa, 12)) return PathKind::NotOurs;
  const uint32_t rootDir = rd32(oa);
  if (rootDir != 0 && rootDir != 0xFFFFFFFDu) return PathKind::NotOurs;   // handle-relative
  const uint32_t nameVA = rd32(oa + 4);
  if (!rangeOk(nameVA, 8)) return PathKind::NotOurs;
  const uint16_t len = uint16_t(rd32(nameVA) >> 16);
  const uint32_t bufVA = rd32(nameVA + 4);
  if (len == 0 || len > 1024 || !rangeOk(bufVA, len)) return PathKind::NotOurs;
  std::string s(reinterpret_cast<const char*>(guestVAToHost(bufVA)), len);
  out.raw = s;
  for (char& c : s)
    if (c == '\\') c = '/';
  while (s.rfind("/?\?/", 0) == 0) s.erase(0, 4);   // "\??\" (escaped: no trigraph)
  while (!s.empty() && s[0] == '/') s.erase(0, 1);
  const size_t colon = s.find(':');
  if (colon == std::string::npos || colon == 0) return PathKind::NotOurs;
  out.key = lower(s.substr(0, colon));
  if (!g_mounts.count(out.key))
    return g_knownRoots.count(out.key) ? PathKind::Unmounted : PathKind::NotOurs;
  const std::string rest = s.substr(colon + 1);
  size_t pos = 0;
  while (pos <= rest.size()) {
    const size_t slash = rest.find('/', pos);
    const std::string c = rest.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
    if (!c.empty()) {
      if (!xeniaValidPathChars(c, false) || !validHostComponent(c)) return PathKind::Invalid;
      out.comps.push_back(c);
    }
    if (slash == std::string::npos) break;
    pos = slash + 1;
  }
  return PathKind::Ok;
}

// VirtualFileSystem::OpenFile + NtCreateFile for a mounted package path.
uint32_t ntCreateLocked(const ParsedPath& pp, uint32_t access, uint32_t disposition,
                        uint32_t options, uint32_t* outHandle, uint32_t* outAction,
                        fs::path* outHost) {
  const Mount& m = g_mounts.at(pp.key);
  fs::path target = m.pkg;
  for (const std::string& c : pp.comps) target /= c;
  *outHost = target;
  if (!pp.comps.empty()) {
    const HostStat ps = hostStat(target.parent_path());
    if (!ps.exists || !ps.isDir) { *outAction = kActDoesNotExist; return kStPathNotFound; }
  }
  const HostStat st = hostStat(target);
  if (st.exists && st.isDir && (options & kOptNonDirectoryFile)) return kStFileIsDirectory;
  switch (disposition) {
    case 1: case 4:   // OPEN, OVERWRITE: must exist
      if (!st.exists) { *outAction = kActDoesNotExist; return kStNameNotFound; }
      break;
    case 2:           // CREATE: must not exist
      if (st.exists) { *outAction = kActExists; return kStNameCollision; }
      break;
    case 0: case 3: case 5:
      break;
    default:
      return kStInvalidParameter;
  }
  const bool wantsWrite = (access & (0x2u | 0x4u | 0x40000000u | 0x10000000u)) != 0;
  std::FILE* f = nullptr;
  bool isDir = false;
  if (!st.exists) {
    if (pp.comps.empty()) { *outAction = kActDoesNotExist; return kStPathNotFound; }
    *outAction = kActCreated;
    if (options & kOptDirectoryFile) {
      std::error_code ec;
      if (!fs::create_directory(target, ec) || ec) return kStAccessDenied;
      isDir = true;
    } else {
      f = hostOpen(target, "w+b");
      if (!f) return kStAccessDenied;
    }
  } else if (st.isDir) {
    if (disposition == 0 || disposition == 4 || disposition == 5) return kStFileIsDirectory;
    *outAction = kActOpened;
    isDir = true;
  } else {
    switch (disposition) {
      case 0: *outAction = kActSuperseded; f = hostOpen(target, "w+b"); break;
      case 4: case 5: *outAction = kActOverwritten; f = hostOpen(target, "w+b"); break;
      default: *outAction = kActOpened; f = hostOpen(target, wantsWrite ? "r+b" : "rb"); break;
    }
    if (!f) return kStAccessDenied;
  }
  const uint32_t h = isDir ? fileHandleAllocDir("xamcontent:" + pp.key) : fileHandleAlloc(f);
  if (!h) {
    if (f) std::fclose(f);
    return kStNoResources;
  }
  // iter-463 phase 2: the actual content write (a package file opened for writing) inherits
  // the taint marker (DEVTOOLS_DESIGN 3.1 B7). A no-op on an untainted run.
  if (wantsWrite) devToolsOnSaveWrite(m.pkg);
  XFile xf;
  xf.host = target;
  xf.rootKey = pp.key;
  xf.isDir = isDir;
  xf.writable = wantsWrite;
  xf.deleteOnClose = (options & kOptDeleteOnClose) != 0;   // FILE_DELETE_ON_CLOSE
  g_files[h] = std::move(xf);
  updateLive();
  *outHandle = h;
  return kStSuccess;
}

bool tryCreateCommon(const char* api, uint32_t pH, uint32_t access, uint32_t oa, uint32_t iosb,
                     uint32_t share, uint32_t disposition, uint32_t options, uint32_t* status) {
  if (!xamContentFsEnabled()) return false;
  std::lock_guard<std::mutex> lk(g_xcMutex);
  if (g_knownRoots.empty()) return false;   // nothing was ever mounted
  ParsedPath pp;
  const PathKind kind = parsePathLocked(oa, pp);
  if (kind == PathKind::NotOurs) return false;
  uint32_t st, action = kActDoesNotExist, h = 0xFFFFFFFFu;
  fs::path host;
  if (kind == PathKind::Invalid) st = kStNameInvalid;
  else if (kind == PathKind::Unmounted) st = kStPathNotFound;
  else st = ntCreateLocked(pp, access, disposition, options, &h, &action, &host);
  if (iosb) {
    wr32(iosb, st);
    wr32(iosb + 4, action);
  }
  if (pH) wr32(pH, st == kStSuccess ? h : 0xFFFFFFFFu);
  XAMC_LOG(kOpNtCreate,
           "%s(\"%s\" access=%08X share=%u disp=%u opts=%08X) -> %08X h=%d action=%u host=\"%s\" "
           "lr=%08X",
           api, pp.raw.c_str(), access, share, disposition, options, st,
           st == kStSuccess ? int(h) : -1, action, u8(host).c_str(), getCurrentGuestLR());
  *status = st;
  return true;
}

// X_FILE_DIRECTORY_INFORMATION (xfile.h): +0 next, +4 index, +8/+16/+24/+32
// creation/access/write/change, +40 EOF, +48 allocation, +56 attrs,
// +60 name length, +64 name.
void writeDirInfo(uint32_t va, uint32_t index, const DirEnt& e) {
  uint8_t hdr[kDirInfoHeader] = {};
  putBe32(hdr + 0, 0);
  putBe32(hdr + 4, index);
  putBe64(hdr + 8, e.st.ctime);
  putBe64(hdr + 16, e.st.atime);
  putBe64(hdr + 24, e.st.mtime);
  putBe64(hdr + 32, e.st.mtime);
  putBe64(hdr + 40, e.st.size);
  putBe64(hdr + 48, (e.st.size + 511) & ~uint64_t(511));
  putBe32(hdr + 56, e.st.isDir ? 0x10u : 0x80u);
  putBe32(hdr + 60, uint32_t(e.name.size()));
  writeBytes(va, hdr, kDirInfoHeader);
  if (!e.name.empty()) writeBytes(va + kDirInfoHeader, e.name.data(), uint32_t(e.name.size()));
}

void relistLocked(XFile& xf) {
  xf.ents.clear();
  xf.listed = true;
  std::vector<fs::path> paths;
  std::error_code ec;
  for (fs::directory_iterator it(xf.host, ec), end; !ec && it != end; it.increment(ec))
    paths.push_back(it->path());
  std::sort(paths.begin(), paths.end());
  for (const fs::path& p : paths) {
    DirEnt e;
    e.name = u8(p.filename());
    e.st = hostStat(p);
    if (e.st.exists) xf.ents.push_back(std::move(e));
  }
}

}  // namespace

// ── public API ───────────────────────────────────────────────────────────────
bool xamContentFsEnabled() {
  const int m = g_fsTestMode.load(std::memory_order_relaxed);
  return m < 0 ? fsEnvOn() : m != 0;
}
bool ntStatusDosErrEnabled() {
  if (!xamContentFsEnabled()) return false;
  const int m = g_dosTestMode.load(std::memory_order_relaxed);
  return m < 0 ? dosEnvOn() : m != 0;
}
void xamContentFsSetEnabledForTest(int mode) { g_fsTestMode.store(mode); }
void ntStatusDosErrSetEnabledForTest(int mode) { g_dosTestMode.store(mode); }

fs::path xamContentRoot() {
#ifdef _WIN32
  if (const wchar_t* e = _wgetenv(L"SR_CONTENT_DIR"); e && *e) return fs::path(e);
  // SR_INSTANCE_ID marks an automated test instance (every roll harness sets
  // it: run_recomp_x, sr_drive, hle_replay, replay_ab). Without an explicit
  // SR_CONTENT_DIR such an instance gets a FRESH per-process scratch root, so
  // rolls never read the player's saves, never see a previous roll's save, and
  // parallel instances never share a package (iter-463 review).
  if (const char* inst = std::getenv("SR_INSTANCE_ID"); inst && *inst) {
    std::string id;
    for (const char* p = inst; *p && id.size() < 32; ++p)
      id.push_back(std::isalnum(uint8_t(*p)) ? *p : '_');
    std::error_code tec;
    fs::path tmp = fs::temp_directory_path(tec);
    if (tec || tmp.empty()) tmp = fs::current_path(tec);
    return tmp / "SupermanRecomp" /
           ("content-" + id + "-" + std::to_string(GetCurrentProcessId()));
  }
  fs::path exeDir;
  wchar_t buf[1024];
  const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(sizeof(buf) / sizeof(buf[0])));
  if (n > 0 && n < sizeof(buf) / sizeof(buf[0])) exeDir = fs::path(buf).parent_path();
  std::error_code ec;
  // UnleashedRecomp's portable.txt convention (user/paths.cpp CheckPortable).
  if (!exeDir.empty() && fs::exists(exeDir / "portable.txt", ec)) return exeDir / "content";
  if (const wchar_t* la = _wgetenv(L"LOCALAPPDATA"); la && *la)
    return fs::path(la) / "SupermanRecomp" / "content";
  if (!exeDir.empty()) return exeDir / "content";
  return fs::current_path(ec) / "content";
#else
  if (const char* e = std::getenv("SR_CONTENT_DIR"); e && *e) return fs::path(e);
  std::error_code ec;
  if (const char* inst = std::getenv("SR_INSTANCE_ID"); inst && *inst)
    return fs::temp_directory_path(ec) / "SupermanRecomp" / (std::string("content-") + inst);
  return fs::current_path(ec) / "content";
#endif
}

// Which rule picked the root (boot log, so a roll's provenance names it).
static const char* xamContentRootSource() {
  const char* cd = std::getenv("SR_CONTENT_DIR");
  if (cd && *cd) return "SR_CONTENT_DIR";
  const char* inst = std::getenv("SR_INSTANCE_ID");
  if (inst && *inst) return "SR_INSTANCE_ID: fresh per-process scratch";
  return "persistent player saves";
}

bool xamContentSpace(uint64_t* total, uint64_t* avail) {
  fs::path p = xamContentRoot();
  std::error_code ec;
  while (!p.empty() && !fs::exists(p, ec)) {
    const fs::path parent = p.parent_path();
    if (parent == p) break;
    p = parent;
  }
  const auto sp = fs::space(p, ec);
  if (ec) return false;
  if (total) *total = uint64_t(sp.capacity);
  if (avail) *avail = uint64_t(sp.available);
  return true;
}

// xeRtlNtStatusToDosError (xboxkrnl_error.cc:968-1004), table verbatim below.
namespace {
// ---- begin verbatim: xenia-canary xboxkrnl_error.cc:21-966 (BSD-3) ----
struct error_lookup_table {
  uint32_t base_code;
  size_t count;
  const uint32_t* entries;
};

// TODO(gibbed): replace these with named error codes
constexpr uint32_t error_table_0x00000103[] = {
    0x000003E5,  // 0x00000103
    0,           //
    0x000000EA,  // 0x00000105
    0x00000514,  // 0x00000106
    0x00000515,  // 0x00000107
    0,           //
    0,           //
    0,           //
    0,           //
    0x000003FE,  // 0x0000010C
    0x00000516,  // 0x0000010D
};

constexpr uint32_t error_table_0x40000002[] = {
    0x00000057,  // 0x40000002
    0,           //
    0,           //
    0,           //
    0x00000517,  // 0x40000006
    0,           //
    0x00000460,  // 0x40000008
    0x000003F6,  // 0x40000009
    0,           //
    0,           //
    0x00000461,  // 0x4000000C
    0x00000518,  // 0x4000000D
};

constexpr uint32_t error_table_0x40020056[] = {
    0x00000720,  // 0x40020056
};

constexpr uint32_t error_table_0x400200AF[] = {
    0x00000779,  // 0x400200AF
};

constexpr uint32_t error_table_0x80000001[] = {
    0x80000001,  // 0x80000001
    0x000003E6,  // 0x80000002
    0x80000003,  // 0x80000003
    0x80000004,  // 0x80000004
    0x000000EA,  // 0x80000005
    0x00000012,  // 0x80000006
    0,           //
    0,           //
    0,           //
    0,           //
    0x0000056F,  // 0x8000000B
    0,           //
    0x0000012B,  // 0x8000000D
    0x0000001C,  // 0x8000000E
    0x00000015,  // 0x8000000F
    0x00000015,  // 0x80000010
    0x000000AA,  // 0x80000011
    0x00000103,  // 0x80000012
    0x000000FE,  // 0x80000013
    0x000000FF,  // 0x80000014
    0x000000FF,  // 0x80000015
    0x00000456,  // 0x80000016
    0,           //
    0,           //
    0,           //
    0x00000103,  // 0x8000001A
    0x0000044D,  // 0x8000001B
    0x00000456,  // 0x8000001C
    0x00000457,  // 0x8000001D
    0x0000044C,  // 0x8000001E
    0x0000044E,  // 0x8000001F
    0,           //
    0x0000044F,  // 0x80000021
    0x00000450,  // 0x80000022
    0,           //
    0,           //
    0x00000962,  // 0x80000025
};

constexpr uint32_t error_table_0x80000288[] = {
    0x0000048D,  // 0x80000288
    0x0000048E,  // 0x80000289
};

constexpr uint32_t error_table_0x80090300[] = {
    0x000005AA,  // 0x80090300
    0x00000006,  // 0x80090301
    0x00000001,  // 0x80090302
    0x00000035,  // 0x80090303
    0x0000054F,  // 0x80090304
    0x00000554,  // 0x80090305
    0x00000120,  // 0x80090306
    0x00000554,  // 0x80090307
    0x00000057,  // 0x80090308
    0x00000057,  // 0x80090309
    0x00000032,  // 0x8009030A
    0x00000558,  // 0x8009030B
    0x0000052E,  // 0x8009030C
    0x00000057,  // 0x8009030D
    0x00000520,  // 0x8009030E
    0x00000005,  // 0x8009030F
    0x00000005,  // 0x80090310
    0x0000051F,  // 0x80090311
    0,           //
    0,           //
    0,           //
    0,           //
    0x00000554,  // 0x80090316
    0,           //
    0x000006F8,  // 0x80090318
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0x00000057,  // 0x80090320
    0x0000007A,  // 0x80090321
    0x00000574,  // 0x80090322
    0,           //
    0,           //
    0x000006FE,  // 0x80090325
    0x00000057,  // 0x80090326
    0x00000057,  // 0x80090327
    0x00000532,  // 0x80090328
    0x00001770,  // 0x80090329
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0x00001771,  // 0x80090330
    0x00000001,  // 0x80090331
};

constexpr uint32_t error_table_0xC0000001[] = {
    0x0000001F,  // 0xC0000001
    0x00000001,  // 0xC0000002
    0x00000057,  // 0xC0000003
    0x00000018,  // 0xC0000004
    0x000003E6,  // 0xC0000005
    0x000003E7,  // 0xC0000006
    0x000005AE,  // 0xC0000007
    0x00000006,  // 0xC0000008
    0x000003E9,  // 0xC0000009
    0x000000C1,  // 0xC000000A
    0x00000057,  // 0xC000000B
    0,           //
    0x00000057,  // 0xC000000D
    0x00000002,  // 0xC000000E
    0x00000002,  // 0xC000000F
    0x00000001,  // 0xC0000010
    0x00000026,  // 0xC0000011
    0x00000022,  // 0xC0000012
    0x00000015,  // 0xC0000013
    0x000006F9,  // 0xC0000014
    0x0000001B,  // 0xC0000015
    0x000000EA,  // 0xC0000016
    0x00000008,  // 0xC0000017
    0x000001E7,  // 0xC0000018
    0x000001E7,  // 0xC0000019
    0x00000057,  // 0xC000001A
    0x00000057,  // 0xC000001B
    0x00000001,  // 0xC000001C
    0xC000001D,  // 0xC000001D
    0x00000005,  // 0xC000001E
    0x00000005,  // 0xC000001F
    0x000000C1,  // 0xC0000020
    0x00000005,  // 0xC0000021
    0x00000005,  // 0xC0000022
    0x0000007A,  // 0xC0000023
    0x00000006,  // 0xC0000024
    0xC0000025,  // 0xC0000025
    0xC0000026,  // 0xC0000026
    0,           //
    0,           //
    0,           //
    0x0000009E,  // 0xC000002A
    0xC000002B,  // 0xC000002B
    0x000001E7,  // 0xC000002C
    0x000001E7,  // 0xC000002D
    0,           //
    0,           //
    0x00000057,  // 0xC0000030
    0,           //
    0x00000571,  // 0xC0000032
    0x0000007B,  // 0xC0000033
    0x00000002,  // 0xC0000034
    0x000000B7,  // 0xC0000035
    0,           //
    0x00000006,  // 0xC0000037
    0,           //
    0x000000A1,  // 0xC0000039
    0x00000003,  // 0xC000003A
    0x000000A1,  // 0xC000003B
    0x0000045D,  // 0xC000003C
    0x0000045D,  // 0xC000003D
    0x00000017,  // 0xC000003E
    0x00000017,  // 0xC000003F
    0x00000008,  // 0xC0000040
    0x00000005,  // 0xC0000041
    0x00000006,  // 0xC0000042
    0x00000020,  // 0xC0000043
    0x00000718,  // 0xC0000044
    0x00000057,  // 0xC0000045
    0x00000120,  // 0xC0000046
    0x0000012A,  // 0xC0000047
    0x00000057,  // 0xC0000048
    0x00000057,  // 0xC0000049
    0x0000009C,  // 0xC000004A
    0x00000005,  // 0xC000004B
    0x00000057,  // 0xC000004C
    0x00000057,  // 0xC000004D
    0x00000057,  // 0xC000004E
    0x0000011A,  // 0xC000004F
    0x000000FF,  // 0xC0000050
    0x00000570,  // 0xC0000051
    0x00000570,  // 0xC0000052
    0x00000570,  // 0xC0000053
    0x00000021,  // 0xC0000054
    0x00000021,  // 0xC0000055
    0x00000005,  // 0xC0000056
    0x00000032,  // 0xC0000057
    0x00000519,  // 0xC0000058
    0x0000051A,  // 0xC0000059
    0x0000051B,  // 0xC000005A
    0x0000051C,  // 0xC000005B
    0x0000051D,  // 0xC000005C
    0x0000051E,  // 0xC000005D
    0x0000051F,  // 0xC000005E
    0x00000520,  // 0xC000005F
    0x00000521,  // 0xC0000060
    0x00000522,  // 0xC0000061
    0x00000523,  // 0xC0000062
    0x00000524,  // 0xC0000063
    0x00000525,  // 0xC0000064
    0x00000526,  // 0xC0000065
    0x00000527,  // 0xC0000066
    0x00000528,  // 0xC0000067
    0x00000529,  // 0xC0000068
    0x0000052A,  // 0xC0000069
    0x00000056,  // 0xC000006A
    0x0000052C,  // 0xC000006B
    0x0000052D,  // 0xC000006C
    0x0000052E,  // 0xC000006D
    0x0000052F,  // 0xC000006E
    0x00000530,  // 0xC000006F
    0x00000531,  // 0xC0000070
    0x00000532,  // 0xC0000071
    0x00000533,  // 0xC0000072
    0x00000534,  // 0xC0000073
    0x00000535,  // 0xC0000074
    0x00000536,  // 0xC0000075
    0x00000537,  // 0xC0000076
    0x00000538,  // 0xC0000077
    0x00000539,  // 0xC0000078
    0x0000053A,  // 0xC0000079
    0x0000007F,  // 0xC000007A
    0x000000C1,  // 0xC000007B
    0x000003F0,  // 0xC000007C
    0x0000053C,  // 0xC000007D
    0x0000009E,  // 0xC000007E
    0x00000070,  // 0xC000007F
    0x0000053D,  // 0xC0000080
    0x0000053E,  // 0xC0000081
    0x00000044,  // 0xC0000082
    0x00000103,  // 0xC0000083
    0x0000053F,  // 0xC0000084
    0x00000103,  // 0xC0000085
    0x0000009A,  // 0xC0000086
    0x0000000E,  // 0xC0000087
    0x000001E7,  // 0xC0000088
    0x00000714,  // 0xC0000089
    0x00000715,  // 0xC000008A
    0x00000716,  // 0xC000008B
    0xC000008C,  // 0xC000008C
    0xC000008D,  // 0xC000008D
    0xC000008E,  // 0xC000008E
    0xC000008F,  // 0xC000008F
    0xC0000090,  // 0xC0000090
    0xC0000091,  // 0xC0000091
    0xC0000092,  // 0xC0000092
    0xC0000093,  // 0xC0000093
    0xC0000094,  // 0xC0000094
    0x00000216,  // 0xC0000095
    0xC0000096,  // 0xC0000096
    0x00000008,  // 0xC0000097
    0x000003EE,  // 0xC0000098
    0x00000540,  // 0xC0000099
    0x000005AA,  // 0xC000009A
    0x00000003,  // 0xC000009B
    0x00000017,  // 0xC000009C
    0x0000048F,  // 0xC000009D
    0x00000015,  // 0xC000009E
    0x000001E7,  // 0xC000009F
    0x000001E7,  // 0xC00000A0
    0x000005AD,  // 0xC00000A1
    0x00000013,  // 0xC00000A2
    0x00000015,  // 0xC00000A3
    0x00000541,  // 0xC00000A4
    0x00000542,  // 0xC00000A5
    0x00000543,  // 0xC00000A6
    0x00000544,  // 0xC00000A7
    0x00000545,  // 0xC00000A8
    0x00000057,  // 0xC00000A9
    0,           //
    0x000000E7,  // 0xC00000AB
    0x000000E7,  // 0xC00000AC
    0x000000E6,  // 0xC00000AD
    0x000000E7,  // 0xC00000AE
    0x00000001,  // 0xC00000AF
    0x000000E9,  // 0xC00000B0
    0x000000E8,  // 0xC00000B1
    0x00000217,  // 0xC00000B2
    0x00000218,  // 0xC00000B3
    0x000000E6,  // 0xC00000B4
    0x00000079,  // 0xC00000B5
    0x00000026,  // 0xC00000B6
    0,           //
    0,           //
    0,           //
    0x00000005,  // 0xC00000BA
    0x00000032,  // 0xC00000BB
    0x00000033,  // 0xC00000BC
    0x00000034,  // 0xC00000BD
    0x00000035,  // 0xC00000BE
    0x00000036,  // 0xC00000BF
    0x00000037,  // 0xC00000C0
    0x00000038,  // 0xC00000C1
    0x00000039,  // 0xC00000C2
    0x0000003A,  // 0xC00000C3
    0x0000003B,  // 0xC00000C4
    0x0000003C,  // 0xC00000C5
    0x0000003D,  // 0xC00000C6
    0x0000003E,  // 0xC00000C7
    0x0000003F,  // 0xC00000C8
    0x00000040,  // 0xC00000C9
    0x00000041,  // 0xC00000CA
    0x00000042,  // 0xC00000CB
    0x00000043,  // 0xC00000CC
    0x00000044,  // 0xC00000CD
    0x00000045,  // 0xC00000CE
    0x00000046,  // 0xC00000CF
    0x00000047,  // 0xC00000D0
    0x00000048,  // 0xC00000D1
    0x00000058,  // 0xC00000D2
    0,           //
    0x00000011,  // 0xC00000D4
    0x00000005,  // 0xC00000D5
    0x000000F0,  // 0xC00000D6
    0x00000546,  // 0xC00000D7
    0,           //
    0x000000E8,  // 0xC00000D9
    0x00000547,  // 0xC00000DA
    0,           //
    0x00000548,  // 0xC00000DC
    0x00000549,  // 0xC00000DD
    0x0000054A,  // 0xC00000DE
    0x0000054B,  // 0xC00000DF
    0x0000054C,  // 0xC00000E0
    0x0000054D,  // 0xC00000E1
    0x0000012C,  // 0xC00000E2
    0x0000012D,  // 0xC00000E3
    0x0000054E,  // 0xC00000E4
    0x0000054F,  // 0xC00000E5
    0x00000550,  // 0xC00000E6
    0x00000551,  // 0xC00000E7
    0x000006F8,  // 0xC00000E8
    0,           //
    0,           //
    0,           //
    0,           //
    0x00000552,  // 0xC00000ED
    0x00000553,  // 0xC00000EE
    0x00000057,  // 0xC00000EF
    0x00000057,  // 0xC00000F0
    0x00000057,  // 0xC00000F1
    0x00000057,  // 0xC00000F2
    0x00000057,  // 0xC00000F3
    0x00000057,  // 0xC00000F4
    0x00000057,  // 0xC00000F5
    0x00000057,  // 0xC00000F6
    0x00000057,  // 0xC00000F7
    0x00000057,  // 0xC00000F8
    0x00000057,  // 0xC00000F9
    0x00000057,  // 0xC00000FA
    0x00000003,  // 0xC00000FB
    0,           //
    0x000003E9,  // 0xC00000FD
    0x00000554,  // 0xC00000FE
    0,           //
    0x000000CB,  // 0xC0000100
    0x00000091,  // 0xC0000101
    0x00000570,  // 0xC0000102
    0x0000010B,  // 0xC0000103
    0x00000555,  // 0xC0000104
    0x00000556,  // 0xC0000105
    0x000000CE,  // 0xC0000106
    0x00000961,  // 0xC0000107
    0x00000964,  // 0xC0000108
    0x0000013D,  // 0xC0000109
    0x00000005,  // 0xC000010A
    0x00000557,  // 0xC000010B
    0,           //
    0x00000558,  // 0xC000010D
    0x00000420,  // 0xC000010E
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0x000005A4,  // 0xC0000117
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0x000003EE,  // 0xC000011E
    0x00000004,  // 0xC000011F
    0x000003E3,  // 0xC0000120
    0x00000005,  // 0xC0000121
    0x000004BA,  // 0xC0000122
    0x00000005,  // 0xC0000123
    0x0000055B,  // 0xC0000124
    0x0000055C,  // 0xC0000125
    0x0000055D,  // 0xC0000126
    0x0000055E,  // 0xC0000127
    0x00000006,  // 0xC0000128
    0,           //
    0,           //
    0x0000055F,  // 0xC000012B
    0,           //
    0x000005AF,  // 0xC000012D
    0,           //
    0,           //
    0x000000C1,  // 0xC0000130
    0,           //
    0,           //
    0x00000576,  // 0xC0000133
    0,           //
    0x0000007E,  // 0xC0000135
    0,           //
    0,           //
    0x000000B6,  // 0xC0000138
    0x0000007F,  // 0xC0000139
    0,           //
    0x00000040,  // 0xC000013B
    0x00000040,  // 0xC000013C
    0x00000033,  // 0xC000013D
    0x0000003B,  // 0xC000013E
    0x0000003B,  // 0xC000013F
    0x0000003B,  // 0xC0000140
    0x0000003B,  // 0xC0000141
    0x0000045A,  // 0xC0000142
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0x0000007C,  // 0xC0000148
    0x00000056,  // 0xC0000149
    0,           //
    0x0000006D,  // 0xC000014B
    0x000003F1,  // 0xC000014C
    0x000003F8,  // 0xC000014D
    0,           //
    0x000003ED,  // 0xC000014F
    0x0000045E,  // 0xC0000150
    0x00000560,  // 0xC0000151
    0x00000561,  // 0xC0000152
    0x00000562,  // 0xC0000153
    0x00000563,  // 0xC0000154
    0x00000564,  // 0xC0000155
    0x00000565,  // 0xC0000156
    0x00000566,  // 0xC0000157
    0x00000567,  // 0xC0000158
    0x000003EF,  // 0xC0000159
    0x00000568,  // 0xC000015A
    0x00000569,  // 0xC000015B
    0x000003F9,  // 0xC000015C
    0x0000056A,  // 0xC000015D
    0,           //
    0,           //
    0,           //
    0,           //
    0x00000459,  // 0xC0000162
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0x00000466,  // 0xC0000169
    0x00000467,  // 0xC000016A
    0x00000468,  // 0xC000016B
    0x0000045F,  // 0xC000016C
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0x00000451,  // 0xC0000172
    0x00000452,  // 0xC0000173
    0x00000453,  // 0xC0000174
    0x00000454,  // 0xC0000175
    0x00000455,  // 0xC0000176
    0x00000469,  // 0xC0000177
    0x00000458,  // 0xC0000178
    0,           //
    0x0000056B,  // 0xC000017A
    0x0000056C,  // 0xC000017B
    0x000003FA,  // 0xC000017C
    0x000003FB,  // 0xC000017D
    0x0000056D,  // 0xC000017E
    0x0000056E,  // 0xC000017F
    0x000003FC,  // 0xC0000180
    0x000003FD,  // 0xC0000181
    0x00000057,  // 0xC0000182
    0x0000045D,  // 0xC0000183
    0x00000016,  // 0xC0000184
    0x0000045D,  // 0xC0000185
    0x0000045D,  // 0xC0000186
    0,           //
    0x000005DE,  // 0xC0000188
    0x00000013,  // 0xC0000189
    0x000006FA,  // 0xC000018A
    0x000006FB,  // 0xC000018B
    0x000006FC,  // 0xC000018C
    0x000006FD,  // 0xC000018D
    0x000005DC,  // 0xC000018E
    0x000005DD,  // 0xC000018F
    0x000006FE,  // 0xC0000190
    0,           //
    0x00000700,  // 0xC0000192
    0x00000701,  // 0xC0000193
    0x0000046B,  // 0xC0000194
    0x000004C3,  // 0xC0000195
    0x000004C4,  // 0xC0000196
    0x000005DF,  // 0xC0000197
    0x0000070F,  // 0xC0000198
    0x00000710,  // 0xC0000199
    0x00000711,  // 0xC000019A
    0x00000712,  // 0xC000019B
};

constexpr uint32_t error_table_0xC0000202[] = {
    0x00000572,  // 0xC0000202
    0x0000003B,  // 0xC0000203
    0x00000717,  // 0xC0000204
    0x0000046A,  // 0xC0000205
    0x000006F8,  // 0xC0000206
    0x000004BE,  // 0xC0000207
    0x000004BE,  // 0xC0000208
    0x00000044,  // 0xC0000209
    0x00000034,  // 0xC000020A
    0x00000040,  // 0xC000020B
    0x00000040,  // 0xC000020C
    0x00000040,  // 0xC000020D
    0x00000044,  // 0xC000020E
    0x0000003B,  // 0xC000020F
    0x0000003B,  // 0xC0000210
    0x0000003B,  // 0xC0000211
    0x0000003B,  // 0xC0000212
    0x0000003B,  // 0xC0000213
    0x0000003B,  // 0xC0000214
    0x0000003B,  // 0xC0000215
    0x00000032,  // 0xC0000216
    0x00000032,  // 0xC0000217
    0,           //
    0,           //
    0,           //
    0,           //
    0x000017E6,  // 0xC000021C
    0,           //
    0,           //
    0,           //
    0x0000046C,  // 0xC0000220
    0x000000C1,  // 0xC0000221
    0,           //
    0,           //
    0x00000773,  // 0xC0000224
    0x00000490,  // 0xC0000225
    0,           //
    0,           //
    0,           //
    0,           //
    0xC000022A,  // 0xC000022A
    0xC000022B,  // 0xC000022B
    0,           //
    0x000004D5,  // 0xC000022D
    0,           //
    0,           //
    0x00000492,  // 0xC0000230
    0,           //
    0,           //
    0x00000774,  // 0xC0000233
    0x00000775,  // 0xC0000234
    0x00000006,  // 0xC0000235
    0x000004C9,  // 0xC0000236
    0x000004CA,  // 0xC0000237
    0x000004CB,  // 0xC0000238
    0x000004CC,  // 0xC0000239
    0x000004CD,  // 0xC000023A
    0x000004CE,  // 0xC000023B
    0x000004CF,  // 0xC000023C
    0x000004D0,  // 0xC000023D
    0x000004D1,  // 0xC000023E
    0x000004D2,  // 0xC000023F
    0x000004D3,  // 0xC0000240
    0x000004D4,  // 0xC0000241
    0,           //
    0x000004C8,  // 0xC0000243
    0,           //
    0,           //
    0x000004D6,  // 0xC0000246
    0x000004D7,  // 0xC0000247
    0x000004D8,  // 0xC0000248
    0x000000C1,  // 0xC0000249
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0x0000054F,  // 0xC0000253
    0,           //
    0,           //
    0,           //
    0x000004D0,  // 0xC0000257
    0,           //
    0x00000573,  // 0xC0000259
    0,           //
    0,           //
    0,           //
    0,           //
    0x00000422,  // 0xC000025E
    0,           //
    0,           //
    0,           //
    0x000000B6,  // 0xC0000262
    0x0000007F,  // 0xC0000263
    0x00000120,  // 0xC0000264
    0x00000476,  // 0xC0000265
    0,           //
    0x000010FE,  // 0xC0000267
    0,           //
    0,           //
    0,           //
    0,           //
    0x000007D1,  // 0xC000026C
    0x000004B1,  // 0xC000026D
    0x00000015,  // 0xC000026E
    0,           //
    0,           //
    0,           //
    0x00000491,  // 0xC0000272
    0,           //
    0,           //
    0x00001126,  // 0xC0000275
    0x00001129,  // 0xC0000276
    0x0000112A,  // 0xC0000277
    0x00001128,  // 0xC0000278
    0x00000780,  // 0xC0000279
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0x00000781,  // 0xC0000280
    0x000000A1,  // 0xC0000281
    0,           //
    0x00000488,  // 0xC0000283
    0x00000489,  // 0xC0000284
    0x0000048A,  // 0xC0000285
    0x0000048B,  // 0xC0000286
    0x0000048C,  // 0xC0000287
    0,           //
    0,           //
    0x00000005,  // 0xC000028A
    0x00000005,  // 0xC000028B
    0,           //
    0x00000005,  // 0xC000028D
    0x00000005,  // 0xC000028E
    0x00000005,  // 0xC000028F
    0x00000005,  // 0xC0000290
    0x00001777,  // 0xC0000291
    0x00001778,  // 0xC0000292
    0x00001772,  // 0xC0000293
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0x00000001,  // 0xC000029C
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0x00001127,  // 0xC00002B2
    0,           //
    0,           //
    0,           //
    0x00000651,  // 0xC00002B6
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0x00000575,  // 0xC00002C3
    0,           //
    0x000003E6,  // 0xC00002C5
    0,           //
    0,           //
    0,           //
    0,           //
    0x000010E8,  // 0xC00002CA
    0,           //
    0x000004E3,  // 0xC00002CC
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0x00000052,  // 0xC00002EA
};

constexpr uint32_t error_table_0xC0020001[] = {
    0x000006A4,  // 0xC0020001
    0x000006A5,  // 0xC0020002
    0x00000006,  // 0xC0020003
    0x000006A7,  // 0xC0020004
    0x000006A8,  // 0xC0020005
    0x000006A9,  // 0xC0020006
    0x000006AA,  // 0xC0020007
    0x000006AB,  // 0xC0020008
    0x000006AC,  // 0xC0020009
    0x000006AD,  // 0xC002000A
    0x000006AE,  // 0xC002000B
    0x000006AF,  // 0xC002000C
    0x000006B0,  // 0xC002000D
    0x000006B1,  // 0xC002000E
    0x000006B2,  // 0xC002000F
    0x000006B3,  // 0xC0020010
    0x000006B4,  // 0xC0020011
    0x000006B5,  // 0xC0020012
    0x000006B6,  // 0xC0020013
    0x000006B7,  // 0xC0020014
    0x000006B8,  // 0xC0020015
    0x000006B9,  // 0xC0020016
    0x000006BA,  // 0xC0020017
    0x000006BB,  // 0xC0020018
    0x000006BC,  // 0xC0020019
    0x000006BD,  // 0xC002001A
    0x000006BE,  // 0xC002001B
    0x000006BF,  // 0xC002001C
    0x000006C0,  // 0xC002001D
    0,           //
    0x000006C2,  // 0xC002001F
    0,           //
    0x000006C4,  // 0xC0020021
    0x000006C5,  // 0xC0020022
    0x000006C6,  // 0xC0020023
    0x000006C7,  // 0xC0020024
    0x000006C8,  // 0xC0020025
    0x000006C9,  // 0xC0020026
    0,           //
    0x000006CB,  // 0xC0020028
    0x000006CC,  // 0xC0020029
    0x000006CD,  // 0xC002002A
    0x000006CE,  // 0xC002002B
    0x000006CF,  // 0xC002002C
    0x000006D0,  // 0xC002002D
    0x000006D1,  // 0xC002002E
    0x000006D2,  // 0xC002002F
    0x000006D3,  // 0xC0020030
    0x000006D4,  // 0xC0020031
    0x000006D5,  // 0xC0020032
    0x000006D6,  // 0xC0020033
    0x000006D7,  // 0xC0020034
    0x000006D8,  // 0xC0020035
    0x000006D9,  // 0xC0020036
    0x000006DA,  // 0xC0020037
    0x000006DB,  // 0xC0020038
    0x000006DC,  // 0xC0020039
    0x000006DD,  // 0xC002003A
    0x000006DE,  // 0xC002003B
    0x000006DF,  // 0xC002003C
    0x000006E0,  // 0xC002003D
    0x000006E1,  // 0xC002003E
    0x000006E2,  // 0xC002003F
    0x000006E3,  // 0xC0020040
    0x000006E4,  // 0xC0020041
    0x000006E5,  // 0xC0020042
    0x000006E6,  // 0xC0020043
    0x000006E7,  // 0xC0020044
    0x000006E8,  // 0xC0020045
    0x000006E9,  // 0xC0020046
    0x000006EA,  // 0xC0020047
    0x000006EB,  // 0xC0020048
    0x000006FF,  // 0xC0020049
    0x0000070E,  // 0xC002004A
    0x0000076A,  // 0xC002004B
    0x0000076B,  // 0xC002004C
    0x0000076C,  // 0xC002004D
    0,           //
    0x00000719,  // 0xC002004F
    0x0000071A,  // 0xC0020050
    0x0000071B,  // 0xC0020051
    0x0000071C,  // 0xC0020052
    0x0000071D,  // 0xC0020053
    0x0000071E,  // 0xC0020054
    0x0000071F,  // 0xC0020055
    0,           //
    0x00000721,  // 0xC0020057
    0x00000722,  // 0xC0020058
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0x0000077A,  // 0xC0020062
    0x0000077B,  // 0xC0020063
};

constexpr uint32_t error_table_0xC0030001[] = {
    0x000006EC,  // 0xC0030001
    0x000006ED,  // 0xC0030002
    0x000006EE,  // 0xC0030003
    0x00000006,  // 0xC0030004
    0x00000006,  // 0xC0030005
    0x000006F1,  // 0xC0030006
    0x000006F2,  // 0xC0030007
    0x000006F3,  // 0xC0030008
    0x000006F4,  // 0xC0030009
    0x000006F5,  // 0xC003000A
    0x000006F6,  // 0xC003000B
    0x000006F7,  // 0xC003000C
};

constexpr uint32_t error_table_0xC0030059[] = {
    0x00000723,  // 0xC0030059
    0x00000724,  // 0xC003005A
    0x00000725,  // 0xC003005B
    0x00000726,  // 0xC003005C
    0,           //
    0x00000728,  // 0xC003005E
    0x0000077C,  // 0xC003005F
    0x0000077D,  // 0xC0030060
    0x0000077E,  // 0xC0030061
};

constexpr uint32_t error_table_0xC0050003[] = {
    0x0000045D,  // 0xC0050003
    0x00000456,  // 0xC0050004
};

constexpr uint32_t error_table_0xC0980001[] = {
    0x00000037,  // 0xC0980001
    0x00000037,  // 0xC0980002
    0,           //
    0,           //
    0,           //
    0,           //
    0,           //
    0x00000037,  // 0xC0980008
};

#define MAKE_ENTRY(x) {x, sizeof(error_table_##x) / sizeof(error_table_##x[0]), error_table_##x}
constexpr error_lookup_table error_tables[] = {
    MAKE_ENTRY(0x00000103), MAKE_ENTRY(0x40000002), MAKE_ENTRY(0x40020056),
    MAKE_ENTRY(0x400200AF), MAKE_ENTRY(0x80000001), MAKE_ENTRY(0x80000288),
    MAKE_ENTRY(0x80090300), MAKE_ENTRY(0xC0000001), MAKE_ENTRY(0xC0000202),
    MAKE_ENTRY(0xC0020001), MAKE_ENTRY(0xC0030001), MAKE_ENTRY(0xC0030059),
    MAKE_ENTRY(0xC0050003), MAKE_ENTRY(0xC0980001), {0, 0, nullptr},
};
#undef MAKE_ENTRY
// ---- end verbatim ----
}  // namespace

uint32_t ntStatusToDosError(uint32_t source_status) {
  uint32_t status = source_status;
  if (!status || (status & 0x20000000)) return status;
  if ((status >> 16) == 0x8007) return status & 0xFFFF;
  if ((status & 0xF0000000) == 0xD0000000) status &= ~0x30000000u;   // high bits don't matter
  for (const error_lookup_table* t = &error_tables[0]; t->base_code; ++t) {
    if (status < t->base_code) break;
    const uint32_t index = status - t->base_code;
    if (index < t->count) {
      const uint32_t result = t->entries[index];
      if (!result) break;
      return result;
    }
  }
  if ((status >> 16) == 0xC001) return status & 0xFFFF;
  return 317;   // ERROR_MR_MID_NOT_FOUND
}

namespace {
uint32_t exp_RtlNtStatusToDosError(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                                   uint32_t r7, uint32_t r8) {
  // iter-466 stutter: with SR_ASYNC_READ an overlapped NtReadFile returns STATUS_PENDING, and
  // the XAPI ReadFile must see ERROR_IO_PENDING (xeRtlNtStatusToDosError: 0x103 -> 997) or its
  // stream readers treat the read as failed. Only this one status, so the rest of the title
  // keeps the SR_NTSTATUS_DOSERR default.
  if (r3 == 0x103u && asyncReadPendingMapped()) return 997u;
  if (!ntStatusDosErrEnabled()) return callPrev(g_prevDosErr, r3, r4, r5, r6, r7, r8);
  const uint32_t rc = ntStatusToDosError(r3);
  if (r3 != 0) XAMC_LOG(kOpDosErr, "RtlNtStatusToDosError(%08X) -> %u lr=%08X", r3, rc, getCurrentGuestLR());
  return rc;
}

// step4/audit batch 3 (logic audit 2.0g) SR_IMPORT_FIX_ATTRS (PLATFORM FIX, baked default ON 2026-10-20 after its A/B;
// =0 = the old answer):
// NtQueryFullAttributesFile answers by real existence, as Xenia 0bd090dbe does (xboxkrnl_io.cc:474-513): a path that
// is not ASCII -> X_STATUS_OBJECT_NAME_INVALID; a path the file system does not resolve -> X_STATUS_NO_SUCH_FILE
// (0xC000000F); a resolved one -> SUCCESS with X_FILE_NETWORK_OPEN_INFORMATION filled (the times, allocation size,
// end of file, attributes). =0: a path outside a content package goes to the generic stub - SUCCESS for every
// path, the info left unwritten, so GetFileAttributesA returns whatever its stack held as the attributes - and the
// D3DX compiler's one-time probe of `ShaderDumpxe:\CompareBackEnds` (CompileShaderCompareBackEnds_sub_823B3370 ->
// GetFileAttributesA sub_82469720, `!= -1`) turned its compare-the-back-ends debug mode on. IDA (2026-10-20): the
// export's only caller is GetFileAttributesA (0x82469758), whose only callers are that probe (0x823B33C0) and the
// effect preprocessor's file #include (EffectPreprocHandleInclude_sub_8236DD90 at 0x8236DE90: -1 or a folder ->
// the path is retried under the includer's folder; only without an include interface). No save, mod or front-end
// path of ours reaches the export (the mod overlay works at NtCreateFile / NtOpenFile).
// The answer per device, as on Xenia 0bd090dbe (Astra batch-3 review, 2026-10-20):
// - A content package path (rmcsave:, ... - the branch above): Xenia's host-path device (vfs/devices/host_path_entry.cc
//   32-53): the host file's times; a folder = DIRECTORY, size 0, allocation 0; a file = NORMAL, its size, allocation
//   rounded to 0x200 (putNetworkOpenInfo).
// - A disc path (game: / d: -> the disc folder): Xenia runs the title from the ISO, so its disc device
//   (disc_image_device.cc:118-161, vfs/entry.cc:21-30). The root entry is DIRECTORY with size, allocation and the
//   three times 0. Every other entry carries the ISO entry's XDVDFS attribute byte | READONLY; end of file is the
//   entry length (a folder's is its directory table length); allocation is the length rounded to 0x200; creation,
//   access and write (change = write) are 1970-01-01 UTC. Ours serves the disc from its extracted folder. So the
//   attribute byte and the times are the disc device's constants (the retail ISO's entries:
//   archive/step4_claude_20261018/audit/b2/census/iso_attrs.json). The lengths come from the folder: a file's size,
//   and a folder's table length from its listing (xdvdfsDirLength).
// - A disc-namespace path that an enabled mod provides (a port feature; Xenia has no overlay) answers the same way,
//   except that the times are the backing host file's or folder's (it is not on the ISO). A mod's file shadows the
//   disc's as at NtCreateFile; a folder only a mod has follows the disc folders.
// - Anything else (a host drive path, a device ours does not mount, e.g. ShaderDumpxe:): NO_SUCH_FILE, as Xenia's
//   ResolvePath without a device (virtual_file_system.cc:140-151).
// A trailing separator is ignored, as Xenia's split_path drops empty parts.
constexpr uint32_t kAttrReadOnly = 0x01u, kAttrDirectory = 0x10u;
constexpr uint32_t kIsoFileAttr = 0x80u;   // NORMAL: all 14 file entries of the retail ISO (iso_attrs.json; folders 0x10)
constexpr uint64_t kFileTime1970 = 116444736000000000ull;   // 1970-01-01 in 100 ns since 1601 (= 10000 * 11644473600000)

bool attrsFixOn() {   // baked: default ON, =0 = the old answer
  static const bool v = [] { const char* e = std::getenv("SR_IMPORT_FIX_ATTRS"); return !e || e[0] != '0'; }();
  return v;
}

// A content package entry (host-path device semantics, above).
void putNetworkOpenInfo(uint32_t va, const HostStat& hs) {
  uint8_t info[56] = {};
  putBe64(info + 0, hs.ctime);
  putBe64(info + 8, hs.atime);
  putBe64(info + 16, hs.mtime);
  putBe64(info + 24, hs.mtime);
  putBe64(info + 32, (hs.size + 511) & ~uint64_t(511));
  putBe64(info + 40, hs.size);
  putBe32(info + 48, hs.isDir ? 0x10u : 0x80u);
  writeBytes(va, info, sizeof(info));
}

void putOpenInfo(uint32_t va, uint64_t ctime, uint64_t atime, uint64_t wtime, uint64_t size, uint32_t attrs) {
  uint8_t info[56] = {};
  putBe64(info + 0, ctime);
  putBe64(info + 8, atime);
  putBe64(info + 16, wtime);
  putBe64(info + 24, wtime);   // change time = write time (xboxkrnl_io.cc:504)
  putBe64(info + 32, (size + 0x1FF) & ~uint64_t(0x1FF));
  putBe64(info + 40, size);
  putBe32(info + 48, attrs);
  writeBytes(va, info, sizeof(info));
}

// An XDVDFS directory table's length for a listing: each entry is 14 bytes + the name, 4-byte aligned, and no entry
// crosses a 2048-byte sector, so the table takes whole sectors (the retail disc's folders: one each, 2048). Empty: 0.
uint64_t xdvdfsDirLength(const std::vector<ModDirEntry>& entries) {
  if (entries.empty()) return 0;
  uint64_t sectors = 1, used = 0;
  for (const ModDirEntry& e : entries) {
    const uint64_t n = (14u + e.name.size() + 3u) & ~uint64_t(3);
    if (used + n > 2048u) { ++sectors; used = 0; }
    used += n;
  }
  return sectors * 2048u;
}

// The listing the guest sees for a disc-namespace folder: the disc folder's entries (if any), then the overlay's.
std::vector<ModDirEntry> discNamespaceListing(const std::string& hostDiscDir) {
  std::vector<ModDirEntry> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(fs::u8path(hostDiscDir), ec)) {
    if (ec) break;
    ModDirEntry d;
    const auto u8 = e.path().filename().u8string();
    d.name.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
    std::error_code ec2;
    d.isDir = e.is_directory(ec2);
    out.push_back(std::move(d));
  }
  modOverlayMergeDir(hostDiscDir, out);   // no-op without an active mod
  return out;
}

uint32_t attrsQueryHost(uint32_t oa, uint32_t infoVA) {
  // The guest's own string (Xenia checks the title's path, not the host path it maps to: a content root under a
  // folder with a non-ASCII name must not make every path invalid). OA: +0 RootDirectory, +4 ANSI_STRING*.
  std::string raw;
  if (rangeOk(oa, 12)) {
    const uint32_t nameVA = rd32(oa + 4);
    if (rangeOk(nameVA, 8)) {
      const uint16_t len = uint16_t(rd32(nameVA) >> 16);
      const uint32_t bufVA = rd32(nameVA + 4);
      if (len != 0 && len <= 2048 && rangeOk(bufVA, len))
        raw.assign(reinterpret_cast<const char*>(guestVAToHost(bufVA)), len);
    }
  }
  uint32_t st = kStNoSuchFile;
  std::string path = raw.empty() ? std::string() : resolveGuestPath(oa);
  while (path.size() > 1 && path.back() == '/') path.pop_back();   // Xenia's split_path drops empty parts
  const std::string discRoot = contentRootDir() + "Superman Returns (USA)";
  const bool inDisc = !path.empty() && (path == discRoot || path.compare(0, discRoot.size() + 1, discRoot + "/") == 0);
  const bool out = rangeOk(infoVA, 56);
  if (!xeniaValidPathChars(raw, false)) {   // Xenia: `if (!IsValidPath(target_path, false))`
    st = kStNameInvalid;
  } else if (inDisc && path == discRoot) {   // the disc device's root entry: DIRECTORY, everything else 0
    if (out) putOpenInfo(infoVA, 0, 0, 0, 0, kAttrDirectory);
    st = kStSuccess;
  } else if (inDisc) {
    const std::string modFile = modOverlayActive() ? modOverlayResolveFile(path) : std::string();
    const HostStat disc = modFile.empty() ? hostStat(fs::u8path(path)) : HostStat{};
    if (!modFile.empty()) {   // a mod's file shadows (or adds) this disc file
      const HostStat m = hostStat(fs::u8path(modFile));
      if (m.exists && !m.isDir) {
        if (out) putOpenInfo(infoVA, m.ctime, m.atime, m.mtime, m.size, kIsoFileAttr | kAttrReadOnly);
        st = kStSuccess;
      }
    } else if (disc.exists && !disc.isDir) {
      if (out) putOpenInfo(infoVA, kFileTime1970, kFileTime1970, kFileTime1970, disc.size, kIsoFileAttr | kAttrReadOnly);
      st = kStSuccess;
    } else if (disc.exists) {
      if (out)
        putOpenInfo(infoVA, kFileTime1970, kFileTime1970, kFileTime1970, xdvdfsDirLength(discNamespaceListing(path)),
                    kAttrDirectory | kAttrReadOnly);
      st = kStSuccess;
    } else if (const std::string modDir = modOverlayActive() ? modOverlayResolveDir(path) : std::string();
               !modDir.empty()) {   // a folder only a mod provides: the backing folder's times
      const HostStat m = hostStat(fs::u8path(modDir));
      if (out)
        putOpenInfo(infoVA, m.ctime, m.atime, m.mtime, xdvdfsDirLength(discNamespaceListing(path)),
                    kAttrDirectory | kAttrReadOnly);
      st = kStSuccess;
    }
  }
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 64 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[IMPORT_FIXES] ATTRS #%u NtQueryFullAttributesFile(\"%s\") -> %08X lr=%08X", n,
                raw.c_str(), st, getCurrentGuestLR());
  return st;
}

// NtQueryFullAttributesFile(OA r3, FILE_NETWORK_OPEN_INFORMATION* r4).
uint32_t exp_NtQueryFullAttributesFile(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                                       uint32_t r7, uint32_t r8) {
  if (xamContentFsEnabled()) {
    std::lock_guard<std::mutex> lk(g_xcMutex);
    if (!g_knownRoots.empty()) {
      ParsedPath pp;
      const PathKind kind = parsePathLocked(r3, pp);
      if (kind != PathKind::NotOurs) {
        uint32_t st = kStNameNotFound;
        if (kind == PathKind::Invalid) {
          st = kStNameInvalid;
        } else if (kind == PathKind::Ok) {
          fs::path target = g_mounts.at(pp.key).pkg;
          for (const std::string& c : pp.comps) target /= c;
          const HostStat hs = hostStat(target);
          if (hs.exists && rangeOk(r4, 56)) {
            if (pp.comps.empty() && attrsFixOn())   // the device's root entry (host_path_device.cc:42-44): DIRECTORY, 0s
              putOpenInfo(r4, 0, 0, 0, 0, kAttrDirectory);
            else
              putNetworkOpenInfo(r4, hs);
            st = kStSuccess;
          }
        }
        // SR_IMPORT_FIX_ATTRS: Xenia answers NO_SUCH_FILE for any path it does not resolve, a package path too
        if (st == kStNameNotFound && attrsFixOn()) st = kStNoSuchFile;
        XAMC_LOG(kOpNtFullAttr, "NtQueryFullAttributesFile(\"%s\") -> %08X", pp.raw.c_str(), st);
        return st;
      }
    }
  }
  if (attrsFixOn()) return attrsQueryHost(r3, r4);   // step4/audit batch 3 (baked): by real existence
  return callPrev(g_prevNtFullAttr, r3, r4, r5, r6, r7, r8);
}
}  // namespace

bool xamContentTryNtCreateFile(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7,
                               uint32_t r8, uint32_t* status) {
  (void)r7; (void)r8;
  if (!xamContentFsEnabled() || !g_anyRootKnown.load()) return false;
  const uint32_t share = ExportRegistry::getCurrentArgR9();
  const uint32_t disposition = ExportRegistry::getCurrentArgR10();
  return tryCreateCommon("NtCreateFile", r3, r4, r5, r6, share, disposition, stackArg9(), status);
}

bool xamContentTryNtOpenFile(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7,
                             uint32_t r8, uint32_t* status) {
  if (!xamContentFsEnabled() || !g_anyRootKnown.load()) return false;
  // NT order (FileHandle, DesiredAccess, OA, IOSB, ShareAccess, OpenOptions):
  // the guest's DeleteFileA passes share 7 in r7 and options 0x4040 in r8
  // (Xenia's NtOpenFile_entry takes r7 as the options - a Xenia quirk).
  return tryCreateCommon("NtOpenFile", r3, r4, r5, r6, r7, 1u /*FILE_OPEN*/, r8, status);
}

bool xamContentTrySetInformationFile(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                                     uint32_t r7, uint32_t r8, uint32_t* status) {
  (void)r8;
  if (!xamContentFsEnabled() || g_liveCount.load() == 0) return false;
  std::lock_guard<std::mutex> lk(g_xcMutex);
  auto it = g_files.find(r3);
  if (it == g_files.end()) return false;
  XFile& xf = it->second;
  std::FILE* f = xf.isDir ? nullptr : fileHandleGet(r3);
  // iter-463 (f): position / length changes under the handle's I/O lock.
  std::shared_ptr<std::mutex> io = f ? fileHandleIoLock(r3) : nullptr;
  std::unique_lock<std::mutex> ioHold;
  if (io) {
    ioHold = std::unique_lock<std::mutex>(*io);
    f = fileHandleGet(r3);  // re-validate under the lock (file_io.hpp): an NtClose may have won it
  }
  uint32_t st = kStSuccess;
  auto rd64 = [&](uint32_t va) {
    return (uint64_t(readGuestU32BE(va)) << 32) | readGuestU32BE(va + 4);
  };
  switch (r7) {
    case 13:   // FileDispositionInformation {BOOLEAN DeleteFile}
      if (r6 < 1 || !rangeOk(r5, 1)) { st = kStInvalidParameter; break; }
      xf.deleteOnClose = *guestVAToHost(r5) != 0;
      break;
    case 14:   // FilePositionInformation {i64}
      if (!f) { st = kStInvalidParameter; break; }
      if (r6 < 8 || !rangeOk(r5, 8)) { st = kStInvalidParameter; break; }
#ifdef _WIN32
      _fseeki64(f, static_cast<__int64>(rd64(r5)), SEEK_SET);
#else
      std::fseek(f, static_cast<long>(rd64(r5)), SEEK_SET);
#endif
      break;
    case 19:   // FileAllocationInformation: accepted, no-op
    case 4:    // FileBasicInformation: accepted, no-op
      break;
    case 20: {  // FileEndOfFileInformation {i64}
      if (!f) { st = kStInvalidParameter; break; }
      if (r6 < 8 || !rangeOk(r5, 8)) { st = kStInvalidParameter; break; }
      if (!xf.writable) { st = kStAccessDenied; break; }
      std::fflush(f);
#ifdef _WIN32
      if (_chsize_s(_fileno(f), static_cast<__int64>(rd64(r5))) != 0) st = kStDiskFull;
#endif
      break;
    }
    default:
      st = kStInvalidInfoClass;
      break;
  }
  if (r4) {
    wr32(r4, st);
    wr32(r4 + 4, 0);
  }
  XAMC_LOG(kOpNtSetInfo, "NtSetInformationFile(h=%u class=%u len=%u) -> %08X deleteOnClose=%d", r3,
           r7, r6, st, xf.deleteOnClose ? 1 : 0);
  *status = st;
  return true;
}

bool xamContentTryQueryDirectoryFile(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                                     uint32_t r7, uint32_t r8, uint32_t* status) {
  (void)r4; (void)r5; (void)r6;
  if (!xamContentFsEnabled() || g_liveCount.load() == 0) return false;
  const uint32_t len = ExportRegistry::getCurrentArgR9();
  const uint32_t maskVA = ExportRegistry::getCurrentArgR10();
  std::lock_guard<std::mutex> lk(g_xcMutex);
  auto it = g_files.find(r3);
  if (it == g_files.end() || !it->second.isDir) return false;
  XFile& xf = it->second;
  if (len < kDirInfoMinLength) {
    *status = kStInfoLengthMismatch;
    return true;
  }
  std::string mask;
  if (maskVA && rangeOk(maskVA, 8)) {
    const uint16_t mlen = uint16_t(rd32(maskVA) >> 16);
    const uint32_t mbuf = rd32(maskVA + 4);
    if (mlen && rangeOk(mbuf, mlen)) mask.assign(reinterpret_cast<const char*>(guestVAToHost(mbuf)), mlen);
  }
  const bool restart = stackArg9() != 0;
  uint32_t st;
  size_t found = size_t(-1);
  if (!xeniaValidPathChars(mask, true) || mask.find_first_of("/\\") != std::string::npos) {
    st = kStInvalidParameter;
  } else if (!mask.empty()) {
    // A new rule always restarts (xfile.cc QueryDirectory).
    xf.rule = mask;
    relistLocked(xf);
    xf.next = 0;
    for (size_t i = 0; i < xf.ents.size(); ++i)
      if (wildcardMatch(xf.rule, xf.ents[i].name)) { found = i; break; }
    st = found == size_t(-1) ? kStNoSuchFile : kStSuccess;
  } else {
    if (restart || !xf.listed) {
      relistLocked(xf);
      xf.next = 0;
    }
    for (size_t i = xf.next; i < xf.ents.size(); ++i)
      if (wildcardMatch(xf.rule, xf.ents[i].name)) { found = i; break; }
    st = found == size_t(-1) ? kStNoMoreFiles : kStSuccess;
  }
  if (st == kStSuccess) {
    const DirEnt& e = xf.ents[found];
    if (kDirInfoHeader + e.name.size() > len || !rangeOk(r8, uint32_t(kDirInfoHeader + e.name.size()))) {
      st = kStNoSuchFile;   // Xenia: "Buffer overflow?"
    } else {
      xf.next = found + 1;
      writeDirInfo(r8, uint32_t(xf.next), e);
    }
  }
  if (r7) {
    wr32(r7, st);
    wr32(r7 + 4, st == kStSuccess ? len : 0);
  }
  XAMC_LOG(kOpNtQueryDir, "NtQueryDirectoryFile(h=%u mask='%s' restart=%d) -> %08X entry='%s'", r3,
           mask.c_str(), restart ? 1 : 0, st,
           st == kStSuccess ? xf.ents[found].name.c_str() : "");
  *status = st;
  return true;
}

void xamContentOnNtClose(uint32_t handle) {
  if (g_liveCount.load(std::memory_order_relaxed) == 0) return;
  std::lock_guard<std::mutex> lk(g_xcMutex);
  if (g_enums.erase(handle)) {
    updateLive();
    return;
  }
  auto it = g_files.find(handle);
  if (it == g_files.end()) return;
  // stub_NtClose already ran fileHandleClose (fclose): do not close again.
  releaseFileLocked(handle, it->second, /*alreadyClosed=*/true);
  g_files.erase(it);
  updateLive();
}

void xamContentResetForTest() {
  std::lock_guard<std::mutex> lk(g_xcMutex);
  for (auto& kv : g_files) fileHandleClose(kv.first);
  g_files.clear();
  g_enums.clear();
  g_mounts.clear();
  g_knownRoots.clear();
  g_anyRootKnown.store(false);
  updateLive();
}

uint32_t xamContentMountCountForTest() {
  std::lock_guard<std::mutex> lk(g_xcMutex);
  return uint32_t(g_mounts.size());
}

void registerXamContentExports(ExportRegistry& reg) {
  if (!fsEnvOn()) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[XAM_CONTENT] SR_XAM_CONTENT_FS=0: the old XamContent/NtWriteFile stubs stay");
    return;
  }
  auto swap = [&](const char* name, ExportFn& prev, ExportFn mine) {
    prev = reg.getExport(name);
    reg.registerExport(name, mine);
  };
  swap("XamContentCreateEx", g_prevCreateEx, exp_XamContentCreateEx);
  swap("XamContentClose", g_prevClose, exp_XamContentClose);
  swap("XamContentFlush", g_prevFlush, exp_XamContentFlush);
  swap("XamContentDelete", g_prevDelete, exp_XamContentDelete);
  swap("XamContentGetCreator", g_prevGetCreator, exp_XamContentGetCreator);
  swap("XamContentSetThumbnail", g_prevSetThumbnail, exp_XamContentSetThumbnail);
  swap("XamContentCreateEnumerator", g_prevCreateEnum, exp_XamContentCreateEnumerator);
  swap("XamEnumerate", g_prevEnumerate, exp_XamEnumerate);
  swap("NtWriteFile", g_prevNtWriteFile, exp_NtWriteFile);
  swap("NtFlushBuffersFile", g_prevNtFlush, exp_NtFlushBuffersFile);
  swap("NtQueryFullAttributesFile", g_prevNtFullAttr, exp_NtQueryFullAttributesFile);
  swap("RtlNtStatusToDosError", g_prevDosErr, exp_RtlNtStatusToDosError);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[XAM_CONTENT] SR_XAM_CONTENT_FS on: real save storage under \"%s\" (%s) "
              "(SR_NTSTATUS_DOSERR %s)",
              u8(xamContentRoot()).c_str(), xamContentRootSource(), dosEnvOn() ? "on" : "off");
}

}  // namespace kernel
}  // namespace sr
