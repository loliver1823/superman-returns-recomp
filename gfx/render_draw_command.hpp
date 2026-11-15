#pragma once

// Shared draw data and shader ABI only. Native resources/submissions remain
// separate from the development hardware oracle and transport implementation.
#include <cstdint>
#include "../kernel/sr_oracle.hpp"

namespace sr::gfx {

struct NativeVertexFetch {
  uint32_t type;       // 0=invalid, 3=vertex
  uint32_t addressDwords; // byte address = addressDwords * 4
  uint32_t sizeWords;  // byte size = sizeWords * 4
  uint32_t endian;
};

struct GuestDrawCommand {
#if SR_ORACLE
  // NATIVE strand A executor (kernel/native_exec.hpp, SR_NATIVE_EXEC): a CP effect the executor
  // performs in stream order (0 = an ordinary command), the push position, and the CHECK's data hash.
  // In the head emitDraw zeroes.
  uint8_t effectKind = 0;
  uint8_t effectN = 0;      // kStores: (op, addr, value, flags) quads in drawParams
  uint8_t execChk = 0;      // CHECK: 1 sampled, 2 sampled + a command-buffer page
#endif
  uint64_t effectSeq = 0;   // items committed to the queue before this one
#if SR_ORACLE
  uint64_t execChkHash = 0;
  uint64_t execChkTex = 0;  // CHECK: the texture fetch constants' bases (first 16 KB each), hashed at push
  uint32_t execChkFetch[3] = {};
#endif
  // State update (from the first cmd[43] call with a2=5).
  uint32_t stateData[5] = {};
  bool hasState = false;

  // Draw submit (from the second cmd[43] call with a2=N).
  uint32_t drawParams[80] = {};  // up to 80 dwords of draw data (largest seen: 72)
  uint32_t drawParamCount = 0;
  uint32_t vertexCount = 0;      // extracted from drawParams[0]
  uint32_t drawFlags = 0;        // extracted from drawParams[1]
  uint32_t bufferVA = 0;         // resolved (cached) VA of the draw data buffer
  uint32_t bufferVAUncached = 0; // original uncached VA (for diagnostics)
  bool hasDraw = false;
  // iter-437e: VdSwap frame-boundary marker (hasDraw stays false — the render
  // loop skips it; only the drain's frame-coherence logic reads it).
  bool frameBoundary = false;
  // iter-440c API-CLEAR (migration Phase 2): the guest's own D3DDevice_Clear /
  // ClearF, pushed at the API boundary by kernel/hle_api_observe.cpp when
  // SR_API_CLEAR=1. Executed by the drain at stream position; replaces the
  // invented drain-top clear (the reference model: the host NEVER invents
  // clears — video.cpp hooks Clear and translates it 1:1).
  bool clearOp = false;
  uint32_t clearFlags = 0;      // ClearF flags: 0xF=TARGET0-3 0x10=Z 0x20=STENCIL
  float clearColor[4] = {};     // RGBA 0..1
  float clearZ = 1.0f;
  uint32_t clearStencil = 0;
  // p2.zpd (SR_NATIVE_ZPD, gfx/native_zpd.hpp): an occlusion-query marker at stream position (hasDraw false):
  // zpdOp 1 = BEGIN, 2 = END of logical query zpdId. The drain turns it into a marker DrawBatch; the render loop
  // records the host BeginQuery / EndQuery there. In the head emitDraw zeroes.
  uint8_t zpdOp = 0;
  uint32_t zpdId = 0;

  // Vertex fetch constant snapshot at time of draw.
  NativeVertexFetch fetchSlots[4] = {};  // first 4 fetch slots
  uint32_t fetchSlotCount = 0;

  // iter-358i: REAL position layout from the bound VS's vfetch instructions
  // (gfx/xenos_ucode.hpp). posFormat is a ucode::VertexFormat enum value;
  // 0 (kUndefined) = not provided (legacy producer), consumer falls back to
  // "3 BE floats at offset 0".
  uint32_t posOffsetBytes = 0;
  uint32_t posFormat = 0;

