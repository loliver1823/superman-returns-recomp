// iter-464 modcore: the mod manager (mod_manager.hpp, docs/MODS.md). The reference for every
// rule here is tools/srmods/srmods.py (same decisions, same file formats).
#include "mod_manager.hpp"

#include "file_io.hpp"
#include "map_markers.hpp"
#include "mod_formats.hpp"
#include "mod_costume_data.hpp"
#include "mod_costume_preview.hpp"
#include "mod_density.hpp"
#include "mod_menu.hpp"
#include "mod_zip.hpp"
#include "asset_receipt.hpp"
#include "xam_content.hpp"
#include "host_automation.hpp"
#include "../app/log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace sr {
namespace kernel {

namespace fs = std::filesystem;

namespace {

constexpr const char* kDiscDir = "Superman Returns (USA)";
constexpr const char* kGameTag = "SRX360-USA";
// tools/srmods/srmods.py USA_RETAIL_FINGERPRINT (python tools/srmods/srmods.py fingerprint).
constexpr const char* kUsaRetailFingerprint = "fe43807d8d15db16";
const char* const kStatusNames[] = {"ok", "disabled", "bad-manifest", "bad-zip", "wrong-game",
                                    "missing-dependency", "declared-conflict", "bad-patch", "bad-content",
                                    "duplicate", "reserved", "off"};
// The 29 extensions the DATA\_workinprogress loose storage registers (table 0x82911D00,
// the owner's mod-system research, not published).
const char* const kLooseExts[] = {"dds", "wmv", "fxh", "fx", "rss", "rml", "spk", "sns", "sgp", "sbm",
                                  "bmp", "pus", "map", "loc", "png", "lef", "igc", "bnk", "rcb", "txt",
                                  "ter", "tga", "xml", "das", "rx2", "hkx", "lua", "ast", "vxd"};

struct Mgr {
  std::mutex mu;
  ModBootEnv env;
  std::string discRoot, modsDir, cacheDir, source;
  std::vector<ModInfo> mods;              // the load order, top first
  std::vector<PortFeatureInfo> feats;
  std::vector<ModLayer> layers;           // mounted, highest priority first
  std::vector<ModConflictInfo> conflicts;
  std::unordered_set<std::string> hidden; // lower generic host paths of conflict losers
  // the saved state (mods.json) and what this boot used
  std::vector<std::string> order;         // ids, top first
  std::set<std::string> disabled;         // lower ids
  std::vector<std::string> bootOrder;
  std::set<std::string> bootDisabled;
  std::set<std::string> disabledCostumes, bootDisabledCostumes;
  bool fromState = false;                 // the player's state (menu changes are saved)
  bool booted = false;
  bool costumeRegistryReady = false;
  bool costumePreviews = false;            // generated Fortress COSTUMES movies are mounted
  std::string selectedCostume, bootCostume; // stable ids, never persisted numeric slots
  std::string wornCostume;                  // set this session by modCostumeNoteWorn
  std::string sessionFamily = "retail";     // the animation family this session loaded (boot)
  uint64_t charAnimBytes = 0;               // own-project characters preloaded at boot (bank bytes)
  bool builtinBizarro = false;              // the built-in Bizarro character is in the registry
  bool convertBankSuits = false;            // own-bank suits are worn as characters (no restart)
  ModDensity densityApplied;                // iter-466: the traffic document this boot serves
  bool densityOn = false;                   // iter-466: the density port feature is on this boot
};

bool g_testBoot = false;  // modManagerResetForTest: built-in characters only when asked for

Mgr& mgr() {
  static Mgr m;
  return m;
}
std::once_flag g_bootOnce;
bool (*g_spawnHook)(std::string*) = nullptr;

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

bool validCostumeId(const std::string& id) {
  const size_t slash = id.find('/');
  return slash != std::string::npos && modValidId(id.substr(0, slash)) &&
         modValidId(id.substr(slash + 1));
}

std::vector<ModCostumeInfo> costumesLocked(const Mgr& g) {
  std::vector<ModCostumeInfo> out;
  for (const auto& m : g.mods)
    if (m.mounted && m.status == ModStatus::Ok)
      for (const auto& c : m.costumes)
        if (!g.bootDisabledCostumes.count(c.id)) out.push_back(c);
  // iter-465: the list reads by DISPLAY NAME (case-insensitive), the stable id breaks ties. Saves
  // and the worn note keep the stable id, so nothing persisted depends on this order or the slots.
  auto upper = [](const std::string& s) {
    std::string u = s;
    for (auto& ch : u) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return u;
  };
  std::sort(out.begin(), out.end(), [&](const auto& a, const auto& b) {
    const std::string na = upper(a.name), nb = upper(b.name);
    return na != nb ? na < nb : a.id < b.id;
  });
  for (size_t i = 0; i < out.size(); ++i) out[i].slot = 5 + static_cast<int>(i);
  if (g.convertBankSuits) {
    // iter-465 charswitch: a suit with its own banks ("animation_banks") is worn at once as a
    // character: its entity copy <entity>_SRCHAR runs on project SRM_FAM_<family>_ANIMS, which the
    // boot generates from the suit's banks (one project per bank family, shared by its suits).
    for (auto& c : out) {
      if (c.banks.empty() || c.bankFamily.size() < 8 || c.entity.size() > 48) continue;
      const std::string fam = "SRM_FAM_" + c.bankFamily.substr(c.bankFamily.size() - 16) + "_";
      c.character = true;
      c.entity += "_SRCHAR";
      c.preloads.emplace_back("ANIM", fam + "ANIMS");
    }
  }
  if (g.builtinBizarro) {
    // iter-465 built-in Bizarro (port feature, docs/MODS.md "Character switch"): the game's own
    // PlayableBizarroEnt, served from the player's own disc (PLAYABLEBIZ bundle) at boot.
    ModCostumeInfo c;
    c.id = "builtin/bizarro";
    c.localId = "bizarro";
    c.name = "BIZARRO";
    c.source = "builtin";
    c.entity = "PlayableBizarroEnt";
    c.description = "Bizarro, the game's own playable Bizarro, free in Metropolis and every mission.";
    c.character = true;
    c.preloads = {{"ANIM", "BizarroExtraAnims"}, {"ANIM", "SingleJointAnims"}, {"ANIM", "BIZ01SupermanAnims"},
                  {"ARENA", "PLAYABLEBIZ_TEXTURE"}, {"ARENA", "PLAYABLEBIZ_SHADER"}, {"ARENA", "PLAYABLEBIZVXD_RS"}};
    c.slot = 5 + static_cast<int>(out.size());
    out.push_back(std::move(c));
  }
  return out;
}

// The PLAYABLEBIZ bundle entries Bizarro needs outside his mission, as loose files (the engine's
// own _workinprogress loader). Names Superman's flows already own (SUPERMANCCTPROJECT,
// SUPERMANJOINTAIMER, the shared power includes, SupermanFSM graphs) are left out so a loose copy
// never shadows his. Compiled XML is served as the equivalent text document.
struct BuiltinEntry { const char* part; const char* name; const char* ext; };
const BuiltinEntry kBizarroEntries[] = {
    {"PLAYABLEBIZ_OTHER", "PLAYABLEBIZARROENT", "xml"}, {"PLAYABLEBIZ_OTHER", "UNPLAYABLEBIZARROCLOTHCAPE", "xml"},
    {"PLAYABLEBIZ_OTHER", "PLAYABLEBIZARROPOWERS", "xml"}, {"PLAYABLEBIZ_OTHER", "DAMAGEVOLUMEBASEPOWERINCLUDE", "xml"},
    {"PLAYABLEBIZ_OTHER", "COLDVISIONINCLUDE", "xml"}, {"PLAYABLEBIZ_OTHER", "HEATBREATHINCLUDE", "xml"},
    {"PLAYABLEBIZ_OTHER", "VACUUMBREATHINCLUDE", "xml"}, {"PLAYABLEBIZ_OTHER", "BLINDVISIONINCLUDE", "xml"},
    {"PLAYABLEBIZ_OTHER", "PLAYABLEBIZARROTARGETSYS", "xml"}, {"PLAYABLEBIZ_OTHER", "BIZARROEXTRAANIMS", "xml"},
    {"PLAYABLEBIZ_OTHER", "BIZ01SUPERMANANIMS", "xml"}, {"PLAYABLEBIZ_OTHER", "SINGLEJOINTANIMS", "xml"},
    {"PLAYABLEBIZ_OTHER", "PLAYABLEBIZVXD_RS_MAP", "xml"}, {"PLAYABLEBIZ_OTHER", "BIZARRO_SFX_PRELOAD", "xml"},
    {"PLAYABLEBIZ_OTHER", "X2_BIZARRO_BOOTSUNDIESCAPE_COL", "dds"}, {"PLAYABLEBIZ_OTHER", "PLAYABLEBIZVXD", "vxd"},
    {"PLAYABLEBIZ_ANIM", "BIZARRO_EXTRA", "bnk"}, {"PLAYABLEBIZ_ANIM", "SINGLEJOINT_MIM_BIZ", "bnk"},
    {"PLAYABLEBIZ_ANIM", "SUPERMAN_MIM_BIZ", "bnk"}, {"PLAYABLEBIZ_ANIM", "SINGLEJOINTCCTPROJECT", "rcb"},
    {"PLAYABLEBIZ_RENDER", "X2_BIZARRO", "rx2"}, {"PLAYABLEBIZ_RENDER", "PLAYABLEBIZ_TEXTURE", "rx2"},
    {"PLAYABLEBIZ_RENDER", "PLAYABLEBIZ_SHADER", "rx2"}, {"PLAYABLEBIZ_RENDER", "PLAYABLEBIZVXD_RS", "rx2"},
};

bool extractBuiltinBizarro(const std::string& discRoot, std::map<std::string, std::vector<uint8_t>>* files,
                           uint64_t* bankBytes, std::string* why) {
  std::vector<uint8_t> top;
  if (!modReadTopEntry(discRoot + "/DATA/objtiv.AST", "PLAYABLEBIZ", &top, why)) return false;
  std::map<std::string, std::vector<uint8_t>> parts;
  for (const auto& e : kBizarroEntries) {
    auto it = parts.find(e.part);
    if (it == parts.end()) {
      std::vector<uint8_t> part;
      if (!modReadNestedEntry(top, e.part, &part, why)) { *why = std::string(e.part) + ": " + *why; return false; }
      it = parts.emplace(e.part, std::move(part)).first;
    }
    std::vector<uint8_t> bytes, text;
    if (!modReadNestedEntry(it->second, e.name, &bytes, why)) { *why = std::string(e.name) + ": " + *why; return false; }
    if (std::strcmp(e.ext, "xml") == 0) {
      if (!modCompiledXmlToText(bytes, &text, why)) { *why = std::string(e.name) + ": " + *why; return false; }
      bytes.swap(text);
    }
    if (std::strcmp(e.ext, "bnk") == 0 || std::strcmp(e.ext, "rcb") == 0) *bankBytes += bytes.size();
    (*files)[std::string(e.name) + "." + e.ext] = std::move(bytes);
  }
  return true;
}

bool readFile(const fs::path& p, std::string* out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  out->assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return true;
}
bool readBytes(const fs::path& p, std::vector<uint8_t>* out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  out->assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return true;
}
bool writeFileAtomic(const fs::path& p, const std::string& data) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  fs::path tmp = p;
  tmp += ".tmp";
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!o) return false;
  }
  fs::rename(tmp, p, ec);
  return !ec;
}

std::string u8(const fs::path& p) { return modPathU8(p); }

// A path inside a mod: relative, no '..' / '.', no ':' or drive, '/'-joined; "" if unsafe.
std::string safeRel(const std::string& in) {
  if (in.empty() || in[0] == '/' || in[0] == '\\' || in.find(':') != std::string::npos) return "";
  std::string out, comp;
  auto flush = [&]() -> bool {
    if (comp.empty()) return true;
    if (comp == "." || comp == "..") return false;
    out += (out.empty() ? "" : "/") + comp;
    comp.clear();
    return true;
  };
  for (char c : in) {
    if (c == '/' || c == '\\') { if (!flush()) return ""; }
    else comp += c;
  }
  if (!flush()) return "";
  return out;
}

std::vector<std::string> splitList(const std::string& s) {
  std::vector<std::string> r;
  std::string cur;
  for (char c : s) {
    if (c == ';' || c == ',') { if (!trim(cur).empty()) r.push_back(trim(cur)); cur.clear(); }
    else cur += c;
  }
  if (!trim(cur).empty()) r.push_back(trim(cur));
  return r;
}

void fail(ModInfo& m, ModStatus st, const std::string& why) {
  if (m.status == ModStatus::Ok) {
    m.status = st;
    m.reason = why;
  }
}

