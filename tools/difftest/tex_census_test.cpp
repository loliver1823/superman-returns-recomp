// iter-458 (Astra 068, corrected per 069): the texture-path allocation/upload
// census (kernel/tex_census). REAL code wherever a seam allows it:
//   * grants through the phys_window registration/lifetime API and the
//     MmFreePhysicalMemory export;
//   * every DRAW through the executor's census prologue + the real buildDraw
//     (executeDrawThroughTapForTest), after a real SetRenderState fetch file
//     and a real SetTexture through the executor;
//   * one upload with a real payload through publish() -> render thread ->
//     executor arm;
//   * the recorder window through the REAL recorder (hotkey arm, Presents), so
//     its open/close edges and the close hook's flush are the production path.
// The producer tap (hle_api_observe's bind capture) is exercised end to end by
// the exe suite (SR_HLE_TEX_UNLOCK_TEST, C14). Producer-side decodes here call
// the census directly with a simulated FIFO clock (txcSetFifoForTest).
//
// Run: build/difftest.exe --tex-census-test   (exit code = failed checks)
// The run KEEPS %TEMP%\sr_txc_test.bin and %TEMP%\sr_txc_rec.srht for
// tools/tex_census.py --integration.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/export_registry.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/phys_window.hpp"
#include "kernel/stubs.hpp"
#include "kernel/tex_census.hpp"
#include "gfx/hle_video.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace K = sr::kernel;
namespace W = sr::kernel::physwin;
namespace T = sr::kernel::txc;
namespace H = sr::gfx::hle;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
  std::fflush(stdout);
}
void fillv(uint32_t va, uint8_t v, uint32_t n) {
  if (uint8_t* p = K::guestVAToHost(va)) std::memset(p, v, n);
}
std::vector<uint8_t> snap(uint32_t va, uint32_t n) {
  const uint8_t* p = K::guestVAToHost(va);
  return p ? std::vector<uint8_t>(p, p + n) : std::vector<uint8_t>(n, 0);
}
std::vector<T::Ev> events() {
  std::vector<T::Ev> v(1u << 16);
  v.resize((size_t)T::eventsForTest(v.data(), v.size()));
  return v;
}
std::vector<T::Ev> ofType(uint32_t t) {
  std::vector<T::Ev> r;
  for (const T::Ev& e : events()) if (e.type == t) r.push_back(e);
  return r;
}
T::Ev lastOf(uint32_t t) {
  const auto v = ofType(t);
  return v.empty() ? T::Ev{} : v.back();
}
uint32_t lastRegSerial() { return lastOf(T::kGrantReg).d; }
std::vector<T::Ev> drawsOf(uint32_t key) {
  std::vector<T::Ev> r;
  for (const T::Ev& e : events()) if (e.type == T::kDrawTex && e.c == key) r.push_back(e);
  return r;
}
void be32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

// The simulated FIFO: every command is executed in publish order, so a
// command's publish index is also its execute index.
uint64_t g_fifo = 0;
uint64_t publishOne() {
  ++g_fifo;
  H::txcSetFifoForTest(g_fifo, g_fifo);
  return g_fifo;
}
// One command through the REAL executor at the next FIFO index (recorded
// first, as renderThreadMain does - a no-op until the recorder is open).
void execOne(const H::RenderCommand& c) {
  ++g_fifo;
  H::txcSetFifoForTest(g_fifo, g_fifo - 1);   // published; execute() advances the other
  H::traceRecordForTest(c);
  H::executeForTest(c);
}
// Producer: an ISSUED bind (snapshot of the 0x8 alias, published, decoded);
// render thread: the slot write and the arm's apply witness.
void uploadAndApply(uint32_t key, uint32_t span, uint32_t dims, uint32_t fmt,
                    uint32_t band = T::kBand2D) {
  const T::BindToken tok = T::bindBegin();
  const std::vector<uint8_t> payload = snap(0x80000000u | key, span);
  const uint64_t i = publishOne();
  T::onTexPublish(key, 0x100, span, dims, fmt, i, payload.data());
  T::onTexDecode(tok, key, span, fmt, dims, true, 8);
  T::onResident(key, band, T::kFromUpload, i);
  T::onTexApply(key, T::kApplied, band, dims, fmt, i);
}
void suppressed(uint32_t key, uint32_t span, uint32_t fmt, uint32_t dims) {
  const T::BindToken tok = T::bindBegin();
  T::onTexDecode(tok, key, span, fmt, dims, false, 0);
}
// A draw through the REAL tap: fetch file (sampler 0) + SetTexture through the
// executor, then the draw command through the executor's census prologue and
// the real buildDraw.
H::RenderCommand drawCmd(H::RenderCommandType type = H::RenderCommandType::DrawVertices) {
  static const uint8_t verts[48] = {};
  H::RenderCommand d{};
  d.type = type;
  d.draw.device = 0;
  d.draw.primType = 4;
  d.draw.start = 0;
  d.draw.count = 3;
  d.draw.vertArena = H::poolWrite(verts, sizeof verts);
  d.draw.vertBytes = sizeof verts;
  d.draw.stride = 16;
  return d;
}
void bindFetch(uint32_t key, uint32_t fmt, uint32_t dims, uint32_t dim, uint32_t depth = 1,
               uint32_t fetchBase = 0) {
  uint8_t blk[236 + 768] = {};
  uint8_t* f = blk + 236;   // sampler 0's fetch entry
  be32(f + 0, 2u);          // a TEXTURE-type entry
  be32(f + 4, ((fetchBase ? fetchBase : key) & 0xFFFFF000u) | (fmt & 0x3Fu));
  be32(f + 8, ((dims >> 16) - 1u) | (((dims & 0xFFFFu) - 1u) << 13) | ((depth - 1u) << 26));
  be32(f + 20, (dim & 3u) << 9);
  H::RenderCommand rs{};
  rs.type = H::RenderCommandType::SetRenderState;
  rs.setRenderState.device = 0;
  rs.setRenderState.blockOffset = H::blockWrite(blk, sizeof blk);
  rs.setRenderState.bytes = sizeof blk;
  execOne(rs);
  H::RenderCommand st{};
  st.type = H::RenderCommandType::SetTexture;
  st.setTexture.sampler = 0;
  st.setTexture.texture = key;
  execOne(st);
}
uint64_t realDraw(uint32_t key, uint32_t fmt, uint32_t dims, uint32_t dim = 1,
                  uint32_t depth = 1, uint32_t fetchBase = 0,
                  H::RenderCommandType type = H::RenderCommandType::DrawVertices) {
  bindFetch(key, fmt, dims, dim, depth, fetchBase);
  const H::RenderCommand d = drawCmd(type);
  ++g_fifo;
  H::txcSetFifoForTest(g_fifo, g_fifo - 1);
  H::traceRecordForTest(d);
  H::executeDrawThroughTapForTest(d);
  return g_fifo;
}
// The draw whose PUBLISH happened at E (already on the clock) executes now.
void executeDrawPublishedAt(uint64_t E) {
  const H::RenderCommand d = drawCmd();
  H::txcSetFifoForTest(g_fifo, E - 1);
  H::traceRecordForTest(d);
  H::executeDrawThroughTapForTest(d);
}
}  // namespace

