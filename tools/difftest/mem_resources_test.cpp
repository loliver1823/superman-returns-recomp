// mem.resources step 2a (docs/RELEASE_COMPOSITION.md section 9.5 item 1): the resource registry's generations
// (gfx/resource_registry.{hpp,cpp}) and the shadow census that grades it against the page model
// (gfx/resource_census.{hpp,cpp}, SR_MEM_RESOURCES=census) - every decision class, every attribution dimension, every
// note source, the coverage tallies, the checkers and the IB arm, on synthetic sequences whose answers are written down
// before the run. C13-C22 are Astra's counterexamples to the first 2a census (archive/memres/astra_review_2a.txt):
// each failed against it (or could not be expressed) and must hold now.
// tools/difftest/mem_resources_mutants.py applies the named mutants to the two units; each must make this suite fail.
//
// Run: build/difftest.exe --mem-resources-test   (exit code = failed checks)
#include "gfx/cube_layout.hpp"
#include "gfx/resource_census.hpp"
#include "gfx/resource_registry.hpp"
#define XXH_INLINE_ALL
#include "gfx/thirdparty/xxhash.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace R = sr::gfx::memres;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}
void check(bool ok, const std::string& name) { check(ok, name.c_str()); }

// memres5: a synthetic vertex shader, big-endian ucode - one exec_end control-flow instruction over the instructions
// from slot 1 (each a fetch or an ALU), and as its pair's second CF a nop or a test's loop / jump opcode.
struct Ins {
  bool fetch;
  uint32_t d0, d1, d2;
};
std::vector<uint8_t> vsUcode(const std::vector<Ins>& ins, uint32_t secondCfOpcode = 0) {
  uint32_t seq = 0;
  for (size_t i = 0; i < ins.size(); i++)
    if (ins[i].fetch) seq |= 1u << (2 * i);
  std::vector<uint32_t> w;
  w.push_back(1u | (uint32_t(ins.size()) << 12) | (seq << 16));  // exec_end: address 1, count, sequence
  w.push_back(2u << 12);                                         // its opcode (2 = exec_end); the second CF's low half 0
  w.push_back(secondCfOpcode << 28);                             // the second CF's opcode
  for (const Ins& x : ins) {
    w.push_back(x.d0);
    w.push_back(x.d1);
    w.push_back(x.d2);
  }
  std::vector<uint8_t> b;
  for (uint32_t v : w) {
    b.push_back(uint8_t(v >> 24));
    b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
  }
  return b;
}
// A vfetch_full of fetch constant `slot` into r<dst>.xyzw, its index r<src>.x (rel: relatively addressed).
Ins vfetch(uint32_t slot, uint32_t strideDw, int32_t offDw, uint32_t fmt, uint32_t src = 0, bool rel = false,
           uint32_t dst = 1) {
  const uint32_t d0 = (src << 5) | (rel ? 1u << 11 : 0u) | (dst << 12) | (1u << 19) | ((slot / 3u) << 20) |
                      ((slot % 3u) << 25);
  const uint32_t d1 = (0u | (1u << 3) | (2u << 6) | (3u << 9)) | (fmt << 16);
  const uint32_t d2 = (strideDw & 0xFFu) | ((uint32_t(offDw) & 0x7FFFFFu) << 8);
  return Ins{true, d0, d1, d2};
}
// An ALU whose vector destination is reg, write mask `mask` (bit 0 = x; default xyzw).
Ins aluWrites(uint32_t reg, uint32_t mask = 0xFu) { return Ins{false, reg | (mask << 16), 0u, 0u}; }
// An ALU whose SCALAR destination is reg (mask x; the vector half writes nothing).
Ins aluScalarWrites(uint32_t reg) { return Ins{false, (reg << 8) | (1u << 20), 0u, 0u}; }
// An ALU exporting its vector result (export_data: destination 0 is an export register, no GPR written).
Ins aluExports() { return Ins{false, (1u << 15) | (0xFu << 16), 0u, 0u}; }
// A vfetch_full into r<dst> with destination swizzle swz (3 bits per component, 7 = keep), index r0.x.
Ins vfetchSwz(uint32_t slot, uint32_t strideDw, uint32_t fmt, uint32_t dst, uint32_t swz) {
  Ins f = vfetch(slot, strideDw, 0, fmt, 0, false, dst);
  f.d1 = (f.d1 & ~0xFFFu) | (swz & 0xFFFu);
  return f;
}
// A vfetch_mini into r2 at offDw (it reads its full fetch's slot and index); pred: is_predicated (d1 bit 31).
Ins vfetchMini(int32_t offDw, uint32_t fmt) {
  return Ins{true, (2u << 12) | (1u << 19), (0u | (1u << 3) | (2u << 6) | (3u << 9)) | (fmt << 16) | (1u << 30),
             (uint32_t(offDw) & 0x7FFFFFu) << 8};
}
Ins predicated(Ins x) {
  x.d1 |= 1u << 31;
  return x;
}
// memres5: a vertex shader of several control-flow instructions: execs (opcode 1-6 / 13-14, with their
// instructions) and jumps (opcode 11: target CF index, word-1 bits). Instructions follow the CF pairs in order.
struct Cf {
  uint32_t op;
  std::vector<Ins> ins;
  uint32_t target = 0, w1bits = 0;
};
std::vector<uint8_t> vsProgram(const std::vector<Cf>& cfs) {
  const uint32_t pairs = uint32_t((cfs.size() + 1) / 2);
  std::vector<uint32_t> cw0, cw1, body;
  uint32_t addr = pairs;
  for (const Cf& c : cfs) {
    if (c.op == 11u || c.ins.empty()) {
      cw0.push_back(c.target & 0x1FFFu);
      cw1.push_back((c.op << 12) | (c.w1bits & 0xFFFu));
      continue;
    }
    uint32_t seq = 0;
    for (size_t i = 0; i < c.ins.size(); i++)
      if (c.ins[i].fetch) seq |= 1u << (2 * i);
    cw0.push_back(addr | (uint32_t(c.ins.size()) << 12) | (seq << 16));
    cw1.push_back(c.op << 12);
    for (const Ins& x : c.ins) {
      body.push_back(x.d0);
      body.push_back(x.d1);
      body.push_back(x.d2);
    }
    addr += uint32_t(c.ins.size());
  }
  if (cw0.size() & 1u) {  // a nop pads the last pair
    cw0.push_back(0);
    cw1.push_back(0);
  }
  std::vector<uint32_t> w;
  for (size_t k = 0; k < cw0.size(); k += 2) {
    w.push_back(cw0[k]);
    w.push_back((cw1[k] & 0xFFFFu) | (cw0[k + 1] << 16));
    w.push_back((cw0[k + 1] >> 16) | (cw1[k + 1] << 16));
  }
  w.insert(w.end(), body.begin(), body.end());
  std::vector<uint8_t> b;
  for (uint32_t v : w) {
    b.push_back(uint8_t(v >> 24));
    b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
  }
  return b;
}

// Two snapshots alive at once at most (each is large: keep them off the stack).
R::Totals& snapA() {
  static R::Totals t;
  R::snapshot(t);
  return t;
}
R::Totals& snapB() {
  static R::Totals t;
  R::snapshot(t);
  return t;
}
uint64_t O(const R::Totals& t, R::Outcome o) { return t.outcome[uint32_t(o)]; }
uint64_t C(const R::Totals& t, R::Cov c) { return t.cov[uint32_t(c)]; }
uint64_t K(const R::Totals& t, R::Feed f, R::Chk c) { return t.keep[uint32_t(f)][uint32_t(c)]; }
uint64_t missedAt(const R::Totals& t, R::Kind k, R::Prov p, R::Src s, R::Site si) {
  return t.missedBy[uint32_t(k)][uint32_t(p)][uint32_t(s)][uint32_t(si)];
}
// The gate counters (any of them moving is a FAIL or a GAP of the grader).
uint64_t gate(const R::Totals& t) {
  return O(t, R::Outcome::kMissed) + O(t, R::Outcome::kClobber) + t.winAliasDiff + C(t, R::Cov::kUnseen) +
         t.covUnaccounted;
}

bool sumsHold(const R::Totals& t) {
  uint64_t d = 0;
  for (uint32_t o = 0; o < R::kOutcomes; o++) d += t.outcome[o];
  if (t.decisions != d) return false;
  const uint64_t missed = O(t, R::Outcome::kMissed);
  if (missed != t.missedWhy[0] + t.missedWhy[1] + t.missedWhy[2] + t.missedWhy[3] || t.missedMovie > missed) return false;
  uint64_t m = 0, o = 0, f = 0;
  for (uint32_t k = 0; k < R::kKinds; k++) {
    for (uint32_t p = 0; p < R::kProvs; p++) {
      f += t.firstBy[k][p];
      for (uint32_t s = 0; s < R::kSrcs; s++)
        for (uint32_t si = 0; si < R::kSites; si++) m += t.missedBy[k][p][s][si];
    }
    for (uint32_t s = 0; s < R::kSrcs; s++) o += t.overBy[k][s];
  }
  if (m != missed || o != O(t, R::Outcome::kOver) || f != t.first) return false;
  // memres2: the missed class's attributes and grants each sum to missed
  uint64_t a = 0, gsum = 0;
  for (uint32_t k = 0; k < R::kKinds; k++)
    for (uint32_t s = 0; s < R::kSrcs; s++)
      for (uint32_t c = 0; c < R::kInCbs; c++)
        for (uint32_t g = 0; g < R::kSegs; g++)
          for (uint32_t r = 0; r < R::kRngs; r++) a += t.missedAttr[k][s][c][g][r];
  for (uint32_t i = 0; i <= R::kMaxGrants; i++) gsum += t.missedGrant[i];
  // (memres3: the attributes and grants count the page-only decisions too, rng same = exactly them)
  const uint64_t po = O(t, R::Outcome::kPageOnly);
  uint64_t same = 0;
  for (uint32_t k = 0; k < R::kKinds; k++)
    for (uint32_t s = 0; s < R::kSrcs; s++)
      for (uint32_t c = 0; c < R::kInCbs; c++)
        for (uint32_t g = 0; g < R::kSegs; g++) same += t.missedAttr[k][s][c][g][uint32_t(R::Rng::kSame)];
  return a == missed + po && gsum == missed + po && same == po &&
         missed == t.missedWhy[0] + t.missedWhy[1] + t.missedWhy[2] + t.missedWhy[3];
}
// The misses with one attribute value (summed over the others).
uint64_t attrCb(const R::Totals& t, R::InCb cb) {
  uint64_t v = 0;
  for (uint32_t k = 0; k < R::kKinds; k++)
    for (uint32_t s = 0; s < R::kSrcs; s++)
      for (uint32_t g = 0; g < R::kSegs; g++)
        for (uint32_t r = 0; r < R::kRngs; r++) v += t.missedAttr[k][s][uint32_t(cb)][g][r];
  return v;
}
uint64_t attrSeg(const R::Totals& t, R::Seg sg) {
  uint64_t v = 0;
  for (uint32_t k = 0; k < R::kKinds; k++)
    for (uint32_t s = 0; s < R::kSrcs; s++)
      for (uint32_t c = 0; c < R::kInCbs; c++)
        for (uint32_t r = 0; r < R::kRngs; r++) v += t.missedAttr[k][s][c][uint32_t(sg)][r];
  return v;
}
uint64_t attrRng(const R::Totals& t, R::Rng rg) {
  uint64_t v = 0;
  for (uint32_t k = 0; k < R::kKinds; k++)
    for (uint32_t s = 0; s < R::kSrcs; s++)
      for (uint32_t c = 0; c < R::kInCbs; c++)
        for (uint32_t g = 0; g < R::kSegs; g++) v += t.missedAttr[k][s][c][g][uint32_t(rg)];
  return v;
}

// The guest-memory view of the census's source-window comparison: one page, its 0x8 and its 0xA bytes (memres6: and a
// second page, the world family's index stream).
uint32_t g_hostPage = 0xFFFFFFFFu, g_streamPage = 0xFFFFFFFFu;
std::vector<uint8_t> g_win8(4096, 0), g_winA(4096, 0), g_str8(4096, 0), g_strA(4096, 0);
const uint8_t* testHost(uint32_t va) {
  const uint32_t page = (va & 0x1FFFFFFFu) >> 12;
  if (page == g_streamPage) return (va >> 28) == 0xAu ? g_strA.data() : g_str8.data();
  if (page != g_hostPage) return nullptr;
  return (va >> 28) == 0xAu ? g_winA.data() : g_win8.data();
}
uint32_t g_ownerVer = 5;
uint32_t testOwnerVer(uint32_t) { return g_ownerVer; }
// The traps' protection, recorded (no guest arena here): the last page protected / released and the counts.
uint32_t g_protPage = 0xFFFFFFFFu, g_relPage = 0xFFFFFFFFu, g_protN = 0, g_relN = 0;
bool g_protOk = true;
bool testProtect(uint32_t page, bool readOnly) {
  if (readOnly) {
    g_protPage = page;
    g_protN++;
    return g_protOk;
  }
  g_relPage = page;
  g_relN++;
  return true;
}

// A 2D k_8_8_8_8 texture fetch constant: base page, w x h, pitch (texels), mip page and mip_max_level (0 = none).
void tex2d(uint32_t f[6], uint32_t basePage, uint32_t w, uint32_t h, uint32_t pitch, uint32_t mipPage, uint32_t mipMax) {
  std::memset(f, 0, 6 * sizeof(uint32_t));
  f[0] = 2u | ((pitch / 32u) << 22);       // type texture, pitch in 32-texel units, linear
  f[1] = 6u | (basePage << 12);           // k_8_8_8_8
  f[2] = (w - 1u) | ((h - 1u) << 13);
  f[4] = mipMax << 6;
  f[5] = (mipPage << 12) | (1u << 9);     // dimension 2D
}

void registryTests() {
  std::printf("-- the registry\n");
  R::Registry reg;
  const uint32_t P = 0x1234;  // a page
  check(reg.decide(P) == R::Decision::kUploadFirst, "R1 a never-noted page's first ensure: a first-use upload");
  check(reg.decide(P) == R::Decision::kUploadFirst, "R1 decide() alone changes nothing");
  check(reg.ensurePage(P) == R::Decision::kUploadFirst && reg.uploadedGen(P) == R::kFirstUseGen,
        "R1 ensurePage commits the first-use upload (uploadedGen = kFirstUseGen)");
  check(reg.ensurePage(P) == R::Decision::kKeep, "R1 then current until a note");
  const uint32_t c0 = reg.clock();
  check(reg.noteCpuWrite(P << 12, 4096, R::Src::kInval) == 1u, "R2 a one-page note covers one page");
  check(reg.clock() == c0 + 1u && reg.cpuGen(P) == c0 + 1u, "R2 the note takes the next clock value");
  check(reg.lastSrc(P) == R::Src::kInval, "R2 the note's source is remembered");
  check(reg.ensurePage(P) == R::Decision::kUploadNoted, "R2 a note since the upload: a noted upload");
  check(reg.ensurePage(P) == R::Decision::kKeep, "R2 then current again");
  reg.noteGpuWrite(P << 12, 4096);
  check(reg.ensurePage(P) == R::Decision::kKeepGpu, "R3 a GPU write after the CPU note: the arena holds the GPU's data");
  check(reg.lastSrc(P) == R::Src::kInval, "R3 a GPU write does not replace the CPU note's source");
  reg.noteCpuWrite(P << 12, 4096, R::Src::kTexLock);
  check(reg.ensurePage(P) == R::Decision::kUploadNoted, "R3 a CPU note after the GPU write: upload (the CPU rewrote it)");
  check(reg.lastSrc(P) == R::Src::kTexLock, "R3 the newest note's source");
  const uint32_t Q = 0x2000;
  reg.noteGpuWrite(Q << 12, 100);
  check(reg.ensurePage(Q) == R::Decision::kKeepGpu, "R4 a resolve destination never uploaded: keep (no first-use clobber)");
  // R5: the page range of a byte range.
  reg.noteCpuWrite(0x0001FFFu, 2u, R::Src::kReadFile);
  check(reg.cpuGen(1) != 0 && reg.cpuGen(2) != 0 && reg.cpuGen(0) == 0 && reg.cpuGen(3) == 0,
        "R5 [0x1FFF, +2) notes pages 1 and 2 only");
  // R6: refused and clamped ranges.
  const uint64_t ref0 = reg.refused();
  check(reg.noteCpuWrite(0xFFFFF000u, 0x1000u, R::Src::kInval) == 0u && reg.refused() == ref0 + 1u,
        "R6 the all-ones sentinel {0xFFFFF000, 0x1000} is refused and counted");
  check(reg.noteCpuWrite(0x1FFFF000u, 0x3000u, R::Src::kInval) == 1u && reg.cpuGen(R::kPages - 1u) != 0u,
        "R6 a range crossing 512 MiB is clamped to the last page");
  check(reg.noteCpuWrite(0x5000u, 0u, R::Src::kInval) == 0u && reg.refused() == ref0 + 1u,
        "R6 an empty range notes nothing and is not a refusal");
  // R7: the version.
  const uint32_t V = 0x3000;
  check(reg.version(V << 12, 8192) == 0u, "R7 untouched pages: version 0");
  reg.noteCpuWrite((V + 1u) << 12, 4096, R::Src::kUnlock);
  const uint32_t vc = reg.cpuGen(V + 1u);
  reg.noteGpuWrite(V << 12, 4096);
  const uint32_t vg = reg.gpuGen(V);
  check(vg > vc && reg.version(V << 12, 8192) == vg, "R7 version = the newest cpu / gpu generation over the range");
  check(reg.version((V + 1u) << 12, 4096) == vc, "R7 per page");
  // R8: noted before its first use.
  const uint32_t N = 0x4000;
  reg.noteCpuWrite(N << 12, 4096, R::Src::kReadFile);
  check(reg.ensurePage(N) == R::Decision::kUploadNoted, "R8 noted before its first use: a noted upload, not a first-use one");
  // R10 (Astra P2): the decision binds to the generations read BEFORE the bytes; a note landing after them stays pending.
  const uint32_t B = 0x4100;
  reg.ensurePage(B);  // first use
  const uint32_t cRead = reg.cpuGen(B), gRead = reg.gpuGen(B);
  reg.noteCpuWrite(B << 12, 4096, R::Src::kUnlock);  // lands between the read and the copy
  check(reg.ensureAt(B, cRead, gRead) == R::Decision::kKeep && reg.uploadedGen(B) == R::kFirstUseGen,
        "R10 ensureAt on the generations read before the note: keep, nothing committed");
  check(reg.ensurePage(B) == R::Decision::kUploadNoted, "R10 ... so the next ensure uploads the noted write");
  const uint32_t D = 0x4200;
  reg.ensurePage(D);
  reg.noteCpuWrite(D << 12, 4096, R::Src::kInval);
  const uint32_t cD = reg.cpuGen(D);
  reg.noteCpuWrite(D << 12, 4096, R::Src::kInval);  // a second note after the read
  check(reg.ensureAt(D, cD, 0) == R::Decision::kUploadNoted && reg.uploadedGen(D) == cD,
        "R10 an upload commits the generation the copied bytes hold (the read one), not the newest");
  check(reg.ensurePage(D) == R::Decision::kUploadNoted, "R10 ... the newer note stays pending");
  // R9: reset.
  reg.reset();
  check(reg.cpuGen(P) == 0u && reg.uploadedGen(P) == 0u && reg.gpuGen(Q) == 0u && reg.clock() == R::kFirstUseGen,
        "R9 reset: every page never noted, the clock at its start");
}

// ---- memres6: the world family's computed indices (C41-C46) ----
// The family's real program: data/shaders_carved/vs_115dacfaba6d2c0f_synth.xpu, its ucode (XXH3 31168A8B9ADBEEFA, the
// shader cache's key). CF0's six ALU instructions are the prologue (archive/memres/vs_dump.py):
//   r0.z = r0.x + c255.x | r0.y = rcp(c4.x);  r0.y = r0.z * r0.y;  r0.y = trunc(r0.y) (truncs);
//   r0.x = -r0.y * c4.x + r0.x (mad);  r0.x = c4.y + r0.x (addsc0);  r0.x = trunc(r0.x) (truncs)
// - the vertex index modulo the instance's vertex count c4.x, plus c4.y; c255 is a literal of the container (0.5, 0,
// 0, 0), c4 its named instance_data - then CF1: r0.x <- vf94[r0.x] (FMT_32 integer, stride 1: the index stream) and
// three fetches of vf95 (stride 9: dwords 0..9) by that r0.x.
const uint32_t kWorld[123] = {
    0x00006005u, 0x400B1200u, 0x1200F05Du, 0x00000000u, 0x600FC200u, 0x12000000u, 0x00004015u, 0x00001200u,
    0xC4000000u, 0x00006019u, 0x601F1200u, 0x12000000u, 0x00003025u, 0x00002200u, 0x00000000u, 0x4C240000u,
    0x006C6C6Cu, 0x8000FF04u, 0xC8020000u, 0x00C6B100u, 0xC1000000u, 0x30200000u, 0x000000B1u, 0xE2000000u,
    0xC8010000u, 0x04B16C6Cu, 0xAB000400u, 0xB0100000u, 0x00000080u, 0xC2000004u, 0x30100000u, 0x0000006Cu,
    0xE2000000u, 0x03F80000u, 0x00212FF8u, 0x00000001u, 0x1DF84000u, 0x00393E88u, 0x00000009u, 0x05F82000u,
    0x40071E88u, 0x00000309u, 0x05F83000u, 0x00191FC1u, 0x00000809u, 0x00100000u, 0x000000B1u, 0xE2000000u,
    0x5C080001u, 0x006CB16Cu, 0xA680FF00u, 0xC8040001u, 0xA0B1B100u, 0x80046400u, 0xC8020000u, 0xA0C4C4B1u,
    0x910465FFu, 0xC8040000u, 0xA0C46DB1u, 0x910465FFu, 0xC8030001u, 0xA06EB200u, 0x40640000u, 0xC801803Eu,
    0x003E0000u, 0x4F000100u, 0xC802803Eu, 0x003E0000u, 0x4F010100u, 0xC804803Eu, 0x003E0000u, 0x4F020100u,
    0xC808803Eu, 0x003E0000u, 0x4F030100u, 0x5C000000u, 0x0000006Cu, 0xE2000000u, 0xC8010000u, 0xA0C4C4B1u,
    0x516502FFu, 0xC8020000u, 0xA06DC4B1u, 0x516502FFu, 0x14400300u, 0xA000001Bu, 0xC2000064u, 0xC8070001u,
    0x04CDB400u, 0x8001E500u, 0xC8078000u, 0x00C0C000u, 0xC2030300u, 0xC8080000u, 0x00CDCD00u, 0xD0010100u,
    0x58840000u, 0x006CB11Bu, 0xA680FF80u, 0xC8070001u, 0x00621B00u, 0xC1010000u, 0xC8080001u, 0x006CB100u,
    0xC1010200u, 0xC8080000u, 0x0015CD00u, 0xD0010000u, 0x16100000u, 0x0100001Bu, 0xE2000000u, 0x16170100u,
    0x006CC01Bu, 0xA100E700u, 0xC8070000u, 0x006CC0C0u, 0xAB01E600u, 0xC80F8001u, 0x00000000u, 0xC2000000u,
    0x00000000u, 0x00000000u, 0x00000000u,
};
std::vector<uint8_t> beBytes(const uint32_t* w, size_t n) {
  std::vector<uint8_t> b;
  for (size_t i = 0; i < n; i++) {
    b.push_back(uint8_t(w[i] >> 24));
    b.push_back(uint8_t(w[i] >> 16));
    b.push_back(uint8_t(w[i] >> 8));
    b.push_back(uint8_t(w[i]));
  }
  return b;
}
// The world program with dwords patched (instruction k's dwords are 3k .. 3k + 2).
std::vector<uint8_t> worldWith(std::initializer_list<std::pair<uint32_t, uint32_t>> patch) {
  std::vector<uint32_t> w(kWorld, kWorld + 123);
  for (const auto& p : patch) w[p.first] = p.second;
  return beBytes(w.data(), w.size());
}
// memres6's synthetic programs (the fail-closed cases a patch of the real one cannot express): an ALU instruction (Xenia
// AluInstruction, as the census decodes it) - the vector op `vop` writing r<vd>.<vmask> from (s1, s2, s3) and the
// scalar op `sop` writing r<sd>.<smask> from s3 (50 retain_prev: none). An operand: a temp r<reg> or a constant c<reg>,
// the components it reads for the result's x..w ("x" = xxxx), negated or not.
struct Opd {
  uint32_t reg;
  bool temp;
  uint32_t swz;
  bool neg;
};
uint32_t compOf(char c) { return c == 'y' ? 1u : c == 'z' ? 2u : c == 'w' ? 3u : 0u; }
uint32_t vSwz(const char* c) {
  const size_t n = std::strlen(c);
  uint32_t s = 0;
  for (uint32_t i = 0; i < 4; i++) s |= ((compOf(c[i < n ? i : n - 1]) - i) & 3u) << (2u * i);
  return s;
}
Opd Rg(uint32_t r, const char* c = "x", bool neg = false) { return Opd{r, true, vSwz(c), neg}; }
Opd Ck(uint32_t r, const char* c = "x", bool neg = false) { return Opd{r, false, vSwz(c), neg}; }
Ins alu(uint32_t vop, uint32_t vd, uint32_t vmask, Opd s1, Opd s2, Opd s3, uint32_t sop = 50, uint32_t sd = 0,
        uint32_t smask = 0) {
  const uint32_t d0 = vd | (sd << 8) | (vmask << 16) | (smask << 20) | (sop << 26);
  const uint32_t d1 = s3.swz | (s2.swz << 8) | (s1.swz << 16) | (uint32_t(s3.neg) << 24) | (uint32_t(s2.neg) << 25) |
                      (uint32_t(s1.neg) << 26);
  const uint32_t d2 = s3.reg | (s2.reg << 8) | (s1.reg << 16) | (vop << 24) | (uint32_t(s3.temp) << 29) |
                      (uint32_t(s2.temp) << 30) | (uint32_t(s1.temp) << 31);
  return Ins{false, d0, d1, d2};
}
// The real program's three fetches as instructions (dwords 33-41: F1, the first vf95 full fetch, its mini fetch).
Ins worldIns(uint32_t k) { return Ins{true, kWorld[3 * k], kWorld[3 * k + 1], kWorld[3 * k + 2]}; }
Ins worldAlu(uint32_t k) { return Ins{false, kWorld[3 * k], kWorld[3 * k + 1], kWorld[3 * k + 2]}; }
// execs of at most six instructions (the last an exec_end)
std::vector<uint8_t> vsExecs(const std::vector<Ins>& ins) {
  std::vector<Cf> cfs;
  for (size_t i = 0; i < ins.size(); i += 6) {
    const size_t e = std::min(ins.size(), i + 6);
    cfs.push_back(Cf{e == ins.size() ? 2u : 1u, std::vector<Ins>(ins.begin() + long(i), ins.begin() + long(e))});
  }
  return vsProgram(cfs);
}