// ---------------------------------------------------------------- BGFA top-level read
uint64_t leN(const uint8_t* p, int w) {
  uint64_t v = 0;
  for (int i = w - 1; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

struct TopEntry { uint64_t off = 0, size = 0; };
bool findTopEntry(std::FILE* f, const std::string& want, TopEntry* out, std::string* err) {
  uint8_t h[0x40];
  _fseeki64(f, 0, SEEK_SET);
  if (std::fread(h, 1, 0x40, f) != 0x40 || std::memcmp(h, "BGFA1.05", 8) != 0) {
    *err = "not a BGFA1.05 container";
    return false;
  }
  const uint32_t count = static_cast<uint32_t>(leN(h + 0x0C, 4)), tocOff = static_cast<uint32_t>(leN(h + 0x10, 4));
  const int wA = h[0x21], wId = h[0x22], wOff = h[0x23], wSize = h[0x24], wUnc = h[0x25], shift = h[0x26];
  const uint32_t ntags = static_cast<uint32_t>(leN(h + 0x28, 4)), namelen = static_cast<uint32_t>(leN(h + 0x2C, 4));
  if (wId > 8 || wOff > 8 || wSize > 8 || wUnc > 8 || wA > 8 || namelen > 4096) {
    *err = "container header out of range";
    return false;
  }
  const size_t rec = 1u + wA + wId + wOff + wSize + wUnc + namelen;
  std::vector<uint8_t> toc(4ull * ntags + rec * count);
  _fseeki64(f, tocOff, SEEK_SET);
  if (std::fread(toc.data(), 1, toc.size(), f) != toc.size()) {
    *err = "container TOC truncated";
    return false;
  }
  bool byId = false;
  uint64_t id = 0;
  if (!want.empty() && want[0] == '#') {
    byId = true;
    id = std::strtoull(want.c_str() + 1, nullptr, 16);
  } else {
    id = fnv1a64Upper(want);
  }
  const uint64_t mask = wId >= 8 ? ~0ull : ((1ull << (8 * wId)) - 1);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* r = &toc[4ull * ntags + rec * i];
    const uint8_t* q = r + 1 + wA;
    const uint64_t eid = leN(q, wId);
    q += wId;
    const uint64_t off = leN(q, wOff) << shift;
    q += wOff;
    const uint64_t size = leN(q, wSize);
    q += wSize + wUnc;
    bool hit = (eid == (id & mask));
    if (!byId && namelen && !hit) {
      std::string nm(reinterpret_cast<const char*>(q), strnlen(reinterpret_cast<const char*>(q), namelen));
      hit = modLower(nm) == modLower(want);
    }
    if (hit) {
      out->off = off;
      out->size = size;
      return true;
    }
  }
  *err = "no entry '" + want + "'";
  return false;
}

std::FILE* openRead(const fs::path& p) {
  std::FILE* f = nullptr;
#ifdef _WIN32
  if (_wfopen_s(&f, p.wstring().c_str(), L"rb") != 0) f = nullptr;
#else
  f = std::fopen(p.string().c_str(), "rb");
#endif
  return f;
}

bool tocSha256(const fs::path& p, uint64_t* size, std::string* hex) {
  std::FILE* f = openRead(p);
  if (!f) return false;
  uint8_t h[0x40];
  bool ok = std::fread(h, 1, 0x40, f) == 0x40;
  std::vector<uint8_t> toc;
  if (ok) {
    const uint32_t tocOff = static_cast<uint32_t>(leN(h + 0x10, 4)), tocLen = static_cast<uint32_t>(leN(h + 0x18, 4));
    toc.resize(tocLen);
    _fseeki64(f, tocOff, SEEK_SET);
    ok = std::fread(toc.data(), 1, tocLen, f) == tocLen;
  }
  _fseeki64(f, 0, SEEK_END);
  *size = static_cast<uint64_t>(_ftelli64(f));
  std::fclose(f);
  if (!ok) return false;
  Sha256 s;
  s.update(h, 0x40);
  s.update(toc.data(), toc.size());
  uint8_t d[32];
  s.final(d);
  *hex = hexOf(d, 32);
  return true;
}

// Lane E E2 (Astra r1): a link (symlink / junction) is never recursively removed or replaced here.
bool isLinkPath(const fs::path& p) {
#ifdef _WIN32
  const DWORD a = GetFileAttributesW(p.wstring().c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  std::error_code ec;
  return fs::is_symlink(fs::symlink_status(p, ec));
#endif
}

// Lane E E2 (Astra r1-r2): the cache's cross-process protocol, two small files per generation tag in <cache>/gen:
//   <tag>.lock  EXCLUSIVE while one process generates and publishes <tag> (the marker re-check, the clean-up of its
//               temp folder and of a marker-less same-name folder, the rename); marked for deletion only as it is
//               closed, after the work (Astra E2 r3: a delete-pending file answers ACCESS_DENIED, not a sharing
//               violation, so a lock marked at its open would turn a contender's 30 s wait into a fast failure).
//   <tag>.use   SHARED by every process that mounted <tag>, for the rest of that process (the overlay serves the
//               generation's files lazily); taken BEFORE the marker check, so before the .lock. A collector removes
//               another tag's folders, its leftover .lock (a generator that died) and then the .use itself only while
//               it holds that tag's .use EXCLUSIVELY, so never one that a process mounted or is generating.
// Both are opened without following a link and refused when they are not a plain file. Windows only (share modes);
// elsewhere a generation is refused, never published without the protocol (Astra E2 r3).
#ifdef _WIN32
HANDLE openPlainFile(const fs::path& p, DWORD access, DWORD share, DWORD* lastErr) {
  HANDLE h = CreateFileW(p.wstring().c_str(), access, share, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    *lastErr = GetLastError();
    return h;
  }
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(h, &info) ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
    CloseHandle(h);
    *lastErr = ERROR_CANT_ACCESS_FILE;
    return INVALID_HANDLE_VALUE;
  }
  return h;
}
void deleteOnClose(HANDLE h) {
  FILE_DISPOSITION_INFO d{};
  d.DeleteFile = TRUE;
  SetFileInformationByHandle(h, FileDispositionInfo, &d, sizeof d);   // best effort: a leftover file is harmless
}
std::string winError(DWORD e) { return std::system_category().message(int(e)); }
// Contention (another process holds the file) waits up to 30 s; a delete-pending file (ACCESS_DENIED while its
// holder, which marks it only as it closes it, finishes closing) is retried briefly - up to 5 times IN A ROW, at any
// point of the 30 s (Astra E2 r4: a mark-and-close at the end of a long wait); any other error is final, reported as
// the OS says it.
template <typename F>
bool retryOpen(F open, std::string* why) {
  int deniedInARow = 0;
  for (int i = 0; i < 300; ++i) {
    DWORD e = 0;
    if (open(&e)) return true;
    deniedInARow = e == ERROR_ACCESS_DENIED ? deniedInARow + 1 : 0;
    if (e == ERROR_SHARING_VIOLATION || (e == ERROR_ACCESS_DENIED && deniedInARow <= 5)) {
      Sleep(100);
      continue;
    }
    *why = winError(e);
    return false;
  }
  *why = "another process holds it (30 s)";
  return false;
}
#endif

std::mutex g_genUseMu;
std::vector<void*> g_genUses;   // the .use handles this process holds (HANDLE)

bool holdGenerationUse(const fs::path& usePath, std::string* why) {
#ifdef _WIN32
  HANDLE h = INVALID_HANDLE_VALUE;
  if (!retryOpen([&](DWORD* e) {
        // shared with other users; deletable (a user clearing the cache), but no collector's exclusive open succeeds
        h = openPlainFile(usePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, e);
        return h != INVALID_HANDLE_VALUE;
      }, why))
    return false;
  std::lock_guard<std::mutex> lk(g_genUseMu);
  g_genUses.push_back(h);
  return true;
#else
  (void)usePath;
  *why = "the generation cache needs Windows file sharing (not available on this platform)";
  return false;
#endif
}

// Test seam (modManagerResetForTest): a re-boot in the same process replaces the mounted set, so the previous boot's
// uses end (a real process boots once and keeps its uses until it exits).
void releaseGenerationUses() {
  std::lock_guard<std::mutex> lk(g_genUseMu);
#ifdef _WIN32
  for (void* h : g_genUses) CloseHandle(static_cast<HANDLE>(h));
#endif
  g_genUses.clear();
}

class GenLock {
 public:
  GenLock(const fs::path& p, std::string* why) {
#ifdef _WIN32
    retryOpen([&](DWORD* e) {
      h_ = openPlainFile(p, GENERIC_READ | GENERIC_WRITE | DELETE, 0, e);
      return h_ != INVALID_HANDLE_VALUE;
    }, why);
#else
    (void)p;
    *why = "the generation cache needs Windows file sharing (not available on this platform)";
#endif
  }
  ~GenLock() {
#ifdef _WIN32
    if (h_ != INVALID_HANDLE_VALUE) {
      deleteOnClose(h_);   // only now: a contender waits on the sharing violation until this close
      CloseHandle(h_);
    }
#endif
  }
  GenLock(const GenLock&) = delete;
  GenLock& operator=(const GenLock&) = delete;
  bool held() const {
#ifdef _WIN32
    return h_ != INVALID_HANDLE_VALUE;
#else
    return false;
#endif
  }

 private:
#ifdef _WIN32
  HANDLE h_ = INVALID_HANDLE_VALUE;
#endif
};

// Removes everything of another generation <otherTag> when no process uses it: its folder, its temp folder, a
// leftover .lock (a generator that died; a live generator holds the .use first) and last its .use. Only while
// holding <otherTag>.use exclusively, never through a link. Quietly keeps it otherwise (a later boot collects it).
// A .use without a folder (a generation that failed before its temp folder) goes the same way (Astra E2 r3).
// The folder's .complete goes FIRST, and is verified gone, before anything else is touched: a removal that stops
// half way (a file held open, a crash) must never leave a partial folder that still claims to be complete. When the
// marker cannot be removed, the tag is left exactly as it was (Astra E2 r4).
void collectGeneration(const fs::path& genRoot, const std::string& otherTag) {
#ifdef _WIN32
  DWORD e = 0;
  HANDLE h = openPlainFile(genRoot / fs::u8path(otherTag + ".use"), GENERIC_READ | DELETE, 0, &e);
  if (h == INVALID_HANDLE_VALUE) return;   // in use (or not ours to judge): keep it
  {
    const fs::path dir = genRoot / fs::u8path(otherTag);
    const fs::path mk = dir / ".complete";
    std::error_code ec, ec2;
    const bool link = isLinkPath(dir);   // a link is never entered (the loop below skips it too)
    const fs::file_status st = link ? fs::file_status{} : fs::status(dir, ec);
    // no folder (an orphan .use, or one removed since the listing): nothing to invalidate - "not found" may come
    // with an error code, which is absence, not an inspection failure (Astra E2 r5)
    if (st.type() == fs::file_type::not_found) ec.clear();
    const bool isDir = !link && !ec && st.type() == fs::file_type::directory;
    if (isDir) fs::remove(mk, ec);
    const bool gone = !ec && (!isDir || (!fs::exists(mk, ec2) && !ec2));
    if (!link && !gone) {
      const std::string why = ec ? ec.message() : ec2 ? ec2.message() : std::string("still present");
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[MODS] cache: kept the unused generation %s (its marker: %s)",
                  u8(dir).c_str(), why.c_str());
      CloseHandle(h);   // the .use stays: nothing of this tag changed
      return;
    }
  }
  for (const fs::path& d : {genRoot / fs::u8path(otherTag), genRoot / fs::u8path(otherTag + ".tmp")}) {
    std::error_code ec;
    if (isLinkPath(d)) continue;
    fs::remove_all(d, ec);
    if (ec)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[MODS] cache: could not remove the unused generation %s: %s", u8(d).c_str(),
                  ec.message().c_str());
  }
  std::error_code ec;
  fs::remove(genRoot / fs::u8path(otherTag + ".lock"), ec);   // a plain remove: a link is removed, never followed
  deleteOnClose(h);
  CloseHandle(h);
#else
  (void)genRoot;
  (void)otherTag;
#endif
}

// A layer id that can name a cache folder: "port-<id>" / "mod-<id>" with the id's own rule (modValidId: letters,
// digits, '_', '-', '.', no leading '.'), so no separators and no traversal; the prefix makes it longer than an id.
bool validLayerId(const std::string& n) {
  if (n.empty() || n.size() > 80 || n[0] == '.') return false;
  for (char c : n)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) return false;
  return true;
}

// Another generation of the same layer: the entry "<layerId>-<hex>" (a folder), or its ".tmp" folder, ".use" or
// ".lock" file, the hex as long as this tag's; *other = that generation's tag. Never this tag, never another layer
// whose id extends this one (its tags are longer).
bool otherGenerationTag(const std::string& n, const std::string& layerId, const std::string& tag, std::string* other) {
  const std::string prefix = layerId + "-";
  if (n.rfind(prefix, 0) != 0) return false;
  std::string t = n;
  for (const char* suffix : {".tmp", ".use", ".lock"}) {
    const size_t k = std::strlen(suffix);
    if (t.size() > k && t.compare(t.size() - k, k, suffix) == 0) {
      t.resize(t.size() - k);
      break;
    }
  }
  if (t == tag || t.size() != tag.size()) return false;
  for (size_t i = prefix.size(); i < t.size(); ++i)
    if (!std::isxdigit(static_cast<unsigned char>(t[i]))) return false;
  *other = t;
  return true;
}

// Generate the patched assets of one layer into <cache>/gen/<id>-<hash>/ (a mirror root).
// "" + *err on failure.
std::string generateAssets(const std::string& layerId, const std::vector<ModGenAsset>& assets,
                           const std::string& discRoot, const std::string& cacheDir, std::string* err) {
  // Lane E E2 (Astra r1): the tag names a folder that is removed and replaced below; no separators, no traversal.
  if (!validLayerId(layerId)) {
    *err = "invalid layer id '" + layerId + "'";
    return "";
  }
  // the cache key: every asset's path, base and patch bytes
  Sha256 key;
  std::vector<std::vector<uint8_t>> patches(assets.size());
  for (size_t i = 0; i < assets.size(); ++i) {
    if (!readBytes(fs::u8path(assets[i].patchPath), &patches[i])) {
      *err = "cannot read " + assets[i].patchPath;
      return "";
    }
    const std::string k = assets[i].asset + "|" + assets[i].baseFile + "|" + assets[i].baseEntry + "|";
    key.update(k.data(), k.size());
    key.update(patches[i].data(), patches[i].size());
  }
  uint8_t d[32];
  key.final(d);
  const std::string tag = layerId + "-" + hexOf(d, 8);
  const fs::path genRoot = fs::u8path(cacheDir) / "gen";
  const fs::path dir = genRoot / fs::u8path(tag);
  std::error_code ec;
  // Lane E E2 (Astra r1-r2): the cache protocol above - this process's use of <tag> first (so no collector can remove
  // it from the marker check on), then, to generate, the tag's lock and the marker check again (another boot may have
  // published it while we waited). Every inspection error is an error.
  fs::create_directories(genRoot, ec);
  if (ec) {   // only the error-code overloads here: a filesystem error is a diagnostic, never an exception (Astra E2 r3)
    std::error_code ec2;
    if (!fs::is_directory(genRoot, ec2)) {
      *err = "cannot create " + u8(genRoot) + ": " + ec.message();
      return "";
    }
  }
  if (isLinkPath(genRoot)) {
    *err = "the cache folder " + u8(genRoot) + " is a link: not used";
    return "";
  }
  std::string why;
  if (!holdGenerationUse(genRoot / fs::u8path(tag + ".use"), &why)) {
    *err = "the cache " + u8(genRoot) + ": " + tag + ".use: " + why;
    return "";
  }
  ec.clear();
  bool done = fs::exists(dir / ".complete", ec);
  if (ec) {
    *err = "cannot inspect " + u8(dir) + ": " + ec.message();
    return "";
  }
  if (done) return u8(dir);
  const GenLock lock(genRoot / fs::u8path(tag + ".lock"), &why);
  if (!lock.held()) {
    *err = "the cache " + u8(genRoot) + ": " + tag + ".lock: " + why;
    return "";
  }
  done = fs::exists(dir / ".complete", ec);
  if (ec) {
    *err = "cannot inspect " + u8(dir) + ": " + ec.message();
    return "";
  }
  if (done) return u8(dir);
  // unused generations of this layer (exactly "<layerId>-<hex>" and its .tmp / .use / .lock: never another layer
  // whose id extends this one), each only while no process uses it (collectGeneration). Listed first, then
  // collected (each tag once); the listing advances with the error-code increment (a range-for throws).
  {
    std::set<std::string> others;
    std::error_code lec;
    for (fs::directory_iterator it(genRoot, lec), end; !lec && it != end; it.increment(lec)) {
      std::string o;
      if (otherGenerationTag(modPathU8(it->path().filename()), layerId, tag, &o)) others.insert(o);
    }
    for (const std::string& o : others) collectGeneration(genRoot, o);
  }
  const fs::path tmp = genRoot / fs::u8path(tag + ".tmp");
  ec.clear();
  if (isLinkPath(tmp)) {
    *err = u8(tmp) + " is a link: not used";
    return "";
  }
  fs::remove_all(tmp, ec);
  if (ec) {
    *err = "cannot clear " + u8(tmp) + ": " + ec.message();
    return "";
  }
  for (size_t i = 0; i < assets.size(); ++i) {
    const ModGenAsset& a = assets[i];
    const fs::path base = fs::u8path(discRoot) / fs::u8path(a.baseFile);
    std::FILE* f = openRead(base);
    if (!f) {
      *err = a.baseFile + " is missing";
      return "";
    }
    struct Closer { std::FILE* f; ~Closer() { std::fclose(f); } } closer{f};
    TopEntry te;
    if (!findTopEntry(f, a.baseEntry, &te, err)) {
      *err = a.baseFile + ": " + *err;
      return "";
    }
    std::vector<uint8_t> entry(static_cast<size_t>(te.size));
    _fseeki64(f, static_cast<int64_t>(te.off), SEEK_SET);
    if (std::fread(entry.data(), 1, entry.size(), f) != entry.size()) {
      *err = a.baseFile + ": entry truncated";
      return "";
    }
    SrPatch sp;
    if (!srpatchParse(patches[i], &sp, err)) {
      *err = a.patchPath + ": " + *err;
      return "";
    }
    SrFileReader rd = [f](uint64_t off, uint64_t n, uint8_t* dst) -> bool {
      _fseeki64(f, static_cast<int64_t>(off), SEEK_SET);
      return std::fread(dst, 1, static_cast<size_t>(n), f) == n;
    };
    std::vector<uint8_t> out;
    if (!srpatchApply(patches[i], sp, entry, rd, &out, err)) {
      *err = a.asset + ": " + *err + " (base " + a.baseFile + ":" + a.baseEntry + ")";
      return "";
    }
    const fs::path o = tmp / fs::u8path(a.asset);
    fs::create_directories(o.parent_path(), ec);
    std::ofstream of(o, std::ios::binary | std::ios::trunc);
    of.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!of) {
      *err = "cannot write " + u8(o);
      return "";
    }
  }
  {
    std::ofstream mk(tmp / ".complete", std::ios::binary | std::ios::trunc);
    mk << "ok\n";
    mk.close();
    if (!mk) {   // an unmarked generation must never be published (Astra E2 r2)
      *err = "cannot write " + u8(tmp / ".complete");
      return "";
    }
  }
  // Lane E (step 5, E2): a same-name generation WITHOUT .complete (a build from before the marker wrote straight into
  // it) made this rename fail on every boot - Windows does not replace a non-empty directory - and the layer fell back
  // silently each time (PROGRESS.md "port-feature cache"). It is incomplete by definition: the marker check above ran
  // under the lock, and every writer publishes a generation only as a renamed temp folder that already holds its
  // marker. So it goes first; a link is never followed, and a failed removal is an error, not a silent fall-back.
  ec.clear();
  const bool stale = fs::exists(dir, ec);
  if (ec) {
    *err = "cannot inspect " + u8(dir) + ": " + ec.message();
    return "";
  }
  if (stale) {
    if (isLinkPath(dir)) {
      *err = u8(dir) + " is a link: not replaced";
      return "";
    }
    fs::remove_all(dir, ec);
    if (ec) {
      *err = "cannot replace the incomplete generation " + u8(dir) + ": " + ec.message();
      return "";
    }
  }
  fs::rename(tmp, dir, ec);
  if (ec) {
    *err = "cannot finish " + u8(dir) + ": " + ec.message();
    return "";
  }
  return u8(dir);
}

