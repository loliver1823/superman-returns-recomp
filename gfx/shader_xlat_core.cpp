// iter-463 runtime shader translation - the pure core. See shader_xlat_core.hpp.
//
// Ports (each marked where it is used):
//   tools/stage_missing_vs.py   patch_masked, prove_same_program, vertex_elements,
//                               derive_fetches, analyse_vsf, input_signature, VF_LINE, main()
//   tools/xtr_draws.py          cf_exec_walk, decode_vfetch
//   tools/XenosRecomp-clone     main.cpp (per-container translation), dxc_compiler.cpp (args)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>  // IUnknown, IStream, CoTaskMemFree (WIN32_LEAN_AND_MEAN drops them)
#include <dxcapi.h>

#include "gfx/shader_xlat_core.hpp"
#include "gfx/shader_mkey.hpp"
#include "gfx/xenos_bridge.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#define XXH_INLINE_ALL
#include "thirdparty/xxhash.h"

namespace sr {
namespace gfx {
namespace xlat {

namespace {

uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint16_t be16(const uint8_t* p) { return uint16_t((uint32_t(p[0]) << 8) | uint32_t(p[1])); }

std::string hex16(uint64_t v) {
  char b[20];
  std::snprintf(b, sizeof(b), "%016" PRIx64, v);
  return b;
}

// tools/xtr_draws.py decode_vfetch (ucode.h VertexFetchInstruction), dwords host-order.
struct Vfetch {
  uint32_t opcode, src, dst, mustBeOne, fetchConst, prefetch, dstSwiz, isSigned, numFormat, signedRf,
      indexRounded, fmt, mini, predicated, stride, predCondition;
  int32_t expAdjust, offset;
};
Vfetch decodeVfetch(uint32_t d0, uint32_t d1, uint32_t d2) {
  Vfetch f{};
  f.opcode = d0 & 0x1Fu;
  f.src = (d0 >> 5) & 0x3Fu;
  f.dst = (d0 >> 12) & 0x3Fu;
  f.mustBeOne = (d0 >> 19) & 1u;
  f.fetchConst = ((d0 >> 20) & 0x1Fu) * 3u + ((d0 >> 25) & 3u);
  f.prefetch = (d0 >> 27) & 7u;
  f.dstSwiz = d1 & 0xFFFu;
  f.isSigned = (d1 >> 12) & 1u;
  f.numFormat = (d1 >> 13) & 1u;
  f.signedRf = (d1 >> 14) & 1u;
  f.indexRounded = (d1 >> 15) & 1u;
  f.fmt = (d1 >> 16) & 0x3Fu;
  int32_t e = int32_t((d1 >> 24) & 0x3Fu);
  f.expAdjust = e >= 32 ? e - 64 : e;
  f.mini = (d1 >> 30) & 1u;
  f.predicated = (d1 >> 31) & 1u;
  f.stride = d2 & 0xFFu;
  int32_t o = int32_t((d2 >> 8) & 0x7FFFFFu);
  f.offset = o >= (1 << 22) ? o - (1 << 23) : o;
  f.predCondition = (d2 >> 31) & 1u;
  return f;
}

bool writesSomething(uint32_t dstSwiz) {
  for (int i = 0; i < 4; i++)
    if (((dstSwiz >> (3 * i)) & 7u) <= 3u) return true;
  return false;
}

std::vector<uint32_t> dwordsOf(const uint8_t* u, size_t bytes) {
  std::vector<uint32_t> d(bytes / 4);
  for (size_t i = 0; i < d.size(); i++) d[i] = be32(u + 4 * i);
  return d;
}

}  // namespace

// ── containers ───────────────────────────────────────────────────────────────
bool parseContainer(const uint8_t* p, size_t n, const std::string& origin, Container& out,
                    std::string* why) {
  auto fail = [&](const char* w) {
    if (why) *why = w;
    return false;
  };
  if (!p || n < 0x24) return fail("shorter than a container header");
  const uint32_t flags = be32(p);
  if ((flags & 0xFFFFFF00u) != 0x102A1100u) return fail("no 0x102A11xx magic");
  const uint64_t virt = be32(p + 4), phys = be32(p + 8);
  // main.cpp's acceptance envelope (iter-440g).
  if (virt < 0x24 || virt >= 0x20000 || phys == 0 || phys >= 0x20000) return fail("sizes out of range");
  const uint64_t total = virt + phys;
  if (total > n) return fail("container runs past the data");
  const uint64_t ctOff = be32(p + 0x10), defOff = be32(p + 0x14), shOff = be32(p + 0x18);
  // Constant table (shader_recompiler.cpp walks every ConstantInfo and reads its name).
  // iter-467 release/installer: offset 0 is accepted. The XDK's own vertex shaders in default.xex
  // (18 of them, all in the AOT cache) carry no table; the recompiler's assert is compiled out and it
  // reads the container header as the table, whose constant count is this offset field itself (0) -
  // an empty table, which the checks below then see exactly as the recompiler does. Before, the
  // runtime translator refused them, so only the offline cache had them (the installer re-derives
  // them byte-identical to it, archive/r2 2026-10-09).
  if (ctOff + 4 + 28 > virt) return fail("constant table out of range");
  const uint64_t ctData = ctOff + 4;
  const uint64_t nConst = be32(p + ctData + 12), ciOff = be32(p + ctData + 16);
  if (nConst > 4096 || ctData + ciOff + nConst * 20 > virt) return fail("constant infos out of range");
  for (uint64_t i = 0; i < nConst; i++) {
    const uint64_t nameOff = ctData + be32(p + ctData + ciOff + i * 20);
    if (nameOff >= virt) return fail("constant name out of range");
    if (!std::memchr(p + nameOff, 0, size_t(virt - nameOff))) return fail("constant name unterminated");
  }
  // Definition table: float4 literals (values in the physical part), then int4s.
  if (defOff != 0) {
    uint64_t pos = defOff + 20;
    if (pos + 4 > virt) return fail("definition table out of range");
    uint32_t guard = 0;
    while (be32(p + pos) != 0) {
      if (pos + 8 > virt || ++guard > 65536) return fail("float4 definitions out of range");
      const uint64_t count = be16(p + pos + 2), physOff = be32(p + pos + 4);
      if (virt + physOff + ((count + 3) / 4) * 16 > total) return fail("float4 literal out of range");
      pos += 8;
      if (pos + 4 > virt) return fail("float4 definitions unterminated");
    }
    pos += 4;
    if (pos + 4 > virt) return fail("int4 definitions out of range");
    while (be32(p + pos) != 0) {
      const uint64_t count = be16(p + pos + 2);
      if (pos + 4 * (1 + count) > virt || ++guard > 65536) return fail("int4 definition out of range");
      pos += 4 * (2 + count);
      if (pos + 4 > virt) return fail("int4 definitions unterminated");
    }
  }
  if (shOff == 0 || shOff + 0x18 > virt) return fail("shader header out of range");
  const uint64_t po = be32(p + shOff), sz = be32(p + shOff + 4);
  if (sz == 0 || (sz & 3u) || po + sz > phys) return fail("ucode out of range");
  const bool isVs = (flags & 1u) != 0;
  if (isVs) {
    if (shOff + 0x24 > virt) return fail("vertex shader header out of range");
    const uint64_t f18 = be32(p + shOff + 0x18), cnt = be32(p + shOff + 0x1C);
    if (shOff + 0x24 + 4 * (f18 + cnt) > virt) return fail("vertex elements out of range");
  } else {
    const uint64_t nInterp = (be32(p + shOff + 0x14) >> 5) & 0x1Fu;
    if (shOff + 0x20 + 4 * nInterp > virt) return fail("interpolators out of range");
  }
  // Every exec slot inside the ucode (XenosRecomp reads the instructions it names).
  const CfWalk w = cfExecWalk(p + virt + po, size_t(sz));
  if (w.malformed) return fail("control flow names an instruction past the ucode");

  out = Container{};
  out.bytes.assign(p, p + total);
  out.origin = origin;
  out.isVs = isVs;
  out.virt = uint32_t(virt);
  out.phys = uint32_t(phys);
  out.shOff = uint32_t(shOff);
  out.po = uint32_t(po);
  out.sz = uint32_t(sz);
  out.ucodeHash = XXH3_64bits(out.ucode(), out.sz);
  out.fullHash = XXH3_64bits(out.bytes.data(), out.bytes.size());
  {
    XXH3_state_t st;
    XXH3_64bits_reset(&st);
    XXH3_64bits_update(&st, out.bytes.data(), out.virt + out.po);
    XXH3_64bits_update(&st, out.bytes.data() + out.virt + out.po + out.sz,
                       out.bytes.size() - (out.virt + out.po + out.sz));
    out.metaHash = XXH3_64bits_digest(&st);
  }
  if (isVs) out.keyed = patchMaskedUcode(out.ucode(), out.sz, out.patchMasked);
  return true;
}

bool sameMetadata(const Container& a, const Container& b) {
  if (a.bytes.size() != b.bytes.size() || a.virt + a.po != b.virt + b.po || a.sz != b.sz) return false;
  const size_t head = a.virt + a.po, tail = head + a.sz;
  return std::memcmp(a.bytes.data(), b.bytes.data(), head) == 0 &&
         std::memcmp(a.bytes.data() + tail, b.bytes.data() + tail, a.bytes.size() - tail) == 0;
}

std::vector<std::vector<uint32_t>> vfetchRuns(const std::vector<uint32_t>& slots) {
  std::vector<uint32_t> s = slots;
  std::sort(s.begin(), s.end());
  s.erase(std::unique(s.begin(), s.end()), s.end());
  std::vector<std::vector<uint32_t>> runs;
  for (uint32_t v : s) {
    if (runs.empty() || runs.back().back() + 1 != v) runs.emplace_back();
    runs.back().push_back(v);
  }
  return runs;
}

bool patchMaskedUcode(const uint8_t* u, size_t bytes, std::vector<uint8_t>& out) {
  out.clear();
  std::vector<uint32_t> slots;
  if (!mkey::vfetchSlots(u, bytes, slots)) return false;  // sm.vfetch_slots: None
  out.assign(u, u + (bytes / 4) * 4);
  auto andBe = [&](size_t dw, uint32_t keep) {
    uint8_t* q = out.data() + dw * 4;
    const uint32_t v = be32(q) & keep;
    q[0] = uint8_t(v >> 24); q[1] = uint8_t(v >> 16); q[2] = uint8_t(v >> 8); q[3] = uint8_t(v);
  };
  for (uint32_t s : slots) {
    andBe(size_t(s) * 3, ~kPatchDw0);
    andBe(size_t(s) * 3 + 1, ~kPatchDw1);
    andBe(size_t(s) * 3 + 2, ~kPatchDw2);
  }
  // iter-467: a reorder inside a contiguous fetch run is the same program - each run's masked
  // instructions in sorted order (kPatchDw0's comment in the header).
  for (const auto& run : vfetchRuns(slots)) {
    if (run.size() < 2) continue;
    std::vector<std::array<uint8_t, 12>> ins(run.size());
    for (size_t i = 0; i < run.size(); i++) std::memcpy(ins[i].data(), out.data() + size_t(run[i]) * 12, 12);
    std::sort(ins.begin(), ins.end());
    for (size_t i = 0; i < run.size(); i++) std::memcpy(out.data() + size_t(run[i]) * 12, ins[i].data(), 12);
  }
  return true;
}

bool ContainerIndex::add(Container&& c) {
  auto r = byFull_.equal_range(c.fullHash);
  for (auto it = r.first; it != r.second; ++it)
    if (all_[it->second]->bytes == c.bytes) return false;
  const size_t id = all_.size();
  byFull_.emplace(c.fullHash, id);
  byUcode_.emplace(c.ucodeHash, id);
  if (c.isVs) {
    nVs_++;
    if (c.keyed) byMasked_.emplace(XXH3_64bits(c.patchMasked.data(), c.patchMasked.size()), id);
  }
  all_.push_back(std::make_unique<Container>(std::move(c)));
  return true;
}

size_t ContainerIndex::absorb(ContainerIndex& other) {
  size_t n = 0;
  for (auto& c : other.all_)
    if (c && add(std::move(*c))) n++;
  other.all_.clear();
  other.byFull_.clear();
  other.byMasked_.clear();
  other.byUcode_.clear();
  other.nVs_ = 0;
  return n;
}

void ContainerIndex::sameProgram(const std::vector<uint8_t>& masked,
                                 std::vector<const Container*>& out) const {
  out.clear();
  auto r = byMasked_.equal_range(XXH3_64bits(masked.data(), masked.size()));
  for (auto it = r.first; it != r.second; ++it) {
    const Container* c = all_[it->second].get();
    if (c->isVs && c->keyed && c->patchMasked == masked) out.push_back(c);
  }
}

void ContainerIndex::exactUcode(uint64_t h, bool isVs, std::vector<const Container*>& out) const {
  out.clear();
  auto r = byUcode_.equal_range(h);
  for (auto it = r.first; it != r.second; ++it) {
    const Container* c = all_[it->second].get();
    if (c->isVs == isVs) out.push_back(c);
  }
}

ScanStats scanContainerDir(const std::string& dir, ContainerIndex& index, int depth) {
  namespace fs = std::filesystem;
  ScanStats st;
  std::error_code ec;
  const fs::path d = fs::u8path(dir);
  if (fs::is_regular_file(d, ec)) {  // a corpus.txt: "<count> <dir>" or "<dir>" per line
    if (depth >= 4) return st;       // a corpus.txt that names itself (or a cycle) ends here
    std::ifstream f(d);
    std::string line;
    while (std::getline(f, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.empty() || line[0] == '#') continue;
      std::string sub = line;
      const size_t sp = line.find(' ');
      if (sp != std::string::npos &&
          std::all_of(line.begin(), line.begin() + sp, [](char ch) { return ch >= '0' && ch <= '9'; }))
        sub = line.substr(sp + 1);
      const ScanStats s = scanContainerDir(sub, index, depth + 1);
      st.files += s.files;
      st.added += s.added;
      st.invalid += s.invalid;
    }
    return st;
  }
  std::vector<fs::path> files;
  for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec))
    if (it->path().extension() == ".xpu") files.push_back(it->path());
  std::sort(files.begin(), files.end());
  for (const auto& p : files) {
    std::ifstream f(p, std::ios::binary);
    std::vector<uint8_t> blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    st.files++;
    Container c;
    if (!parseContainer(blob.data(), blob.size(), p.generic_u8string(), c, nullptr)) {
      st.invalid++;
      continue;
    }
    if (index.add(std::move(c))) st.added++;
  }
  return st;
}

// ── control flow (tools/xtr_draws.py cf_exec_walk) ────────────────────────────
CfWalk cfExecWalk(const uint8_t* u, size_t bytes) {
  CfWalk w;
  const size_t n = bytes / 4, pairs = n / 3;
  if (!u || pairs == 0) return w;
  auto dw = [&](size_t i) { return be32(u + 4 * i); };
  size_t bound = pairs;
  for (size_t i = 0; i < bound; i++) {
    uint32_t cf[2][2];
    mkey::unpackCfPair(dw(3 * i), dw(3 * i + 1), dw(3 * i + 2), cf);
    for (int j = 0; j < 2; j++)
      if (mkey::isExecOpcode((cf[j][1] >> 12) & 0xFu) && (cf[j][0] & 0xFFFu) < bound)
        bound = cf[j][0] & 0xFFFu;
  }
  w.bound = uint32_t(bound);
  for (size_t i = 0; i < bound; i++) {
    uint32_t cf[2][2];
    mkey::unpackCfPair(dw(3 * i), dw(3 * i + 1), dw(3 * i + 2), cf);
    for (int j = 0; j < 2; j++) {
      const uint32_t op = (cf[j][1] >> 12) & 0xFu;
      if (!mkey::isExecOpcode(op)) continue;
      w.sawExec = true;
      const uint32_t address = cf[j][0] & 0xFFFu, count = (cf[j][0] >> 12) & 0x7u;
      uint32_t seq = (cf[j][0] >> 16) & 0xFFFu;
      for (uint32_t k = 0; k < count; k++, seq >>= 2) {
        const uint32_t slot = address + k;
        if (size_t(slot) * 3 + 3 > n) {
          w.malformed = true;
          return w;
        }
        w.slots.push_back({uint32_t(2 * i + j), op, slot, (seq & 1u) != 0, (seq & 2u) != 0});
      }
    }
  }
  return w;
}

std::vector<std::vector<uint32_t>> vfetchRunsByClause(const std::vector<uint32_t>& slots, const CfWalk& w) {
  std::map<uint32_t, std::set<uint32_t>> clauses;  // slot -> the exec clauses (CF indices) that execute it
  for (const CfSlot& s : w.slots) clauses[s.slot].insert(s.cfIndex);
  std::vector<std::vector<uint32_t>> out;
  for (const auto& run : vfetchRuns(slots)) {
    out.emplace_back();
    for (uint32_t s : run) {
      const auto& c = clauses[s];
      if (!out.back().empty()) {
        const auto& p = clauses[out.back().back()];
        if (c.size() != 1 || p.size() != 1 || c != p) out.emplace_back();
      }
      out.back().push_back(s);
    }
  }
  return out;
}

bool fetchOrderKeepsDependences(const uint8_t* u, size_t bytes, const std::vector<uint32_t>& run,
                                const std::vector<size_t>& from, std::string* why) {
  const std::vector<uint32_t> dw = dwordsOf(u, bytes);
  if (from.size() != run.size()) return false;
  struct F { uint32_t src, srcComp, dst, comps; bool rel; };
  std::vector<F> f;
  for (uint32_t s : run) {
    if (size_t(s) * 3 + 2 >= dw.size()) return false;
    const uint32_t d0 = dw[size_t(s) * 3], d1 = dw[size_t(s) * 3 + 1];
    const Vfetch v = decodeVfetch(d0, d1, dw[size_t(s) * 3 + 2]);
    uint32_t comps = 0;
    for (uint32_t c = 0; c < 4; c++)
      if (((v.dstSwiz >> (3 * c)) & 7u) != 7u) comps |= 1u << c;  // written: a source component or a constant
    // ucode.h VertexFetchInstruction dword 0: src_reg_am bit 11, dst_reg_am bit 18, src_swiz bits 30-31 (the index)
    f.push_back({v.src, (d0 >> 30) & 3u, v.dst, comps, ((d0 >> 11) & 1u) != 0 || ((d0 >> 18) & 1u) != 0});
  }
  for (size_t a = 0; a < f.size(); a++)
    for (size_t b = a + 1; b < f.size(); b++) {  // a before b in the runtime program
      const bool raw = f[a].dst == f[b].src && (f[a].comps & (1u << f[b].srcComp));
      const bool war = f[a].src == f[b].dst && (f[b].comps & (1u << f[a].srcComp));
      const bool waw = f[a].dst == f[b].dst && (f[a].comps & f[b].comps);
      const bool dep = f[a].rel || f[b].rel || raw || war || waw;
      if (dep && from[a] > from[b]) {
        if (why)
          *why = "the reorder swaps i" + std::to_string(run[a]) + " and i" + std::to_string(run[b]) +
                 ", which depend on each other";
        return false;
      }
    }
  return true;
}

// ── stage_missing_vs.py prove_same_program ────────────────────────────────────
ProofReport proveSameProgram(const uint8_t* t, const uint8_t* u, size_t bytes) {
  ProofReport rep;
  const CfWalk wu = cfExecWalk(u, bytes), wt = cfExecWalk(t, bytes);
  auto sameSlots = [&] {
    if (wu.slots.size() != wt.slots.size()) return false;
    for (size_t i = 0; i < wu.slots.size(); i++) {
      const CfSlot &a = wu.slots[i], &b = wt.slots[i];
      if (a.cfIndex != b.cfIndex || a.opcode != b.opcode || a.slot != b.slot || a.isFetch != b.isFetch ||
          a.serialize != b.serialize)
        return false;
    }
    return true;
  };
  if (wu.malformed || wt.malformed || !sameSlots()) {
    rep.ok = false;
    rep.problems.push_back("control-flow walk differs or is malformed");
    return rep;
  }
  const std::vector<uint32_t> tdw = dwordsOf(t, bytes), udw = dwordsOf(u, bytes);
  const uint32_t bound = wu.bound;
  for (uint32_t i = 0; i < bound * 3 && i < udw.size(); i++)
    if (tdw[i] != udw[i]) {
      rep.ok = false;
      rep.problems.push_back("CF dword " + std::to_string(i) + " differs");
    }
  rep.cfDwords = bound * 3;
  std::vector<bool> covered(udw.size(), false);
  for (uint32_t i = 0; i < bound * 3 && i < udw.size(); i++) covered[i] = true;
  std::set<uint32_t> seen;
  std::vector<uint32_t> vslots;  // executed vertex fetches, compared run by run below
  for (const CfSlot& s : wu.slots) {
    if (!seen.insert(s.slot).second) continue;
    const size_t b0 = size_t(s.slot) * 3;
    for (size_t k = 0; k < 3; k++) covered[b0 + k] = true;
    const uint32_t a0 = tdw[b0], a1 = tdw[b0 + 1], a2 = tdw[b0 + 2];
    const uint32_t c0 = udw[b0], c1 = udw[b0 + 1], c2 = udw[b0 + 2];
    if (s.isFetch && (c0 & 0x1Fu) == 0) {
      rep.vfetch++;
      vslots.push_back(s.slot);
    } else {
      if (a0 != c0 || a1 != c1 || a2 != c2) {
        rep.ok = false;
        rep.problems.push_back(std::string(s.isFetch ? "TFETCH" : "ALU") + " instruction i" +
                               std::to_string(s.slot) + " differs");
      }
      if (s.isFetch) rep.tfetch++;
      else rep.alu++;
    }
  }
  // Vertex fetches (iter-467): every runtime fetch is a template fetch OF THE SAME CONTIGUOUS RUN
  // whose bits outside the bind-time patch fields are identical (opcode, source and destination
  // registers and their address modes, must_be_one, src_swiz, index rounding, predication), each
  // template fetch used once. The template fetch at the same slot is taken when it qualifies, so
  // an unreordered program pairs in place (the iter-463 rule); a reorder pairs by content. A
  // fetch that matches no unused template fetch of its run refuses the proof.
  auto masked = [&](const std::vector<uint32_t>& dw, uint32_t slot) {
    const size_t b = size_t(slot) * 3;
    return std::array<uint32_t, 3>{dw[b] & ~kPatchDw0, dw[b + 1] & ~kPatchDw1, dw[b + 2] & ~kPatchDw2};
  };
  // render/shader-coverage: runs end at exec-clause boundaries, and the pairing must keep the order of every two
  // fetches that depend on each other (vfetchRunsByClause / fetchOrderKeepsDependences, checked after the run).
  for (const auto& run : vfetchRunsByClause(vslots, wu)) {
    std::vector<bool> used(run.size(), false);
    std::vector<size_t> from(run.size(), run.size());
    for (size_t i = 0; i < run.size(); i++) {
      const uint32_t s = run[i];
      const auto mu = masked(udw, s);
      size_t pick = run.size();
      if (!used[i] && masked(tdw, s) == mu) pick = i;
      for (size_t k = 0; pick == run.size() && k < run.size(); k++)
        if (!used[k] && masked(tdw, run[k]) == mu) pick = k;
      if (pick == run.size()) {
        rep.ok = false;
        rep.problems.push_back("vfetch i" + std::to_string(s) +
                               " unpatched bits differ (no unused fetch of its run i" + std::to_string(run.front()) +
                               "..i" + std::to_string(run.back()) + " matches)");
        continue;
      }
      from[i] = pick;
      used[pick] = true;
      const uint32_t ts = run[pick];
      const size_t bt = size_t(ts) * 3, bu = size_t(s) * 3;
      const Vfetch ft = decodeVfetch(tdw[bt], tdw[bt + 1], tdw[bt + 2]);
      const Vfetch fu = decodeVfetch(udw[bu], udw[bu + 1], udw[bu + 2]);
      std::string d;
      if (ts != s) d = "from i" + std::to_string(ts);
      auto field = [&](const char* name, int64_t x, int64_t y, bool hex3 = false) {
        if (x == y) return;
        char b[64];
        if (hex3)
          std::snprintf(b, sizeof(b), "%s%s %03llx->%03llx", d.empty() ? "" : ", ", name, (long long)x,
                        (long long)y);
        else
          std::snprintf(b, sizeof(b), "%s%s %lld->%lld", d.empty() ? "" : ", ", name, (long long)x,
                        (long long)y);
        d += b;
      };
      field("fetch_const", ft.fetchConst, fu.fetchConst);
      field("prefetch", ft.prefetch, fu.prefetch);
      field("dst_swiz", ft.dstSwiz, fu.dstSwiz, true);
      field("signed", ft.isSigned, fu.isSigned);
      field("num_format", ft.numFormat, fu.numFormat);
      field("signed_rf", ft.signedRf, fu.signedRf);
      field("fmt", ft.fmt, fu.fmt);
      field("exp_adjust", ft.expAdjust, fu.expAdjust);
      field("mini", ft.mini, fu.mini);
      field("stride", ft.stride, fu.stride);
      field("offset", ft.offset, fu.offset);
      if (!d.empty()) rep.diffs.push_back({s, d});
    }
    std::string why;
    if (std::find(from.begin(), from.end(), run.size()) == from.end() &&
        !fetchOrderKeepsDependences(u, bytes, run, from, &why)) {
      rep.ok = false;
      rep.problems.push_back("vfetch run i" + std::to_string(run.front()) + "..i" + std::to_string(run.back()) + ": " +
                             why);
    }
  }
  std::string rest;
  int nRest = 0;
  for (size_t i = 0; i < udw.size(); i++)
    if (!covered[i] && tdw[i] != udw[i] && nRest++ < 8) rest += (rest.empty() ? "" : ",") + std::to_string(i);
  if (nRest) {
    rep.ok = false;
    rep.problems.push_back("non-executed dwords differ: " + rest);
  }
  return rep;
}

bool vertexElements(const Container& c, std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>>& out) {
  out.clear();
  if (!c.isVs) return false;
  const uint8_t* p = c.bytes.data();
  const uint64_t f18 = be32(p + c.shOff + 0x18), cnt = be32(p + c.shOff + 0x1C);
  if (c.shOff + 0x24 + 4 * (f18 + cnt) > c.virt) return false;  // parseContainer checked it
  for (uint64_t i = 0; i < cnt; i++) {
    const uint32_t v = be32(p + c.shOff + 0x24 + 4 * (f18 + i));
    out[v & 0xFFFu] = {(v >> 12) & 0xFu, (v >> 16) & 0xFu};
  }
  return true;
}

// ── recipes (iter-467 release/installer) ─────────────────────────────────────
std::vector<Recipe> parseRecipes(const std::string& text, size_t* bad) {
  std::vector<Recipe> out;
  size_t nBad = 0;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ls(line);
    std::string kind, k;
    Recipe r;
    bool ok = false;
    if (ls >> kind >> k) {
      char* end = nullptr;
      r.key = std::strtoull(k.c_str(), &end, 16);
      const bool keyOk = k.size() == 16 && end && *end == 0;
      if (kind == "xdk") {
        r.xdk = true;
        ok = keyOk && bool(ls >> r.bytes) && r.bytes && (r.bytes & 3u) == 0;
      } else if (kind == "patch") {
        std::string t, where;
        if (keyOk && (ls >> t >> r.bytes >> where) && t.size() == 16 && (where == "disc" || where == "live")) {
          r.tmpl = std::strtoull(t.c_str(), &end, 16);
          r.liveTemplate = where == "live";
          ok = end && *end == 0 && r.bytes && (r.bytes & 3u) == 0;
          std::string f;
          while (ok && ls >> f) {
            RecipeFetch rf;
            unsigned long long v[5];
            size_t at = 0;
            for (int i = 0; i < 5 && ok; i++) {
              const size_t e = i < 4 ? f.find(':', at) : f.size();
              if (e == std::string::npos || e == at) { ok = false; break; }
              v[i] = std::strtoull(f.substr(at, e - at).c_str(), &end, i < 2 ? 10 : 16);
              ok = end && *end == 0;
              at = e + 1;
            }
            if (!ok) break;
            rf.slot = uint32_t(v[0]);
            rf.from = uint32_t(v[1]);
            rf.b0 = uint32_t(v[2]);
            rf.b1 = uint32_t(v[3]);
            rf.b2 = uint32_t(v[4]);
            r.fetches.push_back(rf);
          }
          // No fetch is valid: a vertex shader that reads no vertex (identity recipe - the game binds
          // the template as created; it lets the runtime translate it ahead of use).
        }
      }
    }
    if (ok) out.push_back(std::move(r));
    else nBad++;
  }
  if (bad) *bad = nBad;
  return out;
}

