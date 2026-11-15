// vb_publish.hpp — the vertex-buffer publication decision, as pure functions.
//
// iter-456p Step 3A. Everything here is decode and policy: no guest reads, no
// logging, no globals. The hook does the reading; this decides what the read
// bytes MEAN and whether they must be published. Split that way for one reason
// — the director requires synthetic tests for two simultaneous streams,
// null/rebind, `offset == size` and `offset > size`, unchanged-resource dedup,
// changed-content republish, both alias outcomes and payload corruption, and
// none of those should need a running game to exercise.
//
// ⚠ THE OBJECT LAYOUT IS NOT GUESSED. Generated PPC for `sub_820F3ED8`
// (CreateVertexBuffer) writes `(sizeBytes & 0x03FFFFFC) | flags | 2` to the
// vertex-buffer object at `+0x1C`, and `sub_820F2A50` (SetStreamSource) reads
// that same dword and subtracts `OffsetInBytes` before publishing the vertex
// fetch size. Two independent confirmations that this is the right pair:
//   * Xenia's `xe_gpu_vertex_fetch_t` (xenos.h:1158-1171) packs `size:24` at
//     bit 2 in WORDS, so the raw dword is a byte size with the endian mode in
//     the low 2 bits — which is exactly why the guest can subtract a byte
//     offset from it directly.
//   * UnleashedRecomp computes the same quantity as `buffer->dataSize - offset`
//     (video.cpp ProcSetStreamSource).
#pragma once

#include <cstdint>
#include <cstddef>

