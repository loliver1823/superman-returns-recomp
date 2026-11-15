// tex_version_bank.hpp — the OBSERVER-ONLY texture version banker.
//
// iter-456p. The director's design ruling (docs/CLAIMS_LEDGER.md, "observer-only
// version acquisition"): hash-at-bind is permitted for ONE bounded acquisition,
// but only in a NEW NON-PUBLISHING PATH.
//
//     Do not reuse `texRefreshRevisit()` or `SR_HLE_TEX_REFRESH`; that path
//     contains publication machinery and would weaken the neutrality proof.
//
// ⛔ CAPABILITY BOUNDARY. Nothing in this header may reach a publication API.
// It has no include of hle_video.hpp, no backend handle, and no way to call
// texWrite, classAWrite, enqueue, shadowNoteApplied or any backend method. The
// banker takes BYTES IN and produces RECORDS OUT. That is the whole contract,
// and it is what makes the pixel-neutrality claim checkable by reading one file
// rather than by auditing a call graph.
//
// The defensible claim, in the director's words, is: "The banker introduced no
// executable texture publication or renderer-state mutation." It is NOT a claim
// that an instrumented roll was timing-identical to a natural one — DOGMA makes
// that unprovable.
//
// WHY THIS EXISTS AT ALL: `s_texCaptured` is permanent, so a texture is captured
// once per (base, fmt, w, h) tuple for the life of the process and every later
// bind is dropped. The producer therefore CANNOT emit a v0->v1 transition, which
// is why an inventory of 50 valid v5 artifacts found zero of them. The absence
// was in the instrument, not in the game.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace sr::gfx::hle {

// ── WHY A CANDIDATE DID OR DID NOT QUALIFY ────────────────────────────────────
// Reasons, never a bool. Every refusal is counted, because the director's rule
// is that overflow or unreadability invalidates a NEGATIVE "none found" result
// while leaving a fully evidenced positive intact — and a bool cannot carry that
// distinction.
enum class BankVerdict : uint32_t {
  Qualified = 0,
  SameContent,          // v1 == v0 inside the real footprint: not a transition
  PaddingOnly,          // differs only in pitch padding / tiled tail
  EpochChanged,         // descriptor, dims, pitch, alias window or generation moved
  UnlockIntervened,     // a successful Unlock publication landed between v0 and v1
  ResolveIntervened,    // successful Resolve ownership landed between v0 and v1
  Unreadable,           // guest span not fully readable
  UnsupportedLayout,    // footprint not computable for this layout
  CapacityExceeded,     // a hard limit stopped the probe
  Count
};

inline const char* bankVerdictName(BankVerdict v) {
  switch (v) {
    case BankVerdict::Qualified:         return "Qualified";
    case BankVerdict::SameContent:       return "SameContent";
    case BankVerdict::PaddingOnly:       return "PaddingOnly";
    case BankVerdict::EpochChanged:      return "EpochChanged";
    case BankVerdict::UnlockIntervened:  return "UnlockIntervened";
    case BankVerdict::ResolveIntervened: return "ResolveIntervened";
    case BankVerdict::Unreadable:        return "Unreadable";
    case BankVerdict::UnsupportedLayout: return "UnsupportedLayout";
    case BankVerdict::CapacityExceeded:  return "CapacityExceeded";
    default:                             return "?";
  }
}

// ── THE EPOCH IDENTITY ────────────────────────────────────────────────────────
// The director requires a qualifying pair to share "the same texture-header/
// allocation generation" and "the same complete descriptor epoch and alias
// window". All of it is compared, not a chosen subset: a pair that differs in
// ANY of these is a different allocation wearing the same address.
struct EpochId {
  uint32_t key = 0;            // normalized pixel key
  uint32_t headerIdent = 0;    // texture-header identity (first header dword)
  uint32_t fmtBits = 0;        // fmt | tiled<<8 | endian<<12
  uint32_t dims = 0;           // width<<16 | height
  uint32_t pitch = 0;
  uint32_t dimLevel = 0;       // dimension<<8 | level
  uint32_t aliasLo = 0, aliasHi = 0;   // alias window
  uint64_t allocGeneration = 0;
  uint32_t descEpoch = 0;      // contiguous descriptor-epoch ordinal for this key

  bool operator==(const EpochId& o) const {
    return key == o.key && headerIdent == o.headerIdent &&
           fmtBits == o.fmtBits && dims == o.dims && pitch == o.pitch &&
           dimLevel == o.dimLevel && aliasLo == o.aliasLo &&
           aliasHi == o.aliasHi && allocGeneration == o.allocGeneration &&
           descEpoch == o.descEpoch;
  }
  bool operator!=(const EpochId& o) const { return !(*this == o); }
};

