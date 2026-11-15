// iter-463: masked vertex-shader keys + the [SHADER_MISS] report (gfx/shader_mkey.hpp,
// gfx/shader_cache.cpp). Standalone (no guest, no D3D): it links the REAL shader_cache.cpp
// and app/log.cpp and reads real ucode from the Warworld arena run.
//
// Data: python tools/difftest/shader_mkey_fixture.py --out <DIR> ... (see that file).
// Build + run + mutation check: python tools/difftest/shader_mkey_test_run.py
//
// Checks
//   T1  the C++ key equals tools/shader_mkey.py's on every ucode (16 misses + the cache's VS)
//   T2  DBE06113 (arena, missing) and FAF94BDB (cached) share a masked key; raw keys differ
//   T3  28A1A12B and 3A60C7C1 (the iter-462 dry-run template for it) do NOT: the masked bytes
//       differ at ALU instruction 9, which is not a vertex fetch
//   T4  the rows derived from the RUNTIME ucode equal XenosRecomp's .vs.vfetch sidecar
//       (slot, offset, stride, format, signed, integer) for every ucode with a translation
//   T5  the stride comes from the runtime ucode: DBE06113 derives stride 9 on slot 95 where
//       its cached twin's sidecar says 8
//   T5b the key masks only layout/format: a stride change keeps it, an exp_adjust or
//       signed_rf_mode change (value transforms no sidecar column carries) changes it
//   T6  ShaderCache diagnosis (SR_SHADER_CACHE_DIR = fixture cache): the .vs.mkey index finds
//       FAF94BDB for DBE06113 and names "stride 9(cached 8)"; 28A1A12B gets "no cached VS with
//       the same program AND the same fetch swizzles"; a VS whose ucode was never noted gets
//       "ucode not noted". NB: in the exe nothing calls noteVsMissUcode yet (the IM_LOAD hook
//       in pm4_engine.cpp is not wired), so only the "ucode not noted" branch is live there.
//   T7  the live path (noteVsMissDraw + flushVsMissReport, what the exe runs): read back from
//       the log's tail ring - one first-sighting line per missed VS naming its first PS, none
//       repeated on a later flush, and the summary counts drops across drains until it logs
//       (at most once per 2 s)
//
// Run: shader_mkey_test.exe <fixture dir>   (exit code = failed checks; prints "0 failed")
#include "gfx/shader_cache.hpp"
#include "gfx/shader_mkey.hpp"
#include "app/log.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define XXH_INLINE_ALL
#include "gfx/thirdparty/xxhash.h"

namespace G = sr::gfx;
namespace M = sr::gfx::mkey;

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
  if (ok) {
    g_pass++;
    std::printf("  PASS %s\n", what);
  } else {
    g_fail++;
    std::printf("  FAIL %s %s\n", what, detail.c_str());
  }
}

static std::vector<uint8_t> readAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// The log's in-process tail ring (sr::Log keeps the last kMaxTailLines lines).
static std::vector<std::string> tailLines() {
  std::vector<char> buf(size_t(sr::Log::kMaxTailLines) * 4300, 0);
  sr::Log::getTailLines(buf.data(), int(buf.size()));
  std::vector<std::string> out;
  std::istringstream is(std::string(buf.data()));
  std::string l;
  while (std::getline(is, l)) out.push_back(l);
  return out;
}
static int countLines(const std::vector<std::string>& lines, const std::string& a, const std::string& b = "") {
  int n = 0;
  for (const auto& l : lines)
    if (l.find(a) != std::string::npos && (b.empty() || l.find(b) != std::string::npos)) n++;
  return n;
}

