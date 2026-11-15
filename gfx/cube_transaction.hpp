// cube_transaction.hpp -- H1(b)-r5: the cube upload TRANSACTION as one body
// the backend and a device fixture both drive.
//
// This header exists because r4 was rejected on four lifetime defects that a
// helper-only suite could not see: an allocator Reset issued while its
// submission might still execute, a same-key upload rewriting the published
// shader-visible descriptor, no D3D12 device fixture at all, and a duplicated
// replay summary. Everything that decides object lifetime or publication now
// lives HERE, takes the device/queue/heap as parameters, and is exercised by
// kernel/tex_unlock_test.cpp against a real WARP device with object identities
// observed, not inferred from source spelling.
//
// ── THE UPLOAD-CONTEXT LIFETIME MODEL ──────────────────────────────────────
// A context is {allocator, list, fence, event}. Cube uploads use their OWN
// contexts, never the backend's shared texture-upload allocator: every other
// user of that shared allocator (uploadTexture2D, the readback and dump paths)
// calls Reset unconditionally at its next use, so a cube submission left
// pending on it would be Reset under executing work by an unrelated caller.
// Sharing would make the invariant below unenforceable.
//
//   INVARIANT: the ACTIVE context is idle. It became active either fresh
//   (never submitted) or after submit() PROVED completion by a satisfied
//   wait. acquire() is the only caller of ID3D12CommandAllocator::Reset, and
//   it only ever resets the active context. Microsoft's Reset contract makes
//   a Reset under executing work undefined behaviour; Reset is NOT a
//   pending-work query, so no code path here "probes" an allocator with it.
//
//   Close failure  : nothing was executed. The context is DISCARDED (released;
//                    legal - no GPU work references it) and the next acquire
//                    installs a fresh one. A clean upload can follow at once.
//   Signal failure : work is executing and there is NO completion token. The
//                    context, staging and destination are QUARANTINED in the
//                    lost list until teardown/device loss. Never Reset, never
//                    reused, never released early. A fresh context is
//                    installed on the next acquire without touching this one.
//   Event / Wait   : Signal succeeded, so the fence value IS a completion
//   failure          token. The context and both resources are RETIRED behind
//                    that value and released only when drain() observes the
//                    fence past it. The next acquire installs a fresh context
//                    while that one is pending.
//   Wait satisfied : completion proven. The context returns to active; the
//                    staging buffer may be released immediately.
//
// ── VERSIONED, ATOMIC DESCRIPTOR PUBLICATION ────────────────────────────────
// Indices 1..7 are a FREE LIST, not a cursor. Every upload - first, refresh or
// resize - takes an UNPUBLISHED slot from the free list and an unpublished
// resource. Only after submit() proves completion is the SRV written into
// that slot and the map switched to (resource, slot) in one step under the
// lock. The old pair is retired TOGETHER behind the graphics-use fence the
// backend passes in and its slot returns to the free list only when
// drainGraphics() sees that fence complete. No published, possibly in-flight
// slot is ever rewritten. On any failure the candidate slot goes straight
// back to the free list (it was never published, so nothing can reference
// it) and the published entry is untouched: same resource pointer, same
// index, same descriptor bytes.
//
// ── THE BOUNDED LIMIT: SEVEN SLOTS, NO EVICTION ─────────────────────────────
// Indices 1..7 are the only real cube slots and NOTHING evicts a live cube.
// A refresh of a live key needs a SECOND free slot while its old pair waits
// on the graphics fence, so once all seven slots are live every upload - a
// refresh or a new key - fails NoSlot until drainGraphics() retires one. The
// frozen title fixture has four distinct cube keys, so this is not an
// observed title blocker; it is a recorded implementation limit of H1(b).
// This is NOT a general cube cache: no LRU, no eviction policy, no
// cross-frame residency model. Do not describe it as one.
//
// ── THE EXACT REQUEST CONTRACT (r6) ─────────────────────────────────────────
// validateRequest() decides, before any slot or device work:
//   * the caller's render thread owns Present (else PresentNotOwned: the
//     graphics-fence stamp is advanced by whichever thread presents, and the
//     r5 verdict found that with SR_HLE_OWN_PRESENT=0 that is the MAIN thread
//     while the upload runs on the render thread - an unsynchronized stamp);
//   * the DXGI format is one whose block geometry this unit knows;
//   * every index in [0, depthOrArraySize * mipLevels) appears EXACTLY once
//     (IndexOutOfRange / IndexDuplicate / IndexMissing) - by pigeonhole that
//     pair of checks is the "exactly depthOrArraySize * mipLevels" check;
//   * each subresource's w/h is that (face, mip)'s extent, rows equals the
//     block-row count, rowBytes equals blocks-per-row times bytes-per-block,
//     and the backing holds exactly rows * rowBytes.
// A request that fails any clause consumes no slot and creates no context.
//
// ── THE FAULT SEAM ──────────────────────────────────────────────────────────
// Faults fire BEFORE the operation they model, so the code path afterwards is
// the real one: an injected Signal failure really leaves executing work with
// no token; an injected Event/Wait failure really leaves work whose fence
// nobody waited on. `setCompletedOverride` additionally lets a fixture pin the
// fence value drain() believes is complete, because WARP finishes too fast to
// observe the pending state by timing alone - and because a drain that ignores
// the fence must be a mutant the fixture can kill, not a comment.
#pragma once