// The host translation's index for vertex v (float32, each step rounded once, rcp clamped to [FLT_MIN, FLT_MAX]):
// trunc(c4.y + (-trunc((v + c255.x) * rcp(c4.x)) * c4.x + v)); INT64_MIN when the conversion cannot form one.
float rcpHost(float x) {
  const float r = 1.0f / x;
  if (r != r) return r;
  return r < FLT_MIN ? FLT_MIN : r > FLT_MAX ? FLT_MAX : r;
}
int64_t worldIdx(uint32_t v, float n, float lit, float b) {
  volatile float t1 = float(v) + lit;
  volatile float t2 = rcpHost(n);
  volatile float t3 = t1 * t2;
  volatile float t4 = std::trunc(t3);
  volatile float m = -t4 * n;
  volatile float t5 = m + float(v);
  volatile float t6 = b + t5;
  const float t7 = std::trunc(t6);
  if (!(std::fabs(t7) <= 16777216.0f)) return INT64_MIN;
  return int64_t(std::floor(t7));
}
uint32_t fbits(float x) {
  uint32_t u;
  std::memcpy(&u, &x, 4);
  return u;
}
float fl(uint32_t u) {
  float x;
  std::memcpy(&x, &u, 4);
  return x;
}
// A draw's constants and fetch constants: c4 = (vertex count, base, 0, 0), c255 as the constant file holds it; the
// index stream (slot 94) on the stream page at +0x100, endian 2 (8-in-32: big-endian dwords).
constexpr uint32_t kStreamPage = 0x1F00u, kStreamBase = (kStreamPage << 12) + 0x100u;
const uint32_t kStream[16] = {20, 7, 13, 9, 4, 30, 2, 17, 11, 5, 8, 1, 14, 3, 6, 10};
struct Ctx {
  std::vector<uint32_t> c = std::vector<uint32_t>(1024, 0u), f = std::vector<uint32_t>(192, 0u);
};
Ctx ctx(float n, float b = 0.0f, float c255x = 0.5f, uint32_t streamDw = 64, uint32_t base = kStreamBase) {
  Ctx x;
  x.c[16] = fbits(n);
  x.c[17] = fbits(b);
  x.c[255 * 4] = fbits(c255x);
  x.f[94 * 2] = base | 3u;
  x.f[94 * 2 + 1] = 2u | (streamDw << 2);
  return x;
}
void putBE(std::vector<uint8_t>& b, uint32_t off, uint32_t v) {
  b[off] = uint8_t(v >> 24);
  b[off + 1] = uint8_t(v >> 16);
  b[off + 2] = uint8_t(v >> 8);
  b[off + 3] = uint8_t(v);
}
void streamReset() {
  g_streamPage = kStreamPage;
  std::memset(g_str8.data(), 0, 4096);
  std::memset(g_strA.data(), 0, 4096);
  for (uint32_t i = 0; i < 16; i++) {
    putBE(g_str8, 0x100u + 4u * i, kStream[i]);
    putBE(g_strA, 0x100u + 4u * i, kStream[i]);
    putBE(g_str8, 0x200u + 4u * i, 0x02000000u + i);  // a second stream: values beyond 2^24
    putBE(g_strA, 0x200u + 4u * i, 0x02000000u + i);
  }
}
// The census's bound of a slot in a draw (its first computed request evaluates it; the witness runs as it would).
struct Bnd {
  bool ok;
  int64_t lo, hi;
  uint32_t why;
};
Bnd bound(uint64_t vs, const Ctx& x, uint32_t vMin, uint32_t vMax, uint32_t slot, bool state = true) {
  Bnd b{false, 0, 0, 99};
  const R::DrawScope d(vs, true, true, vMin, vMax, state ? x.c.data() : nullptr, state ? x.f.data() : nullptr);
  b.ok = R::censusSlotBoundForTest(slot, b.lo, b.hi, &b.why);
  return b;
}
// The exact index sets over the draw, as the host computes them: slot 94's, and slot 95's - the stream's value at
// slot 94's index, in either window (the bound must hold both: the GPU's copy is one of them).
struct Ex {
  bool ok;
  int64_t lo94, hi94, lo95, hi95;
};
Ex exact(const Ctx& x, uint32_t vMin, uint32_t vMax, float lit) {
  Ex e{true, INT64_MAX, INT64_MIN, INT64_MAX, INT64_MIN};
  for (uint64_t v = vMin; v <= vMax; v++) {
    const int64_t i = worldIdx(uint32_t(v), fl(x.c[16]), lit, fl(x.c[17]));
    if (i < 0 || i >= 16) {
      e.ok = false;
      continue;
    }
    e.lo94 = std::min(e.lo94, i);
    e.hi94 = std::max(e.hi94, i);
    for (const std::vector<uint8_t>* w : {&g_str8, &g_strA}) {
      const uint8_t* p = w->data() + 0x100u + 4u * uint32_t(i);
      const int64_t s = (int64_t(p[0]) << 24) | (int64_t(p[1]) << 16) | (int64_t(p[2]) << 8) | int64_t(p[3]);
      e.lo95 = std::min(e.lo95, s);
      e.hi95 = std::max(e.hi95, s);
    }
  }
  return e;
}
// The fallback reasons (gfx/resource_census.cpp kFb*, the order of census_vsfb's r.* keys).
enum : uint32_t { kRSlot = 0, kROp, kRRel, kRCtl, kRFmt, kRRead, kRMany, kRRange, kRState, kRClob, kRLong, kRVs, kRLit };
// The test's literal table (gfx/resource_census_literals.inc's rows for the test's hashes; sorted by hash, register).
std::vector<R::LiteralRow> g_lits;
void lit(uint64_t h, uint32_t reg, uint32_t flags, float x) {
  g_lits.push_back(R::LiteralRow{h, reg, flags, {fbits(x), 0u, 0u, 0u}});
}
void known(uint64_t h) { g_lits.push_back(R::LiteralRow{h, 0xFFFFFFFFu, 0u, {0u, 0u, 0u, 0u}}); }

