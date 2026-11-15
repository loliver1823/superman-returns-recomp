// Included by kernel/native_flush.cpp only in difftest (SR_DIFFTEST), after setconst_render_test.inc (it reuses that
// suite's transport fixture, namespace scr): p2.zpd's bake - the producer port of GpuEmitTileSurfaceConfig_82101E08
// (Driver::zpdSurface; EVENT_WRITE_ZPD as the kRkZpd record) against the SDK body.
//
// 82101E08(dev, query, which) through production dispatch on identical fixtures, the per-present A/B
// (SR_AB_TOGGLE=SR_NATIVE_ZPD, SR_AB_PRESENT=1) choosing the arm: OFF = the actual SDK body - the oracle, the XDK's bytes
// in the command buffer - and ON = the native Entry. SR_NATIVE_ZPD itself is unset: the gate's baked default (ON, the
// latency model) is what runs. Cases: one tile (query+144 = 1, and 0: the same path) and two / three tiles, BEGIN
// (which 1: the slot + 0x20) and END (which 0), a slot in the 0xC0 view and one in the 0xE0 view (the XDK's VA ->
// physical fold adds 0x1000 there), the bin masks dev+12432 / +12436 the multi-tile tail re-emits. Every SDK arm is
// first held to an oracle written from the asm (IDA 82101E08, 2026-10-16) with LITERAL expected words - not the port's
// formulas. Modes (one process each; the gates latch at static initialisation):
//   records - segmentless record mode: the native arm's record rendered back (cpRecordPm4) equals the SDK's bytes and
//             holds one kRkZpd {21}; the CP applies the record through the record engine's own consumer
//             (cpApplyRecord -> RecordEngine::nativeRecord, the release path) with the same register effects as the
//             SDK's PM4 executed by a Pm4Engine (RB_MODECONTROL, RB_SURFACE_INFO / RB_COLOR_INFO, RB_SAMPLE_COUNT_ADDR,
//             VGT_EVENT_INITIATOR, the bin masks), one EVENT_WRITE_ZPD counted (cpZpdEvents, opcode 0x5B).
//   cppm4   - SR_NATIVE_FLUSH_CPPM4=1: the same, the CP executing the rendered PM4 (the split's own path).
//   pm4     - SR_NATIVE_FLUSH_PM4=1: the producer-side split writes the span literal: the command buffer equals the
//             SDK's bytes.
// In every mode: the native arm took the call (zpdPortCalls), r3 and dev+0x28 equal the SDK's, and the dirty masks
// dev+0..+39 equal the SDK's (dev+16 |= 8, dev+24 |= 1 << 38 | 1 << 37, dev+32 |= 0x2000). Those six cases keep the
// threshold far: no kick.
// KICKS (Astra review round 1, P2-3): the kick cases set dev+0x30 so 82101E08's own kick checks fire - the ENTRY check
// (cursor > dev+0x30 before anything is written) and the PER-TILE check (dev+0x28 = cursor after each tile's LO +
// SET_CONSTANT, cursor > dev+0x30 -> r3 = GpuCommandBufferFlush 820FD8C0's cursor) - through production dispatch in
// both arms: the close is the native segmentless close in each, its new segment the XDK's own 820FCF90 / 820FCBE8 (the
// generation ring: dev+0x3354 = 0, the progress word at generation 1, so nothing waits). Cases: the entry kick (one
// tile; three tiles in the 0xE0 view), the per-tile kick after tile 0 (two and three tiles), after the LAST tile (two
// tiles: the new segment holds only the tail), after tile 1 with the cursor after tile 0 EQUAL to the threshold (ble:
// no kick there), and every check at once (dev+13492 = 0: the XDK's 54-dword minimum segment, whose threshold is its
// own cursor - the entry kick and a kick after every tile, five segments). After the call the fixture closes the open
// segment; the publication observer (setSegmentPublicationObserver) collects every segment the closes published.
// Compared: the published segments (count, phys, dwords, the PM4 of every part - the guest bytes, the records rendered
// back by cpRecordPm4 - with the SDK arm held to the asm's words split at each kick, each segment closed by the close's
// fence tail), r3 (the cursor after the entry kick + 16, or the cursor the last in-loop kick returned), dev+0x28 (the
// last dword in the new segment), the dirty masks, the post-kick tail (LO {dev+12432} / HI {dev+12436} /
// EVENT_WRITE_ZPD {21} in the new segment before the fence: one kRkZpd {21} record packet in the records modes, literal
// PM4 under SR_NATIVE_FLUSH_PM4, none before the kick) and the CP's effects of every segment applied in order
// (cpSegment: the mode's own consumer). Mutants (zpd_port_run.py): the per-tile kick deleted, the cursor an in-loop kick
// returned dropped or off by 4, the entry kick deleted.
// Run with SR_PM4_RING=0 SR_PM4_RING_EXEC=0 SR_PM4_CP=0 (the runner's configuration; a bare run re-runs itself with
// it, tools/difftest/suite_bare.inc). Runner + mutants: tools/difftest/zpd_port_run.py.