#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "cube_bind_rule.hpp"

namespace sr {
namespace gfx {
namespace cubetx {

namespace CB = sr::gfx::cubebind;

// ── SLOT RULES (pure; mutation-tested without a device) ──────────────────────
// Bit i of the mask set == index i is FREE. Index 0 is the white dummy and is
// never in the mask.
inline constexpr uint32_t kSlotCount = 8;
inline constexpr uint32_t kFreeMaskAll = 0xFEu;   // bits 1..7

inline uint32_t slotAcquire(uint32_t& freeMask, CB::CubeRefuse& why) {
  why = CB::CubeRefuse::None;
  for (uint32_t i = CB::kCubeFirstRealIndex; i <= CB::kCubeLastRealIndex; i++) {
    if (freeMask & (1u << i)) {
      freeMask &= ~(1u << i);
      return i;
    }
  }
  why = CB::CubeRefuse::NoSlot;
  return CB::kCubeDummyIndex;   // degrade to the dummy, never alias a live cube
}

inline void slotRelease(uint32_t& freeMask, uint32_t idx) {
  if (idx >= CB::kCubeFirstRealIndex && idx <= CB::kCubeLastRealIndex)
    freeMask |= (1u << idx);
}

inline uint32_t slotFreeCount(uint32_t freeMask) {
  uint32_t n = 0;
  for (uint32_t i = CB::kCubeFirstRealIndex; i <= CB::kCubeLastRealIndex; i++)
    if (freeMask & (1u << i)) n++;
  return n;
}

// ── THE FAULT SEAM ───────────────────────────────────────────────────────────
enum class Fault : uint8_t { None = 0, Close, Signal, Event, Wait };

// SR_HLE_CUBE_FAIL_SUBMIT=close|signal|event|wait (first letter decides).
inline Fault faultFromEnv(const char* e) {
  if (!e || !e[0]) return Fault::None;
  switch (e[0]) {
    case 'c': case 'C': return Fault::Close;
    case 's': case 'S': return Fault::Signal;
    case 'e': case 'E': return Fault::Event;
    case 'w': case 'W': return Fault::Wait;
    default: return Fault::None;
  }
}

inline const char* faultName(Fault f) {
  switch (f) {
    case Fault::None: return "none";
    case Fault::Close: return "close";
    case Fault::Signal: return "signal";
    case Fault::Event: return "event";
    case Fault::Wait: return "wait";
  }
  return "?";
}

// ── THE SUMMARY LINE (pure) ──────────────────────────────────────────────────
struct Summary {
  uint64_t attempted = 0, uploaded = 0, refused = 0, noSlot = 0;
  uint32_t freeSlots = 0, liveCubes = 0, retiredCubes = 0;
  uint32_t retiredCtx = 0, lostCtx = 0;
};

// Conservation: every attempt ended as exactly one of uploaded/refused; NoSlot
// is a kind of refusal; and the seven real slots are always accounted for as
// free, live (published) or retired (waiting on the graphics fence).
inline bool summaryConserves(const Summary& s) {
  return s.attempted == s.uploaded + s.refused && s.noSlot <= s.refused &&
         s.freeSlots + s.liveCubes + s.retiredCubes == CB::kCubeLastRealIndex;
}

// `site` names WHICH emission path printed the line ("periodic" or
// "replay-final"). A replay emits exactly one replay-final line; live
// execution emits periodic lines; neither path emits the other's.
//
// The FIELDS only. The "[S6_CUBE_SUM]" tag itself is spelled at the
// SR_LOG_BOOT call in D3D12Backend::hleCubeSumReport, because
// tools/audit_log_gating.py recognises an emitter by a "[TAG]" literal at the
// logging call; a tag hidden inside this format string made the emitter
// invisible to that screen (NOT RECOGNIZED, exit 3).
inline constexpr const char* kSummaryTag = "[S6_CUBE_SUM]";

inline int formatSummaryFields(const Summary& s, const char* site, char* buf,
                               size_t cap) {
  return std::snprintf(
      buf, cap,
      "site=%s attempted=%llu uploaded=%llu refused=%llu "
      "noSlot=%llu free=%u live=%u retiredCubes=%u retiredCtx=%u lostCtx=%u "
      "conserves=%s",
      site ? site : "?", (unsigned long long)s.attempted,
      (unsigned long long)s.uploaded, (unsigned long long)s.refused,
      (unsigned long long)s.noSlot, s.freeSlots, s.liveCubes, s.retiredCubes,
      s.retiredCtx, s.lostCtx, summaryConserves(s) ? "YES" : "VIOLATION");
}

// The whole line as a reader sees it in the log: tag, space, fields.
inline int formatSummary(const Summary& s, const char* site, char* buf,
                         size_t cap) {
  const int n = std::snprintf(buf, cap, "%s ", kSummaryTag);
  if (n < 0 || (size_t)n >= cap) return n;
  return n + formatSummaryFields(s, site, buf + n, cap - (size_t)n);
}

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

// One staged subresource: linear host rows plus the destination index.
struct StagedSub {
  std::vector<uint8_t> lin;
  uint32_t rows = 0, rowBytes = 0, index = 0, w = 0, h = 0;
};

struct UploadContext {
  ComPtr<ID3D12CommandAllocator> alloc;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE event = nullptr;
  UINT64 fenceValue = 0;      // last value signalled on `fence`
  uint32_t resets = 0;        // allocator Resets issued on this context
  uint32_t submissions = 0;   // ExecuteCommandLists calls
  uint64_t serial = 0;        // creation ordinal, for identity in fixtures
  UploadContext() = default;
  UploadContext(const UploadContext&) = delete;
  UploadContext& operator=(const UploadContext&) = delete;
  ~UploadContext() { if (event) CloseHandle(event); }
};

// Token valid (Signal succeeded): release once `fence` passes `fenceValue`.
struct RetiredUpload {
  std::shared_ptr<UploadContext> ctx;
  std::vector<ComPtr<ID3D12Resource>> res;
  UINT64 fenceValue = 0;
};

// No token (Signal failed after Execute): held until teardown/device loss.
struct LostUpload {
  std::shared_ptr<UploadContext> ctx;
  std::vector<ComPtr<ID3D12Resource>> res;
};

class UploadContextPool {
 public:
  // The idle active context with its allocator and list reset, or nullptr if
  // D3D12 could not provide one. Creating and resetting here is legal by the
  // invariant: the returned context has no unresolved submission.
  std::shared_ptr<UploadContext> acquire(ID3D12Device* dev) {
    std::shared_ptr<UploadContext> ctx;
    {
      std::lock_guard<std::mutex> lk(m_mu);
      ctx = std::move(m_active);
      m_active.reset();
    }
    if (!ctx) {
      ctx = std::make_shared<UploadContext>();
      if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&ctx->alloc))))
        return nullptr;
      if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        ctx->alloc.Get(), nullptr,
                                        IID_PPV_ARGS(&ctx->list))))
        return nullptr;
      ctx->list->Close();
      if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                  IID_PPV_ARGS(&ctx->fence))))
        return nullptr;
      ctx->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!ctx->event) return nullptr;
      std::lock_guard<std::mutex> lk(m_mu);
      ctx->serial = ++m_created;
    }
    // The ONLY Reset in this unit, on the ONLY context proven idle.
    ctx->resets++;
    if (FAILED(ctx->alloc->Reset())) return nullptr;        // ctx dropped
    if (FAILED(ctx->list->Reset(ctx->alloc.Get(), nullptr))) return nullptr;
    return ctx;
  }

  // A recording that will never be executed (a pre-submit refusal). Ends the
  // recording; the context stays idle and active if Close succeeds, else it
  // is discarded. Nothing here was submitted, so releasing is legal.
  void abortRecording(std::shared_ptr<UploadContext> ctx) {
    if (!ctx) return;
    if (SUCCEEDED(ctx->list->Close())) {
      std::lock_guard<std::mutex> lk(m_mu);
      m_active = std::move(ctx);
    }
  }

  // Close / Execute / Signal / Event / Wait with the lifetime model above.
  // `keep` are the resources the submission references (staging, destination).
  CB::SubmitStatus submit(ID3D12CommandQueue* queue,
                          std::shared_ptr<UploadContext> ctx,
                          std::vector<ComPtr<ID3D12Resource>> keep,
                          Fault fault, DWORD waitMs) {
    if (!queue || !ctx) return CB::SubmitStatus::CloseFailed;

    // ── Close: nothing has been executed yet ──────────────────────────────
    // A Close failure DISCARDS the context: `ctx` is ours by value and no GPU
    // work references it, so letting it die here is legal and leaves the
    // pool with no active context - the next acquire installs a fresh one.
    if (fault == Fault::Close) return CB::SubmitStatus::CloseFailed;
    {
      const HRESULT hrClose = ctx->list->Close();
      const auto st = CB::classifySubmit((int32_t)hrClose, 0, 0, true);
      if (st != CB::SubmitStatus::Ok) return st;
    }

    // ── Execute ───────────────────────────────────────────────────────────
    ID3D12CommandList* lists[] = {ctx->list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ctx->submissions++;
    const UINT64 fv = ctx->fenceValue + 1;

    // ── Signal: the only step that can leave work with NO token ───────────
    if (fault == Fault::Signal) {
      quarantine(std::move(ctx), std::move(keep));
      return CB::SubmitStatus::SignalFailed;
    }
    {
      const HRESULT hrSignal = queue->Signal(ctx->fence.Get(), fv);
      const auto st = CB::classifySubmit(0, (int32_t)hrSignal, 0, true);
      if (st != CB::SubmitStatus::Ok) {
        quarantine(std::move(ctx), std::move(keep));
        return st;
      }
    }
    ctx->fenceValue = fv;

    // ── Event / Wait: token valid; unproven completion retires behind it ──
    if (fault == Fault::Event) {
      retire(std::move(ctx), std::move(keep), fv);
      return CB::SubmitStatus::EventFailed;
    }
    if (ctx->fence->GetCompletedValue() < fv || fault == Fault::Wait) {
      const HRESULT hrEvent = ctx->fence->SetEventOnCompletion(fv, ctx->event);
      const auto stE = CB::classifySubmit(0, 0, (int32_t)hrEvent, true);
      if (stE != CB::SubmitStatus::Ok) {
        retire(std::move(ctx), std::move(keep), fv);
        return stE;
      }
      if (fault == Fault::Wait) {
        retire(std::move(ctx), std::move(keep), fv);
        return CB::SubmitStatus::WaitFailed;
      }
      const DWORD w = WaitForSingleObject(ctx->event, waitMs);
      const auto stW = CB::classifySubmit(0, 0, 0, w == WAIT_OBJECT_0);
      if (stW != CB::SubmitStatus::Ok) {
        retire(std::move(ctx), std::move(keep), fv);
        return stW;
      }
    }

    // ── Completion PROVEN: the context is idle again ──────────────────────
    std::lock_guard<std::mutex> lk(m_mu);
    m_active = std::move(ctx);
    return CB::SubmitStatus::Ok;
  }

  // Release retired submissions whose fence has completed. Returns how many.
  size_t drain() {
    std::lock_guard<std::mutex> lk(m_mu);
    size_t freed = 0, w = 0;
    for (size_t i = 0; i < m_retired.size(); i++) {
      if (completedLocked(m_retired[i])) { freed++; continue; }
      if (w != i) m_retired[w] = std::move(m_retired[i]);
      w++;
    }
    m_retired.resize(w);
    return freed;
  }

  // Only when the device is idle or lost: drops every context.
  void teardown() {
    std::lock_guard<std::mutex> lk(m_mu);
    m_retired.clear();
    m_lost.clear();
    m_active.reset();
  }

  // ── observations (fixtures and the summary line) ──────────────────────
  ID3D12CommandAllocator* activeAllocator() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_active ? m_active->alloc.Get() : nullptr;
  }
  size_t retiredCount() const { std::lock_guard<std::mutex> lk(m_mu); return m_retired.size(); }
  size_t lostCount() const { std::lock_guard<std::mutex> lk(m_mu); return m_lost.size(); }
  std::shared_ptr<UploadContext> retiredCtx(size_t i) const {
    std::lock_guard<std::mutex> lk(m_mu);
    return i < m_retired.size() ? m_retired[i].ctx : nullptr;
  }
  UINT64 retiredFence(size_t i) const {
    std::lock_guard<std::mutex> lk(m_mu);
    return i < m_retired.size() ? m_retired[i].fenceValue : 0;
  }
  std::shared_ptr<UploadContext> lostCtx(size_t i) const {
    std::lock_guard<std::mutex> lk(m_mu);
    return i < m_lost.size() ? m_lost[i].ctx : nullptr;
  }
  uint64_t contextsCreated() const { std::lock_guard<std::mutex> lk(m_mu); return m_created; }
  // Test seam: while >= 0, drain() treats THIS as every retired fence's
  // completed value instead of asking the fence, so a fixture can hold work
  // "pending" at an exact value and release it at an exact value. -1 = off.
  void setCompletedOverride(int64_t v) { m_completedOverride.store(v, std::memory_order_release); }
  int64_t completedOverride() const { return m_completedOverride.load(std::memory_order_acquire); }

 private:
  bool completedLocked(const RetiredUpload& r) const {
    if (!r.ctx || !r.ctx->fence) return true;   // nothing to wait on
    const int64_t ov = m_completedOverride.load(std::memory_order_acquire);
    const UINT64 done = ov >= 0 ? (UINT64)ov : r.ctx->fence->GetCompletedValue();
    return done >= r.fenceValue;   // the ONLY completion test drain() applies
  }
  void quarantine(std::shared_ptr<UploadContext> ctx,
                  std::vector<ComPtr<ID3D12Resource>> keep) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_lost.push_back({std::move(ctx), std::move(keep)});
  }
  void retire(std::shared_ptr<UploadContext> ctx,
              std::vector<ComPtr<ID3D12Resource>> keep, UINT64 fv) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_retired.push_back({std::move(ctx), std::move(keep), fv});
  }

  mutable std::mutex m_mu;
  std::shared_ptr<UploadContext> m_active;
  std::vector<RetiredUpload> m_retired;
  std::vector<LostUpload> m_lost;
  std::atomic<int64_t> m_completedOverride{-1};
  uint64_t m_created = 0;
};