namespace sr::gfx {

// ── The object header, decoded ────────────────────────────────────────────
struct VbDesc {
  bool     valid = false;
  uint32_t rawBase = 0;    // be32(vb + 0x18), as read
  uint32_t rawSize = 0;    // be32(vb + 0x1C), as read
  uint32_t physBase = 0;   // normalized: low 29 bits, alias window stripped
  uint32_t dataSize = 0;   // rawSize & 0x03FFFFFC — the LOGICAL buffer size
  uint32_t endian = 0;     // rawSize & 3
};

// ⚠ `physBase` deliberately drops the alias window. The 0x8 and 0xA windows are
// the SAME memory on console; our harness does not fold them (iter-425), so a
// key that kept the window would file one buffer under two identities and the
// dedup below would republish it forever.
inline VbDesc decodeVbHeader(uint32_t rawBase, uint32_t rawSize) {
  VbDesc d;
  d.rawBase = rawBase;
  d.rawSize = rawSize;
  d.physBase = rawBase & 0x1FFFFFFFu;
  d.dataSize = rawSize & 0x03FFFFFCu;
  d.endian = rawSize & 3u;
  d.valid = d.physBase != 0 && d.dataSize != 0;
  return d;
}

// ── Named refusals ────────────────────────────────────────────────────────
// A bool would collapse eight different reasons into one, and the whole point
// of this slice is that a stream we cannot serve is refused BY NAME rather than
// silently substituted with zeros.
enum class VbRefuse : uint32_t {
  None = 0,
  StreamOutOfRange,   // stream index outside 0..15
  NotLive,            // a null bind — the guest unbound this stream
  BadHeader,          // physBase or dataSize decoded to zero
  SizeTooLarge,       // beyond the per-buffer residency cap
  OffsetBeyondSize,   // offset > dataSize: no legal view exists
  Unreadable,         // neither alias window is mapped
  NoAlias,            // both windows mapped, neither holds content
  Count
};

inline const char* vbRefuseName(VbRefuse r) {
  switch (r) {
    case VbRefuse::None:             return "none";
    case VbRefuse::StreamOutOfRange: return "streamOutOfRange";
    case VbRefuse::NotLive:          return "notLive";
    case VbRefuse::BadHeader:        return "badHeader";
    case VbRefuse::SizeTooLarge:     return "sizeTooLarge";
    case VbRefuse::OffsetBeyondSize: return "offsetBeyondSize";
    case VbRefuse::Unreadable:       return "unreadable";
    case VbRefuse::NoAlias:          return "noAlias";
    default:                         return "?";
  }
}

// ── The binding, producer-side ────────────────────────────────────────────
// The OFFSET is why this state must exist at all. The stream's VB object VA is
// recoverable from the device shadow (`dev[4*(3047+s)]`) and its stride from
// `dev+12256+s`, but the offset is folded into the vertex fetch constant's base
// and cannot be recovered from it. Only the SetStreamSource call carries it.
struct VbBinding {
  uint32_t objectVA = 0;
  uint32_t offset = 0;
  uint32_t stride = 0;
  bool     live = false;
};

constexpr uint32_t kVbMaxStreams = 16;

// `offset == dataSize` is LEGAL and yields an empty view — the reference's
// `dataSize - offset` is zero, and a zero-size view is a real state the guest
// can bind. Only `offset > dataSize` has no legal view.
inline VbRefuse vbCheckBinding(uint32_t stream, const VbBinding& b,
                               const VbDesc& d, uint64_t sizeCap) {
  if (stream >= kVbMaxStreams) return VbRefuse::StreamOutOfRange;
  if (!b.live || b.objectVA == 0) return VbRefuse::NotLive;
  if (!d.valid) return VbRefuse::BadHeader;
  if (sizeCap && (uint64_t)d.dataSize > sizeCap) return VbRefuse::SizeTooLarge;
  if (b.offset > d.dataSize) return VbRefuse::OffsetBeyondSize;
  return VbRefuse::None;
}

// The reference's view size, once the binding is known legal.
inline uint32_t vbViewSize(const VbBinding& b, const VbDesc& d) {
  return d.dataSize - b.offset;   // caller must have passed vbCheckBinding
}

// ── The alias choice ──────────────────────────────────────────────────────
// ⛔ THE vbc3 LESSON, encoded. `src` and `srcAlt` are the SAME buffer through
// the 0x8 and 0xA physical windows. A head-only probe once chose the window
// that held 4 MB of zeros for every 0xA-written buffer and blacked out an
// entire era, starfield included. The choice is made by WHOLE-BUFFER scan with
// an early exit, and it is RECORDED — never re-derived later, because a
// re-derivation can disagree with the bytes actually published.
enum class VbAlias : uint32_t { Primary = 0, Alt = 1 };

inline bool vbSpanHasContent(const uint8_t* p, uint32_t n) {
  if (!p) return false;
  for (uint32_t i = 0; i < n; i++)
    if (p[i]) return true;
  return false;
}

struct VbAliasChoice {
  VbAlias  alias = VbAlias::Primary;
  const uint8_t* bytes = nullptr;
  VbRefuse refuse = VbRefuse::None;
  bool     primaryHadContent = false;
  bool     altHadContent = false;
};

// Prefers the PRIMARY window when both hold content — they are the same memory
// on console, so "both non-empty" is the normal case and flipping between them
// run to run would make the digest unstable for no reason.
inline VbAliasChoice vbChooseAlias(const uint8_t* primary, const uint8_t* alt,
                                   uint32_t bytes) {
  VbAliasChoice c;
  if (!primary && !alt) { c.refuse = VbRefuse::Unreadable; return c; }
  c.primaryHadContent = vbSpanHasContent(primary, bytes);
  c.altHadContent = vbSpanHasContent(alt, bytes);
  if (c.primaryHadContent) { c.alias = VbAlias::Primary; c.bytes = primary; }
  else if (c.altHadContent) { c.alias = VbAlias::Alt; c.bytes = alt; }
  else {
    // Both readable, neither holds content. NOT an error to paper over: an
    // all-zero buffer is publishable and may be exactly what the guest wrote.
    // Recorded as such, with the primary as the source, so the digest is
    // stable and the caller can see it was a genuine all-zero read.
    c.alias = VbAlias::Primary;
    c.bytes = primary ? primary : alt;
    if (!c.bytes) c.refuse = VbRefuse::Unreadable;
  }
  return c;
}

// ── The content digest ────────────────────────────────────────────────────
// FNV-1a over the ORIGINAL guest bytes. Deliberately not the trace's XXH3: this
// value is a change detector and travels in the command; the payload's XXH3 in
// the record tail remains the integrity check, and keeping them different means
// a corrupt payload cannot agree with its own digest by construction.
inline uint64_t vbDigest(const uint8_t* p, uint32_t n) {
  uint64_t h = 1469598103934665603ull;
  if (!p) return 0;
  for (uint32_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

// ── The sparse mutation probe ─────────────────────────────────────────────
// ⚠ THE HOT PATH IS EVERY DRAW. A whole-buffer scan plus a whole-buffer digest
// per draw is ~2.25M full passes over buffers up to 16 MB in a run — it would
// not merely be slow, it would make the recording roll unusable and so waste
// the one authorized fixture refresh.
//
// So the same rule the VB residency cache already measured is reused here
// verbatim (gfx/hle_video.cpp vbCaptureSig/vbBytesMatch): full compare under
// 96 bytes, else first/mid/last 32 bytes, against RAW guest bytes. The full
// digest is computed ONLY when the probe says something changed, or on first
// sight.
//
// ⚠ THE RESIDUE, NAMED: an edit confined to the middle of a large buffer that
// touches none of the three probed windows is MISSED, and the buffer replays at
// its previous generation. That is a counted risk class in the cache and it is
// a counted risk class here — `probeMissRisk` below is what a census reports so
// the exposure is visible rather than assumed away. It is not zero, and calling
// the dedup exact would be the overclaim.
constexpr uint32_t kVbSigBytes = 96;

struct VbSparseSig {
  uint8_t  bytes[kVbSigBytes] = {};
  uint32_t len = 0;
};

inline VbSparseSig vbCaptureSparseSig(const uint8_t* raw, uint32_t n) {
  VbSparseSig sg;
  if (!raw || n == 0) return sg;
  if (n <= kVbSigBytes) {
    for (uint32_t i = 0; i < n; i++) sg.bytes[i] = raw[i];
    sg.len = n;
    return sg;
  }
  for (uint32_t i = 0; i < 32; i++) {
    sg.bytes[i] = raw[i];
    sg.bytes[32 + i] = raw[n / 2 + i];
    sg.bytes[64 + i] = raw[n - 32 + i];
  }
  sg.len = kVbSigBytes;
  return sg;
}

inline bool vbSparseSigMatches(const VbSparseSig& sg, const uint8_t* now,
                               uint32_t n) {
  if (!now || sg.len == 0) return false;
  if (n <= kVbSigBytes) {
    if (sg.len != n) return false;
    for (uint32_t i = 0; i < n; i++)
      if (sg.bytes[i] != now[i]) return false;
    return true;
  }
  if (sg.len != kVbSigBytes) return false;
  for (uint32_t i = 0; i < 32; i++)
    if (sg.bytes[i] != now[i] || sg.bytes[32 + i] != now[n / 2 + i] ||
        sg.bytes[64 + i] != now[n - 32 + i])
      return false;
  return true;
}

// True when the probe CANNOT see the whole buffer, i.e. a mid-buffer edit could
// hide. Reported, never silently accepted.
inline bool vbProbeMissRisk(uint32_t n) { return n > kVbSigBytes; }

// ── The publication decision ──────────────────────────────────────────────
// One resource per (physBase, dataSize, endian). A content change at the same
// key bumps the GENERATION rather than rewriting the entry, so a consumer that
// still holds the old generation is never handed different bytes under the same
// name.
struct VbResourceState {
  uint32_t physBase = 0;
  uint32_t dataSize = 0;
  uint32_t endian = 0;
  uint64_t digest = 0;
  uint32_t generation = 0;
  bool     known = false;
  // ⚠ THE ALIAS IS LOCKED AT FIRST PUBLICATION and every later probe reads the
  // CHOSEN window. Re-choosing per draw would both cost a whole-buffer scan on
  // the hot path and let the source flip between two windows mid-run, which
  // would republish forever and make the generation meaningless.
  VbAlias  alias = VbAlias::Primary;
  VbSparseSig sig;
};

enum class VbPublishAction : uint32_t {
  FirstSight = 0,   // never seen: publish, generation 1
  ContentChanged,   // same key, different bytes: publish, generation + 1
  Unchanged,        // same key, same bytes: bind only, no payload
};

inline const char* vbPublishActionName(VbPublishAction a) {
  switch (a) {
    case VbPublishAction::FirstSight:     return "firstSight";
    case VbPublishAction::ContentChanged: return "contentChanged";
    case VbPublishAction::Unchanged:      return "unchanged";
    default:                              return "?";
  }
}

// ⚠ THE DECISION IS TRANSACTIONAL. `vbPeekPublish` REPORTS what should happen
// and mutates nothing; the caller commits only after the payload is safely
// parked. The first cut mutated here and rolled back on failure, and the
// rollback restored digest and generation but NOT the sparse signature — so a
// failed publication left the NEW signature installed, the next draw's cheap
// probe matched it, and the buffer took the unchanged fast path FOREVER. That
// was not hypothetical: 490 changed attempts against 252 successful rows on the
// one measured roll means 238 buffers were poisoned exactly this way. A partial
// rollback is a whole class of bug; not mutating until commit removes the class
// rather than patching the instance.
//
// ⚠ The key includes dataSize AND endian, not just the base. A reused address
// with a different size is a different resource, and treating it as the same
// one would publish the new size's bytes under the old size's identity.
inline VbPublishAction vbPeekPublish(const VbResourceState& st, const VbDesc& d,
                                     uint64_t digest) {
  if (!st.known || st.physBase != d.physBase || st.dataSize != d.dataSize ||
      st.endian != d.endian)
    return VbPublishAction::FirstSight;
  if (st.digest != digest) return VbPublishAction::ContentChanged;
  return VbPublishAction::Unchanged;
}

// Commits a publication that ACTUALLY HAPPENED. Every field moves together —
// digest, generation, alias and signature — so no later probe can read a state
// that half-describes a publication which never reached the trace.
inline uint32_t vbCommitPublish(VbResourceState& st, const VbDesc& d,
                                uint64_t digest, VbAlias alias,
                                const VbSparseSig& sig) {
  const bool fresh = !st.known || st.physBase != d.physBase ||
                     st.dataSize != d.dataSize || st.endian != d.endian;
  st.physBase = d.physBase;
  st.dataSize = d.dataSize;
  st.endian = d.endian;
  st.digest = digest;
  st.alias = alias;
  st.sig = sig;
  st.generation = fresh ? 1u : st.generation + 1u;
  st.known = true;
  return st.generation;
}

// Retained for the existing tests: decide AND commit in one step, which is only
// correct where the publication cannot fail.
inline VbPublishAction vbDecidePublish(VbResourceState& st, const VbDesc& d,
                                       uint64_t digest) {
  if (!st.known || st.physBase != d.physBase || st.dataSize != d.dataSize ||
      st.endian != d.endian) {
    st.physBase = d.physBase;
    st.dataSize = d.dataSize;
    st.endian = d.endian;
    st.digest = digest;
    st.generation = 1;
    st.known = true;
    return VbPublishAction::FirstSight;
  }
  if (st.digest != digest) {
    st.digest = digest;
    st.generation++;
    return VbPublishAction::ContentChanged;
  }
  return VbPublishAction::Unchanged;
}

// ── The command's packed endian/alias/generation dword ────────────────────
// endian in bits 0-1, alias in bits 8-9, generation in bits 16-31. Packed
// rather than given three dwords because the union has exactly seven and the
// payload's arena offset must have one.
//
// ⚠ THE GENERATION SATURATES, it does not wrap. A wrap at 65536 would make a
// changed buffer read as unchanged under an old generation — the one failure
// this field exists to prevent. Saturation is visible and monotone; a wrap is
// silent and wrong.
constexpr uint32_t kVbMaxGeneration = 0xFFFFu;

inline uint32_t vbPackEndAliasGen(uint32_t endian, VbAlias alias,
                                  uint32_t generation) {
  const uint32_t g = generation > kVbMaxGeneration ? kVbMaxGeneration
                                                   : generation;
  return (endian & 3u) | (((uint32_t)alias & 3u) << 8) | (g << 16);
}
inline uint32_t vbUnpackEndian(uint32_t packed) { return packed & 3u; }
inline VbAlias vbUnpackAlias(uint32_t packed) {
  return (VbAlias)((packed >> 8) & 3u);
}
inline uint32_t vbUnpackGeneration(uint32_t packed) { return packed >> 16; }

}  // namespace sr::gfx
