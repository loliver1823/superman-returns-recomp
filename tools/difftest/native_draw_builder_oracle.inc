// Test-only assembly oracle copied from 273c8ba9 gfx/pm4_engine.cpp.
// Only local inputs/queue sequence are adapted; no production fallback.
static void originalDrawState(GuestDrawCommand& drawCmd, const XenosRegisterFile& xeRegs,
                              const NativeDrawInput& input, const sr::gfx::ucode::VertexBinding* bind) {
  const auto vf0 = input.vertexFetch;
  const uint32_t idxCount = input.initiator >> 16, vgtDrawInitiator = input.initiator;
  const uint32_t dmaBasePhys = input.indexBasePhys, dmaSizeReg = input.indexSize;
  const uint32_t draws = input.ordinal;
  const uint64_t lastVsUcodeHash = input.vertexShader, lastPsUcodeHash = input.pixelShader;
  const uint32_t copyDestBase = input.copyDestBase, copyDestPitch = input.copyDestPitch, copyDestInfo = input.copyDestInfo;
  std::memset(&drawCmd, 0, offsetof(GuestDrawCommand, aluVs));
  drawCmd.effectSeq = input.effectSequence;
  drawCmd.hasDraw = true;
  drawCmd.drawOrdinal = uint32_t(draws);  // 1-based, = Xenia's draw packet ordinal
  drawCmd.vertexCount = idxCount;
  drawCmd.drawParamCount = 12;
  drawCmd.drawParams[0] = drawCmd.vertexCount;
  drawCmd.drawParams[4] = vf0.addressDwords * 4;
  drawCmd.drawParams[6] = vgtDrawInitiator & 0x3F;
  drawCmd.primType = vgtDrawInitiator & 0x3F;
  drawCmd.hasState = true;
  if (bind) {
    drawCmd.stateData[1] = bind->strideWords * 4;  // REAL stride from the VS vfetch
    // Position = the attribute at offset 0 of the binding (fall back to attr 0);
    // color = the first non-position attribute.
    const ucode::Attribute* posAttr = nullptr;
    const ucode::Attribute* colAttr = nullptr;
    for (const auto& at : bind->attributes) {
      if (at.offsetWords == 0 && !posAttr) posAttr = &at;
      else if (at.offsetWords != 0 && !colAttr) colAttr = &at;
    }
    if (!posAttr && !bind->attributes.empty()) posAttr = &bind->attributes[0];
    if (posAttr) {
      drawCmd.posOffsetBytes = static_cast<uint32_t>(posAttr->offsetWords) * 4u;
      drawCmd.posFormat = static_cast<uint32_t>(posAttr->format);
    }
    if (colAttr) {
      drawCmd.colorOffsetBytes = static_cast<uint32_t>(colAttr->offsetWords) * 4u;
      drawCmd.colorFormat = static_cast<uint32_t>(colAttr->format);
    }
  } else {
    // Legacy guess, kept only for the parse-failed path.
    uint32_t guessStride = (vf0.sizeWords > 0) ? (vf0.sizeWords * 4 / idxCount) : 44;
    if (guessStride < 12) guessStride = 12;
    if (guessStride > 256) guessStride = 44;
    drawCmd.stateData[1] = guessStride;
  }
  drawCmd.bufferVA = 0x80000000u + vf0.addressDwords * 4;

  // Indexed (kDMA) draw: index buffer info from the DRAW_INDX packet
  // (Xenia pm4_command_processor_implement.h:1075-1111).
  const uint32_t sourceSelect = (vgtDrawInitiator >> 6) & 3u;
  if (sourceSelect == 0u && dmaBasePhys != 0u) {
    drawCmd.idxFmt32 = ((vgtDrawInitiator >> 11) & 1u) != 0;
    drawCmd.idxBasePhys = dmaBasePhys & ~(drawCmd.idxFmt32 ? 3u : 1u);
    drawCmd.idxEndian = (dmaSizeReg >> 30) & 3u;
  }

  // Viewport snapshot (PA_CL_VPORT_XSCALE..ZOFFSET = 0x210F..0x2114, floats;
  // PA_CL_VTE_CNTL = 0x2206). Captured per draw — registers mutate between
  // draws while the drain consumes later.
  auto regF = [&](uint32_t reg) {
    uint32_t v = xeRegs.read(reg);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  };
  drawCmd.vpScale[0] = regF(0x210F); drawCmd.vpOffset[0] = regF(0x2110);
  drawCmd.vpScale[1] = regF(0x2111); drawCmd.vpOffset[1] = regF(0x2112);
  drawCmd.vpScale[2] = regF(0x2113); drawCmd.vpOffset[2] = regF(0x2114);
  drawCmd.vteCntl = xeRegs.read(0x2206);

  // iter-359c: translated-shader identity + full constant/fetch snapshot at
  // draw time (registers mutate between draws; the drain consumes later).
  drawCmd.vsUcodeHash = lastVsUcodeHash;
  drawCmd.psUcodeHash = lastPsUcodeHash;
  drawCmd.depthControl = xeRegs.read(0x2200);
  drawCmd.blendControl = xeRegs.read(0x2201);
  drawCmd.colorMask = xeRegs.read(0x2104);
  drawCmd.surfaceInfo = xeRegs.read(0x2000);
  drawCmd.colorInfo = xeRegs.read(0x2001);
  drawCmd.depthInfo = xeRegs.read(0x2002);
  // RB_MODECONTROL / RB_COPY_* arrive on BOTH register aliases (0x2208/0x208,
  // 0x2318-1B/0x318-1B) — this game uses the low alias; writeReg() already
  // latches the copy-dest trio into engine fields, use those.
  {
    uint32_t mc = xeRegs.read(0x2208);
    if (!mc) mc = xeRegs.read(0x208);
    drawCmd.modeControl = mc;
  }
  drawCmd.windowOffset = xeRegs.read(0x2080);
  drawCmd.windowScissorTL = xeRegs.read(0x2081);
  drawCmd.windowScissorBR = xeRegs.read(0x2082);
  drawCmd.screenScissorTL = xeRegs.read(0x200E);
  drawCmd.screenScissorBR = xeRegs.read(0x200F);
  drawCmd.suScModeCntl = xeRegs.read(0x2205);
  for (uint32_t i = 0; i < 4; i++) drawCmd.polyOffset[i] = xeRegs.read(0x2380u + i);
  drawCmd.primResetIndex = xeRegs.read(0x2103) & 0xFFFFFFu;
  drawCmd.indxOffset = xeRegs.read(0x2102);
  drawCmd.stencilRefMask = xeRegs.read(0x210D);
  drawCmd.stencilRefMaskBF = xeRegs.read(0x210C);
  drawCmd.minVtxIndx = xeRegs.read(0x2101) & 0xFFFFFFu;
  drawCmd.maxVtxIndx = xeRegs.read(0x2100) & 0xFFFFFFu;
  drawCmd.pointSize = xeRegs.read(0x2280);
  drawCmd.pointMinMax = xeRegs.read(0x2281);
  drawCmd.colorControl = xeRegs.read(0x2202) ? xeRegs.read(0x2202) : xeRegs.read(0x202);
  drawCmd.alphaRef = xeRegs.read(0x210E) ? xeRegs.read(0x210E) : xeRegs.read(0x10E);
  drawCmd.programCntl = xeRegs.read(0x2180);
  drawCmd.contextMisc = xeRegs.read(0x2181);
  drawCmd.copyControl = xeRegs.read(0x2318) ? xeRegs.read(0x2318) : xeRegs.read(0x318);
  drawCmd.copyDestBase = copyDestBase;
  drawCmd.copyDestPitch = copyDestPitch;
  drawCmd.copyDestInfo = copyDestInfo;
  {
    auto regA = [&](uint32_t r) {
      const uint32_t v = xeRegs.read(r);
      return v ? v : xeRegs.read(r - 0x2000u);
    };
    for (uint32_t i = 0; i < 3; i++) {
      drawCmd.colorInfo123[i] = regA(0x2003u + i);
      drawCmd.blendControl123[i] = regA(0x2209u + i);
    }
    drawCmd.clipCntl = regA(0x2204u);
    drawCmd.vtxCntl = regA(0x2302u);
    drawCmd.depthClear = regA(0x231Du);
    drawCmd.colorClear = regA(0x231Eu);
    drawCmd.colorClearLo = regA(0x231Fu);
  }
  const uint32_t* rf = xeRegs.data();
    std::memcpy(drawCmd.boolConsts, rf + 0x4900, sizeof(drawCmd.boolConsts));
    std::memcpy(drawCmd.fetchRaw, rf + 0x4800, sizeof(drawCmd.fetchRaw));
}