// ── THE CUBE TABLE + TRANSACTION ─────────────────────────────────────────────
struct CubeEntry {
  ComPtr<ID3D12Resource> res;
  uint32_t idx = 0, w = 0, h = 0, mips = 0;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};

// An old (resource, slot) pair displaced by a same-key publication. Both wait
// on the GRAPHICS fence the backend stamps, because a frame list submitted
// before the switch may still sample the old slot.
struct RetiredCube {
  ComPtr<ID3D12Resource> res;
  uint32_t idx = 0;
  uint64_t graphicsFence = 0;
};

struct UploadRequest {
  uint32_t key = 0;
  DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
  uint32_t width = 0, height = 0;
  uint16_t depthOrArraySize = 0;   // 6, from the resource plan
  uint16_t mipLevels = 0;
  const StagedSub* subs = nullptr;
  uint32_t count = 0;
  uint64_t graphicsFenceNow = 0;   // the value the frame queue will signal next
  // r6: FAIL CLOSED unless the caller proves the render thread owns Present.
  // The default is false on purpose: a caller that forgets this field cannot
  // publish a cube whose retirement stamp another thread advances.
  bool renderThreadOwnsPresent = false;
  Fault fault = Fault::None;
  int failAtSubresource = -1;      // SR_HLE_CUBE_FAIL_AT: abort the recording here
  DWORD waitMs = 10000;
};