  // iter-358k: full per-draw snapshot for real rasterization. Registers mutate
  // between draws while the drain runs later — everything a draw needs must be
  // captured HERE, at draw time.
  uint32_t primType = 0;       // xenos::PrimitiveType (VGT_DRAW_INITIATOR bits 5:0)
  // Indexed (kDMA) draw info from the DRAW_INDX packet; idxBasePhys==0 = auto-indexed.
  uint32_t idxBasePhys = 0;    // physical address, aligned down to index size
  uint32_t idxEndian = 0;      // VGT_DMA_SIZE.swap_mode (0=none, 1=8in16, 2=8in32)
  bool idxFmt32 = false;       // VGT_DRAW_INITIATOR.index_size (0=u16, 1=u32)
  // Vertex color attribute from the VS vfetch binding; colorFormat 0 = none.
  uint32_t colorOffsetBytes = 0;
  uint32_t colorFormat = 0;    // ucode::VertexFormat
  // Viewport snapshot: PA_CL_VPORT_{X,Y,Z}{SCALE,OFFSET} (0x210F-0x2114, floats)
  // + PA_CL_VTE_CNTL (0x2206: ena bits gate each scale/offset; bit8 vtx_xy_fmt).
  float vpScale[3] = {};
  float vpOffset[3] = {};
  uint32_t vteCntl = 0;

