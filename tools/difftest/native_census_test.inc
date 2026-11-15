// Included by kernel/native_flush.cpp only in difftest (SR_DIFFTEST), after native_residue_test.inc (its rt:: fixture
// - the device, the command buffer, the allocator callback, the emulated in-order CP - is reused): the PM4 OWNER
// CENSUS (SR_NATIVE_BRACKETS=1: kernel/native_brackets' stamps, gfx/pm4_coverage's attribution) on the segmentless
// transport and on the ring path.
//
// Checked against expectations computed here from the fixtures' inputs (the producers' own PM4 by packet key: an owner
// GpuInvalidateMemoryRange 820F4280 = WAIT_UNTIL + the COHER block "3C:1,r05:1,r0A:2", VertexBuffer_Lock 820F3FA0 and
// the texture lock's core = the COHER block "3C:1,r0A:2", BlockUntilIdle 820FDCB8 = WAIT_UNTIL "r05:1", the fixture's
// own unbracketed PM4 = "r20:2"), by the OUTERMOST owner and by the INNERMOST producer (Stamp::inner):
//   producers  every producer's gap PM4 attributed to it although the CP runs the close's host COPY (cpSegment's gap
//              parts: the copy window, keyed by the part's guest address, with the stamps the close moved into the
//              segment); nested scopes split by producer; unbracketed PM4 UNSTAMPED; nothing else attributed;
//   moved      after a native close the closed range holds no stamp (moved with the copy; the close's own bytes are
//              never stamped again - the same-allocation tail and the BeginNewSegment tail);
//   seg-reuse  BeginNewSegment 820FCF90 hands a closed segment's memory out again: its stamps end there, unbracketed PM4
//              written into it reads UNSTAMPED (never the previous lap's owner);
//   block-reuse GpuCmdBufAllocate 820FCDF0 hands it out as a block (a kick block's allocator): the same, read in place
//              at depth 1 as an IB;
//   lagging    the CP runs a closed segment's copy AFTER the memory was handed out again and re-stamped by another
//              producer: the copy keeps the stamps of its own write;
//   block-scopes a 820FCDF0 block is born with its allocating scope (owner and producer, not 820FCDF0), reused
//              outside any bracket reads UNSTAMPED, reused inside another scope reads that scope's;
//   foreign    a bracket on a thread that does not own the device (dev+0x2A08) stamps nothing;
//   zero-owner the device released (D3DDevice_ReleaseThreadOwnership 820F3658: dev+0x2A08 = 0) or this thread's
//              identity unknown (KTHREAD+332 = 0): neither a piece nor a 820FCDF0 block stamped; owned again
//              (D3DDevice_AcquireThreadOwnership 820F3618), the scope's own;
//   transition thread B (another guest thread: its own KPCR, KTHREAD+332 = kThreadIdB) takes the device inside this
//              thread's open bracket (820F3618 steals: no wait) and writes; this thread takes it back (a nested scope):
//              B's bytes UNSTAMPED, this thread's before and after its own; an ownership change no scope of this
//              thread saw (the bracket opened while B owned the device): the piece dropped (owned at BOTH cuts);
//   foreign-alloc a 820FCDF0 block handed out inside a scope on thread B (not the owner): unstamped;
//   interleave A -> B -> A between two cuts that both read this thread: B takes the device (820F3618 on B) after this
//              thread's Acquire entry cut (its scope's before-hook), before its body takes it back - the ownership
//              epoch moved, B's PM4 UNSTAMPED, this thread's before and after its own;
//   read-gap   B takes the device between a cut's owner read and its cursor read (native_brackets' cut-gap hook): the
//              piece holding B's PM4 dropped whole;
//   raw-owner  an owner change no scope sees (raw dev+0x2A08 writes; no guest writer does that): B's stamped PM4 inside
//              this thread's piece stays B's, the piece is refused whole;
//   zpass-kick BeginZPass -> INVAL -> EndZPass: the SDK kick's blocks (82110C78, 820FCDF0 under EndZPass) attributed to
//              EndZPass (no INTERRUPT UNSTAMPED); a close chained behind the kick's pending pump leaves its 16-byte
//              list node marked unowned (kStampChain) - a CP read there counts in chain=, UNSTAMPED, never anyone's;
//   pump-driver the kick's pump run by the production driver 82110840 (its N0 bracket) on thread B, the device the
//              owner thread's, then released: its 820FD218 ring writes the driver's (RING attribution, ownership-
//              independent), the bin-select dwords 82110140 writes to the ring directly unstamped (incomplete
//              attribution, kept RING_UNSTAMPED), the owner thread's PM4 written while the driver's ring write is open
//              never the driver's (UNSTAMPED).
//   swap-frames every present the CP applies ends exactly one census frame: the XE_SWAP packet's at the packet, a
//              kRkSwap record's (SR_NATIVE_SWAP_BLOCK: no 0x64 packet; both record executors) at its present, the
//              record rendered to PM4 once, an unsigned XE_SWAP's at the packet without swallowing the next record's
//              (native/composed-census: before it the composed census never reported - tcmp2 census1).
// zpass-kick and pump-driver attribute the SDK Z-pass kick's PM4 (its INTERRUPT packets, the pump it leaves, the pump
// driver running it), so they hold only with SR_NATIVE_BRACKET=0 - this suite's configured run pins it. Under the
// composed transport (BRACKET native, the kick a record, no pump) their 12 assertions cannot hold by construction: the
// orchestrator accepted this as the one named exception (2026-10-14); tools/difftest/composed_suites_run.py allows
// exactly these two cases in its composed census row and requires census_no_bracket (composed, BRACKET at this pin)
// to pass whole.
// Modes (the runner sets the environment; the argument names the expectation): native (brackets on, segmentless,
// residue gates off), residue (the three residue gates on: the converted producers' rows empty), segmentless-off (the
// guest close, IBs read in place: the same attribution; the previous lap's stamps stay until the memory is handed out
// again), gate-off (SR_NATIVE_BRACKETS unset: nothing counted, nothing stamped). Every mode prints the digest of what
// the CP and the guest saw ([NATIVE_CENSUS_TEST] digest): the runner requires it equal with the census on and off.
#include <condition_variable>
#include <functional>

namespace sr::native::flush {
namespace cen {
namespace K = sr::kernel;
namespace cov = sr::gfx::pm4cov;
using rt::kBuf;
using rt::kBufSize;
using rt::kDev;

int passed = 0, failed = 0;
void check(bool ok, const std::string& name) {
  std::printf("%s%s\n", ok ? "  ok  " : "FAIL: ", name.c_str());
  ok ? ++passed : ++failed;
}
std::atomic<int64_t> g_deadline{INT64_MAX};
const char* volatile g_arm = "";
void watchdog() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (std::chrono::steady_clock::now().time_since_epoch().count() > g_deadline.load()) {
      std::printf("FAIL: %s: completed (no hang)\n", g_arm);
      std::printf("[NATIVE_CENSUS_TEST] %d passed, %d failed\n", passed, failed + 1);
      std::fflush(stdout);
      std::_Exit(1);
    }
  }
}
void arm(const char* name) {
  g_arm = name;
  g_deadline = std::chrono::steady_clock::now().time_since_epoch().count() +
               std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(20)).count();
}

constexpr int kKeys = 512 + cov::kOtherKeys;  // type-3 op | 256 + type-0 page | 512 + other (gfx/pm4_coverage)
constexpr uint32_t kInval = 0x820F4280u, kVbLock = 0x820F3FA0u, kIdle = 0x820FDCB8u, kTexLock = 0x820FFCB0u,
                   kEmit = 0x82113010u, kFlush = 0x820FD8C0u;
// D3DDevice_AcquireThreadOwnership (dev+0x2A08 = the caller's KTHREAD+332, no wait: it steals), ReleaseThreadOwnership
// (dev+0x2A08 = 0), the pump driver GpuCommandBufferReplayDriver, the ring writer
constexpr uint32_t kAcquire = 0x820F3618u, kRelease = 0x820F3658u, kDriver = 0x82110840u, kRingWr = 0x820FD218u;

struct Mode {
  bool on = false, seg = false, res = false;
  const char* name = "";
};
Mode g_mode;

// The census by (row, key), outermost owner and innermost producer, for every row (kEntries, UNSTAMPED, RING_UNSTAMPED).
struct Mark {
  std::vector<uint64_t> own, inn;
};
int rows() { return sr::native::kEntryCount + 2; }
int row(uint32_t va) { return sr::native::entryIndex(va); }
int unstampedRow() { return sr::native::kEntryCount; }
Mark mark() {
  Mark m;
  m.own.resize(size_t(rows()) * kKeys);
  m.inn.resize(size_t(rows()) * kKeys);
  for (int r = 0; r < rows(); r++)
    for (int k = 0; k < kKeys; k++) {
      m.own[size_t(r) * kKeys + k] = cov::ownerPackets(r, k, false);
      m.inn[size_t(r) * kKeys + k] = cov::ownerPackets(r, k, true);
    }
  return m;
}
// The row's packets between two marks, as the census log prints keys ("3C:1,r05:1,r0A:2"; "" = none).
std::string keys(const Mark& a, const Mark& b, int r, bool inner) {
  std::string s;
  if (r < 0 || r >= rows()) return "<no row>";
  const std::vector<uint64_t>& x = inner ? a.inn : a.own;
  const std::vector<uint64_t>& y = inner ? b.inn : b.own;
  char t[40];
  for (int k = 0; k < kKeys; k++) {
    const uint64_t n = y[size_t(r) * kKeys + k] - x[size_t(r) * kKeys + k];
    if (!n) continue;
    if (k < 256) std::snprintf(t, sizeof(t), "%s%02X:%llu", s.empty() ? "" : ",", k, (unsigned long long)n);
    else if (k < 512) std::snprintf(t, sizeof(t), "%sr%02X:%llu", s.empty() ? "" : ",", k - 256, (unsigned long long)n);
    else std::snprintf(t, sizeof(t), "%so%d:%llu", s.empty() ? "" : ",", k - 512, (unsigned long long)n);
    s += t;
  }
  return s;
}
uint64_t count(const Mark& a, const Mark& b, int r, int key, bool inner) {
  const std::vector<uint64_t>& x = inner ? a.inn : a.own;
  const std::vector<uint64_t>& y = inner ? b.inn : b.own;
  return y[size_t(r) * kKeys + key] - x[size_t(r) * kKeys + key];
}
// Every row that moved, by owner and by producer (the scenario's log), and the set of rows that moved.
std::vector<int> moved(const Mark& a, const Mark& b, bool inner) {
  std::vector<int> out;
  for (int r = 0; r < rows(); r++)
    if (!keys(a, b, r, inner).empty()) out.push_back(r);
  return out;
}
std::string rowName(int r) {
  if (r == unstampedRow()) return "UNSTAMPED";
  if (r == unstampedRow() + 1) return "RING_UNSTAMPED";
  char b[16];
  std::snprintf(b, sizeof(b), "%08X", sr::native::kEntries[r].va);
  return b;
}
void logRows(const char* scen, const Mark& a, const Mark& b) {
  for (int inner = 0; inner < 2; inner++)
    for (int r : moved(a, b, inner != 0))
      std::printf("[CENSUS] %s %s=%s keys=%s\n", scen, inner ? "inner" : "owner", rowName(r).c_str(),
                  keys(a, b, r, inner != 0).c_str());
}
bool rowsAre(const Mark& a, const Mark& b, bool inner, std::vector<int> want) {
  std::vector<int> got = moved(a, b, inner);
  std::sort(want.begin(), want.end());
  return got == want;
}