// The block geometry of every DXGI format the cube path can be handed
// (mirrors guestFormatToDxgi in gfx/d3d12_backend.cpp). Anything else refuses
// as UnsupportedFormat before any device work.
inline bool formatBlock(DXGI_FORMAT f, uint32_t& blockDim, uint32_t& bytesPerBlock) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:     blockDim = 1; bytesPerBlock = 4; return true;
    case DXGI_FORMAT_BC1_UNORM:          blockDim = 4; bytesPerBlock = 8; return true;
    case DXGI_FORMAT_BC2_UNORM:          blockDim = 4; bytesPerBlock = 16; return true;
    case DXGI_FORMAT_BC3_UNORM:          blockDim = 4; bytesPerBlock = 16; return true;
    case DXGI_FORMAT_BC5_UNORM:          blockDim = 4; bytesPerBlock = 16; return true;
    case DXGI_FORMAT_R16_UNORM:          blockDim = 1; bytesPerBlock = 2; return true;
    case DXGI_FORMAT_R32_FLOAT:          blockDim = 1; bytesPerBlock = 4; return true;
    case DXGI_FORMAT_R8_UNORM:           blockDim = 1; bytesPerBlock = 1; return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: blockDim = 1; bytesPerBlock = 8; return true;
    case DXGI_FORMAT_R16G16_FLOAT:       blockDim = 1; bytesPerBlock = 4; return true;
    default: return false;
  }
}

