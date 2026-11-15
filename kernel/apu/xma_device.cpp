// iter-463 audio NATIVE: XMA decoder device + the XMA* exports. Contract: xma_device.hpp.
//
// Ported from Xenia Canary (BSD-3-Clause, see docs/THIRD_PARTY.md), oracle build
// canary_experimental@0bd090dbe: src/xenia/apu/xma_decoder.cc (Copyright 2022 Ben Vanik) and
// src/xenia/kernel/xboxkrnl/xboxkrnl_audio_xma.cc (Copyright 2022 Ben Vanik). Released under the
// BSD license. Line references: xd = xma_decoder.cc, xk = xboxkrnl_audio_xma.cc.
#include "kernel/apu/xma_device.hpp"

#include "kernel/export_registry.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/mm_phys.hpp"
#include "app/log.hpp"

#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sr::kernel::apu {

namespace {

#if SR_ORACLE
std::atomic<int> g_newMode{-1};
#endif
std::atomic<int> g_dedMode{-1};

bool envFlag(const char* name, bool dflt) {
  const char* e = std::getenv(name);
  if (!e || !e[0]) return dflt;
  return e[0] != '0';
}

uint64_t nowMs() {
  using namespace std::chrono;
  return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool logN(std::atomic<uint32_t>& c, uint32_t first) {
  const uint32_t n = c.fetch_add(1, std::memory_order_relaxed) + 1;
  return n <= first || (n & (n - 1)) == 0;
}
std::atomic<uint32_t> g_kickTimeoutLogs{0}, g_badCtxLogs{0}, g_aliasLogs{0}, g_callLogs{0},
    g_unallocLogs{0}, g_noteLogs{0};

inline uint32_t be32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline void storeBe32(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}

}  // namespace

#if SR_ORACLE
bool xmaNewEnabled() {
  const int t = g_newMode.load(std::memory_order_relaxed);
  if (t >= 0) return t != 0;
  static const bool v = [] {
    const char* e = std::getenv("SR_XMA_NEW");   // iter-463 rolls: default ON (=0 keeps the A/B)
    return !e || e[0] != '0';
  }();
  return v;
}
#endif  // SR_ORACLE=0: xmaNewEnabled() is the constant true of kernel/apu/xma_api.hpp (C1: no SR_XMA_NEW selector)

bool xmaDedicatedThreadEnabled() {
  const int t = g_dedMode.load(std::memory_order_relaxed);
  if (t >= 0) return t != 0;
  static const bool v = envFlag("SR_XMA_DEDICATED_THREAD", true);
  return v;
}

void xmaSetEnabledForTest(int newMode, int dedicatedMode) {
#if SR_ORACLE
  g_newMode.store(newMode, std::memory_order_relaxed);
#else
  (void)newMode;  // the release configuration has no legacy mode to select
#endif
  g_dedMode.store(dedicatedMode, std::memory_order_relaxed);
}

XmaDevice& XmaDevice::instance() {
  static XmaDevice* d = new XmaDevice();   // never destroyed (worker may outlive statics)
  return *d;
}

XmaDevice::XmaDevice() : memo_(new std::atomic<uint32_t>[0x40000]) {
  for (uint32_t i = 0; i < 0x40000; ++i) memo_[i].store(0, std::memory_order_relaxed);
}

// xd:130-193 (Setup), deferred to the first XMACreateContext.
bool XmaDevice::ensureInit() {
  // Only the SR_XMA_NEW exports reach this; the check keeps the gate visible at the one site that
  // writes guest memory outside a context (the array zero-fill below).
  if (!xmaNewEnabled()) return false;
  std::lock_guard<std::mutex> lk(mu_);
  if (inited_) return true;
  guestBase_ = getGuestBase();
  guestSize_ = getGuestSize();
  const uint32_t va = guestPhysAlloc(kXmaContextArrayBytes, 0x1000);
  if (!va) {
    SR_LOG_AUDIO("[XMA_INIT] FAILED: guestPhysAlloc(0x%X) returned 0", kXmaContextArrayBytes);
    return false;
  }
  uint8_t* h0 = guestVAToHost(va);
  uint8_t* h1 = guestVAToHost(va + kXmaContextArrayBytes - 1);
  if (!h0 || !h1 || h1 - h0 != (ptrdiff_t)(kXmaContextArrayBytes - 1) ||
      !hostRangeReadable(h0, kXmaContextArrayBytes)) {
    SR_LOG_AUDIO("[XMA_INIT] FAILED: context array va=%08X is not one backed host span", va);
    return false;
  }
  const uint32_t pa = xeGetPhysicalAddress(va);
  if (pa == UINT32_MAX ||
      xeGetPhysicalAddress(va + kXmaContextArrayBytes - 1) != pa + kXmaContextArrayBytes - 1) {
    SR_LOG_AUDIO("[XMA_INIT] FAILED: context array va=%08X pa=%08X spans two heaps", va, pa);
    return false;
  }
  std::memset(h0, 0, kXmaContextArrayBytes);
  arrayVA_ = va;
  arrayPA_ = pa;
  dedicated_ = xmaDedicatedThreadEnabled();
  int withDecoder = 0;
  for (uint32_t i = 0; i < kXmaContextCount; ++i) {
    auto dec = makeFfmpegXmaFrameDecoder();
    withDecoder += dec ? 1 : 0;
    contexts_[i].Setup(i, this, va + i * 64, std::move(dec));
  }
  inited_ = true;
  SR_LOG_AUDIO("[XMA_INIT] context array va=%08X pa=%08X (reg 0x600 value) x%u, decoders %d/%u, "
               "dedicated_thread=%d (xma_decoder=new, oracle 0bd090dbe)",
               va, pa, kXmaContextCount, withDecoder, kXmaContextCount, dedicated_ ? 1 : 0);
  if (dedicated_) startWorker();
  return true;
}

void XmaDevice::initForTest(IXmaMemory* mem, uint32_t arrayVA, uint32_t arrayPA,
                            XmaFrameDecoderFactory factory, bool dedicated) {
  shutdownForTest();
  std::lock_guard<std::mutex> lk(mu_);
  testMem_ = mem;
  arrayVA_ = arrayVA;
  arrayPA_ = arrayPA;
  dedicated_ = dedicated;
  for (uint32_t i = 0; i < kXmaContextCount; ++i) {
    contexts_[i].set_is_allocated(false);
    contexts_[i].set_is_enabled(false);
    contexts_[i].Setup(i, this, arrayVA + i * 64, factory ? factory() : nullptr);
    allocated_[i] = false;
    for (int s = 0; s < 3; ++s) {
      side_[i][s].store(0);
      sideBytes_[i][s].store(0);
    }
  }
  for (uint32_t i = 0; i < 0x40000; ++i) memo_[i].store(0, std::memory_order_relaxed);
  inited_ = true;
  if (dedicated_) startWorker();
}

void XmaDevice::shutdownForTest() {
  if (worker_.joinable()) {
    running_.store(false);
    {
      std::lock_guard<std::mutex> lk(workMu_);
      workPending_ = true;
    }
    workCv_.notify_all();
    worker_.join();
  }
  std::lock_guard<std::mutex> lk(mu_);
  inited_ = false;
}

void XmaDevice::startWorker() {
  running_.store(true);
  worker_ = std::thread([this] { workerMain(); });
}

// xd:195-217 WorkerThreadMain. A pure host thread: it never enters guest code.
void XmaDevice::workerMain() {
#ifdef _WIN32
  SetThreadDescription(GetCurrentThread(), L"XMA Decoder");
#endif
  if (guestBase_) setGuestMemory(guestBase_, guestSize_);
  while (running_.load(std::memory_order_acquire)) {
    bool did_work = false;
    for (uint32_t n = 0; n < kXmaContextCount; n++) {
      XmaContextNew& c = contexts_[n];
      if (c.Work()) {
        c.PublishDone(c.last_work_seq());
        works_.fetch_add(1, std::memory_order_relaxed);
        did_work = true;
      }
    }
    if (did_work) {
      { std::lock_guard<std::mutex> lk(doneMu_); }
      doneCv_.notify_all();
      continue;
    }
    {
      std::unique_lock<std::mutex> lk(workMu_);
      workCv_.wait_for(lk, std::chrono::milliseconds(1000),
                       [this] { return workPending_ || !running_.load(); });
      workPending_ = false;
    }
    // iter-463 review: no logging on the worker. The [XMA_SUM] line used to be written HERE,
    // between the kick's wake-up and the scan; the harness logger is synchronous (global
    // spinlock, console WriteFile, periodic fflush), so a slow write held up the decode the
    // kicker was waiting for. b_long1's only two [XMA_KICK_TIMEOUT]s each started 250 / 314 ms
    // after an [XMA_SUM] stamp (~170 summaries in 1440 s). Xenia's decoder thread does no
    // per-wake logging. The summary is written by the kicking guest thread instead (after its
    // kick completed, writeRegister), i.e. still every 10 s while the device is active.
  }
}

// xd:244-252
int XmaDevice::contextId(uint32_t va) const {
  if (!arrayVA_ || va < arrayVA_ || va > arrayVA_ + kXmaContextArrayBytes - 1) return -1;
  if (va & 0x3F) return -1;
  return (int)((va - arrayVA_) >> 6);
}

// xd:254-265 (BitMap::Acquire takes the lowest free index)
uint32_t XmaDevice::allocateContext() {
  std::lock_guard<std::mutex> lk(mu_);
  for (uint32_t i = 0; i < kXmaContextCount; ++i) {
    if (!allocated_[i]) {
      allocated_[i] = true;
      contexts_[i].set_is_allocated(true);
      creates_.fetch_add(1, std::memory_order_relaxed);
      return contexts_[i].guest_ptr();
    }
  }
  createFails_.fetch_add(1, std::memory_order_relaxed);
  SR_LOG_AUDIO("[XMA] create exhausted: all %u contexts allocated", kXmaContextCount);
  return 0;
}

// xd:267-275
void XmaDevice::releaseContext(uint32_t va) {
  const int id = contextId(va);
  if (id < 0) {
    badCtx_.fetch_add(1, std::memory_order_relaxed);
    if (logN(g_badCtxLogs, 8))
      SR_LOG_AUDIO("[XMA] XMAReleaseContext(%08X): not a context of the array at %08X", va,
                   arrayVA_);
    return;
  }
  std::lock_guard<std::mutex> lk(mu_);
  contexts_[id].Release();
  allocated_[id] = false;
  for (int s = 0; s < 3; ++s) {
    side_[id][s].store(0, std::memory_order_relaxed);
    sideBytes_[id][s].store(0, std::memory_order_relaxed);
  }
  releases_.fetch_add(1, std::memory_order_relaxed);
}

// xd:277-283
bool XmaDevice::blockOnContext(uint32_t va, bool poll) {
  const int id = contextId(va);
  if (id < 0) return true;
  return contexts_[id].Block(poll);
}

void XmaDevice::kickGroup(uint32_t baseId, uint32_t mask) {
  std::array<uint64_t, 32> seqs{};
  uint32_t waitMask = 0;
  uint32_t v = mask;
  while (v) {
    const uint32_t bit = (uint32_t)std::countr_zero(v);
    const uint32_t context_id = baseId + bit;
    v &= v - 1;
    if (context_id >= kXmaContextCount) continue;
    XmaContextNew& context = contexts_[context_id];
    kicks_.fetch_add(1, std::memory_order_relaxed);
    seqs[bit] = context.BumpKickSeq();
    context.Enable();
    if (!dedicated_) {
      if (context.Work()) works_.fetch_add(1, std::memory_order_relaxed);
      context.PublishDone(context.last_work_seq());
    } else if (context.is_allocated()) {
      waitMask |= 1u << bit;
    } else {
      unallocKicks_.fetch_add(1, std::memory_order_relaxed);
      if (logN(g_unallocLogs, 8))
        SR_LOG_AUDIO("[XMA_KICK_UNALLOC] ctx=%u (Xenia would block forever here)", context_id);
    }
  }
  if (dedicated_ && waitMask) {
    {
      std::lock_guard<std::mutex> lk(workMu_);
      workPending_ = true;
    }
    workCv_.notify_one();
    // Block until the worker finishes, so the game sees updated context data.
    uint32_t remaining = waitMask;
    while (remaining) {
      const uint32_t bit = (uint32_t)std::countr_zero(remaining);
      remaining &= remaining - 1;
      XmaContextNew& context = contexts_[baseId + bit];
      std::unique_lock<std::mutex> lk(doneMu_);
      if (!doneCv_.wait_for(lk, std::chrono::milliseconds(250),
                            [&] { return context.done_seq() >= seqs[bit]; })) {
        timeouts_.fetch_add(1, std::memory_order_relaxed);
        if (logN(g_kickTimeoutLogs, 32))
          SR_LOG_AUDIO("[XMA_KICK_TIMEOUT] ctx=%u seq=%llu done=%llu enabled=%d lr=%08X", baseId + bit,
                       (unsigned long long)seqs[bit], (unsigned long long)context.done_seq(),
                       context.is_enabled() ? 1 : 0, testMem_ ? 0u : getCurrentGuestLR());
      }
    }
  }
  maybeSummary();
}

void XmaDevice::lockGroup(uint32_t baseId, uint32_t mask) {
  while (mask) {
    const uint32_t context_id = baseId + (uint32_t)std::countr_zero(mask);
    mask &= mask - 1;
    if (context_id >= kXmaContextCount) continue;
    XmaContextNew& context = contexts_[context_id];
    locks_.fetch_add(1, std::memory_order_relaxed);
    context.Disable();
    // Ensure the worker isn't mid-processing this context.
    context.Block(false);
  }
}

void XmaDevice::clearGroup(uint32_t baseId, uint32_t mask) {
  while (mask) {
    const uint32_t context_id = baseId + (uint32_t)std::countr_zero(mask);
    mask &= mask - 1;
    if (context_id >= kXmaContextCount) continue;
    clears_.fetch_add(1, std::memory_order_relaxed);
    contexts_[context_id].Clear();
  }
}

// Native front end: the application always addresses the context directly.
int XmaDevice::nativeId(uint32_t ctxVA, const char* op) {
  const int id = contextId(ctxVA);
  if (id < 0) {
    badCtx_.fetch_add(1, std::memory_order_relaxed);
    if (logN(g_badCtxLogs, 8))
      SR_LOG_AUDIO("[XMA] %s for ctx %08X: not in the context array at %08X", op, ctxVA, arrayVA_);
  }
  return id;
}
void XmaDevice::kickContext(uint32_t ctxVA) {
  const int id = nativeId(ctxVA, "kick");
  if (id >= 0) kickGroup(uint32_t(id) & ~31u, 1u << (uint32_t(id) & 31u));
}
void XmaDevice::lockContext(uint32_t ctxVA) {
  const int id = nativeId(ctxVA, "lock");
  if (id >= 0) lockGroup(uint32_t(id) & ~31u, 1u << (uint32_t(id) & 31u));
}
void XmaDevice::clearContext(uint32_t ctxVA) {
  const int id = nativeId(ctxVA, "clear");
  if (id >= 0) clearGroup(uint32_t(id) & ~31u, 1u << (uint32_t(id) & 31u));
}

void XmaDevice::notePhys(uint32_t ctxVA, int slot, uint32_t pa, uint32_t va, uint32_t bytes) {
  const int id = contextId(ctxVA);
  if (id < 0 || slot < 0 || slot > 2) return;
  if (!pa || !va || !bytes || pa == UINT32_MAX) {
    side_[id][slot].store(0, std::memory_order_release);
    sideBytes_[id][slot].store(0, std::memory_order_release);
    return;
  }
  // Validate once here (guest thread, ~4/s in gameplay), never on the decode path.
  uint8_t* h = testMem_ ? testMem_->TranslateVirtual(va) : nullptr;
  if (!testMem_) {
    uint8_t* h0 = guestVAToHost(va);
    uint8_t* h1 = guestVAToHost(va + bytes - 1);
    h = (h0 && h1 && h1 - h0 == (ptrdiff_t)(bytes - 1) && hostRangeReadableCached(h0, bytes)) ? h0
                                                                                            : nullptr;
  }
  if (!h) {
    if (logN(g_noteLogs, 8))
      SR_LOG_AUDIO("[XMA_PHYS_MISS] ctx=%d slot=%d va=%08X bytes=%u is not one readable host span",
                   id, slot, va, bytes);
    side_[id][slot].store(0, std::memory_order_release);
    sideBytes_[id][slot].store(0, std::memory_order_release);
    return;
  }
  sideBytes_[id][slot].store(bytes, std::memory_order_release);
  side_[id][slot].store(((uint64_t)pa << 32) | va, std::memory_order_release);
  // Page memo (fallback for a PA the side table no longer names).
  for (uint32_t off = 0; off < bytes; off += 0x1000) {
    const uint32_t ppage = (pa + off) >> 12;
    if (ppage >= 0x40000) break;
    const uint32_t want = ((va + off) >> 12) + 1;
    const uint32_t prev = memo_[ppage].exchange(want, std::memory_order_acq_rel);
    if (prev && prev != want) {
      physAliases_.fetch_add(1, std::memory_order_relaxed);
      if (logN(g_aliasLogs, 16))
        SR_LOG_AUDIO("[XMA_PHYS_ALIAS] pa page %08X was va %08X, now va %08X (ctx %d slot %d)",
                     ppage << 12, (prev - 1) << 12, (want - 1) << 12, id, slot);
    }
  }
}

uint8_t* XmaDevice::TranslateVirtual(uint32_t va) {
  if (testMem_) return testMem_->TranslateVirtual(va);
  uint8_t* h0 = guestVAToHost(va);
  uint8_t* h1 = guestVAToHost(va + 63);
  return (h0 && h1 && h1 - h0 == 63) ? h0 : nullptr;
}

uint8_t* XmaDevice::TranslatePhysical(uint32_t ctx_id, uint32_t pa, uint32_t bytes) {
  if (ctx_id >= kXmaContextCount || !bytes) return nullptr;
  uint32_t va = 0;
  for (int s = 0; s < 3 && !va; ++s) {
    const uint64_t e = side_[ctx_id][s].load(std::memory_order_acquire);
    if (e && (uint32_t)(e >> 32) == pa && bytes <= sideBytes_[ctx_id][s].load(std::memory_order_acquire))
      va = (uint32_t)e;
  }
  if (!va) {
    // Page memo: every page of the span must name the same contiguous VA run.
    const uint32_t first = pa >> 12;
    const uint32_t last = (uint32_t)(((uint64_t)pa + bytes - 1) >> 12);
    if (last >= 0x40000) return nullptr;
    const uint32_t v0 = memo_[first].load(std::memory_order_acquire);
    if (!v0) {
      memoMisses_.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
    for (uint32_t p = first + 1; p <= last; ++p) {
      if (memo_[p].load(std::memory_order_acquire) != v0 + (p - first)) {
        memoMisses_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
      }
    }
    va = ((v0 - 1) << 12) | (pa & 0xFFF);
  }
  if (testMem_) return testMem_->TranslateVirtual(va);
  uint8_t* h0 = guestVAToHost(va);
  uint8_t* h1 = guestVAToHost(va + bytes - 1);
  return (h0 && h1 && h1 - h0 == (ptrdiff_t)(bytes - 1)) ? h0 : nullptr;
}

XmaStats XmaDevice::stats() const {
  XmaStats s{};
  s.creates = creates_.load();
  s.createFails = createFails_.load();
  s.releases = releases_.load();
  s.inits = inits_.load();
  s.kicks = kicks_.load();
  s.works = works_.load();
  s.locks = locks_.load();
  s.clears = clears_.load();
  s.timeouts = timeouts_.load();
  s.unallocKicks = unallocKicks_.load();
  s.physAliases = physAliases_.load();
  s.badCtx = badCtx_.load();
  for (const auto& c : contexts_) {
    s.frames += c.frames_decoded();
    s.decodeFails += c.decode_failures();
    s.physMisses += c.phys_misses();
  }
  s.physMisses += memoMisses_.load();
  return s;
}

// A fixed-rate line (every 10 s while the device is active): counts are safe to quote.
void XmaDevice::maybeSummary() {
  const uint64_t now = nowMs();
  uint64_t last = lastSummaryMs_.load(std::memory_order_relaxed);
  if (last == 0) {
    lastSummaryMs_.compare_exchange_strong(last, now);
    return;
  }
  if (now - last < 10000) return;
  if (!lastSummaryMs_.compare_exchange_strong(last, now)) return;
  const XmaStats s = stats();
  int live = 0;
  for (uint32_t i = 0; i < kXmaContextCount; ++i) live += contexts_[i].is_allocated() ? 1 : 0;
  SR_LOG_AUDIO("[XMA_SUM] creates=%llu fails=%llu releases=%llu live=%d inits=%llu kicks=%llu "
               "works=%llu locks=%llu clears=%llu frames=%llu decode_fails=%llu timeouts=%llu "
               "unalloc_kicks=%llu phys_miss=%llu phys_alias=%llu bad_ctx=%llu",
               (unsigned long long)s.creates, (unsigned long long)s.createFails,
               (unsigned long long)s.releases, live, (unsigned long long)s.inits,
               (unsigned long long)s.kicks, (unsigned long long)s.works,
               (unsigned long long)s.locks, (unsigned long long)s.clears,
               (unsigned long long)s.frames, (unsigned long long)s.decodeFails,
               (unsigned long long)s.timeouts, (unsigned long long)s.unallocKicks,
               (unsigned long long)s.physMisses, (unsigned long long)s.physAliases,
               (unsigned long long)s.badCtx);
}

// ─── the exports (xk) ───────────────────────────────────────────────────────────────────────

namespace {

XmaDevice& dev() { return XmaDevice::instance(); }

uint8_t* hostSpan(uint32_t va, uint32_t bytes) {
  uint8_t* h0 = guestVAToHost(va);
  uint8_t* h1 = guestVAToHost(va + bytes - 1);
  return (h0 && h1 && h1 - h0 == (ptrdiff_t)(bytes - 1)) ? h0 : nullptr;
}

// The context as Xenia's lpvoid_t context_ptr: the device's translation (test memory in tests).
uint8_t* ctxHost(uint32_t ctxVA) {
  if (dev().contextId(ctxVA) < 0) {
    if (logN(g_badCtxLogs, 8))
      SR_LOG_AUDIO("[XMA] export on %08X: not a context of the array at %08X", ctxVA,
                   dev().contextArrayVA());
    return nullptr;
  }
  return dev().TranslateVirtual(ctxVA);
}

uint32_t physOf(uint32_t va) { return xeGetPhysicalAddress(va); }

void logCall(const char* name, uint32_t a, uint32_t b, uint32_t c) {
  if (logN(g_callLogs, 48))
    SR_LOG_AUDIO("[XMA] %s(%08X, %08X, %08X)", name, a, b, c);
}

}  // namespace

// xk:58-68
uint32_t xmaExportCreate(uint32_t outPtrVA) {
  logCall("XMACreateContext", outPtrVA, 0, 0);
  uint32_t context_ptr = dev().ensureInit() ? dev().allocateContext() : 0;
  if (uint8_t* out = guestVAToHost(outPtrVA)) storeBe32(out, context_ptr);
  return context_ptr ? 0u : kStatusNoMemory;
}

// xk:70-76
uint32_t xmaExportRelease(uint32_t ctxVA) {
  dev().releaseContext(ctxVA);
  return 0;
}

// xk:117-189
uint32_t xmaExportInitialize(uint32_t ctxVA, uint32_t initVA) {
  logCall("XMAInitializeContext", ctxVA, initVA, 0);
  uint8_t* ctx = ctxHost(ctxVA);
  const uint8_t* init = hostSpan(initVA, kXmaContextInitSize);
  if (!ctx || !init) return kXEFalse;
  dev().inits_.fetch_add(1, std::memory_order_relaxed);
  // Input buffers may be null (buffer 1 in 415607D4).
  const uint32_t in0_va = be32(init + 0x00), in0_count = be32(init + 0x04);
  const uint32_t in1_va = be32(init + 0x08), in1_count = be32(init + 0x0C);
  const uint32_t read_offset = be32(init + 0x10);
  const uint32_t out_va = be32(init + 0x14), out_blocks = be32(init + 0x18);
  const uint32_t sdc = be32(init + 0x20), channel_count = be32(init + 0x24);
  const uint32_t sample_rate = be32(init + 0x28);
  const uint32_t loop_start = be32(init + 0x2C), loop_end = be32(init + 0x30);
  const uint8_t loop_count = init[0x34], loop_sf_end = init[0x35], loop_sf_skip = init[0x36];

  uint32_t in0_pa = 0, in1_pa = 0;
  if (in0_va) {
    in0_pa = physOf(in0_va);
    if (in0_pa == UINT32_MAX) {
      SR_LOG_AUDIO("[XMA] XMAInitializeContext: Invalid input buffer 0 virtual address %08X", in0_va);
      return kXEFalse;
    }
  }
  if (in1_va) {
    in1_pa = physOf(in1_va);
    if (in1_pa == UINT32_MAX) {
      SR_LOG_AUDIO("[XMA] XMAInitializeContext: Invalid input buffer 1 virtual address %08X", in1_va);
      return kXEFalse;
    }
  }
  const uint32_t out_pa = physOf(out_va);
  if (out_pa == UINT32_MAX) {
    SR_LOG_AUDIO("[XMA] XMAInitializeContext: Invalid output buffer virtual address %08X", out_va);
    return kXEFalse;
  }

  std::memset(ctx, 0, sizeof(XMA_CONTEXT_DATA));
  XMA_CONTEXT_DATA context(ctx);
  context.input_buffer_0_ptr = in0_pa;
  context.input_buffer_0_packet_count = in0_count;
  context.input_buffer_1_ptr = in1_pa;
  context.input_buffer_1_packet_count = in1_count;
  context.input_buffer_read_offset = read_offset;
  context.output_buffer_ptr = out_pa;
  context.output_buffer_block_count = out_blocks;
  // context.work_buffer = context_init->work_buffer;  // ?  (Xenia leaves it out too)
  context.subframe_decode_count = sdc;
  context.is_stereo = channel_count >= 1;   // (sic) Xenia xk:175
  context.sample_rate = sample_rate;
  context.loop_start = loop_start;
  context.loop_end = loop_end;
  context.loop_count = loop_count;
  context.loop_subframe_end = loop_sf_end;
  context.loop_subframe_skip = loop_sf_skip;
  context.Store(ctx);

  // Side table: the exact VAs converted here (bitfield-truncated counts, as stored).
  dev().notePhys(ctxVA, 0, in0_pa, in0_va, context.input_buffer_0_packet_count * 2048u);
  dev().notePhys(ctxVA, 1, in1_pa, in1_va, context.input_buffer_1_packet_count * 2048u);
  dev().notePhys(ctxVA, 2, out_pa, out_va, context.output_buffer_block_count * 256u);

#if defined(SR_DIFFTEST)
  if (!xmaNativeFrontEnabled())
    dev().storeIndexedRegister(0x1A80, ctxVA);
  else
#endif
    dev().clearContext(ctxVA);
  return 0;
}

// xk:227-247, 268-288
uint32_t xmaExportSetInputBuffer(int which, uint32_t ctxVA, uint32_t bufVA, uint32_t packetCount) {
  const uint32_t pa = physOf(bufVA);
  if (pa == UINT32_MAX) {
    SR_LOG_AUDIO("[XMA] XMASetInputBuffer%d: Invalid buffer virtual address %08X", which, bufVA);
    return kXEFalse;
  }
  uint8_t* ctx = ctxHost(ctxVA);
  if (!ctx) return kXEFalse;
  XMA_CONTEXT_DATA context(ctx);
  if (which == 0) {
    context.input_buffer_0_ptr = pa;
    context.input_buffer_0_packet_count = packetCount;
  } else {
    context.input_buffer_1_ptr = pa;
    context.input_buffer_1_packet_count = packetCount;
  }
  context.Store(ctx);
  dev().notePhys(ctxVA, which, pa, bufVA,
                 (which == 0 ? context.input_buffer_0_packet_count
                             : context.input_buffer_1_packet_count) * 2048u);
  return 0;
}

// xk:357-360
uint32_t xmaExportEnable(uint32_t ctxVA) {
#if defined(SR_DIFFTEST)
  if (!xmaNativeFrontEnabled())
    dev().storeIndexedRegister(0x1940, ctxVA);
  else
#endif
    dev().kickContext(ctxVA);
  return 0;
}

// xk:364-375
uint32_t xmaExportDisable(uint32_t ctxVA, uint32_t wait) {
  uint32_t result = 0;   // X_E_SUCCESS
#if defined(SR_DIFFTEST)
  if (!xmaNativeFrontEnabled())
    dev().storeIndexedRegister(0x1A40, ctxVA);
  else
#endif
    dev().lockContext(ctxVA);
  if (!dev().blockOnContext(ctxVA, !wait)) {
    result = kXEFalse;
  }
  return result;
}

namespace {

template <typename F>
uint32_t withCtx(uint32_t ctxVA, F f) {
  uint8_t* ctx = ctxHost(ctxVA);
  if (!ctx) return 0;
  return f(ctx);
}

}  // namespace

void registerXmaExports(ExportRegistry& reg) {
  if (!xmaNewEnabled()) return;
  using u32 = uint32_t;
  reg.registerExport("XMACreateContext", +[](u32 r3, u32, u32, u32, u32, u32) {
    return xmaExportCreate(r3);
  });
  reg.registerExport("XMAReleaseContext", +[](u32 r3, u32, u32, u32, u32, u32) {
    return xmaExportRelease(r3);
  });
  reg.registerExport("XMAInitializeContext", +[](u32 r3, u32 r4, u32, u32, u32, u32) {
    return xmaExportInitialize(r3, r4);
  });
  reg.registerExport("XMASetInputBuffer0", +[](u32 r3, u32 r4, u32 r5, u32, u32, u32) {
    return xmaExportSetInputBuffer(0, r3, r4, r5);
  });
  reg.registerExport("XMASetInputBuffer1", +[](u32 r3, u32 r4, u32 r5, u32, u32, u32) {
    return xmaExportSetInputBuffer(1, r3, r4, r5);
  });
  reg.registerExport("XMAEnableContext", +[](u32 r3, u32, u32, u32, u32, u32) {
    return xmaExportEnable(r3);
  });
  reg.registerExport("XMADisableContext", +[](u32 r3, u32 r4, u32, u32, u32, u32) {
    return xmaExportDisable(r3, r4);
  });
  // xk:251-256, 292-297, 309-314
  reg.registerExport("XMAIsInputBuffer0Valid", +[](u32 r3, u32, u32, u32, u32, u32) {
    return withCtx(r3, [](uint8_t* c) { return (u32)XMA_CONTEXT_DATA(c).input_buffer_0_valid; });
  });
  reg.registerExport("XMAIsInputBuffer1Valid", +[](u32 r3, u32, u32, u32, u32, u32) {
    return withCtx(r3, [](uint8_t* c) { return (u32)XMA_CONTEXT_DATA(c).input_buffer_1_valid; });
  });
  reg.registerExport("XMAIsOutputBufferValid", +[](u32 r3, u32, u32, u32, u32, u32) {
    return withCtx(r3, [](uint8_t* c) { return (u32)XMA_CONTEXT_DATA(c).output_buffer_valid; });
  });
  // xk:258-265, 299-306, 316-323
  reg.registerExport("XMASetInputBuffer0Valid", +[](u32 r3, u32, u32, u32, u32, u32) {
    return withCtx(r3, [](uint8_t* c) {
      XMA_CONTEXT_DATA d(c);
      d.input_buffer_0_valid = 1;
      d.Store(c);
      return (u32)0;
    });
  });
  reg.registerExport("XMASetInputBuffer1Valid", +[](u32 r3, u32, u32, u32, u32, u32) {
    return withCtx(r3, [](uint8_t* c) {
      XMA_CONTEXT_DATA d(c);
      d.input_buffer_1_valid = 1;
      d.Store(c);
      return (u32)0;
    });
  });
  reg.registerExport("XMASetOutputBufferValid", +[](u32 r3, u32, u32, u32, u32, u32) {
    return withCtx(r3, [](uint8_t* c) {
      XMA_CONTEXT_DATA d(c);
      d.output_buffer_valid = 1;
      d.Store(c);
      return (u32)0;
    });
  });
  // xk:326-349
  reg.registerExport("XMAGetOutputBufferReadOffset", +[](u32 r3, u32, u32, u32, u32, u32) {
    return withCtx(r3, [](uint8_t* c) { return (u32)XMA_CONTEXT_DATA(c).output_buffer_read_offset; });
  });
  reg.registerExport("XMASetOutputBufferReadOffset", +[](u32 r3, u32 r4, u32, u32, u32, u32) {
    return withCtx(r3, [r4](uint8_t* c) {
      XMA_CONTEXT_DATA d(c);
      d.output_buffer_read_offset = r4;
      d.Store(c);
      return (u32)0;
    });
  });
  reg.registerExport("XMAGetOutputBufferWriteOffset", +[](u32 r3, u32, u32, u32, u32, u32) {
    return withCtx(r3, [](uint8_t* c) { return (u32)XMA_CONTEXT_DATA(c).output_buffer_write_offset; });
  });
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[XMA] SR_XMA_NEW=1: 16 XMA* exports = native port of Xenia xma_decoder=new "
              "(oracle 0bd090dbe), dedicated_thread=%d",
              xmaDedicatedThreadEnabled() ? 1 : 0);
}

}  // namespace sr::kernel::apu
