// cube_bind_rule.hpp -- H1(b) cube resource/SRV/upload/bind decisions, as PURE
// functions so they can be driven by a self-test without a D3D12 device.
//
// Everything here decides; nothing here calls D3D12. The backend applies these
// answers. That split is what let the H1(a) layout and capture rules be
// mutation-tested (gfx/cube_layout.hpp, kernel/cube_capture_rule.hpp), and the
// same reason applies here: a cube that binds into the wrong descriptor band
// samples a different texture and produces a plausible wrong picture rather
// than a crash.
//
// THE CONTRACT (director, iter-457 H1(b) authorization):
//   * resource is TEXTURE2D with DepthOrArraySize = 6
//   * SRV is D3D12_SRV_DIMENSION_TEXTURECUBE
//   * real cube indices are 1..7; index 0 stays the existing white dummy
//   * all six faces and all represented mips upload, with NAMED fail-closed
//     refusals
//   * every live AND replay bind writes ALL THREE descriptor bands: the real
//     index only in the dimensionality-matching band, ZERO in the other two
#pragma once

#include <cstdint>

#include "../kernel/sr_oracle.hpp"

namespace sr {
namespace gfx {
namespace cubebind {

// Xenos DIMENSION, as decoded from the fetch constant (dword 5 >> 9) & 3.
enum class Dim : uint8_t {
  OneD = 0,
  TwoD = 1,
  ThreeD = 2,
  Cube = 3,
};

// The descriptor bands in the shared constant block. 0..15 Texture2D,
// 16..31 Texture3D, 32..47 TextureCube, 48..63 samplers, 64 bools.
inline constexpr uint32_t kBand2D = 0;
inline constexpr uint32_t kBand3D = 16;
inline constexpr uint32_t kBandCube = 32;

// The cube descriptor table holds 8 entries. Index 0 is the white 6-face
// dummy the backend creates at init, so a REAL cube may only take 1..7.
inline constexpr uint32_t kCubeDummyIndex = 0;
inline constexpr uint32_t kCubeFirstRealIndex = 1;
inline constexpr uint32_t kCubeLastRealIndex = 7;
inline constexpr uint32_t kCubeFaces = 6;

// ── WHY A REQUEST WAS REFUSED ───────────────────────────────────────────────
// Every refusal is named. An unnamed refusal is indistinguishable from "we
// silently drew the dummy", which is the failure mode this whole slice exists
// to remove.
enum class CubeRefuse : uint8_t {
  None = 0,
  NotCube,          // the fetch dimension is not CUBE
  ZeroExtent,       // width or height is 0
  NonSquare,        // a cube face must be square
  ExtentTooLarge,   // beyond what we will allocate
  UnsupportedFormat,
  NoMipLevels,      // mipLevels must be >= 1
  TooManyMips,      // more mips than the extent can hold
  NoSlot,           // indices 1..7 exhausted
  FaceDataMissing,  // fewer than six faces present
  MipDataMissing,   // a represented mip has no bytes
  ByteCountMismatch,
  // Post-submit failures: preserved through the caller boundary so a log line
  // names WHICH submission step failed, not just "something failed".
  CloseFailed,
  SignalFailed,
  EventFailed,
  WaitFailed,
  // r6: EXACT request validation (gfx/cube_transaction.hpp validateRequest),
  // decided before any slot or device work. Each is its own name so a
  // counterexample fails by the reason that actually caught it.
  IndexOutOfRange,    // a subresource index >= depthOrArraySize * mipLevels
  IndexDuplicate,     // the same subresource index appears twice
  IndexMissing,       // a subresource index appears zero times
  ExtentMismatch,     // w/h is not that (face, mip)'s extent
  RowCountMismatch,   // rows != block rows for that extent and format
  RowBytesMismatch,   // rowBytes != blocks-per-row * bytes-per-block
  BackingMismatch,    // lin.size() != rows * rowBytes
  // r6: the render thread does not own Present (SR_HLE_OWN_PRESENT=0), so the
  // graphics-fence stamp a same-key refresh retires behind is not
  // synchronized with the thread that advances it. Fail closed.
  PresentNotOwned,
};

// SR_ORACLE=0 (kernel/sr_oracle.hpp): cube_bind_rule.cpp is compiled out with its only callers, the HLE renderer's cube
// upload and bind (d3d12_backend.cpp hleUploadCube / hleSubmitDraw, the SR_PM4_LIVE=0 A/B). The decisions below are
// declared only with SR_ORACLE=1; the types stay (gfx/cube_transaction.hpp, d3d12_backend.hpp).
#if SR_ORACLE
const char* refuseName(CubeRefuse r);
#endif

// ── THE BAND PLAN ───────────────────────────────────────────────────────────
// One slot, one dimensionality, three bands. The real index lands in the band
// that matches the resource, and the other two are ZEROED.
//
// ⛔ WHY ZEROING MATTERS. Before this, the publisher wrote the 2D index and
// the 3D index for the same slot unconditionally and left the cube band at its
// zero-init. That is harmless only while a key can never appear in two tables.
// The moment it can - a key re-created at a different dimensionality, a stale
// entry that outlived its resource - a shader compiled for one dimension reads
// a live index belonging to another, and samples a completely different
// texture. Zeroing is what makes that structurally impossible rather than
// merely unlikely.
struct BandPlan {
  uint32_t band2D = 0;
  uint32_t band3D = 0;
  uint32_t bandCube = 0;
};

// `idxCube` is consulted only when dim == Cube, and so on. A dimension with no
// real index resolves to 0, which is each band's dummy - the SAME picture as
// before this slice, never a wrong one.
#if SR_ORACLE
BandPlan planBands(Dim dim, uint32_t idx2D, uint32_t idx3D, uint32_t idxCube);
#endif

// The dimensionality a slot should bind at, derived from WHICH descriptor
// table owns the key rather than from the fetch shadow. The tables are what
// the descriptors actually are; the shadow is a decoded copy that can be stale.
// Cube wins over volume wins over 2D, because a key is only ever created into
// one of them and a leftover in another is exactly the staleness being guarded.
#if SR_ORACLE
Dim dimFromTables(bool hasCube, bool hasVolume);
#endif

// ── THE RESOURCE PLAN ───────────────────────────────────────────────────────
struct CubeResourcePlan {
  bool ok = false;
  CubeRefuse refuse = CubeRefuse::None;
  uint32_t width = 0;
  uint32_t height = 0;
  // ⛔ TEXTURE2D with SIX ARRAY SLICES, not a TEXTURE3D of depth 6. D3D12
  // has no cube resource dimension: a cube IS a 2D array of six slices, and
  // only the SRV says "cube". Creating a TEXTURE3D here would compile, bind,
  // and sample the wrong thing.
  uint16_t depthOrArraySize = 0;
  uint16_t mipLevels = 0;
  uint32_t subresourceCount = 0;   // faces * mips
};

#if SR_ORACLE
CubeResourcePlan planResource(Dim dim, uint32_t width, uint32_t height,
                              uint32_t mipLevels, bool formatSupported);

// Subresource index for (face, mip) in a 2D array: D3D12 orders them
// mip-major within each array slice.
uint32_t subresourceIndex(uint32_t face, uint32_t mip, uint32_t mipLevels);
#endif

// ── THE UPLOAD PLAN ─────────────────────────────────────────────────────────
// Fail-closed: every face and every represented mip must be present, or the
// upload is refused by name and NOTHING is written. A partial cube is worse
// than the dummy, because it looks like it worked.
struct CubeUploadPlan {
  bool ok = false;
  CubeRefuse refuse = CubeRefuse::None;
  uint32_t faces = 0;
  uint32_t mipLevels = 0;
  uint32_t subresources = 0;
};

// ⛔ WHY FAIL-CLOSED IS THE WHOLE DESIGN. Five faces plus the dummy's sixth
// is a cube that renders - wrongly, in one direction only, and it looks like
// a bug in the scene rather than in the upload.
#if SR_ORACLE
CubeUploadPlan planUpload(const CubeResourcePlan& res, uint32_t facesPresent,
                          uint32_t mipsPresent, uint64_t bytesProvided,
                          uint64_t bytesExpected);
#endif

// ── SLOT ALLOCATION ───────────────────────────────────────────────────────────
// r5: the indices 1..7 are a FREE LIST, not a cursor - see slotAcquire /
// slotRelease in gfx/cube_transaction.hpp. The r2-r4 cursor (allocateIndex /
// advanceIndex) could not express "an old slot returns once the graphics
// fence proves no frame samples it", which versioned publication needs.

// ── POST-SUBMIT RESULT ──────────────────────────────────────────────────────
// uploadCubeSubresources ignored Close/Signal/SetEventOnCompletion/Wait
// HRESULTs and published after a timeout. These are the named outcomes a
// test can drive without a device.
enum class SubmitStatus : uint8_t {
  Ok = 0,
  CloseFailed,
  SignalFailed,
  EventFailed,
  WaitFailed,
};

inline const char* submitStatusName(SubmitStatus s) {
  switch (s) {
    case SubmitStatus::Ok: return "Ok";
    case SubmitStatus::CloseFailed: return "CloseFailed";
    case SubmitStatus::SignalFailed: return "SignalFailed";
    case SubmitStatus::EventFailed: return "EventFailed";
    case SubmitStatus::WaitFailed: return "WaitFailed";
  }
  return "Unknown";
}

// Convert a non-Ok SubmitStatus to the corresponding CubeRefuse. This is
// where the exact failure category crosses the boundary from the submission
// helper to the caller-visible refusal evidence.
inline CubeRefuse submitToRefuse(SubmitStatus s) {
  switch (s) {
    case SubmitStatus::CloseFailed:  return CubeRefuse::CloseFailed;
    case SubmitStatus::SignalFailed: return CubeRefuse::SignalFailed;
    case SubmitStatus::EventFailed:  return CubeRefuse::EventFailed;
    case SubmitStatus::WaitFailed:   return CubeRefuse::WaitFailed;
    default:                         return CubeRefuse::ByteCountMismatch;
  }
}

// HRESULT failed == high bit set. Wait is a separate BOOL so a timeout is
// distinguishable from a failed SetEventOnCompletion.
inline SubmitStatus classifySubmit(int32_t hrClose, int32_t hrSignal,
                                   int32_t hrEvent, bool waitOk) {
  auto failed = [](int32_t hr) -> bool { return hr < 0; };
  if (failed(hrClose)) return SubmitStatus::CloseFailed;
  if (failed(hrSignal)) return SubmitStatus::SignalFailed;
  if (failed(hrEvent)) return SubmitStatus::EventFailed;
  if (!waitOk) return SubmitStatus::WaitFailed;
  return SubmitStatus::Ok;
}

}  // namespace cubebind
}  // namespace gfx
}  // namespace sr