// r6: THE EXACT REQUEST CONTRACT, pure (no device), so a suite can drive every
// counterexample for every supported format without WARP, and the device
// fixture can prove that a refused request consumed no slot and no context.
// See the header comment for the clauses. Returns None when the request is
// exactly what CopyTextureRegion will be asked to copy.
inline CB::CubeRefuse validateRequest(const UploadRequest& rq) {
  if (!rq.renderThreadOwnsPresent) return CB::CubeRefuse::PresentNotOwned;
  if (!rq.subs || !rq.count || !rq.width || !rq.height || !rq.mipLevels ||
      !rq.depthOrArraySize)
    return CB::CubeRefuse::ZeroExtent;
  uint32_t blockDim = 0, bytesPerBlock = 0;
  if (!formatBlock(rq.dxgi, blockDim, bytesPerBlock))
    return CB::CubeRefuse::UnsupportedFormat;
  const uint32_t subCount = uint32_t(rq.depthOrArraySize) * rq.mipLevels;
  // One extra sentinel cell absorbs an out-of-range index so that a removed
  // range check cannot turn into an out-of-bounds write.
  std::vector<uint8_t> seen(size_t(subCount) + 1, 0);
  for (uint32_t i = 0; i < rq.count; i++) {
    const StagedSub& s = rq.subs[i];
    if (s.index >= subCount) return CB::CubeRefuse::IndexOutOfRange;
    uint8_t& mark = seen[s.index < subCount ? s.index : subCount];
    if (mark) return CB::CubeRefuse::IndexDuplicate;
    mark = 1;
    const uint32_t mip = s.index % rq.mipLevels;
    const uint32_t ew = rq.width >> mip ? rq.width >> mip : 1u;
    const uint32_t eh = rq.height >> mip ? rq.height >> mip : 1u;
    if (s.w != ew || s.h != eh) return CB::CubeRefuse::ExtentMismatch;
    const uint32_t rows = (eh + blockDim - 1u) / blockDim;
    const uint32_t rowBytes = ((ew + blockDim - 1u) / blockDim) * bytesPerBlock;
    if (s.rows != rows) return CB::CubeRefuse::RowCountMismatch;
    if (s.rowBytes != rowBytes) return CB::CubeRefuse::RowBytesMismatch;
    if (s.lin.size() != size_t(rows) * rowBytes) return CB::CubeRefuse::BackingMismatch;
  }
  for (uint32_t i = 0; i < subCount; i++)
    if (!seen[i]) return CB::CubeRefuse::IndexMissing;
  return CB::CubeRefuse::None;
}