// ── PUBLICATION SERIALS ───────────────────────────────────────────────────────
// ⚠ The director's explicit correction: "Do not use only the current ownership
// value. An Unlock could occur and later be superseded." A snapshot of the
// CURRENT authority cannot see a publication that happened and was then
// replaced, so the pair would look clean when it is not. These are MONOTONIC
// per-key counters of SUCCESSFUL publications; both must be unchanged from v0 to
// v1, which no supersession can hide.
struct Serials {
  uint64_t unlockPublishes = 0;
  uint64_t resolveOwnerships = 0;
  bool operator==(const Serials& o) const {
    return unlockPublishes == o.unlockPublishes &&
           resolveOwnerships == o.resolveOwnerships;
  }
};

// ── THE REAL FOOTPRINT ────────────────────────────────────────────────────────
// A difference confined to pitch padding or the tiled tail is NOT a content
// change. `realFootprintRows` returns the per-row byte count that is real, and
// the row count; `ok=false` means the layout is not computable and the candidate
// must be refused as UnsupportedLayout rather than guessed at.
struct Footprint {
  bool     ok = false;
  uint32_t rowBytes = 0;    // real bytes at the START of each row
  uint32_t rows = 0;
  uint32_t pitch = 0;
  uint64_t realBytes = 0;   // rowBytes * rows
  uint64_t spanBytes = 0;   // pitch * rows (what the upload actually covers)
};

// Bytes per pixel for the linear formats this acquisition supports. 0 = unknown,
// which becomes UnsupportedLayout rather than an assumption.
// ⚠ NAMES FROM Xenia xenos.h, not from memory. I previously had format 24 down
// as one byte "8-bit (the glyph atlas class)" - it is `k_16`, ONE 16-BIT
// CHANNEL, so two bytes. That mistake would have mis-sized every real-footprint
// diff on a linear k_16 surface by a factor of two, silently comparing half of
// each row and calling the rest padding. It did not affect the tiled candidate
// that surfaced it (tiled layouts are refused outright), but it is corrected
// before any acquisition can reach it.
//   6 = k_8_8_8_8   18 = k_DXT1   20 = k_DXT4_5   24 = k_16   49 = k_DXN
// Compressed formats are NOT listed: they are block-based, so "bytes per pixel"
// is the wrong question and 0 (unsupported) is the honest answer.
inline uint32_t linearBytesPerPixel(uint32_t fmt) {
  switch (fmt) {
    case 2:  return 4;    // 8:8:8:8
    case 3:  return 4;
    case 6:  return 4;    // k_8_8_8_8
    case 23: return 4;
    case 24: return 2;    // k_16 - ONE 16-bit channel (was wrongly 1)
    case 29: return 4;
    case 31: return 4;
    default: return 0;
  }
}

inline Footprint realFootprint(uint32_t fmtBits, uint32_t dims, uint32_t pitch) {
  Footprint f;
  const uint32_t fmt = fmtBits & 0xFFu;
  const bool tiled = ((fmtBits >> 8) & 1u) != 0;
  const uint32_t w = dims >> 16, h = dims & 0xFFFFu;
  if (!w || !h || !pitch) return f;
  // ⚠ TILED IS REFUSED, not approximated. A tiled surface's real bytes are not a
  // prefix of each row, so "rowBytes at the start of the row" is simply wrong for
  // it, and a padding-only change could masquerade as content. The director's
  // contract allows refusing a layout; it does not allow guessing one.
  if (tiled) return f;
  const uint32_t bpp = linearBytesPerPixel(fmt);
  if (!bpp) return f;
  const uint64_t rowBytes = (uint64_t)w * bpp;
  if (rowBytes > pitch) return f;      // pitch cannot be narrower than the row
  f.ok = true;
  f.rowBytes = (uint32_t)rowBytes;
  f.rows = h;
  f.pitch = pitch;
  f.realBytes = rowBytes * h;
  f.spanBytes = (uint64_t)pitch * h;
  return f;
}

// Where the two versions differ INSIDE the real footprint. `changedBytes` counts
// only real bytes; `firstOffset` is the byte offset within the span, so the
// location is reportable rather than merely a count.
struct Diff {
  uint64_t changedBytes = 0;
  uint64_t firstOffset = 0;
  uint32_t firstRow = 0;
  bool     anyPaddingChange = false;   // differs outside the footprint too
};