// ---------------------------------------------------------------- manifests
bool jsonStr(const JsonValue& o, const char* k, std::string* out, std::string* err, size_t maxLen = 4096) {
  const JsonValue* v = o.get(k);
  if (!v) return true;
  if (!v->isString()) { *err = std::string("mod.json: ") + k + " must be a string"; return false; }
  if (v->str.size() > maxLen) {
    *err = std::string("mod.json: ") + k + " is longer than " + std::to_string(maxLen) + " bytes";
    return false;
  }
  *out = v->str;
  return true;
}
bool jsonIds(const JsonValue& o, const char* k, std::vector<std::string>* out, std::string* err) {
  const JsonValue* v = o.get(k);
  if (!v) return true;
  if (!v->isArray()) { *err = std::string("mod.json: ") + k + " must be a list of mod ids"; return false; }
  for (const auto& x : v->arr) {
    if (!x.isString() || !modValidId(x.str)) { *err = std::string("mod.json: ") + k + " must be a list of mod ids"; return false; }
    out->push_back(x.str);
  }
  return true;
}
// "patches" (mods) / "assets" (port features): [{asset, base "<file>:<entry>", patch}]
bool jsonGen(const JsonValue& o, const char* k, const fs::path& root, std::vector<ModGenAsset>* out, std::string* err) {
  const JsonValue* v = o.get(k);
  if (!v) return true;
  if (!v->isArray()) { *err = std::string(k) + " must be a list"; return false; }
  for (const auto& x : v->arr) {
    std::string asset, base, patch;
    if (!x.isObject() || !jsonStr(x, "asset", &asset, err) || !jsonStr(x, "base", &base, err) ||
        !jsonStr(x, "patch", &patch, err))
      return false;
    const std::string a = safeRel(asset), p = safeRel(patch);
    const size_t colon = base.find(':');
    const std::string bf = colon == std::string::npos ? "" : safeRel(base.substr(0, colon));
    const std::string be = colon == std::string::npos ? "" : base.substr(colon + 1);
    std::error_code ec;
    if (a.empty() || p.empty() || bf.empty() || be.empty() || be.find('/') != std::string::npos ||
        !fs::is_regular_file(root / fs::u8path(p), ec)) {
      *err = std::string(k) + ": entry '" + asset + "' needs asset, base \"<file>:<top-level entry>\" and an existing patch";
      return false;
    }
    out->push_back({a, bf, be, u8(root / fs::u8path(p))});
  }
  return true;
}

void parseManifest(ModInfo& m) {
  const fs::path mj = fs::u8path(m.root) / "mod.json";
  std::error_code ec;
  if (!fs::is_regular_file(mj, ec)) return;
  m.legacy = false;
  std::string text, err;
  JsonValue j;
  if (!readFile(mj, &text) || !jsonParse(text, &j, &err)) {
    fail(m, ModStatus::BadManifest, "mod.json: " + (err.empty() ? std::string("unreadable") : err));
    return;
  }
  if (!j.isObject()) { fail(m, ModStatus::BadManifest, "mod.json is not an object"); return; }
  const JsonValue* fmt = j.get("format");  // optional (an older flat mod.json has none)
  if (fmt && (!fmt->isInteger() || fmt->i64 != 1)) { fail(m, ModStatus::BadManifest, "mod.json: format must be 1"); return; }
  std::string id = m.id;
  if (!jsonStr(j, "id", &id, &err) || !jsonStr(j, "name", &m.name, &err, 64) ||
      !jsonStr(j, "author", &m.author, &err, 64) || !jsonStr(j, "version", &m.version, &err, 32) ||
      !jsonStr(j, "description", &m.description, &err, 1024) || !jsonStr(j, "game", &m.game, &err, 64) ||
      !jsonStr(j, "category", &m.category, &err, 32) ||
      !jsonIds(j, "requires", &m.deps, &err) || !jsonIds(j, "conflicts", &m.conflicts, &err) ||
      !jsonGen(j, "patches", fs::u8path(m.root), &m.patches, &err)) {
    fail(m, ModStatus::BadManifest, err);
    return;
  }
  m.id = id;
  if (const JsonValue* costumes = j.get("costumes")) {
    if (!costumes->isArray() || costumes->arr.size() > 256) {
      fail(m, ModStatus::BadManifest, "mod.json: costumes must be an array of at most 256 additions");
      return;
    }
    std::set<std::string> ids, entities;
    const std::string prefix = modLower(modAssetPrefix(m.id));
    for (const auto& item : costumes->arr) {
      ModCostumeInfo c;
      if (!item.isObject() || !jsonStr(item, "id", &c.localId, &err, 64) ||
          !jsonStr(item, "name", &c.name, &err, 128) || !jsonStr(item, "entity", &c.entity, &err, 128) ||
          !modValidId(c.localId) || c.name.empty() || !modValidId(c.entity) ||
          modLower(c.entity).rfind(prefix, 0) != 0 ||
          !ids.insert(modLower(c.localId)).second || !entities.insert(modLower(c.entity)).second) {
        fail(m, ModStatus::BadManifest, "mod.json: costumes need unique ids/names and entity stems in " +
                                         modAssetPrefix(m.id) + " (no retail replacements)");
        return;
      }
      c.id = modLower(m.id) + "/" + modLower(c.localId);
      c.source = m.id;
      if (!jsonStr(item, "description", &c.description, &err, 512) ||
          !jsonStr(item, "thumbnail", &c.thumbnail, &err, 27) ||
          !jsonStr(item, "fullsize", &c.fullsize, &err, 27) ||
          !jsonStr(item, "character", &c.characterName, &err, 48)) {   // iter-465 viewer: optional
        fail(m, ModStatus::BadManifest, err);
        return;
      }
      // The retail Apt movie-name buffer holds 31 chars INCLUDING '.swf'.
      // Every preview belongs to this pack's namespace, never a filesystem URL.
      for (const auto* preview : {&c.thumbnail, &c.fullsize}) {
        if (!preview->empty() && (!modValidId(*preview) || preview->find('.') != std::string::npos ||
                                modLower(*preview).rfind(prefix, 0) != 0)) {
          fail(m, ModStatus::BadManifest, "mod.json: costume previews need scoped Apt movie stems (at most 27 characters)");
          return;
        }
      }
      // Fortress COSTUMES previews: DXT5 .dds files inside the pack (never under DATA, so the
      // engine's loose storage never sees them); wrapped into generated movies at boot.
      for (const auto& field : {std::make_pair("thumbnail_image", &c.thumbImage),
                                std::make_pair("fullsize_image", &c.fullImage)}) {
        std::string rel;
        if (!jsonStr(item, field.first, &rel, &err, 260)) { fail(m, ModStatus::BadManifest, err); return; }
        if (rel.empty()) continue;
        const std::string p = safeRel(rel), low = modLower(p);
        if (p.empty() || low.size() < 5 || low.compare(low.size() - 4, 4, ".dds") != 0 ||
            low.rfind("data/", 0) == 0 || !fs::is_regular_file(fs::u8path(m.root) / fs::u8path(p), ec)) {
          fail(m, ModStatus::BadManifest, std::string("mod.json: costume ") + field.first + " '" + rel +
                                           "' must be a .dds file in the mod, outside DATA");
          return;
        }
        *field.second = u8(fs::u8path(m.root) / fs::u8path(p));
      }
      if (!jsonStr(item, "animation_project", &c.animationProject, &err, 63)) {
        fail(m, ModStatus::BadManifest, err);
        return;
      }
      if (!c.animationProject.empty() && (!modValidId(c.animationProject) ||
          c.animationProject.find('.') != std::string::npos ||
          modLower(c.animationProject).rfind(prefix, 0) != 0)) {
        fail(m, ModStatus::BadManifest, "mod.json: animation_project needs an own-pack scoped resource stem (at most 63 characters)");
        return;
      }
      // Own animation banks (docs/MODS.md "Fortress costumes"): SupermanAnims bank name -> a bank
      // file in the pack, outside DATA. The family id is the content hash of the whole set, so
      // suits (in any pack) that ship the same banks switch among themselves at once.
      if (const JsonValue* banks = c.animationProject.empty() ? item.get("animation_banks") : nullptr) {
        auto refuse = [&](const std::string& why) {
          fail(m, ModStatus::BadManifest, "mod.json: costume " + c.localId + " animation_banks: " + why);
        };
        if (!banks->isObject() || banks->obj.empty() || banks->obj.size() > 6) {
          refuse("an object naming 1..6 SupermanAnims banks (SUPERMAN_FLIGHT, ...) and their files");
          return;
        }
        std::vector<std::pair<std::string, std::string>> digests;
        for (const auto& [name, file] : banks->obj) {
          std::string up = name;
          for (auto& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
          bool known = false;
          for (const char* retail : kSupermanBanks) known = known || up == retail;
          for (const auto& seen : digests) known = known && seen.first != up;
          const std::string p = file.isString() ? safeRel(file.str) : std::string(), low = modLower(p);
          std::vector<uint8_t> bytes;
          std::string why;
          if (!known || p.empty() || low.rfind("data/", 0) == 0 ||
              !fs::is_regular_file(fs::u8path(m.root) / fs::u8path(p), ec) ||
              !readBytes(fs::u8path(m.root) / fs::u8path(p), &bytes) || !modValidSupermanBank(bytes, &why)) {
            refuse("'" + name + "' must be a SupermanAnims bank name mapped to a bank file in the mod, outside DATA" +
                   (why.empty() ? std::string() : " (" + why + ")"));
            return;
          }
          c.banks.emplace_back(up, u8(fs::u8path(m.root) / fs::u8path(p)));
          digests.emplace_back(up, sha256Hex(bytes.data(), bytes.size()));
        }
        std::sort(digests.begin(), digests.end());
        std::string identity;
        for (const auto& [name, digest] : digests) identity += name + "\n" + digest + "\n";
        c.bankFamily = "banks_" + sha256Hex(identity.data(), identity.size()).substr(0, 16);
      } else if (item.get("animation_banks")) {
        fail(m, ModStatus::BadManifest, "mod.json: a costume has animation_banks or animation_project, not both");
        return;
      }
      m.costumes.push_back(std::move(c));
    }
  }
  // iter-465 charswitch: a pack marked "test_only" (generated test characters) never reaches a
  // player's COSTUMES screen; its suits are listed only with SR_TEST_CHARACTERS=1 (test rolls).
  if (const JsonValue* t = j.get("test_only"); t && t->type == JsonValue::Type::Bool && t->b) {
    const char* gate = std::getenv("SR_TEST_CHARACTERS");
    if (!gate || gate[0] != '1') m.costumes.clear();
  }
  if (!j.get("category") || m.category.empty()) m.category = m.costumes.empty() ? "other" : "characters";
  m.category = modLower(m.category);
  if (m.category != "other" && modCategoryLabel(m.category) == "Other") {
    fail(m, ModStatus::BadManifest, "mod.json: unknown category '" + m.category + "'");
    return;
  }
  if (const JsonValue* pr = j.get("priority")) {
    if (!pr->isInteger()) { fail(m, ModStatus::BadManifest, "mod.json: priority must be an integer"); return; }
    m.priority = static_cast<int>(pr->i64);
  }
  if (const JsonValue* en = j.get("enabled")) {
    if (en->type != JsonValue::Type::Bool) { fail(m, ModStatus::BadManifest, "mod.json: enabled must be true/false"); return; }
    m.enabled = en->b;  // the default for a newly found mod; the player's state wins
  }
  std::string prev;
  if (!jsonStr(j, "preview", &prev, &err)) { fail(m, ModStatus::BadManifest, err); return; }
  if (!prev.empty()) {
    const std::string p = safeRel(prev);
    if (p.empty() || !fs::is_regular_file(fs::u8path(m.root) / fs::u8path(p), ec)) {
      fail(m, ModStatus::BadManifest, "mod.json: preview '" + prev + "' is not a file in the mod");
      return;
    }
    m.preview = u8(fs::u8path(m.root) / fs::u8path(p));
  }
  // iter-465 modlib: screenshots (PNG/JPG files inside the mod) and a Nexus Mods page.
  if (const JsonValue* shots = j.get("screenshots")) {
    if (!shots->isArray() || shots->arr.size() > 12) {
      fail(m, ModStatus::BadManifest, "mod.json: screenshots must be a list of at most 12 image files");
      return;
    }
    for (const auto& s : shots->arr) {
      const std::string rel = s.isString() ? safeRel(s.str) : std::string();
      const size_t dot = rel.rfind('.');
      const std::string ext = dot == std::string::npos ? std::string() : modLower(rel.substr(dot));
      if (rel.empty() || !(ext == ".png" || ext == ".jpg" || ext == ".jpeg") ||
          !fs::is_regular_file(fs::u8path(m.root) / fs::u8path(rel), ec)) {
        fail(m, ModStatus::BadManifest, "mod.json: screenshot '" + (s.isString() ? s.str : std::string("?")) +
                                         "' is not a PNG/JPG file in the mod");
        return;
      }
      m.screenshots.push_back(u8(fs::u8path(m.root) / fs::u8path(rel)));
    }
  }
  std::string nexus;
  if (!jsonStr(j, "nexus", &nexus, &err, 256)) { fail(m, ModStatus::BadManifest, err); return; }
  if (!nexus.empty()) {
    // Only a Nexus Mods page over https, and nothing a shell could read as anything else.
    const std::string lo = modLower(nexus);
    bool ok = lo.rfind("https://www.nexusmods.com/", 0) == 0 || lo.rfind("https://nexusmods.com/", 0) == 0;
    for (char c : nexus)
      ok = ok && static_cast<unsigned char>(c) > 0x20 && c != '"' && c != '<' && c != '>' && c != '\\' && c != '^';
    if (!ok) {
      fail(m, ModStatus::BadManifest, "mod.json: nexus must be an https://www.nexusmods.com/ page");
      return;
    }
    m.nexusUrl = nexus;
  }
}

// iter-466 map lane: a mod's pause-map data (docs/MAP.md 4.2): mod.json "map" (an object in the
// map.json schema) or a map.json beside it, not both. Validated like the rest of mod.json: a bad
// entry refuses the mod with the reason the MODS library shows.
void parseMapData(ModInfo& m) {
  const fs::path root = fs::u8path(m.root);
  std::error_code ec;
  const JsonValue* inManifest = nullptr;
  JsonValue j;
  std::string text, err;
  if (fs::is_regular_file(root / "mod.json", ec) && readFile(root / "mod.json", &text) && jsonParse(text, &j, &err) &&
      j.isObject())
    inManifest = j.get("map");
  const bool hasFile = fs::is_regular_file(root / "map.json", ec);
  if (!inManifest && !hasFile) return;
  if (inManifest && hasFile) {
    fail(m, ModStatus::BadManifest, "mod.json: give the map in mod.json \"map\" or in map.json, not both");
    return;
  }
  auto def = std::make_shared<mapmk::Definition>();
  bool ok;
  if (inManifest) {
    ok = mapmk::parseDefinition(*inManifest, m.id, def.get(), &err);
  } else {
    std::string mt;
    ok = readFile(root / "map.json", &mt) && mapmk::parseDefinitionText(mt, m.id, def.get(), &err);
    if (!ok && err.empty()) err = "map.json unreadable";
    if (!ok) err = "map.json: " + err;
  }
  if (!ok) {
    fail(m, ModStatus::BadManifest, inManifest ? "mod.json: " + err : err);
    return;
  }
  m.map = std::move(def);
}

ModInfo loadMod(const fs::path& p, bool isZip, const std::string& cacheDir) {
  ModInfo m;
  m.folder = modPathU8(p.filename());
  m.isZip = isZip;
  m.hostPath = u8(p);
  m.id = isZip ? modPathU8(p.stem()) : m.folder;
  if (isZip) {
    std::error_code ec;
    const auto sz = fs::file_size(p, ec);
    const auto mt = fs::last_write_time(p, ec).time_since_epoch().count();
    const fs::path dst = fs::u8path(cacheDir) / "zip" / fs::u8path(m.id + "-" + std::to_string(sz) + "-" + std::to_string(mt));
    if (!fs::exists(dst / ".complete", ec)) {
      fs::path tmp = dst;
      tmp += ".tmp";
      fs::remove_all(tmp, ec);
      std::string err;
      if (!zipExtractMod(m.hostPath, u8(tmp), &err)) {
        fs::remove_all(tmp, ec);
        m.root = "";
        fail(m, ModStatus::BadZip, err);
        return m;
      }
      { std::ofstream(tmp / ".complete") << "ok\n"; }
      fs::remove_all(dst, ec);
      fs::rename(tmp, dst, ec);
    }
    m.root = u8(dst);
  } else {
    m.root = m.hostPath;
  }
  parseManifest(m);
  if (m.status == ModStatus::Ok) parseMapData(m);
  if (m.name.empty()) m.name = m.id;
  if (m.status == ModStatus::Ok && !modValidId(m.id))
    fail(m, ModStatus::BadManifest, "id '" + m.id + "' is not a plain name (letters, digits, _ - .)");
  return m;
}

std::vector<std::string> parseEnabledTxt(const std::string& text) {
  std::vector<std::string> list;
  size_t pos = 0;
  bool first = true;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    std::string line = text.substr(pos, nl - pos);
    pos = nl + 1;
    if (first && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line = line.substr(3);
    first = false;
    const size_t h = line.find('#');
    if (h != std::string::npos) line = line.substr(0, h);
    line = trim(line);
    bool dup = false;
    for (const auto& x : list) dup = dup || modLower(x) == modLower(line);
    if (!line.empty() && !dup) list.push_back(line);
  }
  return list;
}

std::string stateJson(const std::vector<std::string>& order, const std::set<std::string>& disabled,
    const std::vector<ModInfo>& mods, const std::string& costume = "",
    const std::set<std::string>& disabledCostumes = {}) {
  std::string o = "{\n  \"format\": 1,\n  \"order\": [";
  for (size_t i = 0; i < order.size(); ++i) o += (i ? ", " : "") + jsonQuote(order[i]);
  o += "],\n  \"disabled\": [";
  bool firstD = true;
  for (const auto& id : order) {  // keep the ids' own spelling
    if (!disabled.count(modLower(id))) continue;
    o += (firstD ? "" : ", ") + jsonQuote(id);
    firstD = false;
  }
  (void)mods;
  o += "],\n  \"costume\": " + jsonQuote(costume) + ",\n  \"disabled_costumes\": [";
  bool firstC = true;
  for (const auto& id : disabledCostumes) {
    o += (firstC ? "" : ", ") + jsonQuote(id);
    firstC = false;
  }
  o += "]\n}\n";
  return o;
}

bool saveStateLocked(Mgr& g) {
  if (!g.fromState || !g.env.allowStateWrite) return false;
  return writeFileAtomic(fs::u8path(g.modsDir) / "mods.json",
                        stateJson(g.order, g.disabled, g.mods, g.selectedCostume, g.disabledCostumes));
}

// ---------------------------------------------------------------- the boot
void logBoot(sr::LogSeverity sev, const std::string& s) { SR_LOG_BOOT(sev, "%s", s.c_str()); }

void loadPortFeatures(Mgr& g, std::set<std::string>* reserved) {
  std::vector<fs::path> cands;
  if (!g.env.portDir.empty()) cands.push_back(fs::u8path(g.env.portDir));
  if (!g.env.exeDir.empty()) cands.push_back(fs::u8path(g.env.exeDir) / "port_features");
  cands.push_back(fs::u8path(g.env.contentRoot) / "data" / "port_features");
  std::error_code ec;
  fs::path dir;
  for (const auto& c : cands)
    if (fs::is_directory(c, ec)) { dir = c; break; }
  if (dir.empty()) return;
  std::vector<fs::path> subs;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_directory(ec)) subs.push_back(e.path());
  std::sort(subs.begin(), subs.end(), [](const fs::path& a, const fs::path& b) {
    return modLower(modPathU8(a.filename())) < modLower(modPathU8(b.filename()));
  });
  for (const auto& sd : subs) {
    const fs::path fj = sd / "feature.json";
    if (!fs::is_regular_file(fj, ec)) continue;
    PortFeatureInfo f;
    f.id = modPathU8(sd.filename());
    f.dir = u8(sd);
    std::string text, err;
    JsonValue j;
    if (!readFile(fj, &text) || !jsonParse(text, &j, &err) || !j.isObject()) {
      f.status = ModStatus::BadManifest;
      f.reason = "feature.json: " + err;
      g.feats.push_back(f);
      reserved->insert(modLower(f.id));
      continue;
    }
    std::string id = f.id;
    jsonStr(j, "id", &id, &err);
    // Lane E E2 (Astra r1): the id names the feature's cache folder (generateAssets) - letters, digits, '_', '-', '.'
    if (!modValidId(id)) {
      f.status = ModStatus::BadManifest;
      f.reason = "feature.json: id '" + id + "' is not a valid id (letters, digits, _ - .)";
      g.feats.push_back(f);
      reserved->insert(modLower(f.id));
      continue;
    }
    f.id = id;
    jsonStr(j, "name", &f.name, &err);
    jsonStr(j, "description", &f.description, &err);
    if (const JsonValue* op = j.get("option"); op && op->isString()) f.option = op->str;
    if (f.name.empty()) f.name = f.id;
    reserved->insert(modLower(f.id));
    if (!jsonGen(j, "assets", sd, &f.assets, &err)) {
      f.status = ModStatus::BadManifest;
      f.reason = "feature.json: " + err;
      g.feats.push_back(f);
      continue;
    }
    f.status = ModStatus::Ok;
    if (const JsonValue* b = j.get("base"); b && b->isObject()) {
      for (const auto& kv : b->obj) {
        const JsonValue* sz = kv.second.get("size");
        const JsonValue* th = kv.second.get("toc_sha256");
        uint64_t have = 0;
        std::string hex;
        const fs::path bp = fs::u8path(g.discRoot) / fs::u8path(safeRel(kv.first));
        if (!sz || !th || !sz->isInteger() || !th->isString()) {
          f.status = ModStatus::BadManifest;
          f.reason = "feature.json: base pin of " + kv.first + " is incomplete";
        } else if (!tocSha256(bp, &have, &hex)) {
          f.status = ModStatus::WrongGame;
          f.reason = kv.first + " is missing or unreadable";
        } else if (have != static_cast<uint64_t>(sz->i64) || hex != th->str) {
          f.status = ModStatus::WrongGame;
          f.reason = kv.first + " is not the USA retail file this feature was made for";
        }
        if (f.status != ModStatus::Ok) break;
      }
    }
    g.feats.push_back(f);
  }
}