// The interval map over [vaLo, vaHi): the stamps a CP window would read there now.
std::vector<sr::native::Interval> stampsIn(uint32_t vaLo, uint32_t vaHi) {
  std::vector<sr::native::Interval> v(4096);
  const uint32_t lo = sr::native::stampKey(vaLo), hi = sr::native::stampKey(vaHi);
  v.resize(lo && hi > lo ? sr::native::snapshotIntervals(lo, hi, v.data(), v.size()) : 0u);
  return v;
}

// The stamps over [vaLo, vaHi) into the scenario's log (a "moved" / "ended" check's evidence).
size_t dumpStamps(const char* scen, const char* what, uint32_t vaLo, uint32_t vaHi) {
  const std::vector<sr::native::Interval> v = stampsIn(vaLo, vaHi);
  std::printf("[CENSUS] %s %s: %zu stamp(s) over [%08X, %08X) (keys %08X..)\n", scen, what, v.size(), vaLo, vaHi,
              sr::native::stampKey(vaLo));
  for (size_t i = 0; i < v.size() && i < 8; i++)
    std::printf("[CENSUS]   [%08X, %08X) owner=%s inner=%s kind=%u call=%u\n", v[i].lo, v[i].hi,
                v[i].s.entry >= 0 ? rowName(v[i].s.entry).c_str() : "-",
                v[i].s.inner >= 0 ? rowName(v[i].s.inner).c_str() : "-", unsigned(v[i].s.kind), v[i].s.callId);
  return v.size();
}

// The fixture's own unbracketed PM4 at the cursor (a writer no XDK scope covers: inline guest PM4, a kick block's
// bytes): two type-0 writes of 0x2000 (key r20, which no producer here writes).
void unbracketed(uint32_t k) {
  const uint32_t at = rt::rd(kDev + 0x28u) + 4u;
  const uint32_t w[] = {0x00002000u, 0xC0DE0000u | k, 0x00002000u, 0xC0DE1000u | k};
  for (uint32_t i = 0; i < 4u; i++) rt::wr(at + 4u * i, w[i]);
  rt::wr(kDev + 0x28u, at + 12u);
}

rt::Case invalCase() {
  rt::Case c{};
  c.name = "inval";
  c.prod = rt::kInval;
  c.addr = 0xA0B00123u;
  c.size = 0x2345u;
  return c;
}
rt::Case lockCase() {  // VertexBuffer_Lock 820F3FA0 (the residue suite's lock-vb)
  rt::Case c{};
  c.name = "lock-vb";
  c.prod = rt::kLock;
  return c;
}
rt::Case texCase() {  // the lock core 820F3988 with the texture lock's COHER status (kind 22 when converted)
  rt::Case c{};
  c.name = "lock-texture";
  c.prod = rt::kLock;
  c.route = 1;
  c.status = 0x02000000u;
  c.addr = 0xA0D01234u;
  c.size = 0x3000u;
  return c;
}
rt::Case idleCase() {  // BlockUntilIdle with the open fence S: its kick closes the segment inside the call
  rt::Case c{};
  c.name = "idle";
  c.prod = rt::kIdle;
  return c;
}
void runProducer(const rt::Case& cs) {
  PPCContext c = rt::context();
  rt::callProducer(cs, c);
}
// A table scope opened here around fixture work, as the entry `va` would open it around what it calls.
template <class F>
void within(uint32_t va, F f) {
  PPCContext c = rt::context();
  sr::native::XdkScope scope(row(va), c);
  f();
}

std::unique_ptr<sr::gfx::Pm4Engine> g_engine;
// One scenario's start: the residue suite's fixture state, a fresh emulated CP, the fixture memory unstamped.
void begin(const char* name) {
  arm(name);
  rt::Case base{};
  base.name = name;
  base.prod = rt::kIdle;
  rt::prepare(base);
  sr::native::eraseIntervals(sr::native::stampKey(kBuf), sr::native::stampKey(kBuf + kBufSize));
  g_engine = std::make_unique<sr::gfx::Pm4Engine>();
  sr::gfx::Pm4Engine& e = *g_engine;
  e.submitDraws = false;
  e.cpMode = true;
  e.physToHost = rt::fxMap;
  e.cp.user = &rt::g_fx;
  e.cp.interrupt = rt::fxInterrupt;
  e.cp.running = rt::fxRunning;
  e.cp.trace = rt::fxTrace;
  e.binMask = ~0ull;
  e.binSelect = ~0ull;
  e.writeReg(0x1DCu, 0x00020033u);
  e.writeReg(0x1DDu, rt::physOf(rt::kPc));
  auto& xr = sr::gfx::getXenosRegisters();
  for (uint32_t r : {0x5C8u, 0xA2Fu, 0xA30u, 0xA31u}) xr.write(r, 0xDEAD0000u | r);
  rt::g_eng = &e;
  rt::g_sync = true;
}

// What the CP and the guest saw (FNV-1a): the device, command buffer, ring, fence block, resource, the CP's stores and
// interrupts, its COHER / WAIT_UNTIL registers. Equal with the census on and off = the census is observe-only.
uint64_t g_digest = 0xCBF29CE484222325ull;
void mix(const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; i++) g_digest = (g_digest ^ b[i]) * 0x100000001B3ull;
}
void end(const char* name) {
  const rt::Snap s = rt::snapNow();
  const uint64_t d0 = g_digest;
  for (const std::vector<uint8_t>* v : {&s.device, &s.buffer, &s.ring, &s.fb, &s.res}) mix(v->data(), v->size());
  for (const auto& e : rt::g_fx.log) mix(e.data(), sizeof(e));
  auto& xr = sr::gfx::getXenosRegisters();
  for (uint32_t r : {0x5C8u, 0xA2Fu, 0xA30u, 0xA31u}) {
    const uint32_t v = xr.read(r);
    mix(&v, 4);
  }
  check(!rt::g_bad && !g_engine->nativeRecordFailed && !rt::g_fx.blocked,
        std::string(name) + ": the CP consumed everything in order, no blocked wait");
  std::printf("[CENSUS] %s digest %016llX (from %016llX)\n", name, (unsigned long long)g_digest,
              (unsigned long long)d0);
  rt::g_eng = nullptr;
  g_engine.reset();
  g_deadline = INT64_MAX;
}