bool applyRecipe(const uint8_t* t, size_t bytes, const Recipe& r, std::vector<uint8_t>& out, std::string* why) {
  auto fail = [&](const std::string& w) {
    if (why) *why = w;
    out.clear();
    return false;
  };
  if (r.xdk || !t || bytes != r.bytes || (bytes & 3u)) return fail("template size differs from the recipe's");
  std::vector<uint32_t> slots;
  if (!mkey::vfetchSlots(t, bytes, slots)) return fail("the template's control flow does not walk");
  std::unordered_map<uint32_t, size_t> runOf;
  const auto runs = vfetchRuns(slots);
  for (size_t i = 0; i < runs.size(); i++)
    for (uint32_t s : runs[i]) runOf[s] = i;
  out.assign(t, t + bytes);
  std::set<uint32_t> seenSlot;
  for (const RecipeFetch& f : r.fetches) {
    auto a = runOf.find(f.slot), b = runOf.find(f.from);
    if (a == runOf.end() || b == runOf.end() || a->second != b->second || !seenSlot.insert(f.slot).second)
      return fail("fetch " + std::to_string(f.slot) + " <- " + std::to_string(f.from) +
                  " is not a vertex fetch of one run of the template");
    if ((f.b0 & ~kPatchDw0) || (f.b1 & ~kPatchDw1) || (f.b2 & ~kPatchDw2))
      return fail("fetch " + std::to_string(f.slot) + " carries bits outside the patch fields");
    const uint8_t* src = t + size_t(f.from) * 12;
    const uint32_t d[3] = {(be32(src) & ~kPatchDw0) | f.b0, (be32(src + 4) & ~kPatchDw1) | f.b1,
                           (be32(src + 8) & ~kPatchDw2) | f.b2};
    uint8_t* dst = out.data() + size_t(f.slot) * 12;
    for (int k = 0; k < 3; k++) {
      dst[4 * k] = uint8_t(d[k] >> 24);
      dst[4 * k + 1] = uint8_t(d[k] >> 16);
      dst[4 * k + 2] = uint8_t(d[k] >> 8);
      dst[4 * k + 3] = uint8_t(d[k]);
    }
  }
  if (XXH3_64bits(out.data(), out.size()) != r.key) return fail("the result does not hash to the recipe's key");
  return true;
}