// iter-465 character switch: the bytes the preloaded own-project characters add to the game's
// animation memory (their project's banks + CCT, found as loose files in the mounted layers the
// way the game finds them). kernel/anim_heap.cpp grows HeapAnimation by this much at boot.
// iter-465 charswitch: objectives that add Superman banks to "SupermanAnims" through an association
// document in their own preloads (census: the owner's mod-system research, not published). A character on its own
// project gets the same moves when its pack ships its own copy of the bank (<P><C><suffix>.bnk):
// a sibling association <P><C><tag>ANIMS = <project name="<P><C>ANIMS" rcbfile="<P><C>CCT"><bank .../>
// is added to that objective's preloads next to the retail one.
struct MissionBank { const char* flow; const char* bank; const char* tag; };
const MissionBank kMissionBanks[] = {
    {"BIZOBJ15", "MIM_BIZ", "BIZ01"}, {"ME34TYPE01", "MIM_BIZ", "BIZ01"}, {"ME36TYPE01", "MIM_BIZ", "BIZ01"},
    {"PLAYABLEBIZ", "MIM_BIZ", "BIZ01"}, {"MTOOBJ09", "MIM_MTO", "MTO09"}, {"TWSOBJ04", "TWISTER_IGC", "TWS04"},
    {"WAROBJ01", "WW_IGC", "WAR01"},
};

std::map<std::string, fs::path> looseFilesLocked(const Mgr& g) {
  std::map<std::string, fs::path> loose;  // lower-case file name -> first (winning) layer file
  for (const auto& layer : g.layers) for (const auto& r : layer.roots) {
    std::error_code ec;
    const fs::path dir = fs::u8path(r) / "DATA/_workinprogress";
    if (!fs::is_directory(dir, ec)) continue;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
      if (it->is_regular_file(ec)) loose.emplace(modLower(modPathU8(it->path().filename())), it->path());
  }
  return loose;
}

// -> generated files: each affected objective's <FLOW>PRELOADS.xml (retail + siblings) and the
// sibling association documents. A failure leaves that objective retail (logged, never fatal).
void composeMissionBanksLocked(const Mgr& g, const std::vector<ModCostumeInfo>& costumes,
                               std::map<std::string, std::vector<uint8_t>>* out) {
  const auto loose = looseFilesLocked(g);
  std::map<std::string, ModCostumeInfo> perFlow;  // flow -> a carrier of its extra ANIM preloads
  std::set<std::string> projects;
  for (const auto& c : costumes) {
    const std::string& project = c.animationProject;
    if (project.size() < 6 || modLower(project.substr(project.size() - 5)) != "anims" ||
        !projects.insert(modLower(project)).second) continue;
    const std::string stem = project.substr(0, project.size() - 5);  // <P><C>
    for (const auto& mb : kMissionBanks) {
      const std::string bank = stem + mb.bank;
      if (!loose.count(modLower(bank + ".bnk"))) continue;
      const std::string assoc = stem + mb.tag + "ANIMS";
      const std::string doc = "<project name=\"" + project + "\" rcbfile=\"" + stem + "CCT\">\n  <bank name=\"" +
                              bank + "\"/>\n</project>\n";
      std::vector<uint8_t> bytes(doc.begin(), doc.end());
      bytes.push_back(0);
      (*out)[assoc + ".xml"] = std::move(bytes);
      perFlow[mb.flow].preloads.emplace_back("ANIM", assoc);
    }
  }
  for (auto& [flow, carrier] : perFlow) {
    std::vector<uint8_t> bundle, other, preloads, composed;
    std::string why;
    if (!modReadTopEntry(g.discRoot + "/DATA/objtiv.AST", flow, &bundle, &why) ||
        !modReadNestedEntry(bundle, flow + "_OTHER", &other, &why) ||
        !modReadNestedEntry(other, flow + "PRELOADS", &preloads, &why) ||
        !modComposeCostumePreloads(preloads, {carrier}, &composed, &why)) {
      logBoot(sr::LogSeverity::Warn, "[COSTUME] " + flow + " character banks not added: " + why);
      continue;
    }
    (*out)[flow + "PRELOADS.xml"] = std::move(composed);
    logBoot(sr::LogSeverity::Info, "[COSTUME] " + flow + ": " + std::to_string(carrier.preloads.size()) +
                                    " character association(s) added to its preloads");
  }
}

// The generated files of the converted own-bank suits: per family SRM_FAM_<f>_ANIMS.xml + _CCT.rcb +
// six scoped banks (the suit's own, else the retail one); per suit <entity>_SRCHAR.xml = its entity
// with project SupermanAnims -> the family project. -> bank bytes (animation memory). A suit whose
// files cannot be made stays a plain suit of its family (logged).
uint64_t composeBankCharactersLocked(const Mgr& g, std::vector<ModCostumeInfo>& costumes,
                                     std::map<std::string, std::vector<uint8_t>>* out) {
  std::vector<uint8_t> anim;
  std::string why;
  std::map<std::string, std::vector<uint8_t>> retail;  // SUPERMAN_* and the CCT
  bool haveRetail = false;
  {
    std::vector<uint8_t> bw;
    if (modReadTopEntry(g.discRoot + "/DATA/baseworl.AST", "BASEWORLDFLOW_ANIM", &anim, &why)) {
      haveRetail = true;
      for (const char* n : kSupermanBanks) {
        std::vector<uint8_t> b;
        haveRetail = haveRetail && modReadNestedEntry(anim, n, &b, &why);
        retail[n] = std::move(b);
      }
      std::vector<uint8_t> cct;
      haveRetail = haveRetail && modReadNestedEntry(anim, "SUPERMANCCTPROJECT", &cct, &why);
      retail["CCT"] = std::move(cct);
    }
  }
  const auto loose = looseFilesLocked(g);
  uint64_t bytes = 0;
  std::set<std::string> doneFamilies;
  std::set<uint32_t> used;
  for (auto& c : costumes) {
    if (!c.character || c.banks.empty()) continue;
    std::string fam;
    for (const auto& [type, name] : c.preloads)
      if (type == "ANIM" && name.rfind("SRM_FAM_", 0) == 0) fam = name.substr(0, name.size() - 5);  // strip ANIMS
    auto revert = [&](const std::string& reason) {
      logBoot(sr::LogSeverity::Warn, "[COSTUME] " + c.id + " stays a suit of its family: " + reason);
      c.character = false;
      c.entity = c.entity.substr(0, c.entity.size() - 7);
      c.preloads.clear();
    };
    if (fam.empty() || !haveRetail) { revert(haveRetail ? "no family project" : why); continue; }
    // the suit's entity, served under its character name with the family project
    auto ent = loose.find(modLower(c.entity.substr(0, c.entity.size() - 7) + ".xml"));
    std::vector<uint8_t> entBytes;
    if (ent == loose.end() || !readBytes(ent->second, &entBytes)) { revert("its entity file is not a loose .xml"); continue; }
    std::string text(entBytes.begin(), entBytes.end());
    while (!text.empty() && text.back() == '\0') text.pop_back();
    const std::string from = "project=\"SupermanAnims\"";
    const size_t at = text.find(from);
    if (at == std::string::npos || text.find(from, at + 1) != std::string::npos) { revert("its entity does not name SupermanAnims once"); continue; }
    text.replace(at, from.size(), "project=\"" + fam + "ANIMS\"");
    std::vector<uint8_t> entOut(text.begin(), text.end());
    entOut.push_back(0);
    if (doneFamilies.insert(fam).second) {
      std::map<std::string, std::vector<uint8_t>> files;
      std::string xml = "<project name=\"" + fam + "ANIMS\" rcbfile=\"" + fam + "CCT\">\n";
      bool ok = true;
      for (const char* n : kSupermanBanks) {
        std::vector<uint8_t> src = retail[n], scoped;
        for (const auto& [name, path] : c.banks)
          if (modLower(name) == modLower(n)) ok = ok && readBytes(fs::u8path(path), &src);
        const std::string bankName = fam + std::string(n + 9);  // SUPERMAN_X -> <fam>X
        ok = ok && modScopedBankCopy(src, bankName, fam, &used, &scoped, &why);
        if (!ok) break;
        bytes += scoped.size();
        files[bankName + ".bnk"] = std::move(scoped);
        xml += "  <bank name=\"" + bankName + "\"/>\n";
      }
      if (!ok) { doneFamilies.erase(fam); revert("family banks: " + why); continue; }
      xml += "</project>\n";
      files[fam + "ANIMS.xml"] = std::vector<uint8_t>(xml.begin(), xml.end());
      files[fam + "ANIMS.xml"].push_back(0);
      files[fam + "CCT.rcb"] = retail["CCT"];
      bytes += retail["CCT"].size();
      for (auto& [n, d] : files) (*out)[n] = std::move(d);
      logBoot(sr::LogSeverity::Info, "[COSTUME] bank family " + fam + "ANIMS generated (" + c.id + ")");
    }
    (*out)[c.entity + ".xml"] = std::move(entOut);
  }
  return bytes;
}

uint64_t characterAnimBytesLocked(const Mgr& g, const std::vector<ModCostumeInfo>& costumes) {
  std::set<std::string> projects;
  for (const auto& c : costumes) if (!c.animationProject.empty()) projects.insert(modLower(c.animationProject));
  if (projects.empty()) return 0;
  std::map<std::string, fs::path> loose;  // lower-case file name -> first (winning) layer file
  for (const auto& layer : g.layers) for (const auto& r : layer.roots) {
    std::error_code ec;
    const fs::path dir = fs::u8path(r) / "DATA/_workinprogress";
    if (!fs::is_directory(dir, ec)) continue;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
      if (it->is_regular_file(ec)) loose.emplace(modLower(modPathU8(it->path().filename())), it->path());
  }
  uint64_t total = 0;
  auto add = [&](const std::string& file) {
    auto it = loose.find(modLower(file));
    std::error_code ec;
    if (it != loose.end()) total += fs::file_size(it->second, ec);
  };
  for (const auto& project : projects) {
    auto it = loose.find(project + ".xml");
    std::vector<uint8_t> xml;
    if (it == loose.end() || !readBytes(it->second, &xml)) continue;
    const std::string text(xml.begin(), xml.end());
    for (const char* attr : {"rcbfile=\"", "bank name=\""}) {
      for (size_t at = text.find(attr); at != std::string::npos; at = text.find(attr, at + 1)) {
        const size_t b = at + std::strlen(attr), e = text.find('"', b);
        if (e == std::string::npos) break;
        add(text.substr(b, e - b) + (attr[0] == 'r' ? ".rcb" : ".bnk"));
      }
    }
  }
  return total;
}

