#include "native_draw_builder.hpp"
#include <bit>
#include <cstddef>
#include <cstring>

namespace sr::gfx {

bool buildNativeDrawState(std::span<const uint32_t> state, const NativeDrawInput& input,
                          GuestDrawCommand& output) {
  // Admit the complete draw/shader ABI snapshot before touching the caller's
  // reserved command. Shader arrays remain owned by the later capture stage.
  if (state.size() < 0x4908u || !(input.initiator >> 16) || !input.vertexFetch.addressDwords) return false;
  std::memset(&output, 0, offsetof(GuestDrawCommand, aluVs));
  output.effectSeq = input.effectSequence;
  output.hasDraw = true;
  output.drawOrdinal = input.ordinal;
  output.vertexCount = input.initiator >> 16;
  output.drawParamCount = 12;
  output.drawParams[0] = output.vertexCount;
  output.drawParams[4] = input.vertexFetch.addressDwords * 4u;
  output.drawParams[6] = input.initiator & 0x3Fu;
  output.primType = input.initiator & 0x3Fu;
  output.hasState = true;
  output.stateData[1] = input.layout.strideBytes;
  output.posOffsetBytes = input.layout.posOffsetBytes;
  output.posFormat = input.layout.posFormat;
  output.colorOffsetBytes = input.layout.colorOffsetBytes;
  output.colorFormat = input.layout.colorFormat;
  output.bufferVA = 0x80000000u + input.vertexFetch.addressDwords * 4u;
  const uint32_t source = (input.initiator >> 6) & 3u;
  if (source == 0u && input.indexBasePhys != 0u) {
    output.idxFmt32 = ((input.initiator >> 11) & 1u) != 0;
    output.idxBasePhys = input.indexBasePhys & ~(output.idxFmt32 ? 3u : 1u);
    output.idxEndian = (input.indexSize >> 30) & 3u;
  }
  for (uint32_t axis = 0; axis < 3u; ++axis) {
    output.vpScale[axis] = std::bit_cast<float>(state[0x210Fu + axis * 2u]);
    output.vpOffset[axis] = std::bit_cast<float>(state[0x2110u + axis * 2u]);
  }
  output.vteCntl = state[0x2206u];
  output.vsUcodeHash = input.vertexShader;
  output.psUcodeHash = input.pixelShader;
  output.depthControl = state[0x2200u];
  output.blendControl = state[0x2201u];
  output.colorMask = state[0x2104u];
  output.surfaceInfo = state[0x2000u];
  output.colorInfo = state[0x2001u];
  output.depthInfo = state[0x2002u];
  output.modeControl = state[0x2208u] ? state[0x2208u] : state[0x208u];
  output.windowOffset = state[0x2080u];
  output.windowScissorTL = state[0x2081u];
  output.windowScissorBR = state[0x2082u];
  output.screenScissorTL = state[0x200Eu];
  output.screenScissorBR = state[0x200Fu];
  output.suScModeCntl = state[0x2205u];
  for (uint32_t i = 0; i < 4u; ++i) output.polyOffset[i] = state[0x2380u + i];
  output.primResetIndex = state[0x2103u] & 0xFFFFFFu;
  output.indxOffset = state[0x2102u];
  output.stencilRefMask = state[0x210Du];
  output.stencilRefMaskBF = state[0x210Cu];
  output.minVtxIndx = state[0x2101u] & 0xFFFFFFu;
  output.maxVtxIndx = state[0x2100u] & 0xFFFFFFu;
  output.pointSize = state[0x2280u];
  output.pointMinMax = state[0x2281u];
  output.colorControl = state[0x2202u] ? state[0x2202u] : state[0x202u];
  output.alphaRef = state[0x210Eu] ? state[0x210Eu] : state[0x10Eu];
  output.programCntl = state[0x2180u];
  output.contextMisc = state[0x2181u];
  output.copyControl = state[0x2318u] ? state[0x2318u] : state[0x318u];
  output.copyDestBase = input.copyDestBase;
  output.copyDestPitch = input.copyDestPitch;
  output.copyDestInfo = input.copyDestInfo;
  const auto alias = [&](uint32_t word) { return state[word] ? state[word] : state[word - 0x2000u]; };
  for (uint32_t i = 0; i < 3u; ++i) {
    output.colorInfo123[i] = alias(0x2003u + i);
    output.blendControl123[i] = alias(0x2209u + i);
  }
  output.clipCntl = alias(0x2204u);
  output.vtxCntl = alias(0x2302u);
  output.depthClear = alias(0x231Du);
  output.colorClear = alias(0x231Eu);
  output.colorClearLo = alias(0x231Fu);
  return true;
}

}  // namespace sr::gfx