class CubeTransactor {
 public:
  // Every hleUploadCube entry counts once; refusals before the transaction
  // proper are recorded through recordRefusal so the summary conserves.
  void beginAttempt() { m_attempted.fetch_add(1, std::memory_order_relaxed); }
  void recordRefusal(CB::CubeRefuse why) {
    m_refused.fetch_add(1, std::memory_order_relaxed);
    if (why == CB::CubeRefuse::NoSlot)
      m_noSlot.fetch_add(1, std::memory_order_relaxed);
  }

  // Returns the published slot (1..7) or 0 with `why` named. On 0 the
  // previously published pair for `key` is untouched.
  uint32_t upload(ID3D12Device* dev, ID3D12CommandQueue* queue,
                  D3D12_CPU_DESCRIPTOR_HANDLE slot0, UINT inc,
                  const UploadRequest& rq, CB::CubeRefuse& why) {
    why = CB::CubeRefuse::None;
    // 0. THE EXACT REQUEST CONTRACT, BEFORE any slot or device work (r6).
    //    Present ownership, format geometry, one complete unique index set,
    //    exact per-format row footprint. A request that would make
    //    CopyTextureRegion copy something other than exactly this cube is
    //    refused by the clause that caught it - never copied "mostly".
    why = validateRequest(rq);
    if (why != CB::CubeRefuse::None) { recordRefusal(why); return 0; }
    if (!dev || !queue) {
      why = CB::CubeRefuse::ZeroExtent; recordRefusal(why); return 0;
    }

    // 1. An UNPUBLISHED slot. Never the one the key currently owns.
    uint32_t candidateIdx = 0;
    {
      std::lock_guard<std::mutex> lk(m_mu);
      candidateIdx = slotAcquire(m_freeMask, why);
    }
    if (why != CB::CubeRefuse::None) { recordRefusal(why); return 0; }
    auto giveBackSlot = [&]() {
      std::lock_guard<std::mutex> lk(m_mu);
      slotRelease(m_freeMask, candidateIdx);
    };
    auto refuse = [&](CB::CubeRefuse r) -> uint32_t {
      giveBackSlot();
      why = r;
      recordRefusal(r);
      return 0;
    };

    // 2. An UNPUBLISHED resource.
    ComPtr<ID3D12Resource> candidate;
    {
      D3D12_HEAP_PROPERTIES def{}; def.Type = D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC td{};
      td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;   // six SLICES, not a volume
      td.Width = rq.width; td.Height = rq.height;
      td.DepthOrArraySize = rq.depthOrArraySize;
      td.MipLevels = rq.mipLevels;
      td.Format = rq.dxgi; td.SampleDesc.Count = 1;
      if (FAILED(dev->CreateCommittedResource(
              &def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
              nullptr, IID_PPV_ARGS(&candidate))))
        return refuse(CB::CubeRefuse::UnsupportedFormat);
    }

    // 3. Staging: every subresource 512-aligned so its placed footprint is legal.
    std::vector<uint64_t> offs(rq.count, 0);
    uint64_t total = 0;
    for (uint32_t i = 0; i < rq.count; i++) {
      const uint32_t pitch = (rq.subs[i].rowBytes + 255u) & ~255u;
      offs[i] = (total + 511u) & ~511ull;
      total = offs[i] + uint64_t(pitch) * rq.subs[i].rows;
    }
    if (!total) return refuse(CB::CubeRefuse::ByteCountMismatch);
    ComPtr<ID3D12Resource> staging;
    {
      D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
      D3D12_RESOURCE_DESC sd{};
      sd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      sd.Width = total; sd.Height = 1;
      sd.DepthOrArraySize = 1; sd.MipLevels = 1; sd.SampleDesc.Count = 1;
      sd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(dev->CreateCommittedResource(
              &up, D3D12_HEAP_FLAG_NONE, &sd, D3D12_RESOURCE_STATE_GENERIC_READ,
              nullptr, IID_PPV_ARGS(&staging))))
        return refuse(CB::CubeRefuse::CloseFailed);
      uint8_t* mp = nullptr;
      D3D12_RANGE noRead{0, 0};
      if (FAILED(staging->Map(0, &noRead, reinterpret_cast<void**>(&mp))))
        return refuse(CB::CubeRefuse::CloseFailed);
      for (uint32_t i = 0; i < rq.count; i++) {
        const uint32_t pitch = (rq.subs[i].rowBytes + 255u) & ~255u;
        for (uint32_t y = 0; y < rq.subs[i].rows; y++)
          std::memcpy(mp + offs[i] + uint64_t(y) * pitch,
                      rq.subs[i].lin.data() + size_t(y) * rq.subs[i].rowBytes,
                      rq.subs[i].rowBytes);
      }
      staging->Unmap(0, nullptr);
    }

