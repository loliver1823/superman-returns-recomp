/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// PORTED FROM XENIA (Ben Vanik, BSD-3-Clause).
//   reference/xenia-canary/src/xenia/gpu/xenos.h    — VertexFormat, Endian,
//       SignedRepeatingFractionMode, GetVertexFormatComponentCount
//   reference/xenia-canary/src/xenia/gpu/ucode.h    — ControlFlow*Instruction,
//       VertexFetchInstruction, UnpackControlFlowInstructions
//   reference/xenia-canary/src/xenia/gpu/shader_translator.cc — the CF walk
//       (Shader::AnalyzeUcode / GatherExecInformation /
//        GatherVertexFetchInformation / ParseVertexFetchInstruction)
//
// The bitfield layouts below are copied VERBATIM from Xenia and must not be
// reordered — they mirror Xenos hardware instruction encodings.
//
// Purpose here: decode the *vertex format* of a draw, which does NOT live in
// the fetch constant (that only carries type/address/size/endian) but in the
// vertex shader's vfetch instructions.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sr {
namespace gfx {
namespace ucode {

// ─────────────────────────────── xenos.h ────────────────────────────────────

// xenos.h:191-196
enum class Endian : uint32_t {
  kNone = 0,
  k8in16 = 1,
  k8in32 = 2,
  k16in32 = 3,
};

// xenos.h:641-658 — values are NON-CONTIGUOUS; copied exactly.
enum class VertexFormat : uint32_t {
  kUndefined = 0,
  k_8_8_8_8 = 6,
  k_2_10_10_10 = 7,
  k_10_11_11 = 16,
  k_11_11_10 = 17,
  k_16_16 = 25,
  k_16_16_16_16 = 26,
  k_16_16_FLOAT = 31,
  k_16_16_16_16_FLOAT = 32,
  k_32 = 33,
  k_32_32 = 34,
  k_32_32_32_32 = 35,
  k_32_FLOAT = 36,
  k_32_32_FLOAT = 37,
  k_32_32_32_32_FLOAT = 38,
  k_32_32_32_FLOAT = 57,
};

// xenos.h:800-806
enum class SignedRepeatingFractionMode : uint32_t {
  // Microsoft-style representation with two -1 representations (one is slightly
  // past -1 but clamped).
  kZeroClampMinusOne,
  // OpenGL "alternate mapping" format lacking representation for zero.
  kNoZero,
};

// xenos.h:660-685 (assert_unhandled_case → return 0).
inline int GetVertexFormatComponentCount(VertexFormat format) {
  switch (format) {
    case VertexFormat::k_32:
    case VertexFormat::k_32_FLOAT:
      return 1;
    case VertexFormat::k_16_16:
    case VertexFormat::k_16_16_FLOAT:
    case VertexFormat::k_32_32:
    case VertexFormat::k_32_32_FLOAT:
      return 2;
    case VertexFormat::k_10_11_11:
    case VertexFormat::k_11_11_10:
    case VertexFormat::k_32_32_32_FLOAT:
      return 3;
    case VertexFormat::k_8_8_8_8:
    case VertexFormat::k_2_10_10_10:
    case VertexFormat::k_16_16_16_16:
    case VertexFormat::k_16_16_16_16_FLOAT:
    case VertexFormat::k_32_32_32_32:
    case VertexFormat::k_32_32_32_32_FLOAT:
      return 4;
    default:
      return 0;
  }
}

// Enum-name string for logging. Returns "kInvalid" for out-of-enum values.
const char* formatName(VertexFormat format);

// ─────────────────────────────── ucode.h ────────────────────────────────────

// ucode.h:87-159
enum class ControlFlowOpcode : uint32_t {
  kNop = 0,
  kExec = 1,
  kExecEnd = 2,
  kCondExec = 3,
  kCondExecEnd = 4,
  kCondExecPred = 5,
  kCondExecPredEnd = 6,
  kLoopStart = 7,
  kLoopEnd = 8,
  kCondCall = 9,
  kReturn = 10,
  kCondJmp = 11,
  kAlloc = 12,
  kCondExecPredClean = 13,
  kCondExecPredCleanEnd = 14,
  kMarkVsFetchDone = 15,
};

// ucode.h:163-176
constexpr bool IsControlFlowOpcodeExec(ControlFlowOpcode opcode) {
  return opcode == ControlFlowOpcode::kExec ||
         opcode == ControlFlowOpcode::kExecEnd ||
         opcode == ControlFlowOpcode::kCondExec ||
         opcode == ControlFlowOpcode::kCondExecEnd ||
         opcode == ControlFlowOpcode::kCondExecPred ||
         opcode == ControlFlowOpcode::kCondExecPredEnd ||
         opcode == ControlFlowOpcode::kCondExecPredClean ||
         opcode == ControlFlowOpcode::kCondExecPredCleanEnd;
}

// ucode.h:191-199
enum class AddressingMode : uint32_t {
  kRelative = 0,
  kAbsolute = 1,
};

// ucode.h:203-215
enum class AllocType : uint32_t {
  kNone = 0,
  kVsPosition = 1,
  kVsInterpolators = 2,
  kPsColors = 2,
  kMemory = 3,
};

// ucode.h:218-260 — kExec / kExecEnd.
struct ControlFlowExecInstruction {
  ControlFlowOpcode opcode() const { return opcode_; }
  AddressingMode addressing_mode() const { return address_mode_; }
  uint32_t address() const { return address_; }
  uint32_t count() const { return count_; }
  // Sequence bits, 2 per instruction. [0] - ALU (0) or fetch (1), [1] -
  // serialize.
  uint32_t sequence() const { return sequence_; }
  bool is_predicate_clean() const { return is_predicate_clean_ == 1; }
  bool is_yield() const { return is_yield_ == 1; }