bool synthesizeRawVsContainer(const uint8_t* u, size_t bytes, const std::string& origin, Container& out,
                              std::string* why) {
  auto fail = [&](const std::string& w) {
    if (why) *why = w;
    return false;
  };
  if (!u || !bytes || (bytes & 3u) || bytes > 0x40000) return fail("bad ucode size");
  std::vector<uint32_t> slots;
  if (!mkey::vfetchSlots(u, bytes, slots)) return fail("the control flow does not walk");
  std::vector<uint32_t> el;  // VertexElement: address:12 | usage:4 | usageIndex:4
  for (uint32_t s : slots) {
    if (!writesSomething(be32(u + size_t(s) * 12 + 4) & 0xFFFu)) continue;
    const uint32_t n = uint32_t(el.size());
    if (n > 16) return fail("more than 17 written vertex fetches");
    const uint32_t usage = n == 0 ? 0u : 5u, index = n == 0 ? 0u : n - 1;  // POSITION0, TEXCOORD0..15
    el.push_back((s & 0xFFFu) | (usage << 12) | (index << 16));
  }
  const uint32_t shOff = 0x24, virt = shOff + 0x24 + 4 * uint32_t(el.size());
  std::vector<uint8_t> b(virt + bytes, 0);
  auto put = [&](size_t at, uint32_t v) {
    b[at] = uint8_t(v >> 24);
    b[at + 1] = uint8_t(v >> 16);
    b[at + 2] = uint8_t(v >> 8);
    b[at + 3] = uint8_t(v);
  };
  // The layout of the XDK's own constant-free containers in default.xex (constant table and
  // definition table offsets 0, the shader header right behind the container header).
  put(0x00, 0x102A1101u);           // flags: a vertex shader
  put(0x04, virt);                  // virtualSize
  put(0x08, uint32_t(bytes));       // physicalSize
  put(0x18, shOff);                 // shaderOffset
  put(shOff + 0x00, 0);             // physicalOffset of the ucode
  put(shOff + 0x04, uint32_t(bytes));
  put(shOff + 0x1C, uint32_t(el.size()));  // vertexElementCount (field18 = 0: no leading entries)
  for (size_t i = 0; i < el.size(); i++) put(shOff + 0x24 + 4 * i, el[i]);
  std::memcpy(b.data() + virt, u, bytes);
  std::string w;
  if (!parseContainer(b.data(), b.size(), origin, out, &w)) return fail("synthesized container invalid: " + w);
  return true;
}

// ── post-translation checks ───────────────────────────────────────────────────
std::vector<SidecarRow> parseSidecarRows(const std::string& text) {
  // stage_missing_vs.py read_sidecar: (slot, offset, stride, fmt, signed, integer) of 8+ fields.
  std::vector<SidecarRow> rows;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    long long v[8];
    int k = 0;
    while (k < 8 && (ls >> v[k])) k++;
    if (k >= 8) rows.push_back({int(v[0]), int(v[1]), int(v[2]), int(v[3]), int(v[6]), int(v[7])});
  }
  return rows;
}