static bool key(const std::vector<uint8_t>& u, uint64_t& out) {
  std::vector<uint8_t> m;
  if (!M::maskedUcode(u.data(), u.size(), m)) return false;
  out = XXH3_64bits(m.data(), m.size());
  return true;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: shader_mkey_test <fixture dir>\n");
    return 99;
  }
  const std::string dir = argv[1];
  auto ucodePath = [&](const std::string& h) { return dir + "/ucode/" + h + ".vs.bin"; };

  // ── T1: C++ == Python on every ucode ──
  std::printf("T1 C++ masked key == tools/shader_mkey.py\n");
  {
    std::ifstream f(dir + "/expect.txt");
    std::string h, want;
    int n = 0, bad = 0;
    std::string firstBad;
    while (f >> h >> want) {
      auto u = readAll(ucodePath(h));
      uint64_t k = 0;
      const bool ok = key(u, k);
      char got[32];
      std::snprintf(got, sizeof(got), "%016llx", (unsigned long long)k);
      const bool same = ok ? (want == got) : (want == "NOKEY");
      if (!same && bad++ == 0) firstBad = h + " want " + want + " got " + (ok ? got : "NOKEY");
      n++;
    }
    char w[64];
    std::snprintf(w, sizeof(w), "%d ucodes agree (%d differ)", n - bad, bad);
    check(n >= 200 && bad == 0, w, firstBad);
  }

  const auto dbe = readAll(ucodePath("dbe06113c69da55b"));
  const auto faf = readAll(ucodePath("faf94bdb30e37a25"));
  const auto a28 = readAll(ucodePath("28a1a12bd4dde000"));
  const auto t3a = readAll(ucodePath("3a60c7c1096a40c9"));

  // ── T2 ──
  std::printf("T2 DBE06113 vs FAF94BDB (stride 9 vs 8)\n");
  {
    uint64_t k1 = 0, k2 = 0;
    const bool ok = key(dbe, k1) && key(faf, k2);
    check(ok && k1 == k2, "masked keys equal");
    check(XXH3_64bits(dbe.data(), dbe.size()) != XXH3_64bits(faf.data(), faf.size()), "raw keys differ");
  }

  // ── T3 ──
  std::printf("T3 28A1A12B vs 3A60C7C1 (ALU-different template)\n");
  {
    uint64_t k1 = 0, k2 = 0;
    const bool ok = key(a28, k1) && key(t3a, k2);
    check(ok && k1 != k2, "masked keys differ");
    std::vector<uint8_t> m1, m2;
    std::vector<uint32_t> vf;
    M::maskedUcode(a28.data(), a28.size(), m1);
    M::maskedUcode(t3a.data(), t3a.size(), m2);
    M::vfetchSlots(a28.data(), a28.size(), vf);
    std::vector<uint32_t> diffInstr;
    for (size_t i = 0; i + 4 <= m1.size() && i + 4 <= m2.size(); i += 4)
      if (M::be32(&m1[i]) != M::be32(&m2[i])) {
        const uint32_t ins = uint32_t(i / 12);
        if (diffInstr.empty() || diffInstr.back() != ins) diffInstr.push_back(ins);
      }
    // Expected: i4 (a vfetch whose destination swizzle differs - kept in the key) and
    // i9 (ALU: write mask .xyzw vs .xy). The ALU difference alone makes them different
    // programs, whatever the fetch fields.
    const bool expected = diffInstr.size() == 2 && diffInstr[0] == 4 && diffInstr[1] == 9;
    bool nineIsFetch = false, fourIsFetch = false;
    for (uint32_t s : vf) { nineIsFetch |= (s == 9); fourIsFetch |= (s == 4); }
    std::string d;
    for (uint32_t x : diffInstr) d += " i" + std::to_string(x);
    check(expected, "masked bytes differ at exactly i4 and i9", "(differ at" + d + ")");
    check(fourIsFetch && !nineIsFetch, "i4 is a vertex fetch, i9 is ALU (a different program)");
  }

  // ── T4 ──
  std::printf("T4 rows derived from the runtime ucode == XenosRecomp .vs.vfetch sidecar\n");
  {
    std::ifstream f(dir + "/sidecars.txt");
    std::string h, path;
    int n = 0, bad = 0;
    std::string firstBad;
    while (f >> h) {
      std::getline(f, path);
      path.erase(0, path.find_first_not_of(' '));
      auto u = readAll(ucodePath(h));
      std::vector<M::FetchRow> rows;
      const bool walks = M::deriveFetchRows(u.data(), u.size(), rows);
      std::ifstream sf(path);
      std::string line;
      std::vector<std::vector<long long>> side;
      while (std::getline(sf, line)) {
        std::istringstream is(line);
        std::vector<long long> v;
        long long x;
        while (is >> x) v.push_back(x);
        if (v.size() >= 8) side.push_back(v);
      }
      bool same = walks ? rows.size() == side.size() : side.empty();
      for (size_t i = 0; same && i < rows.size(); i++) {
        const auto& r = rows[i];
        const auto& s = side[i];
        same = s[0] == r.slot && s[1] == r.offsetDwords && s[2] == r.strideDwords && s[3] == r.format &&
               s[6] == (r.isSigned ? 1 : 0) && s[7] == (r.isInteger ? 1 : 0);
      }
      if (!same && bad++ == 0) firstBad = h;
      n++;
    }
    char w[80];
    std::snprintf(w, sizeof(w), "%d translations: sidecar == runtime-ucode rows (%d differ)", n - bad, bad);
    check(n >= 200 && bad == 0, w, firstBad);
  }

  // ── T5 ──
  std::printf("T5 stride from the runtime ucode\n");
  {
    std::vector<M::FetchRow> r1, r2;
    M::deriveFetchRows(dbe.data(), dbe.size(), r1);
    M::deriveFetchRows(faf.data(), faf.size(), r2);
    int s95a = 0, s95b = 0;
    bool all9 = true, all8 = true;
    for (const auto& r : r1)
      if (r.slot == 95) { s95a++; all9 &= r.strideDwords == 9; }
    for (const auto& r : r2)
      if (r.slot == 95) { s95b++; all8 &= r.strideDwords == 8; }
    check(s95a == 4 && all9, "DBE06113: 4 slot-95 rows, all stride 9");
    check(s95b == 4 && all8, "FAF94BDB: 4 slot-95 rows, all stride 8");
  }

  // ── T5b ── (no fixture ucode sets exp_adjust or signed_rf_mode, so T1 cannot see them)
  std::printf("T5b the key masks layout/format only\n");
  {
    std::vector<uint32_t> vf;
    M::vfetchSlots(dbe.data(), dbe.size(), vf);
    auto withBits = [&](size_t dwi, uint32_t orMask, uint32_t andMask) {
      std::vector<uint8_t> u = dbe;
      uint8_t* p = u.data() + dwi * 4;
      const uint32_t v = (M::be32(p) & andMask) | orMask;
      p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
      return u;
    };
    uint64_t k0 = 0, kStride = 0, kExp = 0, kRf = 0;
    const bool ok = !vf.empty() && key(dbe, k0);
    const size_t s = ok ? vf[0] : 0;
    key(withBits(s * 3 + 2, 0x0000000Bu, 0xFFFFFF00u), kStride);  // stride -> 11
    key(withBits(s * 3 + 1, 0x3F000000u, 0xC0FFFFFFu), kExp);     // exp_adjust -> -1
    key(withBits(s * 3 + 1, 0x00004000u, 0xFFFFFFFFu), kRf);      // signed_rf_mode_all -> 1
    check(ok && kStride == k0, "stride change keeps the masked key");
    check(ok && kExp != k0, "exp_adjust change changes the masked key");
    check(ok && kRf != k0, "signed_rf_mode change changes the masked key");
  }

  // ── T6 ──
  std::printf("T6 ShaderCache index + [SHADER_MISS] diagnosis\n");
  {
    const std::string cdir = dir + "/cache";
    _putenv_s("SR_SHADER_CACHE_DIR", cdir.c_str());
    G::ShaderCache cache;
    cache.ensureLoaded();
    const G::CachedShader* faf0 = cache.findVs(0xfaf94bdb30e37a25ull);
    check(faf0 && faf0->mkey != 0, "FAF94BDB loaded with its .vs.mkey");
    check(!cache.findVs(0xdbe06113c69da55bull), "DBE06113 misses the cache (the defect)");
    cache.noteVsMissUcode(0xdbe06113c69da55bull, dbe.data(), dbe.size());
    cache.noteVsMissUcode(0x28a1a12bd4dde000ull, a28.data(), a28.size());
    const std::string d1 = cache.diagnoseVsMiss(0xdbe06113c69da55bull);
    const std::string d2 = cache.diagnoseVsMiss(0x28a1a12bd4dde000ull);
    const std::string d3 = cache.diagnoseVsMiss(0x40078e738d652946ull);
    std::printf("    DBE06113: %s\n    28A1A12B: %s\n    40078E73: %s\n", d1.c_str(), d2.c_str(), d3.c_str());
    check(d1.find("same program as cached VS FAF94BDB30E37A25") != std::string::npos, "DBE06113 -> twin FAF94BDB");
    check(d1.find("stride 9(cached 8)") != std::string::npos, "DBE06113 diagnosis names stride 9 vs cached 8");
    check(d2.find("no cached VS with the same program AND the same fetch swizzles") != std::string::npos,
          "28A1A12B -> no strict twin (A214E11F differs in mini/dst_swiz, 3A60C7C1 in ALU)");
    check(d3.find("ucode not noted") != std::string::npos, "un-noted VS -> ucode not noted");
    uint64_t twin = 0;
    uint64_t k = 0;
    key(dbe, k);
    check(cache.findVsTwin(k, &twin) == faf0 && twin == 0xfaf94bdb30e37a25ull, "findVsTwin(mkey) == FAF94BDB");

    // ── T7: the path the exe runs, read back from the log ──
    std::printf("T7 noteVsMissDraw + flushVsMissReport -> [SHADER_MISS] lines\n");
    const uint64_t kDbe = 0xdbe06113c69da55bull, kA28 = 0x28a1a12bd4dde000ull, k400 = 0x40078e738d652946ull;
    const char* firstDbe = "[SHADER_MISS] VS DBE06113C69DA55B has no translation";
    const char* firstA28 = "[SHADER_MISS] VS 28A1A12BD4DDE000 has no translation";
    const char* first400 = "[SHADER_MISS] VS 40078E738D652946 has no translation";
    const char* summary = "[SHADER_MISS] since last summary:";
    const int miss0 = countLines(tailLines(), "[SHADER_MISS]");
    // drain 1: two drops of DBE06113 -> its first sighting + the first summary
    cache.noteVsMissDraw(kDbe, 0x084f6366540a148aull);
    cache.noteVsMissDraw(kDbe, 0x084f6366540a148aull);
    cache.flushVsMissReport();
    auto L = tailLines();
    std::string all;
    for (const auto& l : L) if (l.find("[SHADER_MISS]") != std::string::npos) all += "\n      " + l;
    std::printf("    after drain 1:%s\n", all.c_str());
    check(miss0 == 0, "no [SHADER_MISS] line before the first drop");
    check(countLines(L, firstDbe, "(first with PS 084F6366540A148A): same program as cached VS FAF94BDB30E37A25") == 1,
          "drain 1: one first-sighting line for DBE06113, with its first PS and its twin");
    check(countLines(L, summary, "2 draws dropped for 1 VS with no translation (2 since boot; top: DBE06113C69DA55Bx2)") == 1,
          "drain 1: summary counts 2 drops for 1 VS");
    // drain 2: nothing dropped -> nothing logged
    cache.flushVsMissReport();
    check(countLines(tailLines(), "[SHADER_MISS]") == 2, "drain 2 (no drops): no new line");
    // drains 3 and 4, inside the 2 s window: first sightings only; drops accumulate
    cache.noteVsMissDraw(kDbe, 0x1111111111111111ull);
    cache.noteVsMissDraw(kDbe, 0x1111111111111111ull);
    cache.noteVsMissDraw(kDbe, 0x1111111111111111ull);
    cache.noteVsMissDraw(kA28, 0x2222222222222222ull);
    cache.flushVsMissReport();
    cache.noteVsMissDraw(k400, 0x3333333333333333ull);
    cache.noteVsMissDraw(k400, 0x3333333333333333ull);
    cache.flushVsMissReport();
    L = tailLines();
    check(countLines(L, firstDbe) == 1, "DBE06113 is not re-announced");
    check(countLines(L, firstA28, "(first with PS 2222222222222222): no cached VS with the same program") == 1,
          "drain 3: first sighting of 28A1A12B");
    check(countLines(L, first400, "(first with PS 3333333333333333): ucode not noted") == 1,
          "drain 4: first sighting of 40078E73 (ucode not noted)");
    check(countLines(L, summary) == 1, "drains 3-4 (< 2 s after the last summary): no summary yet");
    // drain 5, past the window: one summary covering drains 3-5 (6 + 1 = 7 drops, 3 VS)
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    cache.noteVsMissDraw(kDbe, 0x1111111111111111ull);
    cache.flushVsMissReport();
    L = tailLines();
    all.clear();
    for (const auto& l : L) if (l.find(summary) != std::string::npos) all += "\n      " + l;
    std::printf("    summaries:%s\n", all.c_str());
    check(countLines(L, summary, "7 draws dropped for 3 VS with no translation (9 since boot; top: DBE06113C69DA55Bx4") == 1,
          "drain 5: summary covers drains 3-5 (7 drops, 3 VS, 9 since boot)");
    check(countLines(L, "[SHADER_MISS]") == 5, "5 [SHADER_MISS] lines in all (3 first sightings, 2 summaries)");
    check(L.size() < size_t(sr::Log::kMaxTailLines), "the tail ring did not overflow (counts above are complete)");
  }

  std::printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