void bootLocked(Mgr& g, const ModBootEnv& env) {
  g.env = env;
  g.booted = true;
  std::string root = env.contentRoot;
  if (root.empty()) return;
  g.discRoot = u8(fs::u8path(root) / kDiscDir);
  g.modsDir = u8(fs::u8path(root) / "mods");
  g.cacheDir = u8(fs::u8path(g.modsDir) / ".cache");

  // the decision (docs/MODS.md sections 7 and 10)
  enum class Src { None, State, List } src;
  if (!env.hasSrMods) src = env.automated ? Src::None : Src::State;
  else if (env.srMods.empty() || env.srMods == "0") src = Src::None;
  else if (env.srMods == "@player" || env.srMods == "@enabled.txt") src = Src::State;
  else src = Src::List;
  const std::vector<std::string> explicitList = src == Src::List ? splitList(env.srMods) : std::vector<std::string>{};
  bool portDefault;
  if (env.hasPort && (env.srPortFeatures == "0" || env.srPortFeatures == "1")) portDefault = env.srPortFeatures == "1";
  else portDefault = !env.automated && !(env.hasSrMods && env.srMods == "0");
  const bool portForcedOff = env.hasPort && env.srPortFeatures == "0";

  std::set<std::string> reserved;
  loadPortFeatures(g, &reserved);
  reserved.insert("retail");  // "retail/<n>" names the retail Fortress suits (modCostumeRetailSlot)
  for (auto& f : g.feats) {
    bool on = portDefault;
    for (const auto& x : explicitList) on = on || (!portForcedOff && modLower(x) == modLower(f.id));
    if (env.hasCustomFrontend && modLower(f.id) == "custom_frontend") {
      if (env.srCustomFrontend == "1" && !portForcedOff) on = true;
      if (env.srCustomFrontend == "0") on = false;
    }
    f.on = on;
    if (f.status == ModStatus::Ok && !on) { f.status = ModStatus::Off; f.reason = "port features off in this run"; }
    if (f.status == ModStatus::Ok && !f.option.empty()) {
      auto it = env.options.find(f.option);
      const int v = it == env.options.end() ? 1 : it->second;
      if (v != 1) { f.status = ModStatus::Disabled; f.reason = "option " + f.option + " is off"; }
    }
  }

  // discovery
  std::vector<ModInfo> found;
  std::error_code ec;
  if (src != Src::None && fs::is_directory(fs::u8path(g.modsDir), ec)) {
    std::vector<fs::directory_entry> ents;
    for (const auto& e : fs::directory_iterator(fs::u8path(g.modsDir), ec)) ents.push_back(e);
    std::sort(ents.begin(), ents.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
      return modLower(modPathU8(a.path().filename())) < modLower(modPathU8(b.path().filename()));
    });
    for (const auto& e : ents) {
      const std::string n = modPathU8(e.path().filename());
      if (n.empty() || n[0] == '.' || n[0] == '@') continue;
      std::error_code e2;
      if (e.is_directory(e2)) found.push_back(loadMod(e.path(), false, g.cacheDir));
      else if (modLower(modPathU8(e.path().extension())) == ".zip" && e.is_regular_file(e2))
        found.push_back(loadMod(e.path(), true, g.cacheDir));
    }
  }
  std::unordered_map<std::string, size_t> byId;
  for (size_t i = 0; i < found.size(); ++i) {
    const std::string k = modLower(found[i].id);
    if (byId.count(k)) fail(found[i], ModStatus::Duplicate, "another mod has the id " + found[i].id);
    else byId.emplace(k, i);
  }

  // the order and the player's switches
  std::vector<std::string> order;
  std::set<std::string> disabled;
  if (src == Src::State) {
    g.fromState = true;
    g.source = "mods/mods.json";
    const fs::path sj = fs::u8path(g.modsDir) / "mods.json", et = fs::u8path(g.modsDir) / "enabled.txt";
    std::string text, err;
    JsonValue j;
    if (fs::is_regular_file(sj, ec)) {
      if (readFile(sj, &text) && jsonParse(text, &j, &err) && j.isObject()) {
        if (const JsonValue* c = j.get("costume"); c && c->isString()) g.selectedCostume = modLower(c->str);
        if (const JsonValue* c = j.get("disabled_costumes"); c && c->isArray())
          for (const auto& x : c->arr)
            if (x.isString() && validCostumeId(x.str)) g.disabledCostumes.insert(modLower(x.str));
        if (const JsonValue* o = j.get("order"); o && o->isArray())
          for (const auto& x : o->arr) if (x.isString()) order.push_back(x.str);
        if (const JsonValue* d = j.get("disabled"); d && d->isArray())
          for (const auto& x : d->arr) if (x.isString()) disabled.insert(modLower(x.str));
      } else {
        logBoot(sr::LogSeverity::Error, "[MODS] error: mods/mods.json unreadable (" + err + "): default order, every mod on");
        g.source = "mods/mods.json (unreadable)";
      }
    } else {
      // the default order: mod.json priority (higher first), then the id
      std::vector<const ModInfo*> fresh;
      for (const auto& m : found) if (m.status != ModStatus::Duplicate) fresh.push_back(&m);
      std::stable_sort(fresh.begin(), fresh.end(), [](const ModInfo* a, const ModInfo* b) { return a->priority > b->priority; });
      if (fs::is_regular_file(et, ec) && readFile(et, &text)) {
        // migration (docs/MODS.md section 4): enabled.txt's ids on top and on, every other mod off
        order = parseEnabledTxt(text);
        std::set<std::string> listed;
        for (const auto& x : order) listed.insert(modLower(x));
        for (const auto* m : fresh)
          if (!listed.count(modLower(m->id))) { order.push_back(m->id); disabled.insert(modLower(m->id)); }
        g.source = "mods/enabled.txt (migrated)";
        if (env.allowStateWrite && !env.automated && !env.hasSrMods) {
          if (writeFileAtomic(sj, stateJson(order, disabled, found))) {
            fs::path migrated = et;
            migrated += ".migrated";
            fs::rename(et, migrated, ec);
            logBoot(sr::LogSeverity::Info, "[MODS] migrated mods/enabled.txt -> mods/mods.json (enabled.txt renamed enabled.txt.migrated)");
          }
        }
      } else {
        for (const auto* m : fresh) {
          order.push_back(m->id);
          if (!m->enabled) disabled.insert(modLower(m->id));
        }
        g.source = "default order (no mods/mods.json yet)";
      }
    }
    // a mod found but not listed: appended, on unless its mod.json says enabled=false
    std::set<std::string> listed;
    for (const auto& x : order) listed.insert(modLower(x));
    for (const auto& m : found) {
      if (m.status == ModStatus::Duplicate || listed.count(modLower(m.id))) continue;
      order.push_back(m.id);
      listed.insert(modLower(m.id));
      if (!m.enabled) disabled.insert(modLower(m.id));
    }
  } else if (src == Src::List) {
    g.source = "SR_MODS";
    for (const auto& x : explicitList)
      if (!reserved.count(modLower(x))) order.push_back(x);
  } else {
    g.source = "none";
  }
  g.order = order;
  g.disabled = disabled;
  g.bootOrder = order;
  g.bootDisabled = disabled;
  g.bootDisabledCostumes = g.disabledCostumes;

  // the mods in load order + their statuses
  std::set<std::string> used;
  for (const auto& x : order) {
    auto it = byId.find(modLower(x));
    if (it == byId.end()) {
      if (src == Src::List)
        logBoot(sr::LogSeverity::Error, "[MOD] error: mod '" + x + "' is enabled but not in mods/ - skipped");
      continue;
    }
    if (used.count(modLower(x))) continue;
    used.insert(modLower(x));
    ModInfo m = found[it->second];
    m.enabled = !disabled.count(modLower(m.id));
    g.mods.push_back(std::move(m));
  }
  for (auto& m : found)  // duplicates stay visible in the menu, at the end
    if (m.status == ModStatus::Duplicate && src == Src::State) g.mods.push_back(m);
  for (auto& m : g.mods) {
    if (reserved.count(modLower(m.id)))
      fail(m, ModStatus::Reserved, m.id + " is built in now (port feature); this copy is ignored");
    if (!m.enabled) fail(m, ModStatus::Disabled, "switched off");
  }
  std::string fp;
  for (auto& m : g.mods) {
    if (m.status != ModStatus::Ok || m.game.empty()) continue;
    if (fp.empty()) fp = modDataFingerprint(g.discRoot);
    const bool ok = (m.game == kGameTag && fp == std::string(kGameTag) + ":" + kUsaRetailFingerprint) || m.game == fp;
    if (!ok) fail(m, ModStatus::WrongGame, "made for " + m.game + "; this data is " + fp);
  }
  for (bool changed = true; changed;) {
    changed = false;
    std::set<std::string> live;
    for (const auto& m : g.mods) if (m.status == ModStatus::Ok) live.insert(modLower(m.id));
    for (auto& m : g.mods) {
      if (m.status != ModStatus::Ok) continue;
      for (const auto& r : m.deps)
        if (!live.count(modLower(r))) { fail(m, ModStatus::MissingDependency, "needs " + r); changed = true; break; }
    }
    for (size_t a = 0; a < g.mods.size() && !changed; ++a) {
      for (size_t b = a + 1; b < g.mods.size(); ++b) {
        ModInfo& hi = g.mods[a];
        ModInfo& lo = g.mods[b];
        if (hi.status != ModStatus::Ok || lo.status != ModStatus::Ok) continue;
        bool c = false;
        for (const auto& x : hi.conflicts) c = c || modLower(x) == modLower(lo.id);
        for (const auto& x : lo.conflicts) c = c || modLower(x) == modLower(hi.id);
        if (c) { fail(lo, ModStatus::DeclaredConflict, "conflicts with " + hi.id); changed = true; }
      }
    }
  }

  // generated assets (port features, then mods' patches)
  for (auto& f : g.feats) {
    if (f.status != ModStatus::Ok) continue;
    std::string err;
    const std::string dir = generateAssets("port-" + f.id, f.assets, g.discRoot, g.cacheDir, &err);
    if (dir.empty()) {
      f.status = ModStatus::BadPatch;
      f.reason = err;
      continue;
    }
    f.applied = true;
    ModLayer L;
    L.id = f.id;
    L.name = f.name;
    L.port = true;
    L.roots.push_back(dir);
    {  // iter-466 map lane: a port feature's own map.json (the built-in pause-map definition)
      const fs::path mj = fs::u8path(f.dir) / "map.json";
      std::error_code mec;
      std::string mt, merr;
      if (fs::is_regular_file(mj, mec)) {
        auto def = std::make_shared<mapmk::Definition>();
        if (readFile(mj, &mt) && mapmk::parseDefinitionText(mt, f.id, def.get(), &merr)) L.map = std::move(def);
        else logBoot(sr::LogSeverity::Error, "[MAP] port feature " + f.id + " map.json refused: " + merr);
      }
    }
    g.layers.push_back(L);  // appended below the mods afterwards
  }
  // iter-466 density (port feature, kernel/mod_density.hpp): the game's own traffic document with
  // its pool counts scaled, served as a loose file. 100/100 mounts nothing (the retail document).
  {
    bool on = portDefault;
    for (const auto& x : explicitList) on = on || (!portForcedOff && modLower(x) == "density");
    g.densityOn = on;
    ModDensity want;
    want.traffic = env.trafficDensity;
    want.population = env.populationDensity;
    if (!modDensityValid(want.traffic) || !modDensityValid(want.population)) {
      logBoot(sr::LogSeverity::Error, "[PORT] error density: traffic " + std::to_string(want.traffic) + "% / population " +
                                          std::to_string(want.population) + "% out of range (25.." + std::to_string(kDensityMax) + ", steps of 25); retail");
    } else if (on && want != ModDensity{}) {
      std::vector<uint8_t> top, compiled, text;
      std::string why, sum;
      if (modReadTopEntry(g.discRoot + "/DATA/metropol.AST", "METROPOLISWORLDFLOW_OTHER", &top, &why) &&
          modReadNestedEntry(top, "METROPOLISTRAFFIC", &compiled, &why) &&
          modComposeTrafficDensity(compiled, want, &text, &sum, &why)) {
        const fs::path droot = fs::u8path(g.cacheDir) / "gen" / ("density-" + sha256Hex(text.data(), text.size()).substr(0, 16));
        const fs::path doc = droot / "DATA/_workinprogress/_density/METROPOLISTRAFFIC.xml";
        std::error_code e2;
        const bool have = fs::is_regular_file(doc, e2) && fs::file_size(doc, e2) == text.size();
        if (have || writeFileAtomic(doc, std::string(text.begin(), text.end()))) {
          ModLayer L;
          L.id = "density";
          L.name = "Population and traffic density";
          L.port = true;
          L.roots.push_back(u8(droot));
          g.layers.push_back(L);
          g.densityApplied = want;
          logBoot(sr::LogSeverity::Info, "[PORT] applied density (traffic " + std::to_string(want.traffic) +
                                             "%, population " + std::to_string(want.population) + "%: " + sum + ")");
        } else {
          logBoot(sr::LogSeverity::Error, "[PORT] error density: cannot write " + u8(doc) + "; retail traffic");
        }
      } else {
        logBoot(sr::LogSeverity::Error, "[PORT] error density: " + why + "; retail traffic");
      }
    }
  }
  std::vector<ModLayer> modLayers;
  for (auto& m : g.mods) {
    if (m.status != ModStatus::Ok) continue;
    ModLayer L;
    L.id = m.id;
    L.name = m.name;
    if (!m.patches.empty()) {
      std::string err;
      const std::string dir = generateAssets("mod-" + m.id, m.patches, g.discRoot, g.cacheDir, &err);
      if (dir.empty()) { fail(m, ModStatus::BadPatch, err); continue; }
      L.roots.push_back(dir);
    }
    L.roots.push_back(m.root);
    L.map = m.map;
    m.mounted = true;
    modLayers.push_back(L);
  }
  modLayers.insert(modLayers.end(), g.layers.begin(), g.layers.end());
  g.layers = std::move(modLayers);

  // conflicts: the same file path, or the same loose asset stem under DATA/_workinprogress
  // iter-465 modlib: a mod's own metadata (mod.json, its preview, its screenshots) is not game
  // content: two mods that both ship a "preview.jpg" do not conflict.
  std::unordered_map<std::string, std::unordered_set<std::string>> metaFiles;  // layer id -> rel paths
  for (const auto& m : g.mods) {
    auto& s = metaFiles[modLower(m.id)];
    const fs::path rootP = fs::u8path(m.root);
    if (!m.preview.empty()) s.insert(modLower(u8(fs::u8path(m.preview).lexically_relative(rootP))));
    for (const auto& shot : m.screenshots) s.insert(modLower(u8(fs::u8path(shot).lexically_relative(rootP))));
    // iter-465 fortress: a suit's own animation banks and Fortress pictures live outside DATA and
    // are read by the exe (baseworl.AST bank patch, preview movies), never served as game files:
    // suits of one family ship the same bank bytes at the same paths without conflicting.
    for (const auto& c : m.costumes) {
      for (const auto& [name, path] : c.banks) s.insert(modLower(u8(fs::u8path(path).lexically_relative(rootP))));
      for (const auto* img : {&c.thumbImage, &c.fullImage})
        if (!img->empty()) s.insert(modLower(u8(fs::u8path(*img).lexically_relative(rootP))));
    }
  }
  std::unordered_map<std::string, size_t> pathOwner, stemOwner;  // key -> layer index
  for (size_t li = 0; li < g.layers.size(); ++li) {
    const ModLayer& L = g.layers[li];
    const auto metaIt = metaFiles.find(modLower(L.id));
    std::unordered_set<std::string> mine;
    for (const auto& r : L.roots) {
      const fs::path rp = fs::u8path(r);
      for (auto it = fs::recursive_directory_iterator(rp, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string fn = modPathU8(it->path().filename());
        std::error_code e2;
        if (!fn.empty() && (fn[0] == '.' || fn[0] == '@')) {
          if (it->is_directory(e2)) it.disable_recursion_pending();
          continue;
        }
        if (!it->is_regular_file(e2)) continue;
        const std::string rel = u8(it->path().lexically_relative(rp));
        const std::string low = modLower(rel);
        if (low == "mod.json" || !mine.insert(low).second) continue;
        if (metaIt != metaFiles.end() && metaIt->second.count(low)) continue;
        auto po = pathOwner.find(low);
        if (po != pathOwner.end() && po->second != li) {
          const ModLayer& W = g.layers[po->second];
          g.conflicts.push_back({rel, W.id, L.id, W.name, L.name});
          continue;
        }
        pathOwner.emplace(low, li);
        if (low.rfind("data/_workinprogress/", 0) != 0) continue;
        const size_t dot = fn.rfind('.');
        if (dot == std::string::npos || !modLooseExtensionRegistered(modLower(fn.substr(dot + 1)))) continue;
        std::string stem = modLower(fn.substr(0, dot));
        if (stem.rfind("__asset__", 0) == 0) stem = "#" + stem.substr(9);
        auto so = stemOwner.find(stem);
        if (so != stemOwner.end() && so->second != li) {
          const ModLayer& W = g.layers[so->second];
          g.conflicts.push_back({"asset " + fn.substr(0, dot) + " (" + rel + ")", W.id, L.id, W.name, L.name});
          g.hidden.insert(modLower(u8(it->path())));
          continue;
        }
        stemOwner.emplace(stem, li);
      }
    }
  }
  for (auto& m : g.mods)
    for (const auto& c : g.conflicts)
      if (modLower(c.winnerId) == modLower(m.id) || modLower(c.loserId) == modLower(m.id)) ++m.conflictCount;

  // ONE shared table, composed after resolving the existing layers. Character
  // packs never ship competing replacements of the actor flow's attributes.
  // iter-465 built-in characters (port feature): Bizarro from the player's own PLAYABLEBIZ bundle.
  std::map<std::string, std::vector<uint8_t>> builtinFiles;
  uint64_t builtinBankBytes = 0;
  {
    const char* gate = std::getenv("SR_BUILTIN_CHARACTERS");
    const bool want = gate && *gate ? gate[0] == '1' : (!g_testBoot && modNativeCostumesEnabled() && modCharacterSwitchEnabled());
    if (want) {
      std::string why;
      g.builtinBizarro = extractBuiltinBizarro(g.discRoot, &builtinFiles, &builtinBankBytes, &why);
      if (!g.builtinBizarro) {
        builtinFiles.clear();
        builtinBankBytes = 0;
        logBoot(sr::LogSeverity::Warn, "[COSTUME] built-in Bizarro unavailable: " + why);
      }
    }
  }
  g.convertBankSuits = modNativeCostumesEnabled() && modCharacterSwitchEnabled() && !g_testBoot;
  if (const char* conv = std::getenv("SR_BANK_SUITS_AS_CHARACTERS"); conv && *conv) g.convertBankSuits = conv[0] == '1';
  auto costumes = costumesLocked(g);
  std::map<std::string, std::vector<uint8_t>> bankCharacterFiles;
  const uint64_t bankCharacterBytes = composeBankCharactersLocked(g, costumes, &bankCharacterFiles);
  if (!g.env.wornCostume.empty()) g.selectedCostume = modLower(g.env.wornCostume);
  if (!g.env.startupCostume.empty()) g.selectedCostume = modLower(g.env.startupCostume);
  g.bootCostume = g.selectedCostume;
  if (!costumes.empty()) {
    std::vector<uint8_t> base, bundle, script, composed;
    std::string error;
    std::vector<std::string> overriddenBases;
    for (const auto& L : g.layers) {
      for (const auto& r : L.roots) {
        std::error_code scanError;
        const fs::path loose = fs::u8path(r) / "DATA" / "_workinprogress";
        if (!fs::is_directory(loose, scanError)) continue;
        for (auto it = fs::recursive_directory_iterator(loose, scanError);
             !scanError && it != fs::recursive_directory_iterator(); it.increment(scanError)) {
          if (!it->is_regular_file(scanError)) continue;
          if (modLower(modPathU8(it->path().filename())) != "supermenactorworldflowattributes.das") continue;
          if (g.hidden.count(modLower(u8(it->path())))) continue;
          if (base.empty() && !readBytes(it->path(), &base)) error = "cannot read the winning costume attributes";
          overriddenBases.push_back(modLower(u8(it->path())));
        }
      }
    }
    if (base.empty() && error.empty()) {
      if (!modReadTopEntry(g.discRoot + "/DATA/objtiv.AST", "SUPERMENACTORWORLDFLOW", &bundle, &error) ||
          !modReadNestedEntry(bundle, "SUPERMENACTORWORLDFLOW_SCRIPT", &script, &error) ||
          !modReadNestedEntry(script, "SUPERMENACTORWORLDFLOWATTRIBUTES", &base, &error)) {
        // Error already names the failed stage. No partial table is served.
      }
    }
    // The worn suit at boot (SRWornCostume): the player's saved choice, a stable id
    // ("retail/<1..3>" or "<mod>/<costume>"); one that is no longer mounted falls back to the
    // retail default (0: Superman spawns as SupermanEnt and nothing is requested).
    int startupSlot = 0;
    if (!g.selectedCostume.empty()) {
      startupSlot = modCostumeRetailSlot(g.selectedCostume);
      for (const auto& c : costumes) if (c.id == g.selectedCostume) startupSlot = c.slot;
      if (!startupSlot)
        logBoot(sr::LogSeverity::Warn, "[COSTUME] worn suit " + g.selectedCostume +
                                        " is not mounted; Superman wears the retail default");
    }
    // The session's animation family = the worn suit's. The banks inside baseworl.AST load once,
    // at the game's start (BASEWORLDFLOW_ANIM is read before the title and never again), so a
    // suit with its own banks is worn from a start: its banks are written into the retail slots
    // of the winning baseworl.AST (a mod's whole file, else the disc's) and that copy is served.
    // A failure keeps the retail banks and the retail default suit, never a stretched body.
    std::string sessionFamily = kRetailCostumeFamily;
    std::vector<uint8_t> patchedBaseworld;
    for (const auto& c : costumes) {
      if (c.slot != startupSlot || c.banks.empty() || c.character || !error.empty()) continue;
      fs::path basePath = fs::u8path(g.discRoot) / "DATA/baseworl.AST";
      for (const auto& L : g.layers) {
        bool found = false;
        for (const auto& r : L.roots) {
          std::error_code ec;
          const fs::path cand = fs::u8path(r) / "DATA/baseworl.AST";
          if (fs::is_regular_file(cand, ec) && !g.hidden.count(modLower(u8(cand)))) { basePath = cand; found = true; break; }
        }
        if (found) break;
      }
      std::vector<std::pair<std::string, std::vector<uint8_t>>> banks;
      std::string why;
      bool ok = readBytes(basePath, &patchedBaseworld);
      if (!ok) why = "cannot read " + u8(basePath);
      for (const auto& [name, path] : c.banks) {
        if (!ok) break;
        banks.emplace_back(name, std::vector<uint8_t>{});
        if (!readBytes(fs::u8path(path), &banks.back().second)) { ok = false; why = "cannot read " + path; }
      }
      ok = ok && modPatchBaseworldBanks(&patchedBaseworld, banks, &why);
      if (ok) {
        sessionFamily = c.bankFamily;
        logBoot(sr::LogSeverity::Info, "[COSTUME] animation family " + sessionFamily + " (" + c.id + "): " +
                                        std::to_string(banks.size()) + " own banks written into " + u8(basePath));
      } else {
        patchedBaseworld.clear();
        logBoot(sr::LogSeverity::Warn, "[COSTUME] worn suit " + c.id + ": own animation banks not applied (" + why +
                                        "); Superman wears the retail default");
        startupSlot = 0;
      }
    }
    std::map<std::string, std::vector<uint8_t>> startupScripts;
    for (auto& [name, data] : builtinFiles) startupScripts.emplace(name, std::move(data));
    for (auto& [name, data] : bankCharacterFiles) startupScripts.emplace(name, std::move(data));
    if (error.empty()) {
      std::vector<uint8_t> retailScripts, actorScripts = script;
      for (const char* entry : {"BASEWORLDFLOWFSM_S_7E412353", "BASEWORLDFLOWFSM_S_4982930A",
                                "SUPERMENACTORWORLDFLOWFSM_S_60265281"}) {
        std::vector<uint8_t> source, patched;
        for (const auto& L : g.layers) for (const auto& r : L.roots) {
          std::error_code scanError;
          const fs::path loose = fs::u8path(r) / "DATA/_workinprogress";
          if (!fs::is_directory(loose, scanError)) continue;
          for (auto it = fs::recursive_directory_iterator(loose, scanError);
               !scanError && it != fs::recursive_directory_iterator(); it.increment(scanError)) {
            if (!it->is_regular_file(scanError) ||
                modLower(modPathU8(it->path().filename())) != modLower(std::string(entry) + ".lua") ||
                g.hidden.count(modLower(u8(it->path())))) continue;
            if (source.empty() && !readBytes(it->path(), &source)) error = "cannot read winning costume startup Lua";
            overriddenBases.push_back(modLower(u8(it->path())));
          }
        }
        if (source.empty() && error.empty()) {
          const bool actor = std::string(entry) == "SUPERMENACTORWORLDFLOWFSM_S_60265281";
          if (actor && actorScripts.empty() &&
              (!modReadTopEntry(g.discRoot + "/DATA/objtiv.AST", "SUPERMENACTORWORLDFLOW", &bundle, &error) ||
               !modReadNestedEntry(bundle, "SUPERMENACTORWORLDFLOW_SCRIPT", &actorScripts, &error))) break;
          if (!actor && retailScripts.empty() && !modReadTopEntry(g.discRoot + "/DATA/baseworl.AST", "BASEWORLDFLOW_SCRIPT", &retailScripts, &error)) break;
          if (!modReadNestedEntry(actor ? actorScripts : retailScripts, entry, &source, &error)) break;
        }
        if (!error.empty() || !modComposeCostumeSwitchLua(source, entry, &patched, &error)) break;
        startupScripts.emplace(std::string(entry) + ".lua", std::move(patched));
      }
    }
    bool needsProjects = false;
    for (const auto& costume : costumes)
      needsProjects = needsProjects || !costume.animationProject.empty() || !costume.preloads.empty();
    if (needsProjects && error.empty()) {
      const char* gate = std::getenv("SR_COSTUME_PROJECT_PRELOADS");
      if (!modCharacterSwitchEnabled() && (!gate || std::strcmp(gate, "1") != 0))
        error = "own animation projects need the character switch (SR_CHARACTER_SWITCH) or SR_COSTUME_PROJECT_PRELOADS=1";
      std::vector<uint8_t> preloads, other, withProjects;
      bool foundWinningPreloads = false;
      for (const auto& layer : g.layers) for (const auto& r : layer.roots) {
        std::error_code scanError;
        const fs::path loose = fs::u8path(r) / "DATA/_workinprogress";
        if (!fs::is_directory(loose, scanError)) continue;
        for (auto it = fs::recursive_directory_iterator(loose, scanError);
             !scanError && it != fs::recursive_directory_iterator(); it.increment(scanError)) {
          const std::string filename = modPathU8(it->path().filename());
          if (!filename.empty() && (filename[0] == '.' || filename[0] == '@')) {
            if (it->is_directory(scanError)) it.disable_recursion_pending();
            continue;
          }
          if (!it->is_regular_file(scanError) ||
              modLower(filename) != "baseworldflowpreloads.xml" ||
              g.hidden.count(modLower(u8(it->path())))) continue;
          if (!foundWinningPreloads) {
            foundWinningPreloads = true;
            if (!readBytes(it->path(), &preloads)) error = "cannot read winning costume preloads";
          }
          overriddenBases.push_back(modLower(u8(it->path())));
        }
        if (scanError) error = "cannot enumerate costume preload overrides";
      }
      if (!foundWinningPreloads && error.empty()) {
        if (modReadTopEntry(g.discRoot + "/DATA/baseworl.AST", "BASEWORLDFLOW_OTHER", &other, &error))
          modReadNestedEntry(other, "BASEWORLDFLOWPRELOADS", &preloads, &error);
      }
      if (error.empty() && modComposeCostumePreloads(preloads, costumes, &withProjects, &error))
        startupScripts.emplace("BASEWORLDFLOWPRELOADS.xml", std::move(withProjects));
      if (error.empty()) g.charAnimBytes = characterAnimBytesLocked(g, costumes) + builtinBankBytes + bankCharacterBytes;
      if (error.empty()) composeMissionBanksLocked(g, costumes, &startupScripts);
    }
    // Fortress COSTUMES previews: one thumbnail and one full-size movie per suit, wrapped from
    // the player's own disc COSTUMETHUMB_1 / COSTUMEFULLSIZE_1 (only the image and the inner ids
    // change). A preview failure never blocks the suit itself: the record then has no picture.
    bool previews = false;
    if (error.empty()) {
      std::vector<uint8_t> thumbTmpl, fullTmpl, fallbackThumb, fallbackFull, rgba;
      std::string perr;
      if (modReadTopEntry(g.discRoot + "/DATA/ui.AST", "COSTUMETHUMB_1", &thumbTmpl, &perr) &&
          modReadTopEntry(g.discRoot + "/DATA/ui.AST", "COSTUMEFULLSIZE_1", &fullTmpl, &perr)) {
        modCostumeFallbackRgba(kCostumeThumbDim, &rgba);
        modEncodeDxt5(rgba.data(), kCostumeThumbDim, &fallbackThumb);
        modCostumeFallbackRgba(kCostumeFullDim, &rgba);
        modEncodeDxt5(rgba.data(), kCostumeFullDim, &fallbackFull);
        previews = true;
        for (const auto& c : costumes) {
          struct Kind { int dim; const std::vector<uint8_t>* tmpl; const std::string* given; const std::string* image;
                        const std::vector<uint8_t>* fallback; std::string stem; };
          for (const Kind& k : {Kind{kCostumeThumbDim, &thumbTmpl, &c.thumbnail, &c.thumbImage, &fallbackThumb, modCostumeThumbStem(c.id)},
                                Kind{kCostumeFullDim, &fullTmpl, &c.fullsize, &c.fullImage, &fallbackFull, modCostumeFullStem(c.id)}}) {
            if (!k.given->empty()) continue;  // the pack ships its own scoped movie
            std::vector<uint8_t> blocks, movie;
            std::string why;
            const bool own = !k.image->empty() && modReadDdsDxt5(*k.image, k.dim, &blocks, &why);
            if (!k.image->empty() && !own)
              logBoot(sr::LogSeverity::Warn, "[COSTUME] " + c.id + " preview not used (" + why + "); fallback card");
            uint32_t tag = 0x811C9DC5u;
            for (unsigned char ch : modLower(k.stem)) { tag ^= ch; tag *= 0x01000193u; }
            if (!modComposeCostumePreviewMovie(*k.tmpl, k.dim, own ? blocks : *k.fallback, tag, &movie, &why)) {
              logBoot(sr::LogSeverity::Error, "[COSTUME] preview movie " + k.stem + ": " + why);
              previews = false;
              continue;
            }
            startupScripts.emplace(k.stem + ".ast", std::move(movie));
          }
        }
      } else {
        logBoot(sr::LogSeverity::Error, "[COSTUME] previews unavailable: " + perr);
      }
    }
    if (!base.empty() && error.empty() && modComposeCostumeAttributes(base, costumes, &composed, &error, startupSlot, sessionFamily)) {
      std::vector<uint8_t> identity = composed;
      if (!patchedBaseworld.empty()) {
        const std::string digest = "DATA/baseworl.AST" + sha256Hex(patchedBaseworld.data(), patchedBaseworld.size());
        identity.insert(identity.end(), digest.begin(), digest.end());
      }
      for (const auto& [name, data] : startupScripts) {
        identity.insert(identity.end(), name.begin(), name.end());
        identity.push_back(0);
        identity.insert(identity.end(), data.begin(), data.end());
      }
      const fs::path root = fs::u8path(g.cacheDir) / "gen" / ("costumes-" + sha256Hex(identity.data(), identity.size()));
      const fs::path dir = root / "DATA/_workinprogress/_costume_registry";
      bool written = writeFileAtomic(dir / "SUPERMENACTORWORLDFLOWATTRIBUTES.das", std::string(composed.begin(), composed.end()));
      for (const auto& [name, data] : startupScripts)
        written = writeFileAtomic(dir / name, std::string(data.begin(), data.end())) && written;
      if (!patchedBaseworld.empty()) {
        // content-addressed: an existing complete copy of the same size is the same bytes
        std::error_code ec;
        const fs::path bw = root / "DATA/baseworl.AST";
        if (!(fs::is_regular_file(bw, ec) && fs::file_size(bw, ec) == patchedBaseworld.size()))
          written = writeFileAtomic(bw, std::string(patchedBaseworld.begin(), patchedBaseworld.end())) && written;
      }
      if (written) {
        logBoot(sr::LogSeverity::Info, "[COSTUME] worn suit at boot: slot " + std::to_string(startupSlot) +
                                        (g.selectedCostume.empty() ? std::string(" (retail default)") : " (" + g.selectedCostume + ")"));
        ModLayer layer;
        layer.id = "_costume_registry";
        layer.name = "Combined costume registry";
        layer.port = true;
        layer.roots.push_back(u8(root));
        g.layers.insert(g.layers.begin(), std::move(layer));
        // The combined table incorporates the winning base. Hide the source
        // copy so equal-priority engine scan order cannot undo that composition.
        for (const auto& path : overriddenBases) g.hidden.insert(path);
        g.costumeRegistryReady = true;
        g.costumePreviews = previews;
        g.sessionFamily = sessionFamily;
        logBoot(sr::LogSeverity::Info, "[COSTUME] combined registry: " + std::to_string(costumes.size()) +
                                        " additions; retail Costume1..4 preserved");
      } else error = "cannot write the generated costume table";
    }
    if (!g.costumeRegistryReady)
      logBoot(sr::LogSeverity::Error, "[COSTUME] registry unavailable: " + error);
  }

  // the log (count-safe: one line per mod / feature / conflict)
  std::string joined;
  for (const auto& L : g.layers)
    if (!L.port) joined += (joined.empty() ? "" : ", ") + L.id;
  if (!joined.empty())
    logBoot(sr::LogSeverity::Info, "[MOD] enabled (highest priority first): " + joined + "  [source: " + g.source +
                                       "; root \"" + g.modsDir + "\"]");
  for (const auto& m : g.mods)
    if (m.status != ModStatus::Ok && m.status != ModStatus::Disabled)
      logBoot(sr::LogSeverity::Error, "[MOD] error " + m.id + ": " + modStatusName(m.status) + " - " + m.reason);
  for (const auto& f : g.feats) {
    if (f.applied)
      logBoot(sr::LogSeverity::Info, "[PORT] applied " + f.id + " (" + std::to_string(f.assets.size()) +
                                         " generated asset(s) from the player's disc)");
    else if (f.status != ModStatus::Off)
      logBoot(sr::LogSeverity::Error, "[PORT] error " + f.id + ": " + modStatusName(f.status) + " - " + f.reason);
  }
  for (const auto& c : g.conflicts)
    logBoot(sr::LogSeverity::Warn, "[MOD] conflict " + c.what + ": " + c.winnerId + " wins over " + c.loserId);
}