    // 4. Record on an idle context. One COPY_DEST interval, one final
    //    transition (the r2 state-machine fix, kept).
    std::shared_ptr<UploadContext> ctx = m_pool.acquire(dev);
    if (!ctx) return refuse(CB::CubeRefuse::CloseFailed);
    for (uint32_t i = 0; i < rq.count; i++) {
      if (rq.failAtSubresource >= 0 && (int)i == rq.failAtSubresource) {
        // A failure PART WAY THROUGH the recording: nothing executes, nothing
        // publishes, the slot goes back, the context stays usable.
        m_pool.abortRecording(std::move(ctx));
        return refuse(CB::CubeRefuse::CloseFailed);
      }
      D3D12_TEXTURE_COPY_LOCATION d{}, s{};
      d.pResource = candidate.Get();
      d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      d.SubresourceIndex = rq.subs[i].index;
      s.pResource = staging.Get();
      s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      s.PlacedFootprint.Offset = offs[i];
      s.PlacedFootprint.Footprint.Format = rq.dxgi;
      s.PlacedFootprint.Footprint.Width = rq.subs[i].w;
      s.PlacedFootprint.Footprint.Height = rq.subs[i].h;
      s.PlacedFootprint.Footprint.Depth = 1;
      s.PlacedFootprint.Footprint.RowPitch = (rq.subs[i].rowBytes + 255u) & ~255u;
      ctx->list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    }
    {
      D3D12_RESOURCE_BARRIER b{};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = candidate.Get();
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      ctx->list->ResourceBarrier(1, &b);
    }

    // 5. Submit under the lifetime model. The pool owns what must outlive us.
    const auto st = m_pool.submit(queue, std::move(ctx), {staging, candidate},
                                  rq.fault, rq.waitMs);
    if (st != CB::SubmitStatus::Ok) return refuse(CB::submitToRefuse(st));