// ---- producers: every producer's gap PM4 by owner and by producer ----
void producers() {
  const char* n = "producers";
  begin(n);
  const uint32_t seg0 = rt::rd(kDev + 0x34C4u);
  const Mark m0 = mark();
  runProducer(invalCase());                 // owner = producer = INVAL
  runProducer(lockCase());                  // VertexBuffer_Lock
  within(kTexLock, [] { runProducer(texCase()); });  // the texture lock's entry around the lock core
  runProducer(idleCase());                  // BlockUntilIdle: WAIT_UNTIL, then its kick closes the segment
  unbracketed(1u);                      // no scope: UNSTAMPED
  within(kEmit, [] {                    // an entry writing its own PM4 and calling INVAL nested
    unbracketed(2u);
    runProducer(invalCase());
  });
  rt::closeNow();
  rt::apply();
  const Mark m1 = mark();
  logRows(n, m0, m1);
  const std::string N = std::string(n) + ": ";
  const int inval = row(kInval), lock = row(kVbLock), tex = row(kTexLock), idle = row(kIdle), emit = row(kEmit),
            un = unstampedRow();
  if (!g_mode.on) {
    check(moved(m0, m1, false).empty() && moved(m0, m1, true).empty() && cov::accountedPackets() == 0u &&
              cov::unstampedPackets() == 0u,
          N + "census off: nothing counted");
    check(stampsIn(kBuf, kBuf + kBufSize).empty(), N + "census off: nothing stamped");
    end(n);
    return;
  }
  const std::string ownBlock = g_mode.res ? "" : "3C:1,r05:1,r0A:2";  // one owner INVAL: WAIT_UNTIL + COHER
  check(keys(m0, m1, inval, true) == (g_mode.res ? "" : "3C:2,r05:2,r0A:4"),
        N + "producer INVAL: its two calls' WAIT_UNTIL + COHER blocks (the nested one included)");
  check(keys(m0, m1, inval, false) == ownBlock, N + "owner INVAL: the outermost call's block only");
  check(keys(m0, m1, lock, true) == (g_mode.res ? "" : "3C:1,r0A:2") && keys(m0, m1, lock, false) == keys(m0, m1, lock, true),
        N + "VertexBuffer_Lock: its COHER block, as owner and producer");
  check(keys(m0, m1, tex, true) == (g_mode.res ? "" : "3C:1,r0A:2"), N + "texture lock entry: its core's COHER block");
  check(keys(m0, m1, idle, true) == (g_mode.res ? "" : "r05:1"), N + "producer BlockUntilIdle: its WAIT_UNTIL");
  check(keys(m0, m1, emit, true) == "r20:2", N + "producer EmitPm4Packet scope: its own PM4 only (not the nested INVAL's)");
  check(keys(m0, m1, emit, false) == (g_mode.res ? "r20:2" : "3C:1,r05:1,r0A:2,r20:2"),
        N + "owner EmitPm4Packet scope: its own PM4 and the nested INVAL's");
  check(count(m0, m1, un, 256 + 0x20, false) == 2u && count(m0, m1, un, 256 + 0x20, true) == 2u,
        N + "the unbracketed PM4 UNSTAMPED (owner and producer)");
  if (g_mode.seg) {
    // segmentless: the CP ran gap copies and records only - nothing but the producers and the unbracketed PM4
    check(keys(m0, m1, un, false) == "r20:2", N + "segmentless: UNSTAMPED holds the unbracketed PM4 only");
    std::vector<int> wantInner = {emit, un}, wantOwner = {emit, un};
    if (!g_mode.res) {
      wantInner.insert(wantInner.end(), {inval, lock, tex, idle});
      wantOwner.insert(wantOwner.end(), {inval, lock, tex, idle});
    }
    check(rowsAre(m0, m1, true, wantInner) && rowsAre(m0, m1, false, wantOwner),
          N + "segmentless: no other row attributed (the copies' bytes went to their writers)");
    check(dumpStamps(n, "closed", seg0, rt::rd(kDev + 0x34C4u)) == 0u,
          N + "moved: the closed ranges hold no stamp (moved with the copies; the closes' own bytes never restamped)");
  } else {
    check(count(m0, m1, idle, 256 + 0x05, false) == 1u, N + "ring path: owner BlockUntilIdle holds its WAIT_UNTIL");
    check(dumpStamps(n, "closed", seg0, rt::rd(kDev + 0x34C4u)) > 0u,
          N + "ring path: the guest close leaves the in-place stamps (until the memory is handed out again)");
  }
  if (g_mode.res)
    check(rt::ctr(g_rsTaken[kRsInval]) >= 2u && rt::ctr(g_rsTaken[kRsLock]) >= 2u && rt::ctr(g_rsTaken[kRsIdle]) >= 1u,
          N + "residue: the producers were taken natively (record spans, no PM4)");
  end(n);
}

// ---- the BeginNewSegment tail of a native close: the switch's cut does not stamp the closed range ----
void switchTail() {
  const char* n = "switch-tail";
  begin(n);
  const uint32_t seg0 = rt::rd(kDev + 0x34C4u);
  const Mark m0 = mark();
  runProducer(invalCase());
  rt::wr(kDev + 0x30u, rt::rd(kDev + 0x28u) - 4u);  // the cursor past dev+0x30: the close's tail takes a new segment
  rt::closeNow();
  rt::apply();
  const Mark m1 = mark();
  logRows(n, m0, m1);
  const uint32_t seg1 = rt::rd(kDev + 0x34C4u);
  const std::string N = std::string(n) + ": ";
  if (g_mode.on && g_mode.seg) {
    check(seg1 >= kBuf + 0x60000u, N + "precondition: the native close's tail began a new segment (820FCF90)");
    check(dumpStamps(n, "closed", seg0, kBuf + 0x60000u) == 0u,
          N + "moved: the closed allocation holds no stamp (no later cut - the switch's, the ring write's - restamps it)");
    check(keys(m0, m1, row(kInval), true) == (g_mode.res ? "" : "3C:1,r05:1,r0A:2"), N + "producer INVAL: its block");
  }
  end(n);
}

// ---- BeginNewSegment 820FCF90 hands a closed segment's memory out again ----
void segReuse() {
  const char* n = "seg-reuse";
  begin(n);
  const uint32_t a = rt::rd(kDev + 0x34C4u);
  runProducer(invalCase());
  rt::closeNow();
  rt::apply();
  const size_t before = dumpStamps(n, "before the hand-out", a, a + 0x40u);
  rt::g_top = a + 0x10000u;  // the allocator callback's next segment (dev+0x34B4 = 0x10000 bytes) lands on `a`
  PPCContext c = rt::context();
  c.r3.u64 = kDev;
  c.r4.u64 = 0u;
  rt::call(0x820FCF90u, c);
  const uint32_t seg = rt::rd(kDev + 0x34C4u), cur = rt::rd(kDev + 0x28u);
  const size_t after = dumpStamps(n, "after the hand-out", a, a + 0x40u);
  const Mark m0 = mark();
  unbracketed(3u);  // the next writer brackets nothing
  rt::closeNow();
  rt::apply();
  const Mark m1 = mark();
  logRows(n, m0, m1);
  const std::string N = std::string(n) + ": ";
  check(seg == a && cur == a - 4u, N + "precondition: BeginNewSegment's new segment reuses the closed one's memory");
  if (g_mode.on) {
    check(g_mode.seg ? before == 0u : before > 0u,
          N + (g_mode.seg ? "the native close moved the stamps (none left in place)"
                          : "precondition: the guest close left the previous lap's stamps in place"));
    check(after == 0u, N + "BeginNewSegment's hand-out ended the previous lap's stamps");
    check(count(m0, m1, unstampedRow(), 256 + 0x20, false) == 2u && keys(m0, m1, row(kInval), false).empty() &&
              keys(m0, m1, row(kInval), true).empty(),
          N + "no stale owner: the unbracketed PM4 in the reused segment reads UNSTAMPED");
  }
  end(n);
}

// ---- GpuCmdBufAllocate 820FCDF0 hands it out as a block (a kick block), read in place as an IB ----
void blockReuse() {
  const char* n = "block-reuse";
  begin(n);
  const uint32_t a = rt::rd(kDev + 0x34C4u);
  runProducer(invalCase());
  rt::closeNow();
  rt::apply();
  const size_t before = dumpStamps(n, "before the hand-out", a, a + 32u);
  rt::g_top = a + 32u;  // the allocator callback's next block lands on `a`
  PPCContext c = rt::context();
  c.r3.u64 = kDev;
  c.r4.u64 = 8u;  // dwords
  c.r5.u64 = 4u;  // alignment
  rt::call(0x820FCDF0u, c);
  const uint32_t blk = c.r3.u32;
  const size_t after = dumpStamps(n, "after the hand-out", a, a + 32u);
  const std::string N = std::string(n) + ": ";
  check(blk == a, N + "precondition: the block reuses the closed segment's memory");
  const uint32_t w[] = {0x00002000u, 0xC0DE0004u, 0x00002000u, 0xC0DE1004u};  // unbracketed PM4 in the block
  for (uint32_t i = 0; i < 4u; i++) rt::wr(blk + 4u * i, w[i]);
  const Mark m0 = mark();
  g_engine->execute(rt::host(blk), sizeof(w), 1);  // the CP reads it in place, as an IB's target
  const Mark m1 = mark();
  logRows(n, m0, m1);
  if (g_mode.on) {
    check(g_mode.seg ? before == 0u : before > 0u,
          N + (g_mode.seg ? "the native close moved the stamps (none left in place)"
                          : "precondition: the guest close left the previous lap's stamps in place"));
    check(after == 0u, N + "GpuCmdBufAllocate's hand-out ended the previous lap's stamps");
    check(keys(m0, m1, unstampedRow(), false) == "r20:2" && keys(m0, m1, unstampedRow(), true) == "r20:2" &&
              moved(m0, m1, false).size() == 1u,
          N + "no stale owner: the block's PM4 reads UNSTAMPED");
  }
  end(n);
}

// ---- the CP lags: a closed segment's copy runs after its memory was handed out and re-stamped ----
void lagging() {
  const char* n = "lagging";
  begin(n);
  rt::g_sync = false;  // the CP runs only when told
  const uint32_t a = rt::rd(kDev + 0x34C4u);
  const Mark m0 = mark();
  runProducer(invalCase());
  rt::closeNow();  // the INVAL segment is published, not yet run
  rt::g_top = a + 0x10000u;
  PPCContext c = rt::context();
  c.r3.u64 = kDev;
  c.r4.u64 = 0u;
  rt::call(0x820FCF90u, c);  // its memory handed out again
  const uint32_t seg = rt::rd(kDev + 0x34C4u);
  runProducer(lockCase());        // another producer writes (and stamps) there
  const std::vector<sr::native::Interval> now = stampsIn(a, a + 0x40u);
  rt::apply();  // the CP runs the INVAL copy now
  const Mark m1 = mark();
  rt::g_sync = true;
  rt::closeNow();
  rt::apply();
  const Mark m2 = mark();
  logRows(n, m0, m2);
  std::printf("[CENSUS] %s segment %08X, %zu stamps there when the copy ran\n", n, seg, now.size());
  const std::string N = std::string(n) + ": ";
  if (g_mode.on && !g_mode.res) {
    check(seg == a && !now.empty() && now.front().s.entry == row(kVbLock),
          N + "precondition: when the CP ran the copy, its memory was VertexBuffer_Lock's");
    check(keys(m0, m1, row(kInval), true) == "3C:1,r05:1,r0A:2" && keys(m0, m1, row(kVbLock), true).empty(),
          N + "the copy keeps the stamps of its own write: INVAL's packets INVAL's");
    check(keys(m1, m2, row(kVbLock), true) == "3C:1,r0A:2" && keys(m1, m2, row(kInval), true).empty(),
          N + "then VertexBuffer_Lock's segment: its packets its own");
  }
  end(n);
}

