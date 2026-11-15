// draw_contract.hpp — the TARGET-ONLY draw-contract observer.
//
// iter-456p, director's next arc. One row, for one requested draw ordinal, from
// EITHER replay path, so an oracle draw and its structural counterpart in our
// own fixture can be diffed field by field:
//
//     Run it first for oracle/XTR ordinal 2089 and HLE fixture ordinal 949.
//     Stop after the two-row field diff; do not repair anything in the same
//     slice.
//
// WHY IT EXISTS: the seven-slot descriptor fingerprint of oracle draw 2089
// matches exactly one draw per scene frame in our fixture, and for draw 949 the
// slot-15 key is `0B5BE000`. But suppressing 949 left all 16 presented frames
// byte-identical — our counterpart is already PIXEL-INERT. A resemblance plus a
// null is not a mapping, so the next question is which part of the draw contract
// actually differs.
//
// ⛔ OBSERVER ONLY, and default OFF. It reads state that has already been
// assembled and formats a line. It binds nothing, changes no state, and skips no
// draw; inertness is proved by unchanged baseline frame hashes, not asserted.
//
// ⚠ BIG-ENDIAN FIRST. Guest vertex and index bytes are hashed in their ORIGINAL
// guest order, before any host-side decode or byte-swap. Hashing after a decode
// would compare our interpretation of the bytes rather than the bytes, and two
// paths that decode differently would then look identical exactly where they
// differ most.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace sr::gfx {

// Which replay produced the row. Both paths emit the SAME field set so the two
// rows are diffable line-for-line; a field a path cannot know is emitted as
// `-` rather than silently defaulted to zero, because a zero would read as a
// measured value.
enum class DrawContractSource : uint32_t { PM4 = 0, HLE = 1 };

struct DrawContractVFetch {
  uint32_t slot = 0;
  uint32_t raw[2] = {};      // the two-dword vertex fetch constant, raw
  uint32_t strideWords = 0;
  uint32_t attrCount = 0;
  bool     present = false;
};

struct DrawContractTex {
  uint32_t slot = 0;
  uint32_t raw[6] = {};      // the six-dword texture fetch constant, raw
  uint32_t key = 0;          // normalized pixel key, where the path knows it
  bool     present = false;
  // The HLE side knows the KEY but not the raw six-dword descriptor; printing
  // zeroes there read as a measured all-zero descriptor.
  bool     haveRaw = false;
};

struct DrawContract {
  uint64_t ordinal = 0;
  DrawContractSource source = DrawContractSource::PM4;

  // shader pair
  uint64_t vsHash = 0, psHash = 0;
  uint32_t vsBytes = 0, psBytes = 0;
  bool     haveShaders = false;
  // ⚠ SEPARATE from haveShaders. The hashes are known long before the ucode
  // sizes are; one flag for both printed measured-looking zeroes for sizes
  // nobody had wired.
  bool     haveVsBytes = false, havePsBytes = false;

  // primitive / index contract
  uint32_t primType = 0, srcSelect = 0, indexType = 0, drawCount = 0;
  bool     havePrim = false, haveIndexType = false, haveSrcSelect = false;
  // ⚠ THE COUNT DOMAIN, named explicitly. PM4's count is the draw-initiator's
  // index count; HLE's is an API DrawVertices source count. Printing both as
  // `indexCount` manufactured a shared field name and turned a units mismatch
  // into a false "1116 vs 48" finding. The domain is now part of the row, and
  // a reader diffing two rows with different domains is told so.
  const char* countDomain = "-";
  // Same treatment for srcSelect: PM4 carries the draw-initiator's
  // source_select; HLE carries our own producer tag. One name, two meanings.
  const char* srcSelectDomain = "-";

