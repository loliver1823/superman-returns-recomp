// iter-463: runtime shader translation (gfx/shader_xlat_core, gfx/xenos_bridge, XenosRecomp
// linked in, gfx/shader_runtime_xlat + gfx/shader_cache admission). Standalone: no guest, no D3D.
//
// Build + run + mutation check: python tools/difftest/shader_xlat_test_run.py (it compiles this
// file with the XenosRecomp clone sources exactly as cmake/xenos_xlat.cmake does, copies the DXC
// DLLs next to the exe, runs the offline XenosRecomp.exe for T5, then re-runs every mutant).
//
// Checks
//   T1  EXACT == offline: the 16 iter-463 spliced containers and the 3 translator controls,
//       translated in-process, are byte-identical to the installed data/shader_cache/dxil files
//       (.vs.dxil, .vs.vfetch, .vs.vsf.dxil) that the offline XenosRecomp.exe produced from them
//   T2  PROVED == stage_missing_vs.py: each runtime ucode of the 16 (the .vs.bin misses) against
//       the stage corpus -> status OK, path "proved", files byte-identical to the installed cache,
//       .vs.mkey identical to the staged one, and the checks line (vsf multipliers, cb rows, hlsl
//       diff size) equal to the Python manifest's. DBE06113 is the named case (stride 7 -> 9).
//   T3  refusals (each must NOT admit): proof mutants (ALU dword, fetch_const, non-executed dword),
//       a stride variant (must translate, with the NEW stride in the vsf DXIL), a tampered
//       installed reference (retranslation control), a template missing a vertex element, two
//       metadata groups, an exact PS and its two-metadata twin, no template at all
//   T4  the game path: ShaderCache + runtime module - WAIT request admits at once, async request
//       admits after waitIdle, files persisted, a fresh cache admits them at load with no job, the
//       installed cache wins a clash, and (--expect-off, SR_SHADER_RUNTIME_XLAT=0) nothing happens
//   T5  parity (Proof D, --parity-in/--parity-out): every container the offline tool translated
//       from a corpus translates in-process to byte-identical .dxil/.vfetch/.vsf.dxil (and a pixel
//       shader's .ps.at.dxil alpha-test variant, step 4 lane C 0005 / 0006)
//   T8  the alpha-test variant's order (step 4 lane C round 4, gfx/alpha_test.hpp use()): a cached PS
//       without its variant gets one from the runtime translator at IM_LOAD (WAIT), byte-identical to
//       the offline .ps.at.dxil, persisted and admitted by the next boot with no job; a translation
//       that does not reproduce the cached PS is refused and its draw drops ([SHADER_DROP] census);
//       the PS's own variant wins; a persisted variant for another plain translation and a PS entry
//       missing its named variant are skipped at load
//
// Exit code = failed checks; prints "N passed, M failed".
#include "gfx/shader_cache.hpp"
#include "gfx/shader_mkey.hpp"
#include "gfx/shader_runtime_xlat.hpp"
#include "gfx/shader_xlat_core.hpp"
#include "app/log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#define XXH_INLINE_ALL
#include "gfx/thirdparty/xxhash.h"

namespace G = sr::gfx;
namespace X = sr::gfx::xlat;
namespace fs = std::filesystem;

// UTF-8 <-> path in both C++17 and C++20 (u8string() returns std::u8string in C++20).
inline std::string u8s(const std::filesystem::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}
inline std::filesystem::path u8p(const std::string& s) {
#if __cplusplus >= 202002L
  return std::filesystem::path(std::u8string(s.begin(), s.end()));
#else
  return std::filesystem::u8path(s);
#endif
}

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
  if (ok) {
    g_pass++;
    std::printf("  PASS %s\n", what.c_str());
  } else {
    g_fail++;
    std::printf("  FAIL %s %s\n", what.c_str(), detail.c_str());
  }
  std::fflush(stdout);
}

static std::vector<uint8_t> readAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static std::string readText(const fs::path& p) {
  auto b = readAll(p);
  return std::string(b.begin(), b.end());
}
static std::string hx(uint64_t v) {
  char b[20];
  std::snprintf(b, sizeof(b), "%016" PRIx64, v);
  return b;
}
static uint64_t unhex(const std::string& s) { return std::strtoull(s.c_str(), nullptr, 16); }
static uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
static void setBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

static std::vector<std::string> tailLines() {
  std::vector<char> buf(size_t(sr::Log::kMaxTailLines) * 4300, 0);
  sr::Log::getTailLines(buf.data(), int(buf.size()));
  std::vector<std::string> out;
  std::istringstream is(std::string(buf.data()));
  std::string l;
  while (std::getline(is, l)) out.push_back(l);
  return out;
}
static int countLines(const std::string& a, const std::string& b = "") {
  int n = 0;
  for (const auto& l : tailLines())
    if (l.find(a) != std::string::npos && (b.empty() || l.find(b) != std::string::npos)) n++;
  return n;
}

// The installed cache as the translator's InstalledLookup (VS: dxil, vsf, sidecar text).
static X::InstalledLookup lookupDir(const fs::path& dir, std::set<uint64_t> hide = {}) {
  return [dir, hide](uint64_t h, bool isVs, X::InstalledFiles* out) {
    if (hide.count(h)) return false;
    const std::string stem = hx(h) + (isVs ? ".vs" : ".ps");
    if (!fs::exists(dir / (stem + ".dxil"))) return false;
    if (out) {
      out->dxil = readAll(dir / (stem + ".dxil"));
      out->vsf = readAll(dir / (stem + ".vsf.dxil"));
      out->sidecar = readText(dir / (stem + ".vfetch"));
    }
    return true;
  };
}

static bool sameFiles(const X::Files& f, const fs::path& dir, uint64_t h, std::string* what) {
  const std::string stem = hx(h) + (f.isVs ? ".vs" : ".ps");
  bool ok = true;
  auto cmp = [&](const char* ext, const std::vector<uint8_t>& got) {
    const fs::path p = dir / (stem + ext);
    const bool exists = fs::exists(p);
    const bool eq = exists ? readAll(p) == got : got.empty();
    if (!eq) {
      ok = false;
      if (what) *what += std::string(" ") + ext + (exists ? " differs" : " missing offline");
    }
  };
  cmp(".dxil", f.dxil);
  if (f.isVs) {
    cmp(".vfetch", std::vector<uint8_t>(f.sidecar.begin(), f.sidecar.end()));
    cmp(".vsf.dxil", f.vsf);
  } else {
    cmp(".at.dxil", f.at);  // step 4 lane C (0005): the alpha-test variant
  }
  return ok;
}

// tools/XenosRecomp-clone main.cpp's corpus scan: every file (recursive), the magic at 4-byte
// steps, its acceptance envelope, dedupe by XXH3 of the container (std::map = its order).
static std::map<uint64_t, std::vector<uint8_t>> scanLikeOffline(const fs::path& dir) {
  std::map<uint64_t, std::vector<uint8_t>> containers;
  std::error_code ec;
  for (auto& e : fs::recursive_directory_iterator(dir, ec)) {
    if (e.is_directory()) continue;
    auto b = readAll(e.path());
    for (size_t i = 0; b.size() > 36 && i < b.size() - 36 - 1;) {
      const uint8_t* p = b.data() + i;
      const uint32_t flags = be32(p), virt = be32(p + 4), phys = be32(p + 8);
      const uint64_t size = uint64_t(virt) + phys;
      if ((flags & 0xFFFFFF00u) == 0x102A1100u && size <= b.size() - i && virt >= 36 && virt < 0x20000 &&
          phys > 0 && phys < 0x20000) {
        containers.emplace(XXH3_64bits(p, size), std::vector<uint8_t>(p, p + size));
        i += size;
      } else {
        i += 4;
      }
    }
  }
  return containers;
}

static bool loadContainerFile(const fs::path& p, X::Container& c) {
  auto b = readAll(p);
  std::string why;
  return X::parseContainer(b.data(), b.size(), p.generic_string(), c, &why);
}

// Python manifest.txt: per shader "   checks: ..." line.
static std::map<std::string, std::string> pyChecks(const fs::path& manifest) {
  std::map<std::string, std::string> out;
  std::ifstream f(manifest);
  std::string line, cur;
  while (std::getline(f, line)) {
    if (line.size() > 17 && line[16] == ' ' && std::isxdigit(uint8_t(line[0]))) cur = line.substr(0, 16);
    else if (line.rfind("   checks: ", 0) == 0 && !cur.empty()) out[cur] = line.substr(11);
  }
  return out;
}
static std::string field(const std::string& s, const std::string& from, const std::string& to) {
  const size_t a = s.find(from);
  if (a == std::string::npos) return "";
  const size_t b = s.find(to, a + from.size());
  return s.substr(a, (b == std::string::npos ? s.size() : b) - a);
}