// GpuCmdBufAllocate 820FCDF0(dev, dwords, 4) through production dispatch, the allocator callback's block landing on `at`.
uint32_t allocAt(uint32_t at, uint32_t dwords) {
  rt::g_top = at + 4u * dwords;
  PPCContext c = rt::context();
  c.r3.u64 = kDev;
  c.r4.u64 = dwords;
  c.r5.u64 = 4u;
  rt::call(0x820FCDF0u, c);
  return c.r3.u32;
}
// Two type-0 writes (r20:2) into a block, read by the CP in place as an IB's target.
void runBlock(uint32_t blk, uint32_t k) {
  const uint32_t w[] = {0x00002000u, 0xB10C0000u | k, 0x00002000u, 0xB10C1000u | k};
  for (uint32_t i = 0; i < 4u; i++) rt::wr(blk + 4u * i, w[i]);
  g_engine->execute(rt::host(blk), sizeof(w), 1);
}

// ---- a block is born at its 820FCDF0 hand-out with the allocating scope; reused, it reads its new allocator's ----
void blockScopes() {
  const char* n = "block-scopes";
  begin(n);
  const uint32_t a = kBuf + 0x40000u;  // away from the segments
  const Mark m0 = mark();
  uint32_t b1 = 0, b3 = 0;
  within(kEmit, [&] { b1 = allocAt(a, 8u); });  // lap 1: an entry scope allocates and writes it
  runBlock(b1, 1u);
  const Mark m1 = mark();
  const uint32_t b2 = allocAt(a, 8u);  // lap 2: handed out outside any bracket
  runBlock(b2, 2u);
  const Mark m2 = mark();
  within(kTexLock, [&] { b3 = allocAt(a, 8u); });  // lap 3: another scope
  const std::vector<sr::native::Interval> s3 = stampsIn(a, a + 32u);
  runBlock(b3, 3u);
  const Mark m3 = mark();
  logRows(n, m0, m3);
  const std::string N = std::string(n) + ": ";
  check(b1 == a && b2 == a && b3 == a, N + "precondition: the three hand-outs are the same memory");
  if (g_mode.on) {
    check(keys(m0, m1, row(kEmit), true) == "r20:2" && keys(m0, m1, row(kEmit), false) == "r20:2" &&
              moved(m0, m1, true).size() == 1u,
          N + "a block handed out inside a scope is that scope's (owner and producer), nothing else attributed");
    check(keys(m1, m2, unstampedRow(), true) == "r20:2" && moved(m1, m2, true).size() == 1u,
          N + "reused outside any bracket: UNSTAMPED, not the previous allocator's");
    check(keys(m2, m3, row(kTexLock), true) == "r20:2" && moved(m2, m3, true).size() == 1u,
          N + "reused inside another scope: that scope's");
    check(s3.size() == 1u && s3[0].s.kind == sr::native::kStampBlock && s3[0].s.entry == row(kTexLock) &&
              s3[0].s.inner == row(kTexLock),
          N + "the stamp is the block's (kind block, the allocating scope as owner and producer, not 820FCDF0)");
  }
  end(n);
}

// ---- the Z-pass kick: its blocks are the allocating scope's; a close chained behind its pump leaves a marked node ----
void zpassKick() {
  const char* n = "zpass-kick";
  begin(n);
  rt::g_sync = false;  // the CP lags the producer (a pump never runs inside the producer's kick)
  const Mark m0 = mark();
  rt::zBegin();
  runProducer(invalCase());
  rt::zEnd();  // EndZPass 8210E1A0: its flushes, then the SDK kick 82110C78 (via 8210E458), its blocks from 820FCDF0
  const uint32_t a74 = rt::rd(kDev + 0x2A74u);  // the kick's pump work outstanding
  unbracketed(5u);
  // a close while that pump is outstanding chains its segment behind it (820FD800 -> 820FCA80: the list node at the
  // close's end, the fence record's 40 bytes past the cursor); the cursor past dev+0x30: the tail begins a new segment
  const uint32_t node = rt::rd(kDev + 0x28u) + 4u + 40u;
  rt::wr(kDev + 0x30u, rt::rd(kDev + 0x28u) - 4u);
  const uint64_t chained0 = g_segChained.get();
  rt::closeNow();
  const bool chained = g_segChained.get() == chained0 + 1u;
  const std::vector<sr::native::Interval> mark0 = stampsIn(node, node + 16u);
  rt::drain();
  rt::closeNow();
  rt::drain();
  const Mark m1 = mark();
  logRows(n, m0, m1);
  dumpStamps(n, "the list node", node, node + 16u);
  std::printf("[CENSUS] %s dev+0x2A74 after the kick %u, chained %d, node %08X\n", n, a74, chained ? 1 : 0, node);
  const std::string N = std::string(n) + ": ";
  const int endz = row(0x8210E1A0u), un = unstampedRow();
  if (g_mode.on) {
    uint64_t irq = 0;  // the kick's INTERRUPT packets, by producer, wherever attributed
    for (int r = 0; r < rows(); r++) irq += count(m0, m1, r, 0x54, true);
    check(irq >= 1u, N + "precondition: the kick's INTERRUPT reached the CP");
    check(count(m0, m1, endz, 0x54, true) == irq && count(m0, m1, endz, 0x54, false) == irq,
          N + "the kick blocks are attributed: every INTERRUPT EndZPass's (the blocks born with their allocating scope)");
    check(count(m0, m1, un, 256 + 0x20, true) == 2u, N + "the unbracketed PM4 UNSTAMPED");
  }
  if (g_mode.seg) {
    // a CP read of the node's bytes (synthetic, after the pump consumed it; in every segmentless mode, census on or
    // off, so the digests stay comparable) counts apart, never as anyone's packets
    const uint64_t c0 = cov::chainPackets(), u0 = cov::unstampedPackets();
    const Mark m2 = mark();
    runBlock(node, 6u);
    const Mark m3 = mark();
    if (g_mode.on) {
      check(a74 != 0u && chained, N + "precondition: the native close chained behind the kick's pump (a list node)");
      check(mark0.size() == 1u && mark0[0].s.entry < 0 && (mark0[0].s.flags & sr::native::kStampChain) &&
                mark0[0].lo == sr::native::stampKey(node) && mark0[0].hi == sr::native::stampKey(node) + 16u,
            N + "the 16-byte list node is marked unowned (chain), not stamped by the switch's cut or the flush's leave");
      check(cov::chainPackets() - c0 == 2u && cov::unstampedPackets() - u0 == 2u &&
                keys(m2, m3, un, true) == "r20:2" && moved(m2, m3, true).size() == 1u,
            N + "packets read from the node are UNSTAMPED and counted in chain= (an unstamped total reconciles)");
    }
  }
  end(n);
}

// ---- a bracket on a thread that does not own the device stamps nothing (the owner's cursor is not its) ----
void foreign() {
  const char* n = "foreign";
  begin(n);
  const Mark m0 = mark();
  rt::wr(kDev + 0x2A08u, rt::kThreadId + 1u);  // another thread owns the device: this thread's scopes are foreign
  within(kEmit, [] { unbracketed(6u); });      // the owner's bytes written meanwhile (this fixture writes them)
  rt::wr(kDev + 0x2A08u, rt::kThreadId);
  within(kEmit, [] { unbracketed(7u); });      // the control: the owner's own scope
  rt::closeNow();
  rt::apply();
  const Mark m1 = mark();
  logRows(n, m0, m1);
  if (g_mode.on) {
    const std::string N = std::string(n) + ": ";
    check(count(m0, m1, unstampedRow(), 256 + 0x20, true) == 2u && keys(m0, m1, row(kEmit), true) == "r20:2",
          N + "a non-owner thread's bracket stamps nothing: its window's PM4 UNSTAMPED, the owner's scope's its own");
  }
  end(n);
}