  // vertex input
  DrawContractVFetch vfetch[4];
  uint64_t vtxDigest = 0, idxDigest = 0;    // RAW guest bytes, guest order
  // WARNING a HOST-ORDER digest, under its own name because it is NOT
  // comparable to vtxDigest. The HLE staging site only has host bytes;
  // hashing them and calling it vtxDigest would invite a false field match
  // with the PM4 side's guest-order digest.
  uint64_t vtxHostDigest = 0;
  bool     haveVtxHostDigest = false;
  uint32_t srcStride = 0;
  bool     haveSrcStride = false;
  // ⚠ RENAMED. `hostVerts` meant "the LAYOUT was host-synthesized", not "host
  // bytes are available", and printing `hostVerts=0` beside a valid host digest
  // read as a contradiction.
  bool     hostSynthLayout = false, haveHostSynth = false;
  uint32_t vtxBytes = 0, idxBytes = 0;
  bool     vtxInBounds = false, idxInBounds = false;
  bool     haveVtx = false, haveIdx = false;

  // targets
  uint32_t rt[4] = {}, depth = 0;
  bool     haveTargets = false;

  // viewport / scissor / VTE
  uint32_t vte = 0;
  uint32_t vp[6] = {};        // xscale,xoffset,yscale,yoffset,zscale,zoffset
  uint32_t scissorTL = 0, scissorBR = 0;
  bool     haveVp = false, haveScissor = false;

  // fixed function
  uint32_t depthControl = 0, blendControl0 = 0, cullMode = 0, colorMask = 0;
  bool     haveFixed = false, haveCull = false;

  // constant digests
  uint64_t vsConstDigest = 0, psConstDigest = 0, boolConstDigest = 0;
  bool     haveConsts = false;

  // sampled textures (the seven for draw 2089; room for all 32)
  // ⚠ TWO SEPARATE INVENTORIES. PM4 knows which slots are REGISTER-RESIDENT;
  // HLE knows which are BOUND to the draw. They are different populations and a
  // single `texCount` invited a comparison between them.
  DrawContractTex tex[32];
  uint32_t texCount = 0;
  enum class TexDomain : uint32_t { None = 0, RegisterResident, Bound };
  TexDomain texDomain = TexDomain::None;
};

// FNV-1a over raw bytes, in the order given. Deliberately NOT the trace's XXH3:
// this digest never leaves a log line and never gates a decision, so a simple,
// obviously-correct function is preferable to reaching for the payload hasher.
inline uint64_t drawContractDigest(const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) {
    h ^= b[i];
    h *= 1099511628211ull;
  }
  return h;
}

inline const char* drawContractSourceName(DrawContractSource s) {
  return s == DrawContractSource::PM4 ? "PM4" : "HLE";
}