 private:
  // Word 0: (32 bits)
  uint32_t address_ : 12;
  uint32_t count_ : 3;
  uint32_t is_yield_ : 1;
  uint32_t sequence_ : 12;
  uint32_t vc_hi_ : 4;  // Vertex cache?

  // Word 1: (16 bits)
  uint32_t vc_lo_ : 2;
  uint32_t : 7;
  uint32_t is_predicate_clean_ : 1;
  uint32_t : 1;
  AddressingMode address_mode_ : 1;
  ControlFlowOpcode opcode_ : 4;
};
static_assert(sizeof(ControlFlowExecInstruction) == sizeof(uint32_t) * 2, "");

// ucode.h:264-300 — kCondExec, kCondExecEnd, kCondExecPredClean(End).
struct ControlFlowCondExecInstruction {
  ControlFlowOpcode opcode() const { return opcode_; }
  AddressingMode addressing_mode() const { return address_mode_; }
  uint32_t address() const { return address_; }
  uint32_t count() const { return count_; }
  uint32_t sequence() const { return sequence_; }
  uint32_t bool_address() const { return bool_address_; }
  bool condition() const { return condition_ == 1; }
  bool is_yield() const { return is_yield_ == 1; }

 private:
  // Word 0: (32 bits)
  uint32_t address_ : 12;
  uint32_t count_ : 3;
  uint32_t is_yield_ : 1;
  uint32_t sequence_ : 12;
  uint32_t vc_hi_ : 4;

  // Word 1: (16 bits)
  uint32_t vc_lo_ : 2;
  uint32_t bool_address_ : 8;
  uint32_t condition_ : 1;
  AddressingMode address_mode_ : 1;
  ControlFlowOpcode opcode_ : 4;
};
static_assert(sizeof(ControlFlowCondExecInstruction) == sizeof(uint32_t) * 2,
              "");

// ucode.h:304-338 — kCondExecPred, kCondExecPredEnd.
struct ControlFlowCondExecPredInstruction {
  ControlFlowOpcode opcode() const { return opcode_; }
  AddressingMode addressing_mode() const { return address_mode_; }
  uint32_t address() const { return address_; }
  uint32_t count() const { return count_; }
  uint32_t sequence() const { return sequence_; }
  bool is_predicate_clean() const { return is_predicate_clean_ == 1; }
  bool condition() const { return condition_ == 1; }
  bool is_yield() const { return is_yield_ == 1; }

 private:
  // Word 0: (32 bits)
  uint32_t address_ : 12;
  uint32_t count_ : 3;
  uint32_t is_yield_ : 1;
  uint32_t sequence_ : 12;
  uint32_t vc_hi_ : 4;

  // Word 1: (16 bits)
  uint32_t vc_lo_ : 2;
  uint32_t : 7;
  uint32_t is_predicate_clean_ : 1;
  uint32_t condition_ : 1;
  AddressingMode address_mode_ : 1;
  ControlFlowOpcode opcode_ : 4;
};
static_assert(sizeof(ControlFlowCondExecPredInstruction) ==
                  sizeof(uint32_t) * 2,
              "");

// ucode.h:483-509
union ControlFlowInstruction {
  ControlFlowOpcode opcode() const { return opcode_value; }