std::vector<SidecarRow> deriveFetches(const uint8_t* u, size_t bytes) {
  std::vector<mkey::FetchRow> r;
  std::vector<SidecarRow> out;
  if (!mkey::deriveFetchRows(u, bytes, r)) return out;
  for (const auto& x : r)
    out.push_back({int(x.slot), int(x.offsetDwords), int(x.strideDwords), int(x.format), x.isSigned ? 1 : 0,
                   x.isInteger ? 1 : 0});
  return out;
}

int fmtWords(int fmt) {
  return (fmt == 26 || fmt == 32 || fmt == 34 || fmt == 37) ? 2 : fmt == 57 ? 3 : (fmt == 35 || fmt == 38) ? 4 : 1;
}

namespace {
bool startsWith(const std::string& s, size_t pos, const char* pre) {
  const size_t n = std::strlen(pre);
  return s.size() >= pos + n && s.compare(pos, n, pre) == 0;
}
// "%<digits>" at pos; returns the end or npos.
size_t ssaAt(const std::string& s, size_t pos) {
  if (pos >= s.size() || s[pos] != '%') return std::string::npos;
  size_t e = pos + 1;
  while (e < s.size() && s[e] >= '0' && s[e] <= '9') e++;
  return e == pos + 1 ? std::string::npos : e;
}
// "-?<digits>" at pos; returns the end or npos.
size_t intAt(const std::string& s, size_t pos, long long* v) {
  size_t e = pos;
  if (e < s.size() && s[e] == '-') e++;
  const size_t d0 = e;
  while (e < s.size() && s[e] >= '0' && s[e] <= '9') e++;
  if (e == d0) return std::string::npos;
  if (v) *v = std::stoll(s.substr(pos, e - pos));
  return e;
}
}  // namespace

// stage_missing_vs.py analyse_vsf, as line scanning (no std::regex: its recursion on a long
// line can exhaust a host stack, and in-process that is fatal).
VsfAnalysis analyseVsf(const std::string& txt) {
  std::vector<std::string> lines;
  {
    std::istringstream in(txt);
    std::string l;
    while (std::getline(in, l)) {
      if (!l.empty() && l.back() == '\r') l.pop_back();
      lines.push_back(l);
    }
  }
  // defs: ^\s*(%\d+) = (.*)$
  std::map<std::string, std::string> defs;
  for (const auto& l : lines) {
    size_t i = l.find_first_not_of(" \t");
    if (i == std::string::npos) continue;
    const size_t e = ssaAt(l, i);
    if (e == std::string::npos || !startsWith(l, e, " = ")) continue;
    defs[l.substr(i, e - i)] = l.substr(e + 3);
  }
  auto def = [&](const std::string& k) -> std::string {
    auto it = defs.find(k);
    return it == defs.end() ? std::string() : it->second;
  };
  std::set<int64_t> muls, regs, offs;
  // = (mul|shl) i32 (%\d+), (-?\d+)   anywhere in the text; the operand defined by fptosi
  for (const auto& l : lines) {
    for (const char* op : {"= mul i32 ", "= shl i32 "}) {
      size_t at = 0;
      while ((at = l.find(op, at)) != std::string::npos) {
        const size_t s = at + std::strlen(op);
        at = s;
        const size_t e = ssaAt(l, s);
        if (e == std::string::npos || !startsWith(l, e, ", ")) continue;
        long long k = 0;
        if (intAt(l, e + 2, &k) == std::string::npos) continue;
        if (def(l.substr(s, e - s)).rfind("fptosi", 0) != 0) continue;
        muls.insert(op[2] == 'm' ? k : (int64_t(1) << k));
      }
    }
  }
  // ^\s*(%v) = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle (%h), i32 (\d+))
  struct Load { std::string v, hnd; int64_t reg; };
  std::vector<Load> loads;
  const char* kLoad = " = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle ";
  for (const auto& l : lines) {
    size_t i = l.find_first_not_of(" \t");
    if (i == std::string::npos) continue;
    const size_t e = ssaAt(l, i);
    if (e == std::string::npos || !startsWith(l, e, kLoad)) continue;
    const size_t h0 = e + std::strlen(kLoad);
    const size_t h1 = ssaAt(l, h0);
    if (h1 == std::string::npos || !startsWith(l, h1, ", i32 ")) continue;
    long long reg = 0;
    const size_t r1 = intAt(l, h1 + 6, &reg);
    if (r1 == std::string::npos || reg < 0 || !startsWith(l, r1, ")")) continue;
    loads.push_back({l.substr(i, e - i), l.substr(h0, h1 - h0), reg});
  }
  // A handle is the fetch-constant buffer when some component of one of its loads is masked
  // with -4 (the vertex base address): ^\s*(%w) = extractvalue %dx.types.CBufRet.i32 <v>, \d+
  // and "= and i32 <w>, -4\b" anywhere.
  std::set<std::string> fetchHandles;
  for (const auto& ld : loads) {
    const std::string pre = " = extractvalue %dx.types.CBufRet.i32 " + ld.v + ", ";
    for (const auto& l : lines) {
      size_t i = l.find_first_not_of(" \t");
      if (i == std::string::npos) continue;
      const size_t e = ssaAt(l, i);
      if (e == std::string::npos || !startsWith(l, e, pre.c_str())) continue;
      long long comp = 0;
      if (intAt(l, e + pre.size(), &comp) == std::string::npos || comp < 0) continue;
      const std::string w = l.substr(i, e - i);
      const std::string andPat = "= and i32 " + w + ", -4";
      for (const auto& l2 : lines) {
        size_t at = 0;
        while ((at = l2.find(andPat, at)) != std::string::npos) {
          const size_t after = at + andPat.size();
          const bool boundary =
              after >= l2.size() || !(std::isalnum(uint8_t(l2[after])) || l2[after] == '_');
          if (boundary) fetchHandles.insert(ld.hnd);
          at = after;
        }
      }
    }
  }
  for (const auto& ld : loads)
    if (fetchHandles.count(ld.hnd)) regs.insert(ld.reg);
  // @dx.op.bufferLoad.i32(i32 68, %dx.types.Handle %\d+, i32 (%\d+)  -> the constant byte offset in
  // the address. iter-467 release/installer: summed through the whole address expression - every
  // "add i32 A, B" (A and B each a value or a constant) and "and i32 A, -4" (when the offset is a
  // multiple of 4, (b & ~3) + c == (b + c) & ~3). The first form only read "add i32 %x, N" at the
  // load, and dxc sometimes reassociates (base + 36) & ~3 + index * 40 (11B1E5F1: its word-9
  // fetch read as offset 0, the shader refused while its translation was right).
  std::function<int64_t(const std::string&, int)> constOff = [&](const std::string& v, int depth) -> int64_t {
    if (depth > 16) return 0;
    std::string d = def(v);
    bool isAnd = false;
    if (startsWith(d, 0, "add ")) d = d.substr(4);
    else if (startsWith(d, 0, "and i32 ")) { d = d.substr(4); isAnd = true; }
    else return 0;
    for (const char* flag : {"nuw ", "nsw "})
      if (startsWith(d, 0, flag)) d = d.substr(4);
    if (!startsWith(d, 0, "i32 ")) return 0;
    auto operand = [&](size_t at, size_t* end, int64_t* val, bool* isConst) {
      const size_t e = ssaAt(d, at);
      if (e != std::string::npos) {
        *end = e;
        *isConst = false;
        *val = constOff(d.substr(at, e - at), depth + 1);
        return true;
      }
      long long k = 0;
      const size_t n = intAt(d, at, &k);
      if (n == std::string::npos) return false;
      *end = n;
      *isConst = true;
      *val = k;
      return true;
    };
    size_t e1 = 0, e2 = 0;
    int64_t v1 = 0, v2 = 0;
    bool c1 = false, c2 = false;
    if (!operand(4, &e1, &v1, &c1) || !startsWith(d, e1, ", ") || !operand(e1 + 2, &e2, &v2, &c2) || e2 != d.size())
      return 0;
    if (isAnd) return (!c1 && c2 && v2 == -4 && (v1 & 3) == 0) ? v1 : 0;
    return v1 + v2;
  };
  const char* kBl = "@dx.op.bufferLoad.i32(i32 68, %dx.types.Handle ";
  for (const auto& l : lines) {
    size_t at = 0;
    while ((at = l.find(kBl, at)) != std::string::npos) {
      const size_t h0 = at + std::strlen(kBl);
      at = h0;
      const size_t h1 = ssaAt(l, h0);
      if (h1 == std::string::npos || !startsWith(l, h1, ", i32 ")) continue;
      const size_t i1 = ssaAt(l, h1 + 6);
      if (i1 == std::string::npos) continue;
      offs.insert(constOff(l.substr(h1 + 6, i1 - (h1 + 6)), 0));
    }
  }
  VsfAnalysis a;
  a.muls.assign(muls.begin(), muls.end());
  a.regs.assign(regs.begin(), regs.end());
  a.offs.assign(offs.begin(), offs.end());
  return a;
}

bool inputSignature(const std::vector<uint8_t>& blob, std::vector<SigRow>& out) {
  out.clear();
  auto rd = [&](size_t o) {
    uint32_t v;
    std::memcpy(&v, blob.data() + o, 4);
    return v;
  };
  const size_t n = blob.size();
  if (n < 36 || std::memcmp(blob.data(), "DXBC", 4) != 0) return false;
  const uint32_t parts = rd(28);
  if (parts > 64 || 32 + size_t(parts) * 4 > n) return false;
  for (uint32_t i = 0; i < parts; i++) {
    const size_t po = rd(32 + size_t(i) * 4);
    if (po + 8 > n) continue;
    if (std::memcmp(blob.data() + po, "ISG1", 4) != 0) continue;
    const size_t plen = rd(po + 4);
    if (po + 8 + plen > n || plen < 8) return false;
    const size_t pay = po + 8;
    const uint32_t count = rd(pay), paramOff = rd(pay + 4);
    if (count > 64) return false;
    for (uint32_t e = 0; e < count; e++) {
      // PSV0-era ISG1 element (32 bytes): stream, name, semanticIndex, systemValue,
      // componentType, register, mask(u8) rwMask(u8) pad(u16), minPrecision.
      const size_t eb = size_t(paramOff) + size_t(e) * 32;
      if (eb + 32 > plen) return false;
      SigRow r;
      const uint32_t nameOff = rd(pay + eb + 4);
      if (nameOff >= plen) return false;
      const char* nm = reinterpret_cast<const char*>(blob.data() + pay + nameOff);
      const size_t len = strnlen(nm, plen - nameOff);
      if (len == plen - nameOff) return false;
      r.name.assign(nm, len);
      r.index = rd(pay + eb + 8);
      r.sysValue = rd(pay + eb + 12);
      r.compType = rd(pay + eb + 16);
      r.reg = rd(pay + eb + 20);
      r.mask = blob[pay + eb + 24];
      r.rwMask = blob[pay + eb + 25];
      out.push_back(std::move(r));
    }
    return true;
  }
  return false;
}