// Lane E (step 5 E3-E6 r3): the [ASSET] MAIN receipt's candidates (kernel/asset_receipt.hpp) - the disc's
// ui.AST:MAIN always, the custom_frontend port feature's generated MAIN.ast when that layer is mounted - and the
// expectation this boot's mounts imply. A mod that also overrides MAIN wins the stem conflict (mods sit above port
// features, docs/MODS.md 6: "[MOD] conflict asset MAIN (...): <mod> wins over custom_frontend") and the port's MAIN
// is hidden; the expectation line names it, and the registered MAIN then matches neither candidate (source=unknown).
void setupMainReceipt(const Mgr& g) {
  namespace ar = assetreceipt;
  if (g.discRoot.empty()) return;
  std::vector<ar::Candidate> cands;
  std::string err, retailSha = "-", overSha = "-", overFile;
  {
    std::vector<uint8_t> entry;
    ar::Candidate c;
    if (modReadTopEntry(g.discRoot + "/DATA/ui.AST", "MAIN", &entry, &err) &&
        ar::candidateFromEntry("MAIN", "retail", "DATA/ui.AST:MAIN", entry, &c, &err)) {
      retailSha = c.sha256;
      cands.push_back(std::move(c));
    } else {
      logBoot(sr::LogSeverity::Error, "[ASSET] MAIN: the disc's MAIN cannot be read: " + err);
    }
  }
  for (const auto& L : g.layers) {
    if (!L.port || L.id != "custom_frontend" || L.roots.empty()) continue;
    const fs::path p = fs::u8path(L.roots[0]) / "DATA" / "_workinprogress" / "custom_frontend" / "MAIN.ast";
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) continue;
    std::string bytes;
    ar::Candidate c;
    if (readFile(p, &bytes) &&
        ar::candidateFromEntry("MAIN", "override", u8(p), std::vector<uint8_t>(bytes.begin(), bytes.end()), &c, &err)) {
      overSha = c.sha256;
      overFile = u8(p);
      cands.push_back(std::move(c));
    } else {
      logBoot(sr::LogSeverity::Error, "[ASSET] MAIN: the port feature's MAIN cannot be read: " + err);
    }
  }
  std::string modWins;
  for (const auto& c : g.conflicts)
    if (modLower(c.what).rfind("asset main (", 0) == 0) modWins = c.winnerId;
  std::string expect = "[ASSET] MAIN expect source=";
  if (!modWins.empty()) expect += "mod " + modWins + " (wins the MAIN stem over custom_frontend; docs/MODS.md 6)";
  else if (!overFile.empty()) expect += "override (port feature custom_frontend)";
  else expect += "retail";
  expect += " | retail DATA/ui.AST:MAIN sha256=" + retailSha + " | override sha256=" + overSha;
  ar::setCandidates(std::move(cands), expect);
}

}  // namespace