  // iter-359c: translated-shader (XenosRecomp) path. Hashes of the currently
  // bound VS/PS ucode (XXH3-64 of the raw BE IM_LOAD bytes = the shader-cache
  // key), plus per-draw snapshots of everything the translated shaders read —
  // registers mutate between draws while the drain runs later, so all of it
  // must be captured at draw time (same rule as the viewport snapshot).
  uint64_t vsUcodeHash = 0;
  uint64_t psUcodeHash = 0;
  // iter-359e: per-draw render state (Xenia registers.h layouts).
  uint32_t depthControl = 0;  // RB_DEPTHCONTROL 0x2200: b1 z_enable, b2 z_write, [6:4] zfunc
  uint32_t blendControl = 0;  // RB_BLENDCONTROL0 0x2201: src[4:0] op[7:5] dst[12:8] | alpha at +16
  uint32_t colorMask = 0xF;   // RB_COLOR_MASK 0x2104: RT0 = low 4 bits (RGBA)
  // iter-359f: render-pass identity + resolve info (recon wf_3fc645cd census:
  // the frame = 640x640 MSAA helper pass -> 1280x720 HDR scene -> luminance
  // chain -> tonemap/UI, wired by 26 edram_mode-6 resolve rects).
  uint32_t surfaceInfo = 0;   // RB_SURFACE_INFO 0x2000 (pitch/msaa)
  uint32_t colorInfo = 0;     // RB_COLOR_INFO 0x2001 (edram base/format)
  uint32_t depthInfo = 0;     // RB_DEPTH_INFO 0x2002 (edram base/format)
  uint32_t modeControl = 0;   // RB_MODECONTROL 0x2208 (edram_mode: 4=color+depth 5=depth 6=copy)
  // iter-359i: per-pass surface geometry (the tile-2/helper-pass hunt).
  uint32_t windowOffset = 0;     // PA_SC_WINDOW_OFFSET 0x2080 (s15 x | s15 y<<16)
  uint32_t windowScissorTL = 0;  // PA_SC_WINDOW_SCISSOR_TL 0x2081
  uint32_t windowScissorBR = 0;  // PA_SC_WINDOW_SCISSOR_BR 0x2082
  uint32_t screenScissorTL = 0;  // PA_SC_SCREEN_SCISSOR_TL 0x200E
  uint32_t screenScissorBR = 0;  // PA_SC_SCREEN_SCISSOR_BR 0x200F
  uint32_t suScModeCntl = 0;     // PA_SU_SC_MODE_CNTL 0x2205 (b16 vtx_window_offset_enable,
                                 //   b21 multi_prim_ib_ena = strip primitive reset)
  uint32_t primResetIndex = 0xFFFF;  // VGT_MULTI_PRIM_IB_RESET_INDX 0x2103 (24 bits)
  // PA_SU_POLY_OFFSET_FRONT_SCALE/OFFSET, BACK_SCALE/OFFSET 0x2380-0x2383 (float bits).
  uint32_t polyOffset[4] = {};
  // Vertex index remap (Xenia RemapAndConvertVertexIndices): every index,
  // auto or DMA, becomes clamp((i + offset) & 0xFFFFFF, min, max).
  uint32_t drawOrdinal = 0;          // 1-based draw packet ordinal in the stream (Pm4Engine::draws)
  uint32_t resourceGpuGeneration = 0;  // resolve's producer order, retained until GPU execution
  uint32_t stencilRefMask = 0;       // RB_STENCILREFMASK 0x210D (ref | mask<<8 | writemask<<16)
  uint32_t stencilRefMaskBF = 0;     // RB_STENCILREFMASK_BF 0x210C
  uint32_t indxOffset = 0;           // VGT_INDX_OFFSET 0x2102
  uint32_t minVtxIndx = 0;           // VGT_MIN_VTX_INDX 0x2101
  uint32_t maxVtxIndx = 0xFFFFFF;    // VGT_MAX_VTX_INDX 0x2100
  // iter-370 kPointList: point-sprite size. Both are 12.4 fixed point holding
  // the HALF width/height (registers.h:444 "1/2 width or height"), so the
  // screen-space DIAMETER is field * 2/16 — Xenia's exact conversion
  // (d3d12_command_processor.cc:3930).
  //
  // MINMAX clamps a PER-VERTEX size, which we do not implement: a Xenos VS may
  // export one to register 63 (ucode.h kVSPointSizeEdgeFlagKillVertex), but
  // XenosRecomp's VS export switch handles only kVSPosition and looks every
  // other target up in the interpolator map (0..15) — so a shader exporting 63
  // could not have translated at all, and all 109 menu shaders did (iter-359b).
  // For this content the constant register is therefore the whole story.
  // Captured anyway so the assumption is checkable if a later scene breaks it.
  uint32_t pointSize = 0;        // PA_SU_POINT_SIZE 0x2280 (height[15:0] width[31:16])
  uint32_t pointMinMax = 0;      // PA_SU_POINT_MINMAX 0x2281
  uint32_t colorControl = 0;  // RB_COLORCONTROL 0x2202 (alpha test func/enable)
  uint32_t alphaRef = 0;      // RB_ALPHA_REF 0x210E (float bits)
  // iter-426: the movie quad VS exports ZERO to every interpolator — the YUV PS
  // gets its coordinates from the hardware PARAM_GEN feature (SQ_PROGRAM_CNTL
  // bit 18 enables it; SQ_CONTEXT_MISC bits 8-15 pick the interpolator slot).
  uint32_t programCntl = 0;   // SQ_PROGRAM_CNTL 0x2180 (b18 param_gen)
  uint32_t contextMisc = 0;   // SQ_CONTEXT_MISC 0x2181 (b8-15 param_gen_pos)
  uint32_t copyControl = 0;   // RB_COPY_CONTROL 0x2318
  uint32_t copyDestBase = 0;  // RB_COPY_DEST_BASE 0x2319
  uint32_t copyDestPitch = 0; // RB_COPY_DEST_PITCH 0x231A
  uint32_t copyDestInfo = 0;  // RB_COPY_DEST_INFO 0x231B
  // iter-437b: TRUE dest height from the D3D9 surface fetch (HLE Resolve
  // hook). The PM4 path has no height register, so the batch side derives
  // rows from pitch — right for the trace's square helpers, but a 640x360
  // dest got 640 rows = a 280-row guest-memory scribble past the surface.
  // 0 = not provided (PM4 producer), consumer keeps the pitch heuristic.
  uint32_t copyDestHeight = 0;
  // iter-439 P1: the resolve's SOURCE RECT (r5=pSrcRect at the emit — the
  // predicated-tiling passes resolve tile sub-rects; ignoring this copied the
  // top-left region into every tile = the x=640 seam class). All zero = full
  // surface (legacy/PM4 producers). Dest placement mirrors the src rect
  // (X360 Resolve semantics: dest point = src rect origin unless overridden).
  uint16_t copySrcX = 0, copySrcY = 0, copySrcW = 0, copySrcH = 0;
  // iter-427b: explicit vertex-layout override (D3D9-HLE hook). Container VSes
  // carry PLACEHOLDER vfetch instructions (slot/offset/stride patched from the
  // vertex declaration by the console runtime at draw time), so the sidecar
  // derived from the container cannot drive the fetch. When hleLayoutCount>0
  // the drain uses these elements verbatim instead of the sidecar.
  struct HleLayoutElem {
    uint32_t slot = 0, offsetBytes = 0, strideBytes = 0;
    uint32_t format = 0;  // Xenos vertex format (same enum the sidecar uses)
    uint32_t usage = 0, usageIndex = 0;
  };
  HleLayoutElem hleLayout[8] = {};
  uint32_t hleLayoutCount = 0;
  // iter-435: RAW vertex-declaration elements from the D3D9-HLE hook (the vdecl
  // object at dev+11536). The drain pairs them positionally with the translated
  // VS's input signature (vdecl author order == input register order — the same
  // rule the movie hack encoded by hand, generalized). declType is the packed
  // X360 D3DDECLTYPE (low 6 bits = Xenos VertexFormat: 0x2A23B9=FLOAT3,
  // 0x2C23A5=FLOAT2, 0x1A23A6=FLOAT4 — decoded iter-435 against the verified
  // movie layout). hleStreamStride[s] is inferred at the hook as
  // max(offset+size) over stream s's elements (fetch constants carry base+size
  // only; the true stride lives in runtime-patched vfetch ucode we never see).
  struct HleVdeclElem {
    uint32_t stream = 0, offsetBytes = 0, declType = 0;
    uint32_t usage = 0, usageIndex = 0;
  };
  HleVdeclElem hleVdecl[12] = {};
  uint32_t hleVdeclCount = 0;
  uint32_t hleStreamStride[4] = {};
  // iter-435: DrawIndexedVertices base vertex (VGT_INDX_OFFSET arg a3) — added
  // to every fetched index at decode time.
  uint32_t hleBaseVertex = 0;
  // P1 EDRAM model (gfx/edram.hpp, docs/RENDERER_PM4_HYBRID.md): the rest of
  // the render-target / resolve state it reads. Same alias rule as
  // modeControl above (high register, falling back to the low alias).
  uint32_t colorInfo123[3] = {};     // RB_COLOR1/2/3_INFO 0x2003-0x2005
  uint32_t blendControl123[3] = {};  // RB_BLENDCONTROL1/2/3 0x2209-0x220B
  uint32_t clipCntl = 0;             // PA_CL_CLIP_CNTL 0x2204
  uint32_t vtxCntl = 0;              // PA_SU_VTX_CNTL 0x2302 (b0 pix_center)
  uint32_t depthClear = 0;           // RB_DEPTH_CLEAR 0x231D
  uint32_t colorClear = 0;           // RB_COLOR_CLEAR 0x231E
  uint32_t colorClearLo = 0;         // RB_COLOR_CLEAR_LO 0x231F
  // A resolve draw's rectangle: vertex fetch 0, three xy pairs, byte-swapped
  // at emit time (the CPU writes them just before the resolve; the drain runs
  // later).
  float resolveVerts[6] = {};
  bool resolveVertsValid = false;
  // The float-constant blocks (16 vec4 each, bit b = c[16b..16b+15]) the bound VS / PS read
  // (gfx/xenos_ucode constantBlocksRead; bit 16 = relative addressing = all). With
  // SR_PM4_CONST_USED only those blocks of aluVs / aluPs are copied and uploaded.
  uint32_t vsConstUse = 0x1FFFFu, psConstUse = 0x1FFFFu;
  // iter-466 cp round 2 (SR_CP_CONST_TRACK): a tracked command carries only the used blocks
  // whose VALUES changed since the previous tracked command (constPresent: bit b = VS block b,
  // bit 16+b = PS block b); the consumer keeps every block's latest values in order of
  // constSeq (1-based, consecutive). Untracked commands carry every used block, as before.
  bool constTracked = false;
  uint32_t constPresent = 0;
  uint32_t constSeq = 0;
  uint32_t aluVs[1024] = {};    // regs 0x4000-0x43FF: VS float constants c0-255
  uint32_t aluPs[1024] = {};    // regs 0x4400-0x47FF: PS float constants c0-255 (P1: this game
                                // binds named PS constants above c223, e.g. KaS at c229)
  uint32_t boolConsts[8] = {};  // regs 0x4900-0x4907: 256 bool bits (VS@0, PS@128)
  uint32_t fetchRaw[192] = {};  // regs 0x4800-0x48BF: all 32 fetch groups (96 vslots)
};

}  // namespace sr::gfx
