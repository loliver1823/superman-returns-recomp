#pragma once
// iter-458 H-PAIR: the guest D3D runtime's own VS-input <-> vertex-decl pairing
// rule, as a pure function so it can be tested without a device.
//
// WHY THIS EXISTS. The D3D runtime vfetch-PATCHES every vertex shader's ucode at
// draw time (ENGINE_MAP §6d, iter-359): GpuEmitDrawState_sub_82108668 ->
// sub_82108520 (in place, when the GPU is done with the ucode) or
// GpuLoadShaderImmediate_sub_82108300 (a patched copy in the ring). Our HLE
// producer hashes the VS in publishDrawState BEFORE __imp__sub_820FBBF8 runs,
// so the cache key -- and the .vfetch sidecar behind it -- describes the
// UNPATCHED container: every fetch reads slot 95, offset 0, stride 0, format 0.
// OFFPAIR (d3d12_backend.cpp) pairs by the sidecar's offsets, which for such a
// VS maps every input onto the POSITION bytes at offset 0.
//
// THE RULE, from ShaderAssembleVertexFetches_sub_82107A30 (IDA, 2026-09-25):
// for each entry in the container's vfetch table (usage, usageIndex), take the
// FIRST decl element, in decl order and on ANY stream, with equal
// (usage, usageIndex). Patch: fetch slot = 95 - stream, offset = the element's
// byte offset, format = decl type & 0x3F, formatCompAll (signed) = type bit 8,
// numFormatAll (integer) = type bit 9, stride = dev+12256[stream].
// NO match: dword0 |= 0x5F00000 (constIndex 31, select 2 = slot 95), dword1 =
// (w & 0xBFC0CFFF) | 0x60000 (format 6 = k_8_8_8_8, unsigned, normalized,
// isMiniFetch cleared), dword2 = strides[0] with offset 0. Bit positions per
// XenosRecomp shader_code.h VertexFetchInstruction.
//
// SCOPE. The producer snapshots ONE stream, so an input the rule resolves to
// another stream gets the same bounded dummy OFFPAIR uses (offset 0, decoded
// in that input's format) -- never a cross-stream misread. There is still no
// multi-stream path.
#include <cstdint>

#include "shader_cache.hpp"

namespace sr::gfx::vfpair {

struct DeclEl {
  uint32_t off, stream, type, usage, usageIndex;
};

enum class How : uint8_t {
  Matched,       // first semantic match, on the gathered stream
  Default,       // no decl element carries this semantic: the guest default
  DummyForeign,  // resolved to a stream the producer did not snapshot
  DummyBad,      // matched, but the format is undecodable or the element does
                 // not lie wholly inside the vertex record
};

constexpr uint32_t kDefaultFormat = 6u;  // ucode::VertexFormat::k_8_8_8_8

// A sidecar written from UNPATCHED ucode: every fetch has offset, stride and
// format zero. Astra 048: zero fields alone are not proof, so the metadata
// must also be non-empty -- at least one input must carry a semantic that the
// draw's decl names (checked by the caller via anySemanticMatch).
inline bool sidecarUnpatched(const VfetchElement* sv, uint32_t n) {
  if (!n) return false;
  for (uint32_t i = 0; i < n; i++)
    if (sv[i].offsetDwords != 0 || sv[i].strideDwords != 0 || sv[i].format != 0)
      return false;
  return true;
}

inline int firstSemanticMatch(const VfetchElement& sv, const DeclEl* de, uint32_t nDecl) {
  for (uint32_t j = 0; j < nDecl; j++)
    if (de[j].usage == sv.usage && de[j].usageIndex == sv.usageIndex) return int(j);
  return -1;
}

inline bool anySemanticMatch(const VfetchElement* sv, uint32_t n, const DeclEl* de,
                             uint32_t nDecl) {
  for (uint32_t i = 0; i < n; i++)
    if (firstSemanticMatch(sv[i], de, nDecl) >= 0) return true;
  return false;
}

// One input. fmtBytes = the backend's xeFormatBytes (0 = undecodable).
inline VfetchElement pairGuestRule(const VfetchElement& sv, const DeclEl* de,
                                   uint32_t nDecl, uint32_t gatherStream,
                                   uint32_t srcStride,
                                   uint32_t (*fmtBytes)(uint32_t), How* how) {
  VfetchElement out{};
  out.usage = sv.usage;
  out.usageIndex = sv.usageIndex;
  const int k = firstSemanticMatch(sv, de, nDecl);
  const uint32_t stream = k >= 0 ? de[k].stream : 0u;
  const uint32_t fmt = k >= 0 ? (de[k].type & 0x3Fu) : kDefaultFormat;
  const uint32_t fb = fmtBytes(fmt);
  auto dummy = [&](How h) {
    out.offsetDwords = 0;
    out.format = fb ? fmt : 38u;  // OFFPAIR's fallback (38 = k_32_32_32_32_FLOAT)
    out.isSigned = out.isInteger = out.bgra = false;
    if (how) *how = h;
    return out;
  };
  if (stream != gatherStream) return dummy(How::DummyForeign);
  const uint32_t off = k >= 0 ? de[k].off : 0u;
  if (!fb || (off % 4u) || off + fb > srcStride) return dummy(How::DummyBad);
  out.offsetDwords = int32_t(off / 4u);
  out.format = fmt;
  if (k >= 0) {
    out.isSigned = ((de[k].type >> 8) & 1u) != 0;
    out.isInteger = ((de[k].type >> 9) & 1u) != 0;
    out.bgra = ((de[k].type >> 11) & 1u) != 0;
    if (how) *how = How::Matched;
  } else {
    if (how) *how = How::Default;  // unsigned, normalized, no swizzle
  }
  return out;
}

}  // namespace sr::gfx::vfpair