// ---- thread B: a second guest thread with its own identity ----
// Its own KPCR (kpcrBindCurrent), KPCR+0x100 -> the fixture KTHREAD kKtB while it runs a task, whose +332 is kThreadIdB
// (what the SDK's owner test 82468EF8 and the census read), its own guest stack. The suite's thread posts a task and
// waits; meanwhile it makes the owner-thread writes B asks for (ownerWrite), so the interleaving is deterministic.
constexpr uint32_t kKtB = rt::kKt + 0x200u, kThreadIdB = 0x00ABCD02u, kStackB = rt::kStack - 0x10000u;
constexpr uint32_t kPumpCtx = rt::kCell + 0x100u;  // the pump driver's context word (-> the replay state dev+0x2A3C)
struct WorkerB {
  std::mutex mu;
  std::condition_variable cv;
  std::function<void()> task;
  bool posted = false;
  std::atomic<bool> done{false}, ownerAck{false};
  std::atomic<uint32_t> pcr{0}, hostTid{0}, ownerReq{0};  // ownerReq: the key of the write B asks for (0 = none)
};
WorkerB* const g_b = new WorkerB;  // never destroyed: the thread outlives the suite
thread_local bool t_onB = false;
void workerB(uint8_t* base) {
  K::setGuestMemory(base, 0x100000000ull);
  WorkerB& w = *g_b;
  rt::t_inApply = true;  // B's ring writes only queue: the suite's thread runs the CP
  rt::t_stackTop = kStackB;
  t_onB = true;
  w.pcr = K::kpcrBindCurrent(kStackB, kStackB - 0x10000u, 2);
  w.hostTid = GetCurrentThreadId();  // last: startB reads both once this is set
  for (;;) {
    std::function<void()> f;
    {
      std::unique_lock<std::mutex> lk(w.mu);
      w.cv.wait(lk, [&] { return w.posted; });
      w.posted = false;
      f = std::move(w.task);
    }
    const uint32_t pcr = w.pcr.load(), old = pcr ? rt::rd(pcr + 0x100u) : 0u;
    if (pcr) rt::wr(pcr + 0x100u, kKtB);
    f();
    if (pcr) rt::wr(pcr + 0x100u, old);
    w.done = true;
  }
}
bool startB() {
  std::thread(workerB, K::getGuestBase()).detach();
  for (int i = 0; i < 2000 && !g_b->hostTid.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return g_b->hostTid.load() != 0u && g_b->pcr.load() != 0u && g_b->pcr.load() != K::kpcrCurrent();
}
// Run f on thread B and wait for it, making the owner thread's writes it asks for meanwhile.
void onB(std::function<void()> f) {
  WorkerB& w = *g_b;
  rt::wr(kKtB + 332u, kThreadIdB);
  w.done = false;
  {
    std::lock_guard<std::mutex> lk(w.mu);
    w.task = std::move(f);
    w.posted = true;
  }
  w.cv.notify_one();
  while (!w.done.load()) {
    if (const uint32_t k = w.ownerReq.exchange(0u)) {
      unbracketed(k);  // the owner thread's PM4, no bracket of its own open
      w.ownerAck = true;
    }
    std::this_thread::yield();
  }
}
// On thread B: the owner thread writes its PM4 now (B waits for it).
void ownerWrite(uint32_t k) {
  WorkerB& w = *g_b;
  w.ownerAck = false;
  w.ownerReq = k;
  while (!w.ownerAck.load()) std::this_thread::yield();
}
// The suite's 820FD218 after-hook (the emulated CP's, rt::ringAfter): armed, the owner thread writes while B's ring
// write is open (inside the pump driver's bracket on B, before 820FD218's scope leaves and cuts).
// On B it also records each 820FD218 call's ring range [w0, w1) (dev+0x2A44 at its scope's enter and leave): the
// dwords its N0 scope stamps.
std::atomic<uint32_t> g_ownerKey{0};
std::vector<std::pair<uint32_t, uint32_t>> g_ringCalls;  // written on B only, read while B is idle
thread_local uint32_t t_ringW0 = 0;
void ringBefore(void*) {
  if (t_onB) t_ringW0 = rt::rd(kDev + 0x2A44u);
}
void ringAfter(void* p) {
  if (t_onB) {
    g_ringCalls.push_back({t_ringW0, rt::rd(kDev + 0x2A44u)});
    if (const uint32_t k = g_ownerKey.exchange(0u)) ownerWrite(k);
  }
  rt::ringAfter(p);
}
// D3DDevice_AcquireThreadOwnership / ReleaseThreadOwnership(dev) through production dispatch on the calling thread.
uint32_t ownership(uint32_t va) {
  PPCContext c = rt::context();
  c.r3.u64 = kDev;
  rt::call(va, c);
  return rt::rd(kDev + 0x2A08u);
}

// ---- the device released, or this thread's identity unknown: nothing stamped ----
void zeroOwner() {
  const char* n = "zero-owner";
  begin(n);
  const uint32_t a = kBuf + 0x40000u;
  const Mark m0 = mark();
  const uint32_t released = ownership(kRelease);  // dev+0x2A08 = 0
  uint32_t blk = 0;
  within(kEmit, [&] {
    unbracketed(8u);
    blk = allocAt(a, 8u);
  });
  const std::vector<sr::native::Interval> sb = stampsIn(a, a + 32u);
  runBlock(blk, 8u);
  const Mark m1 = mark();
  rt::wr(rt::kKt + 332u, 0u);  // this thread's identity unknown, the device unowned: 0 == 0 is no ownership
  within(kEmit, [] { unbracketed(9u); });
  rt::wr(rt::kKt + 332u, rt::kThreadId);
  const uint32_t acquired = ownership(kAcquire);  // this thread's again
  within(kEmit, [] { unbracketed(10u); });       // the control
  rt::closeNow();
  rt::apply();
  const Mark m2 = mark();
  logRows(n, m0, m2);
  const std::string N = std::string(n) + ": ";
  const int un = unstampedRow(), emit = row(kEmit);
  check(released == 0u && acquired == rt::kThreadId && blk == a,
        N + "precondition: 820F3658 released the device (0), 820F3618 gave it back to this thread, the block at a");
  if (g_mode.on) {
    check(sb.empty() && keys(m0, m1, un, true) == "r20:2" && moved(m0, m1, true).size() == 1u,
          N + "a block handed out with the device released is not stamped: its PM4 UNSTAMPED");
    check(count(m1, m2, un, 256 + 0x20, true) == 4u && count(m1, m2, un, 256 + 0x20, false) == 4u,
          N + "the released device's and the unknown identity's pieces are not stamped: UNSTAMPED");
    check(keys(m1, m2, emit, true) == "r20:2" && keys(m1, m2, emit, false) == "r20:2",
          N + "owned again: the scope's own PM4 its own");
  }
  end(n);
}

// ---- thread B takes the device inside this thread's open bracket ----
void transition() {
  const char* n = "transition";
  begin(n);
  const std::string N = std::string(n) + ": ";
  const int un = unstampedRow(), emit = row(kEmit), tex = row(kTexLock);
  const Mark m0 = mark();
  uint32_t stolen = 0, back = 0;
  within(kEmit, [&] {
    within(kTexLock, [] { unbracketed(11u); });  // this thread's bytes, the device its own at both cuts
    onB([&] {
      stolen = ownership(kAcquire);  // B takes the device (no wait) and writes
      unbracketed(12u);
    });
    back = ownership(kAcquire);  // this thread takes it back: a nested scope, a cut either side
    unbracketed(13u);            // this thread's again
  });
  rt::closeNow();
  rt::apply();
  const Mark m1 = mark();
  // an ownership change no scope of this thread saw: the bracket opens while B owns the device, B writes, and the
  // device is this thread's again by the close (owned at the closing cut only)
  rt::wr(kDev + 0x2A08u, kThreadIdB);
  within(kEmit, [] {
    onB([] { unbracketed(14u); });
    rt::wr(kDev + 0x2A08u, rt::kThreadId);
  });
  rt::closeNow();
  rt::apply();
  const Mark m2 = mark();
  logRows(n, m0, m2);
  check(stolen == kThreadIdB && back == rt::kThreadId,
        N + "precondition: 820F3618 on thread B took the device (its identity), then this thread took it back");
  if (g_mode.on) {
    check(count(m0, m1, un, 256 + 0x20, true) == 2u && count(m0, m1, un, 256 + 0x20, false) == 2u,
          N + "B's PM4 written while B owned the device is not this bracket's: UNSTAMPED");
    check(keys(m0, m1, tex, true) == "r20:2" && keys(m0, m1, emit, true) == "r20:2" &&
              keys(m0, m1, emit, false) == "r20:4",
          N + "this thread's PM4 before B took the device and after it took it back: its scopes' own");
    check(count(m1, m2, un, 256 + 0x20, true) == 2u && keys(m1, m2, emit, false).empty(),
          N + "owned at the closing cut only: the piece spanning B's writes is dropped (UNSTAMPED)");
  }
  end(n);
}

// ---- a 820FCDF0 block handed out inside a scope on thread B (not the owner) ----
void foreignAlloc() {
  const char* n = "foreign-alloc";
  begin(n);
  const uint32_t a = kBuf + 0x40000u;
  uint32_t b1 = 0, b2 = 0;
  const Mark m0 = mark();
  onB([&] { within(kEmit, [&] { b1 = allocAt(a, 8u); }); });
  const std::vector<sr::native::Interval> s1 = stampsIn(a, a + 32u);
  runBlock(b1, 15u);
  const Mark m1 = mark();
  within(kTexLock, [&] { b2 = allocAt(a, 8u); });  // the control: the owner thread
  const std::vector<sr::native::Interval> s2 = stampsIn(a, a + 32u);
  runBlock(b2, 16u);
  const Mark m2 = mark();
  logRows(n, m0, m2);
  const std::string N = std::string(n) + ": ";
  check(b1 == a && b2 == a, N + "precondition: both hand-outs are the same memory");
  if (g_mode.on) {
    check(s1.empty() && keys(m0, m1, unstampedRow(), true) == "r20:2" && moved(m0, m1, true).size() == 1u,
          N + "a block thread B (not the device's owner) allocates inside its scope is not stamped: UNSTAMPED");
    check(s2.size() == 1u && s2[0].s.entry == row(kTexLock) && keys(m1, m2, row(kTexLock), true) == "r20:2" &&
              moved(m1, m2, true).size() == 1u,
          N + "the owner thread's block: its allocating scope's");
  }
  end(n);
}

// ---- ownership changes the cuts' endpoint reads cannot see ----
// Thread B takes the device (820F3618 on B) and writes its PM4 (no bracket of its own: unstamped bytes).
uint32_t g_stolen = 0;
void takeoverB(uint32_t k) {
  onB([k] {
    g_stolen = ownership(kAcquire);
    unbracketed(k);
  });
}
// 820F3618's before-hook (its scope entered - the entry cut and the epoch move done - its body not yet run): armed,
// on the suite's thread, B takes the device and writes before this thread's Acquire body takes it back.
std::atomic<bool> g_armAcquire{false};
void acquireBefore(void*) {
  if (!t_onB && g_armAcquire.exchange(false)) takeoverB(31u);
}
// The census's cut read gap (native_brackets setCutGapHook: after the owner read, before the cursor read): armed, on
// the suite's thread, B takes the device and writes there.
std::atomic<bool> g_armGap{false};
void cutGap() {
  if (!t_onB && g_armGap.exchange(false)) takeoverB(33u);
}

// A -> B -> A between two cuts that both read this thread: B takes the device after this thread's Acquire entry cut.
void interleave() {
  const char* n = "interleave";
  begin(n);
  const std::string N = std::string(n) + ": ";
  const int un = unstampedRow(), emit = row(kEmit), tex = row(kTexLock), acq = row(kAcquire);
  g_stolen = 0;
  const Mark m0 = mark();
  uint32_t back = 0;
  within(kEmit, [&] {
    within(kTexLock, [] { unbracketed(30u); });  // this thread's, before
    g_armAcquire = true;
    back = ownership(kAcquire);  // entry cut: this thread's; B takes it and writes; the body takes it back; exit cut
    unbracketed(32u);            // this thread's, after
  });
  const bool fired = !g_armAcquire.exchange(false);
  rt::closeNow();
  rt::apply();
  const Mark m1 = mark();
  logRows(n, m0, m1);
  check(fired && g_stolen == kThreadIdB && back == rt::kThreadId,
        N + "precondition: B took the device between this thread's Acquire entry cut and its body, which took it back");
  if (g_mode.on) {
    check(count(m0, m1, un, 256 + 0x20, true) == 2u && count(m0, m1, un, 256 + 0x20, false) == 2u &&
              keys(m0, m1, acq, true).empty(),
          N + "both cuts read this thread, the epoch moved between them: B's PM4 is not this bracket's (UNSTAMPED)");
    check(keys(m0, m1, tex, true) == "r20:2" && keys(m0, m1, emit, true) == "r20:2" &&
              keys(m0, m1, emit, false) == "r20:4",
          N + "this thread's PM4 before and after the change: its scopes' own");
  }
  end(n);
}

// A takeover between a cut's owner read and its cursor read: the cursor already holds B's bytes.
void readGap() {
  const char* n = "read-gap";
  begin(n);
  const std::string N = std::string(n) + ": ";
  const int un = unstampedRow(), emit = row(kEmit), tex = row(kTexLock);
  g_stolen = 0;
  const Mark m0 = mark();
  uint32_t back = 0;
  bool fired = false;
  within(kEmit, [&] {
    within(kTexLock, [] {
      unbracketed(34u);
      g_armGap = true;  // the next cut on this thread (this scope's leave): B takes the device inside its reads
    });
    fired = !g_armGap.exchange(false);
    if (!fired) takeoverB(33u);  // the census off: no cut ran - the same takeover at the same point
    back = ownership(kAcquire);  // this thread takes it back
    unbracketed(35u);
  });
  rt::closeNow();
  rt::apply();
  const Mark m1 = mark();
  logRows(n, m0, m1);
  check(fired == g_mode.on && g_stolen == kThreadIdB && back == rt::kThreadId,
        N + "precondition: B took the device inside the scope's leave cut (census on; right after it, census off), this "
            "thread took it back");
  if (g_mode.on) {
    check(count(m0, m1, un, 256 + 0x20, true) == 4u && keys(m0, m1, tex, true).empty() &&
              keys(m0, m1, emit, false) == "r20:2",
          N + "the cut's piece holding B's PM4 is dropped whole (UNSTAMPED), the piece after the take-back its own");
  }
  end(n);
}

// An owner change no scope sees (a raw dev+0x2A08 write - no guest writer does this; the suite does): B's stamped
// PM4 inside this thread's piece is never stamped over.
void rawOwner() {
  const char* n = "raw-owner";
  begin(n);
  const std::string N = std::string(n) + ": ";
  const int un = unstampedRow(), emit = row(kEmit), vb = row(kVbLock);
  const Mark m0 = mark();
  within(kEmit, [] {
    within(kTexLock, [] { unbracketed(36u); });               // this thread's
    rt::wr(kDev + 0x2A08u, kThreadIdB);                        // raw: B owns the device, the epoch does not move
    onB([] { within(kVbLock, [] { unbracketed(37u); }); });  // B, the owner, stamps its own PM4
    rt::wr(kDev + 0x2A08u, rt::kThreadId);                     // raw: this thread's again at its closing cut
    unbracketed(38u);
  });
  rt::closeNow();
  rt::apply();
  const Mark m1 = mark();
  logRows(n, m0, m1);
  if (g_mode.on) {
    check(keys(m0, m1, vb, true) == "r20:2" && keys(m0, m1, vb, false) == "r20:2",
          N + "B's stamped PM4 stays B's: another thread's piece never stamps over it");
    check(count(m0, m1, un, 256 + 0x20, true) == 2u && keys(m0, m1, emit, false) == "r20:2",
          N + "the piece that would overlap B's stamp is refused whole (this thread's PM4 in it UNSTAMPED)");
  }
  end(n);
}

// ---- the kick's pump run by the production driver 82110840 on thread B ----
struct Lap {
  bool ran = false, ok = true;
  uint32_t dwords = 0;                   // ring dwords B wrote
  std::vector<sr::native::Stamp> ring;   // the stamps of those 820FD218 wrote (its scope's ring stamps)
  std::vector<sr::native::Stamp> other;  // the rest: the bin-select dwords 82110140 writes to the ring directly
};
Lap g_lap;
bool pumpViaDriver(uint32_t ctx) {
  bool ok = false;
  onB([&] {
    g_ringCalls.clear();
    const uint32_t w0 = rt::rd(kDev + 0x2A44u);
    rt::wr(kPumpCtx, rt::kS);
    rt::wr(rt::kS + 0x58u, ctx);  // the kick's list pending (as the ISR's DPC leaves it), then the pump thread's driver
    PPCContext c = rt::context();
    c.r3.u64 = kPumpCtx;
    rt::call(kDriver, c);
    const uint32_t w1 = rt::rd(kDev + 0x2A44u);
    ok = c.r3.u32 == 0u && rt::rd(rt::kS) == 0u;  // EndSync's end, the driver lock released
    g_lap.dwords += (w1 - w0) & rt::kRingMask;
    for (uint32_t i = w0, guard = 0; i != w1 && guard <= rt::kRingMask; i = (i + 1u) & rt::kRingMask, guard++) {
      bool via = false;  // inside one of 820FD218's ranges (the fixture's ring does not wrap)
      for (const auto& r : g_ringCalls) via = via || (i >= r.first && i < r.second);
      (via ? g_lap.ring : g_lap.other).push_back(sr::native::takeRingStamp(i & rt::kRingMask));
    }
  });
  g_lap.ran = true;
  g_lap.ok = g_lap.ok && ok;
  rt::g_pumps++;
  return ok;
}
void pumpDriver() {
  const char* n = "pump-driver";
  begin(n);
  rt::g_sync = false;  // the CP lags the producer; the pump runs from the CP's interrupt (rt::apply)
  rt::g_pumpRunner = pumpViaDriver;
  const std::string N = std::string(n) + ": ";
  const int drv = row(kDriver), un = unstampedRow();
  Mark m[3];
  Lap laps[2];
  uint32_t owners[2] = {0u, 0u};
  bool armed[2] = {false, false};
  m[0] = mark();
  for (int lap = 0; lap < 2; lap++) {
    g_lap = Lap{};
    rt::zBegin();
    runProducer(invalCase());
    rt::zEnd();                         // the SDK kick: its INTERRUPT queues the pump
    if (lap == 1) ownership(kRelease);  // lap 2: the device released while the pump runs
    owners[lap] = rt::rd(kDev + 0x2A08u);
    g_ownerKey = 20u + uint32_t(lap);  // the owner thread writes while B's ring write is open
    rt::drain();
    armed[lap] = g_ownerKey.exchange(0u) == 0u;
    if (lap == 1) ownership(kAcquire);
    rt::closeNow();  // the owner thread's bytes reach the CP
    rt::drain();
    laps[lap] = std::move(g_lap);
    m[lap + 1] = mark();
  }
  rt::g_pumpRunner = nullptr;
  logRows(n, m[0], m[2]);
  for (int lap = 0; lap < 2; lap++) {
    const std::string L = N + (lap ? "lap 2 (the device released, dev+0x2A08 = 0): "
                                   : "lap 1 (the device the owner thread's): ");
    const Lap& p = laps[lap];
    size_t drvStamps = 0;
    for (const sr::native::Stamp& s : p.ring) drvStamps += s.entry == drv && s.tid == g_b->hostTid.load();
    size_t otherStamped = 0;
    for (const sr::native::Stamp& s : p.other) otherStamped += s.entry >= 0;
    std::printf("[CENSUS] %s lap %d: driver ran %d ok %d, %u ring dwords (%zu through 820FD218, %zu stamped the "
                "driver's on thread B; first owner %s tid %u; %zu written directly, %zu of them stamped), owner %08X, "
                "owner write %d\n",
                n, lap + 1, p.ran ? 1 : 0, p.ok ? 1 : 0, p.dwords, p.ring.size(), drvStamps,
                p.ring.empty() || p.ring[0].entry < 0 ? "-" : rowName(p.ring[0].entry).c_str(),
                p.ring.empty() ? 0u : p.ring[0].tid, p.other.size(), otherStamped, owners[lap], armed[lap] ? 1 : 0);
    check(p.ran && p.ok && !p.ring.empty() && armed[lap] && owners[lap] == (lap ? 0u : rt::kThreadId),
          L + "precondition: the pump ran through the production driver on thread B (completed, the ring written, "
              "the owner thread wrote meanwhile)");
    if (!g_mode.on) continue;
    bool ringOwned = !p.ring.empty();
    for (const sr::native::Stamp& s : p.ring)
      ringOwned = ringOwned && s.entry == drv && s.tid == g_b->hostTid.load();
    check(ringOwned, L + "the pump's ring writes are the driver's (owner 82110840, thread B), whoever owns the device");
    check(!p.other.empty() && otherStamped == 0u,
          L + "incomplete attribution kept visible: the bin-select dwords 82110140 writes to the ring directly (not "
              "through 820FD218) carry no ring stamp (a CP read counts them RING_UNSTAMPED), never anyone's");
    check(count(m[lap], m[lap + 1], un, 256 + 0x20, true) == 2u &&
              count(m[lap], m[lap + 1], un, 256 + 0x20, false) == 2u && keys(m[lap], m[lap + 1], drv, false).empty() &&
              keys(m[lap], m[lap + 1], drv, true).empty(),
          L + "the owner thread's PM4 written inside the driver's bracket is not the driver's: UNSTAMPED");
  }
  end(n);
}

// ---- 821075E0's alignment pad (p1.flush_residue, native/flush-residue-2; Astra round 1): the census keys a type-2 NOP
// whose next dword is a shader-constant run's header (type-0, 4k registers inside [0x2388, 0x23A0): the run 821075E0
// writes right after its pad) as o6 (cov::kConstRunPadKey) and every other type-2 NOP as o1 - an ALU run's pad, the
// first of two NOPs, a one-register write's, the window's last dword (no next dword). Read in place outside any bracket:
// all UNSTAMPED. tools/perf/transport_census.py gives o6 on an S1 flush line to p1.row3_construns, o1 stays residue. ----
void constRunPad() {
  const char* n = "construns-pad";
  begin(n);
  const uint32_t w[] = {
      0x80000000u, 0x00032388u, 1u, 2u, 3u, 4u,                                        // pad + run 0x2388 x4: o6, r23
      0x80000000u, 0x00034000u, 5u, 6u, 7u, 8u,                                        // pad + ALU run 0x4000: o1, r40
      0x80000000u, 0x80000000u, 0x0007238Cu, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u,           // two NOPs + run: o1, o6, r23
      0x80000000u, 0x0000A388u, 9u,                                                    // pad + one-register write: o1, r23
      0x80000000u};                                                                    // the last dword: o1
  constexpr uint32_t kN = sizeof(w) / 4u;
  const uint32_t blk = allocAt(kBuf + 0x40000u, kN);  // outside any bracket
  for (uint32_t i = 0; i < kN; i++) rt::wr(blk + 4u * i, w[i]);
  const Mark m0 = mark();
  g_engine->execute(rt::host(blk), sizeof(w), 1);
  const Mark m1 = mark();
  logRows(n, m0, m1);
  const std::string N = std::string(n) + ": ";
  check(cov::constRunHeader(0x00032388u) && cov::constRunHeader(0x00172388u) && !cov::constRunHeader(0x00032380u) &&
            !cov::constRunHeader(0x0002238Cu) && !cov::constRunHeader(0x0000A388u) && !cov::constRunHeader(0x0007239Cu) &&
            !cov::constRunHeader(0xC0003C00u) && !cov::constRunHeader(0u),
        N + "the witness: 4k registers from [0x2388, 0x23A0), type-0, not one-register (S1's 0x2380 run, an odd count, "
            "past 0x23A0, a type-3 header, no next dword: no)");
  if (g_mode.on)
    check(keys(m0, m1, unstampedRow(), true) == "r23:3,r40:1,o1:4,o6:2" &&
              keys(m0, m1, unstampedRow(), false) == "r23:3,r40:1,o1:4,o6:2" && moved(m0, m1, true).size() == 1u,
          N + "a NOP right before a construns run header is o6, every other type-2 NOP o1 (UNSTAMPED, nothing else)");
  end(n);
}

// ---- the frame boundary (native/composed-census; tcmp2's dark census1): every present the CP applies ends exactly one
// census frame (one F line). An XE_SWAP packet ends it at the packet hook (pm4cov::packet3, 0x64) and the present that
// packet applies does not end another; SR_NATIVE_SWAP_BLOCK's kRkSwap record - under the composed transport EVERY
// present, with no 0x64 packet anywhere - ends it at Pm4Engine::applySwap (pm4cov::presented), through the live record
// executor (nativeRecord, cpSegment's) and the oracle loop (nativeRecordOracle) alike; the record rendered back to PM4
// (the CPPM4 split) is the packet again: one frame, not two; an XE_SWAP without its 'SWAP' signature ends a frame, applies
// no present and does not swallow the next record's frame. Before presented() the records ended none: the census counted
// packets but never reported (no F, CUM, [PM4_COVMAP] or [NATIVE_BRACKETS] line in 5,640 presents). With the census off
// nothing is counted and the CP presents exactly the same. ----
uint64_t g_cpPresents = 0;  // the CP's present callback (cp.swap) calls
void fxSwap(void*, uint32_t, uint32_t, uint32_t) { ++g_cpPresents; }
struct FrameMark {
  uint64_t frames, presents, presentEnds, cp;
};
FrameMark frameMark() { return {cov::framesEnded(), cov::presentsApplied(), cov::presentEnds(), g_cpPresents}; }
// The step a -> b moved frames / present-ended frames / the CP's presents by exactly these (the census on), or moved no
// census counter and the CP's presents by the same (the census off).
bool frameStep(const FrameMark& a, const FrameMark& b, uint64_t frames, uint64_t presentEnds, uint64_t cp) {
  std::printf("[CENSUS] swap-frames: frames +%llu presents +%llu presentEnds +%llu cp +%llu\n",
              (unsigned long long)(b.frames - a.frames), (unsigned long long)(b.presents - a.presents),
              (unsigned long long)(b.presentEnds - a.presentEnds), (unsigned long long)(b.cp - a.cp));
  if (!g_mode.on)
    return b.frames == a.frames && b.presents == a.presents && b.presentEnds == a.presentEnds && b.cp - a.cp == cp;
  return b.frames - a.frames == frames && b.presentEnds - a.presentEnds == presentEnds && b.presents - a.presents == cp &&
         b.cp - a.cp == cp;
}
void swapFrames() {
  const char* n = "swap-frames";
  begin(n);
  sr::gfx::Pm4Engine& e = *g_engine;
  e.cp.swap = fxSwap;
  g_cpPresents = 0;
  ensureSwapBlockHandlers();
  const std::string N = std::string(n) + ": ";
  const uint32_t blk = allocAt(kBuf + 0x44000u, 64u);  // outside any bracket
  auto putPm4 = [&](const uint32_t* w, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) rt::wr(blk + 4u * i, w[i]);
  };
  // VdSwap's block payload (kRkSwap): no fetch run, front buffer, width, height
  const uint32_t payload[10] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0x1EA00000u, 1280u, 720u};
  uint32_t rec[11] = {recTag(kRkSwap, 10u, 0u)};
  std::memcpy(rec + 1, payload, sizeof(payload));

  const FrameMark m0 = frameMark();
  const uint32_t pkt[] = {0xC0036400u, 0x53574150u, 0x1EA00000u, 1280u, 720u};  // XE_SWAP {'SWAP', fb, w, h}
  putPm4(pkt, 5u);
  e.execute(rt::host(blk), sizeof(pkt), 1);
  const FrameMark m1 = frameMark();
  check(frameStep(m0, m1, 1u, 0u, 1u),
        N + "an XE_SWAP packet: one frame (at the packet), its present not a second (census off: none, the same present)");

  sr::gfx::Pm4Engine::NativeRecordStats st{};
  const bool live = e.nativeRecord(rec, rec + 11, st, false);
  const FrameMark m2 = frameMark();
  check(live && !st.bad && !e.nativeRecordFailed && frameStep(m1, m2, 1u, 1u, 1u),
        N + "a kRkSwap record (live executor, no 0x64 packet): one frame, at its present (census off: none)");

  sr::gfx::Pm4Engine::NativeRecordStats so{};
  e.nativeRecordOracle(rec, rec + 11, so, false);
  const FrameMark m3 = frameMark();
  check(!so.bad && frameStep(m2, m3, 1u, 1u, 1u),
        N + "a kRkSwap record (the oracle loop): one frame, at its present (census off: none)");

  uint8_t pm4[256] = {};
  swapBlockRender(kRkSwap, payload, 10u, pm4, sizeof(pm4));
  uint32_t rendered[64];
  for (uint32_t i = 0; i < 64u; i++) rendered[i] = be32(pm4 + 4u * i);
  putPm4(rendered, 64u);
  e.execute(rt::host(blk), sizeof(pm4), 1);
  const FrameMark m4 = frameMark();
  check(rendered[0] == 0xC0036400u && frameStep(m3, m4, 1u, 0u, 1u),
        N + "the record rendered to PM4 (CPPM4): the XE_SWAP packet's one frame, not two (census off: none)");

  const uint32_t unsigned_[] = {0xC0036400u, 0u, 0u, 0u, 0u};  // XE_SWAP without its 'SWAP' signature
  putPm4(unsigned_, 5u);
  e.execute(rt::host(blk), sizeof(unsigned_), 1);
  const FrameMark m5 = frameMark();
  sr::gfx::Pm4Engine::NativeRecordStats s2{};
  const bool live2 = e.nativeRecord(rec, rec + 11, s2, false);
  const FrameMark m6 = frameMark();
  check(frameStep(m4, m5, 1u, 0u, 0u) && live2 && !s2.bad && frameStep(m5, m6, 1u, 1u, 1u),
        N + "an unsigned XE_SWAP: one frame, no present; the record after it still ends its own frame (census off: none)");
  if (g_mode.on)
    check(m6.frames - m0.frames == 6u && m6.presents - m0.presents == 5u && m6.cp - m0.cp == 5u,
          N + "frames = every present the CP applied + the unsigned XE_SWAP's (6 = 5 + 1)");
  e.cp.swap = nullptr;
  end(n);
}