namespace sr::native::flush {
namespace zpt {
namespace K = sr::kernel;
constexpr uint32_t kQ = 0x70807000u;  // the 152-byte query object (q+24.. the tiles' slot VAs, q+144 the tile count)

int passed = 0, failed = 0;
void check(bool ok, const std::string& name) {
  std::printf("%s%s\n", ok ? "  ok  " : "FAIL: ", name.c_str());
  ok ? ++passed : ++failed;
}

struct Case {
  const char* name;
  uint32_t tiles, which;
  uint32_t slotVA[3];  // q+24+4i
  uint32_t phys[3];    // the expected RB_SAMPLE_COUNT_ADDR per tile (slot + 32*which, folded) - literal, from the asm
  uint32_t m12432, m12436;
};
struct Arm {
  uint32_t c0 = 0, r3 = 0, cursor = 0;
  std::vector<uint8_t> bytes;    // the command buffer from c0 + 4 to the cursor
  std::vector<uint8_t> dirty;    // dev+0..+39
  uint32_t m12432 = 0, m12436 = 0;
  std::vector<uint32_t> record;  // the native arm's record words after its header (record modes)
  uint64_t pos = 0;
  bool recordFound = false;
};

std::vector<uint8_t> beWords(const std::vector<uint32_t>& w) {
  std::vector<uint8_t> b(4u * w.size());
  for (size_t i = 0; i < w.size(); i++) put32(b.data() + 4u * i, w[i]);
  return b;
}

// The asm's words for the case (IDA 82101E08): the type-0 runs, the slot packets, EVENT_WRITE_ZPD {21}.
std::vector<uint32_t> oracle(const Case& c) {
  std::vector<uint32_t> w = {0x00002208u, 6u, 0x00012000u, 0x00020000u, 0u};
  if (c.tiles <= 1u) {
    w.insert(w.end(), {0x00002325u, c.phys[0]});
  } else {
    w.insert(w.end(), {0xC0006100u, 0u});
    for (uint32_t i = 0; i < c.tiles; i++) w.insert(w.end(), {0xC0006000u, 3u << (2u * i), 0xC0012D01u, 0x00040325u, c.phys[i]});
    w.insert(w.end(), {0xC0006000u, c.m12432, 0xC0006100u, c.m12436});
  }
  w.insert(w.end(), {0xC0005B00u, 21u});
  return w;
}

Arm runArm(const Case& c, bool native, int abBit) {
  for (int i = 0; i < 2 && sr::abt::on(abBit) != native; i++) sr::abt::onGuestSwap();
  scr::prepare(0u, 0u);
  std::memset(scr::host(kQ), 0, 0x100u);
  scr::wr(kQ + 144u, c.tiles);
  for (uint32_t i = 0; i < 3u; i++) scr::wr(kQ + 24u + 4u * i, c.slotVA[i]);
  scr::wr(scr::kDev + 12432u, c.m12432);
  scr::wr(scr::kDev + 12436u, c.m12436);
  Arm a;
  a.c0 = scr::rd(scr::kDev + 0x28u);
  a.pos = g_head;
  PPCContext x = scr::context();
  x.r3.u64 = scr::kDev;
  x.r4.u64 = kQ;
  x.r5.u64 = c.which;
  PPC_LOOKUP_FUNC(K::getGuestBase(), 0x82101E08u)(x, K::getGuestBase());
  a.r3 = x.r3.u32;
  a.cursor = scr::rd(scr::kDev + 0x28u);
  if (a.cursor > a.c0 && a.cursor - a.c0 < 0x1000u) a.bytes.assign(scr::host(a.c0 + 4u), scr::host(a.c0 + 4u) + (a.cursor - a.c0));
  a.dirty.assign(scr::host(scr::kDev), scr::host(scr::kDev) + 40u);
  a.m12432 = scr::rd(scr::kDev + 12432u);
  a.m12436 = scr::rd(scr::kDev + 12436u);
  if (native && !g_pm4Out && g_arena) {
    RecHdr h;
    std::memcpy(&h, g_arena + (a.pos % kRecArena), sizeof(h));
    if (h.magic == kRecMagic && h.pos == a.pos && h.bytes >= sizeof(RecHdr) && h.bytes <= kMaxDrawRec) {
      a.recordFound = true;
      a.record.resize((h.bytes - sizeof(RecHdr)) / 4u);
      std::memcpy(a.record.data(), g_arena + (a.pos % kRecArena) + sizeof(RecHdr), 4u * a.record.size());
    }
  }
  return a;
}

// The register effects a CP application leaves (the shared register file, so each engine runs from a known state).
struct Effects {
  uint32_t r2208 = 0, r2000 = 0, r2001 = 0, r2325 = 0, r21F9 = 0;
  uint64_t binMask = 0, binSelect = 0, zpdEvents = 0, op5B = 0;
  bool operator==(const Effects& o) const {
    return r2208 == o.r2208 && r2000 == o.r2000 && r2001 == o.r2001 && r2325 == o.r2325 && r21F9 == o.r21F9 &&
           binMask == o.binMask && binSelect == o.binSelect && zpdEvents == o.zpdEvents && op5B == o.op5B;
  }
};
void clearRegs() {
  auto& regs = sr::gfx::getXenosRegisters();
  for (uint32_t r : {0x2208u, 0x2000u, 0x2001u, 0x2325u, 0x21F9u}) regs.write(r, 0xDEADBEEFu);
}
Effects effects(const sr::gfx::Pm4Engine& eng) {
  auto& regs = sr::gfx::getXenosRegisters();
  return Effects{regs.read(0x2208u), regs.read(0x2000u), regs.read(0x2001u), regs.read(0x2325u), regs.read(0x21F9u),
                 eng.binMask, eng.binSelect, eng.cpZpdEvents, eng.opcodeHist[0x5B]};
}

void runCase(const Case& c, int abBit, bool cppm4, bool pm4) {
  const std::string n = c.name;
  const Arm sdk = runArm(c, false, abBit);
  const std::vector<uint8_t> want = beWords(oracle(c));
  uint8_t d16[8], d24[8], d32[8];
  std::memcpy(d16, sdk.dirty.data() + 16, 8);
  std::memcpy(d24, sdk.dirty.data() + 24, 8);
  std::memcpy(d32, sdk.dirty.data() + 32, 8);
  check(sdk.bytes == want && sdk.r3 == sdk.c0 + 16u && sdk.cursor == sdk.c0 + uint32_t(want.size()),
        n + ": SDK arm: the asm's words (the type-0 runs, the slot packets, EVENT_WRITE_ZPD {21}); r3 = the cursor + 16, "
            "dev+0x28 its last dword");
  check(be64(d16) == 0x8ull && be64(d24) == ((1ull << 38) | (1ull << 37)) && be64(d32) == 0x2000ull,
        n + ": SDK arm: the dirty masks dev+16 |= 8, dev+24 |= 1 << 38 | 1 << 37, dev+32 |= 0x2000");
  std::printf("[ZPD_PORT] %s sdk=%s\n", c.name, scr::hex(sdk.bytes).c_str());

  const uint64_t calls0 = g_zpdPortCalls.get();
  const Arm nat = runArm(c, true, abBit);
  check(g_zpdPortCalls.get() == calls0 + 1u, n + ": the native arm took the call (an Entry, not the guest body)");
  check(nat.c0 == sdk.c0 && nat.r3 == sdk.r3 && nat.cursor == sdk.cursor, n + ": r3 and dev+0x28 equal the SDK's");
  check(nat.dirty == sdk.dirty && nat.m12432 == sdk.m12432 && nat.m12436 == sdk.m12436,
        n + ": the dirty masks dev+0..+39 equal the SDK's (and the bin masks dev+12432 / +12436 unchanged)");
  if (pm4) {
    check(nat.bytes == sdk.bytes, n + ": the producer's literal span (SR_NATIVE_FLUSH_PM4) equals the SDK's bytes");
    std::printf("[ZPD_PORT] %s producer=%s\n", c.name, scr::hex(nat.bytes).c_str());
    return;
  }
  // one kRkZpd {21}, the record's last packet
  uint32_t zpdTags = 0;
  for (size_t i = 0; i < nat.record.size(); i++)
    if (nat.record[i] == recTag(kRkZpd, 0u, 0u)) zpdTags++;
  check(nat.recordFound && zpdTags == 1u && nat.record.size() >= 2u &&
            nat.record[nat.record.size() - 2u] == recTag(kRkZpd, 0u, 0u) && nat.record.back() == 21u,
        n + ": the record ends with one kRkZpd {21} (EVENT_WRITE_ZPD as a packet record, not literal PM4)");
  if (!nat.recordFound) return;
  std::vector<uint8_t> cp;
  sr::gfx::Pm4Engine::NativeRecordStats st;
  bool opaque = false;
  uint32_t rendered = 0;
  cpRecordPm4(nat.record.data(), nat.record.data() + nat.record.size(), cp, st, opaque, rendered);
  check(!st.bad && !opaque && cp == sdk.bytes, n + ": the record rendered back (cpRecordPm4) equals the SDK's bytes");
  std::printf("[ZPD_PORT] %s cp=%s\n", c.name, scr::hex(cp).c_str());
  // The reference: the SDK's PM4 executed by a Pm4Engine; then the record applied as the consumer does.
  clearRegs();
  sr::gfx::Pm4Engine ref;
  ref.cpMode = true;
  ref.submitDraws = false;
  ref.execute(sdk.bytes.data(), sdk.bytes.size());
  const Effects want2 = effects(ref);
  clearRegs();
  sr::gfx::Pm4Engine eng;
  eng.cpMode = true;
  eng.submitDraws = false;
  const bool applied = cpApplyRecord(eng, nat.pos);
  const Effects got = effects(eng);
  check(applied && !eng.nativeRecordFailed && got == want2 && got.zpdEvents == 1u && got.op5B == 1u &&
            got.r21F9 == 21u && got.r2325 == c.phys[c.tiles > 1u ? c.tiles - 1u : 0u],
        n + std::string(": the CP applies the record") + (cppm4 ? " (the CPPM4 split)" : " (RecordEngine::nativeRecord)") +
            ": the registers, the bin masks and one EVENT_WRITE_ZPD equal the SDK's PM4 executed");
}

// ---- kicks inside 82101E08 (Astra review round 1, P2-3; the header's KICKS) ----
// The segments the closes publish, in order (the publication observer: the hand-off the ringless override takes).
struct Pub {
  uint64_t id;
  uint32_t phys, dwords;
};
std::vector<Pub> g_pub;
void observe(uint64_t id, uint32_t phys, uint32_t dwords, bool) { g_pub.push_back(Pub{id, phys, dwords}); }
uint32_t physOf(uint32_t va) { return (((va >> 20) + 0x200u) & 0x1000u) + (va & 0x1FFFFFFFu); }

struct KickCase {
  Case c;
  uint32_t gap;      // guest type-2 NOPs already in the open segment (dwords): the cursor c0 = kSeg - 4 + 4 * gap
  int32_t thr;       // dev+0x30 - c0 (< 0: the entry check kicks); the limit dev+0x2C = dev+0x30 + 160 (the SDK's gap)
  uint32_t segSize;  // dev+13492, every new segment's size (0: the XDK's 54-dword minimum - its threshold is its cursor)
  std::vector<uint32_t> cuts;  // the oracle's word index at each kick, in order (0 = the entry check; 7 + 5 (t + 1) = after tile t)
};
// One published segment: its PM4 - the gap parts as the guest bytes, the record parts rendered back (cpRecordPm4).
struct Seg {
  uint32_t phys = 0, dwords = 0;
  std::vector<uint8_t> pm4;
  uint32_t zpdRecs = 0;  // kRkZpd packets in its record parts
  bool zpdLast = false;  // a record part whose last packet is kRkZpd {21}
};
struct KArm {
  uint32_t c0 = 0, thr = 0, r3 = 0, cursor = 0, segStart = 0;  // segStart: dev+0x34C4 (the open segment) after the call
  std::vector<uint8_t> dirty;                                  // dev+0..+39
  uint32_t m12432 = 0, m12436 = 0;
  std::vector<Seg> segs;
  bool bad = false, applied = true;
  Effects fx;  // the CP's effects of every published segment applied in order
};

std::vector<uint32_t> beRead(const std::vector<uint8_t>& b) {
  std::vector<uint32_t> w(b.size() / 4u);
  for (size_t i = 0; i < w.size(); i++) w[i] = be32(b.data() + 4u * i);
  return w;
}
// A record's packets: the kRkZpd ones, and whether the last one is kRkZpd {21}.
void scanRecord(const uint32_t* w, const uint32_t* e, Seg& s, bool& bad) {
  bool last = false;
  while (w < e) {
    const uint32_t tag = *w;
    uint32_t words;
    if (tag & 0x80000000u) {
      const uint32_t kind = (tag >> 24) & 0x7Fu;
      words = recWords(kind, (tag >> 8) & 0xFFFFu);
      if (kind == kRkZpd) s.zpdRecs++;
      last = tag == recTag(kRkZpd, 0u, 0u) && e - w >= 2 && w[1] == 21u;
    } else {
      words = (tag >> 16) & 0x3FFFu;
      last = false;
    }
    if (size_t(e - w) < 1u + words) {
      bad = true;
      return;
    }
    w += 1 + words;
  }
  if (last) s.zpdLast = true;
}
Seg renderSeg(const SegParts& sp, bool& bad) {
  Seg s;
  s.phys = sp.phys;
  s.dwords = sp.dwords;
  for (const SegPart& part : sp.parts) {
    if (part.kind == 1u && part.off <= sp.bytes.size() && part.len <= sp.bytes.size() - part.off) {
      s.pm4.insert(s.pm4.end(), sp.bytes.begin() + part.off, sp.bytes.begin() + part.off + part.len);
      continue;
    }
    if (part.kind != 0u || !part.record) {
      bad = true;
      continue;
    }
    RecHdr h;
    std::memcpy(&h, part.record->data(), sizeof(h));
    if (h.magic != kRecMagic || h.bytes < sizeof(RecHdr) || h.bytes > 4u * part.record->size()) {
      bad = true;
      continue;
    }
    const uint32_t* w = reinterpret_cast<const uint32_t*>(part.record->data() + sizeof(RecHdr));
    const uint32_t* e = reinterpret_cast<const uint32_t*>(part.record->data() + h.bytes);
    scanRecord(w, e, s, bad);
    const size_t at = s.pm4.size();
    sr::gfx::Pm4Engine::NativeRecordStats st;
    bool opaque = false;
    uint32_t setc = 0;
    cpRecordPm4(w, e, s.pm4, st, opaque, setc);
    if (st.bad || opaque || s.pm4.size() - at != part.len) bad = true;  // a record's PM4 is exactly its span
  }
  if (s.pm4.size() != 4u * size_t(sp.dwords)) bad = true;
  return s;
}
std::vector<uint8_t> concat(const std::vector<Seg>& segs) {
  std::vector<uint8_t> b;
  for (const Seg& s : segs) b.insert(b.end(), s.pm4.begin(), s.pm4.end());
  return b;
}

KArm runKickArm(const KickCase& k, bool native, int abBit) {
  namespace K = sr::kernel;
  const Case& c = k.c;
  for (int i = 0; i < 2 && sr::abt::on(abBit) != native; i++) sr::abt::onGuestSwap();
  scr::prepare(k.gap, 0u);
  // the new segment through the XDK's generation ring (820FCF90 -> 820FCBE8 mode 2: dev+0x3354 = 0, no carve, the
  // progress word fb+4 at generation dev+13500 = 1 so 820FCB30 never waits; no swap-fence throttle, no early close)
  scr::wr(scr::kDev + 13492u, k.segSize);
  scr::wr(scr::kDev + 13496u, 0u);
  scr::wr(scr::kDev + 13504u, scr::kBuf);
  scr::wr(scr::kDev + 13516u, 0x7FFFFFFFu);
  scr::wr(scr::kDev + 20416u, 1u);
  scr::wr(scr::kDev + 15152u, scr::kBuf + 0x60000u);
  scr::wr(scr::kFb + 4u, 1u);
  std::memset(scr::host(kQ), 0, 0x100u);
  scr::wr(kQ + 144u, c.tiles);
  for (uint32_t i = 0; i < 3u; i++) scr::wr(kQ + 24u + 4u * i, c.slotVA[i]);
  scr::wr(scr::kDev + 12432u, c.m12432);
  scr::wr(scr::kDev + 12436u, c.m12436);
  KArm a;
  a.c0 = scr::rd(scr::kDev + 0x28u);
  a.thr = a.c0 + uint32_t(k.thr);
  scr::wr(scr::kDev + 0x30u, a.thr);
  scr::wr(scr::kDev + 0x2Cu, a.thr + 160u);
  sr::native::cdedup::lostStream();
  g_pub.clear();
  PPCContext x = scr::context();
  x.r3.u64 = scr::kDev;
  x.r4.u64 = kQ;
  x.r5.u64 = c.which;
  PPC_LOOKUP_FUNC(K::getGuestBase(), 0x82101E08u)(x, K::getGuestBase());
  a.r3 = x.r3.u32;
  a.cursor = scr::rd(scr::kDev + 0x28u);
  a.segStart = scr::rd(scr::kDev + 0x34C4u);
  a.dirty.assign(scr::host(scr::kDev), scr::host(scr::kDev) + 40u);
  a.m12432 = scr::rd(scr::kDev + 12432u);
  a.m12436 = scr::rd(scr::kDev + 12436u);
  {  // the open segment's own close (its tail: the post-kick packets)
    PPCContext y = scr::context();
    y.r3.u64 = scr::kDev;
    PPC_LOOKUP_FUNC(K::getGuestBase(), 0x820FD8C0u)(y, K::getGuestBase());
  }
  // every published segment rendered (before the consumer: a completed entry leaves the registry)
  std::vector<std::shared_ptr<SegParts>> sps;
  {
    std::lock_guard<std::mutex> lk(g_segMu);
    for (const Pub& p : g_pub) {
      auto it = g_segById.find(p.id);
      if (it == g_segById.end()) a.bad = true;
      else sps.push_back(it->second);
    }
  }
  for (const auto& sp : sps) a.segs.push_back(renderSeg(*sp, a.bad));
  // the consumer: every published segment in order (records through cpApplyRecord - rendered back under CPPM4). CP
  // mode, as the CP runs a segment: EVENT_WRITE_ZPD (0x5B) and the fence's EVENT_WRITE_SHD are its cpPacket's (the
  // fence's stores land in the fence block kFb through physToHost).
  clearRegs();
  auto eng = std::make_unique<sr::gfx::Pm4Engine>();
  eng->submitDraws = false;
  eng->cpMode = true;
  eng->physToHost = [](void*, uint32_t p) -> uint8_t* { return p < 0x20000000u ? scr::host(0x80000000u | p) : nullptr; };
  for (const Pub& p : g_pub)
    if (!cpSegment(*eng, p.id, p.phys, p.dwords)) a.applied = false;
  a.applied = a.applied && !eng->nativeRecordFailed;
  a.fx = effects(*eng);
  std::printf("[ZPD_KICK] %s %s c0=%08X thr=%08X r3=%08X cursor=%08X seg=%08X bad=%d applied=%d pubs=%zu:", c.name,
              native ? "native" : "sdk", a.c0, a.thr, a.r3, a.cursor, a.segStart, int(a.bad), int(a.applied), g_pub.size());
  for (const Seg& s : a.segs) std::printf(" %08X/%u(zpd %u%s)", s.phys, s.dwords, s.zpdRecs, s.zpdLast ? " last" : "");
  std::printf(" | cp 2208=%X 2000=%X 2001=%X 2325=%08X 21F9=%X mask=%016llX select=%016llX zpd=%llu op5B=%llu\n",
              a.fx.r2208, a.fx.r2000, a.fx.r2001, a.fx.r2325, a.fx.r21F9, (unsigned long long)a.fx.binMask,
              (unsigned long long)a.fx.binSelect, (unsigned long long)a.fx.zpdEvents, (unsigned long long)a.fx.op5B);
  return a;
}

void runKickCase(const KickCase& k, int abBit, bool cppm4, bool pm4) {
  const Case& c = k.c;
  const std::string n = c.name;
  const std::vector<uint32_t> w = oracle(c);
  // The expected segments: the guest's NOPs, then the asm's words split at each kick (each segment then closed by the
  // close's 10-dword fence tail).
  std::vector<std::vector<uint32_t>> want;
  {
    std::vector<uint32_t> s(k.gap, 0x80000000u);
    uint32_t at = 0;
    for (uint32_t cut : k.cuts) {
      s.insert(s.end(), w.begin() + at, w.begin() + cut);
      want.push_back(s);
      s.clear();
      at = cut;
    }
    s.insert(s.end(), w.begin() + at, w.end());
    want.push_back(s);
  }
  const bool inLoop = !k.cuts.empty() && k.cuts.back() > 0u;  // the last kick is a per-tile one: r3 = its cursor
  std::string where;
  for (uint32_t cut : k.cuts)
    where += (where.empty() ? "" : ", ") + (cut ? "after tile " + std::to_string((cut - 7u) / 5u - 1u) : std::string("entry"));
  auto fenceAt = [](const std::vector<uint32_t>& g, size_t at) {  // INVALIDATE_STATE 0x7FFF, EVENT_WRITE_SHD x2
    return g.size() == at + 10u && g[at] == 0xC0003B00u && g[at + 1] == 0x7FFFu && g[at + 2] == 0xC0025800u &&
           g[at + 3] == 3u && g[at + 6] == 0xC0025800u && g[at + 7] == 3u;
  };

  const KArm sdk = runKickArm(k, false, abBit);
  bool lit = !sdk.bad && sdk.applied && sdk.segs.size() == want.size() && sdk.segs[0].phys == physOf(scr::kSeg) &&
             sdk.segStart != scr::kSeg && sdk.segs.back().phys == physOf(sdk.segStart);
  for (size_t i = 0; lit && i < want.size(); i++) {
    const std::vector<uint32_t> g = beRead(sdk.segs[i].pm4);
    lit = sdk.segs[i].dwords == want[i].size() + 10u && sdk.segs[i].zpdRecs == 0u && fenceAt(g, want[i].size()) &&
          std::equal(want[i].begin(), want[i].end(), g.begin());
  }
  lit = lit && sdk.cursor == sdk.segStart - 4u + 4u * uint32_t(want.back().size()) &&
        sdk.r3 == sdk.segStart - 4u + (inLoop ? 0u : 16u);
  check(lit, n + ": SDK arm: kicks " + where + " - " + std::to_string(want.size()) +
                 " published segments holding the asm's words split at each kick, each closed by the fence tail; "
                 "dev+0x28 the last dword in the new segment, r3 = " +
                 (inLoop ? "the cursor the last in-loop kick returned" : "the cursor after the entry kick + 16"));
  uint8_t d16[8], d24[8], d32[8];
  std::memcpy(d16, sdk.dirty.data() + 16, 8);
  std::memcpy(d24, sdk.dirty.data() + 24, 8);
  std::memcpy(d32, sdk.dirty.data() + 32, 8);
  check(be64(d16) == 0x8ull && be64(d24) == ((1ull << 38) | (1ull << 37)) && be64(d32) == 0x2000ull,
        n + ": SDK arm: the dirty masks dev+16 |= 8, dev+24 |= 1 << 38 | 1 << 37, dev+32 |= 0x2000");
  std::printf("[ZPD_PORT] %s sdk=%s\n", c.name, scr::hex(concat(sdk.segs)).c_str());

  const uint64_t calls0 = g_zpdPortCalls.get();
  const KArm nat = runKickArm(k, true, abBit);
  check(g_zpdPortCalls.get() == calls0 + 1u, n + ": the native arm took the call (an Entry, not the guest body)");
  check(nat.c0 == sdk.c0 && nat.r3 == sdk.r3 && nat.cursor == sdk.cursor, n + ": r3 and dev+0x28 equal the SDK's");
  check(nat.dirty == sdk.dirty && nat.m12432 == sdk.m12432 && nat.m12436 == sdk.m12436,
        n + ": the dirty masks dev+0..+39 equal the SDK's (and the bin masks dev+12432 / +12436 unchanged)");
  bool pubs = !nat.bad && nat.segs.size() == sdk.segs.size();
  for (size_t i = 0; pubs && i < nat.segs.size(); i++)
    pubs = nat.segs[i].phys == sdk.segs[i].phys && nat.segs[i].dwords == sdk.segs[i].dwords &&
           nat.segs[i].pm4 == sdk.segs[i].pm4;
  check(pubs, n + ": the published segments equal the SDK's (count, phys, dwords, each one's PM4: the guest bytes and "
                  "the records rendered back by cpRecordPm4)");
  // The post-kick tail: the new (last) segment holds the words after the last kick - the multi-tile LO {dev+12432} /
  // HI {dev+12436} and EVENT_WRITE_ZPD {21} - then the fence; EVENT_WRITE_ZPD as one kRkZpd {21} record packet in the
  // records modes (literal PM4 under SR_NATIVE_FLUSH_PM4), and none in a segment before the kick.
  bool tail = !nat.segs.empty();
  if (tail) {
    const std::vector<uint32_t> g = beRead(nat.segs.back().pm4);
    uint32_t before = 0;
    for (size_t i = 0; i + 1 < nat.segs.size(); i++) before += nat.segs[i].zpdRecs;
    tail = fenceAt(g, want.back().size()) && std::equal(want.back().begin(), want.back().end(), g.begin()) && before == 0u &&
           (pm4 ? nat.segs.back().zpdRecs == 0u : nat.segs.back().zpdRecs == 1u && nat.segs.back().zpdLast);
  }
  check(tail, n + ": the post-kick tail in the new segment (the multi-tile LO {dev+12432} / HI {dev+12436}, "
                  "EVENT_WRITE_ZPD {21}) before the close's fence: one kRkZpd {21} record packet (records modes) or "
                  "literal PM4 (SR_NATIVE_FLUSH_PM4), none before the kick");
  check(nat.applied && nat.fx == sdk.fx && nat.fx.zpdEvents == 1u && nat.fx.op5B == 1u &&
            nat.fx.r2325 == c.phys[c.tiles > 1u ? c.tiles - 1u : 0u],
        n + ": the CP applies the published segments in order (cpSegment): the registers, the bin masks and one "
            "EVENT_WRITE_ZPD equal the SDK's segments applied");
  std::printf("[ZPD_PORT] %s %s=%s\n", c.name, pm4 ? "producer" : "cp", scr::hex(concat(nat.segs)).c_str());
}
}  // namespace zpt

int runZpdPortTest(const char* mode) {
  namespace K = sr::kernel;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  {  // a bare run re-runs itself with exactly the runner's configuration for the mode (tools/difftest/suite_bare.inc)
    bare::Conf conf = {{L"SR_PM4_RING", L"0"}, {L"SR_PM4_RING_EXEC", L"0"}, {L"SR_PM4_CP", L"0"},
                       {L"SR_ALLOW_FORCING", L"0"}, {L"SR_AB_TOGGLE", L"SR_NATIVE_ZPD"}, {L"SR_AB_PRESENT", L"1"},
                       {L"SR_NATIVE_SEGMENTLESS", L"1"}, {L"SR_NATIVE_FLUSH", L"1"}};
    if (std::strcmp(mode, "cppm4") == 0) conf.push_back({L"SR_NATIVE_FLUSH_CPPM4", L"1"});
    if (std::strcmp(mode, "pm4") == 0) conf.push_back({L"SR_NATIVE_FLUSH_PM4", L"1"});
    int code = 0;
    if (bare::handled("ZPD_PORT_TEST", "--zpd-port-test", mode, L"SR_NATIVE_ZPD", conf, code)) return code;
  }
  const bool cppm4 = std::strcmp(mode, "cppm4") == 0, pm4 = std::strcmp(mode, "pm4") == 0;
  if (!cppm4 && !pm4 && std::strcmp(mode, "records") != 0) {
    std::printf("FAIL: unknown mode %s (records | cppm4 | pm4)\n[ZPD_PORT_TEST] 0 passed, 1 failed\n", mode);
    return 1;
  }
  K::registerStubs();
  sr::guest::initializeDispatchTable();
  K::kpcrBindCurrent(scr::kStack, scr::kStack - 0x10000u, 3);
  const int abBit = sr::abt::reg("SR_NATIVE_ZPD");
  zpt::check(g_on && g_segmentless && g_cpPm4 == cppm4 && g_pm4Out == pm4 && abBit >= 0 && sr::abt::active(),
             "requested mode: record flush, segmentless, the CPPM4 / PM4 split as named, the per-present A/B");
  zpt::check(zpdPortOn() && !std::getenv("SR_NATIVE_ZPD") && sr::gfx::zpd::mode() == sr::gfx::zpd::kLatency &&
                 !sr::gfx::zpd::explicitForm(),
             "SR_NATIVE_ZPD unset: the port's gate is ON and the mode the latency model, the default form (the bake)");
  zpt::check(PPC_LOOKUP_FUNC(K::getGuestBase(), 0x82101E08u) == ::sub_82101E08,
             "production dispatch resolves 82101E08 to the native override");
  bool mapped = true;
  for (uint32_t va : {scr::kDev, scr::kDev + 0x4FFFu, scr::kCell, scr::kFb, scr::kRing, scr::kBuf,
                      scr::kBuf + scr::kBufSize - 1u, scr::kStack - 0x2000u, zpt::kQ, zpt::kQ + 0xFFu})
    mapped = mapped && scr::host(va) != nullptr;
  zpt::check(mapped, "fixture memory mapped");
  if (zpt::failed) {
    std::printf("[ZPD_PORT_TEST] %d passed, %d failed\n", zpt::passed, zpt::failed);
    return 1;
  }
  if (!g_arena) g_arena = static_cast<uint8_t*>(VirtualAlloc(nullptr, kRecArena, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  // The slot VAs as the game's queries hold them (0xC0 view) and one in the 0xE0 view: phys = VA + 32 * which, then
  // ((VA >> 20) + 0x200) & 0x1000 added to VA & 0x1FFFFFFF - 0x1000 for the 0xE0 view, 0 for the 0xC0 view.
  const zpt::Case cases[] = {
      {"begin-1tile", 1u, 1u, {0xC0F20000u, 0u, 0u}, {0x00F20020u, 0u, 0u}, 0xFFFFFFFFu, 0u},
      {"end-1tile", 1u, 0u, {0xC0F20000u, 0u, 0u}, {0x00F20000u, 0u, 0u}, 0xFFFFFFFFu, 0u},
      {"end-0tiles-e0", 0u, 0u, {0xE0F20040u, 0u, 0u}, {0x00F21040u, 0u, 0u}, 0xFFFFFFFFu, 0u},
      {"begin-1tile-e0", 1u, 1u, {0xE0F20040u, 0u, 0u}, {0x00F21060u, 0u, 0u}, 0xFFFFFFFFu, 0u},
      {"begin-2tiles", 2u, 1u, {0xC0F20080u, 0xC0F200C0u, 0u}, {0x00F200A0u, 0x00F200E0u, 0u}, 0x0000000Fu, 0u},
      {"end-3tiles-e0", 3u, 0u, {0xE0F20100u, 0xC0F20140u, 0xE0F20180u}, {0x00F21100u, 0x00F20140u, 0x00F21180u},
       0x12345678u, 0x9ABCDEF0u},
  };
  for (const auto& c : cases) zpt::runCase(c, abBit, cppm4, pm4);
  // KICKS (the header): {case, the guest's NOPs, dev+0x30 - c0, dev+13492, the oracle's word index at each kick}. The
  // per-tile check follows tile t's 20 bytes at c0 + 28 + 20 (t + 1); the cut after tile t is word 7 + 5 (t + 1). Each
  // multi-tile case keeps dev+12432 != dev+12436 (a swapped tail is visible).
  const zpt::KickCase kicks[] = {
      {{"kick-entry-1tile", 1u, 1u, {0xC0F20400u, 0u, 0u}, {0x00F20420u, 0u, 0u}, 0xFFFFFFFFu, 0u}, 3u, -4, 0x2000u, {0u}},
      {{"kick-entry-3tiles-e0", 3u, 0u, {0xE0F20500u, 0xC0F20540u, 0xE0F20580u}, {0x00F21500u, 0x00F20540u, 0x00F21580u},
        0x0000000Fu, 0x000000F0u},
       1u, -8, 0x2000u, {0u}},
      {{"kick-mid-2tiles-t0", 2u, 1u, {0xC0F20600u, 0xC0F20640u, 0u}, {0x00F20620u, 0x00F20660u, 0u}, 0x00000003u,
        0x0000000Cu},
       0u, 44, 0x2000u, {12u}},
      {{"kick-mid-2tiles-t1", 2u, 0u, {0xE0F20700u, 0xC0F20740u, 0u}, {0x00F21700u, 0x00F20740u, 0u}, 0x12345678u,
        0x9ABCDEF0u},
       2u, 64, 0x2000u, {17u}},
      {{"kick-mid-3tiles-t0", 3u, 1u, {0xC0F20800u, 0xE0F20840u, 0xC0F20880u}, {0x00F20820u, 0x00F21860u, 0x00F208A0u},
        0x0000003Fu, 0x00000FC0u},
       1u, 44, 0x2000u, {12u}},
      {{"kick-mid-3tiles-t1-at-thr", 3u, 0u, {0xC0F20900u, 0xC0F20940u, 0xE0F20980u},
        {0x00F20900u, 0x00F20940u, 0x00F21980u}, 0x00000005u, 0x000000A0u},
       0u, 48, 0x2000u, {17u}},
      {{"kick-every-3tiles-minseg", 3u, 1u, {0xE0F20A00u, 0xE0F20A40u, 0xE0F20A80u},
        {0x00F21A20u, 0x00F21A60u, 0x00F21AA0u}, 0x00000011u, 0x00000022u},
       2u, -4, 0u, {0u, 12u, 17u, 22u}},
  };
  setSegmentPublicationObserver(zpt::observe);
  for (const auto& k : kicks) zpt::runKickCase(k, abBit, cppm4, pm4);
  setSegmentPublicationObserver(nullptr);
  std::printf("[ZPD_PORT_TEST] %d passed, %d failed\n", zpt::passed, zpt::failed);
  return zpt::failed ? 1 : 0;
}
}  // namespace sr::native::flush