std::vector<std::string> diffLines(const std::string& a, const std::string& b) {
  auto split = [](const std::string& s) {
    std::vector<std::string> v;
    std::istringstream in(s);
    std::string l;
    while (std::getline(in, l)) v.push_back(l);
    return v;
  };
  const std::vector<std::string> la = split(a), lb = split(b);
  // Trim the common head and tail, then LCS over the middle.
  size_t h = 0;
  while (h < la.size() && h < lb.size() && la[h] == lb[h]) h++;
  size_t t = 0;
  while (t < la.size() - h && t < lb.size() - h && la[la.size() - 1 - t] == lb[lb.size() - 1 - t]) t++;
  const size_t na = la.size() - h - t, nb = lb.size() - h - t;
  std::vector<std::string> out;
  if (na * nb > 64u * 1024u * 1024u) {  // pathological: report everything as changed
    for (size_t i = 0; i < na; i++) out.push_back("-" + la[h + i]);
    for (size_t j = 0; j < nb; j++) out.push_back("+" + lb[h + j]);
    return out;
  }
  std::vector<uint32_t> L((na + 1) * (nb + 1), 0);
  auto at = [&](size_t i, size_t j) -> uint32_t& { return L[i * (nb + 1) + j]; };
  for (size_t i = na; i-- > 0;)
    for (size_t j = nb; j-- > 0;)
      at(i, j) = la[h + i] == lb[h + j] ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
  size_t i = 0, j = 0;
  while (i < na && j < nb) {
    if (la[h + i] == lb[h + j]) { i++; j++; }
    else if (at(i + 1, j) >= at(i, j + 1)) out.push_back("-" + la[h + i++]);
    else out.push_back("+" + lb[h + j++]);
  }
  while (i < na) out.push_back("-" + la[h + i++]);
  while (j < nb) out.push_back("+" + lb[h + j++]);
  return out;
}

bool isVfetchLine(const std::string& line) {
  // VF_LINE = (XeVfAddr|XeVfLoad|= i(Position|...|Sample)\d+\.|^\s*r\d+\.[xyzw] = [01]\.0;$|vfAddr)
  if (line.find("XeVfAddr") != std::string::npos || line.find("XeVfLoad") != std::string::npos ||
      line.find("vfAddr") != std::string::npos)
    return true;
  static const char* kUsages[] = {"Position", "BlendWeight", "BlendIndices", "Normal", "PointSize",
                                   "TexCoord", "Tangent", "Binormal", "TessFactor", "PositionT",
                                   "Color", "Fog", "Depth", "Sample"};
  size_t at = 0;
  while ((at = line.find("= i", at)) != std::string::npos) {
    const size_t s = at + 3;
    at = s;
    for (const char* u : kUsages) {
      if (!startsWith(line, s, u)) continue;
      size_t e = s + std::strlen(u);
      const size_t d0 = e;
      while (e < line.size() && line[e] >= '0' && line[e] <= '9') e++;
      if (e > d0 && e < line.size() && line[e] == '.') return true;
    }
  }
  // ^\s*r\d+\.[xyzw] = [01]\.0;$
  size_t i = line.find_first_not_of(" \t");
  if (i != std::string::npos && line[i] == 'r') {
    size_t e = i + 1;
    const size_t d0 = e;
    while (e < line.size() && line[e] >= '0' && line[e] <= '9') e++;
    // ".x = 1.0;" is 9 characters after the register number, and nothing may follow.
    if (e > d0 && line.size() == e + 9 && line[e] == '.' && line[e + 1] != 0 &&
        std::strchr("xyzw", line[e + 1]) && line.compare(e + 2, 3, " = ") == 0 &&
        (line[e + 5] == '0' || line[e + 5] == '1') && line.compare(e + 6, 3, ".0;") == 0)
      return true;
  }
  return false;
}

// ── DXC ─────────────────────────────────────────────────────────────────────────
namespace {
std::mutex g_dxcMu;
std::atomic<DxcCreateInstanceProc> g_create{nullptr};
uint64_t g_dllHash = 0;
std::string g_dxcVersion;
DxcLoadTiming g_dxcTiming;  // dcb: written under g_dxcMu by the one dxcLoad that loads

bool hashFile(const std::wstring& p, XXH3_state_t* st, uint64_t* bytes = nullptr) {
  std::ifstream f(std::filesystem::path(p), std::ios::binary);
  if (!f) return false;
  std::vector<char> buf(1 << 20);
  while (f) {
    f.read(buf.data(), buf.size());
    if (f.gcount() > 0) {
      XXH3_64bits_update(st, buf.data(), size_t(f.gcount()));
      if (bytes) *bytes += uint64_t(f.gcount());
    }
  }
  return true;
}
uint64_t usSince(std::chrono::steady_clock::time_point t0) {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
}
}  // namespace

DxcLoadTiming dxcLoadTiming() {
  std::lock_guard<std::mutex> lk(g_dxcMu);
  return g_dxcTiming;
}

bool dxcLoad(const std::wstring& dir, std::string* why) {
  std::lock_guard<std::mutex> lk(g_dxcMu);
  if (g_create.load()) return true;
  const std::wstring dxil = dir + L"\\dxil.dll", dxc = dir + L"\\dxcompiler.dll";
  // dxil.dll first: dxcompiler.dll signs its output through "dxil.dll", and a module already
  // loaded under that name is the one it gets.
  auto tPhase = std::chrono::steady_clock::now();  // dcb: each phase's wall time (observe-only)
  HMODULE hDxil = LoadLibraryW(dxil.c_str());
  g_dxcTiming.loadDxilUs = usSince(tPhase);
  if (!hDxil) {
    if (why) *why = "cannot load dxil.dll from the exe directory (GetLastError " + std::to_string(GetLastError()) + ")";
    return false;
  }
  tPhase = std::chrono::steady_clock::now();
  HMODULE hDxc = LoadLibraryW(dxc.c_str());
  g_dxcTiming.loadDxcUs = usSince(tPhase);
  if (!hDxc) {
    if (why) *why = "cannot load dxcompiler.dll from the exe directory (GetLastError " + std::to_string(GetLastError()) + ")";
    return false;
  }
  auto proc = reinterpret_cast<DxcCreateInstanceProc>(
      reinterpret_cast<void*>(GetProcAddress(hDxc, "DxcCreateInstance")));
  if (!proc) {
    if (why) *why = "dxcompiler.dll has no DxcCreateInstance";
    return false;
  }
  XXH3_state_t st;
  XXH3_64bits_reset(&st);
  tPhase = std::chrono::steady_clock::now();
  uint64_t hashed = 0;
  const bool hashedOk = hashFile(dxc, &st, &hashed) && hashFile(dxil, &st, &hashed);
  g_dxcTiming.hashUs = usSince(tPhase);
  g_dxcTiming.hashBytes = hashed;
  if (!hashedOk) {
    if (why) *why = "cannot read the DXC DLLs to hash them";
    return false;
  }
  g_dllHash = XXH3_64bits_digest(&st);
  // Version, for the log (IDxcVersionInfo2 on the compiler object).
  IDxcCompiler3* c = nullptr;
  if (SUCCEEDED(proc(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), reinterpret_cast<void**>(&c))) && c) {
    IDxcVersionInfo* vi = nullptr;
    if (SUCCEEDED(c->QueryInterface(__uuidof(IDxcVersionInfo), reinterpret_cast<void**>(&vi))) && vi) {
      UINT32 major = 0, minor = 0;
      vi->GetVersion(&major, &minor);
      g_dxcVersion = std::to_string(major) + "." + std::to_string(minor);
      IDxcVersionInfo2* v2 = nullptr;
      if (SUCCEEDED(vi->QueryInterface(__uuidof(IDxcVersionInfo2), reinterpret_cast<void**>(&v2))) && v2) {
        UINT32 commits = 0;
        char* hash = nullptr;
        if (SUCCEEDED(v2->GetCommitInfo(&commits, &hash)) && hash) {
          g_dxcVersion += " (" + std::to_string(commits) + " commits, " + hash + ")";
          CoTaskMemFree(hash);
        }
        v2->Release();
      }
      vi->Release();
    }
    c->Release();
  }
  g_create.store(proc);
  return true;
}

bool dxcLoaded() { return g_create.load() != nullptr; }
uint64_t dxcDllHash() { return g_dllHash; }
std::string dxcVersion() {
  std::lock_guard<std::mutex> lk(g_dxcMu);
  return g_dxcVersion;
}

struct Dxc::Impl {
  IDxcCompiler3* c = nullptr;
};

Dxc::Dxc() {
  DxcCreateInstanceProc proc = g_create.load();
  if (!proc) return;
  IDxcCompiler3* c = nullptr;
  if (FAILED(proc(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), reinterpret_cast<void**>(&c))) || !c) return;
  impl_ = new Impl{c};
}

Dxc::~Dxc() {
  if (impl_) {
    impl_->c->Release();
    delete impl_;
  }
}

bool Dxc::compile(const std::string& hlsl, bool pixel, std::vector<uint8_t>& out, std::string* errors) {
  out.clear();
  if (!impl_) {
    if (errors) *errors = "dxc not loaded";
    return false;
  }
  DxcBuffer source{};
  source.Ptr = hlsl.c_str();
  source.Size = hlsl.size();
  // dxc_compiler.cpp DxcCompiler::compile(src, pixel, /*library*/false, /*spirv*/false): the
  // same argument strings in the same order ("-T vs_6_0" is ONE argument there too).
  const wchar_t* args[] = {pixel ? L"-T ps_6_0" : L"-T vs_6_0", L"-HV 2021", L"-all-resources-bound",
                           L"-Wno-ignored-attributes", L"-Qstrip_reflect", L"-Qstrip_debug"};
  IDxcResult* result = nullptr;
  HRESULT hr = impl_->c->Compile(&source, args, UINT32(sizeof(args) / sizeof(args[0])), nullptr,
                                 __uuidof(IDxcResult), reinterpret_cast<void**>(&result));
  if (FAILED(hr) || !result) {
    if (errors) *errors = "IDxcCompiler3::Compile failed";
    return false;
  }
  HRESULT status = E_FAIL;
  result->GetStatus(&status);
  bool ok = false;
  if (FAILED(status)) {
    if (errors) {
      *errors = "dxc compile failed";
      IDxcBlobUtf8* err = nullptr;
      if (result->HasOutput(DXC_OUT_ERRORS) &&
          SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), reinterpret_cast<void**>(&err),
                                      nullptr)) &&
          err) {
        *errors += ": ";
        errors->append(err->GetStringPointer(), std::min<size_t>(err->GetStringLength(), 600));
        err->Release();
      }
    }
  } else {
    IDxcBlob* obj = nullptr;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), reinterpret_cast<void**>(&obj), nullptr)) &&
        obj) {
      const uint8_t* b = static_cast<const uint8_t*>(obj->GetBufferPointer());
      out.assign(b, b + obj->GetBufferSize());
      obj->Release();
      ok = !out.empty();
    }
    if (!ok && errors) *errors = "dxc produced no object";
  }
  result->Release();
  return ok;
}

bool Dxc::disassemble(const std::vector<uint8_t>& dxil, std::string& out) {
  out.clear();
  if (!impl_ || dxil.empty()) return false;
  DxcBuffer b{};
  b.Ptr = dxil.data();
  b.Size = dxil.size();
  IDxcResult* r = nullptr;
  if (FAILED(impl_->c->Disassemble(&b, __uuidof(IDxcResult), reinterpret_cast<void**>(&r))) || !r) return false;
  IDxcBlobUtf8* txt = nullptr;
  bool ok = false;
  if (SUCCEEDED(r->GetOutput(DXC_OUT_DISASSEMBLY, __uuidof(IDxcBlobUtf8), reinterpret_cast<void**>(&txt), nullptr)) &&
      txt) {
    out.assign(txt->GetStringPointer(), txt->GetStringLength());
    txt->Release();
    ok = true;
  }
  r->Release();
  return ok;
}