inline Diff diffFootprint(const uint8_t* v0, const uint8_t* v1,
                          const Footprint& f) {
  Diff d;
  bool haveFirst = false;
  for (uint32_t r = 0; r < f.rows; r++) {
    const uint64_t base = (uint64_t)r * f.pitch;
    for (uint32_t i = 0; i < f.rowBytes; i++) {
      if (v0[base + i] != v1[base + i]) {
        d.changedBytes++;
        if (!haveFirst) {
          haveFirst = true;
          d.firstOffset = base + i;
          d.firstRow = r;
        }
      }
    }
    for (uint64_t i = f.rowBytes; i < f.pitch; i++)
      if (v0[base + i] != v1[base + i]) d.anyPaddingChange = true;
  }
  return d;
}

// ── THE QUALIFICATION RULE ────────────────────────────────────────────────────
// Pure, so it is testable without a roll. Order matters: epoch and publication
// disqualifications are checked BEFORE content, because a pair that crossed an
// allocation or a publication is not a version pair no matter how its bytes
// compare.
inline BankVerdict qualifyPair(const EpochId& e0, const EpochId& e1,
                               const Serials& s0, const Serials& s1,
                               const Footprint& f, const Diff& d) {
  if (e0 != e1) return BankVerdict::EpochChanged;
  if (s1.unlockPublishes != s0.unlockPublishes)
    return BankVerdict::UnlockIntervened;
  if (s1.resolveOwnerships != s0.resolveOwnerships)
    return BankVerdict::ResolveIntervened;
  if (!f.ok) return BankVerdict::UnsupportedLayout;
  if (d.changedBytes == 0)
    // Nonzero->nonzero is valid; only "no real change" disqualifies. A change
    // confined to padding reports PaddingOnly so the two are distinguishable in
    // the census rather than both reading as "nothing happened".
    return d.anyPaddingChange ? BankVerdict::PaddingOnly
                              : BankVerdict::SameContent;
  return BankVerdict::Qualified;
}

// ── HARD COST LIMITS ──────────────────────────────────────────────────────────
// The director's boundary, verbatim: four Presents, 512 MiB examined, 256 MiB
// banked, 1024 records. Overflow does not silently truncate — it sets
// `overflowed`, and an overflowed run cannot support a negative result.
struct BankLimits {
  uint32_t maxPresents   = 4;
  uint64_t maxExamined   = 512ull * 1024 * 1024;
  uint64_t maxBanked     = 256ull * 1024 * 1024;
  uint32_t maxRecords    = 1024;
};

class VersionBank {
 public:
  struct Record {
    EpochId  epoch;
    uint32_t rawFetch[6] = {};    // the UNDECODED fetch constant, carried so a
                                  // reader can re-derive the decode offline
                                  // rather than trust this decode
    Serials  s0, s1;
    uint64_t v0Hash = 0, v1Hash = 0;
    uint64_t v0NonZero = 0, v1NonZero = 0;
    uint64_t spanBytes = 0, realBytes = 0;
    uint64_t changedBytes = 0, firstChangedOffset = 0;
    uint32_t firstChangedRow = 0;
    uint32_t seedPresent = 0, observePresent = 0;
    uint32_t sampler = 0, bindOrdinal = 0;
    std::vector<uint8_t> v1;      // raw banked bytes, observer-side ONLY
  };

  explicit VersionBank(BankLimits lim = {}) : m_lim(lim) {}

  void noteExamined(uint64_t bytes) {
    m_examined += bytes;
    if (m_examined > m_lim.maxExamined) { m_overflow = true; }
  }
  bool examinedExhausted() const { return m_examined > m_lim.maxExamined; }

  void noteVerdict(BankVerdict v) { m_counts[(uint32_t)v]++; }

  // Returns false and counts CapacityExceeded if a limit stops the bank. The
  // caller must NOT treat that as "nothing changed".
  bool bank(Record&& r) {
    if (m_records.size() >= m_lim.maxRecords ||
        m_banked + r.v1.size() > m_lim.maxBanked) {
      m_overflow = true;
      m_counts[(uint32_t)BankVerdict::CapacityExceeded]++;
      return false;
    }
    m_banked += r.v1.size();
    m_records.push_back(std::move(r));
    m_counts[(uint32_t)BankVerdict::Qualified]++;
    return true;
  }

  const std::vector<Record>& records() const { return m_records; }
  uint64_t examined() const { return m_examined; }
  uint64_t banked() const { return m_banked; }
  bool overflowed() const { return m_overflow; }
  uint64_t count(BankVerdict v) const { return m_counts[(uint32_t)v]; }
  const BankLimits& limits() const { return m_lim; }