// ================================================================ public
// "retail/1".."retail/3" = the retail Fortress suits (Costume1..3: SupermanEnt, SupermanGrayEnt,
// SupermanClassicEnt). 0 for anything else.
int modCostumeRetailSlot(const std::string& id) {
  const std::string k = modLower(id);
  if (k.size() == 8 && k.rfind("retail/", 0) == 0 && k[7] >= '1' && k[7] <= '3') return k[7] - '0';
  return 0;
}

std::string modPathU8(const fs::path& p) {
  const auto u = p.generic_u8string();
  return std::string(u.begin(), u.end());
}

const char* modStatusName(ModStatus s) {
  const int i = static_cast<int>(s);
  return (i >= 0 && i < static_cast<int>(sizeof kStatusNames / sizeof kStatusNames[0])) ? kStatusNames[i] : "?";
}

std::string modLower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool modValidId(const std::string& n) {
  if (n.empty() || n.size() > 64 || n[0] == '.') return false;
  for (char c : n)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) return false;
  return true;
}

bool modLooseExtensionRegistered(const std::string& extLower) {
  for (const char* e : kLooseExts)
    if (extLower == e) return true;
  return false;
}

std::string modAssetPrefix(const std::string& modId) {
  char hash[17] = {};
  std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(fnv1a64Upper(modId)));
  return std::string("SRM_") + hash + "_";
}

std::string modCategoryLabel(const std::string& category) {
  if (category == "characters") return "Skins & Characters";
  if (category == "missions") return "Missions";
  if (category == "worlds") return "Worlds";
  if (category == "gameplay") return "Gameplay";
  if (category == "visuals") return "Visuals";
  if (category == "audio") return "Audio";
  if (category == "interface") return "Interface";
  return "Other";
}

bool modReadTopEntry(const std::string& containerPath, const std::string& entry, std::vector<uint8_t>* out,
                     std::string* err) {
  std::FILE* f = openRead(fs::u8path(containerPath));
  if (!f) { *err = "cannot open " + containerPath; return false; }
  TopEntry te;
  const bool ok = findTopEntry(f, entry, &te, err);
  if (ok) {
    out->resize(static_cast<size_t>(te.size));
    _fseeki64(f, static_cast<int64_t>(te.off), SEEK_SET);
    if (std::fread(out->data(), 1, out->size(), f) != out->size()) { *err = "entry truncated"; std::fclose(f); return false; }
  }
  std::fclose(f);
  return ok;
}

std::string modDataFingerprint(const std::string& discRoot) {
  const fs::path data = fs::u8path(discRoot) / "DATA";
  std::vector<std::pair<std::string, fs::path>> files;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(data, ec)) {
    const std::string n = modPathU8(e.path().filename());
    std::string up = n;
    for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (e.is_regular_file(ec) && up.size() > 4 && up.compare(up.size() - 4, 4, ".AST") == 0) files.push_back({up, e.path()});
  }
  std::sort(files.begin(), files.end());
  Sha256 h;
  for (const auto& kv : files) {
    std::FILE* f = openRead(kv.second);
    uint8_t head[0x40] = {};
    std::vector<uint8_t> toc;
    size_t got = 0;
    uint64_t size = 0;
    if (f) {
      got = std::fread(head, 1, 0x40, f);
      if (got == 0x40 && std::memcmp(head, "BGFA1.05", 8) == 0) {
        const uint32_t to = static_cast<uint32_t>(leN(head + 0x10, 4)), tl = static_cast<uint32_t>(leN(head + 0x18, 4));
        toc.resize(tl);
        _fseeki64(f, to, SEEK_SET);
        toc.resize(std::fread(toc.data(), 1, tl, f));
      }
      _fseeki64(f, 0, SEEK_END);
      size = static_cast<uint64_t>(_ftelli64(f));
      std::fclose(f);
    }
    Sha256 inner;
    inner.update(head, got);
    inner.update(toc.data(), toc.size());
    uint8_t d[32];
    inner.final(d);
    h.update(kv.first.data(), kv.first.size());
    const uint8_t z = 0;
    h.update(&z, 1);
    uint8_t le[8];
    for (int i = 0; i < 8; ++i) le[i] = static_cast<uint8_t>(size >> (8 * i));
    h.update(le, 8);
    h.update(d, 32);
  }
  uint8_t out[32];
  h.final(out);
  return std::string(kGameTag) + ":" + hexOf(out, 8);
}

ModBootEnv modBootEnvFromProcess() {
  ModBootEnv e;
  e.contentRoot = contentRootDir();
#ifdef _WIN32
  wchar_t buf[MAX_PATH * 2] = {};
  const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof buf / sizeof buf[0]));
  if (n) e.exeDir = u8(fs::path(buf).parent_path());
#endif
  if (const char* v = std::getenv("SR_MODS")) { e.hasSrMods = true; e.srMods = v; }
  if (const char* v = std::getenv("SR_COSTUME")) e.startupCostume = v;
  {
    // the suit Superman wears (Fortress COSTUMES): <save root>/settings/costume.ini "worn=<id>"
    std::string worn;
    if (readFile(xamContentRoot() / "settings" / "costume.ini", &worn)) {
      size_t pos = 0;
      while (pos < worn.size()) {
        size_t nl = worn.find('\n', pos);
        if (nl == std::string::npos) nl = worn.size();
        const std::string line = trim(worn.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.rfind("worn=", 0) == 0) e.wornCostume = trim(line.substr(5));
      }
    }
  }
  if (const char* v = std::getenv("SR_PORT_FEATURES")) { e.hasPort = true; e.srPortFeatures = v; }
  if (const char* v = std::getenv("SR_CUSTOM_FRONTEND")) { e.hasCustomFrontend = true; e.srCustomFrontend = v; }
  if (const char* v = std::getenv("SR_PORT_FEATURES_DIR")) e.portDir = v;
  const char* inst = std::getenv("SR_INSTANCE_ID");
  e.automated = inst && *inst;
  e.allowStateWrite = !e.automated;
  // iter-465 modlib: a menu roll on its OWN content root (tools/modlib/make_test_croot.py) may
  // let the menu save that root's mods/mods.json (SR_MODS=@player + SR_MODS_ALLOW_WRITE=1), so
  // a roll can show a toggle, a reorder and a suit checkbox end to end. Never set by players.
  if (const char* w = std::getenv("SR_MODS_ALLOW_WRITE"); w && w[0] == '1') e.allowStateWrite = true;
  // host options (the fe lane's store, <save root>/settings/mod_options.ini: key=value)
  std::string text;
  if (readFile(xamContentRoot() / "settings" / "mod_options.ini", &text)) {
    size_t pos = 0;
    while (pos < text.size()) {
      size_t nl = text.find('\n', pos);
      if (nl == std::string::npos) nl = text.size();
      std::string line = text.substr(pos, nl - pos);
      pos = nl + 1;
      const size_t h = line.find('#');
      if (h != std::string::npos) line = line.substr(0, h);
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
      if (!k.empty() && !v.empty()) e.options[k] = std::atoi(v.c_str());
    }
  }
  // iter-466 density: the player's game_options.ini, or a roll's SR_TRAFFIC_DENSITY / SR_POPULATION_DENSITY
  const ModDensity d = modDensityLoad();
  e.trafficDensity = d.traffic;
  e.populationDensity = d.population;
  if (const char* v = std::getenv("SR_TRAFFIC_DENSITY"); v && *v) e.trafficDensity = std::atoi(v);
  if (const char* v = std::getenv("SR_POPULATION_DENSITY"); v && *v) e.populationDensity = std::atoi(v);
  return e;
}

void modManagerBoot() {
  std::call_once(g_bootOnce, [] {
    Mgr& g = mgr();
    std::lock_guard<std::mutex> lk(g.mu);
    const ModBootEnv env = modBootEnvFromProcess();
    // (g.mu stays locked: bootLocked does not touch it)
    Mgr fresh;
    bootLocked(fresh, env);
    g.env = fresh.env; g.discRoot = fresh.discRoot; g.modsDir = fresh.modsDir; g.cacheDir = fresh.cacheDir;
    g.source = fresh.source; g.mods = std::move(fresh.mods); g.feats = std::move(fresh.feats);
    g.layers = std::move(fresh.layers); g.conflicts = std::move(fresh.conflicts); g.hidden = std::move(fresh.hidden);
    g.order = fresh.order; g.disabled = fresh.disabled; g.bootOrder = fresh.bootOrder;
    g.bootDisabled = fresh.bootDisabled; g.fromState = fresh.fromState; g.booted = true;
    g.disabledCostumes = fresh.disabledCostumes; g.bootDisabledCostumes = fresh.bootDisabledCostumes;
    g.costumeRegistryReady = fresh.costumeRegistryReady; g.costumePreviews = fresh.costumePreviews;
    g.selectedCostume = fresh.selectedCostume; g.bootCostume = fresh.bootCostume;
    g.sessionFamily = fresh.sessionFamily;
    g.charAnimBytes = fresh.charAnimBytes;
    g.builtinBizarro = fresh.builtinBizarro;
    g.convertBankSuits = fresh.convertBankSuits;
    g.densityApplied = fresh.densityApplied;
    g.densityOn = fresh.densityOn;
    // iter-466 density: the PC SETTINGS WORLD rows (kernel/pc_settings.hpp) work when the port
    // feature is on (a player launch); natural rolls leave them disabled.
    pcsettings::setAvailable(kTrafficDensityKey, g.densityOn);
    pcsettings::setAvailable(kPopulationDensityKey, g.densityOn);
    setupMainReceipt(g);   // lane E E3-E6 r3: the [ASSET] MAIN receipt (the real boot only)
  });
}

void modManagerResetForTest(const ModBootEnv& env) {
  std::call_once(g_bootOnce, [] {});  // the real boot never runs after the seam
  g_testBoot = true;
  releaseGenerationUses();   // lane E E2: the previous test boot's mounted set is replaced
  Mgr& g = mgr();
  std::lock_guard<std::mutex> lk(g.mu);
  Mgr fresh;
  bootLocked(fresh, env);
  g.env = fresh.env; g.discRoot = fresh.discRoot; g.modsDir = fresh.modsDir; g.cacheDir = fresh.cacheDir;
  g.source = fresh.source; g.mods = std::move(fresh.mods); g.feats = std::move(fresh.feats);
  g.layers = std::move(fresh.layers); g.conflicts = std::move(fresh.conflicts); g.hidden = std::move(fresh.hidden);
  g.order = fresh.order; g.disabled = fresh.disabled; g.bootOrder = fresh.bootOrder;
  g.bootDisabled = fresh.bootDisabled; g.fromState = fresh.fromState; g.booted = true;
  g.disabledCostumes = fresh.disabledCostumes; g.bootDisabledCostumes = fresh.bootDisabledCostumes;
  g.costumeRegistryReady = fresh.costumeRegistryReady; g.costumePreviews = fresh.costumePreviews;
  g.selectedCostume = fresh.selectedCostume; g.bootCostume = fresh.bootCostume;
  g.sessionFamily = fresh.sessionFamily;
  g.charAnimBytes = fresh.charAnimBytes;
  g.builtinBizarro = fresh.builtinBizarro;
  g.convertBankSuits = fresh.convertBankSuits;
  g.densityApplied = fresh.densityApplied;
  g.densityOn = fresh.densityOn;
}

ModDensity modDensityApplied() {
  modManagerBoot();
  Mgr& g = mgr();
  std::lock_guard<std::mutex> lk(g.mu);
  return g.densityApplied;
}

const std::vector<ModLayer>& modActiveLayers() {
  modManagerBoot();
  return mgr().layers;  // written once at boot (or by the test seam), read-only afterwards
}

bool modHiddenHostPath(const std::string& hostPath) {
  modManagerBoot();
  const Mgr& g = mgr();
  if (g.hidden.empty()) return false;
  std::string p = hostPath;
  for (char& c : p) if (c == '\\') c = '/';
  return g.hidden.count(modLower(p)) != 0;
}

std::string modBootSummary() {
  modManagerBoot();
  const Mgr& g = mgr();
  std::string mods, port;
  for (const auto& L : g.layers) (L.port ? port : mods) += ((L.port ? port : mods).empty() ? "" : ", ") + L.id;
  return "mods: " + (mods.empty() ? std::string("none") : mods) + "; port: " + (port.empty() ? std::string("none") : port);
}

// ---------------------------------------------------------------- menu
std::vector<ModInfo> modMenuList() {
  modManagerBoot();
  Mgr& g = mgr();
  std::lock_guard<std::mutex> lk(g.mu);
  std::vector<ModInfo> out;
  for (const auto& id : g.order)
    for (const auto& m : g.mods)
      if (modLower(m.id) == modLower(id)) {
        ModInfo c = m;
        c.enabled = !g.disabled.count(modLower(m.id));
        out.push_back(c);
      }
  for (const auto& m : g.mods)  // duplicates (not in the order)
    if (m.status == ModStatus::Duplicate) out.push_back(m);
  return out;
}

std::vector<ModCostumeInfo> modCostumeList() {
  modManagerBoot();
  Mgr& g = mgr();
  std::lock_guard<std::mutex> lk(g.mu);
  return costumesLocked(g);
}

bool modNativeCostumesEnabled() {
  // iter-465 player: the costume picker follows the PORT FEATURE switch when the gate is unset
  // (on for players, off for natural rolls; kernel/host_automation.hpp portFeaturesFor).
  const char* gate = std::getenv("SR_NATIVE_COSTUMES");
  if (gate && *gate) return gate[0] == '1';
  return automation::portFeaturesFor(std::getenv("SR_INSTANCE_ID"), std::getenv("SR_PORT_FEATURES"));
}

bool modCharacterSwitchEnabled() {
  // iter-465 character switch (port feature): own-project characters are preloaded, the animation
  // memory grows to hold them, and WEAR swaps the character in. Unset = the costume gate.
  const char* gate = std::getenv("SR_CHARACTER_SWITCH");
  if (gate && *gate) return gate[0] == '1';
  return modNativeCostumesEnabled();
}

uint64_t modCharacterAnimBytes() {
  modManagerBoot();
  return mgr().costumeRegistryReady ? mgr().charAnimBytes : 0;  // immutable after boot
}

bool modCostumeFindActive(const std::string& id, ModCostumeInfo* out) {
  modManagerBoot();
  Mgr& g = mgr();
  std::lock_guard<std::mutex> lk(g.mu);
  if (!out || !g.costumeRegistryReady || !validCostumeId(id)) return false;
  const auto key = modLower(id);
  for (const auto& costume : costumesLocked(g)) if (costume.id == key) {
    *out = costume;
    return true;
  }
  return false;
}

std::vector<ModCostumeChoice> modCostumePackChoices(const std::string& modId) {
  modManagerBoot();
  Mgr& g = mgr();
  std::lock_guard<std::mutex> lk(g.mu);
  const auto active = costumesLocked(g);
  std::vector<ModCostumeChoice> out;
  for (const auto& m : g.mods) {
    if (modLower(m.id) != modLower(modId) || m.status == ModStatus::Duplicate || m.status == ModStatus::Reserved) continue;
    for (const auto& c : m.costumes) {
      ModCostumeChoice choice{c, !g.disabledCostumes.count(c.id), false};
      for (const auto& a : active) if (a.id == c.id && g.costumeRegistryReady) {
        choice.costume.slot = a.slot;
        choice.active = true;
        break;
      }
      out.push_back(std::move(choice));
    }
  }
  return out;
}