// ── translation ───────────────────────────────────────────────────────────────
namespace {
// XenosRecomp's control-flow scan reads 12 bytes per step while the address is below the ucode
// size, so a size that is not a multiple of 12 reads up to 8 bytes past the ucode; zero padding
// keeps that inside our allocation (offline it read the next file bytes or heap).
std::vector<uint8_t> padded(const std::vector<uint8_t>& b) {
  std::vector<uint8_t> p(b);
  p.resize(b.size() + 64, 0);
  return p;
}
// What XenosRecomp's recompile() reads, replayed without reading anything out of bounds - the
// release build has no asserts, and in-process a bad read is fatal (harness VEH). Refuses:
//   - an exec slot whose instruction lies past the ucode (XenosRecomp reads code + address*3);
//   - an executed vertex fetch that writes a component with no vertex element at its address
//     (shader_recompiler.cpp:226 asserts; release dereferences vertexElements.end()). A pixel
//     shader has no vertex elements, so any such fetch in a PS refuses;
//   - a vertex element whose usage indexes past the 14-entry USAGE_* tables.
// The control-flow scan is XenosRecomp's own (not the Xenia walk of cfExecWalk): the CF region
// ends at the lowest NON-ZERO exec address, and CF dwords past the ucode read the container's
// following bytes (then the zero padding of padded()), exactly what the recompiler will see.
bool recompilerPreflight(const Container& c, const std::vector<uint8_t>& buf, std::string* why) {
  const size_t uoff = size_t(c.virt) + c.po, bytes = c.sz;
  auto rd = [&](size_t off) -> uint32_t { return uoff + off + 4 <= buf.size() ? be32(buf.data() + uoff + off) : 0u; };
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> el;
  if (c.isVs) {
    if (!vertexElements(c, el)) {
      if (why) *why = "preflight: vertex elements out of range";
      return false;
    }
    for (const auto& e : el)
      if (e.second.first > 13) {
        if (why) *why = "preflight: vertex element usage " + std::to_string(e.second.first) + " is past the usage tables";
        return false;
      }
  }
  // First CF scan (shader_recompiler.cpp ~1905): the CF region bound, whether control flow is
  // "simple" (no unconditional or backward jump), and ifEndLabels (forward conditional jumps
  // counted per target CF index).
  uint64_t instrSize = bytes;
  bool simple = true;
  std::unordered_map<uint32_t, uint32_t> ifEnd;
  for (uint64_t ia = 0; ia < instrSize; ia += 12) {
    uint32_t cf[2][2];
    mkey::unpackCfPair(rd(size_t(ia)), rd(size_t(ia) + 4), rd(size_t(ia) + 8), cf);
    for (int j = 0; j < 2; j++) {
      const uint32_t op = (cf[j][1] >> 12) & 0xFu;
      if (mkey::isExecOpcode(op)) {
        const uint64_t address = cf[j][0] & 0xFFFu;
        if (address != 0) instrSize = std::min<uint64_t>(instrSize, address * 12);
      } else if (op == 11) {  // CondJmp: address:13, isUnconditional bit 13; direction bit 33
        if (((cf[j][0] >> 13) & 1u) || ((cf[j][1] >> 1) & 1u)) simple = false;
        else ifEnd[cf[j][0] & 0x1FFFu]++;
      }
    }
  }
  // Indentation (simple control flow only; the switch form resets it to 3 per CF instruction).
  // XenosRecomp keeps it in a uint32_t: a LoopEnd or an if-end label with nothing open
  // decrements 0 and the next indent() emits ~4 billion tabs. Garbage container 7FCA4485...
  // (data/shaders_carved) did exactly that: a 4 GB .fail.hlsl offline, unbounded memory
  // in-process. Replay the counter and refuse any underflow.
  if (simple) {
    uint64_t ind = 1;
    uint32_t pc = 0;
    for (uint64_t ia = 0; ia < instrSize; ia += 12) {
      uint32_t cf[2][2];
      mkey::unpackCfPair(rd(size_t(ia)), rd(size_t(ia) + 4), rd(size_t(ia) + 8), cf);
      for (int j = 0; j < 2; j++, pc++) {
        auto it = ifEnd.find(pc);
        if (it != ifEnd.end()) {
          if (ind < it->second) {
            if (why) *why = "preflight: control flow closes more blocks than it opens (if-end at CF " + std::to_string(pc) + ")";
            return false;
          }
          ind -= it->second;
        }
        const uint32_t op = (cf[j][1] >> 12) & 0xFu;
        if (op == 7) ind++;           // LoopStart
        else if (op == 8) {           // LoopEnd
          if (ind == 0) {
            if (why) *why = "preflight: LoopEnd with no open loop at CF " + std::to_string(pc);
            return false;
          }
          ind--;
        } else if (op == 11) ind++;   // forward conditional CondJmp opens an if block
      }
    }
  }
  std::string uncovered;
  for (uint64_t ia = 0; ia < instrSize; ia += 12) {
    uint32_t cf[2][2];
    mkey::unpackCfPair(rd(size_t(ia)), rd(size_t(ia) + 4), rd(size_t(ia) + 8), cf);
    for (int j = 0; j < 2; j++) {
      if (!mkey::isExecOpcode((cf[j][1] >> 12) & 0xFu)) continue;
      const uint32_t address = cf[j][0] & 0xFFFu, count = (cf[j][0] >> 12) & 0x7u;
      uint32_t seq = (cf[j][0] >> 16) & 0xFFFu;
      for (uint32_t i = 0; i < count; i++, seq >>= 2) {
        const size_t slot = size_t(address) + i;
        if (slot * 12 + 12 > bytes) {
          if (why) *why = "preflight: exec names instruction " + std::to_string(slot) + " past the ucode";
          return false;
        }
        if (!(seq & 1u)) continue;
        const uint32_t d0 = be32(buf.data() + uoff + slot * 12), d1 = be32(buf.data() + uoff + slot * 12 + 4);
        if ((d0 & 0x1Fu) == 0 && writesSomething(d1 & 0xFFFu) && !el.count(uint32_t(slot)))
          uncovered += (uncovered.empty() ? "" : ",") + std::to_string(slot);
      }
    }
  }
  if (!uncovered.empty()) {
    if (why)
      *why = std::string("preflight: vfetch at [") + uncovered + "] has no vertex element" +
             (c.isVs ? "" : " (a pixel shader has none)");
    return false;
  }
  return true;
}

bool signedDxil(const std::vector<uint8_t>& d) {
  uint32_t w = 0;
  if (d.size() < 8) return false;
  std::memcpy(&w, d.data() + 4, 4);  // main.cpp: dword 1 (digest) must be non-zero
  return w != 0;
}
}  // namespace

bool preflightContainer(const Container& c, std::string* why) {
  return recompilerPreflight(c, padded(c.bytes), why);
}

// iter-467 release/installer: the container with its float4 literal definitions removed and the
// literal values they pointed at zeroed. XenosRecomp bakes a definition into the HLSL as a
// constant (`float4 cN = asfloat(...)`); without one it reads register N from the live constant
// buffer (g_UnnamedConstants). Xenos ALU instructions have no immediate operand: a literal reaches
// the GPU only through the constant file (D3D loads it there), which is where Xenia, which never
// sees a container, reads it. `regs` names the registers. False when there is no float4 literal.
bool stripFloatLiterals(const Container& c, Container& out, std::string* regs) {
  const uint8_t* p = c.bytes.data();
  const uint32_t defOff = be32(p + 0x14);
  if (!defOff || uint64_t(defOff) + 0x14 > c.virt) return false;
  const uint32_t size = be32(p + defOff + 0x10);
  const uint32_t start = defOff + 0x14, end = start + size;
  if (size < 4 || (size & 3u) || end > c.virt) return false;
  uint32_t i = start;
  std::vector<uint8_t> b = c.bytes;
  std::string names;
  while (i + 4 <= end && be32(p + i) != 0) {
    if (i + 8 > end) return false;
    const uint32_t w0 = be32(p + i), physOff = be32(p + i + 4);
    const uint32_t reg = w0 >> 16, count = w0 & 0xFFFFu;
    const uint64_t at = uint64_t(c.virt) + physOff, len = uint64_t((count + 3) / 4) * 16;
    if (!count || at + len > c.bytes.size()) return false;
    std::fill(b.begin() + std::ptrdiff_t(at), b.begin() + std::ptrdiff_t(at + len), uint8_t(0));
    const uint32_t first = reg - (c.isVs ? 0u : 256u), n = (count + 3) / 4;
    if (!names.empty()) names += ", ";
    names += "c" + std::to_string(first) + (n > 1 ? "..c" + std::to_string(first + n - 1) : "");
    i += 8;
  }
  if (i == start || i + 4 > end) return false;  // no float4 literal, or no terminator
  // The definition list without its float4 part: a terminator, then the int4 / bool lists as they were.
  std::vector<uint8_t> rest(b.begin() + i + 4, b.begin() + end);
  std::fill(b.begin() + start, b.begin() + end, uint8_t(0));
  std::copy(rest.begin(), rest.end(), b.begin() + start + 4);
  std::string why;
  if (!parseContainer(b.data(), b.size(), c.origin + " (float4 literals read from the constant file)", out, &why))
    return false;
  if (regs) *regs = names;
  return true;
}

bool translateContainer(const Container& c, Dxc& dxc, Files& out, std::string* why) {
  out = Files{};
  out.isVs = c.isVs;
  const std::vector<uint8_t> buf = padded(c.bytes);
  if (!recompilerPreflight(c, buf, why)) return false;
  xenos::RecompileOut r;
  if (!xenos::recompile(buf.data(), false, r)) {
    if (why) *why = "recompiler threw: " + r.error;
    return false;
  }
  if (!r.unsupported.empty()) {
    if (why) *why = "unsupported: " + r.unsupported;
    return false;
  }
  if (r.isPixelShader == c.isVs) {
    if (why) *why = "recompiler disagrees on the shader kind";
    return false;
  }
  std::string err;
  if (!dxc.compile(r.hlsl, r.isPixelShader, out.dxil, &err)) {
    if (why) *why = "dxc DXIL compile failed: " + err;
    return false;
  }
  if (!signedDxil(out.dxil)) {
    if (why) *why = "DXIL was not signed properly";
    return false;
  }
  out.hlsl = std::move(r.hlsl);
  out.filterOverrides = r.filterOverrides;
  if (c.isVs) {
    out.sidecar = std::move(r.sidecar);
    xenos::RecompileOut f;
    std::vector<uint8_t> vsf;
    if (xenos::recompile(buf.data(), true, f) && dxc.compile(f.hlsl, false, vsf, nullptr))
      out.vsf = std::move(vsf);
    else
      out.vsfFailHlsl = std::move(f.hlsl);
    std::vector<uint8_t> m;
    if (mkey::maskedUcode(c.ucode(), c.sz, m)) out.mkey = XXH3_64bits(m.data(), m.size());
  } else {
    // step 4 lane C (0005): the alpha-test variant, as main.cpp's second recompile of a pixel shader. A variant
    // dxc cannot compile is left out (offline: AT-SKIP) with its reason in atSkip (round 4: the manifest says so,
    // and the draws the test applies to are dropped - gfx/alpha_test.hpp use() - not drawn without it).
    xenos::RecompileOut a;
    std::vector<uint8_t> at;
    if (!xenos::recompile(buf.data(), false, a, true)) out.atSkip = "the alpha-test recompile threw";
    else if (!a.unsupported.empty()) out.atSkip = "unsupported in the alpha-test recompile";
    else if (!a.alphaTestEmitted) {}  // no colour 0: no variant, nothing skipped
    else if (!dxc.compile(a.hlsl, true, at, nullptr)) out.atSkip = "dxc failed on the alpha-test variant";
    else if (!signedDxil(at)) out.atSkip = "the alpha-test variant's DXIL is not signed";
    else out.at = std::move(at);
  }
  return true;
}