// ---- the graceful quit's pm4_cov render (native/census-quit; tbk2): kernel/quit.cpp prints every counter family once
// (the terminal snapshot) and again (the verify's capture-only re-render) and requires every integer field of every
// re-rendered line equal (quit::snapshotDiff). The CP parked inside a frame: the snapshot's pm4cov::terminal folds that
// partial frame in (its tail line + F line), the re-render finds nothing to fold. The armed witness printed BEFORE the
// fold read frames=N in the snapshot and N+1 in the re-render: tbk2's two terminal snapshots FAILED verify
// ("counters-moved-after-snapshot | [PM4_COV] frames(4648->4649)"). With the census off it prints nothing. ----
void captureLine(void* user, const char* message) {
  std::string& s = *static_cast<std::string*>(user);
  s += message;
  s += '\n';
}
std::string renderTerminal(bool suppress) {  // the quit's capture of the pm4_cov family's printer (kernel/quit.cpp)
  std::string out;
  sr::Log::setThreadCapture(captureLine, &out, suppress);
  cov::terminal("terminal");
  sr::Log::setThreadCapture(nullptr, nullptr, false);
  return out;
}
void terminalRender() {
  const char* n = "terminal-render";
  begin(n);
  sr::gfx::Pm4Engine& e = *g_engine;
  e.cp.swap = fxSwap;
  const std::string N = std::string(n) + ": ";
  // a frame ended at an XE_SWAP, then the start of the next one the drain stopped inside (two register writes)
  const uint32_t w[] = {0xC0036400u, 0x53574150u, 0x1EA00000u, 1280u, 720u,
                        0x00002000u, 0xC0DE0001u, 0x00002000u, 0xC0DE0002u};
  constexpr uint32_t kN = sizeof(w) / 4u;
  const uint32_t blk = allocAt(kBuf + 0x46000u, kN);  // outside any bracket
  for (uint32_t i = 0; i < kN; i++) rt::wr(blk + 4u * i, w[i]);
  e.execute(rt::host(blk), sizeof(w), 1);
  const uint64_t f0 = cov::framesEnded();
  const std::string snap = renderTerminal(false);
  const uint64_t f1 = cov::framesEnded();
  const std::string again = renderTerminal(true);
  const uint64_t f2 = cov::framesEnded();
  char diff[900] = "";
  int fields = 0;
  const int moved = sr::kernel::quit::snapshotDiff(snap, again, diff, sizeof(diff), &fields);
  std::printf("[CENSUS] %s: snapshot %zu bytes, re-render %zu bytes, frames %llu -> %llu -> %llu, %d fields compared, "
              "%d moved%s%s\n", n, snap.size(), again.size(), (unsigned long long)f0, (unsigned long long)f1,
              (unsigned long long)f2, fields, moved, moved ? ": " : "", diff);
  if (g_mode.on) {
    check(f1 == f0 + 1u && f2 == f1 && snap.find(" tail frame=") != std::string::npos &&
              again.find(" tail frame=") == std::string::npos,
          N + "the snapshot folds the drained partial frame in (its tail + F line), the re-render folds nothing");
    check(moved == 0 && fields > 0 && snap.find("[PM4_COV] armed terminal ") != std::string::npos &&
              again.find("[PM4_COV] armed terminal ") != std::string::npos,
          N + "the verify's re-render prints every line with its snapshot fields (quit::snapshotDiff: 0 moved), the armed "
              "witness in both");
  } else {
    check(moved == 0 && f2 == f0 && snap.find("[PM4_COV]") == std::string::npos,
          N + "census off: pm4cov::terminal prints and folds nothing");
  }
  e.cp.swap = nullptr;
  end(n);
}

}  // namespace cen

