#include "cube_bind_rule.hpp"

namespace sr {
namespace gfx {
namespace cubebind {

const char* refuseName(CubeRefuse r) {
  switch (r) {
    case CubeRefuse::None: return "None";
    case CubeRefuse::NotCube: return "NotCube";
    case CubeRefuse::ZeroExtent: return "ZeroExtent";
    case CubeRefuse::NonSquare: return "NonSquare";
    case CubeRefuse::ExtentTooLarge: return "ExtentTooLarge";
    case CubeRefuse::UnsupportedFormat: return "UnsupportedFormat";
    case CubeRefuse::NoMipLevels: return "NoMipLevels";
    case CubeRefuse::TooManyMips: return "TooManyMips";
    case CubeRefuse::NoSlot: return "NoSlot";
    case CubeRefuse::FaceDataMissing: return "FaceDataMissing";
    case CubeRefuse::MipDataMissing: return "MipDataMissing";
    case CubeRefuse::ByteCountMismatch: return "ByteCountMismatch";
    case CubeRefuse::CloseFailed: return "CloseFailed";
    case CubeRefuse::SignalFailed: return "SignalFailed";
    case CubeRefuse::EventFailed: return "EventFailed";
    case CubeRefuse::WaitFailed: return "WaitFailed";
    case CubeRefuse::IndexOutOfRange: return "IndexOutOfRange";
    case CubeRefuse::IndexDuplicate: return "IndexDuplicate";
    case CubeRefuse::IndexMissing: return "IndexMissing";
    case CubeRefuse::ExtentMismatch: return "ExtentMismatch";
    case CubeRefuse::RowCountMismatch: return "RowCountMismatch";
    case CubeRefuse::RowBytesMismatch: return "RowBytesMismatch";
    case CubeRefuse::BackingMismatch: return "BackingMismatch";
    case CubeRefuse::PresentNotOwned: return "PresentNotOwned";
  }
  return "Unknown";
}

BandPlan planBands(Dim dim, uint32_t idx2D, uint32_t idx3D, uint32_t idxCube) {
  BandPlan p;
  switch (dim) {
    case Dim::Cube:
      p.bandCube = idxCube;
      break;
    case Dim::ThreeD:
      p.band3D = idx3D;
      break;
    case Dim::OneD:
    case Dim::TwoD:
    default:
      p.band2D = idx2D;
      break;
  }
  return p;
}

Dim dimFromTables(bool hasCube, bool hasVolume) {
  if (hasCube) return Dim::Cube;
  if (hasVolume) return Dim::ThreeD;
  return Dim::TwoD;
}

uint32_t subresourceIndex(uint32_t face, uint32_t mip, uint32_t mipLevels) {
  return face * mipLevels + mip;
}

static uint32_t maxMipsFor(uint32_t extent) {
  uint32_t n = 0;
  while (extent) {
    ++n;
    extent >>= 1;
  }
  return n;                      // 1x1 -> 1, 2x2 -> 2, 128x128 -> 8
}

CubeResourcePlan planResource(Dim dim, uint32_t width, uint32_t height,
                              uint32_t mipLevels, bool formatSupported) {
  CubeResourcePlan p;
  if (dim != Dim::Cube) { p.refuse = CubeRefuse::NotCube; return p; }
  if (!width || !height) { p.refuse = CubeRefuse::ZeroExtent; return p; }
  // A cube face is square by definition. A non-square request means we
  // decoded something that is not a cube, and guessing an extent here would
  // sample garbage rather than fail.
  if (width != height) { p.refuse = CubeRefuse::NonSquare; return p; }
  if (width > 4096u) { p.refuse = CubeRefuse::ExtentTooLarge; return p; }
  if (!formatSupported) { p.refuse = CubeRefuse::UnsupportedFormat; return p; }
  if (!mipLevels) { p.refuse = CubeRefuse::NoMipLevels; return p; }
  if (mipLevels > maxMipsFor(width)) {
    p.refuse = CubeRefuse::TooManyMips; return p; }

  p.ok = true;
  p.width = width;
  p.height = height;
  p.depthOrArraySize = (uint16_t)kCubeFaces;   // SIX SLICES, TEXTURE2D
  p.mipLevels = (uint16_t)mipLevels;
  p.subresourceCount = kCubeFaces * mipLevels;
  return p;
}

CubeUploadPlan planUpload(const CubeResourcePlan& res, uint32_t facesPresent,
                          uint32_t mipsPresent, uint64_t bytesProvided,
                          uint64_t bytesExpected) {
  CubeUploadPlan u;
  if (!res.ok) { u.refuse = res.refuse; return u; }
  // ⛔ FAIL-CLOSED, ALL SIX. Five faces plus the dummy's sixth is a cube that
  // renders - wrongly, in one direction only, and it looks like a bug in the
  // scene rather than in the upload.
  if (facesPresent != kCubeFaces) {
    u.refuse = CubeRefuse::FaceDataMissing; return u; }
  if (mipsPresent != res.mipLevels) {
    u.refuse = CubeRefuse::MipDataMissing; return u; }
  if (!bytesExpected || bytesProvided != bytesExpected) {
    u.refuse = CubeRefuse::ByteCountMismatch; return u; }

  u.ok = true;
  u.faces = kCubeFaces;
  u.mipLevels = res.mipLevels;
  u.subresources = res.subresourceCount;
  return u;
}

}  // namespace cubebind
}  // namespace gfx
}  // namespace sr