  ControlFlowExecInstruction exec;                    // kExec*
  ControlFlowCondExecInstruction cond_exec;           // kCondExec*
  ControlFlowCondExecPredInstruction cond_exec_pred;  // kCondExecPred*

  struct {
    uint32_t dword_0;
    uint32_t dword_1;
  };
  struct {
    uint32_t unused_0 : 32;
    uint32_t unused_1 : 12;
    ControlFlowOpcode opcode_value : 4;
  };
};
static_assert(sizeof(ControlFlowInstruction) == sizeof(uint32_t) * 2, "");

// ucode.h:515-524 — two CF instructions packed into three dwords.
inline void UnpackControlFlowInstructions(const uint32_t* dwords,
                                          ControlFlowInstruction* out_ab) {
  uint32_t dword_0 = dwords[0];
  uint32_t dword_1 = dwords[1];
  uint32_t dword_2 = dwords[2];
  out_ab[0].dword_0 = dword_0;
  out_ab[0].dword_1 = dword_1 & 0xFFFF;
  out_ab[1].dword_0 = (dword_1 >> 16) | (dword_2 << 16);
  out_ab[1].dword_1 = dword_2 >> 16;
}

// ucode.h:526-600 (only the members we need).
enum class FetchOpcode : uint32_t {
  kVertexFetch = 0,
  kTextureFetch = 1,
};

// ucode.h:672-679
enum class FetchDestinationSwizzle : uint32_t {
  kX = 0,
  kY = 1,
  kZ = 2,
  kW = 3,
  k0 = 4,
  k1 = 5,
  // Keep the current value of the destination register (don't write).
  kKeep = 7,
};

constexpr FetchDestinationSwizzle GetFetchDestinationComponentSwizzle(
    uint32_t swizzle, uint32_t component) {
  return FetchDestinationSwizzle((swizzle >> (3 * component)) & 0b111);
}

// ucode.h:690-775 — VERBATIM bitfield layout.
struct alignas(uint32_t) VertexFetchInstruction {
  FetchOpcode opcode() const { return data_.opcode_value; }

  bool is_predicated() const { return data_.is_predicated; }
  bool predicate_condition() const { return data_.pred_condition == 1; }
  // Vertex fetch constant index [0-95].
  // Applicable only to vfetch_full (the address from vfetch_full is reused in
  // vfetch_mini).
  uint32_t fetch_constant_index() const {
    return data_.const_index * 3 + data_.const_index_sel;
  }

  uint32_t dest() const { return data_.dst_reg; }
  uint32_t dest_swizzle() const { return data_.dst_swiz; }
  bool is_dest_relative() const { return data_.dst_reg_am; }
  uint32_t src() const { return data_.src_reg; }
  uint32_t src_swizzle() const { return data_.src_swiz; }
  bool is_src_relative() const { return data_.src_reg_am; }

  // Not in Xenia's accessor list — a layout self-check: the hardware encoding
  // requires this bit set on every valid vfetch, so it validates our dword_0
  // bitfield alignment (where const_index / src_reg / dst_reg live).
  bool must_be_one() const { return data_.must_be_one == 1; }

  uint32_t prefetch_count() const { return data_.prefetch_count; }
  bool is_mini_fetch() const { return data_.is_mini_fetch == 1; }

  VertexFormat data_format() const { return data_.format; }
  // [-32, 31]
  int exp_adjust() const { return data_.exp_adjust; }
  bool is_signed() const { return data_.fomat_comp_all == 1; }
  bool is_normalized() const { return data_.num_format_all == 0; }
  SignedRepeatingFractionMode signed_rf_mode() const {
    return data_.signed_rf_mode_all;
  }
  bool is_index_rounded() const { return data_.is_index_rounded == 1; }
  // Dword stride, [0, 255].
  uint32_t stride() const { return data_.stride; }
  // Dword offset, [-4194304, 4194303].
  int32_t offset() const { return data_.offset; }

  // Not in Xenia (its callers reinterpret_cast raw dwords); we copy dwords in.
  void setDwords(uint32_t d0, uint32_t d1, uint32_t d2) {
    raw_[0] = d0;
    raw_[1] = d1;
    raw_[2] = d2;
  }