int main(int argc, char** argv) {
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
  std::map<std::string, std::string> a;
  bool expectOff = false;
  bool onlyParity = false;
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--expect-off") == 0) expectOff = true;
    else if (std::strcmp(argv[i], "--only-parity") == 0) onlyParity = true;
    else if (i + 1 < argc) {
      a[argv[i]] = argv[i + 1];
      i++;
    }
  }
  const fs::path cache = a.count("--cache") ? u8p(a["--cache"]) : fs::path("data/shader_cache/dxil");
  const fs::path stage = a.count("--stage") ? u8p(a["--stage"]) : fs::path("archive/v1_arena/stage/missing_vs");
  const fs::path misses =
      a.count("--misses") ? u8p(a["--misses"]) : fs::path("archive/v1_arena/shaders_bindings/misses");
  const std::string corpus = a.count("--corpus") ? a["--corpus"] : (stage / "corpus.txt").generic_string();
  const fs::path work = a.count("--work") ? u8p(a["--work"]) : fs::path("archive/shader_xlat_test_work");
  const fs::path dxcDir = a.count("--dxc") ? u8p(a["--dxc"]) : fs::current_path();
  std::error_code ec;
  fs::create_directories(work, ec);

  // --split-corpus <in> --split-out <dir>: the containers the offline tool would read, split into
  // <dir>/ok (parse + preflight pass: the in-process translator may run them) and
  // <dir>/refused.txt. The runner feeds ok/ to BOTH the offline exe and T5, so the offline batch
  // never meets a container that crashes its recompiler (after a caught crash its later DXC
  // compiles fail too - iter-463 parity run) and the comparison is container for container.
  if (a.count("--split-corpus") && a.count("--split-out")) {
    const fs::path out = u8p(a["--split-out"]);
    fs::create_directories(out / "ok", ec);
    std::ofstream ref(out / "refused.txt");
    size_t nOk = 0, nInvalid = 0, nPre = 0;
    for (auto& [ch, bytes] : scanLikeOffline(u8p(a["--split-corpus"]))) {
      X::Container c;
      std::string w;
      if (!X::parseContainer(bytes.data(), bytes.size(), hx(ch), c, &w)) {
        nInvalid++;
        ref << hx(ch) << " invalid " << w << "\n";
        continue;
      }
      if (!X::preflightContainer(c, &w)) {
        nPre++;
        ref << hx(ch) << " " << (c.isVs ? "VS " : "PS ") << hx(c.ucodeHash) << " " << w << "\n";
        continue;
      }
      std::ofstream(out / "ok" / (hx(ch) + ".xpu"), std::ios::binary)
          .write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
      nOk++;
    }
    std::printf("split: %zu ok, %zu fail validation, %zu fail the preflight\n", nOk, nInvalid, nPre);
    return 0;
  }

  std::string why;
  if (!X::dxcLoad(fs::absolute(dxcDir).wstring(), &why)) {
    std::printf("cannot load DXC from %s: %s\n", u8s(dxcDir).c_str(), why.c_str());
    return 99;
  }
  std::printf("dxc %s (dlls %016" PRIx64 ")\n", X::dxcVersion().c_str(), X::dxcDllHash());
  X::Dxc dxc;
  if (!dxc.ok()) {
    std::printf("no DXC compiler instance\n");
    return 99;
  }

  // --probe <dir of <hash>.{vs,ps}.bin> --probe-corpus <dir;dir;...> [--probe-ref <cache dir>]
  // [--probe-out <dir>] (iter-467 release/installer): translate every runtime ucode a roll dumped
  // (SR_SHADER_DUMP_MISSES=1) against the given container corpora, exactly as the game's runtime
  // translator would (no installed cache), and compare each result with a reference cache (the
  // AOT data/shader_cache/dxil that renders correctly). A tool, not a check: always exit 0.
  if (a.count("--probe")) {
    X::ContainerIndex ix;
    const std::string list = a.count("--probe-corpus") ? a["--probe-corpus"] : "";
    for (size_t p0 = 0; p0 <= list.size();) {
      size_t p1 = list.find(';', p0);
      if (p1 == std::string::npos) p1 = list.size();
      if (p1 > p0) {
        const X::ScanStats st = X::scanContainerDir(list.substr(p0, p1 - p0), ix);
        std::printf("corpus %s: %zu files, %zu added, %zu invalid\n", list.substr(p0, p1 - p0).c_str(), st.files,
                    st.added, st.invalid);
      }
      p0 = p1 + 1;
    }
    const fs::path ref = a.count("--probe-ref") ? u8p(a["--probe-ref"]) : fs::path();
    const fs::path pout = a.count("--probe-out") ? u8p(a["--probe-out"]) : fs::path();
    if (!pout.empty()) fs::create_directories(pout, ec);
    const X::InstalledLookup none = [](uint64_t, bool, X::InstalledFiles*) { return false; };
    std::vector<fs::path> ins;
    for (auto& e : fs::directory_iterator(u8p(a["--probe"]), ec))
      if (e.path().extension() == ".bin") ins.push_back(e.path());
    std::sort(ins.begin(), ins.end());
    std::map<std::string, int> tally;
    for (const auto& p : ins) {
      const std::string n = u8s(p.filename());  // <16 hex>.<vs|ps>.bin
      if (n.size() < 23) continue;
      const bool isVs = n.compare(17, 2, "vs") == 0;
      const uint64_t h = unhex(n.substr(0, 16));
      const auto u = readAll(p);
      const auto t0 = std::chrono::steady_clock::now();
      X::Result r = X::translate(h, isVs, u.data(), u.size(), ix, dxc, none);
      const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0).count();
      std::string cmp;
      if (r.status == X::Status::Ok && !ref.empty()) {
        const std::string stem = hx(h) + (isVs ? ".vs" : ".ps");
        const auto rd = readAll(ref / (stem + ".dxil"));
        if (rd.empty()) cmp = " | ref: none";
        else {
          const bool d = rd == r.files.dxil;
          std::string sc;
          if (isVs) {
            const auto rs = readAll(ref / (stem + ".vfetch"));
            const auto rv = readAll(ref / (stem + ".vsf.dxil"));
            std::string a1(rs.begin(), rs.end()), b1 = r.files.sidecar;
            a1.erase(std::remove(a1.begin(), a1.end(), '\r'), a1.end());
            sc = std::string(" vfetch ") + (a1 == b1 ? "same" : "DIFF") + " vsf " +
                 (rv.empty() && r.files.vsf.empty() ? "none" : (rv == r.files.vsf ? "same" : "DIFF"));
          }
          cmp = std::string(" | ref: dxil ") + (d ? "same" : "DIFF") + sc;
          tally[d ? "ref-dxil-same" : "ref-dxil-diff"]++;
        }
      }
      tally[std::string(isVs ? "VS " : "PS ") + X::statusName(r.status)]++;
      std::printf("%s %s %-11s %-7s %5lld ms%s%s%s\n", hx(h).c_str(), isVs ? "VS" : "PS", X::statusName(r.status),
                  r.path.c_str(), ms, cmp.c_str(), r.status == X::Status::Ok ? "" : " | ", r.reason.c_str());
      if (!pout.empty()) {
        const std::string stem = hx(h) + (isVs ? ".vs" : ".ps");
        std::ofstream(pout / (stem + ".manifest.txt")) << r.manifest << (r.reason.empty() ? "" : "reason ") << r.reason
                                                       << "\n";
        if (!r.files.dxil.empty()) {  // refused PROVED results keep their files: the evidence
          std::ofstream(pout / (stem + ".dxil"), std::ios::binary)
              .write(reinterpret_cast<const char*>(r.files.dxil.data()), std::streamsize(r.files.dxil.size()));
          std::ofstream(pout / (stem + ".hlsl")) << r.files.hlsl;
          if (isVs) std::ofstream(pout / (stem + ".vfetch")) << r.files.sidecar;
        }
        if (r.files.vsf.size()) {
          std::ofstream(pout / (stem + ".vsf.dxil"), std::ios::binary)
              .write(reinterpret_cast<const char*>(r.files.vsf.data()), std::streamsize(r.files.vsf.size()));
          std::string dis;
          if (dxc.disassemble(r.files.vsf, dis)) std::ofstream(pout / (stem + ".vsf.txt")) << dis;
        }
      }
    }
    for (const auto& [k, v] : tally) std::printf("  %-20s %d\n", k.c_str(), v);
    return 0;
  }

  // The 16 staged shaders (the installed iter-463 VS) and the translator controls.
  std::vector<uint64_t> staged;
  for (auto& e : fs::directory_iterator(stage / "containers", ec)) {
    const std::string n = u8s(e.path().filename());  // vs_<h>_masked.xpu
    if (n.size() > 20 && n.rfind("vs_", 0) == 0) staged.push_back(unhex(n.substr(3, 16)));
  }
  std::sort(staged.begin(), staged.end());
  const uint64_t kDbe = 0xdbe06113c69da55bull;

  if (!expectOff && !onlyParity) {
    // ── T1 ──
    std::printf("T1 EXACT translation == the offline files it produced (installed cache)\n");
    {
      int n = 0, same = 0;
      std::string bad;
      std::vector<fs::path> files;
      for (uint64_t h : staged) files.push_back(stage / "containers" / ("vs_" + hx(h) + "_masked.xpu"));
      for (auto& e : fs::directory_iterator(stage / "control" / "in", ec)) files.push_back(e.path());
      for (const auto& p : files) {
        X::Container c;
        if (!loadContainerFile(p, c)) {
          bad += " " + u8s(p.filename()) + "(invalid)";
          continue;
        }
        X::Files f;
        std::string w;
        n++;
        if (!X::translateContainer(c, dxc, f, &w)) {
          bad += " " + hx(c.ucodeHash) + "(" + w + ")";
          continue;
        }
        std::string d;
        if (sameFiles(f, cache, c.ucodeHash, &d)) same++;
        else bad += " " + hx(c.ucodeHash) + ":" + d;
      }
      std::printf("    %d containers, %d byte-identical on .vs.dxil/.vs.vfetch/.vs.vsf.dxil%s\n", n, same,
                  bad.empty() ? "" : (" | " + bad).c_str());
      check(n >= 16 && same == n, "every staged + control container translates byte-identical to the offline cache", bad);
    }

    // ── T2 ──
    std::printf("T2 PROVED translation of the runtime ucode == stage_missing_vs.py + offline XenosRecomp\n");
    X::ContainerIndex index;
    {
      const auto t0 = std::chrono::steady_clock::now();
      const X::ScanStats st = X::scanContainerDir(corpus, index);
      std::printf("    corpus %s: %zu files, %zu containers (%zu VS), %zu invalid, %lld ms\n", corpus.c_str(), st.files,
                  index.size(), index.vsCount(), st.invalid,
                  (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
                      .count());
    }
    const auto py = pyChecks(stage / "manifest.txt");
    const auto inst = lookupDir(cache);
    int nOk = 0, nSame = 0, nChecks = 0;
    std::string bad;
    std::string dbeManifest;
    for (uint64_t h : staged) {
      const auto u = readAll(misses / (hx(h) + ".vs.bin"));
      const auto t0 = std::chrono::steady_clock::now();
      X::Result r = X::translate(h, true, u.data(), u.size(), index, dxc, inst);
      const long long ms =
          (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      if (h == kDbe) dbeManifest = r.manifest;
      if (r.status != X::Status::Ok || r.path != "proved") {
        bad += " " + hx(h) + "(" + X::statusName(r.status) + " " + r.path + ": " + r.reason + ")";
        continue;
      }
      nOk++;
      std::string d;
      const bool same = sameFiles(r.files, cache, h, &d);
      const std::string mk = readText(stage / "dxil" / (hx(h) + ".vs.mkey"));
      const bool mkSame = mk == hx(r.files.mkey) + "\n" || mk == hx(r.files.mkey) + "\r\n";  // py text mode
      if (same && mkSame) nSame++;
      else bad += " " + hx(h) + ":" + d + (mkSame ? "" : " mkey");
      // The checks line against Python's (vsf multipliers + expected, cb rows, hlsl diff size).
      const std::string mine = field(r.manifest, "checks: ", "\n");
      auto it = py.find(hx(h));
      bool eq = it != py.end();
      for (const char* k : {"vsf mul ", "cb rows ", "hlsl diff "}) {
        const std::string to = std::string(k) == "hlsl diff " ? " lines" : " |";
        if (eq && field(mine, k, to) != field(it->second, k, to)) {
          eq = false;
          bad += " " + hx(h) + ":checks[" + field(mine, k, to) + " vs py " + field(it->second, k, to) + "]";
        }
      }
      nChecks += eq;
      std::printf("    %016" PRIx64 " OK %lld ms  %s\n", h, ms, mine.c_str());
    }
    std::printf("    DBE06113 manifest:\n%s", dbeManifest.c_str());
    check(nOk == int(staged.size()) && nOk == 16, "all 16 runtime ucodes are PROVED and translated", bad);
    check(nSame == 16, "their .vs.dxil/.vs.vfetch/.vs.vsf.dxil/.vs.mkey are byte-identical to the offline stage", bad);
    check(nChecks == 16, "the post-translation check values equal stage_missing_vs.py's manifest");
    // iter-467: with the fetch constant a patch field, DBE06113 proves against the game's real
    // compiled container (stride 0, everything from constant 95) - the patch it names is the whole
    // bind-time layout, including the second stream's move to fetch constant 94.
    check(dbeManifest.find("stride 0->9") != std::string::npos &&
              dbeManifest.find("fetch_const 95->94") != std::string::npos,
          "DBE06113's proof names its bind-time patch (stride 0->9, fetch_const 95->94)");

    // ── T3 ──
    std::printf("T3 refusals and variants\n");
    {
      const auto dbe = readAll(misses / (hx(kDbe) + ".vs.bin"));
      std::vector<uint8_t> key;
      X::patchMaskedUcode(dbe.data(), dbe.size(), key);
      std::vector<const X::Container*> same;
      index.sameProgram(key, same);
      check(!same.empty(), "DBE06113 has same-program containers in the corpus");
      const X::Container* tmpl = nullptr;
      for (auto* c : same)
        if (c->ucodeHash == 0x577613a06d355e33ull) tmpl = c;
      check(tmpl != nullptr, "the stage's reference 577613A0 is among them");
      if (tmpl) {
        const X::ProofReport p0 = X::proveSameProgram(tmpl->ucode(), dbe.data(), dbe.size());
        check(p0.ok && p0.diffs.size() == 4, "proof: DBE06113 vs 577613A0 holds with 4 patched vfetches");
        const X::CfWalk w = X::cfExecWalk(dbe.data(), dbe.size());
        uint32_t aluSlot = 0xFFFFFFFFu, vfSlot = 0xFFFFFFFFu;
        std::vector<bool> covered(dbe.size() / 4, false);
        for (uint32_t i = 0; i < w.bound * 3; i++) covered[i] = true;
        for (const auto& s : w.slots) {
          for (int k = 0; k < 3; k++) covered[s.slot * 3 + k] = true;
          if (!s.isFetch && aluSlot == 0xFFFFFFFFu) aluSlot = s.slot;
          if (s.isFetch && (be32(dbe.data() + s.slot * 12) & 0x1Fu) == 0 && vfSlot == 0xFFFFFFFFu) vfSlot = s.slot;
        }
        auto mutated = [&](size_t dw, uint32_t xorv) {
          std::vector<uint8_t> m = dbe;
          setBe32(m.data() + dw * 4, be32(m.data() + dw * 4) ^ xorv);
          return m;
        };
        {
          auto m = mutated(size_t(aluSlot) * 3 + 1, 0x00010000u);
          const X::ProofReport p = X::proveSameProgram(tmpl->ucode(), m.data(), m.size());
          check(!p.ok && !p.problems.empty() && p.problems[0].find("ALU instruction") != std::string::npos,
                "proof refuses an ALU change", p.problems.empty() ? "" : p.problems[0]);
          X::Result r = X::translate(XXH3_64bits(m.data(), m.size()), true, m.data(), m.size(), index, dxc, inst);
          check(r.status != X::Status::Ok, "translate does not admit it", X::statusName(r.status));
        }
        {
          // iter-467: the fetch constant is a bind-time patch field (D3D gives each stream its own
          // vertex fetch constant: 95, 94, ...). Proved, and named in the report.
          auto m = mutated(size_t(vfSlot) * 3, 1u << 20);  // fetch_const (dword 0 bits 20-24)
          const X::ProofReport p = X::proveSameProgram(tmpl->ucode(), m.data(), m.size());
          bool named = false;
          for (const auto& d : p.diffs) named |= d.fields.find("fetch_const") != std::string::npos;
          check(p.ok && named, "proof accepts a fetch-constant change as a bind-time patch and names it");
        }
        {
          // T6 (iter-467) recipes: DBE06113 as a recipe on its template - per executed vertex fetch
          // the template fetch it comes from and its patch-field bits - rebuilds it byte for byte.
          const X::ProofReport p = X::proveSameProgram(tmpl->ucode(), dbe.data(), dbe.size());
          std::map<uint32_t, uint32_t> from;
          for (const auto& d : p.diffs) {
            const size_t at = d.fields.find("from i");
            if (at != std::string::npos) from[d.instr] = uint32_t(std::stoul(d.fields.substr(at + 6)));
          }
          std::vector<uint32_t> vsl;
          for (const auto& s : X::cfExecWalk(dbe.data(), dbe.size()).slots)
            if (s.isFetch && (be32(dbe.data() + s.slot * 12) & 0x1Fu) == 0 &&
                std::find(vsl.begin(), vsl.end(), s.slot) == vsl.end())
              vsl.push_back(s.slot);
          std::string line = "patch " + hx(kDbe) + " " + hx(tmpl->ucodeHash) + " " + std::to_string(dbe.size()) + " disc";
          for (uint32_t s : vsl) {
            char e[64];
            const uint8_t* q = dbe.data() + s * 12;
            std::snprintf(e, sizeof e, " %u:%u:%08x:%08x:%08x", s, from.count(s) ? from[s] : s,
                          be32(q) & X::kPatchDw0, be32(q + 4) & X::kPatchDw1, be32(q + 8) & X::kPatchDw2);
            line += e;
          }
          size_t bad = 0;
          const auto rs = X::parseRecipes("# comment\n" + line + "\nxdk 72cbcaa6a7984111 60\npatch nonsense\n", &bad);
          check(rs.size() == 2 && bad == 1 && !rs[0].xdk && rs[1].xdk && rs[1].bytes == 60,
                "T6 recipes parse (one patch, one xdk; a malformed line is counted and skipped)");
          std::vector<uint8_t> out;
          std::string w;
          const bool ok = !rs.empty() && X::applyRecipe(tmpl->ucode(), tmpl->sz, rs[0], out, &w);
          check(ok && out == dbe, "T6 a recipe on its template rebuilds DBE06113 byte for byte", w);
          {
            // An identity recipe (no fetch: the game binds the template as created) parses and applies.
            const std::string id = "patch " + hx(tmpl->ucodeHash) + " " + hx(tmpl->ucodeHash) + " " +
                                   std::to_string(tmpl->sz) + " live\n";
            size_t bad2 = 0;
            const auto ri = X::parseRecipes(id, &bad2);
            std::vector<uint8_t> o2;
            check(ri.size() == 1 && bad2 == 0 && ri[0].fetches.empty() &&
                      X::applyRecipe(tmpl->ucode(), tmpl->sz, ri[0], o2, &w) &&
                      o2 == std::vector<uint8_t>(tmpl->ucode(), tmpl->ucode() + tmpl->sz),
                  "T6 an identity recipe (no fetch) parses and rebuilds its template", w);
          }
          if (!rs.empty()) {
            X::Recipe r1 = rs[0];
            r1.key ^= 1;
            check(!X::applyRecipe(tmpl->ucode(), tmpl->sz, r1, out, &w) && w.find("hash") != std::string::npos,
                  "T6 a recipe whose result misses its key is refused", w);
            X::Recipe r2 = rs[0];
            r2.fetches[0].b0 |= 0x1Fu;  // opcode bits: outside the patch fields
            check(!X::applyRecipe(tmpl->ucode(), tmpl->sz, r2, out, &w) && w.find("outside") != std::string::npos,
                  "T6 a recipe carrying bits outside the patch fields is refused", w);
            X::Recipe r3 = rs[0];
            r3.fetches[0].from = 0;  // control flow, not a vertex fetch
            check(!X::applyRecipe(tmpl->ucode(), tmpl->sz, r3, out, &w) && w.find("not a vertex fetch") != std::string::npos,
                  "T6 a recipe naming a slot that is not a vertex fetch of the run is refused", w);
          }
        }
        {
          // A destination register no fetch of the run writes: not a reorder, a different program.
          auto m = mutated(size_t(vfSlot) * 3, 0x20u << 12);  // dst_reg (dword 0 bits 12-17) += 32
          const X::ProofReport p = X::proveSameProgram(tmpl->ucode(), m.data(), m.size());
          bool named = false;
          for (const auto& s : p.problems) named |= s.find("unpatched bits differ") != std::string::npos;
          check(!p.ok && named, "proof refuses a vfetch destination-register change");
        }
        {
          // A reorder inside the fetch run (two adjacent vertex fetches swapped) pairs by content.
          // render/shader-coverage: two INDEPENDENT fetches of one exec clause (DBE06113's first pair, i21 / i22, is
          // not: i21 writes r0.x, the index i22 reads - Astra round 1's counterexample, refused below by T3g).
          const X::CfWalk w2 = X::cfExecWalk(dbe.data(), dbe.size());
          std::vector<uint32_t> vs;
          for (const auto& s : w2.slots)
            if (s.isFetch && (be32(dbe.data() + s.slot * 12) & 0x1Fu) == 0) vs.push_back(s.slot);
          std::sort(vs.begin(), vs.end());
          uint32_t sa = 0xFFFFFFFFu;
          for (const auto& run : X::vfetchRunsByClause(vs, w2))
            for (size_t i = 0; i + 1 < run.size() && sa == 0xFFFFFFFFu; i++)
              if (std::memcmp(dbe.data() + run[i] * 12, dbe.data() + run[i + 1] * 12, 12) != 0 &&
                  X::fetchOrderKeepsDependences(dbe.data(), dbe.size(), {run[i], run[i + 1]}, {1, 0}, nullptr))
                sa = run[i];
          if (sa != 0xFFFFFFFFu) {
            std::vector<uint8_t> m = dbe;
            std::swap_ranges(m.begin() + sa * 12, m.begin() + sa * 12 + 12, m.begin() + (sa + 1) * 12);
            const X::ProofReport p = X::proveSameProgram(tmpl->ucode(), m.data(), m.size());
            bool moved = false;
            for (const auto& d : p.diffs) moved |= d.fields.find("from i") != std::string::npos;
            check(p.ok && moved, "proof pairs a reorder inside a fetch run by content (names 'from i')",
                  p.problems.empty() ? "" : p.problems[0]);
            // ... and the index finds the template of the reordered program (the masked key sorts
            // each run), so the runtime translates it.
            X::Result rr = X::translate(XXH3_64bits(m.data(), m.size()), true, m.data(), m.size(), index, dxc, inst);
            check(rr.status == X::Status::Ok && rr.path == "proved",
                  "a reordered variant finds its template through the index and translates PROVED",
                  std::string(X::statusName(rr.status)) + " " + rr.reason);
          } else {
            check(false, "DBE06113 has two adjacent independent vertex fetches of one clause to swap");
          }
        }
        {
          // render/shader-coverage (Astra 2026-10-17, P1 on the reorder rule): a swap of two fetches that DEPEND on
          // each other is a different computation. The second fetch of an adjacent pair is made to read its index
          // from the register and component the first one writes (runtime dst swizzle) - in the template AND the
          // runtime, src fields are not patch fields - then swapped in the runtime only: refused. Unswapped it proves.
          const X::CfWalk w2 = X::cfExecWalk(dbe.data(), dbe.size());
          std::vector<uint32_t> vs;
          for (const auto& s : w2.slots)
            if (s.isFetch && (be32(dbe.data() + s.slot * 12) & 0x1Fu) == 0) vs.push_back(s.slot);
          std::sort(vs.begin(), vs.end());
          uint32_t sa = 0xFFFFFFFFu, comp = 4;
          for (size_t i = 0; i + 1 < vs.size() && sa == 0xFFFFFFFFu; i++) {
            if (vs[i + 1] != vs[i] + 1) continue;
            const uint32_t swz = be32(dbe.data() + vs[i] * 12 + 4) & 0xFFFu;
            for (uint32_t c = 0; c < 4 && comp == 4; c++)
              if (((swz >> (3 * c)) & 7u) != 7u) comp = c;
            if (comp < 4) sa = vs[i];
          }
          check(sa != 0xFFFFFFFFu, "T3g DBE06113 has two adjacent vertex fetches, the first writing a component");
          if (sa != 0xFFFFFFFFu) {
            std::vector<uint8_t> t2(tmpl->ucode(), tmpl->ucode() + tmpl->sz), u2 = dbe;
            const uint32_t dstA = (be32(u2.data() + sa * 12) >> 12) & 0x3Fu;
            for (std::vector<uint8_t>* b : {&t2, &u2}) {
              uint8_t* q = b->data() + (sa + 1) * 12;
              setBe32(q, (be32(q) & ~(0x3Fu << 5) & ~(3u << 30)) | (dstA << 5) | (comp << 30));  // src_reg, src_swiz
            }
            const X::ProofReport p1 = X::proveSameProgram(t2.data(), u2.data(), u2.size());
            check(p1.ok, "T3g a dependent fetch pair in program order still proves",
                  p1.problems.empty() ? "" : p1.problems[0]);
            std::swap_ranges(u2.begin() + sa * 12, u2.begin() + sa * 12 + 12, u2.begin() + (sa + 1) * 12);
            const X::ProofReport p2 = X::proveSameProgram(t2.data(), u2.data(), u2.size());
            bool named = false;
            for (const auto& s : p2.problems) named |= s.find("depend on each other") != std::string::npos;
            check(!p2.ok && named, "T3g a swap of two fetches that depend on each other is refused",
                  p2.problems.empty() ? "(proved)" : p2.problems[0]);
          }
          // Runs end at exec-clause boundaries; a slot two clauses execute ends its run.
          X::CfWalk fw;
          for (uint32_t s : {5u, 6u, 7u, 8u, 9u}) fw.slots.push_back({s < 7 ? 0u : 1u, 0u, s, true, false});
          fw.slots.push_back({2u, 0u, 9u, true, false});  // slot 9 is also executed by clause 2
          const auto runs = X::vfetchRunsByClause({5, 6, 7, 8, 9}, fw);
          check(runs.size() == 3 && runs[0] == std::vector<uint32_t>{5, 6} && runs[1] == std::vector<uint32_t>{7, 8} &&
                    runs[2] == std::vector<uint32_t>{9},
                "T3g fetch runs end at exec-clause boundaries (5 6 | 7 8 | 9)", std::to_string(runs.size()) + " runs");
        }
        {
          size_t dw = SIZE_MAX;
          for (size_t i = 0; i < covered.size(); i++)
            if (!covered[i]) dw = i;
          if (dw != SIZE_MAX) {
            auto m = mutated(dw, 0x1u);
            const X::ProofReport p = X::proveSameProgram(tmpl->ucode(), m.data(), m.size());
            check(!p.ok && p.problems.back().find("non-executed") != std::string::npos,
                  "proof refuses a non-executed dword change");
          } else {
            std::printf("    (no non-executed dword in DBE06113; that case is not exercised)\n");
          }
        }
        {
          // Stride variant: the first full vfetch's stride 9 -> 11 dwords. Same program: it must
          // translate, and the vsf DXIL must multiply the index by the NEW stride (44 bytes).
          std::vector<uint8_t> m = dbe;
          const size_t d2 = size_t(vfSlot) * 3 + 2;
          const uint32_t v = be32(m.data() + d2 * 4);
          setBe32(m.data() + d2 * 4, (v & ~0xFFu) | 11u);
          const uint64_t h2 = XXH3_64bits(m.data(), m.size());
          X::Result r = X::translate(h2, true, m.data(), m.size(), index, dxc, inst);
          check(r.status == X::Status::Ok, "a stride variant of DBE06113 is proved and translated",
                X::statusName(r.status) + std::string(" ") + r.reason);
          const std::string mul = field(r.manifest, "vsf mul", "|");
          check(mul.find("44") != std::string::npos && mul.find("True") != std::string::npos,
                "its vsf DXIL multiplies the index by the runtime stride (44 bytes)", mul);
          check(r.files.vsf != readAll(cache / (hx(kDbe) + ".vs.vsf.dxil")), "its vsf DXIL differs from DBE06113's");
          const auto rows = X::parseSidecarRows(r.files.sidecar);
          check(!rows.empty() && rows[0].stride == 11, "its sidecar carries stride 11");
        }
        {
          // Tampered installed reference: the retranslation control must refuse.
          auto tampered = [&](uint64_t h, bool isVs, X::InstalledFiles* out) {
            if (!inst(h, isVs, out)) return false;
            if (out && !out->vsf.empty()) out->vsf[out->vsf.size() / 2] ^= 0x40;
            return true;
          };
          X::Result r = X::translate(kDbe, true, dbe.data(), dbe.size(), index, dxc, tampered);
          check(r.status == X::Status::Refused && r.reason.find("reference-retranslation!=cache") != std::string::npos,
                "a reference that no longer matches the installed cache refuses", r.reason);
        }
        {
          // A template missing the vertex element of a writing fetch: refused before translation.
          X::Container t2 = *tmpl;
          uint8_t* p = t2.bytes.data();
          const uint32_t cnt = be32(p + t2.shOff + 0x1C);
          setBe32(p + t2.shOff + 0x1C, cnt - 1);  // drops the LAST element
          X::Container t2p;
          std::string w;
          const bool parsed = X::parseContainer(t2.bytes.data(), t2.bytes.size(), "mutant", t2p, &w);
          std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> el0, el1;
          X::vertexElements(*tmpl, el0);
          X::vertexElements(t2p, el1);
          X::ContainerIndex i2;
          i2.add(std::move(t2p));
          X::Result r = X::translate(kDbe, true, dbe.data(), dbe.size(), i2, dxc, inst);
          check(parsed && el1.size() + 1 == el0.size() && r.status == X::Status::Refused &&
                    r.reason.find("no vertex element") != std::string::npos,
                "a template without the vertex element of a written fetch refuses", r.reason);
        }
        {
          // Two metadata groups among the same-program containers (iter-467: THE AGREEMENT RULE on
          // the PROVED path). A difference the translation never reads is admitted - both
          // translations are byte-identical; a difference it does read is refused.
          X::Container t3 = *tmpl;
          t3.bytes[0x0F] ^= 0x01;  // header fieldC: metadata (outside the ucode), never read by XenosRecomp
          X::Container t3p, t1p;
          X::parseContainer(t3.bytes.data(), t3.bytes.size(), "meta-mutant", t3p, nullptr);
          X::parseContainer(tmpl->bytes.data(), tmpl->bytes.size(), tmpl->origin, t1p, nullptr);
          X::ContainerIndex i3;
          i3.add(std::move(t1p));
          i3.add(std::move(t3p));
          X::Result r = X::translate(kDbe, true, dbe.data(), dbe.size(), i3, dxc, inst);
          check(r.status == X::Status::Ok && r.manifest.find("2 metadata variants whose translations agree") !=
                                                 std::string::npos,
                "two metadata among same-program containers whose translations agree is admitted",
                r.reason + " " + r.manifest.substr(0, 400));
          // The first vertex element's usage index (the input semantic the .vs.dxil declares).
          X::Container t4 = *tmpl;
          uint8_t* q = t4.bytes.data();
          const uint32_t f18 = be32(q + t4.shOff + 0x18);
          const size_t e0 = t4.shOff + 0x24 + 4 * size_t(f18);
          setBe32(q + e0, be32(q + e0) ^ (1u << 19));  // usageIndex bit 3: 0..7 -> 8..15, no clash
          X::Container t4p, t5p;
          X::parseContainer(t4.bytes.data(), t4.bytes.size(), "meta-mutant-usage", t4p, nullptr);
          X::parseContainer(tmpl->bytes.data(), tmpl->bytes.size(), tmpl->origin, t5p, nullptr);
          X::ContainerIndex i4b;
          i4b.add(std::move(t5p));
          i4b.add(std::move(t4p));
          X::Result r4 = X::translate(kDbe, true, dbe.data(), dbe.size(), i4b, dxc, inst);
          check(r4.status == X::Status::Refused && r4.reason.find("2 different metadata") != std::string::npos,
                "two metadata among same-program containers whose translations differ refuses", r4.reason);
        }
        {
          // An installed .vs.vfetch checked out CRLF (core.autocrlf=true, the Git for Windows
          // default): the reference control compares sidecar TEXT, so it must still prove.
          auto crlf = [&](uint64_t h, bool isVs, X::InstalledFiles* out) {
            if (!inst(h, isVs, out)) return false;
            if (out) {
              std::string t;
              for (char ch : out->sidecar) {
                if (ch == '\n') t.push_back('\r');
                t.push_back(ch);
              }
              out->sidecar = t;
            }
            return true;
          };
          X::Result r = X::translate(kDbe, true, dbe.data(), dbe.size(), index, dxc, crlf);
          check(r.status == X::Status::Ok && r.path == "proved" &&
                    r.manifest.find("ref retranslation==cache True") != std::string::npos,
                "a CRLF installed sidecar still passes the reference control", r.reason);
        }
        {
          // EXACT VS whose same-program sibling carries other metadata: stage_missing_vs.py rule 2
          // (one metadata among ALL same-program containers) refuses, as on the PROVED path.
          X::Container ex = *tmpl;
          std::memcpy(ex.bytes.data() + ex.virt + ex.po, dbe.data(), dbe.size());
          X::Container exP, sibP;
          const bool p1 = X::parseContainer(ex.bytes.data(), ex.bytes.size(), "exact-dbe", exP, nullptr);
          X::Container sib = *tmpl;
          sib.bytes[0x0F] ^= 0x01;  // header fieldC: metadata
          const bool p2 = X::parseContainer(sib.bytes.data(), sib.bytes.size(), "sibling-meta-mutant", sibP, nullptr);
          X::ContainerIndex i4;
          i4.add(std::move(exP));
          X::Result r0 = X::translate(kDbe, true, dbe.data(), dbe.size(), i4, dxc, inst);
          check(p1 && p2 && r0.status == X::Status::Ok && r0.path == "exact",
                "control: the exact container alone translates EXACT", r0.reason);
          i4.add(std::move(sibP));
          X::Result r = X::translate(kDbe, true, dbe.data(), dbe.size(), i4, dxc, inst);
          check(r.status == X::Status::Refused && r.reason.find("same-program containers (exact path)") != std::string::npos,
                "an exact VS with a same-program sibling under other metadata refuses", r.reason);
        }
      }
      {
        // EXACT pixel shader from the corpus, then its two-metadata twin.
        X::ContainerIndex ip;
        X::scanContainerDir("data/shaders_menu", ip);
        const X::Container* ps = nullptr;
        for (auto& e : fs::directory_iterator("data/shaders_menu", ec)) {
          X::Container c;
          if (loadContainerFile(e.path(), c) && !c.isVs) {
            static X::Container keep;
            keep = c;
            ps = &keep;
            break;
          }
        }
        check(ps != nullptr, "data/shaders_menu has a pixel shader container");
        if (ps) {
          X::Result r = X::translate(ps->ucodeHash, false, ps->ucode(), ps->sz, ip, dxc, inst);
          X::Files f;
          X::translateContainer(*ps, dxc, f, nullptr);
          check(r.status == X::Status::Ok && r.path == "exact" && r.files.dxil == f.dxil,
                "a PS miss translates EXACT from its container", r.reason);
          // iter-467 release/installer (74ab5feb THE AGREEMENT RULE on the EXACT path): a metadata
          // difference XenosRecomp never reads (header fieldC) translates identically - admitted; a
          // difference it reads (the PS interpolator count) - refused.
          X::Container t = *ps;
          t.bytes[0x0F] ^= 0x01;  // header fieldC
          X::Container tp;
          X::parseContainer(t.bytes.data(), t.bytes.size(), "ps-meta-mutant", tp, nullptr);
          X::ContainerIndex ip2;
          {
            X::Container base;
            X::parseContainer(ps->bytes.data(), ps->bytes.size(), ps->origin, base, nullptr);
            ip2.add(std::move(base));
          }
          ip2.add(std::move(tp));
          X::Result r2 = X::translate(ps->ucodeHash, false, ps->ucode(), ps->sz, ip2, dxc, inst);
          check(r2.status == X::Status::Ok && r2.manifest.find("translations agree") != std::string::npos,
                "the same PS ucode under two metadata whose translations agree is admitted", r2.reason);
          // interpolatorInfo (shader header +0x14) bits 5-9 = the interpolator count the PS reads.
          X::Container t2 = *ps;
          uint8_t* q = t2.bytes.data() + t2.shOff + 0x14;
          const uint32_t ii = be32(q);
          const uint32_t cnt = (ii >> 5) & 0x1Fu;
          X::ContainerIndex ip3;
          {
            X::Container base;
            X::parseContainer(ps->bytes.data(), ps->bytes.size(), ps->origin, base, nullptr);
            ip3.add(std::move(base));
          }
          if (cnt > 0) {
            setBe32(q, (ii & ~(0x1Fu << 5)) | ((cnt - 1) << 5));  // one interpolator fewer
            X::Container t2p;
            X::parseContainer(t2.bytes.data(), t2.bytes.size(), "ps-meta-mutant-interp", t2p, nullptr);
            ip3.add(std::move(t2p));
            X::Result r3b = X::translate(ps->ucodeHash, false, ps->ucode(), ps->sz, ip3, dxc, inst);
            check(r3b.status == X::Status::Refused,
                  "the same PS ucode under two metadata whose translations differ refuses", r3b.reason);
          } else {
            check(false, "the probe PS reads at least one interpolator");
          }
        }
        {
          // Preflight on two carved garbage containers that took down the recompiler: 7FCA4485...
          // (a LoopEnd with no loop open: XenosRecomp's uint32 indentation wraps and it emits
          // ~4 billion tabs - a 4 GB .fail.hlsl offline) and 0142416A... (SEH "recompiler
          // crashed" offline, an access violation in-process before the preflight existed).
          // Checked WITHOUT running the recompiler on them.
          X::Container g1, g2;
          std::string w1, w2;
          const bool p1 = loadContainerFile("data/shaders_carved/ps_8ef0df9b55355819_40016B48.xpu", g1);
          const bool p2 = loadContainerFile("data/shaders_carved/ps_27e4526b24bd0a44_41C56768.xpu", g2);
          check(p1 && !X::preflightContainer(g1, &w1) && w1.find("LoopEnd") != std::string::npos,
                "preflight refuses the indentation-underflow container (7FCA4485)", w1);
          check(p2 && !X::preflightContainer(g2, &w2) && w2.find("past the ucode") != std::string::npos,
                "preflight refuses the offline-crash container (0142416A): exec past the ucode", w2);
        }
        {
          // iter-467: analyse_vsf reads a load offset dxc reassociated into the base mask
          // ((base + 36) & ~3, then + index * 40: 11B1E5F1's word-9 fetch, refused before).
          const char* dis =
              "  %51 = extractvalue %dx.types.CBufRet.i32 %21, 2\n"
              "  %52 = and i32 %51, -4\n"
              "  %55 = mul i32 %54, 40\n"
              "  %56 = add i32 %55, %52\n"
              "  %59 = call %dx.types.ResRet.i32 @dx.op.bufferLoad.i32(i32 68, %dx.types.Handle %1, i32 %56, i32 undef)\n"
              "  %79 = add i32 %56, 4\n"
              "  %80 = call %dx.types.ResRet.i32 @dx.op.bufferLoad.i32(i32 68, %dx.types.Handle %1, i32 %79, i32 undef)\n"
              "  %150 = add i32 %51, 36\n"
              "  %151 = and i32 %150, -4\n"
              "  %152 = add i32 %55, %151\n"
              "  %153 = call %dx.types.ResRet.i32 @dx.op.bufferLoad.i32(i32 68, %dx.types.Handle %1, i32 %152, i32 undef)\n";
          const X::VsfAnalysis va = X::analyseVsf(dis);
          const std::vector<int64_t> want{0, 4, 36};
          check(va.offs == want, "analyse_vsf reads the offset of a reassociated (base + 36) & ~3 load",
                "got " + std::to_string(va.offs.size()) + " offsets");
        }
        {
          // T6 (iter-467) the XDK's raw vertex shaders (no container exists): a constant-free container
          // around the ucode translates EXACT, and its sidecar carries the runtime layout of the AOT
          // entry (slot, offset, stride, format; the usage column only names the input). Fixture: a
          // roll's miss dumps (game bytes, local only); absent = not exercised, never a pass.
          const fs::path fx = a.count("--xdk-fixture") ? u8p(a["--xdk-fixture"])
                                                         : fs::path("archive/r2/m1-rc1-iso/shader_misses");
          for (const uint64_t h : {0x72cbcaa6a7984111ull, 0x0a6d1dd7767fdf27ull}) {
            const auto u = readAll(fx / (hx(h) + ".vs.bin"));
            if (u.empty()) {
              std::printf("    (T6 xdk %s: fixture %s absent - not exercised)\n", hx(h).c_str(), u8s(fx).c_str());
              continue;
            }
            X::Container c;
            std::string w;
            const bool made = X::synthesizeRawVsContainer(u.data(), u.size(), "xdk-test", c, &w);
            X::ContainerIndex ix;
            if (made) ix.add(X::Container(c));
            X::Result r = X::translate(h, true, u.data(), u.size(), ix, dxc, inst);
            auto layout = [](const std::string& sidecar) {
              std::string o;
              for (const auto& row : X::parseSidecarRows(sidecar))
                o += std::to_string(row.slot) + "," + std::to_string(row.offset) + "," + std::to_string(row.stride) +
                     "," + std::to_string(row.format) + ";";
              return o;
            };
            // The oracle is the ucode's own fetch layout (derive_fetches). The AOT entry is shown, not
            // trusted: its 72CBCAA6 sidecar reads fetch constant 95 while this ucode's only vertex fetch
            // names constant 0 (dword 6 = 00080000) - it was made from another container (iter-402).
            std::string own;
            for (const auto& row : X::deriveFetches(u.data(), u.size()))
              own += std::to_string(row.slot) + "," + std::to_string(row.offset) + "," + std::to_string(row.stride) +
                     "," + std::to_string(row.format) + ";";
            const auto ref = readAll(cache / (hx(h) + ".vs.vfetch"));
            const std::string refS(ref.begin(), ref.end());
            std::printf("    T6 xdk %s: sidecar [%s], AOT [%s]%s\n", hx(h).c_str(), layout(r.files.sidecar).c_str(),
                        layout(refS).c_str(), layout(refS) == layout(r.files.sidecar) ? "" : " (differs)");
            check(made && r.status == X::Status::Ok && r.path == "exact" && !own.empty() &&
                      own == layout(r.files.sidecar),
                  "T6 xdk raw VS " + hx(h) + " translates EXACT from a synthesized container (sidecar == the ucode's layout)",
                  std::string(made ? "made" : "not made: " + w) + " | " + X::statusName(r.status) + " " + r.path + " " +
                      r.reason + " | ours [" + layout(r.files.sidecar) + "] ucode [" + own + "]");
          }
        }
        {
          // T7 (iter-467 rc4 blockers) THE LITERAL RULE: PS 9EF298A6 is the xex's YUV->RGB shader twice,
          // BT.601 and BT.709 coefficients as float4 literals (c252..c255) - the ucode is the same, so
          // the GPU takes them from the constant file. The corpus copy (data/shaders_carved, the AOT's
          // container) and a twin with other literal values: admitted, reading c252..c255 live; a twin
          // that also differs in what XenosRecomp reads (one interpolator fewer): refused.
          X::Container yuv;
          const bool have = loadContainerFile("data/shaders_carved/ps_fdf2558830b8aaeb_8204F9C0.xpu", yuv);
          check(have && yuv.ucodeHash == 0x9ef298a63a9c23ddull, "T7 the corpus holds PS 9EF298A6's container");
          if (have) {
            X::Container s;
            std::string regs;
            check(X::stripFloatLiterals(yuv, s, &regs) && regs == "c252..c255" && s.ucodeHash == yuv.ucodeHash,
                  "T7 stripFloatLiterals removes PS 9EF298A6's float4 literals (c252..c255)", regs);
            X::Container twin = yuv;
            for (size_t i = yuv.virt + 0x24; i < yuv.virt + 0x2C; i++) twin.bytes[i] ^= 0x5A;  // c254 values
            X::Container twinP;
            X::parseContainer(twin.bytes.data(), twin.bytes.size(), "yuv-twin", twinP, nullptr);
            X::ContainerIndex iy;
            iy.add(X::Container(yuv));
            iy.add(X::Container(twinP));
            X::Result ry = X::translate(yuv.ucodeHash, false, yuv.ucode(), yuv.sz, iy, dxc, inst);
            check(ry.status == X::Status::Ok && ry.manifest.find("differ only in their float4 literal values (c252..c255)") !=
                                                    std::string::npos,
                  "T7 two metadata that differ only in float4 literal values: admitted, the literals read live",
                  ry.reason + " | " + ry.manifest);
            X::Files baked;
            X::translateContainer(yuv, dxc, baked, nullptr);
            check(ry.status == X::Status::Ok && ry.files.dxil != baked.dxil,
                  "T7 the literal-rule DXIL is not the baked one (it reads the registers)");
            X::ContainerIndex iy2;  // the other order: the same translation
            iy2.add(X::Container(twinP));
            iy2.add(X::Container(yuv));
            X::Result ry2 = X::translate(yuv.ucodeHash, false, yuv.ucode(), yuv.sz, iy2, dxc, inst);
            check(ry2.status == X::Status::Ok && ry2.files.dxil == ry.files.dxil,
                  "T7 the literal rule does not depend on which container comes first");
            X::Container both = twin;
            uint8_t* q = both.bytes.data() + both.shOff + 0x14;
            const uint32_t ii = be32(q), cnt = (ii >> 5) & 0x1Fu;
            if (cnt > 0) setBe32(q, (ii & ~(0x1Fu << 5)) | ((cnt - 1) << 5));
            X::Container bothP;
            X::parseContainer(both.bytes.data(), both.bytes.size(), "yuv-twin-interp", bothP, nullptr);
            X::ContainerIndex iy3;
            iy3.add(X::Container(yuv));
            iy3.add(X::Container(bothP));
            X::Result ry3 = X::translate(yuv.ucodeHash, false, yuv.ucode(), yuv.sz, iy3, dxc, inst);
            check(cnt > 0 && ry3.status == X::Status::Refused,
                  "T7 literal values AND an interpolator count differ: still refused", ry3.reason);
          }
          // T7 one input per semantic (XenosRecomp patch 0004): VS E95287FE (created by the game at boot)
          // declares POSITION0 and TEXCOORD0 at four fetch addresses each; one HLSL input per element
          // made dxc fail ("redefinition of parameter 'iPosition0'"). A corpus VS whose second vertex
          // element is rewritten to the first one's semantic must translate (both variants).
          const X::Container* two = nullptr;
          static X::Container keepVs;
          for (auto& e : fs::directory_iterator("data/shaders_menu", ec)) {
            X::Container c;
            if (!loadContainerFile(e.path(), c) || !c.isVs) continue;
            const uint8_t* p = c.bytes.data() + c.shOff;
            if (be32(p + 0x1C) >= 2) {
              keepVs = c;
              two = &keepVs;
              break;
            }
          }
          check(two != nullptr, "T7 data/shaders_menu has a VS with two vertex elements");
          if (two) {
            X::Container d = *two;
            uint8_t* p = d.bytes.data() + d.shOff;
            const uint32_t f18 = be32(p + 0x18);
            uint8_t* e0 = p + 0x24 + 4 * f18;
            const uint32_t v0 = be32(e0), v1 = be32(e0 + 4);
            setBe32(e0 + 4, (v1 & 0xFFFu) | (v0 & ~0xFFFu));  // element 1 takes element 0's usage/index
            X::Container dp;
            std::string w;
            const bool parsed = X::parseContainer(d.bytes.data(), d.bytes.size(), "dup-semantic", dp, &w);
            X::Files f;
            const bool ok = parsed && X::translateContainer(dp, dxc, f, &w);
            // render/shader-coverage: XenosRecomp patch 0004 (release/installer) is NOT in this tree's series,
            // so dxc still refuses this case; reported, not checked, until the patch is ported.
            std::printf("    (T7 one input per semantic: %s - XenosRecomp patch 0004 not in this series; not checked)\n",
                        ok && !f.dxil.empty() && !f.vsf.empty() ? "translates" : "refused");
          }
        }
        X::Result r3 = X::translate(0x0123456789abcdefull, false, nullptr, 0, ip, dxc, inst);
        check(r3.status == X::Status::Refused, "a ucode that does not hash to its key refuses");
        std::vector<uint8_t> junk(96, 0);
        X::Result r4 = X::translate(XXH3_64bits(junk.data(), junk.size()), false, junk.data(), junk.size(), ip, dxc, inst);
        check(r4.status == X::Status::NoTemplate, "a PS with no container waits for one (NO-TEMPLATE)");
      }
    }
  }

  // ── T4 ──
  if (!onlyParity) {
  std::printf("T4 game path: ShaderCache + runtime module%s\n", expectOff ? " (SR_SHADER_RUNTIME_XLAT=0)" : "");
  {
    // render/shader-coverage: hidden[3] and [4] are two more staged VS for the miss-policy checks (T4w).
    std::vector<uint64_t> hidden = {kDbe, 0xe866f0d333de566dull, 0xb8affb5ab5f94923ull};
    for (uint64_t h : staged)
      if (hidden.size() < 5 && std::find(hidden.begin(), hidden.end(), h) == hidden.end() &&
          fs::exists(misses / (hx(h) + ".vs.bin")))
        hidden.push_back(h);
    const fs::path part = work / "cache_minus3";
    fs::remove_all(part, ec);
    fs::create_directories(part, ec);
    for (auto& e : fs::directory_iterator(cache, ec)) {
      const std::string n = u8s(e.path().filename());
      bool hide = false;
      for (uint64_t h : hidden) hide |= n.rfind(hx(h), 0) == 0;
      if (!hide) fs::copy_file(e.path(), part / n, fs::copy_options::overwrite_existing, ec);
    }
    const fs::path rt = work / "rt";
    fs::remove_all(rt, ec);
    _putenv_s("SR_SHADER_CACHE_DIR", u8s(part).c_str());
    _putenv_s("SR_SHADER_RUNTIME_DIR", u8s(rt).c_str());
    _putenv_s("SR_SHADER_CONTAINER_DIRS", corpus.c_str());
    _putenv_s("SR_DXC_DIR", u8s(fs::absolute(dxcDir)).c_str());
    std::vector<std::vector<uint8_t>> u;
    for (uint64_t h : hidden) u.push_back(readAll(misses / (hx(h) + ".vs.bin")));

    G::ShaderCache c1;
    c1.ensureLoaded();
    check(!c1.findVs(kDbe) && c1.vsCount() > 100, "the partial cache lacks DBE06113");
    // render/shader-coverage: every miss-policy wait is a host wait the CP's observer sees (kWaitTranslation).
    static std::atomic<int> s_obsBegin{0}, s_obsEnd{0}, s_obsOther{0};
    static std::atomic<bool> s_obsDrain{false};  // T4w (f): the graceful quit's CP drain
    static const G::ShaderCache::WaitObserver kObs{
        [](int w) { (w == G::ShaderCache::kWaitTranslation ? s_obsBegin : s_obsOther)++; },
        [](int w) { (w == G::ShaderCache::kWaitTranslation ? s_obsEnd : s_obsOther)++; },
        [] { return s_obsDrain.load(); }};
    G::ShaderCache::setWaitObserver(&kObs);
    const G::rtxlat::WaitStats w0 = G::rtxlat::waitStats();
    G::rtxlat::setWaitModeForTest(1);
    c1.requestTranslation(kDbe, true, u[0].data(), u[0].size());
    const G::CachedShader* s = c1.findVs(kDbe);
    if (expectOff) {
      check(!s && G::rtxlat::statusForTest(kDbe, true) == "none", "off: a miss is not translated");
      check(countLines("[SHADER_XLAT] off (SR_SHADER_RUNTIME_XLAT=0)") == 1, "off: the boot line says so");
    } else {
      check(s && s->dxil == readAll(cache / (hx(kDbe) + ".vs.dxil")) &&
                s->dxilFetch == readAll(cache / (hx(kDbe) + ".vs.vsf.dxil")) && !s->layout.empty() &&
                !s->signature.empty(),
            "WAIT: the request admits DBE06113 before it returns, with the offline DXIL + vsf");
      // The worker admits first and persists after (the miss policy wakes the IM_LOAD at the admission).
      G::rtxlat::waitIdleForTest(60000);
      const fs::path d = u8p(G::rtxlat::runtimeDirForTest());
      bool files = true;
      for (const char* ext : {".vs.dxil", ".vs.vfetch", ".vs.vsf.dxil"})
        files &= readAll(d / (hx(kDbe) + ext)) == readAll(cache / (hx(kDbe) + ext));
      files &= fs::exists(d / (hx(kDbe) + ".vs.xlat")) && fs::exists(d / (hx(kDbe) + ".vs.mkey"));
      check(files, "persisted to " + u8s(d) + " byte-identical, with .xlat provenance and .mkey");
      check(d.parent_path() == rt && u8s(d.filename()).rfind("xr-", 0) == 0,
            "entries live under <SR_SHADER_RUNTIME_DIR>/xr-<translator id>/");

      // A write that fails (here: the .vs.vfetch name is taken by a directory, so the rename
      // fails) must leave NO .dxil behind - the key file the next boot admits by.
      fs::create_directories(d / (hx(hidden[2]) + ".vs.vfetch"), ec);
      G::rtxlat::setWaitModeForTest(0);
      c1.requestTranslation(hidden[1], true, u[1].data(), u[1].size());
      c1.requestTranslation(hidden[2], true, u[2].data(), u[2].size());
      for (int i = 0; i < 3; i++) c1.noteVsMissDraw(hidden[2], 0);
      const bool idle = G::rtxlat::waitIdleForTest(120000);
      check(idle && c1.findVs(hidden[1]) && c1.findVs(hidden[2]) &&
                G::rtxlat::statusForTest(hidden[2], true) == "admitted",
            "async: both requests are admitted once the worker is idle");
      check(countLines("[SHADER_XLAT] admitted VS B8AFFB5AB5F94923") == 1, "the admission is logged");
      c1.requestTranslation(hidden[2], true, u[2].data(), u[2].size());
      G::rtxlat::waitIdleForTest(10000);
      check(countLines("[SHADER_XLAT] request VS B8AFFB5AB5F94923") == 1, "a repeated miss is not re-queued");

      check(!fs::exists(d / (hx(hidden[2]) + ".vs.dxil")) &&
                countLines("[SHADER_XLAT] admitted VS B8AFFB5AB5F94923") == 1 &&
                countLines("; NOT persisted") == 1,
            "a failed .vfetch write persists no .dxil (and says NOT persisted); the entry is still admitted in memory");
      G::ShaderCache c2;
      c2.ensureLoaded();
      check(c2.runtimeCount() == 2 && c2.findVs(hidden[0]) && c2.findVs(hidden[1]) && !c2.findVs(hidden[2]),
            "a fresh cache admits the 2 persisted translations at load (no job), not the unpersisted one",
            std::to_string(c2.runtimeCount()));
      {
        // Incomplete / damaged entries are skipped at load (retranslated on the next miss).
        const std::vector<uint8_t> keep = readAll(d / (hx(hidden[1]) + ".vs.vfetch"));
        fs::remove(d / (hx(hidden[1]) + ".vs.vfetch"), ec);
        const std::vector<uint8_t> dx = readAll(d / (hx(hidden[0]) + ".vs.dxil"));
        {
          std::ofstream f(d / (hx(hidden[0]) + ".vs.dxil"), std::ios::binary);
          f.write(reinterpret_cast<const char*>(dx.data()), std::streamsize(dx.size() / 2));
        }
        G::ShaderCache c2b;
        c2b.ensureLoaded();
        check(c2b.runtimeCount() == 0 && countLines("persisted entry skipped (no .vfetch)") == 1 &&
                  countLines("persisted entry skipped (malformed .dxil)") == 1,
              "a VS entry without .vfetch and a truncated .dxil are skipped at load",
              std::to_string(c2b.runtimeCount()));
        {
          std::ofstream f(d / (hx(hidden[0]) + ".vs.dxil"), std::ios::binary);
          f.write(reinterpret_cast<const char*>(dx.data()), std::streamsize(dx.size()));
        }
        {
          std::ofstream f(d / (hx(hidden[1]) + ".vs.vfetch"), std::ios::binary);
          f.write(reinterpret_cast<const char*>(keep.data()), std::streamsize(keep.size()));
        }
      }
      _putenv_s("SR_SHADER_CACHE_DIR", u8s(cache).c_str());
      G::ShaderCache c3;
      c3.ensureLoaded();
      check(c3.runtimeCount() == 0 && c3.findVs(kDbe) == c3.findInstalledVs(kDbe),
            "with the full installed cache the installed entries win (0 admitted)");
      G::CachedShader extra;
      G::ShaderCache::loadEntryFromDir(d, kDbe, true, extra);
      check(!c3.admitRuntime(kDbe, true, std::move(extra)) && c3.runtimeCount() == 0,
            "admitRuntime refuses a hash the installed cache holds");

      // ── T4w: THE MISS POLICY (render/shader-coverage, 2026-10-17) ──
      std::printf("T4w the miss policy: an IM_LOAD miss waits (bounded) for its translation\n");
      check(hidden.size() == 5 && !c1.findVs(hidden[3]) && !c1.findVs(hidden[4]),
            "T4w fixtures: two more staged VS, hidden from the partial cache");
      if (hidden.size() == 5) {
        std::vector<std::vector<uint8_t>> v;
        for (size_t i = 3; i < 5; i++) v.push_back(readAll(misses / (hx(hidden[i]) + ".vs.bin")));
        char h3[20], h4[20];
        std::snprintf(h3, sizeof(h3), "%016" PRIX64, hidden[3]);
        std::snprintf(h4, sizeof(h4), "%016" PRIX64, hidden[4]);
        // (a) the DEFAULT (no override, SR_SHADER_RUNTIME_XLAT_WAIT unset): the request returns admitted.
        G::rtxlat::setWaitModeForTest(-1);
        const G::rtxlat::WaitStats wa = G::rtxlat::waitStats();
        c1.requestTranslation(hidden[3], true, v[0].data(), v[0].size());
        const G::rtxlat::WaitStats wb = G::rtxlat::waitStats();
        check(c1.findVs(hidden[3]) != nullptr && wb.waits == wa.waits + 1 && wb.admitted == wa.admitted + 1,
              "T4w default: the IM_LOAD miss waits and returns with the shader admitted (no draw dropped)");
        check(countLines(std::string("[SHADER_WAIT] VS ") + h3, "admitted after") == 1,
              "T4w default: the wait is named once ([SHADER_WAIT] ... admitted after N ms)");
        // (b) the bound: a translation slower than the budget releases the IM_LOAD at the budget, says so,
        // and the draw that follows reads wait-budget-exceeded; a later IM_LOAD does not wait again; the
        // worker still admits it afterwards.
        G::rtxlat::setJobDelayForTest(3000);
        G::rtxlat::setWaitBudgetForTest(500);
        auto t0 = std::chrono::steady_clock::now();
        c1.requestTranslation(hidden[4], true, v[1].data(), v[1].size());
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const G::rtxlat::WaitStats wc = G::rtxlat::waitStats();
        check(ms >= 450 && ms < 2500 && !c1.findVs(hidden[4]) && wc.timeouts == wb.timeouts + 1 &&
                  G::rtxlat::statusForTest(hidden[4], true) == "queued",
              "T4w bound: a 3 s translation releases the IM_LOAD at the 0.5 s budget", std::to_string(ms) + " ms");
        check(countLines(std::string("[SHADER_WAIT] VS ") + h4, "NOT translated within") == 1,
              "T4w bound: the timeout is named");
        check(c1.noteVsMissDraw(hidden[4], 0) == G::kDropWaitedOut,
              "T4w bound: the draw behind it reads wait-budget-exceeded (dev drops it, release names it)");
        t0 = std::chrono::steady_clock::now();
        c1.requestTranslation(hidden[4], true, v[1].data(), v[1].size());
        const double ms2 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        check(ms2 < 200 && G::rtxlat::waitStats().waits == wc.waits,
              "T4w bound: a later IM_LOAD of a waited-out shader does not wait again", std::to_string(ms2) + " ms");
        G::rtxlat::setJobDelayForTest(0);
        G::rtxlat::setWaitBudgetForTest(-1);
        check(G::rtxlat::waitIdleForTest(60000) && c1.findVs(hidden[4]) != nullptr,
              "T4w bound: the worker still admits it (the next draw has it)");
        // (c) no template: the wait ends at the answer, long before the budget; the draw reads its final state.
        const X::CfWalk w = X::cfExecWalk(u[0].data(), u[0].size());
        uint32_t alu = 0xFFFFFFFFu;
        for (const auto& sl : w.slots)
          if (!sl.isFetch && alu == 0xFFFFFFFFu) alu = sl.slot;
        std::vector<uint8_t> m = u[0];
        if (alu != 0xFFFFFFFFu) {
          const size_t at = (size_t(alu) * 3 + 1) * 4;
          setBe32(m.data() + at, be32(m.data() + at) ^ 0x00010000u);
        }
        const uint64_t hm = XXH3_64bits(m.data(), m.size());
        t0 = std::chrono::steady_clock::now();
        c1.requestTranslation(hm, true, m.data(), m.size());
        const double ms3 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const int st3 = c1.noteVsMissDraw(hm, 0);
        check(alu != 0xFFFFFFFFu && !c1.findVs(hm) && ms3 < 10000 &&
                  (st3 == G::kDropWaiting || st3 == G::kDropRefused),
              "T4w no template: a VS no container runs returns its final answer (no wait to the budget)",
              std::to_string(ms3) + " ms, state " + G::dropStateName(st3));
        // (e) a pixel shader takes the same path: a PS no container runs returns at once and its draw reads it.
        {
          std::vector<uint8_t> junk(96, 0);
          junk[0] = 0x5A;  // not T3's all-zero PS: a hash no request has seen
          const uint64_t hp = XXH3_64bits(junk.data(), junk.size());
          t0 = std::chrono::steady_clock::now();
          c1.requestTranslation(hp, false, junk.data(), junk.size());
          const double msp = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
          const int stp = c1.notePsMissDraw(hp);
          check(!c1.findPs(hp) && msp < 10000 && (stp == G::kDropWaiting || stp == G::kDropRefused),
                "T4w PS: a pixel shader miss waits for its answer and its draw reads the final state",
                std::to_string(msp) + " ms, state " + G::dropStateName(stp));
        }
        // (f) the graceful quit (Astra round 1, P2): once the CP drains, a wait in progress ends within 5 s of it,
        // whatever the budget, so kernel/quit.cpp's 10 s drain is never spent on one translation.
        {
          std::vector<uint8_t> junk(96, 0);
          junk[0] = 0xA5;
          const uint64_t hq = XXH3_64bits(junk.data(), junk.size());
          G::rtxlat::setJobDelayForTest(8000);
          s_obsDrain = true;
          t0 = std::chrono::steady_clock::now();
          c1.requestTranslation(hq, false, junk.data(), junk.size());
          const double msq = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
          s_obsDrain = false;
          G::rtxlat::setJobDelayForTest(0);
          char hqx[20];
          std::snprintf(hqx, sizeof(hqx), "%016" PRIX64, hq);
          check(msq >= 4500 && msq < 7000 && countLines(std::string("[SHADER_WAIT] PS ") + hqx, "cut short by the quit") == 1,
                "T4w quit: a wait in progress when the CP drains ends within 5 s (budget 30 s, an 8 s translation)",
                std::to_string(msq) + " ms");
          G::rtxlat::waitIdleForTest(60000);
        }
        // (d) every wait was a host wait for the CP (begin == end == the waits made; none of another kind).
        const G::rtxlat::WaitStats wd = G::rtxlat::waitStats();
        check(s_obsBegin.load() == int(wd.waits - w0.waits) && s_obsEnd.load() == s_obsBegin.load() &&
                  s_obsOther.load() == 0 && wd.waits - w0.waits == 6,
              "T4w: every wait is reported to the CP's host-wait observer (6 waits: WAIT, default, bound, no template, "
              "PS, quit)",
              std::to_string(s_obsBegin.load()) + "/" + std::to_string(s_obsEnd.load()) + "/" +
                  std::to_string(s_obsOther.load()) + " waits " + std::to_string(wd.waits - w0.waits));
      }
      G::ShaderCache::setWaitObserver(nullptr);
    }
  }

  // ── T8 (step 4 lane C round 4): the alpha-test variant's order (gfx/alpha_test.hpp use()) ──
  if (!expectOff) {
    std::printf("T8 alpha-test variant: (a) the PS's own, (b) made by the runtime translator at IM_LOAD, (c) else drop\n");
    const uint64_t h1 = 0xf6e8327ba646accdull;  // the arena grate (RB_COLORCONTROL GREATER 0.502)
    const uint64_t h2 = 0x032738d48cc2c08full;
    const fs::path k1p = "data/shaders_carved/ps_0164b976492a4add_B7A8CC5C.xpu";  // regen_at.py provenance
    const fs::path k2p = "data/shaders_carved/ps_0b6c88d21a685005_B7ACBDA8.xpu";
    X::Container k1, k2;
    const bool have = loadContainerFile(k1p, k1) && loadContainerFile(k2p, k2) && !k1.isVs && !k2.isVs &&
                      k1.ucodeHash == h1 && k2.ucodeHash == h2 && fs::exists(cache / (hx(h1) + ".ps.at.dxil")) &&
                      fs::exists(cache / (hx(h2) + ".ps.at.dxil"));
    check(have, "T8 fixtures: PS F6E8327B and 032738D4, their containers (data/shaders_carved) and variants");
    if (have) {
      // Installed cache A: the cache minus both PS's .ps.at.dxil, h2's .ps.dxil replaced by h1's (a translation that
      // is not h2's: the provenance refusal).
      const fs::path partA = work / "cache_no_at";
      fs::remove_all(partA, ec);
      fs::create_directories(partA, ec);
      for (auto& e : fs::directory_iterator(cache, ec)) {
        const std::string n = u8s(e.path().filename());
        if (n == hx(h1) + ".ps.at.dxil" || n == hx(h2) + ".ps.at.dxil" || n == hx(h2) + ".ps.dxil") continue;
        fs::copy_file(e.path(), partA / n, fs::copy_options::overwrite_existing, ec);
      }
      fs::copy_file(cache / (hx(h1) + ".ps.dxil"), partA / (hx(h2) + ".ps.dxil"), fs::copy_options::overwrite_existing,
                    ec);
      const fs::path rt6 = work / "rt_at";
      fs::remove_all(rt6, ec);
      _putenv_s("SR_SHADER_CACHE_DIR", u8s(partA).c_str());
      _putenv_s("SR_SHADER_RUNTIME_DIR", u8s(rt6).c_str());
      _putenv_s("SR_DXC_DIR", u8s(fs::absolute(dxcDir)).c_str());
      G::ShaderCache c6;
      c6.ensureLoaded();
      const G::CachedShader* p1 = c6.findPs(h1);
      const G::CachedShader* p2 = c6.findPs(h2);
      check(p1 && p2 && p1->dxilAlphaTest.empty() && p2->dxilAlphaTest.empty() && c6.needsAlphaTestVariant(h1, *p1) &&
                c6.needsAlphaTestVariant(h2, *p2),
            "T8 the partial cache holds both PS without a variant (both write colour 0)");
      // The game's CreatePixelShader containers feed the worker's index (a replay names corpus dirs instead).
      G::ShaderCache::noteContainer(k1.bytes.data(), k1.bytes.size(), false);
      G::ShaderCache::noteContainer(k2.bytes.data(), k2.bytes.size(), false);
      G::rtxlat::setWaitModeForTest(1);
      if (p1) c6.requestAlphaTestVariant(h1, k1.ucode(), k1.sz);
      const std::vector<uint8_t>* v1 = p1 ? c6.alphaTestDxil(h1, *p1) : nullptr;
      const std::vector<uint8_t> off1 = readAll(cache / (hx(h1) + ".ps.at.dxil"));
      check(v1 && *v1 == off1 && G::rtxlat::alphaTestStatusForTest(h1) == "admitted",
            "T8 (b) the grate PS's IM_LOAD: its variant is made before the request returns, byte-identical to the offline "
            ".ps.at.dxil");
      check(p1 && c6.runtimeAlphaTestCount() == 1 && !c6.needsAlphaTestVariant(h1, *p1) && c6.hasAlphaTestVariant(h1, *p1),
            "T8 (b) admitted once; the PS no longer needs one");
      G::rtxlat::waitIdleForTest(60000);
      const fs::path d6 = u8p(G::rtxlat::runtimeDirForTest());
      check(p1 && readAll(d6 / (hx(h1) + ".ps.atv.dxil")) == off1 &&
                readText(d6 / (hx(h1) + ".ps.atv")).find("atv plain " + hx(XXH3_64bits(p1->dxil.data(), p1->dxil.size()))) !=
                    std::string::npos,
            "T8 (b) persisted: <h>.ps.atv.dxil + the .ps.atv manifest naming the plain DXIL it was made for");
      if (p2) c6.requestAlphaTestVariant(h2, k2.ucode(), k2.sz);
      check(p2 && G::rtxlat::alphaTestStatusForTest(h2) == "refused" && !c6.hasAlphaTestVariant(h2, *p2) &&
                countLines("[XLAT_ALPHA] PS 032738D48CC2C08F: the runtime alpha-test variant was NOT made "
                           "(its plain translation differs") == 1,
            "T8 (b) a translation that does not reproduce the cached PS is refused (the variant would not be that shader)");
      const int st = c6.noteAlphaTestDrop(h2);
      c6.flushVsMissReport();
      check(st == G::kDropAlphaVariantFailed && c6.alphaTestDrops() == 1 &&
                countLines("[SHADER_DROP] PS 032738D48CC2C08F: draw DROPPED - alpha-test-variant-unavailable") == 1,
            "T8 (c) its alpha-tested draw is dropped and named ([SHADER_DROP] census line, final)");
      // The next boot: the persisted variant is admitted with no job, and a repeated IM_LOAD asks for nothing.
      G::ShaderCache c6b;
      c6b.ensureLoaded();
      const G::CachedShader* q1 = c6b.findPs(h1);
      check(q1 && c6b.hasAlphaTestVariant(h1, *q1) && v1 && *c6b.alphaTestDxil(h1, *q1) == *v1 &&
                c6b.runtimeAlphaTestCount() == 1 &&
                countLines("[XLAT_ALPHA] 1 persisted runtime alpha-test variants admitted, 0 skipped") >= 1,
            "T8 a fresh cache admits the persisted variant at load (no job)");
      if (q1) c6b.requestAlphaTestVariant(h1, k1.ucode(), k1.sz);
      G::rtxlat::waitIdleForTest(10000);
      check(countLines("[SHADER_XLAT] request PS alpha-test variant F6E8327BA646ACCD") == 1,
            "T8 ... and its IM_LOAD asks for no second translation");
      // (a) wins: with the full cache the PS has its own variant; the persisted one is skipped, nothing requested.
      _putenv_s("SR_SHADER_CACHE_DIR", u8s(cache).c_str());
      G::ShaderCache c6c;
      c6c.ensureLoaded();
      const G::CachedShader* f1 = c6c.findPs(h1);
      check(f1 && !f1->dxilAlphaTest.empty() && c6c.runtimeAlphaTestCount() == 0 && !c6c.needsAlphaTestVariant(h1, *f1) &&
                *c6c.alphaTestDxil(h1, *f1) == off1 &&
                countLines("[XLAT_ALPHA] PS F6E8327BA646ACCD: persisted alpha-test variant skipped (the PS has a variant "
                           "already)") == 1,
            "T8 (a) the PS's own .ps.at.dxil wins; the persisted runtime one is skipped");
      // A persisted variant made for another plain translation is skipped (the installed cache changed), and a
      // persisted PS entry whose manifest names a variant without its file is incomplete.
      const fs::path partB = work / "cache_no_at_b";
      fs::remove_all(partB, ec);
      fs::create_directories(partB, ec);
      for (auto& e : fs::directory_iterator(partA, ec)) {
        const std::string n = u8s(e.path().filename());
        if (n == hx(h2) + ".ps.dxil") continue;  // h2 becomes a runtime-only PS below
        fs::copy_file(e.path(), partB / n, fs::copy_options::overwrite_existing, ec);
      }
      fs::copy_file(cache / (hx(h2) + ".ps.dxil"), partB / (hx(h1) + ".ps.dxil"), fs::copy_options::overwrite_existing,
                    ec);
      fs::copy_file(cache / (hx(h2) + ".ps.dxil"), d6 / (hx(h2) + ".ps.dxil"), fs::copy_options::overwrite_existing, ec);
      {
        std::ofstream xf(d6 / (hx(h2) + ".ps.xlat"), std::ios::binary);
        xf << "shader " << hx(h2) << " PS\ntranslator test\npath exact (T6)\nat alpha-test variant\n";
      }
      fs::remove(d6 / (hx(h2) + ".ps.at.dxil"), ec);
      _putenv_s("SR_SHADER_CACHE_DIR", u8s(partB).c_str());
      G::ShaderCache c6d;
      c6d.ensureLoaded();
      check(c6d.runtimeAlphaTestCount() == 0 && !c6d.findPs(h2) &&
                countLines("[XLAT_ALPHA] PS F6E8327BA646ACCD: persisted alpha-test variant skipped (made for another "
                           "plain translation)") == 1 &&
                countLines("persisted entry skipped (the manifest names an alpha-test variant but there is no "
                           ".ps.at.dxil)") == 1,
            "T8 a variant made for another plain translation is skipped; a PS entry missing its named variant is incomplete");
      G::rtxlat::setWaitModeForTest(-1);
      _putenv_s("SR_SHADER_CACHE_DIR", u8s(cache).c_str());
    }
  }

  }  // !onlyParity

  // ── T5 ──
  if (!expectOff && a.count("--parity-in") && a.count("--parity-out")) {
    std::printf("T5 parity with the offline XenosRecomp.exe over %s\n", a["--parity-in"].c_str());
    // main.cpp's order (container hash); the last successful writer of a ucode wins.
    const std::map<uint64_t, std::vector<uint8_t>> containers = scanLikeOffline(u8p(a["--parity-in"]));
    const fs::path out = u8p(a["--parity-out"]);
    std::map<std::string, X::Files> last;  // "<ucode>.<vs|ps>" -> the last successful translation
    size_t invalid = 0, failed = 0;
    std::ofstream failures(work / "parity_failures.txt");  // "<container hash> <reason>"
    for (auto& [ch, bytes] : containers) {
      X::Container c;
      std::string w;
      if (!X::parseContainer(bytes.data(), bytes.size(), hx(ch), c, &w)) {
        invalid++;
        continue;
      }
      X::Files f;
      if (std::getenv("SR_XLAT_TEST_TRACE")) {
        std::printf("    [trace] %s %s %s\n", hx(ch).c_str(), c.isVs ? "VS" : "PS", hx(c.ucodeHash).c_str());
        std::fflush(stdout);
      }
      if (!X::translateContainer(c, dxc, f, &w)) {
        failed++;
        if (std::getenv("SR_XLAT_TEST_TRACE")) std::printf("    [trace]   failed: %.300s\n", w.c_str());
        failures << hx(ch) << " " << w.substr(0, 200) << "\n";  // the runner matches offline SKIPs
        continue;
      }
      last[hx(c.ucodeHash) + (c.isVs ? ".vs" : ".ps")] = std::move(f);
    }
    size_t same = 0, differ = 0, onlyRt = 0, onlyOff = 0;
    std::string bad;
    for (auto& [stem, f] : last) {
      if (!fs::exists(out / (stem + ".dxil"))) {
        onlyRt++;
        bad += " " + stem + "(only runtime)";
        continue;
      }
      std::string d;
      if (sameFiles(f, out, unhex(stem.substr(0, 16)), &d)) same++;
      else {
        differ++;
        bad += " " + stem + ":" + d;
      }
    }
    for (auto& e : fs::directory_iterator(out, ec)) {
      const std::string n = u8s(e.path().filename());
      if (n.size() == 24 && (n.compare(16, 8, ".vs.dxil") == 0 || n.compare(16, 8, ".ps.dxil") == 0) &&
          !last.count(n.substr(0, 19))) {
        onlyOff++;
        bad += " " + n + "(only offline)";
      }
    }
    std::printf("    %zu containers: %zu outputs byte-identical, %zu differ, %zu only runtime, %zu only offline; "
                "%zu failed validation, %zu failed translation (offline SKIPs)\n",
                containers.size(), same, differ, onlyRt, onlyOff, invalid, failed);
    if (!bad.empty()) std::printf("    %.2000s\n", bad.c_str());
    check(same > 0 && differ == 0 && onlyRt == 0 && onlyOff == 0,
          "every offline output is reproduced byte-identical in-process, and nothing else");
  }

  std::printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