int runNativeCensusTest(const char* mode) {
  namespace K = sr::kernel;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  {  // a bare run re-runs itself with exactly the runner's configuration for the mode (tools/difftest/suite_bare.inc);
     // the configured marker: SR_AB_TOGGLE=SR_NATIVE_BRACKETS (a latched gate, never registered with the A/B: inert)
    const bool off = std::strcmp(mode, "gate-off") == 0, res = std::strcmp(mode, "residue") == 0,
               seg = std::strcmp(mode, "segmentless-off") == 0;
    const wchar_t* const gates = res ? L"1" : L"0";
    const bare::Conf conf = {{L"SR_PM4_RING", L"0"}, {L"SR_PM4_RING_EXEC", L"0"}, {L"SR_PM4_CP", L"0"},
                             {L"SR_ALLOW_FORCING", L"0"}, {L"SR_AB_TOGGLE", L"SR_NATIVE_BRACKETS"},
                             {L"SR_NATIVE_SEGMENTLESS", seg ? L"0" : L"1"}, {L"SR_NATIVE_FLUSH", L"1"},
                             {L"SR_NATIVE_IDLE_WAIT", gates}, {L"SR_NATIVE_INVAL_RANGE", gates},
                             {L"SR_NATIVE_LOCK_COHER", gates}, {L"SR_NATIVE_BRACKET", L"0"}, {L"SR_NATIVE_TRANSPORT", L"0"},
                             {L"SR_NATIVE_BRACKETS", off ? L"0" : L"1"}};
    int code = 0;
    if (bare::handled("NATIVE_CENSUS_TEST", "--native-census-test", mode, L"SR_NATIVE_BRACKETS", conf, code))
      return code;
  }
  cen::Mode& m = cen::g_mode;
  m.name = mode;
  m.on = std::strcmp(mode, "gate-off") != 0;
  m.res = std::strcmp(mode, "residue") == 0;
  m.seg = g_segmentless;
  const bool wantSeg = std::strcmp(mode, "segmentless-off") != 0;
  K::registerStubs();
  sr::guest::initializeDispatchTable();
  K::kpcrBindCurrent(rt::kStack, rt::kStack - 0x10000u, 3);
  cen::check(sr::native::g_bracketsOn == m.on && sr::gfx::pm4cov::on() == m.on && g_on &&
                 (std::strcmp(mode, "gate-off") == 0 || g_segmentless == wantSeg) &&
                 idleWaitOn() == m.res && invalRangeOn() == m.res && lockCoherOn() == m.res &&
                 sr::abt::reg("SR_NATIVE_IDLE_WAIT") < 0 && sr::abt::reg("SR_NATIVE_INVAL_RANGE") < 0 &&
                 sr::abt::reg("SR_NATIVE_LOCK_COHER") < 0 && sr::abt::reg("SR_NATIVE_SEGMENTLESS") < 0,
             std::string("requested mode ") + mode + ": brackets / census, record mode, segmentless, residue gates");
  cen::check(PPC_LOOKUP_FUNC(K::getGuestBase(), 0x820FCDF0u) == sub_820FCDF0 &&
                 PPC_LOOKUP_FUNC(K::getGuestBase(), 0x820FCF90u) == sub_820FCF90 &&
                 PPC_LOOKUP_FUNC(K::getGuestBase(), 0x820FD8C0u) == sub_820FD8C0,
             "production dispatch resolves the two allocators' N0 shims and the close");
  cen::check(cen::row(cen::kInval) >= 0 && cen::row(cen::kVbLock) >= 0 && cen::row(cen::kIdle) >= 0 &&
                 cen::row(cen::kTexLock) >= 0 && cen::row(cen::kEmit) >= 0 && cen::row(0x820FCDF0u) >= 0,
             "the producers and GpuCmdBufAllocate are entry-table rows");
  cen::check(cen::row(cen::kAcquire) >= 0 && cen::row(cen::kRelease) >= 0 && cen::row(cen::kDriver) >= 0 &&
                 cen::row(cen::kRingWr) >= 0 &&
                 PPC_LOOKUP_FUNC(K::getGuestBase(), cen::kAcquire) == sub_820F3618 &&
                 PPC_LOOKUP_FUNC(K::getGuestBase(), cen::kRelease) == sub_820F3658 &&
                 PPC_LOOKUP_FUNC(K::getGuestBase(), cen::kDriver) == sub_82110840,
             "the ownership pair and the pump driver are rows, production dispatch resolves their scoped functions");
  cen::check(rt::host(cen::kStackB - 0x10000u) != nullptr && rt::host(cen::kStackB - 4u) != nullptr && cen::startB(),
             "thread B: its own guest stack, its own KPCR (another thread identity)");
  if (cen::failed) {
    std::printf("[NATIVE_CENSUS_TEST] %d passed, %d failed\n", cen::passed, cen::failed);
    return 1;
  }
  const size_t index = (rt::kAllocCb - PPC_CODE_BASE) >> 2;
  PPCFunc* const saved = K::g_dispatchTable[index];
  K::g_dispatchTable[index] = rt::residueTestAllocate;
  const uint32_t pcr = K::kpcrCurrent(), oldKt = rt::rd(pcr + 0x100u);
  rt::wr(pcr + 0x100u, rt::kKt);
  if (!g_arena) g_arena = static_cast<uint8_t*>(VirtualAlloc(nullptr, kRecArena, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  cen::check(setScopeHook(0x820FD218u, cen::ringBefore, cen::ringAfter), "the emulated CP hooks 820FD218's N0 scope");
  cen::check(setScopeHook(cen::kAcquire, cen::acquireBefore, nullptr),
             "the suite hooks 820F3618's scope (B's takeover after its entry cut)");
  sr::native::setCutGapHook(cen::cutGap);
  std::thread(cen::watchdog).detach();

  cen::producers();
  cen::switchTail();
  cen::segReuse();
  cen::blockReuse();
  if (g_segmentless) cen::lagging();  // a copy outliving its memory exists only on the segmentless transport
  cen::blockScopes();
  cen::constRunPad();
  cen::swapFrames();
  cen::terminalRender();
  cen::foreign();
  cen::zeroOwner();
  cen::transition();
  cen::foreignAlloc();
  cen::interleave();
  cen::readGap();
  cen::rawOwner();
  {  // the Z-pass cycle: EndSync's event imports answer at once, BRACKET's begin has a consumer (the emulated CP)
    auto& registry = K::ExportRegistry::instance();
    const K::ExportFn origSet = registry.getExport("KeSetEvent"), origReset = registry.getExport("KeResetEvent"),
                      origWait = registry.getExport("KeWaitForSingleObject");
    registry.registerExport("KeSetEvent", rt::evNoop);
    registry.registerExport("KeResetEvent", rt::evNoop);
    registry.registerExport("KeWaitForSingleObject", rt::evNoop);
    setSegmentPublicationObserver(rt::observeNoop);
    cen::zpassKick();
    cen::pumpDriver();
    setSegmentPublicationObserver(nullptr);
    registry.registerExport("KeSetEvent", origSet);
    registry.registerExport("KeResetEvent", origReset);
    registry.registerExport("KeWaitForSingleObject", origWait);
  }

  if (sr::native::g_bracketsOn) sr::native::bracketsCensus("census-test");
  setScopeHook(0x820FD218u, nullptr, nullptr);
  setScopeHook(cen::kAcquire, nullptr, nullptr);
  sr::native::setCutGapHook(nullptr);
  rt::wr(pcr + 0x100u, oldKt);
  K::g_dispatchTable[index] = saved;
  std::printf("[NATIVE_CENSUS_TEST] digest %016llX\n", (unsigned long long)cen::g_digest);
  std::printf("[NATIVE_CENSUS_TEST] %d passed, %d failed\n", cen::passed, cen::failed);
  return cen::failed ? 1 : 0;
}
}  // namespace sr::native::flush