bool modCostumeSetIncluded(const std::string& id, bool included) {
  modManagerBoot();
  Mgr& g = mgr();
  std::lock_guard<std::mutex> lk(g.mu);
  const std::string key = modLower(id);
  if (!g.fromState || !g.env.allowStateWrite || !validCostumeId(key)) return false;
  bool known = false;
  for (const auto& m : g.mods)
    if (m.status == ModStatus::Ok || m.status == ModStatus::Disabled ||
        m.status == ModStatus::MissingDependency || m.status == ModStatus::DeclaredConflict)
      for (const auto& c : m.costumes) known = known || c.id == key;
  if (!known) return false;
  const auto before = g.disabledCostumes;
  if (included) g.disabledCostumes.erase(key);
  else g.disabledCostumes.insert(key);
  if (!saveStateLocked(g)) { g.disabledCostumes = before; return false; }
  return true;
}

bool modCostumeRegistryReady() {
  modManagerBoot();
  return mgr().costumeRegistryReady;  // immutable after boot
}

bool modCostumePreviewsReady() {
  modManagerBoot();
  return mgr().costumeRegistryReady && mgr().costumePreviews;  // immutable after boot
}

std::string modCostumeSessionFamily() {
  modManagerBoot();
  return mgr().costumeRegistryReady ? mgr().sessionFamily : std::string(kRetailCostumeFamily);  // immutable after boot
}

bool modCostumeNoteWorn(const std::string& id) {
  modManagerBoot();
  Mgr& g = mgr();
  std::lock_guard<std::mutex> lk(g.mu);
  const std::string key = modLower(id);
  bool valid = modCostumeRetailSlot(key) != 0;
  if (!valid && g.costumeRegistryReady)
    for (const auto& c : costumesLocked(g)) valid = valid || c.id == key;
  if (!valid) return false;
  // The worn suit is the player's, like the settings next to it: the save root's settings
  // folder (a roll's private SR_CONTENT_DIR / per-instance root, never the player's).
  const fs::path ini = xamContentRoot() / "settings" / "costume.ini";
  const std::string text = "# Superman Returns PC port: the suit Superman wears (Fortress of Solitude > COSTUMES).\n"
                           "# retail/1..3 = the retail suits; <mod>/<costume> = a mod suit. Written by the game.\n"
                           "worn=" + key + "\n";
  if (!writeFileAtomic(ini, text)) {
    logBoot(sr::LogSeverity::Error, "[COSTUME] cannot save the worn suit to " + u8(ini));
    return false;
  }
  g.wornCostume = key;
  logBoot(sr::LogSeverity::Info, "[COSTUME] worn suit saved: " + key + " (" + u8(ini) + ")");
  return true;
}

std::string modCostumeWorn() {
  modManagerBoot();
  Mgr& g = mgr();
  std::lock_guard<std::mutex> lk(g.mu);
  return g.wornCostume.empty() ? g.selectedCostume : g.wornCostume;
}

std::string modCostumeSelected() {
  modManagerBoot();
  Mgr& g = mgr();
  std::lock_guard<std::mutex> lk(g.mu);
  return g.selectedCostume;
}

bool modCostumeSelectForRestart(const std::string& id) {
  modManagerBoot();
  Mgr& g = mgr();
  std::lock_guard<std::mutex> lk(g.mu);
  const std::string selected = modLower(id);
  if (!g.fromState || !g.env.allowStateWrite) return false;
  bool valid = selected.empty(); // empty restores the retail default; original picker stays intact
  for (const auto& m : g.mods)
    if (m.mounted && m.status == ModStatus::Ok && !g.disabled.count(modLower(m.id)))
      for (const auto& c : m.costumes)
        valid = valid || (c.id == selected && !g.disabledCostumes.count(c.id) && !g.bootDisabledCostumes.count(c.id));
  if (!valid || (!selected.empty() && !g.costumeRegistryReady)) return false;
  const std::string before = g.selectedCostume;
  g.selectedCostume = selected;
  if (!saveStateLocked(g)) { g.selectedCostume = before; return false; }
  return true;
}

std::vector<PortFeatureInfo> modMenuFeatures() {
  modManagerBoot();
  Mgr& g = mgr();
  std::lock_guard<std::mutex> lk(g.mu);
  return g.feats;
}

std::vector<ModConflictInfo> modMenuConflicts() {
  modManagerBoot();
  Mgr& g = mgr();
  std::lock_guard<std::mutex> lk(g.mu);
  return g.conflicts;
}

bool modMenuSetEnabled(const std::string& id, bool on) {
  modManagerBoot();
  Mgr& g = mgr();
  std::lock_guard<std::mutex> lk(g.mu);
  bool known = false;
  for (const auto& x : g.order) known = known || modLower(x) == modLower(id);
  if (!known) return false;
  const auto before = g.disabled;
  if (on) g.disabled.erase(modLower(id));
  else g.disabled.insert(modLower(id));
  if (!saveStateLocked(g)) {
    g.disabled = before;
    return false;
  }
  return true;
}

bool modMenuMove(const std::string& id, int delta) {
  modManagerBoot();
  Mgr& g = mgr();
  std::lock_guard<std::mutex> lk(g.mu);
  for (size_t i = 0; i < g.order.size(); ++i) {
    if (modLower(g.order[i]) != modLower(id)) continue;
    const long j = static_cast<long>(i) + delta;
    if (j < 0 || j >= static_cast<long>(g.order.size())) return false;
    std::swap(g.order[i], g.order[static_cast<size_t>(j)]);
    if (!saveStateLocked(g)) {
      std::swap(g.order[i], g.order[static_cast<size_t>(j)]);
      return false;
    }
    return true;
  }
  return false;
}

bool modMenuRestartRequired() {
  modManagerBoot();
  Mgr& g = mgr();
  std::lock_guard<std::mutex> lk(g.mu);
  return g.order != g.bootOrder || g.disabled != g.bootDisabled ||
         g.disabledCostumes != g.bootDisabledCostumes || g.selectedCostume != g.bootCostume;
}

bool modMenuAutomated() {
  modManagerBoot();
  return mgr().env.automated;
}

std::string modDiscRoot() {
  modManagerBoot();
  return mgr().discRoot;  // immutable after boot
}

std::string modMenuModsDir() {
  modManagerBoot();
  return mgr().modsDir;  // immutable after boot
}

void modMenuSetRestartHookForTest(bool (*spawn)(std::string* err)) { g_spawnHook = spawn; }

bool modMenuRestart(std::string* whyNot) {
  modManagerBoot();
  if (mgr().env.automated && !g_spawnHook) {
    if (whyNot) *whyNot = "restart is off in automated runs (SR_INSTANCE_ID)";
    return false;
  }
  std::string err;
  bool ok = false;
  if (g_spawnHook) {
    ok = g_spawnHook(&err);
  } else {
    // Use the menu's common shutdown/relaunch path. It waits for the old process
    // to exit; never start a second game while this one holds the game slot.
    modMenuRequestRestart();
    ok = true;
  }
  if (!ok) {
    if (whyNot) *whyNot = err;
    SR_LOG_BOOT(sr::LogSeverity::Error, "[MOD] restart failed: %s", err.c_str());
    return false;
  }
  SR_LOG_BOOT(sr::LogSeverity::Warn, "[MOD] restart: shutdown requested; relaunch after exit");
  return true;
}

bool modMenuCommand(const std::string& cmd, const std::string& args, std::vector<ModMenuCall>* out) {
  if (cmd.compare(0, 6, "SR_Mod") != 0) return false;
  auto text = [&](int idx, const char* key, const std::string& v) {
    if (!v.empty()) out->push_back({"SR_ModText", {ModMenuArg::Int(idx), ModMenuArg::Str(key), ModMenuArg::Str(v)}});
  };
  auto list = [&]() {
    const auto mods = modMenuList();
    const auto conf = modMenuConflicts();
    int32_t state = (modMenuRestartRequired() ? 1 : 0) | (modMenuAutomated() ? 8 : 0);
    for (const auto& L : modActiveLayers()) state |= L.port ? 4 : 2;
    out->push_back({"SR_ModBegin", {ModMenuArg::Int(static_cast<int32_t>(mods.size())), ModMenuArg::Int(state)}});
    for (size_t i = 0; i < mods.size(); ++i) {
      const ModInfo& m = mods[i];
      const int32_t flags = (m.enabled ? 1 : 0) | (m.mounted ? 2 : 0) | (m.conflictCount ? 4 : 0) |
                            (m.preview.empty() ? 0 : 8) | (m.isZip ? 16 : 0) | (m.legacy ? 32 : 0) |
                            (static_cast<int32_t>(m.status) << 8);
      const int idx = static_cast<int>(i);
      out->push_back({"SR_ModItem", {ModMenuArg::Int(idx), ModMenuArg::Str(m.id), ModMenuArg::Int(flags)}});
      text(idx, "name", m.name);
      text(idx, "author", m.author);
      text(idx, "version", m.version);
      text(idx, "description", m.description);
      text(idx, "category", m.category);
      text(idx, "preview", m.preview);
      text(idx, "status", modStatusName(m.status));
      text(idx, "reason", m.reason);
      for (const auto& c : conf) {
        const bool wins = modLower(c.winnerId) == modLower(m.id);
        const bool loses = modLower(c.loserId) == modLower(m.id);
        if (!wins && !loses) continue;
        out->push_back({"SR_ModConflict", {ModMenuArg::Int(idx), ModMenuArg::Int(wins ? 1 : 0),
                                           ModMenuArg::Str(c.what + " <-> " + (wins ? c.loser : c.winner))}});
      }
    }
    out->push_back({"SR_ModEnd", {ModMenuArg::Int(static_cast<int32_t>(mods.size())),
                                  ModMenuArg::Int(static_cast<int32_t>(conf.size()))}});
  };
  std::string a = trim(args);
  const size_t sp = a.find(' ');
  const std::string a0 = sp == std::string::npos ? a : a.substr(0, sp);
  const std::string a1 = sp == std::string::npos ? "" : trim(a.substr(sp + 1));
  if (cmd == "SR_ModList") {
    list();
  } else if (cmd == "SR_ModCostumes") {
    const auto costumes = modNativeCostumesEnabled() && modCostumeRegistryReady()
                            ? modCostumeList() : std::vector<ModCostumeInfo>{};
    const bool previews = modCostumePreviewsReady();
    const int32_t count = static_cast<int32_t>(costumes.size());
    // The screen's name line is ONE line, mGridWidth wide, font 24 (ThumbViewer.CreateTextFields):
    // about 22 capitals at 24 (live: 18 fit, 29 wrapped into the description). A longer name gets
    // a smaller font for the whole screen (down to 16); a name too long even then is cut with
    // "..." and its full text opens the description.
    constexpr int kNameFont = 24, kMinNameFont = 16, kCharsAt24 = 22;
    constexpr size_t kMaxChars = size_t(kCharsAt24) * kNameFont / kMinNameFont;
    int nameFont = kNameFont;
    for (const auto& c : costumes)
      if (!c.name.empty()) nameFont = std::min(nameFont, std::max(kMinNameFont, int(kCharsAt24 * kNameFont / c.name.size())));
    out->push_back({"SR_CostumeBegin", {ModMenuArg::Int(count)}});
    for (int32_t i = 0; i < count; ++i) {
      const auto& c = costumes[i];
      const bool cut = c.name.size() > kMaxChars;
      std::string name = c.name;
      if (cut) {
        name.resize(kMaxChars - 3);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        name += "...";
      }
      out->push_back({"SR_CostumeItem", {ModMenuArg::Int(i), ModMenuArg::Str(c.id), ModMenuArg::Str(name)}});
      // the retail record shape (artassets/Costumes/<kind>/<stem>.swf): the pack's own movie
      // stem, else the boot's generated preview (kernel/mod_costume_preview.hpp)
      const std::string thumb = !c.thumbnail.empty() ? c.thumbnail : (previews ? modCostumeThumbStem(c.id) : "");
      const std::string full = !c.fullsize.empty() ? c.fullsize : (previews ? modCostumeFullStem(c.id) : "");
      // the description field has about four lines above the button hints (about 32 capitals a
      // line, live): a longer text is cut at a word with "..." (the MODS library shows it whole)
      std::string descr = cut ? c.name + ": " + c.description : c.description;
      constexpr size_t kMaxDescr = 110;
      if (descr.size() > kMaxDescr) {
        size_t at = descr.rfind(' ', kMaxDescr - 3);
        descr.resize(at == std::string::npos || at < kMaxDescr / 2 ? kMaxDescr - 3 : at);
        while (!descr.empty() && (descr.back() == ' ' || descr.back() == ',' || descr.back() == '.')) descr.pop_back();
        descr += "...";
      }
      for (const auto& field : {std::make_pair("Description", descr),
                               std::make_pair("ThumbFile", thumb.empty() ? "" : modCostumeThumbPath(thumb)),
                               std::make_pair("FullSizeFile", full.empty() ? "" : modCostumeFullPath(full))})
        out->push_back({"SR_CostumeText", {ModMenuArg::Int(i), ModMenuArg::Str(field.first), ModMenuArg::Str(field.second)}});
    }
    out->push_back({"SR_CostumeEnd", {ModMenuArg::Int(count), ModMenuArg::Int(nameFont)}});
  } else if (cmd == "SR_ModSuitList" || cmd == "SR_ModSuitSet") {
    std::string pack = a0;
    if (cmd == "SR_ModSuitSet") {
      const bool on = a1 == "1";
      const bool ok = (a1 == "0" || a1 == "1") && modCostumeSetIncluded(a0, on);
      out->push_back({"SR_ModSuitSaved", {ModMenuArg::Str(a0), ModMenuArg::Int(ok ? 1 : 0), ModMenuArg::Int(on ? 1 : 0)}});
      pack = a0.substr(0, a0.find('/'));
    }
    const auto choices = modCostumePackChoices(pack);
    out->push_back({"SR_ModSuitBegin", {ModMenuArg::Str(pack), ModMenuArg::Int(static_cast<int32_t>(choices.size())),
                                        ModMenuArg::Int(modMenuRestartRequired() ? 1 : 0)}});
    for (size_t i = 0; i < choices.size(); ++i) {
      const auto& choice = choices[i];
      const int32_t flags = (choice.included ? 1 : 0) | (choice.active ? 2 : 0);
      out->push_back({"SR_ModSuit", {ModMenuArg::Int(static_cast<int32_t>(i)), ModMenuArg::Str(choice.costume.id),
                                     ModMenuArg::Int(flags)}});
      out->push_back({"SR_ModSuitText", {ModMenuArg::Int(static_cast<int32_t>(i)), ModMenuArg::Str("name"),
                                         ModMenuArg::Str(choice.costume.name)}});
    }
    out->push_back({"SR_ModSuitEnd", {ModMenuArg::Int(static_cast<int32_t>(choices.size()))}});
  } else if (cmd == "SR_ModSet") {
    const bool ok = (a1 == "0" || a1 == "1") && modMenuSetEnabled(a0, a1 == "1");
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MOD] menu: set %s %s -> %s", a0.c_str(), a1.c_str(), ok ? "saved" : "refused");
    list();
  } else if (cmd == "SR_ModMove") {
    const bool ok = (a1 == "-1" || a1 == "1") && modMenuMove(a0, std::atoi(a1.c_str()));
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MOD] menu: move %s %s -> %s", a0.c_str(), a1.c_str(), ok ? "saved" : "refused");
    list();
  } else if (cmd == "SR_ModRestart") {
    std::string why;
    const bool ok = modMenuRestart(&why);
    out->push_back({"SR_ModRestarting", {ModMenuArg::Int(ok ? 1 : 0)}});
    if (!ok) text(-1, "restart", why);
  } else if (cmd == "SR_ModFeatures") {
    const auto feats = modMenuFeatures();
    for (size_t j = 0; j < feats.size(); ++j) {
      const PortFeatureInfo& f = feats[j];
      const auto it = mgr().env.options.find(f.option);
      const bool optOn = f.option.empty() || it == mgr().env.options.end() || it->second == 1;
      const int32_t flags = (f.applied ? 1 : 0) | (f.option.empty() ? 0 : 2) | (optOn ? 4 : 0) |
                            (static_cast<int32_t>(f.status) << 8);
      const int idx = -1 - static_cast<int>(j);
      out->push_back({"SR_ModFeature", {ModMenuArg::Int(static_cast<int32_t>(j)), ModMenuArg::Str(f.id), ModMenuArg::Int(flags)}});
      text(idx, "name", f.name);
      text(idx, "description", f.description);
      text(idx, "reason", f.reason);
    }
    out->push_back({"SR_ModEnd", {ModMenuArg::Int(static_cast<int32_t>(feats.size())), ModMenuArg::Int(0)}});
  } else {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[MOD] menu: unknown command %s", cmd.c_str());
  }
  return true;
}

}  // namespace kernel
}  // namespace sr