int runTexCensusTest() {
  std::printf("[TXC_TEST] texture-path allocation/upload census\n");
  const char* tmp = std::getenv("TEMP");
  const std::string dir = std::string(tmp ? tmp : ".");
  const std::string path = dir + "\\sr_txc_test.bin";
  const std::string path2 = dir + "\\sr_txc_test2.bin";
  const std::string srht = dir + "\\sr_txc_rec.srht";
  std::remove(path.c_str());
  std::remove(srht.c_str());
  // The REAL recorder, armed by the operator hotkey path (read once per process).
  _putenv_s("SR_HLE_TRACE_REC", srht.c_str());
  _putenv_s("SR_HLE_TRACE_HOTKEY", "1");
  _putenv_s("SR_HLE_TRACE_FRAMES", "2");
  for (const char* e : {"SR_HLE_TRACE_ARM_TEXKEY", "SR_HLE_TRACE_ARM_UNLOCKN",
                        "SR_HLE_TRACE_ARM_RESOLVEN", "SR_HLE_TRACE_ARM_PRERESOLVE",
                        "SR_HLE_TRACE_ARM_GEOM"})
    _putenv_s(e, "");
  H::resetRecorderForTest(nullptr);
  W::resetForTest();
  T::setForTest(nullptr, 0);
  check(!T::enabled(), "X0 the census is off without SR_TEX_OWN_CENSUS");
  T::setForTest(path.c_str(), 0);
  check(T::enabled(), "X1 the census is on with a path");
  H::txcSetFifoForTest(0, 0);

  constexpr uint32_t kSpan = 0x10000;
  constexpr uint32_t kFmtX = 0x06, kDimsX = (256u << 16) | 256u;
  constexpr uint32_t kFmtY = 0x12, kDimsY = (128u << 16) | 128u;
  const uint32_t p = 0x0B600000u;    // STALE-OWNER (owner changes, bytes identical)
  const uint32_t pc = 0x0B620000u;   // STALE-CONTENT (same owner, selected alias changes)
  const uint32_t r = 0x0B680000u;    // ORDER: the change is observed after the draw's publish
  const uint32_t ra = 0x0B6A0000u;   // ORDER-AMBIGUOUS: the observation straddles the publish
  const uint32_t b = 0x0B640000u;    // BENIGN sequential reuse
  const uint32_t q = 0x0B700000u;    // ALIASED: heap-routed + GPU phys overlap
  const uint32_t qa = 0x0B720000u;   // two ADJACENT disjoint grants: coverage, not aliasing
  const uint32_t m = 0x0B740000u;    // RESIDENT-MISMATCH (one slot per base)
  const uint32_t mq = 0x0B7C0000u;   // aliased AND mismatched (both axes)
  const uint32_t n = 0x0B800000u;    // NO_BASELINE
  const uint32_t u = 0x0BA80000u;    // resident from an UNLOCK
  const uint32_t v3 = 0x0BAC0000u;   // two bands populated
  const uint32_t kk = 0x0BB40000u;   // descriptor-key exactness
  const uint32_t t3 = 0x0BB80000u;   // third-owner replacement
  const uint32_t q3 = 0x0BC80000u;   // Astra 070's schedule: observed, then E published, then appended
  const uint32_t up = 0x0BCC0000u;   // a DrawVerticesUP binding (in the draw denominator)
  const uint32_t bm = 0x0BD40000u;   // cube-dimension fetch, only a 2D resident: band mismatch
  const uint32_t st = 0x0BD80000u;   // stacked (depth > 1) fetch, only a 2D resident
  const uint32_t fb = 0x0BDC0000u;   // the fetch constant's base disagrees with the bound key

  // ── G: grant events carry the FIFO clock bracket ─────────────────────────
  H::txcSetFifoForTest(3, 3);
  g_fifo = 3;
  W::notePhysGrant(0xA0000000u | p, kSpan);
  const uint32_t s1 = lastRegSerial();
  {
    const T::Ev g = lastOf(T::kGrantReg);
    check(s1 != 0 && g.a == 2 && g.b == (0xA0000000u | p) && g.c == kSpan && g.z == 3 && g.w == 3,
          "G1 a GPU physical registration is a GrantReg (kind 2, va, size, serial, [P0,P1]=[3,3])");
  }

  // ── D: an ISSUED decode carries ITS upload's identity and payload ───────
  fillv(0x80000000u | p, 0x11, kSpan);
  {
    const T::BindToken tok = T::bindBegin();
    const std::vector<uint8_t> payload = snap(0x80000000u | p, kSpan);
    const uint64_t i = publishOne();
    T::onTexPublish(p, 0x100, kSpan, kDimsX, kFmtX, i, payload.data());
    // Another path publishes the SAME descriptor from another thread before this
    // bind's decode: it must not be taken for this bind's upload.
    std::thread other([&] {
      const uint64_t j = publishOne();
      T::onTexPublish(p, 0x900, kSpan, kDimsX, kFmtX, j, payload.data());
    });
    other.join();
    fillv(0x80000000u | p, 0x12, kSpan);   // the guest writes AFTER the snapshot
    T::onTexDecode(tok, p, kSpan, kFmtX, kDimsX, true, 8);
    T::onResident(p, T::kBand2D, T::kFromUpload, i);
    T::onTexApply(p, T::kApplied, T::kBand2D, kDimsX, kFmtX, i);
    T::onTexApply(p, T::kOther, T::kBand2D, kDimsX, kFmtX, i + 1);   // the other path's
    fillv(0x80000000u | p, 0x11, kSpan);
    const T::Ev d = lastOf(T::kTexDecode);
    check((d.e & 0xFF) == T::kIssued && ((d.e >> 8) & 0xFF) == 1 && d.f == s1 &&
              ((d.e >> 24) & 0xF) == 8 && d.x == T::probeHash(payload.data(), kSpan) &&
              d.y == i && d.z == i - 1 && d.w == i + 1,
          "D1 ISSUED: its OWN upload's FIFO index and payload hash (not a later re-read, not "
          "the other thread's publish), alias 0x8, [P0,P1] around it");
    const T::Ev res = lastOf(T::kResident);
    const auto ap = ofType(T::kTexApply);
    check(res.b == T::kFromUpload && res.c == T::kBand2D && res.f == 1 && res.z == i &&
              ap.size() == 2 && ap[0].f == 1 && ap[0].z == i && ap[0].c == T::kBand2D,
          "D2 the upload's slot write names the ordinal its apply witness then carries");
  }

  // ── S: same-descriptor reuse with a SUPPRESSED upload ─────────────────────
  W::forgetPhysGrant(0xA0000000u | p);
  {
    const T::Ev rel = lastOf(T::kGrantRel);
    check(rel.d == s1 && rel.e == 2 && rel.z <= rel.w, "G2 a reclaim is a GrantRel (serial s1, how 2)");
  }
  W::notePhysGrant(0xA0000000u | p, kSpan);                               // same bytes, new owner
  const uint32_t s2 = lastRegSerial();
  suppressed(p, kSpan, kFmtX, kDimsX);
  {
    const T::Ev d = lastOf(T::kTexDecode);
    check((d.e & 0xFF) == T::kSuppressedChanged && d.f == s2 && ((d.e >> 24) & 0xF) == 8 &&
              d.x == T::probeHash(snap(0x80000000u | p, kSpan).data(), kSpan),
          "S1 same descriptor, new owner, suppressed upload -> SUPPRESSED_CHANGED (owner s2), "
          "hashed in the upload's alias");
  }
  const size_t decodesS1 = ofType(T::kTexDecode).size();
  suppressed(p, kSpan, kFmtX, kDimsX);
  check(ofType(T::kTexDecode).size() == decodesS1, "S2 an unchanged suppressed bind records nothing");
  fillv(0xA0000000u | p, 0x5A, kSpan);                                    // the UNCHOSEN alias
  suppressed(p, kSpan, kFmtX, kDimsX);
  check(ofType(T::kTexDecode).size() == decodesS1,
        "S3 a change in the unchosen alias (0xA) records nothing: it is not the resident payload");

  W::notePhysGrant(0xA0000000u | pc, kSpan);
  fillv(0x80000000u | pc, 0x21, kSpan);
  uploadAndApply(pc, kSpan, kDimsX, kFmtX);
  fillv(0x80000000u | pc, 0x22, kSpan);
  suppressed(pc, kSpan, kFmtX, kDimsX);
  check((lastOf(T::kTexDecode).e & 0xFF) == T::kSuppressedChanged && lastOf(T::kTexDecode).a == pc,
        "S4 a change in the SELECTED alias on the same owner is SUPPRESSED_CHANGED");

  // Full owner SET, not two serials: replace a third owner.
  W::noteHeapRoutedGrant(0x80000000u | t3, kSpan);
  W::notePhysGrant(0xA0000000u | t3, kSpan);
  W::notePhysGrant(0xC0000000u | t3, kSpan);
  fillv(0x80000000u | t3, 0x31, kSpan);
  uploadAndApply(t3, kSpan, kDimsX, kFmtX);
  const T::Ev t3a = lastOf(T::kTexDecode);
  W::forgetPhysGrant(0xC0000000u | t3);
  W::notePhysGrant(0xC0000000u | t3, kSpan);
  suppressed(t3, kSpan, kFmtX, kDimsX);
  {
    const T::Ev t3b = lastOf(T::kTexDecode);
    check(((t3a.e >> 8) & 0xFF) == 3 && (t3b.e & 0xFF) == T::kSuppressedChanged && t3b.f == t3a.f &&
              t3b.g == t3a.g && ((t3b.e >> 8) & 0xFF) == 3 && t3b.v != t3a.v,
          "S5 replacing the THIRD owner (two smallest serials and count unchanged) is still a change");
  }

  // ── K: descriptor keys are exact (fmt 6 256x256 vs fmt 2 1280x256 collided) ─
  W::notePhysGrant(0xA0000000u | kk, 0x140000);
  fillv(0x80000000u | kk, 0x41, kSpan);
  uploadAndApply(kk, kSpan, kDimsX, 6u);
  suppressed(kk, kSpan, 2u, (1280u << 16) | 256u);
  check((lastOf(T::kTexDecode).e & 0xFF) == T::kNoBaseline,
        "K0 a different descriptor with a formerly-colliding key is NO_BASELINE, not a change");

  // ── O: aliasing is pairwise overlap, not coverage ────────────────────────
  W::noteHeapRoutedGrant(0x80000000u | q, kSpan);
  W::notePhysGrant(0xA0000000u | q, kSpan);
  fillv(0x80000000u | q, 0x51, kSpan);
  uploadAndApply(q, kSpan, kDimsX, kFmtX);
  {
    const T::Ev e = lastOf(T::kTexDecode);
    check(((e.e >> 8) & 0xFF) == 2 && ((e.e >> 16) & 0xFF) == ((1u << 1) | (1u << 2)) &&
              (e.e & T::kDecAliased),
          "O1 heap-routed + GPU phys over the same pages: two owners, ALIASED");
  }
  W::notePhysGrant(0xA0000000u | qa, kSpan / 2);
  W::notePhysGrant(0xA0000000u | (qa + kSpan / 2), kSpan / 2);
  fillv(0x80000000u | qa, 0x52, kSpan);
  uploadAndApply(qa, kSpan, kDimsX, kFmtX);
  {
    const T::Ev e = lastOf(T::kTexDecode);
    check(((e.e >> 8) & 0xFF) == 2 && !(e.e & T::kDecAliased),
          "O2 two ADJACENT disjoint grants covering the span: two owners, NOT aliased");
  }
  {
    const uint32_t o = 0x0B780000u;   // two GPU physical grants overlapping each other
    W::notePhysGrant(0xA0000000u | o, 0x20000);
    W::notePhysGrant(0xA0000000u | (o + 0x10000), 0x20000);
    const T::BindToken tok = T::bindBegin();
    T::onTexDecode(tok, o + 0x18000, 0x1000, kFmtX, kDimsX, false, 0);
    const T::Ev e = lastOf(T::kTexDecode);
    check(((e.e >> 8) & 0xFF) == 2 && (e.e & T::kDecAliased),
          "O3 overlapping grants within ONE registry are both found and ALIASED");
    W::forgetPhysGrant(0xA0000000u | o);
    W::forgetPhysGrant(0xA0000000u | (o + 0x10000));
  }
  {
    const uint32_t o = 0x0C000000u;   // 65 adjacent, disjoint 4 KB owners over one span
    for (uint32_t i = 0; i < 65; i++) W::notePhysGrant(0xA0000000u | (o + i * 0x1000u), 0x1000u);
    const T::BindToken tok = T::bindBegin();
    T::onTexDecode(tok, o, 65u * 0x1000u, kFmtX, kDimsX, false, 0);
    const T::Ev e = lastOf(T::kTexDecode);
    check(((e.e >> 8) & 0xFF) == 65 && !(e.e & T::kDecAliased),
          "O4 65 disjoint owners: exact, NOT aliased (no overflow state exists)");
    W::noteHeapRoutedGrant(0x80000000u | (o + 64u * 0x1000u), 0x1000u);   // one real overlap
    const T::BindToken tok2 = T::bindBegin();
    T::onTexDecode(tok2, o, 65u * 0x1000u, kFmtY, kDimsY, false, 0);
    const T::Ev e2 = lastOf(T::kTexDecode);
    check(((e2.e >> 8) & 0xFF) == 66 && (e2.e & T::kDecAliased),
          "O5 ... and one overlapping pair among 66 owners is ALIASED");
  }
  // aliased AND mismatched
  W::noteHeapRoutedGrant(0x80000000u | mq, kSpan);
  W::notePhysGrant(0xA0000000u | mq, kSpan);
  fillv(0x80000000u | mq, 0x53, kSpan);
  uploadAndApply(mq, kSpan, kDimsX, kFmtX);
  uploadAndApply(mq, kSpan, kDimsY, kFmtY);

  // ── the other scenarios' uploads ─────────────────────────────────────────
  W::notePhysGrant(0xA0000000u | b, kSpan);
  fillv(0x80000000u | b, 0x44, kSpan);
  uploadAndApply(b, kSpan, kDimsX, kFmtX);
  W::forgetPhysGrant(0xA0000000u | b);
  W::notePhysGrant(0xA0000000u | b, kSpan);
  fillv(0x80000000u | b, 0x55, kSpan);
  uploadAndApply(b, kSpan, kDimsY, kFmtY);
  {
    int issued = 0, changed = 0;
    for (const T::Ev& e : ofType(T::kTexDecode))
      if (e.a == b) ((e.e & 0xFF) == T::kIssued ? issued : changed)++;
    check(issued == 2 && changed == 0, "B1 benign reuse: two ISSUED decodes, no SUPPRESSED_CHANGED");
  }
  W::notePhysGrant(0xA0000000u | m, kSpan);
  fillv(0x80000000u | m, 0x77, kSpan);
  uploadAndApply(m, kSpan, kDimsX, kFmtX);
  uploadAndApply(m, kSpan, kDimsY, kFmtY);
  const uint32_t mY = lastOf(T::kTexApply).f;   // the slot's LAST writer
  W::notePhysGrant(0xA0000000u | r, kSpan);
  fillv(0x80000000u | r, 0x66, kSpan);
  uploadAndApply(r, kSpan, kDimsX, kFmtX);
  W::notePhysGrant(0xA0000000u | ra, kSpan);
  fillv(0x80000000u | ra, 0x68, kSpan);
  uploadAndApply(ra, kSpan, kDimsX, kFmtX);
  W::notePhysGrant(0xA0000000u | v3, kSpan);
  fillv(0x80000000u | v3, 0x69, kSpan);
  uploadAndApply(v3, kSpan, kDimsX, kFmtX);
  for (uint32_t k : {q3, up, bm, st, fb}) {
    W::notePhysGrant(0xA0000000u | k, kSpan);
    fillv(0x80000000u | k, uint8_t(k >> 16), kSpan);
    uploadAndApply(k, kSpan, kDimsX, kFmtX);
  }
  T::onResident(v3, T::kBand3D, T::kFromResolve, publishOne());           // a 3D resolve product
  T::onResident(u, T::kBand2D, T::kFromUnlock, publishOne());             // an unlock's slot write
  suppressed(n, 0x1000, kFmtX, kDimsX);
  check((lastOf(T::kTexDecode).e & 0xFF) == T::kNoBaseline,
        "N1 a suppressed bind of a descriptor the census never saw uploaded is NO_BASELINE");

  // ── Z: a registration racing a decode is ordered AFTER it ────────────────
  {
    static std::thread* s_racer = nullptr;
    static std::atomic<bool> s_regDone{false};
    T::setDecodeHookForTest([] {
      s_regDone = false;
      s_racer = new std::thread([] {
        W::notePhysGrant(0xA0000000u | 0x0BF00000u, 0x10000u);
        s_regDone = true;
      });
      for (int ms = 0; ms < 20 && !s_regDone; ms++)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    const T::BindToken tok = T::bindBegin();
    T::onTexDecode(tok, 0x0BF00000u, kSpan, kFmtX, kDimsX, false, 0);
    T::setDecodeHookForTest(nullptr);
    if (s_racer) { s_racer->join(); delete s_racer; s_racer = nullptr; }
    uint64_t decSeq = 0, regSeq = 0;
    uint32_t decLive = 99;
    for (const T::Ev& e : events()) {
      if (e.type == T::kTexDecode && e.a == 0x0BF00000u) { decSeq = e.seq; decLive = (e.e >> 8) & 0xFF; }
      if (e.type == T::kGrantReg && e.b == (0xA0000000u | 0x0BF00000u)) regSeq = e.seq;
    }
    check(decSeq && regSeq && decSeq < regSeq && decLive == 0,
          "Z1 a registration racing a decode lands AFTER it (owners and event are one atomic step)");
  }
  // An ISSUED bind whose owners changed between its start and its sample.
  {
    const uint32_t us = 0x0BF40000u;
    fillv(0x80000000u | us, 0x71, kSpan);
    const T::BindToken tok = T::bindBegin();
    const std::vector<uint8_t> payload = snap(0x80000000u | us, kSpan);
    const uint64_t i = publishOne();
    T::onTexPublish(us, 0x100, kSpan, kDimsX, kFmtX, i, payload.data());
    W::notePhysGrant(0xA0000000u | us, kSpan);            // mid-bind registry mutation
    T::onTexDecode(tok, us, kSpan, kFmtX, kDimsX, true, 8);
    T::onResident(us, T::kBand2D, T::kFromUpload, i);
    T::onTexApply(us, T::kApplied, T::kBand2D, kDimsX, kFmtX, i);
    check(lastOf(T::kTexDecode).e & T::kDecOwnersUnstable,
          "Z2 a registry mutation inside an ISSUED bind marks its owners UNSTABLE");
  }

  // ── H: the probes go through the interval cache with SR_HRR_CACHE OFF ───
  {
    K::hostRangeReadableSetCacheForTest(0);
    suppressed(p, kSpan, kFmtX, kDimsX);                          // may walk once
    const uint64_t vq0 = K::hostRangeReadableStats().vqCalls;
    fillv(0x80000000u | p, 0x13, kSpan);
    suppressed(p, kSpan, kFmtX, kDimsX);
    const uint64_t vq1 = K::hostRangeReadableStats().vqCalls;
    const bool plain = K::hostRangeReadable(K::guestVAToHost(0x80000000u | 0x0BE80000u), 64);
    const uint64_t vq2 = K::hostRangeReadableStats().vqCalls;
    check(vq1 == vq0 && plain && vq2 > vq1,
          "H1 census probes hit the interval cache while ordinary callers still VirtualQuery");
    K::hostRangeReadableSetCacheForTest(-1);
    fillv(0x80000000u | p, 0x11, kSpan);
    suppressed(p, kSpan, kFmtX, kDimsX);                          // back to the uploaded bytes
  }

  // ── E: one upload with a REAL payload through the REAL FIFO ──────────────
  {
    const uint64_t at = g_fifo;
    H::txcSetFifoForTest(at, at);
    std::vector<uint8_t> bytes(4096);
    for (size_t i = 0; i < bytes.size(); i++) bytes[i] = uint8_t(i * 7 + 3);
    const size_t applies = ofType(T::kTexApply).size();
    H::RenderCommand c{};
    c.type = H::RenderCommandType::TextureUpload;
    c.textureUpload.key = 0x0BA00000u;
    c.textureUpload.arenaOffset = H::texWrite(bytes.data(), (uint32_t)bytes.size());
    c.textureUpload.bytes = (uint32_t)bytes.size();
    c.textureUpload.dims = kDimsX;
    c.textureUpload.fmtBits = kFmtX;
    H::enqueue(c);
    for (int ms = 0; ms < 5000 && ofType(T::kTexApply).size() == applies; ms += 5)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const T::Ev pb = lastOf(T::kTexPublish);
    const auto ap = ofType(T::kTexApply);
    check(pb.a == 0x0BA00000u && pb.z == at + 1 && pb.x == T::probeHash(bytes.data(), 4096) &&
              ap.size() == applies + 1 && ap.back().a == 0x0BA00000u && ap.back().b == T::kOther &&
              ap.back().z == at + 1 && ap.back().f == pb.f && H::txcFifoPublished() == at + 1 &&
              H::txcFifoExecutedForTest() == at + 1,
          "E1 publish() hashes the command's REAL payload; the render thread's arm carries the same "
          "FIFO index and ordinal");
    g_fifo = at + 1;
    check(T::bindBegin().p0 == at + 1, "E2 a bind reads the real published count");
  }

  // ── R: a release through the REAL MmFreePhysicalMemory export ────────────
  K::registerStubs();
  auto& reg = K::ExportRegistry::instance();
  auto mmAlloc = reg.getExport("MmAllocatePhysicalMemoryEx");
  auto mmFree = reg.getExport("MmFreePhysicalMemory");
  if (mmAlloc && mmFree) {
    const uint32_t R = mmAlloc(0, 0x10000u, 4u, 0u, 0xFFFFFFFFu, 0u);
    const uint32_t sR = lastRegSerial();
    mmFree(0, R, 0, 0, 0, 0);
    const T::Ev rel = lastOf(T::kGrantRel);
    check(R && rel.d == sR && rel.e == 1 && rel.b == R,
          "R1 a guest free through the export is a GrantRel (released-retained) with txc alone on");
  } else {
    check(false, "R1 the Mm exports are registered");
  }
  check(W::markPhysGrantReleased(0x80000000u | q, false) && lastOf(T::kGrantRel).a == 1 &&
            lastOf(T::kGrantRel).b == (0x80000000u | q) && lastOf(T::kGrantRel).e == 1,
        "R2 a heap-routed grant's release is a GrantRel (kind 1, released-retained)");
  W::noteHeapRoutedGrant(0x80000000u | q, kSpan);   // q aliased again for the window draws

  // ── U / K: outside the window only flagged pages record, 64 per key ──────
  realDraw(0x0B900000u, kFmtX, kDimsX);
  check(drawsOf(0x0B900000u).empty(), "U1 an unflagged draw outside the capture window is not recorded");
  for (uint32_t i = 0; i < 70; i++) realDraw(p, kFmtX, kDimsX);   // p is flagged (S1)
  {
    const auto d = drawsOf(p);
    check(d.size() == 64 && !((d[0].e >> 11) & 1) && d[0].c == p && (d[0].e & 0xFF) == kFmtX &&
              d[0].f == kDimsX && ((d[0].e >> 9) & 3) == 1 && ((d[0].e >> 12) & 7) == 1 &&
              ((d[0].e >> 16) & 0xF) == T::kFromUpload && d[0].g == 1,
          "K1 real draw tap outside the window: a flagged key records its first 64; fetch "
          "fmt/dims/dim decoded from the real fetch file; 2D band holds apply 1");
  }

  // ── W: the REAL recorder window: every bound draw records ────────────────
  W::armRecorderCloseHook();
  H::resetTraceCloseCountForTest();
  H::operatorArmRequest();
  {
    H::RenderCommand present{};
    present.type = H::RenderCommandType::Present;
    execOne(present);
  }
  check(H::traceRecorderOpen(), "W0 the operator-armed recorder opened at a Present");
  const T::Ev openMark = lastOf(T::kMark);
  check(openMark.a == T::kMarkWindowOpen && openMark.z + 1 == g_fifo,
        "W1 the open edge is a Mark with the executor's FIFO position");
  realDraw(p, kFmtX, kDimsX);                    // STALE-OWNER
  realDraw(pc, kFmtX, kDimsX);                   // STALE-CONTENT
  realDraw(b, kFmtY, kDimsY);                    // BENIGN reuse
  realDraw(q, kFmtX, kDimsX);                    // ALIASED
  realDraw(qa, kFmtX, kDimsX);                   // coverage only
  realDraw(m, kFmtX, kDimsX);                    // MISMATCH (slot holds Y)
  realDraw(mq, kFmtX, kDimsX);                   // ALIASED + MISMATCH
  realDraw(n, kFmtX, kDimsX);                    // no resident
  realDraw(u, kFmtX, kDimsX);                    // unlock
  realDraw(v3, kFmtX, kDimsX);                   // two bands
  realDraw(kk, 6u, kDimsX);                      // exact descriptor: fresh
  realDraw(t3, kFmtX, kDimsX);                   // STALE-OWNER (third owner)
  realDraw(up, kFmtX, kDimsX, 1, 1, 0, H::RenderCommandType::DrawVerticesUP);   // UP draw
  realDraw(bm, kFmtX, kDimsX, 3);                // cube dimension, 2D resident
  realDraw(st, kFmtX, kDimsX, 1, 6);             // stacked, 2D resident
  realDraw(fb, kFmtX, kDimsX, 1, 1, 0x0BDE0000u);   // fetch base != bound key
  {
    const H::RenderCommand refused = drawCmd();   // no backend: refused at admission
    execOne(refused);
  }
  {
    const auto du = drawsOf(up);
    const auto db = drawsOf(bm);
    const auto ds = drawsOf(st);
    const auto dfb = drawsOf(fb);
    check(!du.empty() && !db.empty() && ((db.back().e >> 9) & 3) == 3 && !ds.empty() &&
              ((ds.back().e >> 15) & 1) && !((db.back().e >> 15) & 1) && !dfb.empty() &&
              dfb.back().d == 0x0BDE0000u && dfb.back().c == fb,
          "W3 an UP draw reaches the tap; the binding records the fetch's cube dimension, the "
          "stacked bit and the fetch base as decoded");
  }
  // ── Q3: Astra 070's schedule - the observation is taken, THEN draw E is
  // published and executed, THEN the decode appends. P1 is read at the append,
  // so it cannot claim the observation preceded E.
  bindFetch(q3, kFmtX, kDimsX, 1);
  fillv(0x80000000u | q3, 0x3C, kSpan);
  {
    static uint64_t s_q3E = 0;
    static uint32_t s_q3 = 0;
    s_q3 = q3;
    T::setDecodeHookForTest([] {
      T::setDecodeHookForTest(nullptr);
      s_q3E = ++g_fifo;
      executeDrawPublishedAt(s_q3E);
    });
    suppressed(q3, kSpan, kFmtX, kDimsX);
    T::setDecodeHookForTest(nullptr);
    const T::Ev d = lastOf(T::kTexDecode);
    const auto dq = drawsOf(q3);
    check(d.a == q3 && (d.e & 0xFF) == T::kSuppressedChanged && !dq.empty() &&
              dq.back().z == s_q3E && d.w >= s_q3E && d.seq > dq.back().seq,
          "Q3 an observation appended after draw E's publish carries P1 >= E (never P1 < E)");
  }
  // ── Q4: decode observations are SERIALIZED (g_obs). A second decode that
  // starts while the first is between its observation and its append appends
  // AFTER it: append order is observation order.
  {
    static std::thread s_q4;
    T::setDecodeHookForTest([] {
      T::setDecodeHookForTest(nullptr);
      s_q4 = std::thread([] {
        const T::BindToken tb = T::bindBegin();
        T::onTexDecode(tb, 0x0BE40000u, 0x10000u, 0x06u, (256u << 16) | 256u, false, 0);
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });
    {
      const T::BindToken ta = T::bindBegin();
      T::onTexDecode(ta, 0x0BE00000u, kSpan, kFmtX, kDimsX, false, 0);
    }
    T::setDecodeHookForTest(nullptr);
    if (s_q4.joinable()) s_q4.join();
    uint64_t seqA = 0, seqB = 0;
    for (const T::Ev& e : events())
      if (e.type == T::kTexDecode) {
        if (e.a == 0x0BE00000u) seqA = e.seq;
        if (e.a == 0x0BE40000u) seqB = e.seq;
      }
    check(seqA && seqB && seqA < seqB,
          "Q4 decodes are serialized: one started during another's observation appends after it");
  }
  // ORDER: r's change is observed AFTER its draw's publish.
  bindFetch(r, kFmtX, kDimsX, 1);
  const uint64_t eR = publishOne();
  fillv(0x80000000u | r, 0x67, kSpan);
  suppressed(r, kSpan, kFmtX, kDimsX);
  executeDrawPublishedAt(eR);
  // AMBIGUOUS: ra's observation starts before its draw's publish and ends after.
  bindFetch(ra, kFmtX, kDimsX, 1);
  {
    const T::BindToken tok = T::bindBegin();
    const uint64_t eRA = publishOne();
    fillv(0x80000000u | ra, 0x6B, kSpan);
    T::onTexDecode(tok, ra, kSpan, kFmtX, kDimsX, false, 0);
    executeDrawPublishedAt(eRA);
    const T::Ev d = lastOf(T::kTexDecode);
    check(d.a == ra && d.z < eRA && eRA <= d.w,
          "Q1 an observation that straddles the draw's publish carries P0 < E <= P1");
  }
  {
    const auto dr = drawsOf(r);
    check(!dr.empty() && ((dr.back().e >> 11) & 1) && dr.back().z == eR,
          "Q2 the draw published before the change executes later with E = its publish index");
  }
  {
    const auto dv = drawsOf(v3);
    const auto du = drawsOf(u);
    const auto dm = drawsOf(m);
    check(!dv.empty() && ((dv.back().e >> 12) & 7) == 3 &&
              ((dv.back().e >> 20) & 0xF) == T::kFromResolve && !du.empty() &&
              ((du.back().e >> 16) & 0xF) == T::kFromUnlock && du.back().g == 0 && !dm.empty() &&
              dm.back().g == mY && ((dm.back().e >> 11) & 1),
          "W2 the draws record every band's resident (2D+3D both populated; unlock source; "
          "the slot's LAST writer)");
  }
  uint64_t wc[4] = {};
  H::txcWindowCountersForTest(wc);
  // Close the window: the next Presents reach SR_HLE_TRACE_FRAMES.
  for (int i = 0; i < 6 && H::traceRecorderCloseCount() == 0; i++) {
    H::RenderCommand present{};
    present.type = H::RenderCommandType::Present;
    execOne(present);
  }
  const T::Stats post = T::statsForTest();
  check(post.flushed && !H::traceRecorderOpen(),
        "F1 the REAL recorder's close edge and close hook wrote the census");
  {
    const auto v = events();
    T::Ev closeMark{}, cost{};
    int opens = 0, closes = 0;
    for (const T::Ev& e : v) {
      if (e.type != T::kMark) continue;
      if (e.a == T::kMarkWindowOpen) opens++;
      if (e.a == T::kMarkWindowClose) { closes++; closeMark = e; }
      if (e.a == T::kMarkCost) cost = e;
    }
    size_t inWin = 0;
    for (const T::Ev& e : v) if (e.type == T::kDrawTex && ((e.e >> 11) & 1)) inWin++;
    T::Ev openEdge{};
    for (const T::Ev& e : v)
      if (e.type == T::kMark && e.a == T::kMarkWindowOpen) openEdge = e;
    const uint64_t recOpen = openEdge.b | ((uint64_t)openEdge.c << 32);
    const uint64_t recClose = closeMark.b | ((uint64_t)closeMark.c << 32);
    check(opens == 1 && closes == 1 && closeMark.x == wc[0] && closeMark.y == wc[1] &&
              closeMark.w == wc[2] && closeMark.v == 0 && wc[0] == 20 && wc[1] == 19 &&
              wc[2] == 19 && inWin == wc[2] - closeMark.v,
          "F2 one open and one close edge; the close carries the renderer's window counters (20 "
          "draw commands incl. UP and one refused at admission, 19 at the tap, 19 bindings) and "
          "the recorded in-window bindings conserve");
    check(recOpen > 0 && recClose - recOpen == closeMark.z - openEdge.z &&
              H::recorderRecordsForTest() == recClose,
          "F2b the edges carry the recorder's record counts: live records == FIFO commands in the "
          "window, the join the analyzer uses");
    check(!v.empty() && v.back().type == T::kMark && v.back().a == T::kMarkCost && cost.w > 0 &&
              cost.v > 0 && cost.b > 0 && cost.y <= cost.x,
          "F3 the last event is the census's own tap cost (Mark 1)");
    std::FILE* f = std::fopen(path.c_str(), "rb");
    uint32_t hdr[4] = {};
    uint64_t cnt[3] = {};
    bool ok = f && std::fread(hdr, 4, 4, f) == 4 && std::fread(cnt, 8, 3, f) == 3;
    long size = 0;
    if (f) { std::fseek(f, 0, SEEK_END); size = std::ftell(f); std::fclose(f); }
    const long want = 16 + 24 + 8 * (long)T::kTypes + (long)sizeof(T::Ev) * (long)post.events;
    check(ok && hdr[0] == 0x31435854u && hdr[1] == 2 && hdr[2] == 80 && cnt[0] == post.events &&
              cnt[1] == 0 && cnt[2] == post.lastSeq && size == want,
          "F4 header (magic, version 2, 80-byte records, count, overflow, last seq) and size agree");
  }
  W::notePhysGrant(0xA0000000u | 0x0BB00000u, kSpan);
  {
    const T::Stats after = T::statsForTest();
    check(after.events == post.events && after.lastSeq == post.lastSeq + 1 && !T::flush("again"),
          "F5 after the close nothing more is recorded (a seq is still consumed); flush is once");
  }
  H::setTraceCloseHook(nullptr);
  H::resetTraceCloseCountForTest();
  {
    W::resetForTest();
    T::setForTest("Z:/no/such/dir/txc.bin", 0);
    W::notePhysGrant(0xA0000000u | 0x0BD00000u, kSpan);
    const bool wrote = T::flush("bad-path");
    W::notePhysGrant(0xA0000000u | 0x0BD10000u, kSpan);
    const T::Stats s = T::statsForTest();
    check(!wrote && s.flushed && s.events == 2 && s.lastSeq == 3 && !T::flush("again"),
          "F6 a FAILED write still ends the acquisition (reported false, nothing more recorded)");
  }

  // ── IF: observations in flight when the census is written ───────────────
  {
    const std::string p3 = dir + "\\sr_txc_test3.bin";
    W::resetForTest();
    T::setForTest(p3.c_str(), 0);
    {
      const T::BindToken held = T::bindBegin();   // never ends before the flush
      const auto t0 = std::chrono::steady_clock::now();
      T::flush("inflight");
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
      const T::Ev m = lastOf(T::kMark);
      check(m.a == T::kMarkCost && m.c == 1 && m.e == 1 && ms >= 240,
            "IF1 an observation still in flight is waited for (bounded) and then RECORDED as in "
            "flight - never silently lost");
    }
    T::setForTest(p3.c_str(), 0);
    std::thread ender;
    {
      T::BindToken held = T::bindBegin();
      ender = std::thread([t = std::move(held)]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        T::BindToken done = std::move(t);   // ends here
      });
    }
    T::flush("inflight-ends");
    ender.join();
    {
      const T::Ev m = lastOf(T::kMark);
      check(m.a == T::kMarkCost && m.c == 0 && m.e == 0 && m.d >= 20,
            "IF2 ... and one that ends during the wait is let finish: nothing in flight");
    }
    T::setForTest(nullptr, 0);
    std::remove(p3.c_str());
  }

  // ── V: overflow is counted, and a dropped event still consumes a seq ─────
  W::resetForTest();
  T::setForTest(path2.c_str(), 3);
  for (uint32_t i = 0; i < 5; i++) W::notePhysGrant(0xA0000000u | (0x0BC00000u + i * kSpan), kSpan);
  {
    const T::Stats s = T::statsForTest();
    check(s.events == 3 && s.overflow == 2 && s.lastSeq == 5, "V1 past the cap: 3 kept, 2 overflowed, seq 5");
  }
  T::setForTest(nullptr, 0);
  W::resetForTest();
  std::remove(path2.c_str());
  std::printf("[TXC_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