  // The director: overflow "invalidates a negative 'none found' result, but does
  // not invalidate a fully evidenced positive candidate."
  bool negativeResultIsValid() const { return !m_overflow; }

  void reset() {
    m_records.clear();
    m_examined = m_banked = 0;
    m_overflow = false;
    for (uint64_t& c : m_counts) c = 0;
  }

 private:
  BankLimits m_lim;
  std::vector<Record> m_records;
  uint64_t m_examined = 0, m_banked = 0;
  bool m_overflow = false;
  uint64_t m_counts[(uint32_t)BankVerdict::Count] = {};
};

// ── GATE VALIDITY ─────────────────────────────────────────────────────────────
// "Conflicting settings must refuse acquisition, not silently choose behavior."
// Taking the lookup as a parameter is what makes the PRODUCTION decision
// testable: production passes std::getenv, a test passes a stub. (Same lesson as
// the trace-arm counting, which passed its tests while binding nothing.)
struct AcquireEnv {
  bool acquire = false;       // SR_HLE_TEX_VERSION_ACQUIRE=1
  bool haveTraceRec = false;  // SR_HLE_TRACE_REC set to a path
  bool hotkey = false;        // SR_HLE_TRACE_HOTKEY=1
  int  frames = 0;            // SR_HLE_TRACE_FRAMES
  bool refresh = false;       // SR_HLE_TEX_REFRESH enabled
  bool refreshFilters = false;// any refresh key/filter/from-present gate set
};

enum class AcquireGate : uint32_t {
  Off = 0,            // not requested; nothing runs
  Valid,
  NoTracePath,
  NoHotkey,
  WrongFrames,
  RefreshEnabled,
  RefreshFiltersSet,
};

inline const char* acquireGateName(AcquireGate g) {
  switch (g) {
    case AcquireGate::Off:               return "Off";
    case AcquireGate::Valid:             return "Valid";
    case AcquireGate::NoTracePath:       return "NoTracePath";
    case AcquireGate::NoHotkey:          return "NoHotkey";
    case AcquireGate::WrongFrames:       return "WrongFrames";
    case AcquireGate::RefreshEnabled:    return "RefreshEnabled";
    case AcquireGate::RefreshFiltersSet: return "RefreshFiltersSet";
    default:                             return "?";
  }
}

inline AcquireGate acquireGate(const AcquireEnv& e) {
  if (!e.acquire) return AcquireGate::Off;
  if (!e.haveTraceRec) return AcquireGate::NoTracePath;
  if (!e.hotkey) return AcquireGate::NoHotkey;
  if (e.frames != 4) return AcquireGate::WrongFrames;
  if (e.refresh) return AcquireGate::RefreshEnabled;
  if (e.refreshFilters) return AcquireGate::RefreshFiltersSet;
  return AcquireGate::Valid;
}

inline bool acquireActive(const AcquireEnv& e) {
  return acquireGate(e) == AcquireGate::Valid;
}


// ── THE SIDECAR (.texv) ───────────────────────────────────────────────────────
// The director: keep the executable trace SRHT v5 unchanged, and put the v1
// payloads in a SIDECAR. "Do not invent an executable 'observer TextureUpload'
// or reinterpret v5." So `target.srht` stays byte-for-byte an ordinary trace
// carrying only v0, and v1 exists ONLY here.
//
// ⚠ DEVIATION, STATED RATHER THAN HIDDEN: the ruling asks for SHA-256 in the
// manifest. There is no SHA-256 in this tree (only xxhash), and adding a crypto
// implementation to the engine to write a diagnostic manifest is more
// engine-side surface than the job needs. So:
//   * the ENGINE writes target.srht and target.texv, and binds them with the
//     trace's own header totals plus a per-run nonce;
//   * `tools/texv_manifest.py` computes the SHA-256s and sizes OFFLINE.
// A sidecar whose recorded totals do not match the .srht it names is refused, so
// the mismatch case the ruling cares about is still detected in-engine.
struct SidecarHeader {
  char     magic[4];        // "TEXV"
  uint32_t version;         // 1
  uint64_t records;
  uint64_t payloadBytes;
  uint64_t nonce;           // per-run, also written to the run log
  uint64_t srhtRecords;     // the .srht header totals this sidecar belongs to
  uint64_t srhtPayloadBytes;
  uint32_t srhtPresents;
  uint32_t flags;           // bit 0 = the run overflowed a capacity limit
};
static_assert(sizeof(SidecarHeader) == 56, "sidecar header layout is on-disk");