 private:
  struct Data {
    struct {
      FetchOpcode opcode_value : 5;
      uint32_t src_reg : 6;
      uint32_t src_reg_am : 1;
      uint32_t dst_reg : 6;
      uint32_t dst_reg_am : 1;
      uint32_t must_be_one : 1;
      uint32_t const_index : 5;
      uint32_t const_index_sel : 2;
      // Prefetch count minus 1.
      uint32_t prefetch_count : 3;
      // Absolute, one component.
      uint32_t src_swiz : 2;
    };
    struct {
      uint32_t dst_swiz : 12;
      uint32_t fomat_comp_all : 1;
      uint32_t num_format_all : 1;
      SignedRepeatingFractionMode signed_rf_mode_all : 1;
      uint32_t is_index_rounded : 1;
      VertexFormat format : 6;
      uint32_t reserved2 : 2;
      int32_t exp_adjust : 6;
      uint32_t is_mini_fetch : 1;
      uint32_t is_predicated : 1;
    };
    struct {
      uint32_t stride : 8;
      int32_t offset : 23;
      uint32_t pred_condition : 1;
    };
  };
  union {
    Data data_;
    uint32_t raw_[3];
  };
};
static_assert(sizeof(VertexFetchInstruction) == sizeof(uint32_t) * 3, "");

// ─────────────────────── vertex binding extraction ──────────────────────────

struct Attribute {
  VertexFormat format = VertexFormat::kUndefined;
  int32_t offsetWords = 0;
  int32_t expAdjust = 0;
  bool isSigned = false;
  bool isInteger = false;
  SignedRepeatingFractionMode rfMode =
      SignedRepeatingFractionMode::kZeroClampMinusOne;
  uint32_t dstSwiz = 0;
};

struct VertexBinding {
  uint32_t fetchConstantIndex = 0;
  uint32_t strideWords = 0;
  std::vector<Attribute> attributes;
};

// Walks the CF program of a vertex shader's microcode and collects every
// vfetch_full / vfetch_mini, grouped into one binding per fetch constant.
//
// Modeled on Shader::AnalyzeUcode + GatherVertexFetchInformation +
// ParseVertexFetchInstruction. vfetch_mini inherits src_reg / stride /
// is_index_rounded / fetch constant from the preceding vfetch_full, but keeps
// its OWN format / offset / exp_adjust / is_signed / is_normalized.
//
// Never reads out of bounds; returns false on malformed ucode (out is then
// whatever was decoded before the fault — treat as unusable).
bool parseVertexShaderBindings(const uint32_t* ucode, size_t dwordCount,
                               std::vector<VertexBinding>& out);

// Count of decoded vfetch instructions whose must_be_one bit was NOT set,
// accumulated across all parses. A non-zero value means our dword_0 bitfield
// layout is misaligned and every field from it (fetch constant, registers) is
// suspect. Diagnostic only.
uint32_t debugMustBeOneViolations();

// iter-370: is this byte range STRUCTURALLY a Xenos shader at all?
//
// True only if the control-flow section contains at least one exec instruction
// whose target lies inside the shader. That is a type-marker, not a range check
// (the project rule: ASCII garbage passes ranges). It exists because IM_LOAD's
// declared size is not evidence — a parser that briefly leaves the command
// buffer hands this decoder whatever memory follows, and a 108-byte run of
// zeroes or of the ring's own fence packets is small enough to sail through any
// plausibility CAP while being obviously not a shader:
//
//   6a848a499327b455  ... C0003B00 | 00000003 0EFEECE6 8A7EB446 C0025800
//                                    ^ PM4 type-3 headers, ^ a ring address
//   f5874c4464d8e71c  00000000 x24 ...   (all-zero: no exec, no instructions)
//
// Binding one of those as a shader silently repoints lastVs/PsUcodeHash, so
// every draw that follows looks up the WRONG shader identity.
bool hasValidControlFlow(const uint32_t* ucode, size_t dwordCount);

// NATIVE N1 (docs/NATIVE_ROADMAP.md §4.3, "the constant-use walk"): which float
// constants a shader MAY read. Walks every exec clause; for each ALU instruction
// (Xenia ucode.h AluInstruction) every source whose sel bit is 0 (a constant)
// marks its 16-constant block (c0-15 = bit 0 ... c240-255 = bit 15). A source
// that is relatively addressed (Xenia src_const_is_addressed: a0 / aL) sets
// *relative (any constant may be read). Over-approximates: the three source
// fields are counted whatever the opcodes' operand counts - so "not marked"
// means "provably not read". Returns false on malformed ucode.
bool constantBlocksRead(const uint32_t* ucode, size_t dwordCount, uint32_t* blockMask, bool* relative);

}  // namespace ucode
}  // namespace gfx
}  // namespace sr