// Formats the row into `out`. One field per line, `name=value`, so a plain diff
// of two rows names the differing fields directly.
inline void formatDrawContract(const DrawContract& c, char* out, size_t cap) {
  size_t o = 0;
  auto add = [&](const char* fmt, ...) {
    if (o + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(out + o, cap - o, fmt, ap);
    va_end(ap);
    if (n > 0) o += (size_t)n;
  };
  add("source=%s\n", drawContractSourceName(c.source));
  add("ordinal=%llu\n", (unsigned long long)c.ordinal);

  if (c.haveShaders) {
    add("vsHash=%016llX psHash=%016llX\n",
        (unsigned long long)c.vsHash, (unsigned long long)c.psHash);
  } else {
    add("vsHash=- psHash=-\n");
  }
  if (c.haveVsBytes) add("vsBytes=%u\n", c.vsBytes);
  else               add("vsBytes=-\n");
  if (c.havePsBytes) add("psBytes=%u\n", c.psBytes);
  else               add("psBytes=-\n");

  if (c.havePrim) add("primType=%u\n", c.primType);
  else            add("primType=-\n");
  if (c.haveSrcSelect)
    add("srcSelect=%u srcSelectDomain=%s\n", c.srcSelect,
        c.srcSelectDomain);
  else
    add("srcSelect=- srcSelectDomain=-\n");
  if (c.haveIndexType) add("indexType=%u\n", c.indexType);
  else                 add("indexType=-\n");
  // The count ALWAYS carries its domain, so two rows whose counts mean
  // different things can never be diffed as if they meant the same thing.
  if (c.havePrim) add("drawCount=%u countDomain=%s\n", c.drawCount, c.countDomain);
  else            add("drawCount=- countDomain=-\n");

  for (const DrawContractVFetch& v : c.vfetch) {
    if (!v.present) continue;
    add("vfetch slot=%u raw=%08X,%08X strideWords=%u attrs=%u\n", v.slot,
        v.raw[0], v.raw[1], v.strideWords, v.attrCount);
  }
  if (c.haveVtx)
    add("vtxDigest=%016llX vtxBytes=%u inBounds=%d\n",
        (unsigned long long)c.vtxDigest, c.vtxBytes, c.vtxInBounds ? 1 : 0);
  else
    add("vtxDigest=- vtxBytes=- inBounds=-\n");
  if (c.haveVtxHostDigest)
    add("vtxHostDigest=%016llX\n", (unsigned long long)c.vtxHostDigest);
  else
    add("vtxHostDigest=-\n");
  if (c.haveSrcStride) add("srcStride=%u\n", c.srcStride);
  else                 add("srcStride=-\n");
  if (c.haveHostSynth) add("hostSynthLayout=%d\n", c.hostSynthLayout ? 1 : 0);
  else                 add("hostSynthLayout=-\n");
  if (c.haveIdx)
    add("idxDigest=%016llX idxBytes=%u inBounds=%d\n",
        (unsigned long long)c.idxDigest, c.idxBytes, c.idxInBounds ? 1 : 0);
  else
    add("idxDigest=- idxBytes=- inBounds=-\n");

  if (c.haveTargets)
    add("rt=%08X,%08X,%08X,%08X depth=%08X\n", c.rt[0], c.rt[1], c.rt[2],
        c.rt[3], c.depth);
  else
    add("rt=- depth=-\n");

  if (c.haveVp)
    add("vte=%08X vp=%08X,%08X,%08X,%08X,%08X,%08X\n", c.vte,
        c.vp[0], c.vp[1], c.vp[2], c.vp[3], c.vp[4], c.vp[5]);
  else
    add("vte=- vp=-\n");
  if (c.haveScissor)
    add("scissor=%08X,%08X\n", c.scissorTL, c.scissorBR);
  else
    add("scissor=-\n");

  if (c.haveFixed)
    add("depthControl=%08X blendControl0=%08X colorMask=%08X\n",
        c.depthControl, c.blendControl0, c.colorMask);
  else
    add("depthControl=- blendControl0=- colorMask=-\n");
  if (c.haveCull)
    add("cull=%08X\n", c.cullMode);
  else
    add("cull=-\n");

  if (c.haveConsts)
    add("vsConstDigest=%016llX psConstDigest=%016llX boolConstDigest=%016llX\n",
        (unsigned long long)c.vsConstDigest,
        (unsigned long long)c.psConstDigest,
        (unsigned long long)c.boolConstDigest);
  else
    add("vsConstDigest=- psConstDigest=- boolConstDigest=-\n");

  add("texCount=%u texDomain=%s\n", c.texCount, c.texDomain == DrawContract::TexDomain::RegisterResident ? "register-resident" : c.texDomain == DrawContract::TexDomain::Bound ? "bound" : "-");
  for (uint32_t i = 0; i < 32; i++) {
    const DrawContractTex& t = c.tex[i];
    if (!t.present) continue;
    if (t.haveRaw)
      add("tex slot=%u key=%08X raw=%08X,%08X,%08X,%08X,%08X,%08X\n",
          t.slot, t.key, t.raw[0], t.raw[1], t.raw[2], t.raw[3], t.raw[4],
          t.raw[5]);
    else
      add("tex slot=%u key=%08X raw=-\n", t.slot, t.key);
  }
  if (o < cap) out[o] = 0;
}

}  // namespace sr::gfx