const char* statusName(Status s) {
  switch (s) {
    case Status::Ok: return "OK";
    case Status::NoTemplate: return "NO-TEMPLATE";
    case Status::Refused: return "REFUSED";
    default: return "FAILED";
  }
}

namespace {

// Group containers by byte-identical metadata; returns the number of groups and fills the first.
size_t metadataGroups(const std::vector<const Container*>& in, std::vector<const Container*>& first,
                      bool* differOnlyInCreateFields) {
  std::vector<std::vector<const Container*>> groups;
  for (const Container* c : in) {
    bool placed = false;
    for (auto& g : groups)
      if (sameMetadata(*g[0], *c)) {
        g.push_back(c);
        placed = true;
        break;
      }
    if (!placed) groups.push_back({c});
  }
  first = groups.empty() ? std::vector<const Container*>{} : groups[0];
  if (differOnlyInCreateFields) {
    // Census aid for a refusal: do the groups differ only in +0x1C..+0x23, the two fields
    // CreateShader fills in (XenosRecomp never reads them)?
    bool only = groups.size() > 1;
    for (size_t i = 1; only && i < groups.size(); i++) {
      std::vector<uint8_t> a = groups[0][0]->bytes, b = groups[i][0]->bytes;
      if (a.size() != b.size()) { only = false; break; }
      std::fill(a.begin() + 0x1C, a.begin() + 0x24, 0);
      std::fill(b.begin() + 0x1C, b.begin() + 0x24, 0);
      Container ca = *groups[0][0], cb = *groups[i][0];
      ca.bytes = a;
      cb.bytes = b;
      only = sameMetadata(ca, cb);
    }
    *differOnlyInCreateFields = only;
  }
  return groups.size();
}

// The installed .vfetch is text: a checkout with core.autocrlf=true writes it CRLF while the
// recompiler emits LF. The boot loader's sscanf ignores the CR; so does this comparison.
std::string noCR(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char ch : s)
    if (ch != '\r') o.push_back(ch);
  return o;
}

bool installedHas(const InstalledLookup& inst, uint64_t h, bool isVs) {
  return inst && inst(h, isVs, nullptr);
}

std::string joinI64(const std::vector<int64_t>& v) {
  std::string s = "[";
  for (size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + std::to_string(v[i]);
  return s + "]";
}

// Every executed vfetch that writes a component has a vertex element at its address (the
// XenosRecomp input-layout path asserts it; release build: an end() iterator dereference).
bool elementsCover(const Container& tmpl, const uint8_t* u, size_t bytes, std::string* uncovered) {
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> el;
  if (!vertexElements(tmpl, el)) return false;
  std::vector<uint32_t> slots;
  mkey::vfetchSlots(u, bytes, slots);
  bool ok = true;
  for (uint32_t s : slots) {
    const uint32_t d1 = be32(u + size_t(s) * 12 + 4);
    if (writesSomething(d1 & 0xFFFu) && !el.count(s)) {
      ok = false;
      if (uncovered) *uncovered += (uncovered->empty() ? "" : ",") + std::to_string(s);
    }
  }
  return ok;
}

}  // namespace

// step 4 lane C round 4: a PS manifest's alpha-test line - the variant, none (no colour 0), or AT-SKIP (why).
// shader_runtime_xlat.cpp entryComplete keys on "at alpha-test variant".
static std::string atLine(const Files& f) {
  if (!f.at.empty()) return "at alpha-test variant\n";
  if (f.atSkip.empty()) return "at none (the PS writes no colour 0)\n";
  return "at AT-SKIP (variant unavailable: " + f.atSkip + ")\n";
}