void familyTests() {
  const uint64_t HA = 0x31168A8B9ADBEEFAull;  // the real hash: c255 = 0.5 (the corpus's literal)
  const uint64_t HB = 0x6E6E000000000001ull, HC = 0x6E6E000000000002ull, HD = 0x6E6E000000000003ull,
                 HE = 0x6E6E000000000004ull, HF = 0x6E6E000000000005ull, HL1 = 0x6E6E000000000006ull,
                 HL2 = 0x6E6E000000000007ull, HOP = 0x6E6E000000000010ull, HREL = 0x6E6E000000000011ull,
                 HCTL = 0x6E6E000000000012ull, HCTL2 = 0x6E6E000000000013ull, HFMT = 0x6E6E000000000014ull,
                 HCLB = 0x6E6E000000000015ull, HCLB2 = 0x6E6E000000000016ull, HLONG = 0x6E6E000000000017ull,
                 HNONE = 0x6E6E0000000000FFull;
  const float kUlp24 = -5.9604644775390625e-8f, kUlp25 = -2.98023223876953125e-8f;  // -2^-24, -2^-25
  g_lits.clear();
  lit(HA, 255, 0, 0.5f), known(HA);
  lit(HB, 255, 0, 0.0f), known(HB);
  lit(HC, 255, 0, kUlp24), known(HC);
  lit(HD, 255, 0, kUlp25), known(HD);
  lit(HE, 255, 0, -0.75f), known(HE);
  known(HF);                                // c255 read live (no literal row)
  lit(HL1, 255, 1, 0.0f), known(HL1);       // defined two ways in the corpus
  for (uint64_t h : {HOP, HREL, HCTL, HCTL2, HFMT, HCLB, HCLB2, HLONG}) known(h);
  // (HL2: no row - its container was never read)
  std::sort(g_lits.begin(), g_lits.end(), [](const R::LiteralRow& a, const R::LiteralRow& b) {
    return a.hash != b.hash ? a.hash < b.hash : a.reg < b.reg;
  });
  R::censusSetLiteralsForTest(g_lits.data(), g_lits.size());
  R::censusSetWitnessKForTest(1);  // every computed-bound draw evaluated exactly (the census: the first 256, then 1 in 64)
  const std::vector<uint8_t> world = beBytes(kWorld, 123);
  for (uint64_t h : {HA, HB, HC, HD, HE, HF, HL1, HL2}) R::noteVsUcode(h, world.data(), world.size());
  streamReset();
  const R::Totals& w0 = snapA();
  const uint64_t sampled0 = w0.boundSampled, viol0 = w0.boundViolations;

  // C41: the two levels. c4 = (4, 0): vertices 0..11 are three instances of 4; slot 94 indexes 0..3 (the prologue,
  // exact on integers), slot 95 the stream's values there {20, 7, 13, 9}.
  {
    const Ctx x = ctx(4.0f);
    const Ex e = exact(x, 0, 11, 0.5f);
    const Bnd b94 = bound(HA, x, 0, 11, 94), b95 = bound(HA, x, 0, 11, 95);
    check(e.ok && e.lo94 == 0 && e.hi94 == 3 && b94.ok && b94.lo == 0 && b94.hi == 3,
          "C41 slot 94 (the index stream) bounded through the prologue per vertex: [0, 3], the exact set's range");
    check(e.lo95 == 7 && e.hi95 == 20 && b95.ok && b95.lo == 7 && b95.hi == 20,
          "C41 slot 95 bounded through the stream's values at slot 94's index set: [7, 20]");
    // a request of slot 95 over a page's declared [0, 0x800): its fetched dwords are vertices 7..20 of 9 dwords
    const uint32_t P = 0x1F10u;
    g_hostPage = P;
    std::memset(g_win8.data(), 0x70, 4096);
    std::memset(g_winA.data(), 0x70, 4096);
    const R::Totals& a = snapB();
    {
      const R::KindScope k(R::Kind::kVb);
      const R::DrawScope d(HA, true, true, 0, 11, x.c.data(), x.f.data());
      const R::SlotScope sl(95);
      R::WalkScope r(1, false, P << 12, 0x800u);
      R::candPage(P, R::Site::kDrain, 60, true);
    }
    R::modelPage(P, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& b = snapA();
    check(b.fetchBounded - a.fetchBounded == 1 && b.fetchBoundedComp - a.fetchBoundedComp == 1 &&
              b.fetchFbShader == a.fetchFbShader,
          "C41 the request is bounded through the computed index (fetch.bounded.computed)");
    g_win8[0x2F4] = 0x11;  // the dword past vertex 20's last (20 * 9 + 9 = 189 dwords: 0x2F4)
    g_win8[0xF8] = 0x12;   // the dword before vertex 7's first (7 * 9 = 63: 0xFC)
    R::modelPage(P, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& c = snapB();
    check(O(c, R::Outcome::kPageOnly) - O(b, R::Outcome::kPageOnly) == 1 &&
              O(c, R::Outcome::kMissed) == O(b, R::Outcome::kMissed),
          "C41 writes beside the bounded vertices (inside the declared range): PAGE_ONLY");
    g_win8[0x200] = 0x13;  // vertex 14
    R::modelPage(P, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& d = snapA();
    const std::string m = R::censusLastSampleForTest(0);
    check(O(d, R::Outcome::kMissed) - O(c, R::Outcome::kMissed) == 1 &&
              m.find(" reqs=01F100FC+1F8:vb/b") != std::string::npos && m.find(" via=b") != std::string::npos,
          "C41 a write to a bounded vertex: MISSED, its range a bound's (/b, via=b): " + m);
    // the same draw's request over a declared range of 0x200 bytes: the bound reaches its end (clamped: /c, via=c)
    const uint32_t P2 = 0x1F18u;
    g_hostPage = P2;
    std::memset(g_win8.data(), 0x71, 4096);
    std::memset(g_winA.data(), 0x71, 4096);
    {
      const R::KindScope k(R::Kind::kVb);
      const R::DrawScope dd(HA, true, true, 0, 11, x.c.data(), x.f.data());
      const R::SlotScope sl(95);
      R::WalkScope r(1, false, P2 << 12, 0x200u);
      R::candPage(P2, R::Site::kDrain, 62, true);
    }
    R::modelPage(P2, true, R::Why::kContent, R::Prov::k8);
    g_win8[0x1F0] = 0x14;
    R::modelPage(P2, true, R::Why::kContent, R::Prov::k8);
    const std::string mc = R::censusLastSampleForTest(0);
    check(mc.find(" reqs=01F180FC+104:vb/c") != std::string::npos && mc.find(" via=c") != std::string::npos,
          "C41 a bound clamped to the declared end: /c, via=c: " + mc);
    // C55 (mem-native): an indexed draw's COMPUTED slot keeps its bound - the used-vertex runs are r0.x's vertices, not
    // the computed index's: indices {0, 11} (vMin 0, vMax 11 as above), slot 95's bound [7, 20] is still fetched
    const uint32_t P3 = 0x1F1Cu;
    g_hostPage = P3;
    std::memset(g_win8.data(), 0x72, 4096);
    std::memset(g_winA.data(), 0x72, 4096);
    {
      const std::vector<uint16_t> ends = {0, 11};
      const R::KindScope k(R::Kind::kVb);
      const R::DrawScope dd(HA, true, reinterpret_cast<const uint8_t*>(ends.data()), 2, false, 0, 0, false, 0, 0xFFFFFF,
                            false, 0xFFFF, x.c.data(), x.f.data());
      const R::SlotScope sl(95);
      R::WalkScope r(1, false, P3 << 12, 0x800u);
      R::candPage(P3, R::Site::kDrain, 63, true);
    }
    R::modelPage(P3, true, R::Why::kContent, R::Prov::k8);
    const uint64_t cm0 = O(snapB(), R::Outcome::kMissed);
    g_win8[0x200] = 0x15;  // vertex 14 of slot 95's bound
    R::modelPage(P3, true, R::Why::kContent, R::Prov::k8);
    check(O(snapB(), R::Outcome::kMissed) - cm0 == 1,
          "C55 an indexed draw's computed slot keeps its bound [7, 20]: vertex 14 fetched, the write MISSED");
    // slot 94's own request: the stream's dwords at its index set, 0..3
    {
      const R::KindScope k(R::Kind::kVb);
      const R::DrawScope dd(HA, true, true, 0, 11, x.c.data(), x.f.data());
      const R::SlotScope sl(94);
      R::WalkScope r(1, false, kStreamBase, 0x100u);
      R::candPage(kStreamPage, R::Site::kDrain, 61, true);
    }
    R::modelPage(kStreamPage, true, R::Why::kContent, R::Prov::k8);
    const uint64_t po1 = O(snapB(), R::Outcome::kPageOnly);
    g_str8[0x113] ^= 1u;  // stream index 4: not fetched
    R::modelPage(kStreamPage, true, R::Why::kContent, R::Prov::k8);
    const uint64_t po2 = O(snapB(), R::Outcome::kPageOnly), mi2 = O(snapB(), R::Outcome::kMissed);
    g_str8[0x10F] ^= 1u;  // stream index 3
    R::modelPage(kStreamPage, true, R::Why::kContent, R::Prov::k8);
    const uint64_t mi3 = O(snapB(), R::Outcome::kMissed);
    check(po2 - po1 == 1 && mi3 - mi2 == 1, "C41 the stream's request: index 4 (past the set) PAGE_ONLY, index 3 MISSED");
    streamReset();
    g_hostPage = 0xFFFFFFFFu;
  }
  // C42: the stream's 0xA copy differs (index 2 holds 30 there): the bound holds both windows' values.
  {
    putBE(g_strA, 0x108u, 30u);
    const Ctx x = ctx(4.0f);
    const Ex e = exact(x, 0, 11, 0.5f);
    const Bnd b = bound(HA, x, 0, 11, 95);
    check(e.hi95 == 30 && b.ok && b.lo == 7 && b.hi == 30, "C42 the stream read in both windows: slot 95 [7, 30]");
    streamReset();
  }
  // C43: adversarial constants - each draw's exact set inside the census's bound (and the witness's own check).
  {
    struct Adv {
      const char* name;
      uint64_t vs;
      float lit, n, b;
      uint32_t vMin, vMax;
      int64_t lo94, hi94;  // the exact range expected (the test's own arithmetic, cross-checked)
    };
    const Adv adv[] = {
        {"c4.x = 1, the quotient exactly an integer (5 / 1): trunc on both sides", HB, 0.0f, 1.0f, 0.0f, 5, 5, 0, 0},
        {"c4.x = 1, the quotient one ulp below an integer ((1 - 2^-24) / 1)", HC, kUlp24, 1.0f, 0.0f, 1, 1, 1, 1},
        {"c4.x = 1, a float sum on a tie (1 - 2^-25 rounds to 1: the interval widens)", HD, kUlp25, 1.0f, 0.0f, 1, 1,
         0, 0},
        {"c4.x non-integer (2.5)", HA, 0.5f, 2.5f, 0.0f, 0, 9, 0, 1},
        {"c4.x non-integer (2.5), the vertex whose quotient rounds to 1", HA, 0.5f, 2.5f, 0.0f, 2, 2, 0, 0},
        {"c4.x large (1e30): no wrap, the last vertex the largest index", HA, 0.5f, 1e30f, 0.0f, 0, 9, 0, 9},
        {"c4.x negative (-4): rcp clamped to FLT_MIN", HA, 0.5f, -4.0f, 0.0f, 9, 9, 9, 9},
        {"c4.y a base (3)", HA, 0.5f, 4.0f, 3.0f, 0, 7, 3, 6},
        {"c255 a literal other than the constant file's (-0.75 baked in, the file holds 0.5)", HE, -0.75f, 4.0f, 0.0f,
         0, 11, 0, 4},
        {"c255 read live (no literal: the file's 0.5)", HF, 0.5f, 4.0f, 0.0f, 0, 11, 0, 3},
    };
    for (const Adv& k : adv) {
      const Ctx x = ctx(k.n, k.b, 0.5f);
      const Ex e = exact(x, k.vMin, k.vMax, k.lit);
      const Bnd b94 = bound(k.vs, x, k.vMin, k.vMax, 94), b95 = bound(k.vs, x, k.vMin, k.vMax, 95);
      char tail[160];
      std::snprintf(tail, sizeof tail, ": exact [%lld, %lld] / [%lld, %lld], bound [%lld, %lld] / [%lld, %lld]",
                    (long long)e.lo94, (long long)e.hi94, (long long)e.lo95, (long long)e.hi95, (long long)b94.lo,
                    (long long)b94.hi, (long long)b95.lo, (long long)b95.hi);
      check(e.ok && e.lo94 == k.lo94 && e.hi94 == k.hi94 && b94.ok && b95.ok && b94.lo <= e.lo94 &&
                e.hi94 <= b94.hi && b95.lo <= e.lo95 && e.hi95 <= b95.hi,
            std::string("C43 ") + k.name + tail);
    }
    // the bound is tight where the arithmetic is exact (the end's own vertex included)
    const Bnd big = bound(HA, ctx(1e30f), 0, 9, 94);
    check(big.ok && big.lo == 0 && big.hi == 9, "C43 c4.x large: slot 94 exactly [0, 9]");
  }
  // C44: the fail-closed reasons, one case each: the bound refused (its reason) and a request falls back counted by it.
  {
    std::vector<Ins> longProg;
    for (uint32_t i = 0; i < 49; i++) longProg.push_back(alu(0, 3, 1, Rg(3), Ck(1), Rg(3)));  // r3.x += c1.x
    for (uint32_t k = 5; k <= 10; k++) longProg.push_back(worldAlu(k));
    longProg.push_back(worldIns(11));
    longProg.push_back(worldIns(12));
    std::vector<Ins> clob;
    for (uint32_t k = 5; k <= 10; k++) clob.push_back(worldAlu(k));
    clob.push_back(worldIns(11));
    clob.push_back(alu(0, 0, 1, Rg(0), Ck(1), Rg(0)));  // r0.x += c1.x after the index fetch
    clob.push_back(worldIns(12));
    // instruction 7 (dwords 21-23, a scalar truncs) given a vector dp4 writing r0.x
    const std::vector<uint8_t> uOp =
        worldWith({{21, kWorld[21] | (1u << 16)}, {23, (kWorld[23] & ~(0x1Fu << 24)) | (15u << 24)}});
    const std::vector<uint8_t> uRel = worldWith({{16, kWorld[16] | (1u << 31)}});
    const std::vector<uint8_t> uCtl = worldWith({{16, kWorld[16] | (1u << 28)}});
    const std::vector<uint8_t> uCtl2 = worldWith({{1, (kWorld[1] & ~0xF000u) | 0x3000u}});  // CF0 a cond_exec
    const std::vector<uint8_t> uFmt = worldWith({{34, (kWorld[34] & ~(0x3Fu << 16)) | (36u << 16)}});  // F1 32_FLOAT
    const std::vector<uint8_t> uClb = worldWith({{34, kWorld[34] | 4u}});  // F1 writes x the constant 0
    const std::vector<uint8_t> uClb2 = vsExecs(clob), uLong = vsExecs(longProg);
    for (const auto& p : {std::make_pair(HOP, &uOp), std::make_pair(HREL, &uRel), std::make_pair(HCTL, &uCtl),
                          std::make_pair(HCTL2, &uCtl2), std::make_pair(HFMT, &uFmt), std::make_pair(HCLB, &uClb),
                          std::make_pair(HCLB2, &uClb2), std::make_pair(HLONG, &uLong)})
      R::noteVsUcode(p.first, p.second->data(), p.second->size());
    struct Fb {
      const char* name;
      uint64_t vs;
      Ctx x;
      uint32_t vMin, vMax, slot, reason;
      bool state;
    };
    const uint64_t HSTR2 = 0x5A5A00000000000Cull;  // C36's two strides for one slot (the direct walk's unsafe slot)
    const std::vector<uint8_t> uStr2 = vsUcode({vfetch(0, 4, 0, 38), vfetch(0, 8, 0, 38)});
    R::noteVsUcode(HSTR2, uStr2.data(), uStr2.size());
    const Fb fbs[] = {
        {"slot (the direct walk's unsafe slot: two strides)", HSTR2, ctx(4.0f), 0, 11, 0, kRSlot, true},
        {"op (a dp4 in the prologue)", HOP, ctx(4.0f), 0, 11, 95, kROp, true},
        {"rel (a relatively addressed constant in the prologue)", HREL, ctx(4.0f), 0, 11, 95, kRRel, true},
        {"ctl (a predicated prologue instruction)", HCTL, ctx(4.0f), 0, 11, 95, kRCtl, true},
        {"ctl (the prologue in a conditional exec)", HCTL2, ctx(4.0f), 0, 11, 94, kRCtl, true},
        {"fmt (the index stream 32_FLOAT: slot 95 only)", HFMT, ctx(4.0f), 0, 11, 95, kRFmt, true},
        {"read (the index stream unreadable in both windows)", HA, ctx(4.0f, 0.0f, 0.5f, 64, 0x1E000100u), 0, 11, 95,
         kRRead, true},
        {"many (65,537 vertices)", HA, ctx(4.0f), 0, 65536, 94, kRMany, true},
        {"range (c4.x = 0: rcp FLT_MAX, the quotient overflows)", HA, ctx(0.0f), 0, 3, 94, kRRange, true},
        {"range (slot 94's indices past its stream's two dwords)", HA, ctx(4.0f, 0.0f, 0.5f, 2), 0, 11, 95, kRRange,
         true},
        {"range (slot 95's stream values beyond 2^24: no index formed)", HA,
         ctx(4.0f, 0.0f, 0.5f, 64, (kStreamPage << 12) + 0x200u), 0, 11, 95, kRRange, true},
        {"state (no constants handed to the draw)", HA, ctx(4.0f), 0, 11, 94, kRState, false},
        {"clob (the index fetch writes x a constant)", HCLB, ctx(4.0f), 0, 11, 95, kRClob, true},
        {"clob (an ALU writes r0.x after the index fetch)", HCLB2, ctx(4.0f), 0, 11, 95, kRClob, true},
        {"long (a prologue of 55 instructions)", HLONG, ctx(4.0f), 0, 11, 94, kRLong, true},
        {"vs (a VS never loaded)", HNONE, ctx(4.0f), 0, 11, 94, kRVs, true},
        {"lit (its literal defined two ways in the corpus)", HL1, ctx(4.0f), 0, 11, 94, kRLit, true},
        {"lit (a computed VS whose container was never read)", HL2, ctx(4.0f), 0, 11, 94, kRLit, true},
    };
    for (const Fb& k : fbs) {
      int64_t lo = 0, hi = 0;
      uint32_t why = 99;
      bool ok;
      {
        const R::DrawScope d(k.vs, true, true, k.vMin, k.vMax, k.state ? k.x.c.data() : nullptr,
                             k.state ? k.x.f.data() : nullptr);
        ok = R::censusSlotBoundForTest(k.slot, lo, hi, &why);
      }
      const R::Totals& a = snapA();
      {
        const R::KindScope kd(R::Kind::kVb);
        const R::DrawScope d(k.vs, true, true, k.vMin, k.vMax, k.state ? k.x.c.data() : nullptr,
                             k.state ? k.x.f.data() : nullptr);
        const R::SlotScope sl(k.slot);
        R::WalkScope r(1, false, 0x1F20u << 12, 0x800u);
        R::skipDeferred(0x1F20u);
      }
      const R::Totals& b = snapB();
      char tail[96];
      std::snprintf(tail, sizeof tail, " (bound %d reason %u; fallback +%llu, reason +%llu)", ok ? 1 : 0, why,
                    (unsigned long long)(b.fetchFbShader - a.fetchFbShader),
                    (unsigned long long)(b.fbReason[k.reason] - a.fbReason[k.reason]));
      check(!ok && why == k.reason && b.fetchFbShader - a.fetchFbShader == 1 &&
                b.fbReason[k.reason] - a.fbReason[k.reason] == 1,
            std::string("C44 fail-closed, ") + k.name + tail);
    }
    // the fallbacks that leave the other level bounded: an unusable stream refuses slot 95 only
    check(bound(HFMT, ctx(4.0f), 0, 11, 94).ok && bound(HA, ctx(4.0f, 0.0f, 0.5f, 2), 0, 11, 94).ok &&
              bound(HA, ctx(4.0f, 0.0f, 0.5f, 64, 0x1E000100u), 0, 11, 94).ok,
          "C44 fmt / range / read of the stream leave slot 94's prologue bound");
    // census_vsfb: the fallbacks by reason (their sum fetch.fallback.shader), the VS with fallbacks; its _top line
    const std::string t = R::censusText("census", 12);
    const size_t v0 = t.find("[MEM_RES] census_vsfb census ");
    const std::string vl = v0 == std::string::npos ? "" : t.substr(v0, t.find('\n', v0) - v0);
    check(vl.find(" r.slot=1 r.op=1 r.rel=1 r.ctl=2 r.fmt=1 r.read=1 r.many=1 r.range=3 r.state=1 r.clob=2 r.long=1 "
                  "r.vs=1 r.lit=2 vs=13") != std::string::npos && snapA().fetchFbShader == 18,
          "C44 census_vsfb: each reason's count, 13 VS, 18 shader fallbacks: " + vl);
    check(t.find("[MEM_RES] census_vsfb_top census top=31168A8B9ADBEEFA:6:state,5A5A00000000000C:1:slot,") !=
              std::string::npos,
          "C44 census_vsfb_top: the VS with the most fallbacks first (hash:requests:its last reason)");
  }
  // C45: the soundness witness - every computed draw above evaluated exactly, none outside its bound; a bound narrowed
  // by one index (a test's) is caught, its BOUNDV sample names the slot.
  {
    const R::Totals& a = snapA();
    check(a.boundSampled - sampled0 >= 20 && a.boundViolations == viol0,
          "C45 the witness evaluated every computed draw (K = 1) and found no exact index outside its bound");
    R::censusSetBoundShrinkForTest(1);
    (void)bound(HA, ctx(4.0f), 0, 11, 94);
    R::censusSetBoundShrinkForTest(0);
    const R::Totals& b = snapB();
    check(b.boundSampled - a.boundSampled == 1 && b.boundViolations - a.boundViolations == 1,
          "C45 a bound one index short: the witness reports the draw (bound.check.violations)");
    const std::string t = R::censusText("census", 11);
    check(t.find(" bound.check.sampled=") != std::string::npos && t.find(" bound.check.violations=") != std::string::npos &&
              t.find(" fetch.bounded.computed=") != std::string::npos,
          "C45 the census line carries fetch.bounded.computed and bound.check.sampled / violations");
  }
  // C46 (memres6 (c), the keep's window): a legacy KEEP serves the model's resident copy - the window of its last
  // upload of the page - whatever its label (the legacy path labels every keep l8): memres5's Warworld ALIAS samples.
  {
    const uint32_t K1 = 0x1F30u, K2 = 0x1F40u;
    R::noteGrantWindow(0xA0000000u | (K1 << 12), 0x1000u);
    R::noteGrantWindow(0xA0000000u | (K2 << 12), 0x1000u);
    g_hostPage = K1;
    std::memset(g_win8.data(), 0, 4096);  // the title wrote through 0xA only
    std::memset(g_winA.data(), 0x64, 4096);
    const R::Totals& a = snapA();
    R::modelPage(K1, true, R::Why::kContent, R::Prov::kLegacyA);  // the model uploads from 0xA (0x8 zero)
    R::noteCoher(0x03000000u, K1 << 12, 0x1000u);
    R::modelPage(K1, false, R::Why::kContent, R::Prov::kLegacy8);  // ... and keeps it, labelled l8
    R::noteCoher(0x03000000u, K1 << 12, 0x1000u);
    R::modelPage(K1, false, R::Why::kContent, R::Prov::kLegacy8);  // a second keep
    const R::Totals& b = snapB();
    check(b.winAliasDiff == a.winAliasDiff && O(b, R::Outcome::kOver) - O(a, R::Outcome::kOver) == 2,
          "C46 keeps of a copy uploaded from 0xA, labelled l8: the model's window is 0xA, the registry's grant 0xA - "
          "no win.alias_diff");
    g_hostPage = K2;
    std::memset(g_win8.data(), 0x21, 4096);  // both windows hold bytes, different ones
    std::memset(g_winA.data(), 0x65, 4096);
    R::modelPage(K2, true, R::Why::kContent, R::Prov::kLegacy8);  // the model uploads from 0x8
    R::noteCoher(0x03000000u, K2 << 12, 0x1000u);
    R::modelPage(K2, false, R::Why::kContent, R::Prov::kLegacy8);
    const R::Totals& c = snapA();
    check(c.winAliasDiff - b.winAliasDiff == 2,
          "C46 a copy the model uploaded from 0x8 against the registry's 0xA: the upload and the keep both win.alias_diff");
    const std::string t = R::censusLastSampleForTest(2);
    check(t.find(" h8=") != std::string::npos && t.find(" hA=") != std::string::npos &&
              t.find(" mlast=8") != std::string::npos && t.find(" h8=Z") == std::string::npos,
          "C46 the ALIAS tail: each copy's hash (Z = zero) and the model's last upload window: " + t);
    g_hostPage = 0xFFFFFFFFu;
  }
  R::censusSetWitnessKForTest(64);
  g_streamPage = 0xFFFFFFFFu;
}

void warworldLiteralTest() {
  constexpr uint64_t hash = 0xDBE06113C69DA55Bull;
  const char* path = "data/shaders_carved/vs_dbe06113c69da55b_masked.xpu";
  // This exact previously translated container supplies the two baked literals;
  // a matching name alone is insufficient, so validate its actual ucode hash.
  std::vector<uint8_t> blob(3816);
  FILE* file = std::fopen(path, "rb");
  bool read = file && std::fread(blob.data(), 1, blob.size(), file) == blob.size();
  if (file) {
    read = read && std::fgetc(file) == EOF;
    std::fclose(file);
  }
  // This is one pinned fixture, not a container-parser test. Its recorded
  // virtual size (2912) plus physical ucode offset (64) gives this exact span.
  // Hash the bytes themselves so a replacement file cannot silently pass.
  constexpr size_t ucodeOffset = 2976, ucodeSize = 840;
  static_assert(ucodeOffset + ucodeSize == 3816);
  const uint8_t* ucode = blob.data() + ucodeOffset;
  const bool valid = read && XXH3_64bits(ucode, ucodeSize) == hash;
  check(valid, "C47 Warworld DBE06113 fixture is the validated exact runtime ucode");
  if (!valid) return;
  R::censusResetForTest(true);
  R::censusSetLiteralsForTest(nullptr, 0);
  streamReset();
  R::noteVsUcode(hash, ucode, ucodeSize);
  // Independent scalar evaluation of this actual program gives F1 indices 0..3
  // and stream values {20,7,13,9}. Live c254/c255 stay zero deliberately.
  const Ctx x = ctx(4.0f, 0.0f, 0.0f);
  const Bnd first = bound(hash, x, 0, 11, 94), second = bound(hash, x, 0, 11, 95);
  check(first.ok && first.lo == 0 && first.hi == 3 && second.ok && second.lo == 7 && second.hi == 20,
        "C47 exact DBE06113 bounds use generated container literals despite different live constants");
  size_t count = 0;
  const R::LiteralRow* rows = R::censusLiterals(count);
  std::vector<R::LiteralRow> missing;
  for (size_t i = 0; i < count; ++i)
    if (rows[i].hash != hash) missing.push_back(rows[i]);
  R::censusResetForTest(true);
  R::censusSetLiteralsForTest(missing.data(), missing.size());
  R::noteVsUcode(hash, ucode, ucodeSize);
  const Bnd refused = bound(hash, x, 0, 11, 95);
  check(!refused.ok && refused.why == kRLit,
        "C47 deleting DBE06113 corpus rows restores the actual Warworld lit fallback");
  R::censusSetLiteralsForTest(nullptr, 0);
}

void fetchPayloadTests() {
  std::printf("-- bounded full-page origin payload diagnostics\n");
  constexpr uint32_t page = 0x17166000;
  const uint64_t budget = R::FetchPayloadStore::indexBytes() + 2 * R::FetchPayloadStore::entryBytes();
  R::FetchPayloadStore store(budget);
  R::FetchPageVersion version;
  version.known = true; version.window = 0xA; version.cpu = 17; version.gpu = 9;
  version.uploaded = 17; version.ownerSum = 33; version.ownerSeq = 2; version.grantGeneration = 100;
  std::array<uint8_t, 4096> before{}, after{};
  std::array<uint64_t, 16> fetched{};
  std::array<uint64_t, 8> ring{};
  fetched[(0x218 / 4) >> 6] = uint64_t(1) << ((0x218 / 4) & 63);
  ring[3] = uint64_t(page + 0x218) << 32 | (4u << 4) | 1;
  auto missing = store.capture(page, before.data(), version, true, fetched);
  check(!missing->known && missing->reason == "baseline_missing", "C52 missing full-page baseline stays unknown");
  store.record(page, before.data(), version, true);
  after[0x218] = 1; after[0x603] = 2; after[4095] = 3;
  auto later = version; later.epoch = 99; later.grantGeneration = 105;
  const auto frozen = store.capture(page, after.data(), later, true, fetched, ring, ring[3], 4);
  check(frozen->known && frozen->before == before && frozen->after == after &&
        frozen->baseline.epoch == 0 && frozen->current.epoch == 99 &&
        frozen->baseline.grantGeneration == 100 && frozen->current.grantGeneration == 105 &&
        frozen->requestRing == ring && frozen->requestLast == ring[3] && frozen->requestHead == 4,
        "C52 immutable payload retains complete pages, full versions and actual request history");
  R::FetchAnalysis analysis; analysis.complete = true;
  analysis.uses = {{0, 0, 0, page + 0x600, 4}};
  auto joined = R::intersectFetchPayload(*frozen, analysis);
  check(joined.complete && joined.changedWords.size() == 3 && joined.candidateWords == 1 &&
        joined.changedWords[0].phys == page + 0x218 && joined.changedWords[0].afterBE == 0x01000000 &&
        joined.changedWords[0].cumulativeFetched && !joined.changedWords[0].candidate &&
        joined.changedWords[1].phys == page + 0x600 && joined.changedWords[1].afterBE == 2 &&
        !joined.changedWords[1].cumulativeFetched && joined.changedWords[1].candidate &&
        joined.changedWords[2].phys == page + 4092 && joined.changedWords[2].afterBE == 3,
        "C52 all changed dwords join later candidate bytes even when first changed metadata is excluded");
  analysis.uses.push_back({1, 1, 0, page + 0x602, 1});
  joined = R::intersectFetchPayload(*frozen, analysis);
  check(joined.candidateWords == 1, "C52 overlapping attributes count a changed dword once");
  analysis.uses = {{0, 0, 0, page + 4095, 2}, {1, 1, 0, page - 1, 1}};
  joined = R::intersectFetchPayload(*frozen, analysis);
  check(joined.complete && joined.candidateWords == 1 && joined.changedWords.back().candidate,
        "C52 page clipping includes a last-byte overlap and excludes an adjacent-page-only span");
  analysis.uses.push_back({2, 0, 0, 0x1FFFFFFF, 2});
  joined = R::intersectFetchPayload(*frozen, analysis);
  check(!joined.complete && joined.changedWords.empty() && joined.candidateWords == 0 &&
        std::all_of(joined.candidateMask.begin(), joined.candidateMask.end(), [](uint64_t x) { return x == 0; }),
        "C52 invalid later use discards earlier partial membership");
  analysis.uses.clear(); analysis.complete = false; analysis.reason = "source_changed";
  check(!R::intersectFetchPayload(*frozen, analysis).complete, "C52 incomplete consumer cannot claim absence");
  analysis.complete = true;
  auto tampered = *frozen; tampered.changed.fill(0);
  check(!R::intersectFetchPayload(tampered, analysis).complete,
        "C52 changed bitmap omission is detected against full raw bytes");
  auto changed = later; changed.window = 8;
  check(!store.capture(page, after.data(), changed, true, fetched)->known, "C52 source alias change stays unknown");
  changed = later; ++changed.cpu;
  check(!store.capture(page, after.data(), changed, true, fetched)->known, "C52 CPU generation change stays unknown");
  changed = later; ++changed.gpu;
  check(!store.capture(page, after.data(), changed, true, fetched)->known, "C52 GPU generation change stays unknown");
  changed = later; ++changed.uploaded;
  check(!store.capture(page, after.data(), changed, true, fetched)->known, "C52 upload generation change stays unknown");
  changed = later; ++changed.ownerSum;
  check(!store.capture(page, after.data(), changed, true, fetched)->known, "C52 full-page owner change stays unknown");
  check(!store.capture(page, after.data(), later, false, fetched)->known,
        "C52 observed byte or request race stays unknown");
  store.record(page + 4096, before.data(), version, true);
  store.record(page + 8192, before.data(), version, true);
  store.record(page + 8192, before.data(), version, true);
  const auto capped = store.capture(page + 8192, after.data(), version, true, fetched);
  const auto stats = store.stats();
  check(!capped->known && capped->reason == "baseline_capped" && stats.seen == 3 && stats.retained == 2 &&
        stats.capped == 1 && stats.charged == budget && stats.peak == budget,
        "C52 fixed index and entries obey exact charged budget, unique capped count and no eviction");
  store.record(page, after.data(), later, true);
  check(frozen->before == before && frozen->after == after && frozen->baseline.cpu == 17,
        "C52 subsequent registry uploads cannot rewrite frozen origin evidence");
  store.record(page, before.data(), later, false);
  check(!store.capture(page, after.data(), later, true, fetched)->known && store.stats().invalid == 1,
        "C52 unstable baseline publication invalidates the prior baseline");
  store.record(page + 1, before.data(), version, true);
  check(store.stats().invalid == 2 && !store.capture(page + 1, after.data(), version, true, fetched)->known,
        "C52 unaligned page addresses are refused without aliasing a valid baseline");
  bool rejected = false;
  try { R::FetchPayloadStore tooSmall(R::FetchPayloadStore::indexBytes() - 1); }
  catch (const std::invalid_argument&) { rejected = true; }
  check(rejected, "C52 a budget smaller than fixed metadata is refused");
}

void fetchTargetSelectionTests() {
  R::FetchTargetLatch latch;
  R::FetchTargetOrigin first, out;
  first.sequence = 41; first.ordinal = 1234; first.target = 0x17166218;
  first.missed = 7; first.epoch = 12; first.cpu = 20; first.grantGeneration = 31;
  first.ownerSum = 5; first.ownerSeq = 2; first.known = true;
  check(!latch.select(first), "C51 pre-HUD misses cannot select a target");
  check(!latch.arm("screens/hud/main_hud_extra") && !latch.arm(nullptr),
        "C51 only the exact main HUD path arms selection");
  check(latch.arm("screens/hud/main_hud"), "C51 exact observed HUD arms once");
  auto invalid = first; invalid.target++;
  check(!latch.select(invalid), "C51 unaligned moved byte cannot name a dword");
  invalid = first; invalid.sequence = 0;
  check(!latch.select(invalid), "C51 unscoped decisions cannot select a target");
  check(latch.select(first), "C51 first eligible observed miss selects its actual relocated address");
  auto second = first; second.sequence++; second.target += 0x10000;
  check(!latch.select(second) && !latch.arm("screens/hud/main_hud"),
        "C51 later miss or repeated HUD never replaces the selected origin");
  check(!latch.later(42, out) && !latch.pending(40, out) && latch.pending(41, out),
        "C51 grant confirmation is deferred to the originating command exit");
  second = first; second.target++;
  check(!latch.finalize(second), "C51 finalization cannot switch the target");
  first.known = false; first.reason = "origin_changed_or_unknown";
  first.grantCount = 1; first.serial[0] = 99;
  check(latch.finalize(first), "C51 changed origin remains an explicit finalized unknown");
  check(!latch.later(40, out) && !latch.later(41, out),
        "C51 earlier and causing commands never become consumed evidence");
  check(latch.later(42, out) && out.target == 0x17166218 && out.sequence == 41 &&
        out.ordinal == 1234 && out.missed == 7 && out.epoch == 12 && out.cpu == 20 &&
        out.grantGeneration == 31 && out.ownerSum == 5 && out.ownerSeq == 2 &&
        out.serial[0] == 99 && !out.known,
        "C51 later draw retains exact originating miss epoch and unknown ownership");
  first.known = true;
  check(!latch.finalize(first) && !latch.select(second) && latch.later(43, out) && !out.known,
        "C51 failed first confirmation cannot silently retry on a later allocation");
  R::FetchTargetLatch known;
  known.arm("screens/hud/main_hud");
  first.known = true; first.reason = "origin_confirmed";
  first.grantDigest = 701; first.grantCount = 1; first.serial[0] = 88;
  check(known.select(first) && known.finalize(first) && known.later(42, out) && out.known &&
        out.grantCount == 1 && out.grantDigest == 701 && out.serial[0] == 88,
        "C51 confirmed origin preserves its exact live grant tuple for later draws");
  R::FetchTargetLatch payloadLatch;
  payloadLatch.arm("screens/hud/main_hud");
  auto payload = std::make_shared<R::FetchPagePayload>(); payload->page = 0x17166000;
  first.payload = payload;
  check(payloadLatch.canSelect() && payloadLatch.select(first) && !payloadLatch.canSelect(),
        "C52 payload selection cannot allocate repeated later origins");
  auto finalized = std::make_shared<R::FetchPagePayload>(*payload); finalized->grantCount = 1;
  finalized->grantSerial[0] = 77; finalized->known = true; first.payload = finalized;
  check(payloadLatch.finalize(first) && payloadLatch.later(42, out) && out.payload == finalized &&
        out.payload->grantSerial[0] == 77 && out.payload->known,
        "C52 finalization publishes the immutable full-page grant-confirmed payload");
  R::FetchTargetLatch raced;
  raced.arm("screens/hud/main_hud");
  std::atomic<uint32_t> ready{0}, winners{0};
  std::atomic<bool> start{false};
  auto compete = [&](uint32_t address) {
    auto candidate = first; candidate.target = address;
    ready.fetch_add(1);
    while (!start.load()) std::this_thread::yield();
    if (raced.select(candidate)) winners.fetch_add(1);
  };
  std::thread a(compete, 0x17166218), b(compete, 0x17176218);
  while (ready.load() != 2) std::this_thread::yield();
  start.store(true); a.join(); b.join();
  check(winners.load() == 1 && raced.pending(41, out) &&
        (out.target == 0x17166218 || out.target == 0x17176218),
        "C51 concurrent eligible selection freezes exactly one whole origin");
}

void capturedFetchTests() {
  std::printf("-- sampled actual-consumer fetch diagnostics (no census grade changes)\n");
  constexpr uint64_t directHash = 0x7C00000000000001ull, computedHash = directHash + 1,
                     constantHash = directHash + 2, manyHash = directHash + 3;
  const auto direct = vsUcode({vfetch(0, 8, 0, 36), vfetchMini(6, 36)});
  R::noteVsUcode(directHash, direct.data(), direct.size());
  uint32_t constants[1024] = {}, fetch[192] = {}, indices[] = {1, 4};
  fetch[0] = 0x10003; fetch[1] = 0x400u << 2;
  R::FetchCaptureInput in;
  in.drawId = 101; in.vsHash = directHash; in.indexed = true;
  in.indices = indices; in.indexCount = 2; in.constants = constants; in.fetch = fetch;
  auto contains = [](const R::FetchAnalysis& a, uint32_t phys, uint32_t slot) {
    return std::any_of(a.uses.begin(), a.uses.end(), [&](const R::FetchUse& u) {
      return u.slot == slot && phys >= u.phys && uint64_t(phys) < uint64_t(u.phys) + u.len;
    });
  };
  const auto sparse = R::captureFetchDraw(in);
  check(R::confirmFetchDraw(*sparse, in), "C48 actual staged inputs confirm the admitted capture");
  const auto a = R::analyzeFetchDraw(*sparse, {});
  check(a.complete && a.uses.size() == 4 && contains(a, 0x10020, 0) && contains(a, 0x10098, 0) &&
        !contains(a, 0x10024, 0) && !contains(a, 0x10040, 0),
        "C48 actual sparse indices and separated attributes preserve both kinds of holes");
  indices[0] = 2;
  check(!R::confirmFetchDraw(*sparse, in), "C48 a changed actual converted IB invalidates consumption confirmation");
  check(contains(R::analyzeFetchDraw(*sparse, {}), 0x10020, 0) &&
        !contains(R::analyzeFetchDraw(*sparse, {}), 0x10040, 0),
        "C48 capture owns its immutable converted index copy");
  uint32_t denseIndices[] = {1, 2, 4};
  in.indices = denseIndices; in.indexCount = 3;
  const auto b = R::analyzeFetchDraw(*R::captureFetchDraw(in), {});
  check(b.complete && contains(b, 0x10040, 0),
        "C48 equal extrema with different sparse membership are not memo-equivalent");
  uint32_t cutIndices[] = {1, UINT32_MAX, 4};
  in.indices = cutIndices; in.stripCut = true;
  const auto cut = R::analyzeFetchDraw(*R::captureFetchDraw(in), {});
  check(cut.complete && cut.uses.size() == 4, "C48 converted strip cut is not a vertex");
  in.stripCut = false;
  check(!R::analyzeFetchDraw(*R::captureFetchDraw(in), {}).complete,
        "C48 non-strip UINT32_MAX is unknown, never silently omitted");
  in.indexed = false; in.indices = nullptr; in.firstVertex = 1; in.indexCount = 4;
  check(contains(R::analyzeFetchDraw(*R::captureFetchDraw(in), {}), 0x10060, 0),
        "C48 actual no-IB first/count includes each submitted vertex");
  in.firstVertex = 0xFFFFFF;
  check(!R::analyzeFetchDraw(*R::captureFetchDraw(in), {}).complete,
        "C48 overflowing no-IB normalized range is unknown");
  in.firstVertex = 0; in.indexCount = 4097;
  check(R::analyzeFetchDraw(*R::captureFetchDraw(in), {}).reason == "vertex-budget",
        "C48 vertex budget refuses before reading input");
  Ins roundedFetch = vfetch(0, 1, 0, 36);
  roundedFetch.d1 |= 1u << 15;
  const auto roundedUcode = vsUcode({roundedFetch});
  R::noteVsUcode(directHash + 4, roundedUcode.data(), roundedUcode.size());
  in.vsHash = directHash + 4; in.firstVertex = 0x800001; in.indexCount = 1;
  fetch[0] = 3; fetch[1] = 0xFFFFFFu << 2;
  const auto rounded = R::analyzeFetchDraw(*R::captureFetchDraw(in), {});
  check(rounded.complete && contains(rounded, 0x800002u * 4u, 0),
        "C48 direct rounded fetch retains float32 half-step possibilities above2^23");

  // r1.x=2*r0.x; F1 is an integer at that index; slot1 uses the F1 value.
  // This exercises the production interval evaluator with bytes independent of
  // both guest aliases. A current-host F1 substitution changes the answer.
  Ins f1 = vfetch(0, 1, 0, 33, 1, false, 2);
  f1.d1 |= (1u << 13);  // integer format, not normalized
  const auto computed = vsUcode({alu(0, 1, 1, Rg(0), Rg(0), Rg(0)), f1,
                                 vfetch(1, 4, 0, 36, 2, false, 3)});
  R::noteVsUcode(computedHash, computed.data(), computed.size());
  uint32_t compIndices[] = {1, 3};
  in = R::FetchCaptureInput{};
  in.drawId = 102; in.vsHash = computedHash; in.indexed = true;
  in.indices = compIndices; in.indexCount = 2; in.constants = constants; in.fetch = fetch;
  fetch[0] = kStreamBase | 3; fetch[1] = (64u << 2) | 2u;
  fetch[2] = 0x20003; fetch[3] = 0x400u << 2;
  const auto cp = R::captureFetchDraw(in);
  check(R::confirmFetchDraw(*cp, in), "C49 computed draw confirms its actual staging identity");
  const auto span = R::requestedFetchF1(*cp);
  check(span.phys == kStreamBase + 8 && span.len == 20,
        "C49 requested F1 snapshot binds only the actual sparse input extent");
  std::vector<uint8_t> frozen(20, 0);
  putBE(frozen, 0, 7); putBE(frozen, 16, 11);
  R::FetchFrozenSource source;
  source.drawId = in.drawId; source.phys = span.phys; source.bytes = frozen.data();
  source.byteCount = frozen.size(); source.recordedBefore = 17; source.recordedAfter = 17; source.complete = true;
  putBE(g_str8, 0x108, 41); putBE(g_strA, 0x108, 43);
  putBE(g_str8, 0x118, 45); putBE(g_strA, 0x118, 47);
  const auto ca = R::analyzeFetchDraw(*cp, source);
  check(ca.complete && contains(ca, 0x20070, 1) && contains(ca, 0x200B0, 1) &&
        !contains(ca, 0x20290, 1) && !contains(ca, 0x20080, 1),
        "C49 frozen recorded F1 wins over both newer guest aliases, with sparse L2 holes");
  putBE(frozen, 0, 5); source.recordedBefore = source.recordedAfter = 18;
  const auto cb = R::analyzeFetchDraw(*cp, source);
  check(cb.complete && contains(cb, 0x20050, 1) && !contains(cb, 0x20070, 1),
        "C49 second recorded F1 version produces a distinct consumed candidate set");
  source.recordedAfter = 19;
  const auto changed = R::analyzeFetchDraw(*cp, source);
  check(!changed.complete && changed.uses.empty() && changed.reason == "f1-version",
        "C49 a changed recorded source version cannot produce an exclusion");
  source.recordedAfter = 18; source.complete = false;
  check(!R::analyzeFetchDraw(*cp, source).complete, "C49 omitted source confirmation is unknown");
  source.complete = true; source.drawId++;
  check(!R::analyzeFetchDraw(*cp, source).complete, "C49 another draw's snapshot is unknown");
  source.drawId--; source.byteCount--;
  check(R::analyzeFetchDraw(*cp, source).reason == "f1-source", "C49 truncated source is unknown");

  // Validated literal metadata cannot stand in for a missing live constant block.
  const R::LiteralRow knownRows[] = {{constantHash, 0xFFFFFFFFu, 0, {0, 0, 0, 0}}};
  R::censusSetLiteralsForTest(knownRows, 1);
  const auto withConstant = vsUcode({alu(0, 1, 1, Rg(0), Ck(32), Rg(0)), f1,
                                     vfetch(1, 4, 0, 36, 2, false, 3)});
  R::noteVsUcode(constantHash, withConstant.data(), withConstant.size());
  R::censusSetLiteralsForTest(nullptr, 0);
  in.vsHash = constantHash;
  const auto absent = R::analyzeFetchDraw(*R::captureFetchDraw(in), {});
  check(!absent.complete && absent.reason == "constant-block", "C50 missing staged constant block is unknown");
  in.constantBlocks = 1u << 2;
  const auto constantCapture = R::captureFetchDraw(in);
  check(R::requestedFetchF1(*constantCapture).len != 0,
        "C50 actual constant block permits computed source planning");
  constants[32 * 4] = fbits(1.0f);
  check(!R::confirmFetchDraw(*constantCapture, in), "C50 changed actual constant bytes invalidate confirmation");
  constants[32 * 4] = 0;
  fetch[2] += 4;
  check(!R::confirmFetchDraw(*constantCapture, in), "C50 changed actual fetch binding invalidates confirmation");
  fetch[2] -= 4;

  std::vector<Ins> many(129, vfetch(0, 8, 0, 36));
  const auto manyUcode = vsExecs(many);
  R::noteVsUcode(manyHash, manyUcode.data(), manyUcode.size());
  in.vsHash = manyHash;
  check(!R::analyzeFetchDraw(*R::captureFetchDraw(in), {}).complete,
        "C50 per-instruction budget cannot silently omit a fetch");
  std::vector<Ins> broad;
  for (uint32_t i = 0; i < 17; ++i) broad.push_back(vfetch(0, 8, int32_t(i), 36));
  const auto broadUcode = vsExecs(broad);
  R::noteVsUcode(directHash + 5, broadUcode.data(), broadUcode.size());
  in.vsHash = directHash + 5; in.indexed = false; in.firstVertex = 0; in.indexCount = 4096;
  fetch[0] = 0x10003; fetch[1] = 0xFFFFFFu << 2;
  const auto capped = R::analyzeFetchDraw(*R::captureFetchDraw(in), {});
  check(!capped.complete && capped.uses.empty() && capped.reason == "fetch-range-or-budget",
        "C50 span budget returns unknown with no partial exclusion set");
  in.vsHash = directHash; fetch[0] = 0x1FFFFFFFu; fetch[1] = 0x400u << 2;
  const auto overflow = R::analyzeFetchDraw(*R::captureFetchDraw(in), {});
  check(!overflow.complete && overflow.uses.empty(), "C50 physical extent overflow discards all partial output");
}

void censusTests() {
  std::printf("-- the census (in-process: censusResetForTest)\n");
  R::TestHooks hooks{testHost, testOwnerVer};
  hooks.protect = testProtect;
  hooks.trapLimit = 0;  // no traps until C30 (the misses below would arm them)
  R::censusSetTestHooks(&hooks);
  R::censusResetForTest(false);
  check(!R::census() && R::registry() == nullptr, "C0 off: no census, no registry");
  R::noteCoher(0x03000000u, 0x10000u, 0x1000u);
  R::modelPage(0x10, true, R::Why::kContent, R::Prov::k8);
  R::noteUpAlloc(0x1E900000u, 0x100u, true, 0x1E820000u, 0x1EF20000u);
  R::noteGrant(0xAAE30000u, 0x0DFF0000u, 1u);
  R::hostWillWrite(0x80010000u, 0x1000u);
  {
    const R::Totals& t = snapA();
    check(t.decisions == 0 && t.noteCalls[uint32_t(R::Src::kInval)] == 0 && t.coherRequests == 0 && t.upAllocs == 0 &&
              t.grants == 0 && t.cbHi == 0 && !R::censusTrapHitForTest(0x80010000u, 1u),
          "C0 off: the hooks count nothing (memres2's too)");
  }
  R::censusResetForTest(true);
  check(R::census() && R::registry() != nullptr, "C1 on: the census and its registry");
  R::Registry& g = *R::registry();
  // C1: the coherency statuses (memres2: classified by value - the guest's four, and any other).
  R::noteCoher(0x03000000u, 0x100000u, 0x2000u);  // pages 0x100, 0x101
  R::noteCoher(0x02000000u, 0x200000u, 0x1000u);  // page 0x200
  R::noteCoher(0x01000000u, 0x300000u, 0x2000u);  // the command buffer's whole-buffer invalidate (pages 0x300, 0x301)
  R::noteCoher(0x03000100u, 0x400000u, 0x3000u);  // the Resolve's request after its copy (pages 0x400-0x402)
  R::noteCoher(0x00000100u, 0x480000u, 0x1000u);  // an unclassified status
  R::noteCoher(0x00010000u, 0x1000u, 0u);          // the SDK's shader-constant sync (memres2's fifth status)
  R::noteCoher(0x03000000u, 0xFFFFF000u, 0x1000u);  // the sentinel: refused
  {
    const R::Totals& t = snapA();
    check(t.noteCalls[uint32_t(R::Src::kInval)] == 2 && t.notePages[uint32_t(R::Src::kInval)] == 2,
          "C1 0x03000000: two calls, two pages (the sentinel call counted, its pages none)");
    check(g.lastSrc(0x100) == R::Src::kInval && g.lastSrc(0x101) == R::Src::kInval, "C1 0x03000000 notes Src::kInval");
    check(t.noteCalls[uint32_t(R::Src::kTexLock)] == 1 && g.lastSrc(0x200) == R::Src::kTexLock,
          "C1 0x02000000 (the texture lock) notes Src::kTexLock");
    check(t.noteWhole == 1 && t.noteCalls[uint32_t(R::Src::kCmdBuf)] == 1 && g.lastSrc(0x301) == R::Src::kCmdBuf &&
              g.cpuGen(0x300) != 0 && t.cbLo == 0x300000u && t.cbHi == 0x302000u,
          "C1 0x01000000 (the whole command buffer) notes Src::kCmdBuf over its range and teaches the census that range");
    check(t.noteResolve == 1 && t.noteResolvePages == 3 && g.cpuGen(0x400) == 0 && g.cpuGen(0x402) == 0,
          "C1 0x03000100 (the Resolve's request) counted as note.resolve, never a CPU note");
    check(t.noteCalls[uint32_t(R::Src::kOther)] == 1 && g.lastSrc(0x480) == R::Src::kOther,
          "C1 an unclassified status: noted as a CPU write (Src::kOther), counted");
    check(t.noteShSync == 1 && g.cpuGen(0x1) == 0 && g.lastSrc(0x1) == R::Src::kNone,
          "C1 0x00010000 (the shader-constant sync) counted as note.shsync, never a note");
    check(t.noteRefused == 1, "C1 the sentinel range refused and counted");
    uint64_t calls = 0, inval = 0, res = 0;
    for (uint32_t i = 0; i < R::kCoherSlots; i++) {
      if (!t.coherUsed[i]) continue;
      calls += t.coherCalls[i];
      if (t.coherStatus[i] == 0x03000000u) inval = t.coherCalls[i];
      if (t.coherStatus[i] == 0x03000100u) res = t.coherPages[i];
    }
    check(t.coherRequests == 7 && calls == 7 && inval == 2 && res == 3 && t.coherOverflow == 0,
          "C1 census_coher: every request by its status value (0x03000000 twice, the Resolve's 3 pages)");
  }
  // C2: a vertex page: first use, kept, then re-uploaded by the model with no note -> missed, attributed.
  const uint32_t VB = 0x500;
  {
    const R::KindScope k(R::Kind::kVb);
    R::candPage(VB, R::Site::kDrain, 1, true);
  }
  R::modelPage(VB, true, R::Why::kContent, R::Prov::k8);
  R::modelPage(VB, false, R::Why::kContent, R::Prov::k8);
  R::modelPage(VB, true, R::Why::kContent, R::Prov::k8);
  {
    const R::Totals& t = snapA();
    check(t.decisions == 3 && O(t, R::Outcome::kAgreeUp) == 1 && O(t, R::Outcome::kAgreeKeep) == 1 &&
              O(t, R::Outcome::kMissed) == 1 && O(t, R::Outcome::kOver) == 0,
          "C2 first use agrees, the keep agrees, the unannounced rewrite is MISSED");
    check(t.first == 1 && t.firstBy[uint32_t(R::Kind::kVb)][uint32_t(R::Prov::k8)] == 1,
          "C2 the first-use upload counted by kind and provenance");
    check(missedAt(t, R::Kind::kVb, R::Prov::k8, R::Src::kNone, R::Site::kDrain) == 1,
          "C2 the miss attributed vb / 8 / src none / drain");
    check(t.missedWhy[uint32_t(R::Why::kContent)] == 1 && t.missedWhy[uint32_t(R::Why::kWindow)] == 0 &&
              t.missedWhy[uint32_t(R::Why::kGpu)] == 0, "C2 the model's reason: content");
    check(C(t, R::Cov::kCand) == 1 && C(t, R::Cov::kVal) == 3, "C2 coverage: one candidate, three decisions");
  }
  // C3: an announced rewrite agrees; a note without a byte change is over-invalidation.
  R::noteCoher(0x03000000u, VB << 12, 0x1000u);
  R::modelPage(VB, true, R::Why::kContent, R::Prov::k8);
  R::noteCoher(0x02000000u, VB << 12, 0x1000u);
  R::modelPage(VB, false, R::Why::kContent, R::Prov::k8);
  {
    const R::Totals& t = snapA();
    check(O(t, R::Outcome::kAgreeUp) == 2 && O(t, R::Outcome::kMissed) == 1,
          "C3 a noted rewrite: the registry uploads too (agree)");
    check(O(t, R::Outcome::kOver) == 1 && t.overBy[uint32_t(R::Kind::kVb)][uint32_t(R::Src::kTexLock)] == 1,
          "C3 a note with unchanged bytes (no GPU data under it): over-invalidation, by kind and source");
  }
  // C4: a CPU write over a page the GPU wrote, unannounced -> missed (gpu).
  const uint32_t RT = 0x600;
  {
    const R::KindScope k(R::Kind::kTex);
    R::candPage(RT, R::Site::kPrepass, 1, true);
  }
  R::noteGpuWrite(RT << 12, 0x1000u);
  R::modelPage(RT, false, R::Why::kContent, R::Prov::kA);  // the model keeps the GPU data
  R::modelPage(RT, true, R::Why::kContent, R::Prov::kA);   // the CPU copy's hash moved
  {
    const R::Totals& t = snapA();
    check(t.missedWhy[uint32_t(R::Why::kGpu)] == 1 &&
              missedAt(t, R::Kind::kTex, R::Prov::kA, R::Src::kNone, R::Site::kPrepass) == 1,
          "C4 a CPU write over a resolved page with no note: missed (gpu), tex / A / none / prepass");
    check(t.noteCalls[uint32_t(R::Src::kGpu)] == 1 && t.notePages[uint32_t(R::Src::kGpu)] == 1,
          "C4 the GPU write counted as a note of its own source");
  }
  // C5: a window switch with no note -> missed (window).
  const uint32_t W = 0x700;
  R::modelPage(W, true, R::Why::kContent, R::Prov::k8);
  R::modelPage(W, true, R::Why::kWindow, R::Prov::kA);
  {
    const R::Totals& t = snapA();
    check(t.missedWhy[uint32_t(R::Why::kWindow)] == 1, "C5 the serving window changed with no note: missed (window)");
    check(missedAt(t, R::Kind::kOther, R::Prov::kA, R::Src::kNone, R::Site::kDrain) == 1,
          "C5 an untagged page is kind other, site drain");
  }
  // C6: the range cache's keep covers every page of the range, with the thread's kind, and its checks classify it.
  {
    const R::Totals& a = snapA();
    {
      const R::KindScope k(R::Kind::kVb);
      R::modelKeepRange(0x800, 0x803, R::Site::kDrain, R::Prov::k8, 9);
    }
    const R::Totals& b = snapB();
    check(b.decisions - a.decisions == 4 && b.first - a.first == 4 &&
              b.firstBy[uint32_t(R::Kind::kVb)][uint32_t(R::Prov::k8)] -
                      a.firstBy[uint32_t(R::Kind::kVb)][uint32_t(R::Prov::k8)] == 4,
          "C6 a 4-page range-cache keep: 4 decisions (first uses the model kept)");
    check(O(b, R::Outcome::kOver) - O(a, R::Outcome::kOver) == 4 && gate(b) == gate(a),
          "C6 ... over-invalidation (cost), never a gate counter (the first decision of a page may be a keep)");
    check(C(b, R::Cov::kDecided) - C(a, R::Cov::kDecided) == 4 &&
              K(b, R::Feed::kRc, R::Chk::kUnchecked) - K(a, R::Feed::kRc, R::Chk::kUnchecked) == 4,
          "C6 ... decided in the request; no check ran: 4 unchecked range-cache keeps");
  }
  {
    const R::Totals& a = snapA();
    R::wtChecked(0x804, false);
    R::rcChecked(0x804);
    R::wtChecked(0x805, true);
    R::rcChecked(0x805);
    R::rcMismatch(0x806);
    R::modelKeepRange(0x804, 0x807, R::Site::kDrain, R::Prov::k8, 9);
    const R::Totals& b = snapB();
    check(K(b, R::Feed::kRc, R::Chk::kChecked) - K(a, R::Feed::kRc, R::Chk::kChecked) == 1 &&
              K(b, R::Feed::kRc, R::Chk::kStale) - K(a, R::Feed::kRc, R::Chk::kStale) == 1 &&
              K(b, R::Feed::kRc, R::Chk::kMismatch) - K(a, R::Feed::kRc, R::Chk::kMismatch) == 1 &&
              K(b, R::Feed::kRc, R::Chk::kUnchecked) - K(a, R::Feed::kRc, R::Chk::kUnchecked) == 1,
          "C6 the range check's results classify the keep: checked 1, stale 1, mismatch 1, unchecked 1");
  }
  // C7: the IB arm - its own numbers, outside the page decisions.
  {
    std::vector<uint8_t> ib(256, 0x11);
    const uint32_t IBP = 0x900000u;
    const R::Totals& a = snapA();
    R::ibRead(IBP, 256, ib.data(), R::Prov::k8);  // first conversion
    R::ibRead(IBP, 256, ib.data(), R::Prov::k8);  // unchanged
    ib[7] = 0x22;
    R::ibRead(IBP, 256, ib.data(), R::Prov::k8);  // changed, no note -> missed
    const R::Totals& b = snapB();
    check(b.ibReads - a.ibReads == 3 && b.ibAgreeUp - a.ibAgreeUp == 1 && b.ibAgreeKeep - a.ibAgreeKeep == 1 &&
              b.ibMissed - a.ibMissed == 1 && b.ibFirst - a.ibFirst == 1,
          "C7 IB: the first conversion agrees, unchanged bytes agree, changed bytes with no note are an IB miss");
    check(b.decisions == a.decisions && O(b, R::Outcome::kMissed) == O(a, R::Outcome::kMissed),
          "C7 (Astra P3) the IB arm stays out of the page decisions and the page gate");
    check(b.ibMissedBy[uint32_t(R::Prov::k8)][uint32_t(R::Src::kNone)] == 1, "C7 attributed 8 / none");
  }
  {
    std::vector<uint8_t> ib(256, 0x44);
    const uint32_t IBP = 0x900000u;
    const R::Totals& a = snapA();
    R::noteCoher(0x03000000u, IBP, 0x1000u);
    R::ibRead(IBP, 256, ib.data(), R::Prov::k8);  // changed, noted -> agree
    R::noteCoher(0x03000000u, IBP, 0x1000u);
    R::ibRead(IBP, 256, ib.data(), R::Prov::k8);  // noted, unchanged -> over
    const R::Totals& b = snapB();
    check(b.ibAgreeUp - a.ibAgreeUp == 1 && b.ibOver - a.ibOver == 1 && b.ibMissed == a.ibMissed,
          "C7 IB: a noted change agrees; a note with unchanged bytes is over-invalidation");
  }
  // C8: NtReadFile notes by window.
  {
    const R::Totals& a = snapA();
    R::noteReadFile(0x40001000u, 0x1000u);  // a virtual heap buffer: not GPU-addressable here
    R::noteReadFile(0x8A000000u, 0x1000u);  // 0x8 window: phys 0x0A000000
    R::noteReadFile(0xE0010000u, 0x1000u);  // 0xE window: phys 0x00010000 + 0x1000
    const R::Totals& b = snapB();
    check(b.noteLowVa - a.noteLowVa == 1, "C8 a virtual-heap destination is counted, not noted");
    check(g.lastSrc(0x0A000000u >> 12) == R::Src::kReadFile, "C8 a 0x8-window read notes its physical page");
    check(g.lastSrc(0x11u) == R::Src::kReadFile && g.cpuGen(0x10u) == 0u,
          "C8 a 0xE-window read notes phys + 0x1000 (the allocators' fold)");
    check(b.noteCalls[uint32_t(R::Src::kReadFile)] - a.noteCalls[uint32_t(R::Src::kReadFile)] == 2,
          "C8 two ReadFile notes");
  }
  // C9: the Unlock note from a texture fetch constant; a movie plane's range names its misses.
  {
    uint32_t f[6];
    tex2d(f, 0x02000u, 64, 64, 64, 0x02100u, 3);  // 64x64, mips at 0x02100000 (mip_max 3)
    R::noteUnlock(0x50001000u, f, false);
    check(g.lastSrc(0x02000u) == R::Src::kUnlock && g.lastSrc(0x02100u) == R::Src::kUnlock,
          "C9 the Unlock notes the base and the mip address");
    check(g.cpuGen(0x02003u) != 0u && g.cpuGen(0x02004u) == 0u && g.cpuGen(0x02010u) == 0u,
          "C9 ... the base level's extent (64 x 4 bytes x 64 rows = 16 KiB: pages 0x02000-0x02003)");
    const R::Totals& a = snapA();
    R::noteUnlock(0x50001004u, nullptr, false);
    const R::Totals& b = snapB();
    check(b.noteUnlockNoFetch - a.noteUnlockNoFetch == 1, "C9 an unreadable header is counted");
    check(b.noteUnlockUnmatched - a.noteUnlockUnmatched == 1, "C9 an Unlock with no Lock window open is counted");
    tex2d(f, 0x03000u, 64, 64, 64, 0, 0);
    R::noteUnlock(0x50001008u, f, true);  // a movie plane at 0x03000000
    const uint32_t MP = 0x03000u;
    R::modelPage(MP, true, R::Why::kContent, R::Prov::kLegacyA);  // first use after the note (noted upload)
    R::modelPage(MP, true, R::Why::kContent, R::Prov::kLegacyA);  // the decoder rewrote it: no note
    const R::Totals& c = snapA();
    check(c.missedMovie - b.missedMovie == 1 && O(c, R::Outcome::kMissed) - O(b, R::Outcome::kMissed) == 1,
          "C9 a miss inside a movie plane's range is named movie");
    check(missedAt(c, R::Kind::kOther, R::Prov::kLegacyA, R::Src::kUnlock, R::Site::kDrain) == 1,
          "C9 ... attributed to its last note (the Unlock) and the heuristic window lA");
  }
  // C13 (Astra P1, first_kept): a never-noted all-zero page's FIRST model decision can be a keep (the model's hashes
  // start at the zero page's) - the registry's first-use upload is over-invalidation, not a coverage gap.
  {
    const R::Totals& a = snapA();
    R::modelPage(0xA00, false, R::Why::kContent, R::Prov::kLegacy8);
    const R::Totals& b = snapB();
    check(O(b, R::Outcome::kOver) - O(a, R::Outcome::kOver) == 1 && b.first - a.first == 1 && gate(b) == gate(a),
          "C13 a zero page kept on its first decision: over (first use), no gate counter moves");
  }
  // C14 (Astra P1, clobber): GPU write -> a conservative CPU note -> unchanged RAM: the registry would upload RAM over
  // the GPU's data the model keeps. A gate counter of its own, never `over`.
  {
    const uint32_t G = 0xB00;
    R::modelPage(G, true, R::Why::kContent, R::Prov::k8);  // first use: uploaded
    R::noteGpuWrite(G << 12, 0x1000u);                     // a resolve wrote it
    R::noteCoher(0x03000000u, G << 12, 0x1000u);           // a conservative CPU-write range over it
    const R::Totals& a = snapA();
    R::modelPage(G, false, R::Why::kContent, R::Prov::k8);  // the bytes did not move: the model keeps the GPU data
    const R::Totals& b = snapB();
    check(O(b, R::Outcome::kClobber) - O(a, R::Outcome::kClobber) == 1 && O(b, R::Outcome::kOver) == O(a, R::Outcome::kOver),
          "C14 registry upload over GPU data the model kept: CLOBBER (a gate counter), not over");
    R::noteGpuWrite(G << 12, 0x1000u);
    R::noteCoher(0x03000000u, G << 12, 0x1000u);
    R::modelPage(G, true, R::Why::kContent, R::Prov::k8);  // the CPU copy moved too: both replace the GPU data
    const R::Totals& c = snapA();
    check(c.clobberBoth - b.clobberBoth == 1 && O(c, R::Outcome::kAgreeUp) - O(b, R::Outcome::kAgreeUp) == 1 &&
              O(c, R::Outcome::kClobber) == O(b, R::Outcome::kClobber),
          "C14 both replace the GPU data (the CPU copy moved): agree_up, counted as clobber.both");
  }
  // C15 (Astra P1, the source window): a registry upload serves the allocation's provenance (0x8 when it names none);
  // the model served the zero-page heuristic's 0xA. agree_up before - the bytes the two would upload compared now.
  {
    const uint32_t H = 0xC00;
    g_hostPage = H;
    std::memset(g_win8.data(), 0, 4096);
    g_win8[17] = 1;
    std::memset(g_winA.data(), 0x5A, 4096);
    const R::Totals& a = snapA();
    R::modelPage(H, true, R::Why::kContent, R::Prov::kLegacyA);  // first use, the model served 0xA by the heuristic
    const R::Totals& b = snapB();
    check(O(b, R::Outcome::kAgreeUp) - O(a, R::Outcome::kAgreeUp) == 1 && b.winAliasDiff - a.winAliasDiff == 1 &&
              b.winHeur - a.winHeur == 1,
          "C15 agree_up from the heuristic's 0xA, the registry's 0x8 holds other bytes: win.alias_diff (a gate counter)");
    std::memcpy(g_winA.data(), g_win8.data(), 4096);
    R::noteCoher(0x03000000u, H << 12, 0x1000u);
    R::modelPage(H, true, R::Why::kContent, R::Prov::kLegacyA);
    const R::Totals& c = snapA();
    check(c.winAliasSame - b.winAliasSame == 1 && c.winAliasDiff == b.winAliasDiff,
          "C15 the same disagreement over identical bytes: win.alias_same (harmless now)");
    g_hostPage = H + 1u;
    std::memset(g_win8.data(), 0, 4096);
    std::memset(g_winA.data(), 0x33, 4096);
    R::modelPage(H + 1u, false, R::Why::kContent, R::Prov::kUndecided);  // a write-watch keep under the legacy rule
    const R::Totals& d = snapB();
    check(d.winAliasDiff - c.winAliasDiff == 1 && O(d, R::Outcome::kOver) - O(c, R::Outcome::kOver) == 1,
          "C15 a first-use registry upload over a page the legacy rule serves from 0xA: win.alias_diff too");
    check(R::censusLastSampleForTest(2).find(" src=none grant=none pw_flags=") != std::string::npos &&
              R::censusLastSampleForTest(2).find(" pw_contested=") != std::string::npos &&
              (d.trapArmed + d.trapArmFail + d.trapDeclined) - (c.trapArmed + c.trapArmFail + c.trapDeclined) == 1,
          "C15 memres4: the ALIAS sample names the last note's source, the grant and the allocators' records, and the "
          "ALIAS asks for a trap on the page's two window copies (declined here: no traps before C30)");
    check(R::censusLastSampleForTest(2).find(" h8=Z hA=") != std::string::npos &&
              R::censusLastSampleForTest(2).find(" mlast=-") != std::string::npos,
          "C15 memres6: the ALIAS tail names each copy (Z: all zero) and the model's last upload window (- none)");
    R::modelPage(H + 2u, true, R::Why::kContent, R::Prov::kLegacyA);  // its windows unreadable
    const R::Totals& e = snapA();
    check(e.winUnmapped - d.winUnmapped == 1 && e.winAliasDiff == d.winAliasDiff,
          "C15 windows that cannot be read are counted apart (win.unmapped)");
    g_hostPage = 0xFFFFFFFFu;
  }
  // C16 (Astra P2, a late note): the generations are read before the bytes are hashed; a note landing in between leaves
  // the decision a race (the bytes may predate the write) and the note pending. Before: ensurePage re-read the note,
  // agreed, committed its generation - and the completed bytes' next upload was reported MISSED.
  {
    const uint32_t T = 0xD00;
    R::modelPage(T, true, R::Why::kContent, R::Prov::k8);  // first use
    const uint32_t up0 = g.uploadedGen(T);
    const R::Totals& a = snapA();
    {
      R::WalkScope v(1, true);
      R::snapPage(T);
      R::noteCoher(0x03000000u, T << 12, 0x1000u);          // the guest's note lands while the model hashes
      R::modelPage(T, true, R::Why::kContent, R::Prov::k8);  // the model saw moved bytes
    }
    const R::Totals& b = snapB();
    check(O(b, R::Outcome::kRaceNote) - O(a, R::Outcome::kRaceNote) == 1 && gate(b) == gate(a),
          "C16 a note in the hash window: race.note, not a missed boundary");
    check(g.uploadedGen(T) == up0, "C16 ... the registry commits nothing (the bytes may not hold the noted write)");
    {
      R::WalkScope v(1, true);
      R::snapPage(T);
      R::modelPage(T, true, R::Why::kContent, R::Prov::k8);  // the next validation: the completed bytes
    }
    const R::Totals& c = snapA();
    check(O(c, R::Outcome::kAgreeUp) - O(b, R::Outcome::kAgreeUp) == 1 && gate(c) == gate(b),
          "C16 ... the next decision uploads with the model (the note was pending): agree, no miss");
    check(c.covUnaccounted == b.covUnaccounted, "C16 each validation scope disposed of its one candidate");
  }
  // C17 (Astra P2, a prepass read inside Lock..Unlock): the guest is still writing the texture; the model's host read
  // sees moved bytes the registry has no note for yet. race.lock, then the Unlock's note agrees.
  {
    uint32_t f[6];
    tex2d(f, 0x04000u, 64, 64, 64, 0, 0);
    const uint32_t L = 0x04000u, TEX = 0x50002000u;
    {
      const R::KindScope k(R::Kind::kTex);
      R::candPage(L, R::Site::kPrepass, 2, true);
      R::candPage(0x04010u, R::Site::kPrepass, 2, true);
    }
    R::modelPage(L, true, R::Why::kContent, R::Prov::k8);        // first use
    R::modelPage(0x04010u, true, R::Why::kContent, R::Prov::k8);  // a page outside the texture
    const R::Totals& a = snapA();
    const uint64_t open0 = a.lockOpenPages;
    R::noteLock(TEX, f);
    check(snapB().lockOpenPages - open0 == 4, "C17 the Lock opens a window over the texture (4 pages)");
    R::modelPage(L, true, R::Why::kContent, R::Prov::k8);        // the prepass reads it mid-write
    R::modelPage(0x04010u, true, R::Why::kContent, R::Prov::k8);  // control: no lock there, no note
    const R::Totals& b = snapB();
    check(O(b, R::Outcome::kRaceLock) - O(a, R::Outcome::kRaceLock) == 1, "C17 inside the open Lock: race.lock");
    check(O(b, R::Outcome::kMissed) - O(a, R::Outcome::kMissed) == 1, "C17 ... outside it the same read is MISSED");
    R::noteUnlock(TEX, f, false);
    R::modelPage(L, true, R::Why::kContent, R::Prov::k8);  // the completed texture
    const R::Totals& c = snapA();  // (reuses a's storage: open0 holds what is needed of it)
    check(c.lockOpenPages == open0 && c.noteUnlockUnmatched == b.noteUnlockUnmatched,
          "C17 the Unlock closes the window it opened");
    check(O(c, R::Outcome::kAgreeUp) - O(b, R::Outcome::kAgreeUp) == 1 &&
              O(c, R::Outcome::kMissed) == O(b, R::Outcome::kMissed),
          "C17 ... and its note makes the next read agree");
  }
  // C18 (Astra P1/P2, the epoch skips): a skip relies on a stamp; the census checks it recorded that stamp.
  {
    const uint32_t E = 0xE00;
    {
      const R::KindScope k(R::Kind::kVb);
      R::candPage(E, R::Site::kDrain, 7, true);
    }
    R::modelPage(E, true, R::Why::kContent, R::Prov::k8);
    R::noteCoher(0x03000000u, E << 12, 0x1000u);  // a note after the decision, inside the epoch
    const R::Totals& a = snapA();
    R::skipReuse(E, 7, true);
    const R::Totals& b = snapB();
    check(C(b, R::Cov::kReuse) - C(a, R::Cov::kReuse) == 1 && b.reuseNoted - a.reuseNoted == 1,
          "C18 a reuse of a recorded stamp, across a note since the decision: reuse + reuse.noted");
    R::skipReuse(E, 8, true);  // a stamp no hooked path recorded
    const R::Totals& c = snapA();
    check(C(c, R::Cov::kUnseen) - C(b, R::Cov::kUnseen) == 1, "C18 a reuse of a stamp the census never saw: unseen");
    R::candPage(E, R::Site::kRender, 3, false);  // the render's stamp (frame start)
    R::modelPage(E, false, R::Why::kContent, R::Prov::k8);
    R::skipReuse(E, 3, false);
    R::skipReuse(E, 3, true);  // the drain's check never accepts a render stamp
    const R::Totals& d = snapB();
    check(C(d, R::Cov::kReuse) - C(c, R::Cov::kReuse) == 1 && C(d, R::Cov::kUnseen) - C(c, R::Cov::kUnseen) == 1,
          "C18 the render's reuse of its own stamp is seen; the drain's check of it is not");
    R::candPage(E, R::Site::kDrain, 9, true);
    R::failedPage(E);
    R::skipReuse(E, 9, true);
    const R::Totals& e = snapA();
    check(C(e, R::Cov::kReuseFailed) - C(d, R::Cov::kReuseFailed) == 1 && C(e, R::Cov::kFailed) - C(d, R::Cov::kFailed) == 1,
          "C18 a reuse of a failed validation: reuse_failed (nothing was served)");
  }
  // C19: the walk scopes - every page disposed of exactly once, per request and per validation.
  {
    const R::Totals& a = snapA();
    {
      R::WalkScope r(4, false);
      R::candPage(0xF00, R::Site::kDrain, 4, true);
      R::candPage(0xF01, R::Site::kDrain, 4, true);
      R::skipDeferred(0xF02);
    }
    const R::Totals& b = snapB();
    check(b.covUnaccounted - a.covUnaccounted == 1 && b.covReq - a.covReq == 4,
          "C19 a 4-page request whose walk disposed of 3: cov.unaccounted 1");
    {
      R::WalkScope r(2, false);
      R::skipDeferred(0xF03);
      {
        R::WalkScope v(1, true);
        R::failedPage(0xF04);
      }
      R::skipDeferred(0xF05);
    }
    const R::Totals& c = snapA();
    check(c.covUnaccounted == b.covUnaccounted && c.covValCands - b.covValCands == 1 &&
              C(c, R::Cov::kDeferred) - C(b, R::Cov::kDeferred) == 2,
          "C19 a nested validation counts its own candidates; both scopes balance");
  }
  // C20 (Astra P2, the Unlock's span): a 32x32 k_8_8_8_8 texture with unpacked mips - each level at its own aligned
  // stride (cube_layout.hpp). The first census noted one base-sized span (4 KiB) at the mip address.
  {
    uint32_t f[6];
    tex2d(f, 0x05000u, 32, 32, 32, 0x05100u, 5);
    const sr::gfx::cube::CubeLayout L = sr::gfx::cube::computeGuestLayout(6, 32, 32, 32, false, 6, false, 1, 1, 4, 1);
    check(L.ok && L.mipsTotalExtentBytes > 4096u, "C20 the real layout's mip extent exceeds one base-sized span");
    R::noteUnlock(0x50003000u, f, false);
    bool all = true;
    for (uint32_t p = 0x05100u; p < 0x05100u + ((L.mipsTotalExtentBytes + 4095u) >> 12); p++)
      all = all && g.cpuGen(p) != 0u;
    check(all && g.cpuGen(0x05000u) != 0u, "C20 the Unlock notes every page of the mip chain's real extent");
  }
  // C21 (Astra P2, movie marks): a mark holds while the allocation over the page is the plane's.
  {
    uint32_t f[6];
    tex2d(f, 0x06000u, 64, 64, 64, 0, 0);
    const uint32_t MV = 0x06001u;
    g_ownerVer = 5;
    R::noteUnlock(0x50004000u, f, true);
    const R::Totals& a = snapA();
    R::modelPage(MV, true, R::Why::kContent, R::Prov::kLegacyA);  // noted: agree
    R::modelPage(MV, true, R::Why::kContent, R::Prov::kLegacyA);  // the decoder: missed (movie)
    g_ownerVer = 6;                                                // the allocation over it changed
    R::modelPage(MV, true, R::Why::kContent, R::Prov::kLegacyA);
    const R::Totals& b = snapB();
    check(O(b, R::Outcome::kMissed) - O(a, R::Outcome::kMissed) == 2 && b.missedMovie - a.missedMovie == 1,
          "C21 the mark names the decoder's miss; after the allocation changed it no longer does");
    R::noteUnlock(0x50004000u, f, true);  // the plane again: marked under the new allocation
    R::modelPage(MV, true, R::Why::kContent, R::Prov::kLegacyA);
    R::modelPage(MV, true, R::Why::kContent, R::Prov::kLegacyA);
    R::noteUnlock(0x50005000u, f, false);  // another texture unlocked over the same pages
    R::modelPage(MV, true, R::Why::kContent, R::Prov::kLegacyA);
    R::modelPage(MV, true, R::Why::kContent, R::Prov::kLegacyA);
    const R::Totals& c = snapA();
    check(O(c, R::Outcome::kMissed) - O(b, R::Outcome::kMissed) == 2 && c.missedMovie - b.missedMovie == 1,
          "C21 re-marked by the plane's Unlock; another texture's Unlock over it ends the mark");
  }
  // C22: the write watch's keeps classified by the model's own checks.
  {
    const R::Totals& a = snapA();
    R::wtChecked(0x1100, false);
    R::acceptPage(0x1100, R::Site::kDrain, 4, true, R::Prov::k8);
    R::acceptPage(0x1101, R::Site::kDrain, 4, true, R::Prov::k8);
    R::wtChecked(0x1102, true);
    R::acceptPage(0x1102, R::Site::kDrain, 4, true, R::Prov::k8);
    R::wtChecked(0x1103, false);
    R::acceptPage(0x1104, R::Site::kDrain, 4, true, R::Prov::k8);  // the check was another page's
    const R::Totals& b = snapB();
    check(K(b, R::Feed::kWt, R::Chk::kChecked) - K(a, R::Feed::kWt, R::Chk::kChecked) == 1 &&
              K(b, R::Feed::kWt, R::Chk::kStale) - K(a, R::Feed::kWt, R::Chk::kStale) == 1 &&
              K(b, R::Feed::kWt, R::Chk::kUnchecked) - K(a, R::Feed::kWt, R::Chk::kUnchecked) == 2,
          "C22 write-watch keeps: checked 1, stale 1, unchecked 2 (none, or another page's check)");
    check(C(b, R::Cov::kDecided) - C(a, R::Cov::kDecided) == 4 && b.decisions - a.decisions == 4,
          "C22 each accept is a decision of the request");
  }
  // C23: a readback re-baselines and stamps a page (pm4ShmReadback); a later reuse of that stamp is seen.
  {
    const R::Totals& a = snapA();
    R::rebasePage(0x1200, 11, true);
    R::skipReuse(0x1200, 11, true);
    R::rebasePage(0x1201, 12, false);
    R::skipReuse(0x1201, 12, false);
    const R::Totals& b = snapB();
    check(C(b, R::Cov::kRebase) - C(a, R::Cov::kRebase) == 2 && C(b, R::Cov::kReuse) - C(a, R::Cov::kReuse) == 2 &&
              C(b, R::Cov::kUnseen) == C(a, R::Cov::kUnseen),
          "C23 a readback's stamp (the drain's and the render's) is recorded: its reuse is seen");
  }
  // C24: the range cache's keep stamps nothing in the model: its epoch must not overwrite the stamp a walk set (the render
  // thread's later reuse of that stamp), yet the range memo's reuse of the kept range is seen.
  {
    const uint32_t P = 0x1300;
    R::candPage(P, R::Site::kDrain, 5, true);  // the walk stamped it with drain epoch 5
    R::modelPage(P, true, R::Why::kContent, R::Prov::k8);
    R::modelKeepRange(P, P, R::Site::kDrain, R::Prov::k8, 6);  // the range cache keeps it in drain epoch 6
    const R::Totals& a = snapA();
    R::skipReuse(P, 5, false);  // the render reuses the walk's stamp (5 >= its frame start)
    R::skipMemo(P, 6);          // the drain's memo reuses the kept range
    const R::Totals& b = snapB();
    check(C(b, R::Cov::kReuse) - C(a, R::Cov::kReuse) == 2 && C(b, R::Cov::kUnseen) == C(a, R::Cov::kUnseen),
          "C24 a range-cache keep leaves the walk's stamp in place, and the memo's reuse of the keep is seen");
    R::skipMemo(P, 7);
    check(C(snapA(), R::Cov::kUnseen) - C(b, R::Cov::kUnseen) == 1, "C24 a memo claim of another epoch is unseen");
  }
  // C25 (memres1's clobbers - the counterexample, fails on the first census): the Resolve 8210C5F8 copies into its
  // destination, then requests coherency over it with COHER_STATUS_HOST 0x03000100 (0x8210D2F4). The first census read
  // every status but 0x03000000 / 0x01000000 as a CPU write (src=host): a note newer than the GPU's write, so the registry
  // would "upload RAM over the GPU's data" the model kept - 37.6 M / 64.2 M / 43.8 M clobbers in mrc1-3.
  {
    const uint32_t RS = 0x1400;
    {
      const R::KindScope k(R::Kind::kResolve);
      R::candPage(RS, R::Site::kRender, 13, false);
    }
    R::modelPage(RS, true, R::Why::kContent, R::Prov::kA);  // first use
    const R::Totals& a = snapA();
    R::noteGpuWrite(RS << 12, 0x1000u);                      // the copy wrote the destination
    R::noteCoher(0x03000100u, RS << 12, 0x1000u);            // the Resolve's request after it (memres1's order)
    R::modelPage(RS, false, R::Why::kContent, R::Prov::kA);  // the model keeps the GPU's data
    R::noteCoher(0x03000100u, RS << 12, 0x1000u);            // the next resolve's request, applied before its copy
    R::modelPage(RS, false, R::Why::kContent, R::Prov::kA);
    const R::Totals& b = snapB();
    check(O(b, R::Outcome::kClobber) == O(a, R::Outcome::kClobber) &&
              O(b, R::Outcome::kAgreeKeep) - O(a, R::Outcome::kAgreeKeep) == 2,
          "C25 a resolve's write and its own coherency request: the registry keeps the GPU's data (agree_keep), no clobber");
    check(b.noteResolve - a.noteResolve == 2 && g.lastSrc(RS) == R::Src::kNone,
          "C25 ... the requests counted as note.resolve, no CPU note on the page");
  }
  // C26 (memres1's missed vertex pages - the counterexample, fails on the first census): BeginVertices /
  // DrawVerticesUP carve the inline vertices from the command buffer (GpuCmdBufAllocate 820FCDF0) and announce nothing
  // per allocation; the console's boundary is the close's vertex-cache invalidate of the whole buffer (0x01000000 over
  // [dev+0x34AC, dev+0x34B0)), which the first census dropped - 230 of memres1's 231 MISSED samples lie in the buffer.
  const uint32_t CB0 = 0x1E820u, CB1 = 0x1EF20u;  // memres1's command buffer: phys 0x1E820000-0x1EF20000
  {
    const uint32_t UP = 0x1E900u;
    const R::Totals& a = snapA();
    R::noteUpAlloc(UP << 12, 0x200u, false, CB0 << 12, CB1 << 12);
    {
      const R::KindScope k(R::Kind::kVb);
      R::candPage(UP, R::Site::kDrain, 14, true);
    }
    R::modelPage(UP, true, R::Why::kContent, R::Prov::k8);           // first use
    R::noteCoher(0x01000000u, CB0 << 12, (CB1 - CB0) << 12);         // the next close's whole-buffer invalidate
    R::modelPage(UP, true, R::Why::kContent, R::Prov::k8);           // the lap's new vertices
    const R::Totals& b = snapB();
    check(O(b, R::Outcome::kMissed) == O(a, R::Outcome::kMissed) &&
              O(b, R::Outcome::kAgreeUp) - O(a, R::Outcome::kAgreeUp) == 2,
          "C26 the close's whole-buffer invalidate notes the inline vertices: agree_up, not missed");
    check(g.lastSrc(UP) == R::Src::kCmdBuf && b.cbLo == (CB0 << 12) && b.cbHi == (CB1 << 12) &&
              b.upAllocs - a.upAllocs == 1 && b.upAllocsZpass == a.upAllocsZpass,
          "C26 ... Src::kCmdBuf; the buffer's range and the allocation learned (noteUpAlloc)");
  }
  // C27: the missed class's attributes - a rewrite with no whole-buffer request between (a pinned segment's vertices,
  // or the first lap): cb in, seg zp / up; a page outside the buffer: cb out, seg none.
  {
    const uint32_t UPZ = 0x1E910u, OUT = 0x1500u;
    R::noteUpAlloc(UPZ << 12, 0x100u, true, CB0 << 12, CB1 << 12);  // made while a Z-pass list records
    {
      const R::KindScope k(R::Kind::kVb);
      R::candPage(UPZ, R::Site::kDrain, 16, true);
      R::candPage(OUT, R::Site::kDrain, 16, true);
    }
    R::modelPage(UPZ, true, R::Why::kContent, R::Prov::k8);  // first use
    R::modelPage(OUT, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& a = snapA();
    R::modelPage(UPZ, true, R::Why::kContent, R::Prov::k8);  // rewritten, no request: missed
    R::modelPage(OUT, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& b = snapB();
    check(O(b, R::Outcome::kMissed) - O(a, R::Outcome::kMissed) == 2 &&
              b.missedAttr[uint32_t(R::Kind::kVb)][uint32_t(R::Src::kCmdBuf)][uint32_t(R::InCb::kIn)]
                          [uint32_t(R::Seg::kZpass)][uint32_t(R::Rng::kUnknown)] -
                      a.missedAttr[uint32_t(R::Kind::kVb)][uint32_t(R::Src::kCmdBuf)][uint32_t(R::InCb::kIn)]
                                  [uint32_t(R::Seg::kZpass)][uint32_t(R::Rng::kUnknown)] == 1,
          "C27 a pinned segment's vertices rewritten with no request: missed vb / cmdbuf / cb in / seg zp");
    check(attrCb(b, R::InCb::kOut) - attrCb(a, R::InCb::kOut) == 1 &&
              attrSeg(b, R::Seg::kNone) - attrSeg(a, R::Seg::kNone) == 1,
          "C27 ... a page outside the buffer: cb out, seg none");
  }
  // C28 (memres3): the fetched bytes against the registry's copy - a request names its range [phys, +len) (its 16-byte
  // chunks join the page's fetched mask); the registry's upload records its copy; a model re-upload whose fetched chunks
  // all match the copy is PAGE_ONLY (a page-granular false miss: reported, not gated), one with a moved fetched chunk is
  // MISSED (rng moved). memres2's SetStreamSource header-fence stamp is the first kind.
  {
    const uint32_t RP = 0x1600u;
    g_hostPage = RP;
    std::memset(g_win8.data(), 0x10, 4096);
    std::memset(g_winA.data(), 0x10, 4096);
    {
      R::WalkScope r(1, false, (RP << 12) + 0x100u, 0x100u);  // the request fetches [0x100, 0x200) of the page
      const R::KindScope k(R::Kind::kVb);
      R::candPage(RP, R::Site::kDrain, 17, true);
    }
    R::modelPage(RP, true, R::Why::kContent, R::Prov::k8);  // first use: the registry's copy recorded
    const R::Totals& a = snapA();
    g_win8[0x808] = 0x77;                                   // a header's fence: a byte no request fetches
    R::modelPage(RP, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& b = snapB();
    check(O(b, R::Outcome::kPageOnly) - O(a, R::Outcome::kPageOnly) == 1 &&
              O(b, R::Outcome::kMissed) == O(a, R::Outcome::kMissed) &&
              attrRng(b, R::Rng::kSame) - attrRng(a, R::Rng::kSame) == 1 && gate(b) == gate(a),
          "C28 only bytes no request fetches moved: PAGE_ONLY (rng same), no gate counter moves");
    g_win8[0x180] = 0x66;                                   // a fetched byte
    R::modelPage(RP, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& c = snapA();
    check(O(c, R::Outcome::kMissed) - O(b, R::Outcome::kMissed) == 1 &&
              attrRng(c, R::Rng::kMoved) - attrRng(b, R::Rng::kMoved) == 1,
          "C28 a fetched byte moved: MISSED, rng moved (the registry's copy is still its first upload's)");
    // a page the registry copied but no request named a byte range of: nothing fetched to compare - MISSED (rng unk)
    const uint32_t RQ = 0x1610u;
    g_hostPage = RQ;
    std::memset(g_win8.data(), 0x30, 4096);
    std::memset(g_winA.data(), 0x30, 4096);
    {
      R::WalkScope r(1, false);  // a request without its range (memres1's form)
      const R::KindScope k(R::Kind::kVb);
      R::candPage(RQ, R::Site::kDrain, 18, true);
    }
    R::modelPage(RQ, true, R::Why::kContent, R::Prov::k8);  // first use: the registry's copy recorded
    g_win8[0x40] = 0x31;
    R::modelPage(RQ, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& d = snapB();
    check(O(d, R::Outcome::kMissed) - O(c, R::Outcome::kMissed) == 1 &&
              O(d, R::Outcome::kPageOnly) == O(c, R::Outcome::kPageOnly) &&
              attrRng(d, R::Rng::kUnknown) - attrRng(c, R::Rng::kUnknown) == 1,
          "C28 a copied page no request named a range of: nothing fetched to compare - MISSED (rng unk), not page-only");
    g_hostPage = 0xFFFFFFFFu;
  }
  // C53 (mem-native): the D3D header fence stamp as an EXACT CPU-write patch. mrc_fetch4's Warworld class: a request
  // whose fetched range covers a header (00200001 00000001 <fence> ...) packed beside vertex data; SetStreamSource
  // stamps the outgoing buffer's fence at header + 8 with no coherency record. Without a note the model's re-upload is
  // MISSED (rng moved: the control - the census's misses return when the note is removed); with notePatch the
  // registry's copy takes exactly that dword at the decision (census_patch applied) and the page's generations do not
  // move; a second, unannounced fetched write on the same page is still MISSED (a patch hides nothing else).
  {
    const uint32_t HP = 0x1630u, fenceOff = 0x188u;
    g_hostPage = HP;
    std::memset(g_win8.data(), 0x40, 4096);
    std::memset(g_winA.data(), 0x40, 4096);
    {
      R::WalkScope r(1, false, (HP << 12) + 0x100u, 0x200u);  // the request's range covers the header at 0x180
      const R::KindScope k(R::Kind::kVb);
      R::candPage(HP, R::Site::kDrain, 40, true);
    }
    R::modelPage(HP, true, R::Why::kContent, R::Prov::k8);  // first use: the registry's copy recorded
    const R::Totals& a = snapA();
    g_win8[fenceOff + 3] = 0x17;                            // the stamp, no note: the control
    R::modelPage(HP, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& b = snapB();
    check(O(b, R::Outcome::kMissed) - O(a, R::Outcome::kMissed) == 1 &&
              attrRng(b, R::Rng::kMoved) - attrRng(a, R::Rng::kMoved) == 1,
          "C53 control: the fence stamp with no note is MISSED, rng moved (mrc_fetch4's class)");
    const uint32_t cpu0 = g.cpuGen(HP), clock0 = g.clock();
    g_win8[fenceOff + 3] = 0x18;                            // the next stamp, now noted exactly
    R::notePatch(0x80000000u + (HP << 12) + fenceOff, 4u);
    check(g.cpuGen(HP) == cpu0 && g.clock() == clock0, "C53 a patch moves neither the page's generation nor the clock");
    R::modelPage(HP, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& c = snapA();
    const std::string line = R::censusText("census", 1);
    check(O(c, R::Outcome::kMissed) == O(b, R::Outcome::kMissed) &&
              O(c, R::Outcome::kPageOnly) - O(b, R::Outcome::kPageOnly) == 1,
          "C53 the noted stamp: the registry's patched copy matches every fetched dword - no miss");
    check(line.find("census_patch census notes=1 dwords=1 refused=0 applied=1 applied_dw=1 upload=0 gpu=0 nocopy=0") !=
              std::string::npos,
          "C53 census_patch reports the note and its application: " + line.substr(line.find("census_patch")).substr(0, 140));
    g_win8[fenceOff + 3] = 0x19;                            // a stamp noted ...
    R::notePatch(0x80000000u + (HP << 12) + fenceOff, 4u);
    g_win8[0x104] = 0x41;                                   // ... and a fetched vertex dword rewritten with no note
    R::modelPage(HP, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& d = snapB();
    check(O(d, R::Outcome::kMissed) - O(c, R::Outcome::kMissed) == 1,
          "C53 a patch hides nothing else: an unannounced fetched write on the patched page is still MISSED");
    R::notePatch(0x40000000u + fenceOff, 4u);               // virtual heap: refused
    R::noteCoher(0x03000000u, HP << 12, 0x1000u);           // a page note, then a patch: the upload's copy holds it
    g_win8[fenceOff + 3] = 0x1A;
    R::notePatch(0x80000000u + (HP << 12) + fenceOff, 4u);
    R::modelPage(HP, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& e = snapA();
    const std::string line2 = R::censusText("census", 1);
    check(O(e, R::Outcome::kAgreeUp) - O(d, R::Outcome::kAgreeUp) == 1 &&
              line2.find("notes=3 dwords=3 refused=1 applied=2 applied_dw=2 upload=1 gpu=0 nocopy=0") !=
                  std::string::npos,
          "C53 an upload's decision takes the pending patch (upload=1), the virtual-heap patch is refused");
    g_hostPage = 0xFFFFFFFFu;
  }
  // C31 (memres3: a sound `same`; the counterexample to a check of the deciding request's chunks only): a page-only
  // decision compares the chunks fetched so far; the registry keeps its old copy. A LATER request fetches a chunk the
  // first did not - a reuse in the same drain, no decision of its own - and that chunk had moved too: the registry would
  // serve it stale. The union of every request's chunks (the reuse's included) and the diverged page's re-check make the
  // page's next decision MISSED (why stale), although the model keeps (its copy is current since the page-only upload).
  {
    const uint32_t P2 = 0x1650u;
    g_hostPage = P2;
    std::memset(g_win8.data(), 0x20, 4096);
    std::memset(g_winA.data(), 0x20, 4096);
    {
      R::WalkScope r(1, false, P2 << 12, 0x100u);  // fetches [0, 0x100)
      const R::KindScope k(R::Kind::kVb);
      R::candPage(P2, R::Site::kDrain, 30, true);
    }
    R::modelPage(P2, true, R::Why::kContent, R::Prov::k8);  // first use: the registry's copy recorded
    g_win8[0x904] = 0x55;                                   // an unannounced write in a chunk nobody fetched yet
    {
      R::WalkScope r(1, false, P2 << 12, 0x100u);
      R::candPage(P2, R::Site::kDrain, 31, true);
    }
    R::modelPage(P2, true, R::Why::kContent, R::Prov::k8);  // [0, 0x100) did not move: page-only, the page diverges
    const R::Totals& a = snapA();
    {
      R::WalkScope r(1, false, (P2 << 12) + 0x900u, 0x10u);  // a second request fetching chunk 0x900, reused this drain
      R::skipReuse(P2, 31, true);
    }
    const R::Totals& b = snapB();
    check(O(a, R::Outcome::kPageOnly) >= 1 && b.decisions == a.decisions && C(b, R::Cov::kReuse) - C(a, R::Cov::kReuse) == 1,
          "C31 the page-only decision, then a reuse fetching another chunk (no decision of its own)");
    {
      R::WalkScope r(1, false, P2 << 12, 0x100u);  // the next drain: the write watch keeps it (the model's keep)
      R::acceptPage(P2, R::Site::kDrain, 32, true, R::Prov::k8);
    }
    const R::Totals& c = snapA();
    check(O(c, R::Outcome::kMissed) - O(b, R::Outcome::kMissed) == 1 &&
              c.missedWhy[uint32_t(R::Why::kStale)] - b.missedWhy[uint32_t(R::Why::kStale)] == 1 &&
              O(c, R::Outcome::kAgreeKeep) == O(b, R::Outcome::kAgreeKeep),
          "C31 the reuse's chunk joined the union: the diverged page's next decision is MISSED (stale), not agree_keep");
    R::noteCoher(0x03000000u, P2 << 12, 0x1000u);  // a note: the registry uploads, its copy is RAM again
    {
      R::WalkScope r(1, false, P2 << 12, 0x100u);
      R::candPage(P2, R::Site::kDrain, 33, true);
    }
    R::modelPage(P2, true, R::Why::kContent, R::Prov::k8);
    {
      R::WalkScope r(1, false, P2 << 12, 0x100u);
      R::acceptPage(P2, R::Site::kDrain, 34, true, R::Prov::k8);
    }
    const R::Totals& d = snapB();
    check(O(d, R::Outcome::kMissed) == O(c, R::Outcome::kMissed) &&
              O(d, R::Outcome::kAgreeKeep) - O(c, R::Outcome::kAgreeKeep) == 1,
          "C31 ... until the registry uploads (its copy recorded again, the page no longer diverged): agree_keep");
    // ... and stays so: a later write both keeps miss (the model's keep stale too - its own checker's business,
    // wt.stale) is agree_keep: the census grades the registry against the model, and a page the registry re-copied is
    // not re-checked
    g_win8[0x10] = 0x56;
    {
      R::WalkScope r(1, false, P2 << 12, 0x100u);
      R::acceptPage(P2, R::Site::kDrain, 35, true, R::Prov::k8);
    }
    const R::Totals& e = snapA();
    check(O(e, R::Outcome::kMissed) == O(d, R::Outcome::kMissed) &&
              O(e, R::Outcome::kAgreeKeep) - O(d, R::Outcome::kAgreeKeep) == 1,
          "C31 ... the registry's upload ended the divergence: a later both-blind keep is agree_keep, not stale");
    g_hostPage = 0xFFFFFFFFu;
  }
  // C32 (memres4's diagnostics; memres5's dword-exact comparison): a request fetching [0x100, 0x148) of the page
  // fetches the first two dwords of chunk 0x140. A write to its third dword (not fetched) is PAGE_ONLY - memres3's
  // chunk comparison read it MISSED - at=148 mdw=3, the chunk's bytes and the request's range printed; a write to its
  // second dword (fetched) is MISSED at=144. A write no request covers is PAGE_ONLY at its dword, mdw=0, no covering
  // range. (memres6: a range's class follows it - these requests have no draw scope: fallbacks, /f - and via= names the
  // strongest class that marked the moved dword: f, or - when nothing did.)
  {
    const uint32_t P4 = 0x1680u, P5 = 0x1690u;
    g_hostPage = P4;
    std::memset(g_win8.data(), 0x40, 4096);
    std::memset(g_winA.data(), 0x40, 4096);
    {
      const R::KindScope k(R::Kind::kVb);
      R::WalkScope r(1, false, (P4 << 12) + 0x100u, 0x48u);
      R::candPage(P4, R::Site::kDrain, 40, true);
    }
    R::modelPage(P4, true, R::Why::kContent, R::Prov::k8);  // first use: the registry's copy
    const R::Totals& a = snapA();
    g_win8[0x14A] = 0x5A;  // the third dword of chunk 0x140 (beyond the request's 0x148)
    R::modelPage(P4, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& b0 = snapB();
    const std::string m0 = R::censusLastSampleForTest(1);
    check(O(b0, R::Outcome::kPageOnly) - O(a, R::Outcome::kPageOnly) == 1 &&
              O(b0, R::Outcome::kMissed) == O(a, R::Outcome::kMissed) &&
              m0.find(" at=148 mdw=3 bytes=404040404040404040405A40") != std::string::npos &&
              m0.find(" reqs=01680100+48:vb/f last=01680100+48:vb/f via=-") != std::string::npos,
          "C32 the straddled chunk's unfetched dword: PAGE_ONLY at=148 mdw=3 (dword-exact), its bytes and range: " + m0);
    g_win8[0x145] = 0x5B;  // the chunk's second dword: fetched
    R::modelPage(P4, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& b = snapA();
    const std::string m = R::censusLastSampleForTest(0);
    check(O(b, R::Outcome::kMissed) - O(b0, R::Outcome::kMissed) == 1 && m.find(" at=144 mdw=3 bytes=") != std::string::npos &&
              m.find(" via=f") != std::string::npos,
          "C32 ... its fetched dword: MISSED at=144, marked by a fallback's declared range (memres6: /f, via=f): " + m);
    g_hostPage = P5;
    std::memset(g_win8.data(), 0x41, 4096);
    std::memset(g_winA.data(), 0x41, 4096);
    {
      const R::KindScope k(R::Kind::kVb);
      R::WalkScope r(1, false, P5 << 12, 0x40u);
      R::candPage(P5, R::Site::kDrain, 41, true);
    }
    R::modelPage(P5, true, R::Why::kContent, R::Prov::k8);
    g_win8[0x808] = 0x77;  // a header's fence, no request near it
    R::modelPage(P5, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& c = snapB();
    const std::string p = R::censusLastSampleForTest(1);
    check(O(c, R::Outcome::kPageOnly) - O(b, R::Outcome::kPageOnly) == 1 &&
              p.find(" at=808 mdw=0 bytes=414141414141414177") != std::string::npos &&
              p.find(" reqs=- last=01690000+40:vb/f via=-") != std::string::npos,
          "C32 a page-only decision: its first changed dword (unfetched), no covering range: " + p);
    check(c.decisions - b.decisions == 2 && O(c, R::Outcome::kMissed) == O(b, R::Outcome::kMissed),
          "C32 ... two decisions (its first use, the page-only one), no further miss");
    g_hostPage = 0xFFFFFFFFu;
  }
  // C54 (mem-native; after C32, whose samples share its attribute cells): the stamp racing a decision. The decision
  // takes the page's pending patches first, then compares; a stamp noted (before its store, 201b29d4) and stored
  // between the two leaves a moved fetched dword whose note is pending - race.note (census_patch race=), and the next
  // decision's take applies it (no miss). The control: the same store with no note is MISSED. A pending note hides
  // nothing else: an unannounced fetched write beside it is MISSED. mrc_dw4: the one Warworld miss was the fence-stamp
  // header dword (17166218).
  {
    static uint32_t raceHp = 0, raceVal = 0, raceExtra = 0;
    static bool raceNote = false;
    constexpr uint32_t fenceOff = 0x188u;
    raceHp = 0x1640u;
    g_hostPage = raceHp;
    std::memset(g_win8.data(), 0x40, 4096);
    std::memset(g_winA.data(), 0x40, 4096);
    {
      R::WalkScope r(1, false, (raceHp << 12) + 0x100u, 0x200u);
      const R::KindScope k(R::Kind::kVb);
      R::candPage(raceHp, R::Site::kDrain, 40, true);
    }
    R::modelPage(raceHp, true, R::Why::kContent, R::Prov::k8);  // first use: the registry's copy recorded
    auto racer = [](uint32_t page) {
      if (page != raceHp || !raceVal) return;
      if (raceNote) R::notePatch(0x80000000u + (raceHp << 12) + fenceOff, 4u);  // the hook notes before the store
      g_win8[fenceOff + 3] = uint8_t(raceVal);
      if (raceExtra) g_win8[0x104] = uint8_t(raceExtra);
      raceVal = 0;
    };
    hooks.afterTake = racer;
    R::censusSetTestHooks(&hooks);
    const R::Totals& a = snapA();
    raceVal = 0x21;
    raceNote = true;
    R::modelPage(raceHp, true, R::Why::kContent, R::Prov::k8);  // take (nothing) -> note + store -> compare
    const R::Totals& b = snapB();
    const std::string line = R::censusText("census", 1);
    check(O(b, R::Outcome::kRaceNote) - O(a, R::Outcome::kRaceNote) == 1 &&
              O(b, R::Outcome::kMissed) == O(a, R::Outcome::kMissed) &&
              line.find(" nocopy=0 race=1") != std::string::npos,
          "C54 a stamp noted and stored between the decision's take and its comparison: race.note, census_patch race=1");
    R::modelPage(raceHp, true, R::Why::kContent, R::Prov::k8);  // the next decision takes the pending note
    const R::Totals& c = snapA();
    check(O(c, R::Outcome::kMissed) == O(b, R::Outcome::kMissed) &&
              O(c, R::Outcome::kRaceNote) == O(b, R::Outcome::kRaceNote) &&
              O(c, R::Outcome::kPageOnly) - O(b, R::Outcome::kPageOnly) == 1,
          "C54 the next decision's take applies the raced note: page-only, no miss");
    raceVal = 0x22;
    raceNote = false;  // the control: the same store with no note
    R::modelPage(raceHp, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& d = snapB();
    check(O(d, R::Outcome::kMissed) - O(c, R::Outcome::kMissed) == 1 &&
              O(d, R::Outcome::kRaceNote) == O(c, R::Outcome::kRaceNote),
          "C54 control: the same racing store with no note is MISSED");
    R::notePatch(0x80000000u + (raceHp << 12) + fenceOff, 4u);  // settle the control's dword (its note, late)
    R::modelPage(raceHp, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& e = snapA();
    raceVal = 0x23;
    raceNote = true;
    raceExtra = 0x42;  // a noted stamp and an unannounced fetched vertex dword in the same race
    R::modelPage(raceHp, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& f = snapB();
    check(O(f, R::Outcome::kMissed) - O(e, R::Outcome::kMissed) == 1 &&
              O(f, R::Outcome::kRaceNote) == O(e, R::Outcome::kRaceNote),
          "C54 a pending note hides nothing else: an unannounced fetched write in the same race is MISSED");
    raceExtra = 0;
    hooks.afterTake = nullptr;
    R::censusSetTestHooks(&hooks);
    g_hostPage = 0xFFFFFFFFu;
  }
  // C33-C38 (memres5, index-bounded fetched bytes): memres4's Warworld misses were header fence words inside a vertex
  // fetch's DECLARED range (mdw=F, dw_in_fetch=1). A draw's fetched bytes are the vertices its indices address -
  // [base + (vMin * stride + minOffset) * 4, base + (vMax * stride + maxEnd) * 4) - when every vfetch of the slot takes
  // its index from r0.x directly; else (fail-closed) the declared range, counted.
  auto fb = [](const R::Totals& t) { return t.fetchFbShader + t.fetchFbDraw; };
  // One draw of slot 0 over the page's declared range [0, 0x800) with indices 9..0 (stride 4 dwords: vertices
  // [0, 0xA0)), its page then copied by the registry (first use).
  auto drawOnce = [](uint32_t page, uint64_t vs, uint32_t slot) {
    std::vector<uint16_t> idx(10);
    for (uint32_t i = 0; i < 10; i++) idx[i] = uint16_t(9 - i);
    {
      const R::KindScope k(R::Kind::kVb);
      const R::DrawScope d(vs, true, reinterpret_cast<const uint8_t*>(idx.data()), 10, false, 0, 0, false, 0, 0xFFFFFF,
                           false, 0xFFFF);
      const R::SlotScope sl(slot);
      R::WalkScope r(1, false, page << 12, 0x800u);
      R::candPage(page, R::Site::kDrain, 50, true);
    }
    R::modelPage(page, true, R::Why::kContent, R::Prov::k8);
  };
  {
    const uint32_t P6 = 0x16A0u;
    const uint64_t H1 = 0x5A5A000000000001ull;
    const std::vector<uint8_t> u1 = vsUcode({vfetch(0, 4, 0, 38)});  // r1 <- vf0[r0.x], 16-byte records (32x4 float)
    R::noteVsUcode(H1, u1.data(), u1.size());
    g_hostPage = P6;
    std::memset(g_win8.data(), 0x60, 4096);
    std::memset(g_winA.data(), 0x60, 4096);
    const R::Totals& a = snapA();
    drawOnce(P6, H1, 0);
    const R::Totals& b = snapB();
    check(b.fetchBounded - a.fetchBounded == 1 && fb(b) == fb(a),
          "C33 a slot whose vfetch indexes r0.x directly: the request's fetched bytes are index-bounded (fetch.bounded)");
    g_win8[0x108] = 0x11;  // a header's fence: inside the declared range, past the last indexed vertex
    R::modelPage(P6, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& c = snapA();
    check(O(c, R::Outcome::kPageOnly) - O(b, R::Outcome::kPageOnly) == 1 &&
              O(c, R::Outcome::kMissed) == O(b, R::Outcome::kMissed),
          "C33 a write past the last indexed vertex (inside the declared range): PAGE_ONLY (the declared range: MISSED)");
    g_win8[0x40] = 0x12;  // vertex 4
    R::modelPage(P6, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& d = snapB();
    check(O(d, R::Outcome::kMissed) - O(c, R::Outcome::kMissed) == 1, "C33 a write to an indexed vertex: MISSED");
    g_hostPage = 0xFFFFFFFFu;
  }
  // C55 (mem-native): a direct slot's fetched bytes are the vertices its draw's indices USE. mrc_race1's Warworld
  // misses: SwapMeshChunkBindings draw-record dwords (171561A8) between the vertices a neighbour VB's draws use, inside
  // their [vMin, vMax] span; the fetch capture found them in no sampled draw's exact read set. Indices 0-3 and 8-9 (C33's
  // shader: 4 dwords per vertex): vertices 4-7 ([0x40, 0x80)) are not fetched - a write there is PAGE_ONLY, a write to
  // vertex 8 MISSED; an auto-indexed draw over 0..9 still fetches vertex 5 (its span is its one run).
  {
    auto kv = [](const std::string& t, const char* key) -> uint64_t {
      const size_t at = t.find(std::string(" ") + key + "=");
      return at == std::string::npos ? ~0ull : std::strtoull(t.c_str() + at + std::strlen(key) + 2, nullptr, 10);
    };
    const uint32_t P = 0x17A0u, Q = 0x17A4u;
    const uint64_t H1 = 0x5A5A000000000001ull;  // C33's: r1 <- vf0[r0.x], 4 dwords per vertex
    g_hostPage = P;
    std::memset(g_win8.data(), 0x63, 4096);
    std::memset(g_winA.data(), 0x63, 4096);
    const std::vector<uint16_t> idx = {0, 1, 2, 3, 9, 8, 9, 3};
    const std::string t0 = R::censusText("census", 1);
    const R::Totals& a = snapA();
    {
      const R::KindScope k(R::Kind::kVb);
      const R::DrawScope d(H1, true, reinterpret_cast<const uint8_t*>(idx.data()), uint32_t(idx.size()), false, 0, 0,
                           false, 0, 0xFFFFFF, false, 0xFFFF);
      const R::SlotScope sl(0);
      R::WalkScope r(1, false, P << 12, 0x800u);
      R::candPage(P, R::Site::kDrain, 52, true);
    }
    R::modelPage(P, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& b = snapB();
    const std::string t1 = R::censusText("census", 1);
    check(b.fetchBounded - a.fetchBounded == 1 && fb(b) == fb(a) && kv(t1, "vtx_exact") - kv(t0, "vtx_exact") == 1 &&
              kv(t1, "vtx_runs") - kv(t0, "vtx_runs") == 2,
          "C55 an indexed draw's direct slot: bounded, marked per used vertex - two runs (census_patch vtx_exact / "
          "vtx_runs)");
    g_win8[0x50] = 0x31;  // vertex 5: no index uses it (the [vMin, vMax] span covers it)
    R::modelPage(P, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& c = snapA();
    check(O(c, R::Outcome::kPageOnly) - O(b, R::Outcome::kPageOnly) == 1 &&
              O(c, R::Outcome::kMissed) == O(b, R::Outcome::kMissed),
          "C55 a write to a vertex between the used runs: PAGE_ONLY (the span read it MISSED)");
    g_win8[0x84] = 0x32;  // vertex 8: used
    R::modelPage(P, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& d = snapB();
    check(O(d, R::Outcome::kMissed) - O(c, R::Outcome::kMissed) == 1, "C55 a write to a used vertex after the gap: MISSED");
    g_hostPage = Q;
    std::memset(g_win8.data(), 0x64, 4096);
    std::memset(g_winA.data(), 0x64, 4096);
    {
      const R::KindScope k(R::Kind::kVb);
      const R::DrawScope dd(H1, false, nullptr, 10, false, 0, 0, false, 0, 0xFFFFFF, false, 0xFFFF);  // auto-indexed
      const R::SlotScope sl(0);
      R::WalkScope r(1, false, Q << 12, 0x800u);
      R::candPage(Q, R::Site::kDrain, 53, true);
    }
    R::modelPage(Q, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& e = snapA();
    g_win8[0x50] = 0x33;  // vertex 5 of the auto-indexed span
    R::modelPage(Q, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& f = snapB();
    check(O(f, R::Outcome::kMissed) - O(e, R::Outcome::kMissed) == 1,
          "C55 an auto-indexed draw's span is one run: its vertex 5 is fetched, the write MISSED");
    g_hostPage = 0xFFFFFFFFu;
  }
  // C58 (mem-native): a miss whose moved dwords only an earlier owner version's draws fetched is still MISSED, counted
  // missed_prior (classification; Astra round 4: the marks are never cleared). markf4_ww_census: the record dword
  // 171561A8 was marked fetched in the title era by a vertex buffer that then occupied the memory; the Warworld arena
  // reused it for SwapMesh records ~50 s later. A dword fetched under version 5, the page re-owned (6) and written,
  // a request that does not fetch it: MISSED, missed_prior +1 (owner_clears +1); the control (no re-owning): MISSED,
  // missed_prior +0.
  {
    const uint64_t H1 = 0x5A5A000000000001ull;  // C33's: 4 dwords per vertex
    auto kv = [](const std::string& t, const char* key) -> uint64_t {
      const size_t at = t.find(std::string(" ") + key + "=");
      return at == std::string::npos ? ~0ull : std::strtoull(t.c_str() + at + std::strlen(key) + 2, nullptr, 10);
    };
    const std::vector<uint16_t> first = {0, 1, 2, 3, 4, 5}, later = {0, 1};
    auto draw = [&](uint32_t page, const std::vector<uint16_t>& idx, uint32_t clock) {
      {
        const R::KindScope k(R::Kind::kVb);
        const R::DrawScope d(H1, true, reinterpret_cast<const uint8_t*>(idx.data()), uint32_t(idx.size()), false, 0,
                             0, false, 0, 0xFFFFFF, false, 0xFFFF);
        const R::SlotScope sl(0);
        R::WalkScope r(1, false, page << 12, 0x800u);
        R::candPage(page, R::Site::kDrain, clock, true);
      }
      R::modelPage(page, true, R::Why::kContent, R::Prov::k8);
    };
    for (int reown = 1; reown >= 0; reown--) {
      const uint32_t P = reown ? 0x17B0u : 0x17B4u;
      g_hostPage = P;
      std::memset(g_win8.data(), 0x66, 4096);
      std::memset(g_winA.data(), 0x66, 4096);
      const uint32_t v0 = g_ownerVer;
      draw(P, first, 55);  // vertices 0-5 fetched under the version now
      const std::string t0 = R::censusText("census", 1);
      if (reown) g_ownerVer = v0 + 1u;  // the page re-owned: another tenancy
      g_win8[0x50] = 0x43;  // vertex 5 (fetched by the first tenancy only)
      const R::Totals& a = snapA();
      draw(P, later, 56);  // the new tenancy's draw fetches vertices 0-1
      const R::Totals& b = snapB();
      const std::string t1 = R::censusText("census", 1);
      if (reown)
        check(O(b, R::Outcome::kMissed) - O(a, R::Outcome::kMissed) == 1 &&
                  kv(t1, "missed_prior") - kv(t0, "missed_prior") == 1 &&
                  kv(t1, "owner_clears") - kv(t0, "owner_clears") == 1,
              "C58 re-owned: vertex 5 (the first tenancy's mark only) written: MISSED, missed_prior +1");
      else
        check(O(b, R::Outcome::kMissed) - O(a, R::Outcome::kMissed) == 1 &&
                  kv(t1, "missed_prior") == kv(t0, "missed_prior"),
              "C58 control: the same tenancy - vertex 5 MISSED, not prior");
      g_ownerVer = v0;
    }
    g_hostPage = 0xFFFFFFFFu;
  }
  // C59 (mem-native, Astra round 4 P1-d): the draw's converted indices checked against the used-vertex bitmap - an
  // index it lacks (the guest rewrote its index buffer after the census read it) counts vtx_stale (a coverage GAP).
  // C60 (P1-c): a direct slot whose fetch ROUNDS its index, indices at or above 2^23: the declared range (fallback).
  {
    auto kv = [](const std::string& t, const char* key) -> uint64_t {
      const size_t at = t.find(std::string(" ") + key + "=");
      return at == std::string::npos ? ~0ull : std::strtoull(t.c_str() + at + std::strlen(key) + 2, nullptr, 10);
    };
    const uint64_t H1 = 0x5A5A000000000001ull;
    const std::vector<uint16_t> idx = {0, 1, 2, 3};
    const uint32_t same[] = {3, 0, 0xFFFFFFFFu, 2}, moved[] = {0, 5}, gap[] = {0, 1, 3};
    const std::string t0 = R::censusText("census", 1);
    {
      const R::DrawScope d(H1, true, reinterpret_cast<const uint8_t*>(idx.data()), 4, false, 0, 0, false, 0, 0xFFFFFF,
                           false, 0xFFFF);
      R::censusCheckUsed(same, 4);
    }
    const std::string t1 = R::censusText("census", 1);
    {
      const std::vector<uint16_t> holes = {0, 3};
      const R::DrawScope d(H1, true, reinterpret_cast<const uint8_t*>(idx.data()), 4, false, 0, 0, false, 0, 0xFFFFFF,
                           false, 0xFFFF);
      R::censusCheckUsed(moved, 2);  // 5: past vMax
      const R::DrawScope d2(H1, true, reinterpret_cast<const uint8_t*>(holes.data()), 2, false, 0, 0, false, 0,
                            0xFFFFFF, false, 0xFFFF);
      R::censusCheckUsed(gap, 3);  // 1: inside [0, 3], not used
    }
    const std::string t2 = R::censusText("census", 1);
    check(kv(t1, "vtx_stale") == kv(t0, "vtx_stale") && kv(t2, "vtx_stale") - kv(t1, "vtx_stale") == 2,
          "C59 converted indices the bitmap holds: no vtx_stale; one past the range and one in a hole: vtx_stale +2");
    const uint64_t HR = 0x5A5A0000000000B1ull;
    Ins fr = vfetch(0, 1, 0, 36);
    fr.d1 |= 1u << 15;  // round the index (is_index_rounded)
    const std::vector<uint8_t> ur = vsUcode({fr});
    R::noteVsUcode(HR, ur.data(), ur.size());
    const std::vector<uint32_t> big = {0x800001u, 0x800004u};
    auto req = [&](uint64_t vs, uint32_t page) {
      const R::KindScope k(R::Kind::kVb);
      const R::DrawScope d(vs, true, reinterpret_cast<const uint8_t*>(big.data()), 2, true, 0, 0, false, 0, 0xFFFFFF,
                           false, 0xFFFFFFFF);
      const R::SlotScope sl(0);
      R::WalkScope r(1, false, page << 12, 0x800u);
      R::skipDeferred(page);
    };
    const uint64_t fb0 = snapA().fetchFbShader, bd0 = snapA().fetchBounded;
    req(HR, 0x17C0u);
    const uint64_t fb1 = snapB().fetchFbShader, bd1 = snapB().fetchBounded;
    req(H1, 0x17C4u);
    const uint64_t fb2 = snapA().fetchFbShader, bd2 = snapA().fetchBounded;
    check(fb1 - fb0 == 1 && bd1 == bd0 && bd2 - bd1 == 1 && fb2 == fb1,
          "C60 a rounded direct fetch over indices >= 2^23: the declared range; a truncating one: still bounded");
  }
  // C57 (mem-native, Astra round 3): the used-vertex bitmap only while every index is below 2^23 (a rounded fetch of
  // 0x800001 reads 0x800002 in float32) - above it the span; and it comes from one snapshot of the index bytes.
  {
    const uint64_t H1 = 0x5A5A000000000001ull;
    const std::vector<uint32_t> lo = {0x7FFF00u, 0x7FFFFFu}, hi = {0x800001u, 0x800004u};
    bool exLo, exHi;
    {
      const R::DrawScope d(H1, true, reinterpret_cast<const uint8_t*>(lo.data()), 2, true, 0, 0, false, 0, 0xFFFFFF,
                           false, 0xFFFFFFFF);
      exLo = R::censusDrawExactForTest();
    }
    {
      const R::DrawScope d(H1, true, reinterpret_cast<const uint8_t*>(hi.data()), 2, true, 0, 0, false, 0, 0xFFFFFF,
                           false, 0xFFFFFFFF);
      exHi = R::censusDrawExactForTest();
    }
    check(exLo && !exHi, "C57 indices below 2^23: the used-vertex bitmap; an index at or above 2^23: the span");
  }
  // C56 (mem-native): a used vertex fetches its slot's ELEMENT dwords, not its whole stride. exact2_ww_census: a run's
  // span covered the record dword between the elements (VS E4C1704E825A22E9's slot 95 reads dwords 0..3 of 9). A
  // 9-dword stride read at dwords 0..3: vertex 1's dword 5 is not fetched (PAGE_ONLY), its dword 2 is (MISSED); two
  // elements 0..3 and 6..8: dword 5 (between them) PAGE_ONLY, dword 7 MISSED.
  {
    const std::vector<uint16_t> idx = {0, 1, 2, 3};
    struct Case {
      uint32_t page;
      uint64_t vs;
      std::vector<uint8_t> u;
      uint32_t gapDw, readDw;
      const char* name;
    };
    const Case cases[] = {
        {0x17A8u, 0x5A5A0000000000A1ull, vsUcode({vfetch(0, 9, 0, 38)}), 5u, 2u, "one element, dwords 0..3 of 9"},
        {0x17ACu, 0x5A5A0000000000A2ull, vsUcode({vfetch(0, 9, 0, 38), vfetch(0, 9, 6, 57)}), 5u, 7u,
         "two elements, dwords 0..3 and 6..8 of 9"},
    };
    for (const Case& k : cases) {
      R::noteVsUcode(k.vs, k.u.data(), k.u.size());
      g_hostPage = k.page;
      std::memset(g_win8.data(), 0x65, 4096);
      std::memset(g_winA.data(), 0x65, 4096);
      {
        const R::KindScope kk(R::Kind::kVb);
        const R::DrawScope d(k.vs, true, reinterpret_cast<const uint8_t*>(idx.data()), uint32_t(idx.size()), false, 0,
                             0, false, 0, 0xFFFFFF, false, 0xFFFF);
        const R::SlotScope sl(0);
        R::WalkScope r(1, false, k.page << 12, 0x800u);
        R::candPage(k.page, R::Site::kDrain, 54, true);
      }
      R::modelPage(k.page, true, R::Why::kContent, R::Prov::k8);
      const R::Totals& a = snapA();
      g_win8[(9u + k.gapDw) * 4u + 1u] = 0x41;  // vertex 1's dword between / past its elements
      R::modelPage(k.page, true, R::Why::kContent, R::Prov::k8);
      const R::Totals& b = snapB();
      check(O(b, R::Outcome::kPageOnly) - O(a, R::Outcome::kPageOnly) == 1 &&
                O(b, R::Outcome::kMissed) == O(a, R::Outcome::kMissed),
            std::string("C56 ") + k.name + ": a used vertex's dword no element reads is PAGE_ONLY");
      g_win8[(9u + k.readDw) * 4u + 1u] = 0x42;  // vertex 1's element dword
      R::modelPage(k.page, true, R::Why::kContent, R::Prov::k8);
      const R::Totals& c = snapA();
      check(O(c, R::Outcome::kMissed) - O(b, R::Outcome::kMissed) == 1,
            std::string("C56 ") + k.name + ": its element's dword is MISSED");
    }
    g_hostPage = 0xFFFFFFFFu;
  }
  // C34 / C35 / C36 (fail-closed): a relatively addressed index, an instruction writing r0 before the vfetch, a jump in
  // the program - the slot falls back to the declared range (fetch.fallback.shader), and the same fence write is MISSED.
  {
    struct Case {
      const char* name;
      uint32_t page;
      uint64_t vs;
      std::vector<uint8_t> u;
    };
    const Case cases[] = {
        {"C34 a relatively addressed vfetch index", 0x16B0u, 0x5A5A000000000002ull, vsUcode({vfetch(0, 4, 0, 38, 0, true)})},
        {"C35 r0 written (an ALU) before the vfetch", 0x16C0u, 0x5A5A000000000003ull,
         vsUcode({aluWrites(0), vfetch(0, 4, 0, 38)})},
        {"C36 a backward jump in the program (cond_jmp to CF 0)", 0x16C8u, 0x5A5A000000000004ull, vsUcode({vfetch(0, 4, 0, 38)}, 11u)},
        {"C36 an index register other than r0", 0x16CCu, 0x5A5A000000000005ull, vsUcode({vfetch(0, 4, 0, 38, 3)})},
        {"C36 a vfetch writing r0 before the slot's next vfetch", 0x16CEu, 0x5A5A000000000006ull,
         vsUcode({vfetch(1, 4, 0, 38, 0, false, 0), vfetch(0, 4, 0, 38)})},
        {"C35 r0 written by an ALU's scalar destination before the vfetch", 0x16B4u, 0x5A5A000000000009ull,
         vsUcode({aluScalarWrites(0), vfetch(0, 4, 0, 38)})},
        {"C36 an element format the census does not know", 0x16B8u, 0x5A5A00000000000Aull,
         vsUcode({vfetch(0, 4, 0, 0)})},
        {"C36 a stride of 0", 0x16BCu, 0x5A5A00000000000Bull, vsUcode({vfetch(0, 0, 0, 38)})},
        {"C36 two strides for one slot", 0x16C4u, 0x5A5A00000000000Cull,
         vsUcode({vfetch(0, 4, 0, 38), vfetch(0, 8, 0, 38)})},
        {"C36 a loop in the program (loop_start)", 0x1760u, 0x5A5A000000000010ull, vsUcode({vfetch(0, 4, 0, 38)}, 7u)},
        {"C36 a cond_jmp forward by address but its direction bit set", 0x1764u, 0x5A5A000000000011ull,
         vsProgram({{2u, {vfetch(0, 4, 0, 38)}}, {11u, {}, 5u, 2u}})},
        {"C35 an ALU writing r0.x alone", 0x1768u, 0x5A5A000000000012ull,
         vsUcode({aluWrites(0, 1u), vfetch(0, 4, 0, 38)})},
        {"C36 a mini fetch after its full fetch's conditional exec", 0x176Cu, 0x5A5A000000000013ull,
         vsProgram({{3u, {vfetch(0, 4, 0, 38)}}, {2u, {vfetchMini(4, 38)}}})},
        {"C36 a mini fetch after a predicated full fetch", 0x1770u, 0x5A5A000000000014ull,
         vsUcode({predicated(vfetch(0, 4, 0, 38)), vfetchMini(4, 38)})},
        {"C36 a mini fetch after a forward jump over its full fetch's exec", 0x1774u, 0x5A5A000000000015ull,
         vsProgram({{11u, {}, 3u, 0u}, {1u, {vfetch(0, 4, 0, 38)}}, {2u, {vfetchMini(4, 38)}}})},
        // memres6: an r0.x write by an instruction outside the computed walk's set - the walk refuses (op), so the
        // direct walk's r0 check alone decides the slot
        {"C35 r0.x written by a vector op outside the interpreter's set (dp4) before the vfetch", 0x1778u,
         0x5A5A000000000016ull, vsUcode({alu(15, 0, 1u, Rg(1), Rg(1), Rg(1)), vfetch(0, 4, 0, 38)})},
        {"C35 r0.x written by a scalar op outside the interpreter's set (exp) before the vfetch", 0x177Cu,
         0x5A5A000000000017ull, vsUcode({alu(0, 0, 0u, Rg(1), Rg(1), Rg(1), 14, 0, 1u), vfetch(0, 4, 0, 38)})},
    };
    for (const Case& k : cases) {
      R::noteVsUcode(k.vs, k.u.data(), k.u.size());
      g_hostPage = k.page;
      std::memset(g_win8.data(), 0x61, 4096);
      std::memset(g_winA.data(), 0x61, 4096);
      const R::Totals& a = snapA();
      drawOnce(k.page, k.vs, 0);
      g_win8[0x108] = 0x13;
      R::modelPage(k.page, true, R::Why::kContent, R::Prov::k8);
      const R::Totals& b = snapB();
      check(b.fetchFbShader - a.fetchFbShader == 1 && b.fetchBounded == a.fetchBounded &&
                O(b, R::Outcome::kMissed) - O(a, R::Outcome::kMissed) == 1,
            std::string(k.name) + ": falls back to the declared range (fetch.fallback.shader); the fence write MISSED");
    }
    // a safe shader, its slot 0 bounded, its slot 1 (r0 already overwritten) not
    const uint64_t H7 = 0x5A5A000000000007ull;
    const std::vector<uint8_t> u7 = vsUcode({vfetch(0, 4, 0, 38, 0, false, 0), vfetch(1, 4, 0, 38)});
    R::noteVsUcode(H7, u7.data(), u7.size());
    const R::Totals& a = snapA();
    drawOnce(0x16D8u, H7, 0);
    drawOnce(0x16DCu, H7, 1);
    drawOnce(0x16DEu, H7, 7);  // a slot the shader never fetches
    const R::Totals& b = snapB();
    check(b.fetchBounded - a.fetchBounded == 1 && b.fetchFbShader - a.fetchFbShader == 2,
          "C36 per slot: the first vfetch (before r0 is overwritten) bounded, the second and an unknown slot fall back");
    // an export (export_data) and a GPR other than r0 written before the vfetch: no r0 write, the slot stays bounded
    const uint64_t H13 = 0x5A5A00000000000Dull;
    const std::vector<uint8_t> u13 = vsUcode({aluExports(), aluWrites(2), aluScalarWrites(3), vfetch(0, 4, 0, 38)});
    R::noteVsUcode(H13, u13.data(), u13.size());
    const R::Totals& c = snapA();
    drawOnce(0x16D0u, H13, 0);
    const R::Totals& d = snapB();
    check(d.fetchBounded - c.fetchBounded == 1 && d.fetchFbShader == c.fetchFbShader,
          "C35 an export and writes of r2 / r3 before the vfetch are no r0 write: bounded");
    // what the walk follows (each bounded): only r0.x is the index; forward control flow runs a subsequence of the walk
    struct Ok {
      const char* name;
      uint32_t page;
      uint64_t vs;
      std::vector<uint8_t> u;
    };
    const Ok oks[] = {
        {"C35 an ALU writing r0.yzw (not the index's x)", 0x1780u, 0x5A5A000000000020ull,
         vsUcode({aluWrites(0, 0xEu), vfetch(0, 4, 0, 38)})},
        {"C35 a vfetch writing r0.yzw (x kept)", 0x1784u, 0x5A5A000000000021ull,
         vsUcode({vfetchSwz(1, 4, 38, 0, 7u | (1u << 3) | (2u << 6) | (3u << 9)), vfetch(0, 4, 0, 38)})},
        {"C36 a forward cond_jmp (an if block)", 0x1788u, 0x5A5A000000000022ull,
         vsProgram({{11u, {}, 2u, 0u}, {2u, {vfetch(0, 4, 0, 38)}}})},
        {"C36 a mini fetch in its full fetch's exec", 0x178Cu, 0x5A5A000000000023ull,
         vsUcode({vfetch(0, 4, 0, 38), vfetchMini(4, 38)})},
        {"C36 a mini fetch after an earlier unconditional exec", 0x1790u, 0x5A5A000000000024ull,
         vsProgram({{1u, {vfetch(0, 4, 0, 38)}}, {2u, {vfetchMini(4, 38)}}})},
    };
    for (const Ok& k : oks) {
      R::noteVsUcode(k.vs, k.u.data(), k.u.size());
      const R::Totals& e = snapA();
      drawOnce(k.page, k.vs, 0);
      const R::Totals& f = snapB();
      check(f.fetchBounded - e.fetchBounded == 1 && f.fetchFbShader == e.fetchFbShader,
            std::string(k.name) + ": followed, the slot index-bounded");
    }
    g_hostPage = 0xFFFFFFFFu;
  }
  // C37 (dword-exact under the bound): an IB header 4-byte aligned right after the last vertex dword shares its 16-byte
  // chunk (memres4: IB headers at 0x254 / 0x25C / 0x2AC): stride 1 dword, vertices 0..84 = [0, 0x154); the header's
  // fence at 0x15C is not fetched (PAGE_ONLY), the last vertex at 0x150 is (MISSED).
  {
    const uint32_t P9 = 0x16E0u;
    const uint64_t H8 = 0x5A5A000000000008ull;
    const std::vector<uint8_t> u8 = vsUcode({vfetch(0, 1, 0, 36)});  // one 32-bit float per vertex
    R::noteVsUcode(H8, u8.data(), u8.size());
    g_hostPage = P9;
    std::memset(g_win8.data(), 0x62, 4096);
    std::memset(g_winA.data(), 0x62, 4096);
    {
      const R::KindScope k(R::Kind::kVb);
      const R::DrawScope d(H8, true, true, 0, 84);
      const R::SlotScope sl(0);
      R::WalkScope r(1, false, P9 << 12, 0x400u);
      R::candPage(P9, R::Site::kDrain, 51, true);
    }
    R::modelPage(P9, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& a = snapA();
    g_win8[0x15C] = 0x14;  // the IB header's fence (Common at 0x154)
    R::modelPage(P9, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& b = snapB();
    check(O(b, R::Outcome::kPageOnly) - O(a, R::Outcome::kPageOnly) == 1 &&
              O(b, R::Outcome::kMissed) == O(a, R::Outcome::kMissed),
          "C37 a fence in the last vertex's 16-byte chunk, past the bound: PAGE_ONLY (dword-exact)");
    g_win8[0x150] = 0x15;  // the last vertex
    R::modelPage(P9, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& c = snapA();
    check(O(c, R::Outcome::kMissed) - O(b, R::Outcome::kMissed) == 1, "C37 ... the last vertex's own dword: MISSED");
    g_hostPage = 0xFFFFFFFFu;
  }
  // C38 (fail-closed, the draw): a vertex request outside any draw scope, and a draw whose index buffer could not be read
  // - the declared range (fetch.fallback.draw).
  {
    const uint64_t H1 = 0x5A5A000000000001ull;
    const R::Totals& a = snapA();
    {
      const R::KindScope k(R::Kind::kVb);
      R::WalkScope r(1, false, 0x16F0u << 12, 0x100u);
      R::skipDeferred(0x16F0u);
    }
    {
      const R::KindScope k(R::Kind::kVb);
      const R::DrawScope d(H1, true, nullptr, 10, false, 0, 0, false, 0, 0xFFFFFF, false, 0xFFFF);  // indexed, unreadable
      const R::SlotScope sl(0);
      R::WalkScope r(1, false, 0x16F1u << 12, 0x100u);
      R::skipDeferred(0x16F1u);
    }
    const R::Totals& b = snapB();
    check(b.fetchFbDraw - a.fetchFbDraw == 2 && b.fetchBounded == a.fetchBounded,
          "C38 no draw scope / an unreadable index buffer: the declared range (fetch.fallback.draw)");
  }
  // C39 (the draw's index range, as pm4StageVsf converts): the strip's reset index reads no vertex; offset, 24-bit mask
  // and clamp; auto-indexed draws; nothing left = no vertex.
  {
    uint32_t mn = 0, mx = 0;
    const uint16_t be16[] = {0x0500, 0x0200, 0x0900, 0xFFFF, 0x0700};  // 5 2 9 reset 7, byte-swapped (endian 2)
    const uint8_t* p16 = reinterpret_cast<const uint8_t*>(be16);
    check(R::vertexIndexRange(p16, 5, false, 2, 0, false, 0, 0xFFFFFF, true, 0xFFFF, mn, mx) && mn == 2 && mx == 9,
          "C39 a strip's reset index is skipped: [2, 9]");
    check(R::vertexIndexRange(p16, 5, false, 2, 0, false, 0, 0xFFFFFF, false, 0xFFFF, mn, mx) && mn == 2 && mx == 0xFFFF,
          "C39 ... a list reads it as a vertex: [2, 0xFFFF]");
    check(R::vertexIndexRange(p16, 3, false, 2, 10, true, 0, 12, false, 0xFFFF, mn, mx) && mn == 12 && mx == 12,
          "C39 offset 10 then the clamp [0, 12]: [12, 12]");
    check(R::vertexIndexRange(p16, 3, false, 2, 10, false, 0, 12, false, 0xFFFF, mn, mx) && mn == 12 && mx == 19,
          "C39 ... the clamp off: [12, 19]");
    const uint32_t be32v[] = {0x01000000u, 0x00010000u};  // 1, 256 byte-swapped (endian 2)
    check(R::vertexIndexRange(reinterpret_cast<const uint8_t*>(be32v), 2, true, 2, 0, false, 0, 0xFFFFFF, false, 0, mn,
                              mx) && mn == 1 && mx == 256,
          "C39 32-bit indices: [1, 256]");
    const uint32_t s16in32[] = {0x00050000u, 0x00020000u};  // 5, 2 as endian 3 (16-in-32) delivers them
    const uint32_t s8in16[] = {0x00000500u, 0x00000900u};   // 5, 9 as endian 1 (8-in-16)
    const uint16_t raw16[] = {7, 3};                         // endian 0: no swap
    check(R::vertexIndexRange(reinterpret_cast<const uint8_t*>(s16in32), 2, true, 3, 0, false, 0, 0xFFFFFF, false, 0, mn,
                              mx) && mn == 2 && mx == 5 &&
              R::vertexIndexRange(reinterpret_cast<const uint8_t*>(s8in16), 2, true, 1, 0, false, 0, 0xFFFFFF, false, 0,
                                  mn, mx) && mn == 5 && mx == 9 &&
              R::vertexIndexRange(reinterpret_cast<const uint8_t*>(raw16), 2, false, 0, 0, false, 0, 0xFFFFFF, false, 0,
                                  mn, mx) && mn == 3 && mx == 7,
          "C39 the swaps as the conversion's raw(): 32-bit endian 3 [2, 5], endian 1 [5, 9]; 16-bit endian 0 [3, 7]");
    check(R::vertexIndexRange(nullptr, 5, false, 0, 3, false, 0, 0xFFFFFF, false, 0, mn, mx) && mn == 3 && mx == 7,
          "C39 an auto-indexed draw of 5 from 3: [3, 7]");
    const uint16_t resets[] = {0xFFFF, 0xFFFF};
    check(!R::vertexIndexRange(reinterpret_cast<const uint8_t*>(resets), 2, false, 0, 0, false, 0, 0xFFFFFF, true,
                               0xFFFF, mn, mx) &&
              !R::vertexIndexRange(nullptr, 0, false, 0, 0, false, 0, 0xFFFFFF, false, 0, mn, mx),
          "C39 only resets / no index: no vertex");
  }
  // C40 (memres5, the registry's window): the live owner (the model's provenance 8 / A), else the last grant covering
  // the page (noteGrantWindow - MmAllocatePhysicalMemoryEx, the XMemAlloc override's physical and heap-routed grants -
  // kept after the grant's release), else 0x8. memres4's ALIAS pages: a released 0xA arena's textures, no live owner.
  {
    const uint32_t G1 = 0x1720u, G2 = 0x1730u, G3 = 0x1740u;
    auto fill = [](uint32_t page) {
      g_hostPage = page;
      std::memset(g_win8.data(), 0, 4096);  // the title wrote through 0xA only
      std::memset(g_winA.data(), 0x63, 4096);
    };
    R::noteGrantWindow(0xA0000000u | (G1 << 12), 0x2000u);  // an 0xA grant (released since: nothing tracks that)
    const R::Totals& a = snapA();
    fill(G1);
    R::modelPage(G1, false, R::Why::kContent, R::Prov::kUndecided);  // a keep under the legacy rule: 0xA by content
    const R::Totals& b = snapB();
    check(b.winAliasDiff == a.winAliasDiff && b.winGrant - a.winGrant == 1,
          "C40 no live owner, an 0xA last grant: the registry serves 0xA (win.grant), no win.alias_diff");
    fill(G2);
    R::modelPage(G2, false, R::Why::kContent, R::Prov::kUndecided);
    const R::Totals& c = snapA();
    check(c.winAliasDiff - b.winAliasDiff == 1 && c.winDefault - b.winDefault == 1,
          "C40 never granted: 0x8 (win.default) - the alias stays a gate counter");
    R::noteGrantWindow(0x80000000u | (G1 << 12), 0x1000u);  // a later heap-routed grant over G1: 0x8
    fill(G1);
    R::noteCoher(0x03000000u, G1 << 12, 0x1000u);
    R::modelPage(G1, true, R::Why::kContent, R::Prov::kUndecided);
    const R::Totals& d = snapB();
    check(d.winAliasDiff - c.winAliasDiff == 1 && d.winGrant - c.winGrant == 1,
          "C40 the last grant wins: a heap-routed 0x8 grant after the 0xA one serves 0x8");
    R::noteGrantWindow(0x80000000u | (G3 << 12), 0x1000u);
    fill(G3);
    R::modelPage(G3, true, R::Why::kContent, R::Prov::kA);  // the live owner names 0xA
    const R::Totals& e = snapA();
    check(e.winAliasDiff == d.winAliasDiff && e.winLive - d.winLive == 1,
          "C40 a live owner outranks the last grant (win.live)");
    g_hostPage = 0xFFFFFFFFu;
  }
  // C29: the grant holding a missed page (memres1's one readfile sample, page 0x17571000, lies in the resource arena
  // granted at va AAE30000 +0xDFF0000 by lr 826FAFB8).
  {
    R::noteGrant(0xAAE30000u, 0x0DFF0000u, 0x826FAFB8u);
    R::noteGrant(0xBEF2B000u, 0x1000u, 0x827C1260u);  // under 1 MiB: not remembered
    const uint32_t AP = 0x17571u, NP = 0x1700u;
    R::modelPage(AP, true, R::Why::kContent, R::Prov::k8);
    R::modelPage(NP, true, R::Why::kContent, R::Prov::k8);
    const R::Totals& a = snapA();
    R::modelPage(AP, true, R::Why::kContent, R::Prov::k8);  // a miss inside the arena
    R::modelPage(NP, true, R::Why::kContent, R::Prov::k8);  // a miss in no remembered grant
    const R::Totals& b = snapB();
    check(b.grants == 1 && b.grantVa[0] == 0xAAE30000u && b.grantLr[0] == 0x826FAFB8u &&
              b.missedGrant[0] - a.missedGrant[0] == 1 && b.missedGrant[R::kMaxGrants] - a.missedGrant[R::kMaxGrants] == 1,
          "C29 a miss in the arena counted under its grant (va, size, lr); one outside under none");
  }
  // C61 (mem-native, the orchestrator's 2026-10-17 decision; after C29, which counts the grants from the first): a
  // miss whose moved dwords a named CPU-only writer wrote (noteCpuOnly: RenderModel_SwapMeshChunkBindings' draw
  // records) is still MISSED, counted missed_cpu_only; the
  // control (no name) is not; the names survive an owner-version bump (the 64 KB block's counter is no tenancy:
  // cpuonly9's one unclassified miss) but not a re-grant of the page nor 2 s without a renaming; renamed under the
  // new grant they hold again; another dword of the page named does not cover the moved one.
  {
    static uint32_t fakeMs = 1000u;
    hooks.nowMs = [] { return fakeMs; };
    R::censusSetTestHooks(&hooks);
    R::noteGrant(0x80000000u + (0x17D0u << 12), 0x00100000u, 0x826FAFB8u);  // a census grant over C61's pages
    const uint64_t H1 = 0x5A5A000000000001ull;
    auto kv = [](const std::string& t, const char* key) -> uint64_t {
      const size_t at = t.find(std::string(" ") + key + "=");
      return at == std::string::npos ? ~0ull : std::strtoull(t.c_str() + at + std::strlen(key) + 2, nullptr, 10);
    };
    const std::vector<uint16_t> idx = {0, 1, 2, 3, 4, 5};
    auto draw = [&](uint32_t page, uint32_t clock) {
      {
        const R::KindScope k(R::Kind::kVb);
        const R::DrawScope d(H1, true, reinterpret_cast<const uint8_t*>(idx.data()), uint32_t(idx.size()), false, 0, 0,
                             false, 0, 0xFFFFFF, false, 0xFFFF);
        const R::SlotScope sl(0);
        R::WalkScope r(1, false, page << 12, 0x800u);
        R::candPage(page, R::Site::kDrain, clock, true);
      }
      R::modelPage(page, true, R::Why::kContent, R::Prov::k8);
    };
    struct Case {
      uint32_t page;
      bool name, reown, rename;
      uint64_t expect;
      const char* what;
      uint32_t nameOff = 0x50u;  // the dword named (the write is always vertex 5's first, 0x50)
      int regrant = 0;           // 1: a new census grant over the page after the naming; 2: 2.5 s pass after it
    };
    const Case cases[] = {
        {0x17D0u, true, false, false, 1, "C61 a moved dword the CPU-only writer wrote: MISSED, missed_cpu_only +1"},
        {0x17D4u, false, false, false, 0, "C61 control: the same write, no name - MISSED, not cpu_only"},
        {0x17D8u, true, true, false, 1, "C61 an owner-version bump after the naming (same grant): still cpu_only"},
        {0x17DCu, true, false, false, 0, "C61 the page re-granted after the naming: not cpu_only", 0x50u, 1},
        {0x17E0u, true, false, false, 0, "C61 another dword of the page named, the moved one not: not cpu_only", 0x54u},
        {0x17E4u, true, false, false, 0, "C61 2.5 s without a renaming: the names expired, not cpu_only", 0x50u, 2},
        {0x17E8u, true, false, true, 1, "C61 re-granted, then renamed under the new grant: cpu_only", 0x50u, 1},
    };
    for (const Case& k : cases) {
      g_hostPage = k.page;
      std::memset(g_win8.data(), 0x67, 4096);
      std::memset(g_winA.data(), 0x67, 4096);
      const uint32_t v0 = g_ownerVer;
      draw(k.page, 57);
      if (k.name) R::noteCpuOnly(0x80000000u + (k.page << 12) + k.nameOff, 4u);
      if (k.reown) g_ownerVer = v0 + 1u;
      if (k.regrant == 1) R::noteGrant(0x80000000u + (k.page << 12), 0x00100000u, 0x826FAFB8u);
      if (k.regrant == 2) fakeMs += 2500u;
      if (k.rename) R::noteCpuOnly(0x80000000u + (k.page << 12) + k.nameOff, 4u);
      g_win8[0x50] = 0x44;  // vertex 5, fetched
      const std::string t0 = R::censusText("census", 1);
      const R::Totals& a = snapA();
      draw(k.page, 58);
      const R::Totals& b = snapB();
      const std::string t1 = R::censusText("census", 1);
      check(O(b, R::Outcome::kMissed) - O(a, R::Outcome::kMissed) == 1 &&
                kv(t1, "missed_cpu_only") - kv(t0, "missed_cpu_only") == k.expect,
            k.what);
      g_ownerVer = v0;
    }
    hooks.nowMs = nullptr;
    R::censusSetTestHooks(&hooks);
    g_hostPage = 0xFFFFFFFFu;
  }
  check(sumsHold(snapA()), "C10 every attribution's marginal sums to its total; decisions = the outcome classes");
  // The census lines as a roll prints them (tools/perf/test_mem_res_census.py --difftest-out grades this output: the
  // grader must read exactly what this unit counted - the MEMRES_EXPECT line carries the unit's own totals).
  {
    const R::Totals& t = snapA();
    std::fflush(stdout);  // (the lines before complete in the file before the unit's own lines start)
    std::printf("MEMRES_EXPECT decisions=%llu missed=%llu missed.content=%llu missed.window=%llu missed.gpu=%llu "
                "missed.movie=%llu clobber=%llu race.note=%llu race.lock=%llu over=%llu agree_up=%llu agree_keep=%llu "
                "first=%llu win.alias_diff=%llu cov.unseen=%llu cov.unaccounted=%llu keep.wt.unchecked=%llu "
                "keep.rc.unchecked=%llu ib.missed=%llu page_only=%llu missed.stale=%llu fetch.bounded=%llu "
                "fetch.fallback.shader=%llu fetch.fallback.draw=%llu win.grant=%llu win.default=%llu "
                "fetch.bounded.computed=%llu bound.check.sampled=%llu bound.check.violations=%llu\n",
                (unsigned long long)t.decisions, (unsigned long long)O(t, R::Outcome::kMissed),
                (unsigned long long)t.missedWhy[0], (unsigned long long)t.missedWhy[1],
                (unsigned long long)t.missedWhy[2], (unsigned long long)t.missedMovie,
                (unsigned long long)O(t, R::Outcome::kClobber), (unsigned long long)O(t, R::Outcome::kRaceNote),
                (unsigned long long)O(t, R::Outcome::kRaceLock), (unsigned long long)O(t, R::Outcome::kOver),
                (unsigned long long)O(t, R::Outcome::kAgreeUp), (unsigned long long)O(t, R::Outcome::kAgreeKeep),
                (unsigned long long)t.first, (unsigned long long)t.winAliasDiff,
                (unsigned long long)C(t, R::Cov::kUnseen), (unsigned long long)t.covUnaccounted,
                (unsigned long long)K(t, R::Feed::kWt, R::Chk::kUnchecked),
                (unsigned long long)K(t, R::Feed::kRc, R::Chk::kUnchecked), (unsigned long long)t.ibMissed,
                (unsigned long long)O(t, R::Outcome::kPageOnly), (unsigned long long)t.missedWhy[3],
                (unsigned long long)t.fetchBounded, (unsigned long long)t.fetchFbShader,
                (unsigned long long)t.fetchFbDraw, (unsigned long long)t.winGrant, (unsigned long long)t.winDefault,
                (unsigned long long)t.fetchBoundedComp, (unsigned long long)t.boundSampled,
                (unsigned long long)t.boundViolations);
    // As the log prints them (the logger's local-time prefix; the samples above came through the logger).
    const std::string txt = R::censusText("final", 42);
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm lt{};
    localtime_s(&lt, &tt);
    char pre[64];
    std::snprintf(pre, sizeof pre, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [INFO ] [BOOT] ", lt.tm_year + 1900,
                  lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, ms);
    size_t s = 0, n = 0;
    while (s < txt.size()) {
      const size_t e = txt.find('\n', s);
      const std::string line = txt.substr(s, e == std::string::npos ? std::string::npos : e - s);
      if (!line.empty()) {
        std::printf("%s%s\n", pre, line.c_str());
        n++;
      }
      if (e == std::string::npos) break;
      s = e + 1;
    }
    // stdout is buffered when redirected and the logger's lines are not: flush now, or a later sample (C30's) lands
    // inside a census line the buffer still holds (memres3's first worker run: census_coher cut in two)
    std::fflush(stdout);
    check(n == 13 && txt.find("[MEM_RES] census final swaps=42 decisions=") == 0 &&
              txt.find("\n[MEM_RES] census_patch final notes=") != std::string::npos,
          "C12 thirteen census lines (census_patch the mem-native one), the totals first (why, swaps, decisions)");
    check(txt.find(" fetch.bounded.computed=") != std::string::npos &&
              txt.find(" bound.check.sampled=") != std::string::npos &&
              txt.find(" bound.check.violations=") != std::string::npos &&
              txt.find("\n[MEM_RES] census_vsfb final r.slot=") != std::string::npos &&
              txt.find(" r.lit=") != std::string::npos && txt.find("\n[MEM_RES] census_vsfb_top final top=") !=
                                                                std::string::npos,
          "C12 the memres6 keys: the computed bounds, the witness, census_vsfb (and its _top line)");
    check(txt.find("[MEM_RES] census_missed_grant final none=") != std::string::npos &&
              txt.find(" g.AAE30000+DFF0000@826FAFB8=1") != std::string::npos &&
              txt.find("[MEM_RES] census_coher final requests=") != std::string::npos &&
              txt.find(" calls.03000100=") != std::string::npos &&
              txt.find(" vb/cmdbuf/in/zp/unk=1") != std::string::npos,
          "C12 the memres2 lines: the grant's key, the statuses by value, the attribute cells");
    check(txt.find(" page_only=") != std::string::npos && txt.find(" missed.stale=1") != std::string::npos &&
              txt.find(" note.shsync=1") != std::string::npos && txt.find(" calls.00010000=1") != std::string::npos &&
              txt.find("/same=") != std::string::npos,
          "C12 the memres3 keys: page_only, missed.stale, note.shsync, the shader sync's status, the page-only cells");
    check(txt.find(" fetch.bounded=") != std::string::npos && txt.find(" fetch.fallback=") != std::string::npos &&
              txt.find(" fetch.fallback.shader=") != std::string::npos &&
              txt.find(" fetch.fallback.draw=") != std::string::npos && txt.find(" win.live=") != std::string::npos &&
              txt.find(" win.grant=") != std::string::npos && txt.find(" win.default=") != std::string::npos,
          "C12 the memres5 keys: the fetch bound and its fallbacks, the registry's window rules");
  }
  R::censusResetForTest(false);
  const R::Totals& z = snapA();
  check(z.decisions == 0 && O(z, R::Outcome::kMissed) == 0 && z.noteCalls[uint32_t(R::Src::kInval)] == 0 &&
            z.covReq == 0 && z.lockOpenPages == 0 && z.coherRequests == 0 && z.grants == 0 && z.traps == 0,
        "C11 censusResetForTest starts from zero");
  // C30: naming the writer (memres2) - a miss outside the command buffer arms a one-shot trap (the page write-
  // protected); its first write records the writer and releases the page; a miss inside the buffer arms none; a page is
  // trapped once; a failed protection is counted and leaves the page untrapped; a kernel-mode host write
  // (hostWillWrite, gww::HostWriteScope) releases an armed trap first and is recorded as writer=host; the limit holds;
  // the census line after a fire prints WRITER once.
  {
    R::censusResetForTest(true);
    hooks.trapLimit = 3;
    R::censusSetTestHooks(&hooks);
    g_protN = g_relN = 0;
    g_protOk = true;
    R::noteCoher(0x01000000u, CB0 << 12, (CB1 - CB0) << 12);  // the buffer's range known
    auto miss = [](uint32_t p) {
      R::modelPage(p, true, R::Why::kContent, R::Prov::k8);
      R::modelPage(p, true, R::Why::kContent, R::Prov::k8);
    };
    miss(0x1E930u);  // inside the command buffer
    check(g_protN == 0 && snapA().trapArmed == 0, "C30 a miss inside the command buffer arms no trap");
    miss(0x1800u);
    check(g_protN == 1 && g_protPage == 0x1800u && snapA().trapArmed == 1, "C30 a miss outside it arms a trap on its page");
    R::modelPage(0x1800u, true, R::Why::kContent, R::Prov::k8);
    check(g_protN == 1, "C30 the page missed again: trapped once");
    check(!R::censusTrapHitForTest(0x80001000u, 0x99u), "C30 a write to an untrapped page is not the census's");
    g_hostPage = 0x1800u;  // (memres4: the bytes at va - 8 readable - a header's first 32 bytes when va is its fence)
    for (uint32_t i = 0; i < 4096u; i++) g_winA[i] = uint8_t(i);
    check(R::censusTrapHitForTest(0xA1800024u, 0x1234u) && g_relN == 1 && g_relPage == 0x1800u,
          "C30 the first write fires the trap and releases the page");
    g_hostPage = 0xFFFFFFFFu;
    check(!R::censusTrapHitForTest(0xA1800028u, 0x1235u) && g_relN == 1, "C30 ... once (a later write is not recorded)");
    g_protOk = false;
    miss(0x1801u);
    g_protOk = true;
    const R::Totals& a = snapA();
    check(a.trapArmFail == 1 && a.trapArmed == 1 && !R::censusTrapHitForTest(0x81801000u, 1u),
          "C30 a failed protection: counted, the page left untrapped");
    {
      R::WalkScope r(1, false, 0x1802000u, 0x20u);  // a request fetching [0, 0x20) of page 0x1802 (memres3: in_fetch)
      R::skipDeferred(0x1802u);
    }
    miss(0x1802u);
    check(g_protPage == 0x1802u && snapB().trapArmed == 2, "C30 the next miss arms the third slot");
    R::hostWillWrite(0x81802010u, 0x20u);
    const R::Totals& b = snapB();
    check(b.trapFired == 2 && b.trapHost == 1 && g_relPage == 0x1802u,
          "C30 a kernel-mode host write releases an armed trap first (writer=host)");
    miss(0x1803u);
    check(g_protN == 3 && snapA().trapDeclined >= 1, "C30 the limit (3 slots) declines the next");
    const std::string t1 = R::censusText("census", 9);
    check(t1.find("[MEM_RES] WRITER #1 page=01800000 va=A1800024 writer=guest") != std::string::npos &&
              t1.find("[MEM_RES] WRITER #2 page=01802000 va=81802010 writer=host") != std::string::npos &&
              t1.find(" trap.armed=2 trap.fired=2 trap.host=1 trap.arm_fail=1") != std::string::npos,
          "C30 the census line prints each fired trap's WRITER and the trap counters");
    const size_t w1 = t1.find("[MEM_RES] WRITER #1 "), w2 = t1.find("[MEM_RES] WRITER #2 ");
    const std::string l1 = w1 == std::string::npos ? "" : t1.substr(w1, t1.find('\n', w1) - w1);
    const std::string l2 = w2 == std::string::npos ? "" : t1.substr(w2, t1.find('\n', w2) - w2);
    check(l1.find(" in_fetch=0") != std::string::npos && l2.find(" in_fetch=1") != std::string::npos,
          "C30 in_fetch: the write to a chunk no request fetched reads 0, the one to a fetched chunk 1 (memres3)");
    check(l1.find(" on=missed dw_in_fetch=0 req=- hdr=1C1D1E1F202122232425262728292A2B2C2D2E2F303132333435363738393A3B") !=
              std::string::npos &&
              l2.find(" on=missed dw_in_fetch=1 req=01802000+20:") != std::string::npos &&
              l2.find(" hdr=" + std::string(64, '0')) != std::string::npos,
          "C30 memres4: what armed the trap, the written dword's own fetch and its covering range, the 32 bytes at va - 8 "
          "(an unreadable page: zeros): " + l2);
    const std::string t2 = R::censusText("census", 10);
    check(t2.find("WRITER") == std::string::npos, "C30 ... each WRITER once");
  }
  // C41-C46 (memres6): from zero (each sample the first of its cell), no traps
  R::censusResetForTest(true);
  hooks.trapLimit = 0;
  R::censusSetTestHooks(&hooks);
  familyTests();
  R::censusSetLiteralsForTest(nullptr, 0);
  warworldLiteralTest();
  capturedFetchTests();
  fetchPayloadTests();
  fetchTargetSelectionTests();
  R::censusResetForTest(false);
  R::censusSetTestHooks(nullptr);
}
}  // namespace

int runMemResourcesTest() {
  std::printf("mem.resources step 2a: the registry and its shadow census\n");
  registryTests();
  censusTests();
  std::printf("mem resources test: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}

// memres6 (e), the offline differential test: tools/perf/mem_res_family.py reads the world family's draws from replay
// traces - each draw's VS ucode, its 1024 VS constant dwords and 192 fetch constant dwords, its index buffer and the
// conversion's parameters, F1's stream as the trace held it - and evaluates their EXACT index ranges per computed slot
// as the host translation computes them (its own float32 evaluation of XenosRecomp's semantics, the containers'
// literals), with adversarial variants of c4. This mode replays each draw through the census's production path (the
// generated literal table, noteVsUcode, the DrawScope that converts the index buffer itself) and requires: the census's
// vertex range = the tool's; for an exact range (status 0) the census's bound contains it, or the census falls back; for
// an index the host cannot form (status 2) the census falls back; the witness (every draw, K = 1) finds no violation.
// Status 1 (a prologue outside the tool's evaluator) is counted, not checked. Exit code = failures.
//   build/difftest.exe --mem-resources-family-test <fixture.bin>
namespace {
std::map<uint32_t, std::vector<uint8_t>> g_famPages;  // page -> its 4096 bytes (both windows)
const uint8_t* famHost(uint32_t va) {
  const auto it = g_famPages.find((va & 0x1FFFFFFFu) >> 12);
  return it == g_famPages.end() ? nullptr : it->second.data();
}
uint32_t famOwner(uint32_t) { return 1; }
struct FamReader {
  std::vector<uint8_t> b;
  size_t o = 0;
  bool ok = true;
  bool take(void* out, size_t n) {
    if (!ok || o + n > b.size()) return ok = false;
    std::memcpy(out, b.data() + o, n);
    o += n;
    return true;
  }
  uint32_t u32() {
    uint32_t v = 0;
    take(&v, 4);
    return v;
  }
  uint64_t u64() {
    uint64_t v = 0;
    take(&v, 8);
    return v;
  }
  int64_t i64() {
    int64_t v = 0;
    take(&v, 8);
    return v;
  }
  std::vector<uint8_t> bytes(size_t n) {
    std::vector<uint8_t> v;
    if (!ok || o + n > b.size()) {
      ok = false;
      return v;
    }
    v.assign(b.begin() + long(o), b.begin() + long(o + n));
    o += n;
    return v;
  }
};
}  // namespace

int runMemResourcesFamilyTest(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: difftest --mem-resources-family-test <fixture.bin> (tools/perf/mem_res_family.py --out)\n");
    return 2;
  }
  FamReader rd;
  {
    FILE* f = std::fopen(argv[2], "rb");
    if (!f) {
      std::printf("FAIL: cannot open %s\n", argv[2]);
      return 2;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    rd.b.resize(n > 0 ? size_t(n) : 0u);
    const size_t got = rd.b.empty() ? 0 : std::fread(rd.b.data(), 1, rd.b.size(), f);
    std::fclose(f);
    if (got != rd.b.size()) {
      std::printf("FAIL: short read of %s\n", argv[2]);
      return 2;
    }
  }
  if (rd.u32() != 0x3646524Du || rd.u32() != 1u) {
    std::printf("FAIL: %s is not a version-1 MRF6 fixture\n", argv[2]);
    return 2;
  }
  R::TestHooks hooks{famHost, famOwner};
  hooks.trapLimit = 0;
  R::censusResetForTest(true);
  R::censusSetTestHooks(&hooks);
  R::censusSetLiteralsForTest(nullptr, 0);  // the generated table (gfx/resource_census_literals.inc)
  R::censusSetWitnessKForTest(1);
  const uint32_t nVs = rd.u32();
  for (uint32_t i = 0; i < nVs && rd.ok; i++) {
    const uint64_t h = rd.u64();
    const std::vector<uint8_t> u = rd.bytes(rd.u32());
    R::noteVsUcode(h, u.data(), u.size());
  }
  const uint32_t nDraws = rd.u32();
  uint64_t fails = 0, bounded = 0, fellBack = 0, unchecked = 0, unknownOk = 0, vrMismatch = 0, slotsTotal = 0;
  uint64_t byReason[R::kFbReasonsMax] = {};
  uint64_t tight = 0, widthSum = 0;  // (the bound's excess over the exact range, information)
  for (uint32_t di = 0; di < nDraws && rd.ok; di++) {
    const uint64_t vs = rd.u64();
    const uint32_t dup = rd.u32(), flags = rd.u32(), endian = rd.u32(), offset = rd.u32(), minV = rd.u32(),
                   maxV = rd.u32(), reset = rd.u32(), count = rd.u32();
    (void)dup;
    const std::vector<uint8_t> ib = rd.bytes(rd.u32());
    std::vector<uint32_t> consts(1024), fetch(192);
    rd.take(consts.data(), 4096);
    rd.take(fetch.data(), 768);
    const uint32_t sBase = rd.u32();
    const std::vector<uint8_t> stream = rd.bytes(rd.u32());
    const uint32_t pyMin = rd.u32(), pyMax = rd.u32();
    const std::vector<uint8_t> tagB = rd.bytes(rd.u32());
    const std::string tag(tagB.begin(), tagB.end());
    const uint32_t nSlots = rd.u32();
    if (!rd.ok) break;
    // the stream's pages as the trace held them (both windows; the rest of each page zero)
    g_famPages.clear();
    for (size_t k = 0; k < stream.size(); k++) {
      const uint32_t a = (sBase & 0x1FFFFFFFu) + uint32_t(k);
      std::vector<uint8_t>& pg = g_famPages[a >> 12];
      if (pg.empty()) pg.assign(4096, 0);
      pg[a & 0xFFFu] = stream[k];
    }
    const bool indexed = flags & 1u, idx32 = flags & 2u, stripCut = flags & 4u, clampOn = flags & 8u;
    uint32_t vMin = 0, vMax = 0;
    const bool any = R::vertexIndexRange(indexed ? ib.data() : nullptr, count, idx32, endian, offset, clampOn, minV,
                                         maxV, stripCut, reset, vMin, vMax);
    if ((any ? vMin : 0xFFFFFFFFu) != pyMin || (any ? vMax : 0u) != pyMax) {
      vrMismatch++;
      std::printf("FAIL: %s vs=%016llX: the census's vertex range [%u, %u] (any %d) != the tool's [%u, %u]\n",
                  tag.c_str(), (unsigned long long)vs, vMin, vMax, any ? 1 : 0, pyMin, pyMax);
    }
    const R::DrawScope d(vs, indexed, indexed ? ib.data() : nullptr, count, idx32, endian, offset, clampOn, minV, maxV,
                         stripCut, reset, consts.data(), fetch.data());
    for (uint32_t si = 0; si < nSlots && rd.ok; si++) {
      const uint32_t slot = rd.u32(), status = rd.u32();
      const int64_t lo = rd.i64(), hi = rd.i64();
      slotsTotal++;
      int64_t bLo = 0, bHi = 0;
      uint32_t why = 0;
      const bool ok = R::censusSlotBoundForTest(slot, bLo, bHi, &why);
      if (!ok) {
        fellBack++;
        if (why < R::kFbReasonsMax) byReason[why]++;
      }
      if (status == 1u) {
        unchecked++;
        continue;
      }
      if (status == 2u) {
        if (ok) {
          fails++;
          std::printf("FAIL: %s vs=%016llX slot %u: the host forms no index (status 2) but the census bounds it "
                      "[%lld, %lld]\n", tag.c_str(), (unsigned long long)vs, slot, (long long)bLo, (long long)bHi);
        } else {
          unknownOk++;
        }
        continue;
      }
      if (!ok) continue;  // a fallback: its declared range, a superset (sound)
      bounded++;
      if (bLo > lo || hi > bHi) {
        fails++;
        std::printf("FAIL: %s vs=%016llX slot %u: the exact range [%lld, %lld] is not inside the bound [%lld, %lld]\n",
                    tag.c_str(), (unsigned long long)vs, slot, (long long)lo, (long long)hi, (long long)bLo,
                    (long long)bHi);
      } else {
        tight += (bLo == lo && bHi == hi) ? 1u : 0u;
        widthSum += uint64_t((lo - bLo) + (bHi - hi));
      }
    }
  }
  R::Totals& t = snapA();
  std::printf("[MEMRES6_FAMILY] fixture=%s draws=%u slots=%llu bounded=%llu (tight %llu, excess %llu indices) "
              "fallback=%llu unchecked=%llu unknown_fell_back=%llu FAIL=%llu vrange_mismatch=%llu "
              "witness.sampled=%llu witness.violations=%llu read=%s\n",
              argv[2], nDraws, (unsigned long long)slotsTotal, (unsigned long long)bounded, (unsigned long long)tight,
              (unsigned long long)widthSum, (unsigned long long)fellBack, (unsigned long long)unchecked,
              (unsigned long long)unknownOk, (unsigned long long)fails, (unsigned long long)vrMismatch,
              (unsigned long long)t.boundSampled, (unsigned long long)t.boundViolations, rd.ok ? "ok" : "TRUNCATED");
  static const char* const kNames[] = {"slot", "op", "rel", "ctl", "fmt", "read", "many",
                                       "range", "state", "clob", "long", "vs", "lit"};
  std::printf("[MEMRES6_FAMILY] fallback reasons:");
  for (uint32_t r = 0; r < 13u; r++) std::printf(" %s=%llu", kNames[r], (unsigned long long)byReason[r]);
  std::printf("\n");
  const uint64_t bad = fails + vrMismatch + t.boundViolations + (rd.ok ? 0u : 1u) + (bounded == 0u ? 1u : 0u);
  std::printf("mem resources family test: %s\n", bad ? "FAIL" : "PASS (every exact range inside its bound)");
  R::censusResetForTest(false);
  R::censusSetTestHooks(nullptr);
  return int(std::min<uint64_t>(bad, 125u));
}