    // 6. PUBLISH: the SRV goes into the candidate slot (unpublished, so no
    //    in-flight list can reference it), then the map switches in one step.
    const uint32_t publishIdx = candidateIdx;   // NEVER the key's current slot
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
      sv.Format = rq.dxgi;
      sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
      sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      sv.TextureCube.MipLevels = rq.mipLevels;
      sv.TextureCube.MostDetailedMip = 0;
      sv.TextureCube.ResourceMinLODClamp = 0.0f;
      D3D12_CPU_DESCRIPTOR_HANDLE h{slot0.ptr + SIZE_T(publishIdx) * inc};
      dev->CreateShaderResourceView(candidate.Get(), &sv, h);
    }
    {
      std::lock_guard<std::mutex> lk(m_mu);
      m_srvWrites[publishIdx]++;
      CubeEntry fresh;
      fresh.res = candidate; fresh.idx = publishIdx;
      fresh.w = rq.width; fresh.h = rq.height; fresh.mips = rq.mipLevels;
      fresh.fmt = rq.dxgi;
      auto it = m_map.find(rq.key);
      if (it != m_map.end()) {
        // The old pair retires TOGETHER behind the graphics fence.
        m_retired.push_back({it->second.res, it->second.idx, rq.graphicsFenceNow});
        it->second = fresh;
      } else {
        m_map.emplace(rq.key, fresh);
      }
    }
    m_uploaded.fetch_add(1, std::memory_order_relaxed);
    return publishIdx;
  }

  uint32_t slotForKey(uint32_t key) const {
    std::lock_guard<std::mutex> lk(m_mu);
    const auto it = m_map.find(key);
    return it == m_map.end() ? 0u : it->second.idx;
  }
  // A COPY of the published entry (fixtures compare resource identity).
  bool entry(uint32_t key, CubeEntry& out) const {
    std::lock_guard<std::mutex> lk(m_mu);
    const auto it = m_map.find(key);
    if (it == m_map.end()) return false;
    out = it->second;
    return true;
  }

  // Old pairs whose graphics fence has passed: release the resource and
  // return the slot to the free list, together.
  size_t drainGraphics(uint64_t completedGraphicsFence) {
    std::lock_guard<std::mutex> lk(m_mu);
    size_t freed = 0, w = 0;
    for (size_t i = 0; i < m_retired.size(); i++) {
      if (m_retired[i].graphicsFence <= completedGraphicsFence) {
        slotRelease(m_freeMask, m_retired[i].idx);
        freed++;
        continue;
      }
      if (w != i) m_retired[w] = std::move(m_retired[i]);
      w++;
    }
    m_retired.resize(w);
    return freed;
  }
  size_t drainUploads() { return m_pool.drain(); }

  Summary summary() const {
    Summary s;
    s.attempted = m_attempted.load(std::memory_order_relaxed);
    s.uploaded = m_uploaded.load(std::memory_order_relaxed);
    s.refused = m_refused.load(std::memory_order_relaxed);
    s.noSlot = m_noSlot.load(std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lk(m_mu);
      s.freeSlots = slotFreeCount(m_freeMask);
      s.liveCubes = (uint32_t)m_map.size();
      s.retiredCubes = (uint32_t)m_retired.size();
    }
    s.retiredCtx = (uint32_t)m_pool.retiredCount();
    s.lostCtx = (uint32_t)m_pool.lostCount();
    return s;
  }

  uint32_t freeMask() const { std::lock_guard<std::mutex> lk(m_mu); return m_freeMask; }
  uint32_t srvWrites(uint32_t slot) const {
    std::lock_guard<std::mutex> lk(m_mu);
    return slot < kSlotCount ? m_srvWrites[slot] : 0u;
  }
  size_t retiredCubes() const { std::lock_guard<std::mutex> lk(m_mu); return m_retired.size(); }
  UploadContextPool& pool() { return m_pool; }
  const UploadContextPool& pool() const { return m_pool; }

  void teardown() {
    m_pool.teardown();
    std::lock_guard<std::mutex> lk(m_mu);
    m_retired.clear();
    m_map.clear();
    m_freeMask = kFreeMaskAll;
  }

 private:
  mutable std::mutex m_mu;
  std::unordered_map<uint32_t, CubeEntry> m_map;
  uint32_t m_freeMask = kFreeMaskAll;
  std::vector<RetiredCube> m_retired;
  uint32_t m_srvWrites[kSlotCount] = {};
  UploadContextPool m_pool;
  std::atomic<uint64_t> m_attempted{0}, m_uploaded{0}, m_refused{0}, m_noSlot{0};
};
#endif  // _WIN32

}  // namespace cubetx
}  // namespace gfx
}  // namespace sr