constexpr uint32_t kSidecarVersion = 1;
constexpr uint32_t kSidecarOverflowed = 1u << 0;

// One fixed-size row per banked version pair; the raw v1 bytes follow it.
struct SidecarRecord {
  uint32_t key, headerIdent;
  uint32_t rawFetch[6];          // the undecoded fetch constant
  uint32_t fmtBits, dims, pitch, dimLevel, aliasLo, aliasHi;
  uint64_t allocGeneration;
  uint32_t descEpoch, pad0;
  uint64_t v0Hash, v1Hash;
  uint64_t v0NonZero, v1NonZero;
  uint64_t spanBytes, realBytes;
  uint64_t changedBytes, firstChangedOffset;
  uint32_t firstChangedRow, seedPresent, observePresent, sampler;
  uint32_t bindOrdinal, pad1;
  uint64_t s0Unlock, s0Resolve, s1Unlock, s1Resolve;
  uint32_t payloadBytes, pad2;
  uint64_t payloadHash;
};
static_assert(sizeof(SidecarRecord) == 208, "sidecar record layout is on-disk");

struct TraceTotals {
  uint64_t records = 0;
  uint64_t payloadBytes = 0;
  uint32_t presents = 0;
};


// Writes the sidecar. File I/O only — still no publication API is reachable from
// here, which is the whole point of the boundary.
//
// Returns false on ANY write failure. A partially written sidecar is worse than
// none: it would be refused offline anyway, and a silent short write would let a
// treatment arm be built from a truncated v1.
inline bool writeSidecar(const char* path, const VersionBank& bank,
                         const TraceTotals& trace, uint64_t nonce) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  uint64_t payloadTotal = 0;
  for (const VersionBank::Record& r : bank.records())
    payloadTotal += r.v1.size();

  SidecarHeader h{};
  h.magic[0] = 'T'; h.magic[1] = 'E'; h.magic[2] = 'X'; h.magic[3] = 'V';
  h.version = kSidecarVersion;
  h.records = bank.records().size();
  h.payloadBytes = payloadTotal;
  h.nonce = nonce;
  h.srhtRecords = trace.records;
  h.srhtPayloadBytes = trace.payloadBytes;
  h.srhtPresents = trace.presents;
  h.flags = bank.overflowed() ? kSidecarOverflowed : 0u;
  bool ok = std::fwrite(&h, sizeof(h), 1, f) == 1;

  for (const VersionBank::Record& r : bank.records()) {
    SidecarRecord rec{};
    rec.key = r.epoch.key;
    rec.headerIdent = r.epoch.headerIdent;
    for (int i = 0; i < 6; i++) rec.rawFetch[i] = r.rawFetch[i];
    rec.fmtBits = r.epoch.fmtBits;
    rec.dims = r.epoch.dims;
    rec.pitch = r.epoch.pitch;
    rec.dimLevel = r.epoch.dimLevel;
    rec.aliasLo = r.epoch.aliasLo;
    rec.aliasHi = r.epoch.aliasHi;
    rec.allocGeneration = r.epoch.allocGeneration;
    rec.descEpoch = r.epoch.descEpoch;
    rec.v0Hash = r.v0Hash;
    rec.v1Hash = r.v1Hash;
    rec.v0NonZero = r.v0NonZero;
    rec.v1NonZero = r.v1NonZero;
    rec.spanBytes = r.spanBytes;
    rec.realBytes = r.realBytes;
    rec.changedBytes = r.changedBytes;
    rec.firstChangedOffset = r.firstChangedOffset;
    rec.firstChangedRow = r.firstChangedRow;
    rec.seedPresent = r.seedPresent;
    rec.observePresent = r.observePresent;
    rec.sampler = r.sampler;
    rec.bindOrdinal = r.bindOrdinal;
    rec.s0Unlock = r.s0.unlockPublishes;
    rec.s0Resolve = r.s0.resolveOwnerships;
    rec.s1Unlock = r.s1.unlockPublishes;
    rec.s1Resolve = r.s1.resolveOwnerships;
    rec.payloadBytes = (uint32_t)r.v1.size();
    rec.payloadHash = r.v1Hash;
    ok = ok && std::fwrite(&rec, sizeof(rec), 1, f) == 1;
    ok = ok && std::fwrite(r.v1.data(), 1, r.v1.size(), f) == r.v1.size();
  }
  ok = (std::fclose(f) == 0) && ok;
  return ok;
}

}  // namespace sr::gfx::hle
