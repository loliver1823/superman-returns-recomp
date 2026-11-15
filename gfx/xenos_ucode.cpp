/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// PORTED FROM XENIA (Ben Vanik, BSD-3-Clause) — see xenos_ucode.hpp for the
// full provenance list. The CF walk below mirrors Shader::AnalyzeUcode
// (shader_translator.cc:42-120), GatherExecInformation, and
// GatherVertexFetchInformation / ParseVertexFetchInstruction, minus the
// disassembly and ALU/texture paths we don't need.

#include "xenos_ucode.hpp"

#include <algorithm>
#include <cstring>

namespace sr {
namespace gfx {
namespace ucode {

const char* formatName(VertexFormat format) {
  switch (format) {
    case VertexFormat::kUndefined:          return "kUndefined";
    case VertexFormat::k_8_8_8_8:           return "k_8_8_8_8";
    case VertexFormat::k_2_10_10_10:        return "k_2_10_10_10";
    case VertexFormat::k_10_11_11:          return "k_10_11_11";
    case VertexFormat::k_11_11_10:          return "k_11_11_10";
    case VertexFormat::k_16_16:             return "k_16_16";
    case VertexFormat::k_16_16_16_16:       return "k_16_16_16_16";
    case VertexFormat::k_16_16_FLOAT:       return "k_16_16_FLOAT";
    case VertexFormat::k_16_16_16_16_FLOAT: return "k_16_16_16_16_FLOAT";
    case VertexFormat::k_32:                return "k_32";
    case VertexFormat::k_32_32:             return "k_32_32";
    case VertexFormat::k_32_32_32_32:       return "k_32_32_32_32";
    case VertexFormat::k_32_FLOAT:          return "k_32_FLOAT";
    case VertexFormat::k_32_32_FLOAT:       return "k_32_32_FLOAT";
    case VertexFormat::k_32_32_32_32_FLOAT: return "k_32_32_32_32_FLOAT";
    case VertexFormat::k_32_32_32_FLOAT:    return "k_32_32_32_FLOAT";
    default:                                return "kInvalid";
  }
}

namespace {

uint32_t g_mustBeOneViolations = 0;

// shader.h:167 GetUsedResultComponents, reduced: a fetch writes nothing if
// every destination component swizzle is kKeep or a constant (k0/k1).
uint32_t usedResultComponents(uint32_t dstSwiz) {
  uint32_t used = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    FetchDestinationSwizzle s = GetFetchDestinationComponentSwizzle(dstSwiz, i);
    if (s >= FetchDestinationSwizzle::kX && s <= FetchDestinationSwizzle::kW) {
      used |= 1u << uint32_t(s);
    }
  }
  return used;
}

// ParseVertexFetchInstruction (shader_translator.cc:1002-1046) +
// GatherVertexFetchInformation (:380-432), fused.
//
// Returns true if `op` is a full fetch (i.e. the caller should latch it as the
// new previous_full_op).
bool gatherVertexFetch(const VertexFetchInstruction& op,
                       const VertexFetchInstruction& previousFullOp,
                       bool havePreviousFull,
                       std::vector<VertexBinding>& out) {
  const bool isMini = op.is_mini_fetch();
  // shader_translator.cc:1014 — mini fetches reuse the full fetch's addressing.
  const VertexFetchInstruction& fullOp = isMini ? previousFullOp : op;

  // A vfetch_mini with no preceding vfetch_full is malformed; its inherited
  // fields would be garbage. Skip it rather than invent a binding.
  if (isMini && !havePreviousFull) return false;

  // shader_translator.cc:404 — an instruction that fetches nothing gets no
  // binding (a vfetch_full may exist purely to set up a later vfetch_mini).
  if (!usedResultComponents(op.dest_swizzle())) return !isMini;

  const uint32_t fetchConstant = fullOp.fetch_constant_index();

  Attribute attr;
  // NOTE :1036-1042 — these come from `op`, NOT `full_op`: a mini fetch has its
  // own format/offset/exp_adjust/signedness.
  attr.format = op.data_format();
  attr.offsetWords = op.offset();
  attr.expAdjust = op.exp_adjust();
  attr.isSigned = op.is_signed();
  attr.isInteger = !op.is_normalized();
  attr.rfMode = op.signed_rf_mode();
  attr.dstSwiz = op.dest_swizzle();

  for (auto& binding : out) {
    if (binding.fetchConstantIndex == fetchConstant) {
      binding.attributes.push_back(attr);
      return !isMini;
    }
  }

  VertexBinding binding;
  binding.fetchConstantIndex = fetchConstant;
  binding.strideWords = fullOp.stride();  // :1038 — stride is the full fetch's.
  binding.attributes.push_back(attr);
  out.push_back(std::move(binding));
  return !isMini;
}

}  // namespace

uint32_t debugMustBeOneViolations() { return g_mustBeOneViolations; }

bool constantBlocksRead(const uint32_t* ucode, size_t dwordCount, uint32_t* blockMask, bool* relative) {
  *blockMask = 0;
  *relative = false;
  if (!ucode || dwordCount < 3) return false;
  const uint32_t pairs = uint32_t(dwordCount / 3);
  uint32_t bound = pairs;
  for (uint32_t i = 0; i < bound; ++i) {
    ControlFlowInstruction cfAb[2];
    UnpackControlFlowInstructions(ucode + size_t(i) * 3, cfAb);
    for (uint32_t j = 0; j < 2; ++j)
      if (IsControlFlowOpcodeExec(cfAb[j].opcode())) bound = std::min(bound, cfAb[j].exec.address());
  }
  if (bound == 0) return false;
  bool malformed = false;
  for (uint32_t i = 0; i < bound; ++i) {
    ControlFlowInstruction cfAb[2];
    UnpackControlFlowInstructions(ucode + size_t(i) * 3, cfAb);
    for (uint32_t j = 0; j < 2; ++j) {
      const ControlFlowInstruction& cf = cfAb[j];
      uint32_t execAddress, execCount, sequence;
      switch (cf.opcode()) {
        case ControlFlowOpcode::kExec:
        case ControlFlowOpcode::kExecEnd:
          execAddress = cf.exec.address();
          execCount = cf.exec.count();
          sequence = cf.exec.sequence();
          break;
        case ControlFlowOpcode::kCondExec:
        case ControlFlowOpcode::kCondExecEnd:
        case ControlFlowOpcode::kCondExecPredClean:
        case ControlFlowOpcode::kCondExecPredCleanEnd:
          execAddress = cf.cond_exec.address();
          execCount = cf.cond_exec.count();
          sequence = cf.cond_exec.sequence();
          break;
        case ControlFlowOpcode::kCondExecPred:
        case ControlFlowOpcode::kCondExecPredEnd:
          execAddress = cf.cond_exec_pred.address();
          execCount = cf.cond_exec_pred.count();
          sequence = cf.cond_exec_pred.sequence();
          break;
        default:
          continue;
      }
      for (uint32_t k = 0; k < execCount; ++k, sequence >>= 2) {
        const uint32_t at = execAddress + k;
        if (size_t(at) * 3 + 3 > dwordCount) {
          malformed = true;
          break;
        }
        if (sequence & 0b01) continue;  // fetch
        const uint32_t w1 = ucode[size_t(at) * 3 + 1], w2 = ucode[size_t(at) * 3 + 2];
        // dword 1: bit 29 const_address_register_relative, 30 const_1_rel_abs, 31 const_0_rel_abs.
        // dword 2: src3_reg 0-7, src2_reg 8-15, src1_reg 16-23, vector_opc 24-28,
        //          src3_sel 29, src2_sel 30, src1_sel 31 (sel 1 = temp register).
        const bool c0rel = (w1 >> 31) & 1u, c1rel = (w1 >> 30) & 1u;
        const bool t1 = (w2 >> 31) & 1u, t2 = (w2 >> 30) & 1u, t3 = (w2 >> 29) & 1u;
        const uint32_t reg[3] = {(w2 >> 16) & 0xFFu, (w2 >> 8) & 0xFFu, w2 & 0xFFu};
        const bool isConst[3] = {!t1, !t2, !t3};
        const bool addressed[3] = {c0rel, t1 ? c0rel : c1rel, (t1 && t2) ? c0rel : c1rel};
        for (int s = 0; s < 3; ++s) {
          if (!isConst[s]) continue;
          if (addressed[s]) *relative = true;
          *blockMask |= 1u << (reg[s] >> 4);
        }
      }
    }
  }
  return !malformed;
}

bool hasValidControlFlow(const uint32_t* ucode, size_t dwordCount) {
  if (!ucode || dwordCount < 3) return false;
  // Same CF walk parseVertexShaderBindings opens with: instructions are packed
  // 2-per-3-dwords at the top, and an exec's target address bounds the CF
  // section. Applies to pixel shaders too — the CF encoding is shared.
  const uint32_t pairs = uint32_t(dwordCount / 3);
  uint32_t bound = pairs;
  bool sawExec = false;
  for (uint32_t i = 0; i < bound; ++i) {
    ControlFlowInstruction cfAb[2];
    UnpackControlFlowInstructions(ucode + size_t(i) * 3, cfAb);
    for (uint32_t j = 0; j < 2; ++j) {
      const ControlFlowInstruction& cf = cfAb[j];
      if (!IsControlFlowOpcodeExec(cf.opcode())) continue;
      const uint32_t addr = cf.exec.address();
      const uint32_t count = cf.exec.count();
      // The exec's instructions must lie inside the shader. A run of zeroes
      // decodes as all-kNop and never gets here; ring bytes that happen to
      // decode as an exec almost always point outside.
      if (addr == 0 || uint64_t(addr) + count > uint64_t(pairs)) return false;
      bound = std::min(bound, addr);
      sawExec = true;
    }
  }
  return sawExec && bound != 0;
}

bool parseVertexShaderBindings(const uint32_t* ucode, size_t dwordCount,
                               std::vector<VertexBinding>& out) {
  out.clear();
  if (!ucode || dwordCount < 3) return false;

  // shader_translator.cc:53-81 — CF instructions are packed 2-per-3-dwords at
  // the top of the ucode. Their extent is unknown, so bound it by the lowest
  // exec target address seen (what freedreno does).
  uint32_t cfPairIndexBound = uint32_t(dwordCount / 3);
  for (uint32_t i = 0; i < cfPairIndexBound; ++i) {
    ControlFlowInstruction cfAb[2];
    UnpackControlFlowInstructions(ucode + size_t(i) * 3, cfAb);
    for (uint32_t j = 0; j < 2; ++j) {
      const ControlFlowInstruction& cf = cfAb[j];
      if (IsControlFlowOpcodeExec(cf.opcode())) {
        cfPairIndexBound = std::min(cfPairIndexBound, cf.exec.address());
      }
    }
  }
  if (cfPairIndexBound == 0) return false;

  bool sawExec = false;
  bool malformed = false;
  VertexFetchInstruction previousVfetchFull;
  std::memset(&previousVfetchFull, 0, sizeof(previousVfetchFull));
  bool havePreviousFull = false;

  for (uint32_t i = 0; i < cfPairIndexBound; ++i) {
    ControlFlowInstruction cfAb[2];
    UnpackControlFlowInstructions(ucode + size_t(i) * 3, cfAb);
    for (uint32_t j = 0; j < 2; ++j) {
      const ControlFlowInstruction& cf = cfAb[j];
      uint32_t execAddress, execCount, sequence;
      switch (cf.opcode()) {
        case ControlFlowOpcode::kExec:
        case ControlFlowOpcode::kExecEnd:
          execAddress = cf.exec.address();
          execCount = cf.exec.count();
          sequence = cf.exec.sequence();
          break;
        case ControlFlowOpcode::kCondExec:
        case ControlFlowOpcode::kCondExecEnd:
        case ControlFlowOpcode::kCondExecPredClean:
        case ControlFlowOpcode::kCondExecPredCleanEnd:
          execAddress = cf.cond_exec.address();
          execCount = cf.cond_exec.count();
          sequence = cf.cond_exec.sequence();
          break;
        case ControlFlowOpcode::kCondExecPred:
        case ControlFlowOpcode::kCondExecPredEnd:
          execAddress = cf.cond_exec_pred.address();
          execCount = cf.cond_exec_pred.count();
          sequence = cf.cond_exec_pred.sequence();
          break;
        default:
          continue;  // Non-exec CF: nothing to fetch from.
      }
      sawExec = true;

      // GatherExecInformation: sequence carries 2 bits per instruction;
      // bit 0 = fetch (vs ALU), bit 1 = serialize.
      for (uint32_t k = 0; k < execCount; ++k, sequence >>= 2) {
        const uint32_t instrOffset = execAddress + k;
        // Every instruction is 3 dwords — validate before touching memory.
        if (size_t(instrOffset) * 3 + 3 > dwordCount) {
          malformed = true;
          break;
        }
        if (!(sequence & 0b01)) continue;  // ALU.

        const uint32_t* p = ucode + size_t(instrOffset) * 3;
        VertexFetchInstruction op;
        op.setDwords(p[0], p[1], p[2]);
        if (op.opcode() != FetchOpcode::kVertexFetch) continue;  // tfetch.
        if (!op.must_be_one()) g_mustBeOneViolations++;

        if (gatherVertexFetch(op, previousVfetchFull, havePreviousFull, out)) {
          previousVfetchFull = op;
          havePreviousFull = true;
        }
      }
      if (malformed) break;
    }
    if (malformed) break;
  }

  return sawExec && !malformed;
}

}  // namespace ucode
}  // namespace gfx
}  // namespace sr
