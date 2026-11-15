// Included by kernel/native_flush.cpp only in difftest (SR_DIFFTEST).
// Run with SR_NATIVE_SEGMENTLESS=1, SR_NATIVE_FLUSH_CPPM4=0; no game/GPU worker.
#include <condition_variable>

namespace sr::native::flush {
namespace {
struct SegmentTestDelay {
  std::mutex lock;
  std::condition_variable changed;
  bool entered = false, release = false, expired = false;
  static void flush(void* user) {
    auto& self = *static_cast<SegmentTestDelay*>(user);
    std::unique_lock<std::mutex> lk(self.lock);
    if (self.entered) return;
    self.entered = true;
    self.changed.notify_all();
    self.expired = !self.changed.wait_for(lk, std::chrono::seconds(3), [&] { return self.release; });
  }
};
}

int runSegmentOwnershipTest() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  int passed = 0, failed = 0;
  auto check = [&](bool ok, const char* label) {
    std::printf("%s%s\n", ok ? "  ok  " : "FAIL: ", label);
    ok ? ++passed : ++failed;
  };
  if (!g_segmentless || g_cpPm4 || g_arena || !g_segById.empty()) {
    std::printf("FAIL: isolated segmentless native fixture environment required\n");
    return 1;
  }
  g_arena = static_cast<uint8_t*>(VirtualAlloc(nullptr, kRecArena, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!g_arena) return 2;
  constexpr uint32_t va = 0x706C0000u, phys = 0x006C0000u, count = 4u;
  uint8_t stores[8]{};
  auto engine = [&] {
    sr::gfx::Pm4Engine eng;
    eng.submitDraws = false;
    eng.user = stores;
    eng.physToHost = [](void* data, uint32_t address) -> uint8_t* {
      return address < 8u ? static_cast<uint8_t*>(data) + address : nullptr;
    };
    return eng;
  };
  auto emitted = [&](uint64_t pos, uint32_t value) {
    std::lock_guard<SpinLock> lk(g_mu);
    uint8_t* slot = g_arena + (pos % kRecArena);
    const RecHdr h{kRecMagic, uint32_t(sizeof(RecHdr) + 20u), pos, 0u, 0u};
    const uint32_t body[] = {recTag(kRkFence, 0u, 0u), 2u, value, 6u, value + 1u};
    std::memcpy(slot, &h, sizeof(h));
    std::memcpy(slot + sizeof(h), body, sizeof(body));
    g_head = pos + h.bytes;
    std::lock_guard<std::mutex> sl(g_segMu);
    return SegFoot{va, va + 16u, pos, captureSegmentRecord(slot, h.bytes, pos)};
  };
  auto value = [&] { return be32(stores); };
  auto live = [&](uint64_t id) {
    std::lock_guard<std::mutex> lk(g_segMu);
    return g_segById.count(id) != 0;
  };
  auto complete = [&](const std::shared_ptr<SegParts>& sp) {
    std::lock_guard<std::mutex> lk(g_segMu);
    return sp->complete();
  };
  auto arenaExec = [&](uint64_t pos) {
    RecHdr h;
    std::lock_guard<SpinLock> lk(g_mu);
    std::memcpy(&h, g_arena + (pos % kRecArena), sizeof(h));
    return h.exec;
  };

  // Exercise the actual writer, including its capture-site wiring. A local
  // device cursor is sufficient here: a segmentless fence writes no guest PM4.
  // Overwrite its arena slot before building the consumer's parts.
  {
    uint8_t device[64]{};
    Item items[1]{};
    Item fence{};
    fence.pkt = 1u;
    fence.rk = kRkFence;
    fence.fixedBytes = 40u;
    fence.body[0] = 2u; fence.body[1] = 201u;
    fence.body[2] = 6u; fence.body[3] = 202u;
    Driver writer;
    writer.d = device;
    writer.items = items;
    writer.c = writer.spanStart = va - 4u;
    {
      std::lock_guard<SpinLock> lk(g_mu);
      writer.add(fence);
      writer.closeSpan();
    }
    const SegFoot foot = g_segFeet.empty() ? SegFoot{} : g_segFeet.front();
    check(g_segFeet.size() == 1u && foot.lo == va && foot.hi == va + 40u &&
              writer.nItems == 0u && writer.spanStart == writer.c && be32(device + 40u) == writer.c,
          "production writer closes one fence span and publishes its footprint");
    {
      std::lock_guard<std::mutex> lk(g_segMu);
      const auto audit = auditFindLocked(foot.pos);  // step 6 I12: either arm (the ring / the map)
      check(foot.record && audit && audit == foot.record,
            "production writer capture site owns bytes and registers reclaim audit");
    }
    emitted(kRecArena, 707u);
    auto written = buildParts(va, va + 40u, {foot});
    check(written->parts.size() == 1u && written->parts[0].pos == foot.pos &&
              written->parts[0].record == foot.record,
          "production writer footprint builds the exact consumer part");
    registerParts(phys, 10u, written, false);
    t_segPublishing = written;
    check(segmentPublishId(phys, 10u) == written->id,
          "production writer consumer publishes its exact direct id");
    auto consumer = engine();
    check(cpSegment(consumer, written->id, phys, 10u) && value() == 201u && be32(stores + 4u) == 202u,
          "production writer consumer executes emitted fence after arena reuse");
    check(foot.record && foot.record->completed.load() && complete(written) && arenaExec(kRecArena) == 0u,
          "production writer completion belongs to captured record");
    check(recordExecutedForAudit(foot.pos, g_arena + (foot.pos % kRecArena)),
          "production writer reclaim audit survives overwritten header");
    // Reset this isolated writer case before the existing registry fixtures.
    g_segFeet.clear(); g_segById.clear(); auditClearLocked();
    g_head = g_reclaim = 0u;
    g_cpTail.store(0u);
    std::memset(stores, 0, sizeof(stores));
  }

  // Capture at emission, then overwrite the arena BEFORE the segment closes.
  auto first = emitted(0u, 101u);
  emitted(kRecArena, 909u);
  auto sp = buildParts(va, va + 16u, {first});
  registerParts(phys, count, sp, true, 2u);
  check(sp->parts.size() == 1u && sp->parts[0].record == first.record,
        "closed part retains the exact record captured at emission");
  check(segmentPublishId(phys, count) == sp->id, "first deferred pass publishes its registered id");
  auto eng = engine();
  check(cpSegment(eng, sp->id, phys, count), "first deferred pass consumes the production part list");
  check(value() == 101u && be32(stores + 4u) == 102u,
        "arena reuse before close cannot replace the first consumer's bytes");
  check(arenaExec(kRecArena) == 0u, "old owned execution cannot mark a reused arena slot complete");
  check(first.record->completed.load() && !complete(sp),
        "first completed pass still owes the unpublished second pass");
  segmentBracketBegin(); segmentBracketBegin();
  check(live(sp->id), "bracket expiry retains an entry which owes its second pass");
  std::memset(stores, 0, sizeof(stores));
  check(segmentPublishId(phys, count) == sp->id, "second deferred pass publishes the retained id");
  check(cpSegment(eng, sp->id, phys, count), "second deferred pass consumes the production part list");
  check(value() == 101u && be32(stores + 4u) == 102u,
        "second consumer uses emission bytes after arena wrap and delayed publication");
  check(complete(sp) && sp->started == 2u && sp->finished == 2u,
        "two-pass entry completes only after both published consumers return");
  segmentBracketBegin();
  check(!live(sp->id), "completed old bracket entry is collected");

  for (bool deferred : {false, true}) {
    auto foot = emitted(kRecArena + (deferred ? 256u : 128u), 303u);
    auto delayed = buildParts(va, va + 16u, {foot});
    registerParts(phys, count, delayed, deferred);
    if (!deferred) t_segPublishing = delayed;
    check(segmentPublishId(phys, count) == delayed->id, "delayed fixture publishes exactly one consumer");
    SegmentTestDelay delay;
    auto slow = engine();
    slow.cp.user = &delay; slow.cp.flush = SegmentTestDelay::flush;
    bool consumed = false;
    std::atomic<bool> returned{false};
    std::thread worker([&] {
      consumed = cpSegment(slow, delayed->id, phys, count);
      returned.store(true, std::memory_order_release);
    });
    bool entered;
    {
      std::unique_lock<std::mutex> lk(delay.lock);
      entered = delay.changed.wait_for(lk, std::chrono::seconds(2), [&] { return delay.entered; });
    }
    check(entered, "consumer reaches the bounded real execution callback");
    check(!complete(delayed), "started consumer is not complete while execution is blocked");
    check(!foot.record->completed.load() && arenaExec(foot.pos) == 0u,
          "record and arena audit remain unfinished while execution is blocked");
    segmentBracketBegin(); segmentBracketBegin();
    check(live(delayed->id), "registry retains a consumer through delayed execution and bracket expiry");
    g_mu.lock();  // A real producer can hold this while waiting for queue space.
    {
      std::lock_guard<std::mutex> lk(delay.lock);
      delay.release = true;
      delay.changed.notify_all();
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!returned.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < until)
      std::this_thread::yield();
    const bool finishedWithoutProducer = returned.load(std::memory_order_acquire);
    g_mu.unlock();
    worker.join();
    check(finishedWithoutProducer, "consumer can finish while producer owns the arena lock");
    check(consumed && !delay.expired && complete(delayed) && foot.record->completed.load() && arenaExec(foot.pos) == 0u,
          "execution completion is published after the last part returns");
    check(recordExecutedForAudit(foot.pos, g_arena + (foot.pos % kRecArena)),
          "record reclaim audit reads stable completion independent of arena header");
    if (deferred) segmentBracketBegin();
    check(!live(delayed->id), "finished consumer can be removed from the registry");
  }

  // A published and completed first pass is not disposable under capacity
  // pressure: the second pass has not been published. Empty fillers represent
  // other producer-owned entries whose publication is also still owed.
  auto pressuredFoot = emitted(kRecArena + 512u, 505u);
  auto pressured = buildParts(va, va + 16u, {pressuredFoot});
  registerParts(phys, count, pressured, true, 2u);
  check(segmentPublishId(phys, count) == pressured->id && cpSegment(eng, pressured->id, phys, count),
        "capacity fixture completes its first production consumer");
  std::vector<uint64_t> owedIds{pressured->id};
  for (size_t n = 0; n < kSegDeferredCap; ++n) {
    auto owed = std::make_shared<SegParts>();
    registerParts(phys + 32u + uint32_t(n) * 4u, count, owed, true, 2u);
    owedIds.push_back(owed->id);
  }
  check(live(pressured->id), "capacity collection retains an entry which owes its second pass");
  check(g_segDeferred.size() == kSegDeferredCap + 1u && g_segCapPressure.get() != 0u,
        "capacity pressure retains and reports all live unpublished consumers");
  bool everyIdRetained;
  {
    std::lock_guard<std::mutex> lk(g_segMu);
    everyIdRetained = std::all_of(owedIds.begin(), owedIds.end(), [&](uint64_t id) {
      const auto it = g_segById.find(id);
      return it != g_segById.end() && it->second->id == id && it->second->passes == 2u && !it->second->complete();
    });
  }
  check(everyIdRetained, "capacity pressure does not discard unfinished passes");
  std::memset(stores, 0, sizeof(stores));
  check(segmentPublishId(phys, count) == pressured->id && cpSegment(eng, pressured->id, phys, count) && value() == 505u,
        "owed second pass remains publishable after capacity collection");
  check(g_cpBad.get() == 0u && g_cpTorn.get() == 0u, "owned production consumers report no bad or torn records");

  g_segDeferred.clear(); g_segById.clear(); auditClearLocked();
  VirtualFree(g_arena, 0, MEM_RELEASE); g_arena = nullptr;
  std::printf("[NATIVE_SEGMENT_OWNERSHIP_TEST] %d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
}  // namespace sr::native::flush