Result translate(uint64_t h, bool isVs, const uint8_t* ucode, size_t bytes, const ContainerIndex& index,
                 Dxc& dxc, const InstalledLookup& installed) {
  Result res;
  const char* kind = isVs ? "VS" : "PS";
  std::ostringstream man;
  man << "shader " << hex16(h) << " " << kind << "\n"
      << "translator " << xenos::translatorSourceId() << " dxc " << hex16(dxcDllHash()) << "\n";
  if (!ucode || bytes == 0 || (bytes & 3u) || XXH3_64bits(ucode, bytes) != h) {
    res.status = Status::Refused;
    res.reason = "runtime ucode does not hash to its key";
    return res;
  }

  // ── EXACT: a container holds this very ucode (offline main.cpp per container) ──
  std::vector<const Container*> exact;
  index.exactUcode(h, isVs, exact);
  if (!exact.empty()) {
    std::sort(exact.begin(), exact.end(), [](const Container* a, const Container* b) { return a->origin < b->origin; });
    std::vector<const Container*> group;
    bool onlyCreate = false;
    const size_t ng = metadataGroups(exact, group, &onlyCreate);
    bool agreed = false;
    if (ng > 1) {
      // iter-467 release/installer: THE AGREEMENT RULE. Several metadata variants hold this very
      // ucode (the xex carries some PS twice). Translate one container of every variant: when the
      // translations are byte-identical (DXIL, sidecar, fetch variant), the differing metadata never
      // reaches the output, so the answer is the same whichever container the game created - proof
      // by construction, not a guess. Any difference keeps the refusal. Before, such shaders existed
      // only in the offline AOT cache, which a release does not ship.
      std::vector<const Container*> reps;
      for (const Container* c : exact) {
        bool seen = false;
        for (const Container* r : reps) seen = seen || sameMetadata(*r, *c);
        if (!seen) reps.push_back(c);
      }
      Files first;
      bool same = true;
      std::string why0;
      for (size_t i = 0; i < reps.size() && same; i++) {
        Files f;
        std::string w;
        if (!translateContainer(*reps[i], dxc, f, &w)) {
          same = false;
          why0 = w;
          break;
        }
        if (i == 0) first = std::move(f);
        else same = f.dxil == first.dxil && f.sidecar == first.sidecar && f.vsf == first.vsf && f.at == first.at;
      }
      // iter-467 release/installer: THE LITERAL RULE. The variants may differ only in the VALUES of
      // their float4 literals (PS 9EF298A6: the xex's YUV->RGB shader twice, BT.601 and BT.709
      // coefficients in c252..c255). The GPU takes a literal from the constant file - the ucode is
      // the same, and Xenos has no immediate operand - so translate the shader reading those
      // registers live: proof that this is the only difference is that the containers are byte-
      // identical once their float4 literals are removed (stripFloatLiterals).
      std::vector<Container> stripped;
      std::string literalRegs;
      if (!same && why0.empty()) {
        for (const Container* r : reps) {
          Container sc;
          std::string regs;
          if (!stripFloatLiterals(*r, sc, &regs) ||
              (!stripped.empty() && (sc.bytes != stripped[0].bytes))) {
            stripped.clear();
            break;
          }
          if (stripped.empty()) literalRegs = regs;
          stripped.push_back(std::move(sc));
        }
      }
      if (!same && !stripped.empty()) {
        std::string w;
        if (!translateContainer(stripped[0], dxc, res.files, &w)) {
          res.status = Status::Failed;
          res.reason = "literal rule: " + w;
          return res;
        }
        res.status = Status::Ok;
        res.path = "exact";
        res.container = stripped[0].bytes;
        man << "path exact (offline XenosRecomp main.cpp per container)\n"
            << "container " << reps[0]->origin << " (" << exact.size() << " holding this ucode, " << ng
            << " metadata variants that differ only in their float4 literal values (" << literalRegs
            << "): translated reading them from the constant file, where the GPU takes them)\n";
        if (!isVs) man << atLine(res.files);
        res.manifest = man.str();
        return res;
      }
      if (!same) {
        res.status = Status::Refused;
        res.reason = std::to_string(ng) + " different metadata among " + std::to_string(exact.size()) +
                     " containers holding this exact ucode" +
                     (onlyCreate ? " (they differ only in +0x1C..+0x23, the fields CreateShader fills)" : "") +
                     (why0.empty() ? ", and their translations differ" : ", and one does not translate: " + why0);
        return res;
      }
      agreed = true;
    }
    // A VS ucode seen at IM_LOAD has been patched at bind time, so the stage rule applies here
    // too (stage_missing_vs.py rule 2): ONE metadata among every same-program (patch-masked)
    // container, not only among those holding these exact bytes.
    if (isVs) {
      std::vector<uint8_t> mk;
      std::vector<const Container*> same, sameFirst;
      bool sameOnlyCreate = false;
      size_t sng = 0;
      if (patchMaskedUcode(ucode, bytes, mk)) {
        index.sameProgram(mk, same);
        if (!same.empty()) sng = metadataGroups(same, sameFirst, &sameOnlyCreate);
      }
      if (sng > 1) {
        res.status = Status::Refused;
        res.reason = std::to_string(sng) + " different metadata among " + std::to_string(same.size()) +
                     " same-program containers (exact path)" +
                     (sameOnlyCreate ? " (they differ only in +0x1C..+0x23, the fields CreateShader fills)" : "");
        return res;
      }
    }
    const Container& c = *group[0];
    std::string uncovered;
    if (isVs && !elementsCover(c, ucode, bytes, &uncovered)) {
      res.status = Status::Refused;
      res.reason = "vfetch at [" + uncovered + "] has no vertex element in the container";
      return res;
    }
    std::string why;
    if (!translateContainer(c, dxc, res.files, &why)) {
      res.status = Status::Failed;
      res.reason = why;
      return res;
    }
    res.status = Status::Ok;
    res.path = "exact";
    res.container = c.bytes;
    man << "path exact (offline XenosRecomp main.cpp per container)\n"
        << "container " << c.origin << " (" << exact.size() << " holding this ucode, "
        << (agreed ? std::to_string(ng) + " metadata variants whose translations agree byte for byte, first "
                   : std::string("1 metadata "))
        << hex16(c.metaHash) << ")\n";
    if (isVs && res.files.vsf.empty()) man << "vsf VSF-SKIP (non-fatal, as offline)\n";
    if (!isVs) man << atLine(res.files);
    res.manifest = man.str();
    return res;
  }
  if (!isVs) {
    res.status = Status::NoTemplate;
    res.reason = "no captured or corpus PS container holds this ucode";
    return res;
  }

  // ── PROVED: stage_missing_vs.py main() for one runtime ucode ──
  std::vector<uint8_t> key;
  if (!patchMaskedUcode(ucode, bytes, key)) {
    res.status = Status::Refused;
    res.reason = "malformed control flow";
    return res;
  }
  std::vector<const Container*> same;
  index.sameProgram(key, same);
  if (same.empty()) {
    res.status = Status::NoTemplate;
    res.reason = "no container runs the same program (no template)";
    return res;
  }
  std::vector<const Container*> members;
  bool onlyCreate = false;
  const size_t ng = metadataGroups(same, members, &onlyCreate);
  size_t agreedGroups = 0;
  if (ng > 1) {
    // iter-467 release/installer: THE AGREEMENT RULE, as on the EXACT path. Several metadata
    // variants run this program (118071C8: the XEX's container and one the game creates at run
    // time). Splice the runtime ucode into one container of every variant and translate each:
    // when every translation is byte-identical (DXIL, sidecar, fetch variant) the differing
    // metadata never reaches the output, so whichever container the game created gives the same
    // answer. Any difference - or a variant the proof or the element check refuses - keeps the
    // refusal.
    std::vector<const Container*> reps;
    for (const Container* c : same) {
      bool seen = false;
      for (const Container* r : reps) seen = seen || sameMetadata(*r, *c);
      if (!seen) reps.push_back(c);
    }
    std::stable_sort(reps.begin(), reps.end(), [](const Container* a, const Container* b) { return a->origin < b->origin; });
    Files first;
    std::string why0;
    bool agree = true;
    for (size_t i = 0; i < reps.size() && agree; i++) {
      const ProofReport pr = proveSameProgram(reps[i]->ucode(), ucode, bytes);
      std::string unc;
      if (!pr.ok || !elementsCover(*reps[i], ucode, bytes, &unc)) {
        agree = false;
        why0 = "variant " + reps[i]->origin + " is not proved for this ucode";
        break;
      }
      Container sp = *reps[i];
      std::memcpy(sp.bytes.data() + sp.virt + sp.po, ucode, bytes);
      Files f;
      std::string w;
      if (!translateContainer(sp, dxc, f, &w)) {
        agree = false;
        why0 = "variant " + reps[i]->origin + " does not translate: " + w;
        break;
      }
      if (i == 0) first = std::move(f);
      else if (!(f.dxil == first.dxil && f.sidecar == first.sidecar && f.vsf == first.vsf && f.at == first.at)) {
        agree = false;
        why0 = "the translations of " + reps[0]->origin + " and " + reps[i]->origin + " differ";
      }
    }
    if (!agree) {
      res.status = Status::Refused;
      res.reason = std::to_string(ng) + " different metadata among same-program containers" +
                   (onlyCreate ? " (they differ only in +0x1C..+0x23, the fields CreateShader fills)" : "") + "; " +
                   why0;
      return res;
    }
    agreedGroups = ng;
    // Continue with the variant of the first representative (its translation is the agreed one).
    members.clear();
    for (const Container* c : same)
      if (sameMetadata(*reps[0], *c)) members.push_back(c);
  }
  // Template: a member whose ucode is in the installed cache, else the first by origin.
  std::stable_sort(members.begin(), members.end(), [&](const Container* a, const Container* b) {
    const bool ca = installedHas(installed, a->ucodeHash, true), cb = installedHas(installed, b->ucodeHash, true);
    if (ca != cb) return ca;
    return a->origin < b->origin;
  });
  const Container& tmpl = *members[0];
  const ProofReport proof = proveSameProgram(tmpl.ucode(), ucode, bytes);
  if (!proof.ok) {
    res.status = Status::Refused;
    res.reason = "same-program proof failed:";
    for (size_t i = 0; i < proof.problems.size() && i < 4; i++) res.reason += " " + proof.problems[i] + ";";
    return res;
  }
  std::string uncovered;
  if (!elementsCover(tmpl, ucode, bytes, &uncovered)) {
    res.status = Status::Refused;
    res.reason = "vfetch at [" + uncovered + "] has no vertex element in the template";
    return res;
  }
  Container sp = tmpl;
  std::memcpy(sp.bytes.data() + sp.virt + sp.po, ucode, bytes);
  sp.ucodeHash = XXH3_64bits(sp.ucode(), sp.sz);
  if (sp.ucodeHash != h) {
    res.status = Status::Failed;
    res.reason = "spliced container does not hash to the key";
    return res;
  }
  sp.origin = "spliced(" + tmpl.origin + ")";
  std::string why;
  if (!translateContainer(sp, dxc, res.files, &why)) {
    res.status = Status::Failed;
    res.reason = why;
    return res;
  }
  if (res.files.vsf.empty()) {  // stage_missing_vs.py: FAILED, XenosRecomp produced no .vs.vsf.dxil
    res.status = Status::Failed;
    res.reason = "XenosRecomp produced no .vs.vsf.dxil";
    return res;
  }
  // Reference: the lowest-hash cached sibling, else the template itself.
  std::vector<uint64_t> sibs;
  for (const Container* m : members) sibs.push_back(m->ucodeHash);
  std::sort(sibs.begin(), sibs.end());
  sibs.erase(std::unique(sibs.begin(), sibs.end()), sibs.end());
  uint64_t ref = tmpl.ucodeHash;
  bool refCached = false;
  for (uint64_t s : sibs)
    if (installedHas(installed, s, true)) {
      ref = s;
      refCached = true;
      break;
    }
  const Container* refSrc = nullptr;
  for (const Container* m : members)
    if (m->ucodeHash == ref) {
      refSrc = m;
      break;
    }
  Files refFiles;
  std::string refWhy;
  const bool refOk = refSrc && translateContainer(*refSrc, dxc, refFiles, &refWhy);

  // 4. sidecar == the rows derived from the runtime ucode
  const bool c4 = [&] {
    const auto a = parseSidecarRows(res.files.sidecar), b = deriveFetches(ucode, bytes);
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
      if (a[i].slot != b[i].slot || a[i].offset != b[i].offset || a[i].stride != b[i].stride ||
          a[i].format != b[i].format || a[i].isSigned != b[i].isSigned || a[i].isInteger != b[i].isInteger)
        return false;
    return true;
  }();
  // 5. the vsf DXIL encodes the runtime layout
  std::string dis;
  const bool haveDis = dxc.disassemble(res.files.vsf, dis);
  const VsfAnalysis va = haveDis ? analyseVsf(dis) : VsfAnalysis{};
  const std::vector<SidecarRow> der = deriveFetches(ucode, bytes);
  std::set<int64_t> expMul, expRegs, expOffs, first;
  {
    std::vector<uint32_t> slots;
    mkey::vfetchSlots(ucode, bytes, slots);
    for (uint32_t s : slots) {
      const Vfetch f = decodeVfetch(be32(ucode + s * 12), be32(ucode + s * 12 + 4), be32(ucode + s * 12 + 8));
      if (!f.mini && f.stride) expMul.insert(int64_t(f.stride) * 4);
    }
    for (const auto& r : der) {
      expRegs.insert(r.slot >> 1);
      for (int k = 0; k < fmtWords(r.format); k++) expOffs.insert(int64_t(r.offset) * 4 + 4 * k);
      first.insert(int64_t(r.offset) * 4);
    }
  }
  std::set<int64_t> gotMul(va.muls.begin(), va.muls.end());
  gotMul.erase(0);
  const bool c5mul = haveDis && gotMul == expMul;
  const bool c5regs = haveDis && std::set<int64_t>(va.regs.begin(), va.regs.end()) == expRegs;
  const std::set<int64_t> gotOffs(va.offs.begin(), va.offs.end());
  const bool c5offs = haveDis && std::includes(expOffs.begin(), expOffs.end(), gotOffs.begin(), gotOffs.end()) &&
                      std::includes(gotOffs.begin(), gotOffs.end(), first.begin(), first.end());
  // 6. against the reference translation
  bool c6sig = false, c6hlsl = false, c6ctl = true;
  size_t diffN = 0, nonVf = 0;
  std::string used;
  if (refOk) {
    std::vector<SigRow> s1, s2;
    inputSignature(res.files.dxil, s1);
    inputSignature(refFiles.dxil, s2);
    c6sig = !s1.empty() && s1.size() == s2.size();
    for (size_t i = 0; c6sig && i < s1.size(); i++) {
      c6sig = s1[i].sameDecl(s2[i]);
      if (s1[i].rwMask != s2[i].rwMask) used += " " + s1[i].name + std::to_string(s1[i].index);
    }
    const auto d = diffLines(refFiles.hlsl, res.files.hlsl);
    diffN = d.size();
    for (const auto& l : d)
      if (!isVfetchLine(l.substr(1))) nonVf++;
    c6hlsl = nonVf == 0;
    if (refCached) {
      InstalledFiles inst;
      c6ctl = installed(ref, true, &inst) && inst.dxil == refFiles.dxil && inst.vsf == refFiles.vsf &&
              noCR(inst.sidecar) == noCR(refFiles.sidecar);
    }
  }
  const bool ok = c4 && c5mul && c5regs && c5offs && refOk && c6sig && c6hlsl && c6ctl;

  man << "path proved (tools/stage_missing_vs.py)\n"
      << "template " << tmpl.origin << " (ucode " << hex16(tmpl.ucodeHash) << "; " << same.size()
      << " same-program containers, "
      << (agreedGroups ? std::to_string(agreedGroups) + " metadata variants whose translations agree byte for byte, first "
                       : std::string("1 metadata "))
      << hex16(tmpl.metaHash) << "; ref " << hex16(ref)
      << (refCached ? " cached" : " (template itself)") << ")\n"
      << "proof: CF " << proof.cfDwords << " dwords identical, " << proof.alu << " ALU + " << proof.tfetch
      << " TFETCH identical, " << proof.vfetch << " VFETCH (" << proof.diffs.size()
      << " differ in patch fields only)\n";
  for (const auto& vd : proof.diffs) man << "   i" << vd.instr << ": " << vd.fields << "\n";
  man << "checks: sidecar==ucode " << (c4 ? "True" : "False") << " | vsf mul " << joinI64(va.muls) << " exp "
      << joinI64(std::vector<int64_t>(expMul.begin(), expMul.end())) << " " << (c5mul ? "True" : "False")
      << " | cb rows " << joinI64(va.regs) << " " << (c5regs ? "True" : "False") << " | load offs "
      << (c5offs ? "True"
                 : "False (got " + joinI64(std::vector<int64_t>(gotOffs.begin(), gotOffs.end())) + " expected within " +
                       joinI64(std::vector<int64_t>(expOffs.begin(), expOffs.end())) + " covering " +
                       joinI64(std::vector<int64_t>(first.begin(), first.end())) + ")")
      << " | ISG1==ref " << (c6sig ? "True" : "False")
      << (used.empty() ? "" : " (used differs:" + used + ")") << " | hlsl diff " << diffN
      << " lines, non-vfetch " << nonVf << " " << (c6hlsl ? "True" : "False");
  if (refCached) man << " | ref retranslation==cache " << (c6ctl ? "True" : "False");
  if (!refOk) man << " | reference did not translate: " << refWhy;
  man << "\n";
  res.manifest = man.str();
  res.path = "proved";
  res.container = sp.bytes;
  if (!ok) {
    res.status = Status::Refused;
    res.reason = "post-translation checks failed: ";
    if (!c4) res.reason += "sidecar!=ucode ";
    if (!c5mul) res.reason += "vsf-multipliers ";
    if (!c5regs) res.reason += "vsf-cb-rows ";
    if (!c5offs) res.reason += "vsf-load-offsets ";
    if (!refOk) res.reason += "reference-untranslatable ";
    if (refOk && !c6sig) res.reason += "input-signature ";
    if (refOk && !c6hlsl) res.reason += "hlsl-non-vfetch-diff ";
    if (!c6ctl) res.reason += "reference-retranslation!=cache ";
    return res;
  }
  res.status = Status::Ok;
  return res;
}

}  // namespace xlat
}  // namespace gfx
}  // namespace sr
