#!/usr/bin/env python3
"""xtr_draws.py -- the Xenia GPU trace (.xtr) as a per-draw DATA oracle (TOOL-1).

The pixel oracle (xenia-gpu-d3d12-trace-dump) answers "what should the frame
look like". This tool answers the mechanism questions underneath it: for every
draw packet in the trace, WHAT the guest handed the GPU - shader pair, ALU
constants, fetch constants (vertex + texture), render-target/depth/viewport
registers, the vertex bytes - so a draw in OUR fixture can be diffed field by
field against the console's own state for the same shader pair.

PROVENANCE (every layout below is derived from these pins, not from memory):
  * Xenia at reference/xenia-canary @ 0b4e6c440e91f118b42a5e9e32a6b629ce4c387c
      src/xenia/gpu/trace_protocol.h            record layouts (v1)
      src/xenia/gpu/pm4_command_processor_implement.h
          ExecutePacketType0/1 (L328-L370), INDIRECT_BUFFER record special case
          (L393-L406: the 0x3F dispatch packet is recorded as TWO dwords and its
          inner packets follow as their own records), REG_RMW (L758), EVENT_WRITE
          (L867), EVENT_WRITE_EXT (L921: screen extents, NOT a resolve),
          ExecutePacketType3Draw (L1049), SET_CONSTANT (L1191), SET_CONSTANT2 /
          SET_SHADER_CONSTANTS (L1222, L1300), LOAD_ALU_CONSTANT (L1245),
          IM_LOAD (L1308), IM_LOAD_IMMEDIATE (L1338)
      src/xenia/gpu/d3d12/d3d12_command_processor.cc L2515-L2517: a draw
          issued while RB_MODECONTROL.edram_mode == EdramMode::kCopy IS the
          resolve (IssueCopy). That is the resolve classification used here.
      src/xenia/gpu/xenos.h L1618-L1678 PM4 opcodes; EdramMode (kNoOperation 0,
          kColorDepth 4, kDepthOnly 5, kCopy 6); CopyCommand
      src/xenia/gpu/registers.h RB_MODECONTROL (L660), RB_COPY_CONTROL (L853),
          RB_COPY_DEST_INFO (L870), RB_COPY_DEST_PITCH (L887),
          VGT_DRAW_INITIATOR (L311); register_table.inc register numbers
      src/xenia/gpu/ucode.h ControlFlowExecInstruction / CondExec /
          CondExecPred (L217-L328), UnpackControlFlowInstructions (L515),
          VertexFetchInstruction (L690-L755); shader_translator.cc
          AnalyzeUcode (L42-L130) and GatherExecInformation (L349-L378): the
          control-flow walk that decides WHICH 3-dword slots are fetches.
  * MCLA (3bdull4h2008/mcla-pc @ 5aa1969604b13fe44b5acc81afbed2943926a4fa),
    reused under the user's 2026-09-11 ruling with attribution:
      src/renderer/xtr_dump_validator.cpp  - the standalone record-level
          validator. Its per-record byte accounting (NEED/SKIP), its "exit
          nonzero with a reason" contract and its synthetic self-test builder
          shaped the fail-closed reader and tools/test_xtr_draws.py. Compiled
          from the pin and run as an independent cross-check (see TOOLKIT.md).
      src/renderer/xenos_microcode.cpp     - consulted for DecodeControlFlow
          (matches ucode.h); its vertex-fetch word-1 decode does NOT match
          ucode.h (exp_adjust/is_mini bit positions), so it was not reused.
  * The in-repo C++ mirror gfx/xenos_ucode.cpp (parseVertexShaderBindings)
    is the second implementation of the same Xenia walk; its [VFETCH] census
    in gfx/gpu_trace_replay.cpp is the independent cross-check for XT1.

FAIL CLOSED. Every record is read with exact-length reads; every count and
payload length is checked against the file; EVERY supplied payload is decoded
and length-checked before it is used or discarded - memory reads and writes
against their declared decoded length (writes are then discarded, never input
memory), register snapshots against count*4, EDRAM snapshots against
xenos::kEdramSizeBytes (10,485,760) and gamma ramps against (256 + 3*128)*4 =
2,560 bytes; a missing dependency is a named refusal (never a silent zero hash
or a zero-filled buffer); reads from memory the trace never supplied are
counted and reported, never hidden. Refusals exit nonzero with the byte offset
of the record and the reason.

RESOLVE SOURCE (draw_util.cc GetResolveInfo, L1082-L1083 and L1256-L1259):
copy_src_select 0..3 resolves the colour target whose register is
RB_COLOR_INFO::rt_register_indices[select] (0x2001, 0x2003, 0x2004, 0x2005);
4..7 resolves the depth buffer (RB_DEPTH_INFO 0x2002). The resolve views and
JSON `resolve_details` name that source by kind, index, register, raw value,
12-bit tile base, format and (colour only) the signed 6-bit exponent bias.

RESOLVE INPUTS (TOOL-2A, schema /3). Every resolve_details row carries
`get_resolve_info_inputs`: the trace-resident inputs of draw_util.cc
GetResolveInfo, captured LOSSLESSLY at the draw and never derived:
  * vertex fetch constant 0 (register_file.h L59-L67: registers 0x4800/0x4801,
    xenos.h L1158-L1172: type 2 bits, address 30 bits IN DWORDS, endian 2 bits,
    size 24 bits IN DWORDS) with both raw dwords and the decoded fields;
  * its 24 vertex bytes (six dwords) taken ONLY from decoded MemoryRead
    records that lie inside that draw's own PacketStart/PacketEnd interval -
    Xenia reads them at draw_util.cc L959-L962 and the writer records them as
    an UNCACHED MemoryRead (trace_writer.cc L142-L148) exactly there. The
    trace-wide memory image, the packet body's own write and MemoryWrite
    records never satisfy this proof (r1): a reused address whose current
    draw lacks all or part of its own read is INCOMPLETE, never a stale or
    mixed payload;
  * the raw registers GetResolveInfo/GetScissor read for the rectangle:
    PA_SU_VTX_CNTL, PA_SU_SC_MODE_CNTL, PA_SC_WINDOW_OFFSET, window scissor
    TL/BR, screen scissor TL/BR (draw_util.cc L963-L1016, GetScissor
    L551-L724) and the clear values RB_DEPTH_CLEAR / RB_COLOR_CLEAR /
    RB_COLOR_CLEAR_LO (L1301-L1303); numbers per register_table.inc;
  * provenance: the draw packet's record offset in the trace.
  Status is fail closed: a fetch that is not kVertex or not 6 dwords is a
  named GetResolveInfo REJECTION (L954-L958 returns false; no payload is
  read); a valid fetch whose 24 bytes were not all supplied by the trace is
  INCOMPLETE (missing count reported, no bytes emitted); only a fully
  supplied payload is COMPLETE. Nothing is zero-filled. Host configuration
  that no .xtr record carries (draw resolution scales, the two half-pixel
  cvars, the fixed-RG16/RGBA16 flags) is listed as `not_in_trace`, never
  invented. The adjusted destination base/extent and the rectangle itself
  were NOT computed by TOOL-2A; TOOL-2B (schema /4) derives them, see below.

GUEST DESTINATION RANGE (TOOL-2B, schema /4). Every resolve_details row also
carries `guest_destination_range`: a bit-exact Python port of the pinned
GetResolveInfo path from the /3 inputs - copy-command validation, vertex
endian swap (GpuSwap, all four modes), FloatToD3D11Fixed16p8 (NaN, infinities,
nearest-even, saturation), min/max + top-left rule, PA_SU_VTX_CNTL pixel
center, signed window offset + enable, GetScissor<false> with
window_offset_disable, 8-pixel expansion, surface-pitch clamp, maximum-height
clamp, the empty-region early return, depth-format substitution, the 64-entry
format table and resolvable classification, pitch/height alignment to 32,
base-granularity masks, Tiled2D/Tiled3D, lower/upper tiled bounds and the
extent invariants - with C++-width wrapping. The result depends on NONE of
the six host parameters (draw_util.cc L1076-L1079 only stores the scales;
L1228-L1233 and L1272-L1281 affect fields this derivation does not produce).
One top-level `derivation_manifest` carries the version, the pin and the
stage-to-source permalinks; rows carry only `derivation_version`, status,
auditable intermediates and the final range. Only a nonempty, fully derived
range is `join_usable`; /3 rejections and incompletes propagate without
arithmetic; unsupported copy command, invalid MSAA, the empty rectangle and
a non-resolvable colour format are named states, never normal ranges.

STATIC TEXTURE BINDINGS (TOOL-3A, schema /5). Every NORMAL draw (kColorDepth
or kDepth mode; kCopy resolves are not texture consumers in this census) gets
a `static_texture_bindings[]` row: the VS and the PS ucode bound at the draw
are walked with the pinned control-flow analysis (Shader::AnalyzeUcode,
GatherExecInformation, GatherTextureFetchInformation - the SAME walk the
vertex-fetch census uses) and every texture-fetch instruction the program
statically names is listed per stage with its opcode, fetch constant and
dimension; setTexLOD / setGradientH / setGradientV use no binding, exactly as
in Xenia; the union slot mask is deduplicated only at the end; both stages'
references to one slot are kept. For every referenced slot the row carries
the raw six-dword fetch constant AND its complete decode from the pinned
xe_gpu_texture_fetch_t layout (type/sign/clamp/pitch/tiled; format /
endianness / request size / stacked / base; the 1D / 2D-or-stacked / 3D size
views; number format, swizzle, signed exponent adjust, filters; volume and
mip controls with signed LOD bias and gradient adjusts; border, dimension,
packed-mip flag and mip page). These are STATIC BINDING OPPORTUNITIES -
instructions the shader could execute - not dynamically executed samples:
predicates, branches, coordinates and LOD are not proven. A malformed,
unmapped or missing shader is a named incomplete state; the tool NEVER falls
back to "every type-2 fetch constant". The census conserves
draws_with_static_binding + draws_with_no_static_binding +
draws_analysis_not_complete == normal_draws_total and reports T3A1 (is the
set of type-2 constants the referenced set?) with explicit denominators. No
consumer memory range, producer match, ownership, contents or pixel claim is
made; the post-translation mask Xenia requests at draw time
(dxbc_shader_translator.cc PostTranslation) is NOT reproduced here.

REQUESTED BINDINGS AND TEXTURE BACKING (TOOL-3B, schema /6). Xenia requests
textures at draw time from the union of the two stages' POST-TRANSLATION
masks (DxbcShader::GetUsedTextureMaskAfterTranslation, formed in
DxbcShaderTranslator::PostTranslation). This tool cannot run that translator;
it CONSUMES a table recorded by running the actual pinned translator
(--post-masks, produced by archive/tool3b/mask_oracle.py through a host
program linked against the pinned checkout's own build), keyed by stage and
the exact ucode SHA-256, and marks every stage 'translated' /
'translation_invalid' / 'analysis_mismatch' / 'not_in_table' / 'no_table' /
'ucode_unmapped' / 'missing'. Slots in the post-mask union are REQUESTED
BINDING OPPORTUNITIES - still static: no predicate, branch or sample is
proven executed. The /5 per-draw repetition is normalized losslessly:
`static_texture_shaders` (one analysis per exact ucode content, keyed by
SHA-256, carrying the trace's XXH3 identity) and `texture_fetch_constants`
(one entry per exact 24-byte raw constant, keyed by its 48 hex digits); rows
reference both; --expand-v5 reconstructs the accepted /5 projection exactly.
Only for requested constants the pinned texture path is ported:
TextureCache::BindingInfoFromFetchConstant (type check under the pinned
default gpu_allow_invalid_fetch_constants=true, 1D validity, GetBaseFormat),
texture_util::GetSubresourcesFromFetchConstant (size views, 17-bit physical
pages, mip range normalization, base/mip page dropping) and
GetGuestTextureLayout (pitch/stride/extent per level, packed mip tail, tiled
upper bounds, 4 KB subresource alignment) - giving SEPARATE base and mip
guest backing intervals exactly as Texture::GetGuestBaseSize/GetGuestMipsSize
watch them. Invalid, no-data, invalid-1D and zero-byte-block formats are
named states with no interval. The intervals are what Xenia would keep
resident for the texture, not bytes proven sampled; no producer match,
ownership, contents or pixel claim is made.

RESOLVE/CONSUMER INTERVAL RELATION (TOOL-3C, schema /7). A TEMPORAL,
COMPONENT-LEVEL, HALF-OPEN interval relation: every join-usable resolve
destination range (TOOL-2B, `[extent_start, extent_end_exclusive)`) is compared
with the base and the mip backing interval (TOOL-3B, kept separate) of every
requested binding occurrence of every STRICTLY LATER normal draw of the same
trace, and classified as exactly one of equal / producer_contains_consumer /
consumer_contains_producer / partial_overlap / disjoint; endpoint contact is
disjoint; the exact intersection is kept for every non-disjoint pair and
detail rows exist only for those. Every overlapping earlier resolve is kept -
the tool never selects a latest or preferred owner, never infers that a copy
or a sample executed, never calls a producer destination and a consumer key
format-compatible and never infers resource identity or contents. The cvar
`gpu_allow_invalid_fetch_constants` is consumed as a FILTER (its false setting
excludes occurrences whose backing was accepted only under the true default);
both projections are reported side by side and the detail table is the
pinned-default one.

Exit codes: 0 ok | 1 usage | 2 malformed trace (offset+reason)
            3 missing dependency | 4 unsupported encoding/version/tag

  python tools/xtr_draws.py TRACE                       # summary
  python tools/xtr_draws.py TRACE --json out.json       # + machine-readable summary
  python tools/xtr_draws.py TRACE --list                # one row per draw packet
  python tools/xtr_draws.py TRACE --draw N --consts     # VS/PS float file
  python tools/xtr_draws.py TRACE --draw N --verts 8    # decoded vfetch bytes
  python tools/xtr_draws.py TRACE --draw N --textures   # static + requested bindings, decoded constants, backing
  python tools/xtr_draws.py TRACE --post-masks archive/tool3b/masks.json --json out.json --expand-v5 v5.json
  python tools/xtr_draws.py TRACE --post-masks archive/tool3b/masks.json --relations   # resolve/consumer interval relation
  python tools/xtr_draws.py TRACE --shaders             # per-pair census
  python tools/xtr_draws.py TRACE --resolves            # kCopy-mode draws
  python tools/xtr_draws.py TRACE --vfetch-census       # XT1: CF walk vs naive scan

EVIDENCE LIMITS: a single-frame trace is one frame; counts are per trace;
"resolve" means a kCopy-mode draw per Xenia (EVENT_WRITE_EXT is counted
separately as a screen-extent event); this tool renders nothing and is not a
pixel oracle; register state is the trace's, not ours.
"""
import argparse
import array
import bisect
import collections
import hashlib
import json
import os
import struct
import sys

try:
    import cramjam
except ImportError:  # pragma: no cover - refused by name when needed
    cramjam = None
try:
    import xxhash
except ImportError:  # pragma: no cover - refused by name when needed
    xxhash = None

TOOL_VERSION = '2.6.0'
SCHEMA = 'xtr_draws-summary/7'
XENIA_PIN = '0b4e6c440e91f118b42a5e9e32a6b629ce4c387c'
MCLA_PIN = '5aa1969604b13fe44b5acc81afbed2943926a4fa'

EXIT_OK, EXIT_USAGE, EXIT_MALFORMED, EXIT_DEPENDENCY, EXIT_UNSUPPORTED = 0, 1, 2, 3, 4
EXIT_INCOMPLETE = 5   # iter-463 --coverage: a bound backing the trace never supplied

# TraceCommandType (trace_protocol.h)
K_PRIM_START, K_PRIM_END, K_IB_START, K_IB_END, K_PKT_START, K_PKT_END, \
    K_MEM_READ, K_MEM_WRITE, K_EDRAM, K_EVENT, K_REGS, K_GAMMA = range(12)
ENC_NONE, ENC_SNAPPY = 0, 1
HEADER_BYTES = 48
TRACE_VERSION = 1

PAGE_SHIFT = 16
PAGE = 1 << PAGE_SHIFT
PHYS_MASK = 0x1FFFFFFF
MAX_DECODED_BYTES = 512 << 20
MAX_REGS = 0x5003          # RegisterFile::kRegisterCount (register_file.h L40)
MAX_WARNINGS = 64

# PM4 type-3 opcodes (xenos.h L1618-L1678)
OP_NOP, OP_REG_RMW, OP_DRAW_INDX, OP_VIZ_QUERY = 0x10, 0x21, 0x22, 0x23
OP_IM_LOAD, OP_IM_LOAD_IMMEDIATE, OP_SET_CONSTANT, OP_LOAD_ALU_CONSTANT = 0x27, 0x2B, 0x2D, 0x2F
OP_DRAW_INDX_BIN, OP_DRAW_INDX_2_BIN, OP_DRAW_INDX_2, OP_INDIRECT_BUFFER_PFD = 0x34, 0x35, 0x36, 0x37
OP_INVALIDATE_STATE, OP_WAIT_REG_MEM, OP_MEM_WRITE, OP_INDIRECT_BUFFER = 0x3B, 0x3C, 0x3D, 0x3F
OP_COND_WRITE, OP_EVENT_WRITE, OP_ME_INIT, OP_SET_BIN_BASE_OFFSET = 0x45, 0x46, 0x48, 0x4B
OP_SET_BIN_MASK, OP_SET_BIN_SELECT, OP_INTERRUPT, OP_SET_CONSTANT2 = 0x50, 0x51, 0x54, 0x55
OP_SET_SHADER_CONSTANTS, OP_EVENT_WRITE_SHD, OP_EVENT_WRITE_CFL = 0x56, 0x58, 0x59
OP_EVENT_WRITE_EXT, OP_EVENT_WRITE_ZPD, OP_CONTEXT_UPDATE = 0x5A, 0x5B, 0x5E
OP_SET_BIN_MASK_LO, OP_SET_BIN_MASK_HI, OP_SET_BIN_SELECT_LO, OP_SET_BIN_SELECT_HI = 0x60, 0x61, 0x62, 0x63
OP_XE_SWAP = 0x64
IGNORED_OPS = frozenset({
    OP_NOP, OP_VIZ_QUERY, OP_INVALIDATE_STATE, OP_WAIT_REG_MEM, OP_COND_WRITE,
    OP_ME_INIT, OP_SET_BIN_BASE_OFFSET, OP_SET_BIN_MASK, OP_SET_BIN_SELECT,
    OP_INTERRUPT, OP_EVENT_WRITE_CFL, OP_EVENT_WRITE_ZPD, OP_CONTEXT_UPDATE,
    OP_SET_BIN_MASK_LO, OP_SET_BIN_MASK_HI, OP_SET_BIN_SELECT_LO,
    OP_SET_BIN_SELECT_HI, OP_XE_SWAP,
})

# Registers (register_table.inc)
REG_RB_SURFACE_INFO, REG_RB_COLOR_INFO, REG_RB_DEPTH_INFO = 0x2000, 0x2001, 0x2002
REG_VGT_EVENT_INITIATOR, REG_VGT_DRAW_INITIATOR = 0x21F9, 0x21FC
REG_VGT_DMA_BASE, REG_VGT_DMA_SIZE = 0x21FD, 0x21FE
REG_RB_MODECONTROL = 0x2208
REG_RB_COPY_CONTROL, REG_RB_COPY_DEST_BASE = 0x2318, 0x2319
REG_RB_COPY_DEST_PITCH, REG_RB_COPY_DEST_INFO = 0x231A, 0x231B
REG_RB_COLOR1_INFO, REG_RB_COLOR2_INFO, REG_RB_COLOR3_INFO = 0x2003, 0x2004, 0x2005
# RB_COLOR_INFO::rt_register_indices (registers.cc L16-L20): the colour target
# a copy_src_select of 0..3 names. NOT contiguous: 0x2002 is RB_DEPTH_INFO.
RT_INFO_REGS = (REG_RB_COLOR_INFO, REG_RB_COLOR1_INFO, REG_RB_COLOR2_INFO, REG_RB_COLOR3_INFO)
MAX_COLOR_RENDER_TARGETS = 4          # xenos::kMaxColorRenderTargets
# xenos::ColorRenderTargetFormat / DepthRenderTargetFormat (xenos.h L297-L316, L404-L408)
COLOR_RT_FORMAT_NAMES = {
    0: 'k_8_8_8_8', 1: 'k_8_8_8_8_GAMMA', 2: 'k_2_10_10_10', 3: 'k_2_10_10_10_FLOAT',
    4: 'k_16_16', 5: 'k_16_16_16_16', 6: 'k_16_16_FLOAT', 7: 'k_16_16_16_16_FLOAT',
    10: 'k_2_10_10_10_AS_10_10_10_10', 12: 'k_2_10_10_10_FLOAT_AS_16_16_16_16',
    14: 'k_32_FLOAT', 15: 'k_32_32_FLOAT',
}
DEPTH_RT_FORMAT_NAMES = {0: 'kD24S8', 1: 'kD24FS8'}
# Payloads whose records carry NO decoded length: the snapshot is the whole
# EDRAM (xenos.h L453-L457: 2048 tiles * 16 * 80 samples * 4 bytes) and the
# gamma ramp is 256 DC_LUT_30_COLOR + 3*128 DC_LUT_PWL_DATA dwords
# (trace_writer.cc L323-L328), or the record is malformed.
EDRAM_BYTES = 2048 * 16 * 80 * 4
GAMMA_BYTES = (256 + 3 * 128) * 4

# GetResolveInfo's trace-resident register inputs (draw_util.cc L963-L1016,
# GetScissor L551-L724, clear values L1301-L1303); numbers per register_table.inc.
REG_PA_SC_SCREEN_SCISSOR_TL, REG_PA_SC_SCREEN_SCISSOR_BR = 0x200E, 0x200F   # L1245-L1246
REG_PA_SC_WINDOW_OFFSET = 0x2080                                             # L1248
REG_PA_SC_WINDOW_SCISSOR_TL, REG_PA_SC_WINDOW_SCISSOR_BR = 0x2081, 0x2082    # L1249-L1250
REG_PA_SU_SC_MODE_CNTL = 0x2205                                              # L1303
REG_PA_SU_VTX_CNTL = 0x2302                                                  # L1336
REG_RB_DEPTH_CLEAR, REG_RB_COLOR_CLEAR, REG_RB_COLOR_CLEAR_LO = 0x231D, 0x231E, 0x231F   # L1363-L1365
RESOLVE_INPUT_REGS = (
    ('PA_SU_VTX_CNTL', REG_PA_SU_VTX_CNTL),
    ('PA_SU_SC_MODE_CNTL', REG_PA_SU_SC_MODE_CNTL),
    ('PA_SC_WINDOW_OFFSET', REG_PA_SC_WINDOW_OFFSET),
    ('PA_SC_WINDOW_SCISSOR_TL', REG_PA_SC_WINDOW_SCISSOR_TL),
    ('PA_SC_WINDOW_SCISSOR_BR', REG_PA_SC_WINDOW_SCISSOR_BR),
    ('PA_SC_SCREEN_SCISSOR_TL', REG_PA_SC_SCREEN_SCISSOR_TL),
    ('PA_SC_SCREEN_SCISSOR_BR', REG_PA_SC_SCREEN_SCISSOR_BR),
    ('RB_DEPTH_CLEAR', REG_RB_DEPTH_CLEAR),
    ('RB_COLOR_CLEAR', REG_RB_COLOR_CLEAR),
    ('RB_COLOR_CLEAR_LO', REG_RB_COLOR_CLEAR_LO),
)
# xenos::FetchConstantType (xenos.h L1147-L1152); draw_util.cc L954 requires kVertex and 3*2 dwords
FETCH_TYPE_NAMES = {0: 'kInvalidTexture', 1: 'kInvalidVertex', 2: 'kTexture', 3: 'kVertex'}
FETCH_TYPE_VERTEX = 3
RESOLVE_VERTEX_DWORDS = 6
# GetResolveInfo parameters that NO .xtr record carries: caller-supplied
# (d3d12_render_target_cache.cc L1307-L1312) or cvars read inside it
# (draw_util.cc L1230-L1233). Reported by name, never invented.
RESOLVE_INPUTS_NOT_IN_TRACE = (
    'draw_resolution_scale_x', 'draw_resolution_scale_y',
    'resolve_resolution_scale_fill_half_pixel_offset', 'half_pixel_offset',
    'fixed_rg16_truncated_to_minus_1_to_1', 'fixed_rgba16_truncated_to_minus_1_to_1',
)
ALU_BASE, FETCH_BASE, BOOL_BASE, LOOP_BASE, REGS_BASE = 0x4000, 0x4800, 0x4900, 0x4908, 0x2000
CONSTANT_BASES = {0: ALU_BASE, 1: FETCH_BASE, 2: BOOL_BASE, 3: LOOP_BASE, 4: REGS_BASE}

# EdramMode (xenos.h)
MODE_NOOP, MODE_COLOR_DEPTH, MODE_DEPTH_ONLY, MODE_COPY = 0, 4, 5, 6
MODE_NAMES = {MODE_NOOP: 'noop', MODE_COLOR_DEPTH: 'color+depth',
              MODE_DEPTH_ONLY: 'depth', MODE_COPY: 'COPY(resolve)'}
COPY_COMMAND_NAMES = {0: 'raw', 1: 'convert', 2: 'constant_one', 3: 'null'}

REG_NAMES = {
    0x2000: 'RB_SURFACE_INFO', 0x2001: 'RB_COLOR_INFO', 0x2002: 'RB_DEPTH_INFO',
    0x2003: 'RB_COLOR1_INFO', 0x2004: 'RB_COLOR2_INFO', 0x2005: 'RB_COLOR3_INFO',
    0x200E: 'PA_SC_SCREEN_SCISSOR_TL', 0x200F: 'PA_SC_SCREEN_SCISSOR_BR',
    0x2080: 'PA_SC_WINDOW_OFFSET', 0x2081: 'PA_SC_WINDOW_SCISSOR_TL',
    0x2082: 'PA_SC_WINDOW_SCISSOR_BR', 0x2100: 'VGT_MAX_VTX_INDX',
    0x2101: 'VGT_MIN_VTX_INDX', 0x2102: 'VGT_INDX_OFFSET',
    0x2103: 'VGT_MULTI_PRIM_IB_RESET_INDX', 0x2104: 'RB_COLOR_MASK',
    0x2105: 'RB_BLEND_RED', 0x210F: 'PA_CL_VPORT_XSCALE', 0x2110: 'PA_CL_VPORT_XOFFSET',
    0x2111: 'PA_CL_VPORT_YSCALE', 0x2112: 'PA_CL_VPORT_YOFFSET',
    0x2113: 'PA_CL_VPORT_ZSCALE', 0x2114: 'PA_CL_VPORT_ZOFFSET',
    0x2180: 'SQ_PROGRAM_CNTL', 0x2181: 'SQ_CONTEXT_MISC', 0x2182: 'SQ_INTERPOLATOR_CNTL',
    0x21F9: 'VGT_EVENT_INITIATOR', 0x21FC: 'VGT_DRAW_INITIATOR', 0x21FD: 'VGT_DMA_BASE',
    0x21FE: 'VGT_DMA_SIZE',
    0x2200: 'RB_DEPTHCONTROL', 0x2201: 'RB_BLENDCONTROL0', 0x2202: 'RB_COLORCONTROL',
    0x2204: 'PA_CL_CLIP_CNTL', 0x2205: 'PA_SU_SC_MODE_CNTL', 0x2206: 'PA_CL_VTE_CNTL',
    0x2208: 'RB_MODECONTROL', 0x2209: 'RB_BLENDCONTROL1', 0x220A: 'RB_BLENDCONTROL2',
    0x220B: 'RB_BLENDCONTROL3', 0x2301: 'RB_ALPHA_REF', 0x2302: 'PA_SU_VTX_CNTL', 0x2307: 'SQ_VS_CONST',
    0x2308: 'SQ_PS_CONST', 0x2318: 'RB_COPY_CONTROL', 0x2319: 'RB_COPY_DEST_BASE',
    0x231A: 'RB_COPY_DEST_PITCH', 0x231B: 'RB_COPY_DEST_INFO', 0x231D: 'RB_DEPTH_CLEAR',
    0x231E: 'RB_COLOR_CLEAR', 0x231F: 'RB_COLOR_CLEAR_LO',
}
DRAW_REGS = sorted(REG_NAMES)

# xenos::VertexFormat (ucode.h / gfx/xenos_ucode.hpp) - the ENUM values, not
# the r1 script's guesses (which had k_16_16 at 24).
VFMT = {
    0: 'kUndefined', 6: 'k_8_8_8_8', 7: 'k_2_10_10_10', 16: 'k_10_11_11',
    17: 'k_11_11_10', 25: 'k_16_16', 26: 'k_16_16_16_16', 31: 'k_16_16_FLOAT',
    32: 'k_16_16_16_16_FLOAT', 33: 'k_32', 34: 'k_32_32', 35: 'k_32_32_32_32',
    36: 'k_32_FLOAT', 37: 'k_32_32_FLOAT', 38: 'k_32_32_32_32_FLOAT',
    57: 'k_32_32_32_FLOAT',
}


class TraceError(Exception):
    """A named refusal: exit `code`, at byte `offset`, because `reason`."""

    def __init__(self, code, offset, reason):
        super().__init__('%s (offset %d)' % (reason, offset))
        self.code, self.offset, self.reason = code, offset, reason


# ── memory ───────────────────────────────────────────────────────────────────

class Memory:
    """Sparse 512 MB physical memory as 64 KB pages, plus the exact byte
    ranges the trace supplied (merged intervals). Reads report how many bytes
    fell OUTSIDE every supplied range; those bytes are zero in the returned
    buffer but never silently: the caller must account for them."""

    def __init__(self):
        self.pages = {}
        self._starts = []     # merged supplied intervals, sorted
        self._ends = []       # exclusive ends, parallel to _starts
        # iter-463: WHEN each byte was first supplied - disjoint intervals
        # (start, end, seq) sorted by start, seq = the write's ordinal in the
        # stream. Xenia's TracePlayer applies a MemoryRead when it reaches the
        # record, so a draw only sees bytes whose first seq precedes it.
        self.seq = 0
        self._fs = []
        self._fe = []
        self._fq = []

    def _cover(self, a, b):
        if b <= a:
            return
        i = bisect.bisect_left(self._starts, a)
        if i > 0 and self._ends[i - 1] >= a:
            i -= 1
            a = self._starts[i]
        j = i
        while j < len(self._starts) and self._starts[j] <= b:
            b = max(b, self._ends[j])
            j += 1
        self._starts[i:j] = [a]
        self._ends[i:j] = [b]

    def _gaps(self, a, b):
        """The sub-intervals of [a, b) no record has supplied yet."""
        out = []
        pos = a
        i = bisect.bisect_right(self._starts, a) - 1
        if i >= 0 and self._ends[i] > a:
            pos = min(b, self._ends[i])
        i += 1
        while pos < b and i < len(self._starts) and self._starts[i] < b:
            if self._starts[i] > pos:
                out.append((pos, self._starts[i]))
            pos = min(b, self._ends[i])
            i += 1
        if pos < b:
            out.append((pos, b))
        return out

    def missing_before(self, a, b, seq):
        """Bytes in [a, b) whose first supplying write is NOT before stream
        ordinal `seq` - never supplied, or supplied only later (iter-463)."""
        if b <= a:
            return 0
        have = 0
        i = max(0, bisect.bisect_right(self._fs, a) - 1)
        while i < len(self._fs) and self._fs[i] < b:
            if self._fq[i] < seq:
                s, t = max(a, self._fs[i]), min(b, self._fe[i])
                if t > s:
                    have += t - s
            i += 1
        return (b - a) - have

    def uncovered(self, a, b):
        """Bytes in [a, b) that no record supplied."""
        if b <= a:
            return 0
        missing = 0
        pos = a
        i = bisect.bisect_right(self._starts, a) - 1
        if i >= 0 and self._ends[i] > a:
            pos = min(b, self._ends[i])
        i += 1
        while pos < b and i < len(self._starts) and self._starts[i] < b:
            missing += self._starts[i] - pos
            pos = min(b, self._ends[i])
            i += 1
        if pos < b:
            missing += b - pos
        return missing

    def write(self, addr, data):
        addr &= PHYS_MASK
        mv = memoryview(data)
        for s, t in self._gaps(addr, addr + len(mv)):
            i = bisect.bisect_left(self._fs, s)
            self._fs.insert(i, s)
            self._fe.insert(i, t)
            self._fq.insert(i, self.seq)
        self.seq += 1
        self._cover(addr, addr + len(mv))
        off = 0
        while off < len(mv):
            p, o = divmod(addr + off, PAGE)
            n = min(PAGE - o, len(mv) - off)
            pg = self.pages.get(p)
            if pg is None:
                pg = bytearray(PAGE)
                self.pages[p] = pg
            pg[o:o + n] = mv[off:off + n]
            off += n

    def read(self, addr, n):
        """(bytes, missing_bytes) - missing counts bytes outside every
        supplied range, exactly."""
        addr &= PHYS_MASK
        out = bytearray()
        off = 0
        while off < n:
            p, o = divmod(addr + off, PAGE)
            k = min(PAGE - o, n - off)
            pg = self.pages.get(p)
            if pg is None:
                out += bytes(k)
            else:
                out += pg[o:o + k]
            off += k
        return bytes(out), self.uncovered(addr, addr + n)

    def has(self, addr, n=1):
        """True iff every byte of [addr, addr+n) was supplied by the trace."""
        addr &= PHYS_MASK
        return self.uncovered(addr, addr + n) == 0

    def page_count(self):
        return len(self.pages)


# ── TOOL-2B: the pinned GetResolveInfo guest destination range ───────────────
#
# Every function below is a transcription of the pinned source named in
# DERIVATION_MANIFEST['stages']; integer widths are emulated explicitly
# (u32 / i32 / i64 wrap, sign extension of bitfields) so the Python result is
# the C++ result. Nothing here consumes a host parameter.

DERIVATION_VERSION = 'xtr_draws-resolve-range/1'
XENIA_REPOSITORY = 'https://github.com/xenia-canary/xenia-canary'
MAX_RESOLVE_SIZE = (1 << 14) - 8          # xenos.h L1027-L1029: (1 << kResolveSizeBits) - kResolveAlignmentPixels
STORAGE_ALIGN_BLOCKS = 32                 # texture_address.h L65-L67
TEXTURE_TILE_WH = 32                      # xenos.h L1198-L1199
TEXTURE_TILE_DEPTH = 4                    # xenos.h L1202-L1203
# texture_info_formats.inl L1-L64 (name, FormatType, bits_per_pixel) indexed by TextureFormat
FORMAT_TABLE = [
    ('k_1_REVERSE', 'kUncompressed', 1), ('k_1', 'kUncompressed', 1), ('k_8', 'kResolvable', 8),
    ('k_1_5_5_5', 'kResolvable', 16), ('k_5_6_5', 'kResolvable', 16), ('k_6_5_5', 'kResolvable', 16),
    ('k_8_8_8_8', 'kResolvable', 32), ('k_2_10_10_10', 'kResolvable', 32), ('k_8_A', 'kResolvable', 8),
    ('k_8_B', 'kResolvable', 8), ('k_8_8', 'kResolvable', 16), ('k_Cr_Y1_Cb_Y0_REP', 'kCompressed', 16),
    ('k_Y1_Cr_Y0_Cb_REP', 'kCompressed', 16), ('k_16_16_EDRAM', 'kUncompressed', 32),
    ('k_8_8_8_8_A', 'kResolvable', 32), ('k_4_4_4_4', 'kResolvable', 16), ('k_10_11_11', 'kResolvable', 32),
    ('k_11_11_10', 'kResolvable', 32), ('k_DXT1', 'kCompressed', 4), ('k_DXT2_3', 'kCompressed', 8),
    ('k_DXT4_5', 'kCompressed', 8), ('k_16_16_16_16_EDRAM', 'kUncompressed', 64), ('k_24_8', 'kUncompressed', 32),
    ('k_24_8_FLOAT', 'kUncompressed', 32), ('k_16', 'kResolvable', 16), ('k_16_16', 'kResolvable', 32),
    ('k_16_16_16_16', 'kResolvable', 64), ('k_16_EXPAND', 'kUncompressed', 16), ('k_16_16_EXPAND', 'kUncompressed', 32),
    ('k_16_16_16_16_EXPAND', 'kUncompressed', 64), ('k_16_FLOAT', 'kResolvable', 16), ('k_16_16_FLOAT', 'kResolvable', 32),
    ('k_16_16_16_16_FLOAT', 'kResolvable', 64), ('k_32', 'kUncompressed', 32), ('k_32_32', 'kUncompressed', 64),
    ('k_32_32_32_32', 'kUncompressed', 128), ('k_32_FLOAT', 'kResolvable', 32), ('k_32_32_FLOAT', 'kResolvable', 64),
    ('k_32_32_32_32_FLOAT', 'kResolvable', 128), ('k_32_AS_8', 'kCompressed', 8), ('k_32_AS_8_8', 'kCompressed', 16),
    ('k_16_MPEG', 'kUncompressed', 16), ('k_16_16_MPEG', 'kUncompressed', 32), ('k_8_INTERLACED', 'kUncompressed', 8),
    ('k_32_AS_8_INTERLACED', 'kCompressed', 8), ('k_32_AS_8_8_INTERLACED', 'kCompressed', 16),
    ('k_16_INTERLACED', 'kUncompressed', 16), ('k_16_MPEG_INTERLACED', 'kUncompressed', 16),
    ('k_16_16_MPEG_INTERLACED', 'kUncompressed', 32), ('k_DXN', 'kCompressed', 8),
    ('k_8_8_8_8_AS_16_16_16_16', 'kResolvable', 32), ('k_DXT1_AS_16_16_16_16', 'kCompressed', 4),
    ('k_DXT2_3_AS_16_16_16_16', 'kCompressed', 8), ('k_DXT4_5_AS_16_16_16_16', 'kCompressed', 8),
    ('k_2_10_10_10_AS_16_16_16_16', 'kResolvable', 32), ('k_10_11_11_AS_16_16_16_16', 'kResolvable', 32),
    ('k_11_11_10_AS_16_16_16_16', 'kResolvable', 32), ('k_32_32_32_FLOAT', 'kUncompressed', 96),
    ('k_DXT3A', 'kCompressed', 4), ('k_DXT5A', 'kCompressed', 4), ('k_CTX1', 'kCompressed', 4),
    ('k_DXT3A_AS_1_1_1_1', 'kCompressed', 4), ('k_8_8_8_8_GAMMA_EDRAM', 'kUncompressed', 32),
    ('k_2_10_10_10_FLOAT_EDRAM', 'kUncompressed', 32),
]
assert len(FORMAT_TABLE) == 64
DEPTH_TEXTURE_FORMAT = {0: 22, 1: 23}     # texture_info.h L170-L181: kD24S8 -> k_24_8, kD24FS8 -> k_24_8_FLOAT


def _perma(path, a, b=None):
    return '%s/blob/%s/%s#L%d%s' % (XENIA_REPOSITORY, XENIA_PIN, path, a, ('-L%d' % b) if b else '')


DERIVATION_MANIFEST = dict(
    derivation_version=DERIVATION_VERSION,
    xenia_repository=XENIA_REPOSITORY, xenia_commit=XENIA_PIN,
    input_schema='xtr_draws-summary/3',
    output='guest destination rectangle (pixels, post-clamp, 8-aligned), adjusted destination base, '
           'tiled byte extent [extent_start, extent_end_exclusive)',
    guest_range_host_parameter_dependencies=[],
    host_parameters_not_in_trace=list(RESOLVE_INPUTS_NOT_IN_TRACE),
    host_parameter_note='draw_util.cc L1076-L1079 only stores the draw resolution scales; L1228-L1233 '
                        '(fill_half_pixel_offset) and L1272-L1281 (fixed-16 exponent) affect fields this '
                        'derivation does not produce; the rectangle, adjusted base and extent depend on none '
                        'of the six host parameters',
    integer_model='C++ widths emulated: uint32 wrap for addresses/extents, int32 for Tiled2D and the '
                  'rectangle, int64 for Tiled3D; the independent oracle is compiled with -fwrapv',
    stages=collections.OrderedDict([
        ('copy_command_validation', [_perma('src/xenia/gpu/draw_util.cc', 939, 947)]),
        ('vertex_fetch_validation', [_perma('src/xenia/gpu/draw_util.cc', 953, 958)]),
        ('vertex_read', [_perma('src/xenia/gpu/draw_util.cc', 959, 962),
                         _perma('src/xenia/gpu/trace_writer.cc', 142, 148)]),
        ('vertex_endian_swap', [_perma('src/xenia/gpu/xenos.h', 1092, 1124)]),
        ('pixel_center_half_pixel', [_perma('src/xenia/gpu/draw_util.cc', 963, 968),
                                     _perma('src/xenia/gpu/xenos.h', 900, 906),
                                     _perma('src/xenia/gpu/registers.h', 493, 503)]),
        ('float_to_fixed_16_8', [_perma('src/xenia/ui/graphics_util.cc', 17, 60)]),
        ('rectangle_min_max_top_left', [_perma('src/xenia/gpu/draw_util.cc', 969, 991)]),
        ('window_offset', [_perma('src/xenia/gpu/draw_util.cc', 993, 1001),
                           _perma('src/xenia/gpu/registers.h', 610, 620),
                           _perma('src/xenia/gpu/registers.h', 456, 490)]),
        ('scissor', [_perma('src/xenia/gpu/draw_util.cc', 1003, 1016),
                     _perma('src/xenia/gpu/draw_util.cc', 551, 724),
                     _perma('src/xenia/gpu/registers.h', 586, 645)]),
        ('alignment_8', [_perma('src/xenia/gpu/draw_util.cc', 1024, 1027),
                         _perma('src/xenia/gpu/xenos.h', 1022, 1023),
                         _perma('src/xenia/base/math.h', 63, 65)]),
        ('msaa_validation', [_perma('src/xenia/gpu/draw_util.cc', 1029, 1038)]),
        ('surface_pitch_clamp', [_perma('src/xenia/gpu/draw_util.cc', 1040, 1051),
                                 _perma('src/xenia/gpu/registers.h', 670, 680)]),
        ('height_clamp', [_perma('src/xenia/gpu/draw_util.cc', 1053, 1059),
                          _perma('src/xenia/gpu/xenos.h', 1027, 1029)]),
        ('empty_region', [_perma('src/xenia/gpu/draw_util.cc', 1065, 1069)]),
        ('div_8', [_perma('src/xenia/gpu/draw_util.cc', 1071, 1074)]),
        ('destination_format', [_perma('src/xenia/gpu/draw_util.cc', 1082, 1083),
                                _perma('src/xenia/gpu/draw_util.cc', 1098, 1108),
                                _perma('src/xenia/gpu/texture_info.h', 170, 181)]),
        ('format_table', [_perma('src/xenia/gpu/texture_info_formats.inl', 1, 64),
                          _perma('src/xenia/gpu/texture_info_formats.cc', 19, 26),
                          _perma('src/xenia/gpu/texture_info.h', 183, 205),
                          _perma('src/xenia/base/math.h', 317, 323)]),
        ('pitch_height_alignment_32', [_perma('src/xenia/gpu/draw_util.cc', 1131, 1145),
                                       _perma('src/xenia/gpu/texture_address.h', 65, 67),
                                       _perma('src/xenia/gpu/registers.h', 887, 897)]),
        ('base_granularity', [_perma('src/xenia/gpu/draw_util.cc', 1146, 1164),
                              _perma('src/xenia/gpu/xenos.h', 1211, 1218)]),
        ('tiled_2d', [_perma('src/xenia/gpu/draw_util.cc', 1183, 1194),
                      _perma('src/xenia/gpu/texture_address.h', 172, 208),
                      _perma('src/xenia/gpu/texture_util.h', 294, 301),
                      _perma('src/xenia/gpu/texture_util.cc', 450, 477)]),
        ('tiled_3d', [_perma('src/xenia/gpu/draw_util.cc', 1165, 1182),
                      _perma('src/xenia/gpu/texture_address.h', 213, 238),
                      _perma('src/xenia/gpu/texture_util.h', 302, 312),
                      _perma('src/xenia/gpu/texture_util.cc', 479, 510),
                      _perma('src/xenia/gpu/xenos.h', 1202, 1203)]),
        ('non_resolvable_format', [_perma('src/xenia/gpu/draw_util.cc', 1195, 1200)]),
        ('extent_invariants', [_perma('src/xenia/gpu/draw_util.cc', 1201, 1206)]),
    ]),
)


def u32(x):
    return x & 0xFFFFFFFF


def i32(x):
    x &= 0xFFFFFFFF
    return x - (1 << 32) if x & 0x80000000 else x


def u64(x):
    return x & 0xFFFFFFFFFFFFFFFF


def i64(x):
    x &= 0xFFFFFFFFFFFFFFFF
    return x - (1 << 64) if x & (1 << 63) else x


def sext(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def xe_align(v, a):
    """base/math.h L63-L65: (value + alignment - 1) & ~(alignment - 1)."""
    return (v + a - 1) & ~(a - 1)


def log2_floor(v):
    """base/math.h L317-L323 (for v > 0)."""
    return v.bit_length() - 1


def gpu_swap32(v, endian):
    """xenos.h L1092-L1109 (GpuSwapInline; the C++ `default:` is kNone)."""
    v &= 0xFFFFFFFF
    if endian == 1:                                        # k8in16
        return ((v << 8) & 0xFF00FF00) | ((v >> 8) & 0x00FF00FF)
    if endian == 2:                                        # k8in32
        return int.from_bytes(v.to_bytes(4, 'little'), 'big')
    if endian == 3:                                        # k16in32
        return ((v >> 16) & 0xFFFF) | u32(v << 16)
    return v                                               # kNone


def f32_bits(f):
    try:
        return struct.unpack('<I', struct.pack('<f', f))[0]
    except OverflowError:                                  # beyond float range -> +-inf
        return 0x7F800000 if f > 0 else 0xFF800000


def f32_from_bits(b):
    return struct.unpack('<f', struct.pack('<I', b & 0xFFFFFFFF))[0]


def f32_add(a, b):
    """float + float in IEEE single precision: the exact double sum of two
    floats rounded once to float (exact for every non-saturating magnitude)."""
    return f32_from_bits(f32_bits(a + b))


def float_to_fixed16p8(f):
    """ui/graphics_util.cc L17-L60, bit-exact (NaN and tiny -> 0, saturation,
    hidden bit, two's complement, nearest-even, arithmetic shift)."""
    if not (abs(f) >= 1.0 / 512.0):
        return 0
    if f >= 32768.0 - 1.0 / 256.0:
        return (1 << 23) - 1
    if f <= -32768.0:
        return -32768 * 256
    bits = f32_bits(f)
    r = (bits & ((1 << 23) - 1)) | (1 << 23)
    if bits >> 31:
        r = u32(-r)
    exponent = ((bits >> 23) & 255) - 127
    extra = u32(15 - exponent)
    if extra:
        r = u32(r + ((1 << (extra - 1)) - 1 + ((r >> extra) & 1)))
        r = u32(i32(r) >> extra)
    return i32(r)


def get_scissor_no_pitch_clamp(wsc_tl, wsc_br, wnd_off, ssc_tl, ssc_br):
    """draw_util.cc L636-L724, GetScissorTmpl<false> (the scalar branch);
    registers.h L586-L645 bitfields (14-bit unsigned window scissor, 15-bit
    signed window offset and screen scissor, window_offset_disable at bit 31)."""
    tl_x, tl_y = wsc_tl & 0x3FFF, (wsc_tl >> 16) & 0x3FFF
    br_x, br_y = wsc_br & 0x3FFF, (wsc_br >> 16) & 0x3FFF
    wx, wy = sext(wnd_off, 15), sext(wnd_off >> 16, 15)
    mask = 0 if (wsc_tl >> 31) & 1 else -1                 # ~(int32(value) >> 31)
    tl_x += wx & mask
    tl_y += wy & mask
    br_x += wx & mask
    br_y += wy & mask
    tl_x = max(tl_x, sext(ssc_tl, 15))
    tl_y = max(tl_y, sext(ssc_tl >> 16, 15))
    br_x = min(br_x, sext(ssc_br, 15))
    br_y = min(br_y, sext(ssc_br >> 16, 15))
    tl_x = max(tl_x, 0)
    tl_y = max(tl_y, 0)
    br_x = max(br_x, tl_x)
    br_y = max(br_y, tl_y)
    return dict(offset=[tl_x, tl_y], extent=[br_x - tl_x, br_y - tl_y])


def tiled_combine(oib, bank, pipe, y_lsb, wrap):
    """texture_address.h L172-L184; `wrap` is i32 (2D) or i64 (3D)."""
    return wrap(i32((y_lsb << 4) | (pipe << 6) | (bank << 11)) | (oib & 0xF) | (((oib >> 4) & 1) << 5)
                | (((oib >> 5) & 7) << 8) | wrap((oib >> 8) << 12))


def tiled_2d(x, y, pitch_aligned, bpp_log2):
    """texture_address.h L190-L208 (int32 arithmetic, -fwrapv semantics)."""
    outer_blocks = i32(((y >> 5) * i32(pitch_aligned >> 5) + (x >> 5)) << 6)
    inner_blocks = (((y >> 1) & 7) << 3) | (x & 7)
    oib = i32((outer_blocks | inner_blocks) << bpp_log2)
    bank = (y >> 4) & 1
    pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1)
    return tiled_combine(oib, bank, pipe, y & 1, i32)


def tiled_3d(x, y, z, pitch_aligned, height_aligned, bpp_log2):
    """texture_address.h L213-L238 (int32 outer/inner blocks, int64 result)."""
    outer_blocks = i32(u32(((u32((z >> 2) * (height_aligned >> 4) + (y >> 4)) * i32(pitch_aligned >> 5))
                            + (x >> 5)) << 7))
    inner_blocks = ((z & 3) << 5) | (((y >> 1) & 3) << 3) | (x & 7)
    oib = i64((outer_blocks | inner_blocks) << bpp_log2)
    bank = ((y >> 3) ^ (z >> 2)) & 1
    pipe = ((x >> 3) & 3) ^ (bank << 1)
    return tiled_combine(oib, bank, pipe, y & 1, i64)


def tiled_lower_bound_2d(left, top, pitch_aligned, bpp_log2):
    """texture_util.h L294-L301."""
    return u32(tiled_2d(i32(left & ~(TEXTURE_TILE_WH - 1)), i32(top & ~(TEXTURE_TILE_WH - 1)),
                        pitch_aligned, bpp_log2))


def tiled_upper_bound_2d(right, bottom, pitch_aligned, bpp_log2):
    """texture_util.cc L450-L477."""
    if not right or not bottom:
        return 0
    ub = u32(tiled_2d(i32((right - 1) & ~(TEXTURE_TILE_WH - 1)), i32((bottom - 1) & ~(TEXTURE_TILE_WH - 1)),
                      pitch_aligned, bpp_log2))
    if bpp_log2 == 0:
        ub = u32(ub + 0xA00)
    elif bpp_log2 == 1:
        ub = u32(ub + 0xC00)
    else:
        ub = u32(ub + (0x400 << bpp_log2))
    return ub


def tiled_lower_bound_3d(left, top, front, pitch_aligned, height_aligned, bpp_log2):
    """texture_util.h L302-L312 (note the pin masks `front` with ~kTextureTileDepth, i.e. ~4)."""
    return u64(tiled_3d(i32(left & ~(TEXTURE_TILE_WH - 1)), i32(top & ~(TEXTURE_TILE_WH - 1)),
                        i32(front & ~TEXTURE_TILE_DEPTH), pitch_aligned, height_aligned, bpp_log2))


def tiled_upper_bound_3d(right, bottom, back, pitch_aligned, height_aligned, bpp_log2):
    """texture_util.cc L479-L510."""
    if not right or not bottom or not back:
        return 0
    ub = u64(tiled_3d(i32((right - 1) & ~(TEXTURE_TILE_WH - 1)), i32((bottom - 1) & ~(TEXTURE_TILE_WH - 1)),
                      i32((back - 1) & ~(TEXTURE_TILE_DEPTH - 1)), pitch_aligned, height_aligned, bpp_log2))
    if bpp_log2 == 0:
        ub = u64(ub + u32(((pitch_aligned >> 6) << 12) + 0xC00 + ((pitch_aligned & (1 << 5)) << (10 - 5))))
    else:
        ub = u64(ub + u32(u32((pitch_aligned << (12 - 5 - 1)) + (0x1000 >> 1)) << bpp_log2))
    return ub


def derive_guest_destination_range(copy_control, payload_bytes, endian, pa_su_vtx_cntl, pa_su_sc_mode_cntl,
                                   wnd_off, wsc_tl, wsc_br, ssc_tl, ssc_br, surface_info,
                                   copy_dest_base, copy_dest_pitch, copy_dest_info, depth_info):
    """The pinned GetResolveInfo range path (draw_util.cc L926-L1206) for one
    resolve whose vertex fetch is already known valid and whose 24 payload
    bytes are `payload_bytes` as guest memory held them. Returns the derived
    dict (status, intermediates, final range). No host parameter enters."""
    d = collections.OrderedDict(derivation_version=DERIVATION_VERSION, status=None, reason=None,
                                join_usable=False)
    copy_command = (copy_control >> 20) & 3
    d['copy_command'] = copy_command
    if copy_command not in (0, 1):                               # L939-L947: kRaw / kConvert only
        d.update(status='rejected_copy_command',
                 reason='Unsupported resolve copy command %d (draw_util.cc L939-L947 returns false)' % copy_command)
        return d
    # L959-L975: little-endian host read, GpuSwap per fetch endian, + half pixel, fixed 16.8
    swapped = [gpu_swap32(int.from_bytes(payload_bytes[4 * i:4 * i + 4], 'little'), endian) for i in range(6)]
    half = 0.5 if (pa_su_vtx_cntl & 1) == 0 else 0.0            # PixelCenter::kD3DZero == 0
    fixed = [float_to_fixed16p8(f32_add(f32_from_bits(b), half)) for b in swapped]
    d['vertices_swapped_bits'] = ['%08X' % b for b in swapped]
    d['half_pixel_offset'] = half
    d['vertices_fixed_16_8'] = fixed
    x0 = min(fixed[0], fixed[2], fixed[4])
    y0 = min(fixed[1], fixed[3], fixed[5])
    x1 = max(fixed[0], fixed[2], fixed[4])
    y1 = max(fixed[1], fixed[3], fixed[5])
    x0, y0, x1, y1 = (x0 + 127) >> 8, (y0 + 127) >> 8, (x1 + 127) >> 8, (y1 + 127) >> 8
    d['rect_pixels'] = [x0, y0, x1, y1]
    wx, wy = sext(wnd_off, 15), sext(wnd_off >> 16, 15)
    vtx_window_offset_enable = (pa_su_sc_mode_cntl >> 16) & 1
    d['window_offset'] = dict(enable=vtx_window_offset_enable, x=wx, y=wy)
    if vtx_window_offset_enable:                                 # L993-L1001
        x0, y0, x1, y1 = x0 + wx, y0 + wy, x1 + wx, y1 + wy
    d['rect_after_window_offset'] = [x0, y0, x1, y1]
    sc = get_scissor_no_pitch_clamp(wsc_tl, wsc_br, wnd_off, ssc_tl, ssc_br)   # L1003-L1016
    d['scissor'] = sc
    right, bottom = sc['offset'][0] + sc['extent'][0], sc['offset'][1] + sc['extent'][1]
    x0 = min(max(x0, sc['offset'][0]), right)
    y0 = min(max(y0, sc['offset'][1]), bottom)
    x1 = min(max(x1, sc['offset'][0]), right)
    y1 = min(max(y1, sc['offset'][1]), bottom)
    d['rect_after_scissor'] = [x0, y0, x1, y1]
    x0 &= ~7                                                     # L1024-L1027
    y0 &= ~7
    x1 = xe_align(x1, 8)
    y1 = xe_align(y1, 8)
    d['rect_aligned_8'] = [x0, y0, x1, y1]
    msaa = (surface_info >> 16) & 3
    d['msaa_samples'] = msaa
    if msaa > 2:                                                 # L1029-L1038: > k4X
        d.update(status='rejected_msaa', reason='%dx MSAA requested in a resolve, Xenos supports up to 4x '
                 '(draw_util.cc L1029-L1038 returns false)' % (1 << msaa))
        return d
    surface_pitch_aligned = (surface_info & 0x3FFF) & ~7          # L1040-L1043
    d['surface_pitch_aligned'] = surface_pitch_aligned
    if x1 > surface_pitch_aligned:                               # L1044-L1049
        x0 = min(x0, surface_pitch_aligned)
        x1 = min(x1, surface_pitch_aligned)
    d['rect_after_pitch_clamp'] = [x0, y0, x1, y1]
    if y1 - y0 > MAX_RESOLVE_SIZE:                               # L1053-L1059
        y1 = y0 + MAX_RESOLVE_SIZE
    d['rect_final'] = [x0, y0, x1, y1]
    if x0 >= x1 or y0 >= y1:                                     # L1065-L1069
        d.update(status='empty_rectangle', width_div_8=0, height_div_8=0,
                 reason='empty or inverted rectangle after clipping: GetResolveInfo returns true with '
                        'width_div_8 == 0 and the caller skips the resolve (draw_util.cc L1065-L1069)')
        return d
    d['width_div_8'] = u32(x1 - x0) >> 3                         # L1071-L1074
    d['height_div_8'] = u32(y1 - y0) >> 3
    # destination (L1082-L1083, L1098-L1108, L1131-L1206)
    is_depth = ((copy_control & 7) >= MAX_COLOR_RENDER_TARGETS)
    if is_depth:
        dest_format = DEPTH_TEXTURE_FORMAT[(depth_info >> 16) & 1]
    else:
        dest_format = (copy_dest_info >> 7) & 0x3F
    fname, ftype, bpp = FORMAT_TABLE[dest_format]
    pitch_aligned = xe_align(copy_dest_pitch & 0x3FFF, STORAGE_ALIGN_BLOCKS)
    height_aligned = xe_align((copy_dest_pitch >> 16) & 0x3FFF, STORAGE_ALIGN_BLOCKS)
    d['destination'] = dict(is_depth=is_depth, format=dest_format, format_name=fname, format_type=ftype,
                            bits_per_pixel=bpp, pitch_aligned=pitch_aligned, height_aligned=height_aligned,
                            pitch_aligned_div_32=pitch_aligned >> 5, height_aligned_div_32=height_aligned >> 5)
    base = u32(copy_dest_base)
    base_adjusted = base
    if is_depth or ftype == 'kResolvable':
        bpp_log2 = log2_floor(bpp >> 3)
        is_3d = (copy_dest_info >> 3) & 1
        slice_ = (copy_dest_info >> 4) & 7
        xg = 7 - min(2, bpp_log2 + is_3d)                        # xenos.h L1211-L1214
        yg = 5 if is_3d else 7 - min(2, bpp_log2)                # xenos.h L1215-L1218
        xmask, ymask = (1 << xg) - 1, (1 << yg) - 1
        offset_x_div_8 = (u32(x0) & xmask) >> 3
        offset_y_div_8 = (u32(y0) & ymask) >> 3
        bx = u32(x0) & ~xmask & 0xFFFFFFFF
        by = u32(y0) & ~ymask & 0xFFFFFFFF
        if is_3d:
            base_adjusted = u32(base_adjusted + u32(tiled_3d(i32(bx), i32(by), 0, pitch_aligned, height_aligned, bpp_log2)))
            start = u32(base + u32(tiled_lower_bound_3d(u32(x0), u32(y0), slice_, pitch_aligned, height_aligned, bpp_log2)))
            end = u32(base + u32(tiled_upper_bound_3d(u32(x1), u32(y1), slice_ + 1, pitch_aligned, height_aligned, bpp_log2)))
        else:
            base_adjusted = u32(base_adjusted + u32(tiled_2d(i32(bx), i32(by), pitch_aligned, bpp_log2)))
            start = u32(base + tiled_lower_bound_2d(u32(x0), u32(y0), pitch_aligned, bpp_log2))
            end = u32(base + tiled_upper_bound_2d(u32(x1), u32(y1), pitch_aligned, bpp_log2))
        d['tiling'] = dict(mode='3D' if is_3d else '2D', bytes_per_block_log2=bpp_log2, slice=slice_,
                           x_base_granularity_log2=xg, y_base_granularity_log2=yg,
                           dest_base_x=bx, dest_base_y=by,
                           offset_x_div_8=offset_x_div_8, offset_y_div_8=offset_y_div_8)
        status, reason = 'complete', None
    else:                                                        # L1195-L1200
        start = end = base_adjusted
        d['tiling'] = None
        status = 'non_resolvable_format'
        reason = ('destination format %s(%d) is %s, not a resolvable ColorFormat: pinned Xenia logs an error '
                  'and produces a ZERO extent (draw_util.cc L1195-L1200); unusable for any join' % (fname, dest_format, ftype))
    invariants = start >= base_adjusted and end >= base_adjusted and end >= start   # L1201-L1206
    length = u32(end - start)
    d['range'] = dict(raw_base=base, adjusted_base=base_adjusted, extent_start=start,
                      extent_length=length, extent_end_exclusive=end, invariants_hold=invariants)
    declared_pitch, declared_height = copy_dest_pitch & 0x3FFF, (copy_dest_pitch >> 16) & 0x3FFF
    d['raw_vs_derived'] = dict(
        adjusted_base_equals_raw=base_adjusted == base,
        extent_start_equals_raw_base=start == base,
        adjusted_minus_raw=u32(base_adjusted - base),
        extent_start_minus_raw=u32(start - base),
        declared_linear_span_bytes=declared_pitch * declared_height * bpp // 8,
        extent_equals_declared_linear_span=length == declared_pitch * declared_height * bpp // 8)
    d.update(status=status, reason=reason, join_usable=(status == 'complete' and length > 0 and invariants))
    return d


def derive_for_row(r):
    """Apply the derivation to one resolve record; /3 fetch statuses propagate
    without arithmetic (fail closed)."""
    gi = r['get_resolve_info_inputs']
    copy_command = (r['copy_control'] >> 20) & 3
    if copy_command not in (0, 1):
        # L939-L947 precedes the fetch check in pinned Xenia
        return collections.OrderedDict(derivation_version=DERIVATION_VERSION, status='rejected_copy_command',
                                       reason='Unsupported resolve copy command %d (draw_util.cc L939-L947 returns false)'
                                              % copy_command, join_usable=False, copy_command=copy_command)
    if gi['status'] != 'complete':
        return collections.OrderedDict(derivation_version=DERIVATION_VERSION,
                                       status='inputs_' + gi['status'],
                                       reason='no arithmetic: /3 inputs are %s (%s)'
                                              % (gi['status'], gi['rejection'] or 'vertex payload not fully supplied in this draw packet'),
                                       join_usable=False, copy_command=copy_command)
    regs = gi['registers']
    payload = bytes.fromhex(gi['vertex_payload']['bytes_hex'])
    return derive_guest_destination_range(
        r['copy_control'], payload, gi['vertex_fetch_0']['endian'], regs['PA_SU_VTX_CNTL'],
        regs['PA_SU_SC_MODE_CNTL'], regs['PA_SC_WINDOW_OFFSET'], regs['PA_SC_WINDOW_SCISSOR_TL'],
        regs['PA_SC_WINDOW_SCISSOR_BR'], regs['PA_SC_SCREEN_SCISSOR_TL'], regs['PA_SC_SCREEN_SCISSOR_BR'],
        r['surface_info'], r['dest_base'], r['dest_pitch'], r['dest_info'], r['depth_info'])


RANGE_STATUSES = ('complete', 'rejected_copy_command', 'inputs_rejected', 'inputs_incomplete',
                  'rejected_msaa', 'empty_rectangle', 'non_resolvable_format')


def range_label(g):
    if g['status'] != 'complete' and 'range' not in g:
        return g['status']
    rg, rect = g['range'], g['rect_final']
    return ('%s rect=(%d,%d)-(%d,%d) %s(%d) bpp=%d %s base=%08X->%08X extent=[%08X,%08X) len=%d%s'
            % (g['status'], rect[0], rect[1], rect[2], rect[3], g['destination']['format_name'],
               g['destination']['format'], g['destination']['bits_per_pixel'],
               g['tiling']['mode'] if g['tiling'] else '-', rg['raw_base'], rg['adjusted_base'],
               rg['extent_start'], rg['extent_end_exclusive'], rg['extent_length'],
               '' if rg['invariants_hold'] else ' INVARIANTS-VIOLATED'))


class PacketLocalReads:
    """The decoded MemoryRead payloads recorded INSIDE one packet's
    PacketStart/PacketEnd interval, in record order. This is the only
    evidence that a byte was supplied for THIS packet: the trace-wide image,
    the packet body write and MemoryWrite records never enter it. Reset at
    every PacketStart, discarded when an INDIRECT_BUFFER dispatch packet is
    consumed, handed only to that PacketEnd's execution."""

    def __init__(self):
        self.segments = []      # (base, bytes) in record order
        self.records = 0

    def add(self, base, data):
        self.segments.append((base & PHYS_MASK, bytes(data)))
        self.records += 1

    def extract(self, addr, n):
        """(bytes, missing): the n bytes of [addr, addr+n) assembled from the
        segments (a later record wins on overlap; split reads are joined) and
        the exact count of target bytes NO segment supplied. The caller must
        treat bytes as unusable unless missing == 0."""
        addr &= PHYS_MASK
        out = bytearray(n)
        covered = bytearray(n)
        for base, data in self.segments:
            lo, hi = max(base, addr), min(base + len(data), addr + n)
            if lo >= hi:
                continue
            out[lo - addr:hi - addr] = data[lo - base:hi - base]
            covered[lo - addr:hi - addr] = b'\x01' * (hi - lo)
        missing = n - sum(covered)
        return bytes(out), missing


def be32s(b):
    return struct.unpack('>%dI' % (len(b) // 4), b[:len(b) & ~3])


def f32(u):
    return struct.unpack('<f', struct.pack('<I', u & 0xFFFFFFFF))[0]


def hash_ucode(b, offset=0):
    """XXH3-64 of the raw big-endian ucode bytes (ShaderCache::hashUcode).
    NEVER 0 for want of the library: that is a named dependency refusal."""
    if xxhash is None:
        raise TraceError(EXIT_DEPENDENCY, offset,
                         'shader hashing needs the xxhash package (pip install '
                         'xxhash); refusing rather than reporting hash 0')
    return xxhash.xxh3_64_intdigest(b)


# ── shader ucode: the Xenia control-flow walk (XT1) ──────────────────────────
#
# CF instructions are packed two per three dwords at the top of the program;
# each exec-class CF names `count` 3-dword instruction slots at `address` and a
# 12-bit `sequence` whose bit (2k) says slot k is a FETCH (else ALU). Only
# fetch slots whose opcode is kVertexFetch (0) are vertex fetches. Scanning
# every triple for "opcode==0 && must_be_one" (the r1 script) ALSO matches CF
# words and ALU words whose bits happen to line up - that is hypothesis XT1.

CF_EXEC_OPCODES = frozenset({1, 2, 3, 4, 5, 6, 13, 14})   # kExec..kCondExecPredCleanEnd


def _unpack_cf_pair(d0, d1, d2):
    """ucode.h UnpackControlFlowInstructions: (a_dw0, a_dw1), (b_dw0, b_dw1)."""
    a = (d0, d1 & 0xFFFF)
    b = (((d1 >> 16) | (d2 << 16)) & 0xFFFFFFFF, d2 >> 16)
    return a, b


def _cf_fields(dw0, dw1):
    opcode = (dw1 >> 12) & 0xF
    return dict(opcode=opcode, address=dw0 & 0xFFF, count=(dw0 >> 12) & 7,
                sequence=(dw0 >> 16) & 0xFFF)


def _dst_used(dst_swiz):
    """shader.h GetUsedResultComponents, reduced: any component swizzle in
    x..w (0..3) writes something; k0/k1/kKeep do not."""
    for i in range(4):
        if ((dst_swiz >> (3 * i)) & 7) <= 3:
            return True
    return False


def decode_vfetch(d0, d1, d2, slot):
    """ucode.h VertexFetchInstruction (L690-L755), dwords already host-order."""
    exp = (d1 >> 24) & 0x3F
    if exp >= 32:
        exp -= 64
    offset = (d2 >> 8) & 0x7FFFFF
    if offset >= (1 << 22):
        offset -= (1 << 23)
    return dict(
        slot=slot, opcode=d0 & 0x1F, src=(d0 >> 5) & 0x3F, dst=(d0 >> 12) & 0x3F,
        must_be_one=(d0 >> 19) & 1,
        fetch_const=((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 3),
        prefetch=(d0 >> 27) & 7, dst_swiz=d1 & 0xFFF,
        signed=(d1 >> 12) & 1, num_format=(d1 >> 13) & 1, signed_rf=(d1 >> 14) & 1,
        index_rounded=(d1 >> 15) & 1, fmt=(d1 >> 16) & 0x3F, exp_adjust=exp,
        mini=(d1 >> 30) & 1, predicated=(d1 >> 31) & 1,
        stride=d2 & 0xFF, offset=offset, pred_condition=(d2 >> 31) & 1,
    )


def cf_exec_walk(ucode):
    """The pinned control-flow enumeration shared by the vertex-fetch and the
    texture-fetch analyses. Shader::AnalyzeUcode (shader_translator.cc
    L53-L64): the CF program ends at the lowest exec target and the bound
    SHRINKS WHILE SCANNING; GatherExecInformation (L349-L372): every exec-class
    CF names `count` instruction slots from `address`, sequence bit 0 = fetch
    (else ALU), bit 1 = serialize. Returns the slots in program order as
    (cf_index, cf_opcode, slot, is_fetch, serialize). Fails closed: a slot
    beyond the ucode stops the walk with malformed=True (the pin does not check
    and would read past the buffer)."""
    dw = be32s(ucode)
    n = len(dw)
    pairs = n // 3
    out = dict(dwords=n, pairs=pairs, bound=0, exec_count=0, saw_exec=False, malformed=False,
               malformed_slot=None, slots=[])
    if pairs == 0:
        return out
    bound = pairs
    i = 0
    while i < bound:
        for dw0, dw1 in _unpack_cf_pair(dw[3 * i], dw[3 * i + 1], dw[3 * i + 2]):
            cf = _cf_fields(dw0, dw1)
            if cf['opcode'] in CF_EXEC_OPCODES:
                bound = min(bound, cf['address'])
        i += 1
    out['bound'] = bound
    for i in range(bound):
        for j, (dw0, dw1) in enumerate(_unpack_cf_pair(dw[3 * i], dw[3 * i + 1], dw[3 * i + 2])):
            cf = _cf_fields(dw0, dw1)
            if cf['opcode'] not in CF_EXEC_OPCODES:
                continue
            out['saw_exec'] = True
            out['exec_count'] += 1
            seq = cf['sequence']
            for k in range(cf['count']):
                slot = cf['address'] + k
                is_fetch, serialize = bool(seq & 1), bool(seq & 2)
                seq >>= 2
                if slot * 3 + 3 > n:
                    out['malformed'] = True
                    out['malformed_slot'] = slot
                    return out
                out['slots'].append((2 * i + j, cf['opcode'], slot, is_fetch, serialize))
    return out


def parse_vs_vfetch(ucode):
    """The Xenia walk (AnalyzeUcode + GatherExecInformation +
    GatherVertexFetchInformation) on top of cf_exec_walk. Returns dict(ok,
    malformed, attrs, bindings, exec_slots, must_be_one_violations).

    attrs    : one entry per vertex-fetch instruction that writes something,
               with the binding's fetch constant/stride (a mini fetch inherits
               them from the previous full fetch) and its own format/offset.
    bindings : ordered list of dict(fetch_const, stride, attrs=[...]).
    exec_slots: every (slot, is_fetch) the CF program names - the ground truth
               the naive triple scan is compared against."""
    dw = be32s(ucode)
    walk = cf_exec_walk(ucode)
    out = dict(ok=False, malformed=walk['malformed'], attrs=[], bindings=[], exec_slots=[],
               must_be_one_violations=0, vfetch_slots=set())
    prev_full = None
    bindings = []
    for _cf_index, _cf_opcode, slot, is_fetch, _serialize in walk['slots']:
        out['exec_slots'].append((slot, is_fetch))
        if not is_fetch:
            continue
        d0, d1, d2 = dw[slot * 3], dw[slot * 3 + 1], dw[slot * 3 + 2]
        if (d0 & 0x1F) != 0:
            continue   # texture fetch
        op = decode_vfetch(d0, d1, d2, slot)
        out['vfetch_slots'].add(slot)
        if not op['must_be_one']:
            out['must_be_one_violations'] += 1
        if op['mini']:
            if prev_full is None:
                continue   # malformed mini: inherit nothing, invent nothing
            full = prev_full
        else:
            full = op
        if not _dst_used(op['dst_swiz']):
            if not op['mini']:
                prev_full = op
            continue
        attr = dict(op)
        attr['fetch_const'] = full['fetch_const']
        attr['stride'] = full['stride']
        for b in bindings:
            if b['fetch_const'] == full['fetch_const']:
                b['attrs'].append(attr)
                break
        else:
            bindings.append(dict(fetch_const=full['fetch_const'],
                                 stride=full['stride'], attrs=[attr]))
        out['attrs'].append(attr)
        if not op['mini']:
            prev_full = op
    out['bindings'] = bindings
    out['ok'] = walk['saw_exec'] and not walk['malformed']
    return out


def parse_vs_vfetch_naive(ucode):
    """The r1 rule, kept ONLY as the XT1 counterexample: every 3-dword triple
    whose low 5 bits are 0 and whose bit 19 is set is called a vertex fetch."""
    dw = be32s(ucode)
    hits = []
    for i in range(0, len(dw) - 2, 3):
        d0, d1, d2 = dw[i], dw[i + 1], dw[i + 2]
        if (d0 & 0x1F) != 0 or not ((d0 >> 19) & 1):
            continue
        hits.append(dict(slot=i // 3, fmt=(d1 >> 16) & 0x3F))
    return hits


# ── register decoders ────────────────────────────────────────────────────────

def decode_color_info(v):
    """registers.h RB_COLOR_INFO (L736-L752): 12-bit tile base, 4-bit format at
    bit 16, SIGNED 6-bit exponent bias at bit 20. RB_COLOR[1-3]_INFO share it."""
    exp = (v >> 20) & 0x3F
    if exp >= 32:
        exp -= 64
    fmt = (v >> 16) & 0xF
    return dict(base=v & 0xFFF, fmt=fmt, fmt_name=COLOR_RT_FORMAT_NAMES.get(fmt, 'fmt%d' % fmt),
                exp_bias=exp)


def decode_depth_info(v):
    """registers.h RB_DEPTH_INFO (L835-L846): 12-bit tile base, 1-bit format at bit 16."""
    fmt = (v >> 16) & 1
    return dict(base=v & 0xFFF, fmt=fmt, fmt_name=DEPTH_RT_FORMAT_NAMES[fmt])


def resolve_source(copy_src_select, rd):
    """draw_util.cc GetResolveInfo: `is_depth = copy_src_select >=
    kMaxColorRenderTargets` (L1082-L1083); otherwise the colour target is
    `regs.Get<RB_COLOR_INFO>(rt_register_indices[copy_src_select])`
    (L1256-L1259). `rd` reads a register by index."""
    if copy_src_select >= MAX_COLOR_RENDER_TARGETS:
        raw = rd(REG_RB_DEPTH_INFO)
        d = decode_depth_info(raw)
        return dict(kind='depth', index=copy_src_select, register=REG_RB_DEPTH_INFO,
                    register_name='RB_DEPTH_INFO', raw=raw, base=d['base'], fmt=d['fmt'],
                    fmt_name=d['fmt_name'], exp_bias=None)
    # masked so a misrouted select can never index past the four-entry table
    reg = RT_INFO_REGS[copy_src_select & 3]
    raw = rd(reg)
    c = decode_color_info(raw)
    return dict(kind='color', index=copy_src_select, register=reg, register_name=REG_NAMES[reg],
                raw=raw, base=c['base'], fmt=c['fmt'], fmt_name=c['fmt_name'],
                exp_bias=c['exp_bias'])


def resolve_source_label(sc):
    head = ('COLOR%d' % sc['index']) if sc['kind'] == 'color' else ('DEPTH(src%d)' % sc['index'])
    tail = (' expBias=%+d' % sc['exp_bias']) if sc['kind'] == 'color' else ''
    return '%s %s=%08X base=%03X fmt=%s(%d)%s' % (head, sc['register_name'], sc['raw'],
                                                   sc['base'], sc['fmt_name'], sc['fmt'], tail)


def decode_surface_info(v):
    return dict(pitch=v & 0x3FFF, msaa=(v >> 16) & 3)


def decode_copy_control(v):
    """registers.h RB_COPY_CONTROL (L853)."""
    return dict(copy_src_select=v & 7, copy_sample_select=(v >> 4) & 7,
                color_clear_enable=(v >> 8) & 1, depth_clear_enable=(v >> 9) & 1,
                copy_command=(v >> 20) & 3,
                copy_command_name=COPY_COMMAND_NAMES[(v >> 20) & 3])


def decode_copy_dest_info(v):
    """registers.h RB_COPY_DEST_INFO (L870)."""
    exp = (v >> 16) & 0x3F
    if exp >= 32:
        exp -= 64
    return dict(endian=v & 7, array=(v >> 3) & 1, slice=(v >> 4) & 7,
                fmt=(v >> 7) & 0x3F, number=(v >> 13) & 7, exp_bias=exp,
                swap=(v >> 24) & 1)


def decode_copy_dest_pitch(v):
    """registers.h RB_COPY_DEST_PITCH (L887)."""
    return dict(pitch=v & 0x3FFF, height=(v >> 16) & 0x3FFF)


# ── TOOL-3A: static texture bindings (schema /5) ─────────────────────────────

STATIC_BINDING_VERSION = 'xtr_draws-static-texture-bindings/1'
# ucode.h FetchOpcode L526-L671; ParseTextureFetchInstruction names L1058-L1085
FETCH_OPCODE_NAMES = {0: 'vfetch', 1: 'tfetch', 16: 'getBCF', 17: 'getCompTexLOD', 18: 'getGradients',
                      19: 'getWeights', 24: 'setTexLOD', 25: 'setGradientH', 26: 'setGradientV'}
FETCH_OPCODES_NO_BINDING = frozenset({24, 25, 26})       # shader_translator.cc L446-L452: "Doesn't use bindings"
FETCH_OPCODES_DIMENSION_SUFFIX = frozenset({1, 16, 17, 19})
FETCH_OP_DIMENSION_NAMES = {0: 'k1D', 1: 'k2D', 2: 'k3DOrStacked', 3: 'kCube'}   # xenos.h L166-L171
DATA_DIMENSION_NAMES = {0: 'k1D', 1: 'k2DOrStacked', 2: 'k3D', 3: 'kCube'}         # xenos.h L95-L100
TEXTURE_SIGN_NAMES = {0: 'kUnsigned', 1: 'kSigned', 2: 'kUnsignedBiased', 3: 'kGamma'}   # xenos.h L119-L128
CLAMP_MODE_NAMES = {0: 'kRepeat', 1: 'kMirroredRepeat', 2: 'kClampToEdge', 3: 'kMirrorClampToEdge',
                    4: 'kClampToHalfway', 5: 'kMirrorClampToHalfway', 6: 'kClampToBorder',
                    7: 'kMirrorClampToBorder'}                                            # xenos.h L102-L111
TEXTURE_FILTER_NAMES = {0: 'kPoint', 1: 'kLinear', 2: 'kBaseMap', 3: 'kUseFetchConst'}    # xenos.h L129-L136
ANISO_FILTER_NAMES = {0: 'kDisabled', 1: 'kMax_1_1', 2: 'kMax_2_1', 3: 'kMax_4_1', 4: 'kMax_8_1',
                      5: 'kMax_16_1', 7: 'kUseFetchConst'}                                # xenos.h L138-L146
ARBITRARY_FILTER_NAMES = {0: 'k2x4Sym', 1: 'k2x4Asym', 2: 'k4x2Sym', 3: 'k4x2Asym', 4: 'k4x4Sym',
                          5: 'k4x4Asym', 7: 'kUseFetchConst'}                             # xenos.h L814-L822
BORDER_COLOR_NAMES = {0: 'k_ABGR_Black', 1: 'k_ABGR_White', 2: 'k_ACBYCR_Black', 3: 'k_ACBCRY_Black'}
ENDIAN_NAMES = {0: 'kNone', 1: 'k8in16', 2: 'k8in32', 3: 'k16in32'}
# every bitfield of xenos.h xe_gpu_texture_fetch_t L1229-L1330, in declaration order (the
# oracle prints the same list)
TEXTURE_FETCH_FIELD_ORDER = (
    'type', 'sign_x', 'sign_y', 'sign_z', 'sign_w', 'clamp_x', 'clamp_y', 'clamp_z', 'pitch', 'tiled',
    'format', 'endianness', 'request_size', 'stacked', 'nearest_clamp_policy', 'base_address',
    'size_1d.width', 'size_2d.width', 'size_2d.height', 'size_2d.stack_depth',
    'size_3d.width', 'size_3d.height', 'size_3d.depth',
    'num_format', 'swizzle', 'exp_adjust', 'mag_filter', 'min_filter', 'mip_filter', 'aniso_filter',
    'arbitrary_filter', 'border_size',
    'vol_mag_filter', 'vol_min_filter', 'mip_min_level', 'mip_max_level', 'mag_aniso_walk', 'min_aniso_walk',
    'lod_bias', 'grad_exp_adjust_h', 'grad_exp_adjust_v',
    'border_color', 'force_bc_w_to_max', 'tri_clamp', 'aniso_bias', 'dimension', 'packed_mips', 'mip_address')


def decode_tfetch_instruction(d0, d1, d2):
    """ucode.h TextureFetchInstruction L835-L879 (host-order dwords): every
    field, signed ones sign-extended (lod_bias 7 bits, offsets 5 bits)."""
    opcode = d0 & 0x1F
    dim = (d2 >> 14) & 3
    name = FETCH_OPCODE_NAMES.get(opcode)
    if opcode in FETCH_OPCODES_DIMENSION_SUFFIX:
        name = name + ('1D', '2D', '3D', 'Cube')[dim]
    dst_swizzle = d1 & 0xFFF
    return collections.OrderedDict(
        opcode=opcode, opcode_name=name if name is not None else 'unknown', opcode_known=name is not None,
        src_reg=(d0 >> 5) & 0x3F, src_reg_am=(d0 >> 11) & 1, dst_reg=(d0 >> 12) & 0x3F, dst_reg_am=(d0 >> 18) & 1,
        fetch_valid_only=(d0 >> 19) & 1, fetch_constant=(d0 >> 20) & 0x1F, tx_coord_denorm=(d0 >> 25) & 1,
        src_swizzle=(d0 >> 26) & 0x3F,
        dst_swizzle=dst_swizzle, mag_filter=(d1 >> 12) & 3, min_filter=(d1 >> 14) & 3, mip_filter=(d1 >> 16) & 3,
        aniso_filter=(d1 >> 18) & 7, arbitrary_filter=(d1 >> 21) & 7, vol_mag_filter=(d1 >> 24) & 3,
        vol_min_filter=(d1 >> 26) & 3, use_computed_lod=(d1 >> 28) & 1, use_register_lod=(d1 >> 29) & 1,
        unk=(d1 >> 30) & 1, is_predicated=(d1 >> 31) & 1,
        use_register_gradients=d2 & 1, sample_location=(d2 >> 1) & 1, lod_bias=sext(d2 >> 2, 7),
        unused=(d2 >> 9) & 0x1F, dimension=dim, dimension_name=FETCH_OP_DIMENSION_NAMES[dim],
        offset_x=sext(d2 >> 16, 5), offset_y=sext(d2 >> 21, 5), offset_z=sext(d2 >> 26, 5),
        predicate_condition=(d2 >> 31) & 1,
        writes_result=_dst_used(dst_swizzle),
        is_binding=(name is not None and opcode != 0 and opcode not in FETCH_OPCODES_NO_BINDING))


def analyze_texture_fetches(ucode):
    """Shader::AnalyzeUcode / GatherExecInformation / GatherTextureFetchInformation
    (shader_translator.cc L42-L140, L349-L378, L435-L470) restricted to what
    the binding set needs: every fetch slot whose opcode is not kVertexFetch is
    a texture-fetch instruction (L365-L371); setTexLOD / setGradientH /
    setGradientV use no binding (L446-L452); every other KNOWN opcode binds its
    fetch constant (L456-L467); the slot mask is deduplicated only at the end.
    An unknown fetch opcode hits assert_unhandled_case in the pin
    (ParseTextureFetchInstruction L1090-L1092) and makes the analysis
    incomplete - nothing is invented for it."""
    dw = be32s(ucode)
    walk = cf_exec_walk(ucode)
    a = collections.OrderedDict(analysis_version=STATIC_BINDING_VERSION, status='incomplete', reason=None,
                                ucode_dwords=walk['dwords'], cf_pair_bound=walk['bound'],
                                exec_instructions=walk['exec_count'], instruction_slots=len(walk['slots']),
                                fetch_slots=0, vertex_fetch_slots=0, instructions=[], binding_slots=[],
                                binding_mask=0, binding_instructions=0, non_binding_instructions=0,
                                unknown_opcode_instructions=0)
    mask = 0
    unknown = []
    for cf_index, cf_opcode, slot, is_fetch, serialize in walk['slots']:
        if not is_fetch:
            continue
        a['fetch_slots'] += 1
        d0, d1, d2 = dw[slot * 3], dw[slot * 3 + 1], dw[slot * 3 + 2]
        if (d0 & 0x1F) == 0:                                   # kVertexFetch -> GatherVertexFetchInformation
            a['vertex_fetch_slots'] += 1
            continue
        ins = decode_tfetch_instruction(d0, d1, d2)
        row = collections.OrderedDict(cf_index=cf_index, cf_opcode=cf_opcode, slot=slot, serialize=serialize)
        row.update(ins)
        a['instructions'].append(row)
        if not ins['opcode_known']:
            a['unknown_opcode_instructions'] += 1
            unknown.append('%d@slot%d' % (ins['opcode'], slot))
        elif ins['is_binding']:
            a['binding_instructions'] += 1
            mask |= 1 << ins['fetch_constant']
        else:
            a['non_binding_instructions'] += 1
    a['binding_slots'] = [b for b in range(32) if (mask >> b) & 1]
    a['binding_mask'] = mask
    if walk['dwords'] == 0:
        a['reason'] = 'empty ucode'
    elif walk['malformed']:
        a['reason'] = ('exec names instruction slot %d beyond the ucode (%d dwords): pinned Xenia would read '
                       'past the buffer' % (walk['malformed_slot'], walk['dwords']))
    elif not walk['saw_exec']:
        a['reason'] = 'no exec-class control-flow instruction (bound %d of %d CF pairs)' % (walk['bound'], walk['pairs'])
    elif unknown:
        a['reason'] = ('unknown fetch opcode %s: ParseTextureFetchInstruction has no case (assert_unhandled_case), '
                       'the binding is not reproducible' % ', '.join(unknown))
    else:
        a['status'] = 'complete'
    return a


def compact_texture_instruction(ins):
    """The per-draw view of one statically referenced texture instruction
    (the complete decode lives in static_texture_shader_analyses)."""
    return collections.OrderedDict(
        cf_index=ins['cf_index'], cf_opcode=ins['cf_opcode'], slot=ins['slot'], opcode=ins['opcode'],
        opcode_name=ins['opcode_name'], fetch_constant=ins['fetch_constant'], dimension=ins['dimension'],
        dimension_name=ins['dimension_name'], is_binding=ins['is_binding'], writes_result=ins['writes_result'],
        is_predicated=ins['is_predicated'])


def decode_tfetch(f):
    """xenos.h xe_gpu_texture_fetch_t L1229-L1330: every bitfield of the six
    host-order dwords, signed fields sign-extended (exp_adjust 6, lod_bias 10,
    grad_exp_adjust_h/v 5, aniso_bias 4), all three size views, plus named
    and byte-scaled conveniences."""
    d0, d1, d2, d3, d4, d5 = (int(x) & 0xFFFFFFFF for x in f)
    t = collections.OrderedDict()
    t['raw'] = [d0, d1, d2, d3, d4, d5]
    t['type'] = d0 & 3                                    # +0 dword_0
    t['sign_x'], t['sign_y'], t['sign_z'], t['sign_w'] = (d0 >> 2) & 3, (d0 >> 4) & 3, (d0 >> 6) & 3, (d0 >> 8) & 3
    t['clamp_x'], t['clamp_y'], t['clamp_z'] = (d0 >> 10) & 7, (d0 >> 13) & 7, (d0 >> 16) & 7
    t['pitch'] = (d0 >> 22) & 0x1FF                       # +22, pixels >> 5
    t['tiled'] = (d0 >> 31) & 1
    t['format'] = d1 & 0x3F                               # +0 dword_1
    t['endianness'] = (d1 >> 6) & 3
    t['request_size'] = (d1 >> 8) & 3
    t['stacked'] = (d1 >> 10) & 1
    t['nearest_clamp_policy'] = (d1 >> 11) & 1
    t['base_address'] = (d1 >> 12) & 0xFFFFF              # +12, bytes >> 12
    t['size_1d'] = collections.OrderedDict(width=d2 & 0xFFFFFF)                                   # dword_2 views
    t['size_2d'] = collections.OrderedDict(width=d2 & 0x1FFF, height=(d2 >> 13) & 0x1FFF, stack_depth=(d2 >> 26) & 0x3F)
    t['size_3d'] = collections.OrderedDict(width=d2 & 0x7FF, height=(d2 >> 11) & 0x7FF, depth=(d2 >> 22) & 0x3FF)
    t['num_format'] = d3 & 1                              # +0 dword_3
    t['swizzle'] = (d3 >> 1) & 0xFFF
    t['exp_adjust'] = sext(d3 >> 13, 6)
    t['mag_filter'], t['min_filter'], t['mip_filter'] = (d3 >> 19) & 3, (d3 >> 21) & 3, (d3 >> 23) & 3
    t['aniso_filter'] = (d3 >> 25) & 7
    t['arbitrary_filter'] = (d3 >> 28) & 7
    t['border_size'] = (d3 >> 31) & 1
    t['vol_mag_filter'] = d4 & 1                          # +0 dword_4
    t['vol_min_filter'] = (d4 >> 1) & 1
    t['mip_min_level'] = (d4 >> 2) & 0xF
    t['mip_max_level'] = (d4 >> 6) & 0xF
    t['mag_aniso_walk'] = (d4 >> 10) & 1
    t['min_aniso_walk'] = (d4 >> 11) & 1
    t['lod_bias'] = sext(d4 >> 12, 10)
    t['grad_exp_adjust_h'] = sext(d4 >> 22, 5)
    t['grad_exp_adjust_v'] = sext(d4 >> 27, 5)
    t['border_color'] = d5 & 3                            # +0 dword_5
    t['force_bc_w_to_max'] = (d5 >> 2) & 1
    t['tri_clamp'] = (d5 >> 3) & 3
    t['aniso_bias'] = sext(d5 >> 5, 4)
    t['dimension'] = (d5 >> 9) & 3
    t['packed_mips'] = (d5 >> 11) & 1
    t['mip_address'] = (d5 >> 12) & 0xFFFFF               # +12, bytes >> 12
    # byte scales, three names and the dimension-selected size (nothing here is
    # a layout derivation; enum labels come from tfetch_names)
    t['type_name'] = FETCH_TYPE_NAMES[t['type']]
    t['format_name'] = FORMAT_TABLE[t['format']][0]
    t['dimension_name'] = DATA_DIMENSION_NAMES[t['dimension']]
    t['base_address_bytes'] = t['base_address'] << 12
    t['mip_address_bytes'] = t['mip_address'] << 12
    t['pitch_pixels'] = t['pitch'] << 5
    dim = t['dimension']
    if dim == 0:
        size = (t['size_1d']['width'] + 1, 1, 1)
    elif dim == 2:
        size = (t['size_3d']['width'] + 1, t['size_3d']['height'] + 1, t['size_3d']['depth'] + 1)
    elif dim == 3:
        size = (t['size_2d']['width'] + 1, t['size_2d']['height'] + 1, 6)
    else:
        size = (t['size_2d']['width'] + 1, t['size_2d']['height'] + 1, t['size_2d']['stack_depth'] + 1)
    t['size'] = collections.OrderedDict(width=size[0], height=size[1], depth=size[2])
    return t


def tfetch_names(t):
    """Enum labels of a decoded fetch constant (xenos.h L102-L200, L814-L822):
    for the human views; not part of the per-row JSON."""
    return collections.OrderedDict(
        endian=ENDIAN_NAMES[t['endianness']],
        format_type=FORMAT_TABLE[t['format']][1], format_bits_per_pixel=FORMAT_TABLE[t['format']][2],
        signs=[TEXTURE_SIGN_NAMES[t[k]] for k in ('sign_x', 'sign_y', 'sign_z', 'sign_w')],
        clamps=[CLAMP_MODE_NAMES[t[k]] for k in ('clamp_x', 'clamp_y', 'clamp_z')],
        mag=TEXTURE_FILTER_NAMES[t['mag_filter']], min=TEXTURE_FILTER_NAMES[t['min_filter']],
        mip=TEXTURE_FILTER_NAMES[t['mip_filter']],
        aniso=ANISO_FILTER_NAMES.get(t['aniso_filter'], 'reserved%d' % t['aniso_filter']),
        arbitrary=ARBITRARY_FILTER_NAMES.get(t['arbitrary_filter'], 'reserved%d' % t['arbitrary_filter']),
        border_color=BORDER_COLOR_NAMES[t['border_color']],
        swizzle_components=[(t['swizzle'] >> (3 * c)) & 7 for c in range(4)])


def tfetch_field_vector(t):
    """decode_tfetch(...) projected onto TEXTURE_FETCH_FIELD_ORDER (the
    oracle's output order)."""
    out = []
    for name in TEXTURE_FETCH_FIELD_ORDER:
        if '.' in name:
            a, b = name.split('.')
            out.append(t[a][b])
        else:
            out.append(t[name])
    return out


def tfetch_dims(t):
    return t['size']['width'], t['size']['height'], t['size']['depth']


def static_binding_census(eng):
    """Conserving census over the per-normal-draw rows; every denominator is
    explicit and nothing beyond the static binding set is inferred."""
    rows = eng.static_bindings
    complete = [r for r in rows if r['analysis_status'] == 'complete']
    stage_status = collections.OrderedDict()
    instr = collections.OrderedDict()
    for st in ('vs', 'ps'):
        c = collections.Counter(r['stages'][st]['status'] for r in rows)
        stage_status[st] = collections.OrderedDict(complete=c['complete'], incomplete=c['incomplete'], missing=c['missing'])
        by_op = collections.Counter()
        binding = non_binding = unknown = 0
        for r in rows:
            key = r['stages'][st]['analysis_key']
            for ins in (eng.shader_table[key]['analysis']['instructions'] if key else []):
                by_op[ins['opcode_name']] += 1
                if not ins['is_binding'] and ins['opcode_name'] == 'unknown':
                    unknown += 1
                elif ins['is_binding']:
                    binding += 1
                else:
                    non_binding += 1
        instr[st] = collections.OrderedDict(binding=binding, non_binding=non_binding, unknown_opcode=unknown,
                                            by_opcode=collections.OrderedDict(sorted(by_op.items())))
    by_type = collections.Counter()
    for r in complete:
        for b in r['bindings']:
            by_type[eng.fetch_constants[b['fetch_constant_key']]['decoded']['type_name']] += 1
    with_binding = sum(1 for r in complete if r['union_slots'])
    without = sum(1 for r in complete if not r['union_slots'])
    not_complete = len(rows) - len(complete)
    distinct = collections.OrderedDict()
    for st in ('vs', 'ps'):
        seen = {}
        for r in rows:
            h = r['stages'][st]['ucode_hash']
            if h is not None:
                seen[h] = r['stages'][st]['status']
        distinct[st] = collections.OrderedDict(complete=sum(1 for v in seen.values() if v == 'complete'),
                                               incomplete=sum(1 for v in seen.values() if v != 'complete'))
    kinds = collections.Counter(d['kind'] for d in eng.draws)
    return collections.OrderedDict(
        analysis_version=STATIC_BINDING_VERSION,
        meaning='static binding opportunities from the pinned ucode analysis (Shader::AnalyzeUcode / '
                'GatherExecInformation / GatherTextureFetchInformation, shader_translator.cc L42-L140, '
                'L349-L378, L435-L470); not the post-translation mask Xenia requests at draw time and not '
                'dynamically executed samples; no consumer range, producer match, ownership or contents claim',
        normal_draws_total=len(rows), draw_packets_total=len(eng.draws),
        excluded=collections.OrderedDict(resolves=kinds.get('resolve', 0), noop_draws=kinds.get('noop', 0),
                                         unknown_mode_draws=kinds.get('unknown_mode', 0)),
        shader_analysis=collections.OrderedDict(vs=stage_status['vs'], ps=stage_status['ps'],
                                                draws_both_complete=len(complete), draws_not_complete=not_complete),
        referenced_instructions=collections.OrderedDict(
            denominator='per-draw occurrences over all normal draws (both complete and not)',
            vs=instr['vs'], ps=instr['ps']),
        referenced_slots=collections.OrderedDict(
            denominator='per-draw slot counts over the draws whose VS and PS analyses are both complete',
            vs=sum(len(mask_slots(r['stages']['vs']['binding_mask'])) for r in complete),
            ps=sum(len(mask_slots(r['stages']['ps']['binding_mask'])) for r in complete),
            union=sum(len(r['union_slots']) for r in complete),
            overlap=sum(len(r['overlap_slots']) for r in complete)),
        referenced_slots_by_fetch_constant_type=collections.OrderedDict(
            (FETCH_TYPE_NAMES[k], by_type[FETCH_TYPE_NAMES[k]]) for k in range(4)),
        type2_slots_total=sum(len(r['type2_slots']) for r in complete),
        unused_type2_slots_total=sum(len(r['unused_type2_slots']) for r in complete),
        draws_with_unused_type2=sum(1 for r in complete if r['unused_type2_slots']),
        draws_with_referenced_non_texture_slot=sum(1 for r in complete if r['referenced_non_texture_slots']),
        draws_with_static_binding=with_binding, draws_with_no_static_binding=without,
        draws_analysis_not_complete=not_complete,
        conservation_holds=(with_binding + without + not_complete == len(rows)),
        t3a1=collections.OrderedDict(
            claim='every type-2 fetch constant at a normal draw is a texture consumer of that draw',
            complete_draws=len(complete),
            type2_set_equals_union=sum(1 for r in complete if set(r['type2_slots']) == set(r['union_slots'])),
            type2_has_unused=sum(1 for r in complete if r['unused_type2_slots']),
            union_has_non_type2=sum(1 for r in complete if r['referenced_non_texture_slots'])),
        distinct_shaders=distinct)


# ── TOOL-3B: requested bindings and texture backing (schema /6) ─────────────

TEXTURE_BACKING_VERSION = 'xtr_draws-texture-backing/1'
# texture_info_formats.inl L1-L64: (block_width, block_height) per TextureFormat, row for row with
# FORMAT_TABLE (name, type, bits per pixel); the pin's bytes_per_block = bw * bh * bpp / 8 (k_32_AS_8,
# k_32_AS_8_8 and k_32_AS_8_INTERLACED are 4x1 / 2x1 / 4x1 blocks; k_1 / k_1_REVERSE have 0 bytes per block)
FORMAT_BLOCK_TABLE = [
    (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (2, 1), (2, 1), (1, 1),
    (1, 1), (1, 1), (1, 1), (1, 1), (4, 4), (4, 4), (4, 4), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1),
    (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (1, 1), (4, 1), (2, 1), (1, 1),
    (1, 1), (1, 1), (4, 1), (1, 1), (1, 1), (1, 1), (1, 1), (4, 4), (1, 1), (4, 4), (4, 4), (4, 4), (1, 1), (1, 1),
    (1, 1), (1, 1), (4, 4), (4, 4), (4, 4), (4, 4), (1, 1), (1, 1),
]
assert len(FORMAT_BLOCK_TABLE) == 64
_FORMAT_INDEX = {name: i for i, (name, _t, _b) in enumerate(FORMAT_TABLE)}
# texture_info.h L27-L60 / L89-L113 GetBaseFormat: the eleven resampling / gamma formats
BASE_FORMAT_REMAP = {_FORMAT_INDEX[a]: _FORMAT_INDEX[b] for a, b in (
    ('k_16_EXPAND', 'k_16_FLOAT'), ('k_16_16_EXPAND', 'k_16_16_FLOAT'), ('k_16_16_16_16_EXPAND', 'k_16_16_16_16_FLOAT'),
    ('k_8_8_8_8_AS_16_16_16_16', 'k_8_8_8_8'), ('k_DXT1_AS_16_16_16_16', 'k_DXT1'),
    ('k_DXT2_3_AS_16_16_16_16', 'k_DXT2_3'), ('k_DXT4_5_AS_16_16_16_16', 'k_DXT4_5'),
    ('k_2_10_10_10_AS_16_16_16_16', 'k_2_10_10_10'), ('k_10_11_11_AS_16_16_16_16', 'k_10_11_11'),
    ('k_11_11_10_AS_16_16_16_16', 'k_11_11_10'), ('k_8_8_8_8_GAMMA_EDRAM', 'k_8_8_8_8'))}
GPU_ALLOW_INVALID_FETCH_CONSTANTS = True      # gpu_flags.cc L29-L34: the pinned DEFAULT is true
TEXTURE_MAX_MIPS = 14                         # xenos.h L1194-L1197: max(13, 11) + 1
TEXTURE_SUBRESOURCE_ALIGNMENT = 1 << 12       # xenos.h L1177-L1179
TEXTURE_LINEAR_ROW_ALIGNMENT = 1 << 8         # xenos.h L1224-L1226
TEXTURE_2D_CUBE_MAX_WIDTH_HEIGHT = 1 << 13    # xenos.h L1184-L1186
PHYSICAL_PAGE_MASK = 0x1FFFF                  # texture_util.cc L79-L80: 17-bit physical 4 KB page
BACKING_STATUSES = ('complete', 'invalid_type', 'no_data', 'invalid_1d', 'unsupported_format')
POST_STATUSES = ('translated', 'translation_invalid', 'analysis_mismatch', 'not_in_table', 'no_table',
                 'ucode_unmapped', 'missing')

TEXTURE_BACKING_MANIFEST = dict(
    backing_version=TEXTURE_BACKING_VERSION,
    xenia_commit=XENIA_PIN, xenia_repository=XENIA_REPOSITORY,
    input='one exact 24-byte texture fetch constant (the six raw dwords at the draw) that a stage of the draw '
          'requests through its post-translation mask',
    output='TextureKey fields, normalized subresources, TextureGuestLayout, and the separate base and mip guest '
           'backing intervals [page << 12, + size) that pinned Xenia watches for the texture',
    host_defaults_assumed=dict(gpu_allow_invalid_fetch_constants=True),
    host_default_note='texture_cache.cc L884-L887 accepts FetchConstantType::kInvalidTexture only under the cvar; '
                      'the pinned default (gpu_flags.cc L29-L34) is true and is what this derivation assumes; the '
                      'row records allowed_by_cvar so the other setting can be applied without recomputation',
    integer_model='uint32 arithmetic throughout (xe::align, next_pow2, strides, extents); the 3D tiled upper bound '
                  'is computed in uint64 and truncated to uint32 as in the pin; fields the pin leaves '
                  'uninitialized (mip_offsets_bytes[0] when the packed tail is not level 0; mips[0] when the packed '
                  'tail is level 0 and there are no mips) are reported as null and named in undefined_in_pin',
    stages=collections.OrderedDict([
        ('request_mask', [_perma('src/xenia/gpu/d3d12/d3d12_command_processor.cc', 2664, 2669),
                          _perma('src/xenia/gpu/dxbc_shader_translator.cc', 1348, 1372),
                          _perma('src/xenia/gpu/dxbc_shader.h', 52, 58)]),
        ('binding_info_from_fetch_constant', [_perma('src/xenia/gpu/texture_cache.cc', 871, 965),
                                              _perma('src/xenia/gpu/gpu_flags.cc', 29, 34)]),
        ('subresources_from_fetch_constant', [_perma('src/xenia/gpu/texture_util.cc', 23, 114)]),
        ('base_format', [_perma('src/xenia/gpu/texture_info.h', 27, 60), _perma('src/xenia/gpu/texture_info.h', 89, 113)]),
        ('format_block_geometry', [_perma('src/xenia/gpu/texture_info_formats.cc', 21, 26),
                                   _perma('src/xenia/gpu/texture_info_formats.cc', 45, 62),
                                   _perma('src/xenia/gpu/texture_info.h', 195, 201)]),
        ('packed_mip_level', [_perma('src/xenia/gpu/texture_util.h', 41, 44)]),
        ('packed_mip_offset', [_perma('src/xenia/gpu/texture_util.cc', 116, 206)]),
        ('guest_texture_layout', [_perma('src/xenia/gpu/texture_util.cc', 208, 448)]),
        ('tiled_upper_bounds', [_perma('src/xenia/gpu/texture_util.cc', 450, 510)]),
        ('texture_key_and_guest_sizes', [_perma('src/xenia/gpu/texture_cache.h', 164, 224),
                                         _perma('src/xenia/gpu/texture_cache.h', 247, 252),
                                         _perma('src/xenia/gpu/texture_cache.cc', 520, 536)]),
        ('constants', [_perma('src/xenia/gpu/xenos.h', 1177, 1179), _perma('src/xenia/gpu/xenos.h', 1184, 1186),
                       _perma('src/xenia/gpu/xenos.h', 1194, 1197), _perma('src/xenia/gpu/xenos.h', 1198, 1203),
                       _perma('src/xenia/gpu/xenos.h', 1224, 1226)]),
        ('math', [_perma('src/xenia/base/math.h', 97, 106), _perma('src/xenia/base/math.h', 325, 330)]),
    ]),
)


def xe_log2_ceil(v):
    """base/math.h L325-L330: 32 - lzcnt(v - 1) in uint32 (0 -> 32, 1 -> 0)."""
    v = u32(v - 1)
    return v.bit_length()


def xe_next_pow2(v):
    """base/math.h L97-L106 in uint32 (0 -> 0)."""
    v = u32(v - 1)
    v |= v >> 1
    v |= v >> 2
    v |= v >> 4
    v |= v >> 8
    v |= v >> 16
    return u32(v + 1)


def format_block_info(fmt):
    """FormatInfo for one TextureFormat: (block_width, block_height, bits_per_pixel, bytes_per_block)."""
    bw, bh = FORMAT_BLOCK_TABLE[fmt]
    bpp = FORMAT_TABLE[fmt][2]
    return bw, bh, bpp, (bw * bh * bpp) // 8


def get_packed_mip_level(width, height):
    """texture_util.h L41-L44."""
    l2 = xe_log2_ceil(min(width, height))
    return l2 - 4 if l2 > 4 else 0


def get_packed_mip_offset(width, height, depth, fmt, mip):
    """texture_util.cc L116-L206 -> (packed, x_blocks, y_blocks, z_blocks)."""
    log2_width, log2_height = xe_log2_ceil(width), xe_log2_ceil(height)
    log2_size = min(log2_width, log2_height)
    if log2_size > 4 + mip:
        return False, 0, 0, 0
    packed_mip_base = log2_size - 4 if log2_size > 4 else 0
    packed_mip = mip - packed_mip_base
    if packed_mip < 3:
        if log2_width > log2_height:
            x_blocks, y_blocks = 0, 16 >> packed_mip
        else:
            x_blocks, y_blocks = 16 >> packed_mip, 0
        z_blocks = 0
    else:
        if log2_width > log2_height:
            offset = u32(1 << (log2_width - packed_mip_base)) >> (packed_mip - 2)
            x_blocks, y_blocks = offset, 0
        else:
            offset = u32(1 << (log2_height - packed_mip_base)) >> (packed_mip - 2)
            x_blocks, y_blocks = 0, offset
        if offset < 4:
            log2_depth = xe_log2_ceil(depth)
            z_blocks = (log2_depth - packed_mip) * 4 if log2_depth > 1 + packed_mip else 4
        else:
            z_blocks = 0
    bw, bh, _bpp, _bpb = format_block_info(fmt)
    return True, x_blocks >> log2_floor(bw), y_blocks >> log2_floor(bh), z_blocks


def get_subresources_from_fetch(t):
    """texture_util.cc L23-L114 on a decode_tfetch() dict."""
    dim = t['dimension']
    if dim == 0:
        w1, h1, d1 = t['size_1d']['width'], 0, 0
    elif dim == 1:
        w1, h1 = t['size_2d']['width'], t['size_2d']['height']
        d1 = t['size_2d']['stack_depth'] if t['stacked'] else 0
    elif dim == 2:
        w1, h1, d1 = t['size_3d']['width'], t['size_3d']['height'], t['size_3d']['depth']
    else:
        w1, h1, d1 = t['size_2d']['width'], t['size_2d']['height'], 5
    longest = max(w1, h1)
    if dim == 2:
        longest = max(longest, d1)
    size_mip_max_level = log2_floor(u32(longest + 1))
    base_page = t['base_address'] & PHYSICAL_PAGE_MASK
    mip_page = t['mip_address'] & PHYSICAL_PAGE_MASK
    if mip_page == 0:
        mip_min_level = mip_max_level = 0
    else:
        mip_min_level = min(t['mip_min_level'], size_mip_max_level)
        mip_max_level = max(min(t['mip_max_level'], size_mip_max_level), mip_min_level)
    if mip_max_level != 0:
        if base_page == 0:
            mip_min_level = max(mip_min_level, 1)
        if mip_min_level != 0:
            base_page = 0
    else:
        mip_page = 0
    return collections.OrderedDict(width_minus_1=w1, height_minus_1=h1, depth_or_array_size_minus_1=d1,
                                   base_page=base_page, mip_page=mip_page, mip_min_level=mip_min_level,
                                   mip_max_level=mip_max_level, size_mip_max_level=size_mip_max_level)


def binding_info_from_fetch(t, allow_invalid=GPU_ALLOW_INVALID_FETCH_CONSTANTS):
    """texture_cache.cc L871-L965 -> (status, reason, key or None, subresources)."""
    ftype = t['type']
    if ftype == 2:
        pass
    elif ftype == 0 and allow_invalid:
        pass
    elif ftype == 0:
        return ('invalid_type', 'fetch constant type kInvalidTexture and gpu_allow_invalid_fetch_constants is off '
                '(texture_cache.cc L884-L895)', None, None)
    else:
        return ('invalid_type', 'fetch constant type %s(%d) is completely invalid (texture_cache.cc L896-L902)'
                % (FETCH_TYPE_NAMES[ftype], ftype), None, None)
    sub = get_subresources_from_fetch(t)
    if sub['base_page'] == 0 and sub['mip_page'] == 0:
        return 'no_data', 'no texture data at all: base and mip pages both 0 (texture_cache.cc L910-L913)', None, sub
    if t['dimension'] == 0:
        bad = []
        if sub['width_minus_1'] >= TEXTURE_2D_CUBE_MAX_WIDTH_HEIGHT:
            bad.append('1D texture too wide (%d)' % (sub['width_minus_1'] + 1))
        if t['tiled']:
            bad.append('1D texture tiled')
        if t['packed_mips']:
            bad.append('1D texture with packed mips')
        if bad:
            return 'invalid_1d', '; '.join(bad) + ' - ignored by pinned Xenia (texture_cache.cc L914-L942)', None, sub
    fmt = BASE_FORMAT_REMAP.get(t['format'], t['format'])
    key = collections.OrderedDict(
        base_page=sub['base_page'], mip_page=sub['mip_page'], dimension=t['dimension'],
        dimension_name=DATA_DIMENSION_NAMES[t['dimension']], width_minus_1=sub['width_minus_1'],
        height_minus_1=sub['height_minus_1'], depth_or_array_size_minus_1=sub['depth_or_array_size_minus_1'],
        pitch=t['pitch'], pitch_texels=t['pitch'] << 5, mip_max_level=sub['mip_max_level'], tiled=t['tiled'],
        packed_mips=t['packed_mips'], format=fmt, format_name=FORMAT_TABLE[fmt][0], raw_format=t['format'],
        endianness=t['endianness'])
    return 'valid', None, key, sub


def _level_dict(row_pitch_bytes, z_slice_stride_block_rows, array_slice_stride_bytes, x_extent_blocks,
                y_extent_blocks, z_extent, array_slice_data_extent_bytes, level_data_extent_bytes):
    return collections.OrderedDict(row_pitch_bytes=row_pitch_bytes, z_slice_stride_block_rows=z_slice_stride_block_rows,
                                   array_slice_stride_bytes=array_slice_stride_bytes, x_extent_blocks=x_extent_blocks,
                                   y_extent_blocks=y_extent_blocks, z_extent=z_extent,
                                   array_slice_data_extent_bytes=array_slice_data_extent_bytes,
                                   level_data_extent_bytes=level_data_extent_bytes)


ZERO_LEVEL = _level_dict(0, 0, 0, 0, 0, 0, 0, 0)


def guest_texture_layout(dimension, pitch_div_32, width, height, depth_or_array_size, tiled, fmt, packed, has_base,
                         max_level):
    """texture_util.cc L208-L448 in uint32, level by level. Levels and offsets
    the pin never writes are None and listed in undefined_in_pin."""
    undefined = []
    if dimension == 0:
        height = 1
    depth = depth_or_array_size if dimension == 2 else 1
    array_size = depth_or_array_size if dimension == 1 else (6 if dimension == 3 else 1)
    L = collections.OrderedDict(array_size=array_size, max_level=0, packed_level=None, base=None,
                                mips=[None] * TEXTURE_MAX_MIPS, mip_offsets_bytes=[None] * TEXTURE_MAX_MIPS,
                                mips_total_extent_bytes=0, undefined_in_pin=undefined)
    if not width or not height or not depth or not array_size:            # L236-L241: all zero
        L.update(array_size=0, packed_level=0, base=dict(ZERO_LEVEL), mips=[dict(ZERO_LEVEL)] * TEXTURE_MAX_MIPS,
                 mip_offsets_bytes=[0] * TEXTURE_MAX_MIPS)
        return L
    max_level = min(max_level, log2_floor(max(width, height, depth)))    # L244-L248
    L['max_level'] = max_level
    packed_level = get_packed_mip_level(width, height) if packed else None   # UINT32_MAX when not packed
    L['packed_level'] = packed_level
    pl = packed_level if packed_level is not None else 0xFFFFFFFF
    if not has_base:
        L['base'] = dict(ZERO_LEVEL)
    if pl != 0:
        L['mips'][0] = dict(ZERO_LEVEL)
    max_stored_level = min(max_level, pl)
    mips_end = max_stored_level + 1
    for i in range(mips_end, TEXTURE_MAX_MIPS):
        L['mips'][i] = dict(ZERO_LEVEL)
        L['mip_offsets_bytes'][i] = 0
    bw, bh, _bpp, bytes_per_block = format_block_info(fmt)
    loop_level_last = int(max_level != 0) if pl == 0 else max_stored_level
    mip_offset_bytes = 0
    for loop_level in range(0 if has_base else 1, loop_level_last + 1):
        is_base = loop_level == 0
        level = 0 if pl == 0 else loop_level
        if is_base:
            rp_texels = u32(pitch_div_32 << 5)
            zs_rows = height
        else:
            rp_texels = max(xe_next_pow2(width) >> level, 1)
            zs_rows = max(xe_next_pow2(height) >> level, 1)
        rp_blocks_tile_aligned = xe_align(xe_align(rp_texels, bw) // bw, TEXTURE_TILE_WH)
        row_pitch_bytes = u32(rp_blocks_tile_aligned * bytes_per_block)
        if not tiled and not is_base:
            row_pitch_bytes = xe_align(row_pitch_bytes, TEXTURE_LINEAR_ROW_ALIGNMENT)
        z_slice_stride_block_rows = (xe_align(xe_align(zs_rows, bh) // bh, TEXTURE_TILE_WH) if dimension != 0 else 1)
        array_slice_stride_bytes = u32(row_pitch_bytes * z_slice_stride_block_rows)
        z_stride_bytes = array_slice_stride_bytes
        if dimension == 2:
            array_slice_stride_bytes = u32(array_slice_stride_bytes * xe_align(depth_or_array_size, TEXTURE_TILE_DEPTH))
        array_slice_stride_bytes = u32(xe_align(array_slice_stride_bytes, TEXTURE_SUBRESOURCE_ALIGNMENT))
        if level == pl:
            x_extent = y_extent = z_extent = 0
            last = 0 if is_base else max_level
            for sub in range(pl, last + 1):
                _p, sx, sy, sz = get_packed_mip_offset(width, height, depth, fmt, sub)
                x_extent = max(x_extent, u32(sx + xe_align(max(width >> sub, 1), bw) // bw))
                y_extent = max(y_extent, u32(sy + xe_align(max(height >> sub, 1), bh) // bh))
                z_extent = max(z_extent, u32(sz + max(depth >> sub, 1)))
        else:
            x_extent = xe_align(max(width >> level, 1), bw) // bw
            y_extent = xe_align(max(height >> level, 1), bh) // bh
            z_extent = max(depth >> level, 1)
        if tiled:
            bpb_log2 = log2_floor(bytes_per_block)
            if dimension == 2:
                slice_data = u32(tiled_upper_bound_3d(x_extent, y_extent, z_extent, rp_blocks_tile_aligned,
                                                      z_slice_stride_block_rows, bpb_log2))
            else:
                slice_data = tiled_upper_bound_2d(x_extent, y_extent, rp_blocks_tile_aligned, bpb_log2)
        else:
            slice_data = u32(z_stride_bytes * (z_extent - 1) + row_pitch_bytes * (y_extent - 1)
                             + bytes_per_block * x_extent)
        level_data = u32(array_slice_stride_bytes * (array_size - 1) + slice_data)
        lv = _level_dict(row_pitch_bytes, z_slice_stride_block_rows, array_slice_stride_bytes, x_extent, y_extent,
                         z_extent, slice_data, level_data)
        if is_base:
            L['base'] = lv
        else:
            L['mips'][level] = lv
            L['mip_offsets_bytes'][level] = mip_offset_bytes
            L['mips_total_extent_bytes'] = max(L['mips_total_extent_bytes'], u32(mip_offset_bytes + level_data))
            mip_offset_bytes = u32(mip_offset_bytes + array_slice_stride_bytes * array_size)
    for i in range(TEXTURE_MAX_MIPS):
        if L['mips'][i] is None:
            undefined.append('mips[%d]' % i)
        if L['mip_offsets_bytes'][i] is None:
            undefined.append('mip_offsets_bytes[%d]' % i)
    return L


def derive_texture_backing(raw):
    """The pinned request-to-backing path for ONE exact fetch constant: returns
    the named status, the TextureKey fields, subresources, layout and the
    separate base / mip guest backing intervals (what Texture watches)."""
    t = decode_tfetch(raw)
    d = collections.OrderedDict(backing_version=TEXTURE_BACKING_VERSION, status=None, reason=None, join_usable=False,
                                fetch_constant_type=t['type'], fetch_constant_type_name=t['type_name'],
                                allowed_by_cvar=(t['type'] == 0))
    status, reason, key, sub = binding_info_from_fetch(t)
    d['subresources'] = sub
    if status != 'valid':
        d.update(status=status, reason=reason, key=None, format=None, layout=None, base_interval=None,
                 mips_interval=None)
        return d
    d['key'] = key
    bw, bh, bpp, bpb = format_block_info(key['format'])
    d['format'] = collections.OrderedDict(block_width=bw, block_height=bh, bits_per_pixel=bpp, bytes_per_block=bpb,
                                          bytes_per_block_log2=log2_floor(bpb) if bpb else None)
    if bpb == 0:
        d.update(status='unsupported_format',
                 reason='%s has 0 bytes per block: log2_floor(0) is undefined in the pinned layout arithmetic'
                        % key['format_name'], layout=None, base_interval=None, mips_interval=None)
        return d
    layout = guest_texture_layout(key['dimension'], key['pitch'], key['width_minus_1'] + 1, key['height_minus_1'] + 1,
                                  key['depth_or_array_size_minus_1'] + 1, key['tiled'], key['format'],
                                  key['packed_mips'], key['base_page'] != 0, key['mip_max_level'])
    d['layout'] = layout
    base_size = layout['base']['level_data_extent_bytes'] if layout['base'] is not None else 0
    mips_size = layout['mips_total_extent_bytes']
    if key['base_page'] != 0:
        start = key['base_page'] << 12
        d['base_interval'] = collections.OrderedDict(start=start, length=base_size, end_exclusive=u32(start + base_size))
    else:
        d['base_interval'] = None
    if key['mip_page'] != 0:
        start = key['mip_page'] << 12
        d['mips_interval'] = collections.OrderedDict(start=start, length=mips_size, end_exclusive=u32(start + mips_size))
    else:
        d['mips_interval'] = None
    d['status'] = 'complete'
    d['join_usable'] = bool((d['base_interval'] and d['base_interval']['length'])
                            or (d['mips_interval'] and d['mips_interval']['length']))
    return d


def mask_slots(mask):
    """The slot list of a 32-bit fetch-constant mask (None -> [])."""
    return [b for b in range(32) if mask and (mask >> b) & 1]


def fetch_constant_key(raw):
    """The exact 24-byte raw content as 48 hex digits (dword order)."""
    return ''.join('%08X' % (int(x) & 0xFFFFFFFF) for x in raw)


def load_post_mask_table(path):
    """The recorded actual-translator run (archive/tool3b/mask_oracle.py output):
    identity + per (stage, ucode SHA-256) post-translation mask and status."""
    with open(path, encoding='utf-8') as f:
        j = json.load(f)
    ident = j.get('identity', {})
    table = dict(path=os.path.abspath(path), file_sha256=sha256_file(path), identity=ident,
                 pin_matches=(ident.get('xenia_pin') == XENIA_PIN), shaders=len(j.get('shaders', [])), by_key={})
    for e in j.get('shaders', []):
        table['by_key'][(e['stage'], e['ucode_sha256'])] = e
    return table


def post_translation_for(table, stage, ucode_sha256, analysis_mask, unmapped):
    """Per-stage post-translation status from the table; fails closed."""
    p = collections.OrderedDict(status=None, reason=None, mask=None, binding_slots=[], bindings=[])
    if unmapped:
        p.update(status='ucode_unmapped', reason='ucode partly unmapped: never translated')
        return p
    if table is None:
        p.update(status='no_table', reason='no post-translation mask table supplied (--post-masks)')
        return p
    if not table['pin_matches']:
        p.update(status='not_in_table', reason='table pin %r is not the tool pin' % table['identity'].get('xenia_pin'))
        return p
    e = table['by_key'].get((stage, ucode_sha256))
    if e is None:
        p.update(status='not_in_table', reason='this %s ucode (sha256 %s) was not translated by the recorded run'
                 % (stage, ucode_sha256[:16]))
        return p
    if not e.get('translation_valid'):
        p.update(status='translation_invalid', reason='DxbcShaderTranslator produced an invalid translation (%d errors)'
                 % e.get('translation_errors', 0))
        return p
    if int(e['xenia_analysis_mask'], 16) != analysis_mask:
        p.update(status='analysis_mismatch', reason="Xenia's analysis mask %s differs from this tool's %08X"
                 % (e['xenia_analysis_mask'], analysis_mask))
        return p
    mask = int(e['post_mask'], 16)
    p.update(status='translated', mask=mask, binding_slots=[b for b in range(32) if (mask >> b) & 1],
             bindings=[collections.OrderedDict(fetch_constant=b['fetch_constant'], dimension=b['dimension'],
                                               dimension_name=FETCH_OP_DIMENSION_NAMES[b['dimension']],
                                               is_signed=b['is_signed']) for b in e.get('post_bindings', [])])
    return p


def expand_static_bindings_v5(summary):
    """Deterministic expander: rebuild the accepted /5 projection
    (static_texture_shader_analyses, static_texture_bindings) from the /6
    normalized tables. Nothing is computed anew; every field is copied."""
    shaders = summary['static_texture_shaders']
    consts = summary['texture_fetch_constants']
    analyses = collections.OrderedDict()
    for e in sorted(shaders.values(), key=lambda e: e['ucode_xxh3_64'] or ''):
        if e['ucode_xxh3_64'] is not None and not e['unmapped_bytes'] and e['ucode_xxh3_64'] not in analyses:
            analyses[e['ucode_xxh3_64']] = e['analysis']
    rows = []
    for r in summary['static_texture_bindings']:
        stages = collections.OrderedDict()
        ins_by_stage = {}
        for st in ('vs', 'ps'):
            src = r['stages'][st]
            a = shaders[src['analysis_key']]['analysis'] if src['analysis_key'] else None
            ins_by_stage[st] = a['instructions'] if a else []
            stages[st] = collections.OrderedDict(
                status=src['status'], reason=src['reason'], ucode_hash=src['ucode_hash'],
                binding_slots=mask_slots(src['binding_mask']), binding_mask=src['binding_mask'],
                instructions=[compact_texture_instruction(i) for i in ins_by_stage[st]],
                binding_instructions=a['binding_instructions'] if a else 0,
                non_binding_instructions=a['non_binding_instructions'] if a else 0,
                unknown_opcode_instructions=a['unknown_opcode_instructions'] if a else 0)
        bindings = []
        for b in r['bindings']:
            c = consts[b['fetch_constant_key']]
            bindings.append(collections.OrderedDict(
                slot=b['slot'], stages=list(b['stages']),
                instructions_per_stage=dict((st, sum(1 for i in ins_by_stage[st] if i['is_binding'] and i['fetch_constant'] == b['slot']))
                                            for st in ('vs', 'ps')),
                fetch_constant_type=c['decoded']['type'], fetch_constant_type_name=c['decoded']['type_name'],
                is_texture_constant=c['decoded']['type'] == 2, raw=list(c['raw']), decoded=c['decoded']))
        rows.append(collections.OrderedDict(
            draw_ord=r['draw_ord'], frame=r['frame'], mode=r['mode'], vs=r['vs'], ps=r['ps'],
            analysis_status=r['analysis_status'], stages=stages, union_slots=list(r['union_slots']),
            overlap_slots=list(r['overlap_slots']), bindings=bindings, type2_slots=list(r['type2_slots']),
            unused_type2_slots=list(r['unused_type2_slots']),
            referenced_non_texture_slots=list(r['referenced_non_texture_slots'])))
    return collections.OrderedDict(static_texture_shader_analyses=analyses, static_texture_bindings=rows,
                                   static_texture_binding_census=summary['static_texture_binding_census'])


def requested_binding_census(eng):
    """Conserving census of post-translation status, requested slots and
    backing statuses; every denominator explicit."""
    rows = eng.static_bindings
    post_rows = [r for r in rows if r['post_translation']['status'] == 'complete']
    stage_status = collections.OrderedDict()
    for st in ('vs', 'ps'):
        c = collections.Counter(r['post_translation'][st]['status'] for r in rows)
        stage_status[st] = collections.OrderedDict((k, c[k]) for k in POST_STATUSES)
    req_consts = {k for k, e in eng.fetch_constants.items() if e['requested']}
    cstat = collections.Counter(eng.fetch_constants[k]['backing']['status'] for k in req_consts)
    ostat = collections.Counter(o['backing_status'] for r in post_rows for o in r['requested_binding_opportunities'])
    eq_rows = sum(1 for r in post_rows if r['post_translation']['requested_equals_analysis_union'])
    all_usable = sum(1 for r in post_rows if r['requested_binding_opportunities']
                     and all(o['join_usable'] for o in r['requested_binding_opportunities']))
    none_req = sum(1 for r in post_rows if not r['requested_binding_opportunities'])
    some_unusable = len(post_rows) - all_usable - none_req
    t = eng.post_table
    return collections.OrderedDict(
        backing_version=TEXTURE_BACKING_VERSION,
        meaning='requested binding opportunities = slots in the union of the two stages\' POST-TRANSLATION masks as '
                'recorded from the actual pinned DxbcShaderTranslator; backing intervals = what pinned Xenia would '
                'watch for the texture (Texture::GetGuestBaseSize/GetGuestMipsSize); neither proves a sample '
                'executed, bytes read, a producer match, ownership or contents',
        post_translation_table=collections.OrderedDict(
            supplied=t is not None, path=t['path'] if t else None, file_sha256=t['file_sha256'] if t else None,
            pin_matches=t['pin_matches'] if t else None, shaders_in_table=t['shaders'] if t else 0,
            xenia_pin=t['identity'].get('xenia_pin') if t else None,
            oracle_source_sha256=t['identity'].get('oracle_source_sha256') if t else None,
            oracle_exe_sha256=t['identity'].get('oracle_exe_sha256') if t else None,
            translator_options=t['identity'].get('translator_options') if t else None),
        normal_draws_total=len(rows),
        post_translation=collections.OrderedDict(vs=stage_status['vs'], ps=stage_status['ps'],
                                                 draws_post_complete=len(post_rows),
                                                 draws_post_not_complete=len(rows) - len(post_rows)),
        requested_slots=collections.OrderedDict(
            denominator='per-draw slot counts over the draws whose VS and PS are both translated',
            vs=sum(len(mask_slots(r['post_translation']['vs']['mask'])) for r in post_rows),
            ps=sum(len(mask_slots(r['post_translation']['ps']['mask'])) for r in post_rows),
            union=sum(len(r['post_translation']['requested_union']) for r in post_rows),
            overlap=sum(len(r['post_translation']['requested_overlap']) for r in post_rows)),
        requested_vs_analysis=collections.OrderedDict(
            draws_equal=eq_rows, draws_differ=len(post_rows) - eq_rows,
            slots_requested_not_analysis=sum(len(r['post_translation']['requested_minus_analysis']) for r in post_rows),
            slots_analysis_not_requested=sum(len(r['post_translation']['analysis_minus_requested']) for r in post_rows)),
        distinct_shaders_translated=collections.OrderedDict(
            vs=sum(1 for e in eng.shader_table.values() if e['post_translation'].get('vs', {}).get('status') == 'translated'),
            ps=sum(1 for e in eng.shader_table.values() if e['post_translation'].get('ps', {}).get('status') == 'translated')),
        distinct_fetch_constants=len(eng.fetch_constants),
        distinct_requested_constants=len(req_consts),
        constant_backing_status=collections.OrderedDict((k, cstat[k]) for k in BACKING_STATUSES),
        constant_backing_conserved=(sum(cstat.values()) == len(req_consts)),
        constants_join_usable=sum(1 for k in req_consts if eng.fetch_constants[k]['backing']['join_usable']),
        occurrence_backing_status=collections.OrderedDict((k, ostat[k]) for k in BACKING_STATUSES),
        occurrences_total=sum(ostat.values()),
        occurrences_join_usable=sum(1 for r in post_rows for o in r['requested_binding_opportunities'] if o['join_usable']),
        draws_all_requested_join_usable=all_usable, draws_with_requested_not_usable=some_unusable,
        draws_no_requested_slots=none_req,
        draws_conserved=(all_usable + some_unusable + none_req == len(post_rows)))


# ── TOOL-3C: temporal resolve/consumer interval relation (schema /7) ─────────

RELATION_VERSION = 'xtr_draws-resolve-consumer-relation/1'
RELATIONS = ('equal', 'producer_contains_consumer', 'consumer_contains_producer', 'partial_overlap', 'disjoint')
NON_DISJOINT_RELATIONS = RELATIONS[:4]
PRODUCER_EXCLUSIONS = ('not_join_usable', 'no_range', 'invariants_violated', 'wrapped_interval', 'inconsistent_length',
                       'empty_interval')
OCCURRENCE_EXCLUSIONS = ('cvar_excluded', 'backing_not_join_usable')
COMPONENT_EXCLUSIONS = ('absent', 'wrapped_interval', 'inconsistent_length', 'empty_interval')
RELATION_MEANING = ('resolve-command destination ranges (TOOL-2B, opportunities that a copy was issued) related to the '
                    'SEPARATE base and mip guest backing intervals of the requested binding occurrences (TOOL-3B, '
                    'opportunities that a texture was bound) of STRICTLY LATER normal draws of the same trace; '
                    'half-open byte intervals, endpoint contact is disjoint, exactly one relation per pair; every '
                    'overlapping earlier resolve is kept - no latest/preferred owner is selected, no copy or sample is '
                    'proven executed, no resource identity, contents or format compatibility is inferred; the '
                    'destination and key metadata on a detail row are observations only')
RELATION_DEFINITIONS = collections.OrderedDict(
    producer='resolve_details row whose guest_destination_range is join_usable with invariants_hold, a positive length '
             'and monotonic half-open endpoints where end - start == length; interval [extent_start, extent_end_exclusive)',
    consumer='normal draw with both post-translation stages translated; each requested binding occurrence whose backing '
             'is join_usable after the cvar filter contributes its base_interval and its mips_interval as separate '
             'components, [start, end_exclusive)',
    temporal='consumer draw_ord > producer draw_ord within one trace; earlier or same-ordinal draws are never compared',
    relations=collections.OrderedDict(
        disjoint='producer.end <= consumer.start or consumer.end <= producer.start (endpoint contact included)',
        equal='identical endpoints', producer_contains_consumer='producer.start <= consumer.start and consumer.end <= producer.end',
        consumer_contains_producer='consumer.start <= producer.start and producer.end <= consumer.end',
        partial_overlap='overlapping with neither containing the other'),
    intersection='[max(starts), min(ends)) of every non-disjoint pair',
    cvar_filter='gpu_allow_invalid_fetch_constants=false excludes occurrences whose backing has allowed_by_cvar=true; the '
                'layout arithmetic is not recomputed',
    detail_rows='only non-disjoint pairs are materialized; disjoint pairs are counted',
    observations='producer destination metadata is kept once per producer (producers.rows[resolve_index].destination_observed); '
                 'consumer key metadata once per constant (texture_fetch_constants[fetch_constant_key].backing.key); a detail '
                 'row references both by resolve_index and fetch_constant_key and repeats neither',
)


def interval_defect(start, end_exclusive, length):
    """The named defect of a declared half-open interval, or None when it is monotonic, consistent and non-empty."""
    if end_exclusive < start:
        return 'wrapped_interval'
    if end_exclusive - start != length:
        return 'inconsistent_length'
    if length == 0:
        return 'empty_interval'
    return None


def classify_intervals(p_start, p_end, c_start, c_end):
    """Producer [p_start, p_end) against consumer [c_start, c_end), both
    non-empty and monotonic: exactly one of RELATIONS plus the intersection
    (None when disjoint). Endpoint contact is disjoint."""
    if p_end <= c_start or c_end <= p_start:
        return 'disjoint', None
    if p_start == c_start and p_end == c_end:
        rel = 'equal'
    elif p_start <= c_start and c_end <= p_end:
        rel = 'producer_contains_consumer'
    elif c_start <= p_start and p_end <= c_end:
        rel = 'consumer_contains_producer'
    else:
        rel = 'partial_overlap'
    i_start, i_end = max(p_start, c_start), min(p_end, c_end)
    return rel, collections.OrderedDict(start=i_start, end_exclusive=i_end, length=i_end - i_start)


def temporally_eligible(producer_draw_ord, consumer_draw_ord):
    """A resolve is related only to a STRICTLY later draw of the same trace."""
    return consumer_draw_ord > producer_draw_ord


def relation_producers(eng):
    """Eligible producers from the /4 rows, every exclusion named per row."""
    rows, excluded, excluded_rows = [], collections.Counter(), []
    for i, r in enumerate(eng.resolves):
        g = r['guest_destination_range']
        rg = g.get('range')
        reasons = []                                  # every failed check, not only the first
        if not g.get('join_usable'):
            reasons.append('not_join_usable')
        if rg is None:
            reasons.append('no_range')
        else:
            if not rg['invariants_hold']:
                reasons.append('invariants_violated')
            defect = interval_defect(rg['extent_start'], rg['extent_end_exclusive'], rg['extent_length'])
            if defect:
                reasons.append(defect)
        if reasons:
            for k in reasons:
                excluded[k] += 1
            excluded_rows.append(collections.OrderedDict(resolve_index=i, draw_ord=r['draw_ord'], status=g.get('status'),
                                                         reasons=reasons))
            continue
        dst, tl, src = g.get('destination') or {}, g.get('tiling') or {}, r.get('source') or {}
        rows.append(collections.OrderedDict(
            resolve_index=i, draw_ord=r['draw_ord'], frame=r['frame'], start=rg['extent_start'],
            end_exclusive=rg['extent_end_exclusive'], length=rg['extent_length'], raw_base=rg['raw_base'],
            adjusted_base=rg['adjusted_base'],
            destination_observed=collections.OrderedDict(
                format=dst.get('format'), format_name=dst.get('format_name'), is_depth=dst.get('is_depth'),
                bits_per_pixel=dst.get('bits_per_pixel'), tiling_mode=tl.get('mode'), slice=tl.get('slice'),
                pitch_aligned=dst.get('pitch_aligned'), height_aligned=dst.get('height_aligned'),
                width_div_8=g.get('width_div_8'), height_div_8=g.get('height_div_8'),
                source_kind=src.get('kind'), source_format_name=src.get('fmt_name'))))
    return rows, excluded, excluded_rows


def relation_consumer_components(eng, row, allow_invalid):
    """The eligible base/mip components of one normal draw row under the cvar
    filter -> (components, occurrences_total, occurrences_eligible,
    occurrence_exclusions, component_exclusions, excluded_occurrence_keys)."""
    comps, occ_excl, comp_excl, excluded_keys = [], collections.Counter(), collections.Counter(), []
    opps = row['requested_binding_opportunities']
    eligible = 0
    for o in opps:
        e = eng.fetch_constants[o['fetch_constant_key']]
        b = e['backing']
        if b is None:                                 # never derived: fail closed, never invented
            occ_excl['backing_not_join_usable'] += 1
            continue
        if not allow_invalid and b['allowed_by_cvar']:
            occ_excl['cvar_excluded'] += 1
            excluded_keys.append(collections.OrderedDict(draw_ord=row['draw_ord'], slot=o['slot'],
                                                         fetch_constant_key=o['fetch_constant_key']))
            continue
        if not b['join_usable']:
            occ_excl['backing_not_join_usable'] += 1
            continue
        eligible += 1
        for comp in ('base', 'mips'):
            iv = b[comp + '_interval']
            if iv is None:
                comp_excl['absent'] += 1
                continue
            defect = interval_defect(iv['start'], iv['end_exclusive'], iv['length'])
            if defect:
                comp_excl[defect] += 1
                continue
            comps.append(collections.OrderedDict(
                draw_ord=row['draw_ord'], slot=o['slot'], stages=list(o['stages']), fetch_constant_key=o['fetch_constant_key'],
                component=comp, start=iv['start'], end_exclusive=iv['end_exclusive'], length=iv['length']))
    return comps, len(opps), eligible, occ_excl, comp_excl, excluded_keys


def _pair_key(p, c):
    return 'resolve#%d/draw#%d>draw#%d/t%d/%s/%s' % (p['resolve_index'], p['draw_ord'], c['draw_ord'], c['slot'],
                                                     c['component'], c['fetch_constant_key'])


def resolve_consumer_relation(eng, allow_invalid=True):
    """The whole relation for one trace under one cvar setting."""
    producers, p_excluded, p_excluded_rows = relation_producers(eng)
    details = []
    per_draw = collections.OrderedDict()
    totals = collections.Counter()
    rel_total = collections.Counter()
    producers_overlap = collections.Counter()           # resolve_index -> non-disjoint pairs
    hist_producers_per_component = collections.Counter()
    hist_components_per_producer = collections.Counter()
    occ_zero_overlap = comp_zero_overlap = 0
    occ_excl_total, comp_excl_total = collections.Counter(), collections.Counter()
    cvar_excluded_keys = []
    t3c2 = dict(raw_equal=0, raw_equal_and_non_disjoint=0, raw_equal_but_disjoint=[], non_disjoint_not_raw_equal=[],
                adjusted_equal=0, adjusted_equal_and_non_disjoint=0, adjusted_equal_but_disjoint=[],
                non_disjoint_not_adjusted_equal=[])
    for row in eng.static_bindings:
        c_ord = row['draw_ord']
        earlier = [p for p in producers if temporally_eligible(p['draw_ord'], c_ord)]
        post_complete = row['post_translation']['status'] == 'complete'
        summary = collections.OrderedDict(
            relation_version=RELATION_VERSION, post_status=row['post_translation']['status'],
            eligible_earlier_producers=len(earlier), requested_occurrences=0, occurrences_eligible=0,
            occurrences_excluded=collections.OrderedDict((k, 0) for k in OCCURRENCE_EXCLUSIONS),
            components=collections.OrderedDict(base=0, mips=0),
            components_excluded=collections.OrderedDict((k, 0) for k in COMPONENT_EXCLUSIONS),
            candidate_pairs=0, relations=collections.OrderedDict((k, 0) for k in RELATIONS), non_disjoint_pairs=0,
            overlapping_producer_draw_ords=[])
        if not post_complete:
            totals['draws_post_incomplete'] += 1
            per_draw[c_ord] = summary
            continue
        totals['draws_post_complete'] += 1
        comps, occ_total, occ_elig, occ_excl, comp_excl, excl_keys = relation_consumer_components(eng, row, allow_invalid)
        cvar_excluded_keys.extend(excl_keys)
        occ_excl_total.update(occ_excl)
        comp_excl_total.update(comp_excl)
        summary['requested_occurrences'] = occ_total
        summary['occurrences_eligible'] = occ_elig
        for k in OCCURRENCE_EXCLUSIONS:
            summary['occurrences_excluded'][k] = occ_excl[k]
        for k in COMPONENT_EXCLUSIONS:
            summary['components_excluded'][k] = comp_excl[k]
        totals['occurrences_total'] += occ_total
        totals['occurrences_eligible'] += occ_elig
        rel_counts = collections.Counter()
        overlapping = set()
        occ_overlap = collections.Counter()
        for c in comps:
            summary['components'][c['component']] += 1
            totals['components_' + c['component']] += 1
            n_overlap = 0
            for p in earlier:
                rel, inter = classify_intervals(p['start'], p['end_exclusive'], c['start'], c['end_exclusive'])
                rel_counts[rel] += 1
                raw_eq = p['raw_base'] == c['start']
                adj_eq = p['start'] == c['start']
                nd = rel != 'disjoint'
                t3c2['raw_equal'] += raw_eq
                t3c2['adjusted_equal'] += adj_eq
                if raw_eq and nd:
                    t3c2['raw_equal_and_non_disjoint'] += 1
                elif raw_eq:
                    t3c2['raw_equal_but_disjoint'].append(_pair_key(p, c))
                elif nd:
                    t3c2['non_disjoint_not_raw_equal'].append(_pair_key(p, c))
                if adj_eq and nd:
                    t3c2['adjusted_equal_and_non_disjoint'] += 1
                elif adj_eq:
                    t3c2['adjusted_equal_but_disjoint'].append(_pair_key(p, c))
                elif nd:
                    t3c2['non_disjoint_not_adjusted_equal'].append(_pair_key(p, c))
                if nd:
                    n_overlap += 1
                    overlapping.add(p['draw_ord'])
                    producers_overlap[p['resolve_index']] += 1
                    occ_overlap[(c['slot'], c['fetch_constant_key'])] += 1
                    details.append(collections.OrderedDict(
                        relation=rel,
                        producer=collections.OrderedDict(resolve_index=p['resolve_index'], draw_ord=p['draw_ord'], start=p['start'],
                                                         end_exclusive=p['end_exclusive'], length=p['length'],
                                                         raw_base=p['raw_base'], adjusted_base=p['adjusted_base']),
                        consumer=collections.OrderedDict(draw_ord=c['draw_ord'], slot=c['slot'], stages=list(c['stages']),
                                                         fetch_constant_key=c['fetch_constant_key'], component=c['component'],
                                                         start=c['start'], end_exclusive=c['end_exclusive'], length=c['length']),
                        intersection=inter, ordinal_distance=c['draw_ord'] - p['draw_ord']))
            hist_producers_per_component[n_overlap] += 1
            if n_overlap == 0:
                comp_zero_overlap += 1
        seen_occ = {(c['slot'], c['fetch_constant_key']) for c in comps}
        occ_zero_overlap += sum(1 for k in seen_occ if occ_overlap[k] == 0)
        pairs = sum(rel_counts.values())
        summary['candidate_pairs'] = pairs
        for k in RELATIONS:
            summary['relations'][k] = rel_counts[k]
        summary['non_disjoint_pairs'] = sum(rel_counts[k] for k in NON_DISJOINT_RELATIONS)
        summary['overlapping_producer_draw_ords'] = sorted(overlapping)
        rel_total.update(rel_counts)
        totals['candidate_pairs'] += pairs
        per_draw[c_ord] = summary
    for p in producers:
        hist_components_per_producer[producers_overlap[p['resolve_index']]] += 1
    non_disjoint = sum(rel_total[k] for k in NON_DISJOINT_RELATIONS)
    zero_overlap_producers = [p['draw_ord'] for p in producers if producers_overlap[p['resolve_index']] == 0]
    census = collections.OrderedDict(
        candidate_pairs=totals['candidate_pairs'],
        relations=collections.OrderedDict((k, rel_total[k]) for k in RELATIONS),
        conservation=('candidate_pairs == equal + producer_contains_consumer + consumer_contains_producer + '
                      'partial_overlap + disjoint'),
        conservation_holds=(totals['candidate_pairs'] == sum(rel_total[k] for k in RELATIONS)),
        non_disjoint_pairs=non_disjoint, detail_rows=len(details), detail_rows_equal_non_disjoint=(len(details) == non_disjoint),
        producers_with_later_overlap=sum(1 for p in producers if producers_overlap[p['resolve_index']]),
        producers_with_zero_later_overlap=collections.OrderedDict(count=len(zero_overlap_producers), draw_ords=zero_overlap_producers),
        occurrences_with_zero_overlap=occ_zero_overlap, components_with_zero_overlap=comp_zero_overlap,
        multiplicity=collections.OrderedDict(
            producers_per_component=collections.OrderedDict((str(k), v) for k, v in sorted(hist_producers_per_component.items())),
            components_per_producer=collections.OrderedDict((str(k), v) for k, v in sorted(hist_components_per_producer.items())),
            max_producers_per_component=max(hist_producers_per_component) if hist_producers_per_component else 0,
            components_with_multiple_producers=sum(v for k, v in hist_producers_per_component.items() if k > 1)),
        t3c1_any_non_disjoint_pair=(non_disjoint > 0),
        t3c3_every_component_at_most_one_producer=all(k <= 1 for k in hist_producers_per_component))
    t3c2_out = collections.OrderedDict(
        claim='raw/base-address equality finds the same relationships as the full intervals',
        denominators=collections.OrderedDict(candidate_pairs=totals['candidate_pairs'], non_disjoint_pairs=non_disjoint),
        raw_base_equal=collections.OrderedDict(
            pairs=t3c2['raw_equal'], and_non_disjoint=t3c2['raw_equal_and_non_disjoint'],
            false_positives_equal_but_disjoint=collections.OrderedDict(count=len(t3c2['raw_equal_but_disjoint']), pairs=t3c2['raw_equal_but_disjoint']),
            false_negatives_non_disjoint_not_equal=collections.OrderedDict(count=len(t3c2['non_disjoint_not_raw_equal']), pairs=t3c2['non_disjoint_not_raw_equal']),
            equals_full_relation=(not t3c2['raw_equal_but_disjoint'] and not t3c2['non_disjoint_not_raw_equal'])),
        adjusted_start_equal=collections.OrderedDict(
            pairs=t3c2['adjusted_equal'], and_non_disjoint=t3c2['adjusted_equal_and_non_disjoint'],
            false_positives_equal_but_disjoint=collections.OrderedDict(count=len(t3c2['adjusted_equal_but_disjoint']), pairs=t3c2['adjusted_equal_but_disjoint']),
            false_negatives_non_disjoint_not_equal=collections.OrderedDict(count=len(t3c2['non_disjoint_not_adjusted_equal']), pairs=t3c2['non_disjoint_not_adjusted_equal']),
            equals_full_relation=(not t3c2['adjusted_equal_but_disjoint'] and not t3c2['non_disjoint_not_adjusted_equal'])))
    consumers = collections.OrderedDict(
        normal_draws=len(eng.static_bindings), draws_post_complete=totals['draws_post_complete'],
        draws_post_incomplete=totals['draws_post_incomplete'], requested_occurrences=totals['occurrences_total'],
        occurrences_eligible=totals['occurrences_eligible'],
        occurrences_excluded=collections.OrderedDict((k, occ_excl_total[k]) for k in OCCURRENCE_EXCLUSIONS),
        components=collections.OrderedDict(base=totals['components_base'], mips=totals['components_mips'],
                                           total=totals['components_base'] + totals['components_mips']),
        components_excluded=collections.OrderedDict((k, comp_excl_total[k]) for k in COMPONENT_EXCLUSIONS))
    return collections.OrderedDict(
        allow_invalid_fetch_constants=allow_invalid,
        producers=collections.OrderedDict(resolves_total=len(eng.resolves), eligible=len(producers),
                                          excluded_rows_total=len(p_excluded_rows),
                                          excluded_checks=collections.OrderedDict((k, p_excluded[k]) for k in PRODUCER_EXCLUSIONS),
                                          excluded_rows=p_excluded_rows, rows=producers),
        consumers=consumers, census=census, t3c2=t3c2_out, per_draw=per_draw, details=details,
        cvar_excluded_occurrences=cvar_excluded_keys)


def relation_hash(rel):
    """Normalized SHA-256 of the sorted detail rows plus the relation histogram (trace-local content only)."""
    lines = sorted(json.dumps(d, sort_keys=True, separators=(',', ':')) for d in rel['details'])
    lines.append(json.dumps(rel['census']['relations'], sort_keys=True, separators=(',', ':')))
    return hashlib.sha256('\n'.join(lines).encode()).hexdigest()


def relation_for(eng, allow_invalid=True):
    """Cached relation per cvar setting."""
    cache = getattr(eng, '_relation_cache', None)
    if cache is None:
        cache = eng._relation_cache = {}
    if allow_invalid not in cache:
        cache[allow_invalid] = resolve_consumer_relation(eng, allow_invalid)
    return cache[allow_invalid]


def relation_report(eng):
    """The /7 top-level object: the default-true relation with details, the compact true/false matrix."""
    rt, rf = relation_for(eng, True), relation_for(eng, False)

    def matrix_entry(rel):
        return collections.OrderedDict(
            allow_invalid_fetch_constants=rel['allow_invalid_fetch_constants'],
            occurrences_excluded_by_cvar=rel['consumers']['occurrences_excluded']['cvar_excluded'],
            occurrences_eligible=rel['consumers']['occurrences_eligible'], components=rel['consumers']['components']['total'],
            candidate_pairs=rel['census']['candidate_pairs'], relations=rel['census']['relations'],
            detail_rows=rel['census']['detail_rows'], normalized_hash=relation_hash(rel))
    mt, mf = matrix_entry(rt), matrix_entry(rf)
    same = all(mt[k] == mf[k] for k in ('occurrences_eligible', 'components', 'candidate_pairs', 'relations', 'detail_rows',
                                        'normalized_hash'))
    matrix = collections.OrderedDict([('true', mt), ('false', mf), ('same_as_default', same)])
    if same:
        matrix['differences'] = None
    else:
        matrix['differences'] = collections.OrderedDict(
            changed_counts=collections.OrderedDict((k, [mt[k], mf[k]]) for k in ('occurrences_eligible', 'components', 'candidate_pairs',
                                                                                 'detail_rows') if mt[k] != mf[k]),
            changed_relations=collections.OrderedDict((k, [mt['relations'][k], mf['relations'][k]]) for k in RELATIONS
                                                      if mt['relations'][k] != mf['relations'][k]),
            affected_occurrences=rf['cvar_excluded_occurrences'])
    return collections.OrderedDict(
        relation_version=RELATION_VERSION, meaning=RELATION_MEANING, definitions=RELATION_DEFINITIONS,
        canonical_cvar=collections.OrderedDict(gpu_allow_invalid_fetch_constants=True),
        producers=rt['producers'], consumers=rt['consumers'], census=rt['census'], t3c2=rt['t3c2'],
        details=rt['details'], cvar_matrix=matrix)


def relation_line(d):
    p, c, i = d['producer'], d['consumer'], d['intersection']
    return ('resolve#%d draw#%d [%08X,%08X) +%d -> draw#%d t%d %s %s [%08X,%08X) +%d : %s inter=[%08X,%08X) +%d dist=%d'
            % (p['resolve_index'], p['draw_ord'], p['start'], p['end_exclusive'], p['length'], c['draw_ord'], c['slot'],
               c['stages'], c['component'], c['start'], c['end_exclusive'], c['length'], d['relation'], i['start'],
               i['end_exclusive'], i['length'], d['ordinal_distance']))


# ── the PM4 engine ───────────────────────────────────────────────────────────

class Engine:
    def __init__(self, hash_shaders=True, post_table=None):
        self.mem = Memory()
        self.regs = array.array('I', bytes(4 * MAX_REGS))
        self.hash_shaders = hash_shaders
        self.vs = b''
        self.ps = b''
        self.vs_hash = None
        self.ps_hash = None
        self.ucodes = {}
        self.frame = 0
        self.draws = []
        self.resolves = []
        self.opcodes = collections.Counter()
        self.unknown_ops = collections.Counter()
        self.event_initiators = collections.Counter()
        self.packets_executed = 0
        self.ib_dispatch_records = 0
        self.ib_depth = 0
        self.ib_spans = 0
        self.leading_ib_ends = 0
        self.ib_wrapper_ends = 0
        self.screen_extent_events = 0
        self.zero_index_draws = 0
        self.vs_loads = 0
        self.ps_loads = 0
        self.unmapped_reads = 0
        self.resolve_input_counts = collections.Counter()   # complete / rejected / incomplete
        self.range_status_counts = collections.Counter()   # TOOL-2B status census
        self.vs_tex = None            # TOOL-3A: analysis of the VS ucode bound now (dict or None)
        self.ps_tex = None            # TOOL-3A: analysis of the PS ucode bound now
        self.tex_analyses = {}        # ucode hash -> analysis (hashed loads only)
        self.static_bindings = []     # one row per NORMAL draw
        self.post_table = post_table  # TOOL-3B: recorded actual-translator masks (or None)
        self.shader_table = {}        # analysis key (ucode sha256[-unmapped]) -> normalized shader entry
        self.fetch_constants = {}     # 48-hex raw key -> normalized fetch constant entry
        self.pending_reads = None    # PacketLocalReads of the packet awaiting its End (parser-owned)
        self.packet_reads = None     # PacketLocalReads of the packet being executed (execute-owned)
        self.unmapped_bytes = 0
        self.mem_writes_pm4 = 0
        self.warnings = []
        self.warning_counts = collections.Counter()
        # XT1 census, per VS load event (the same unit the C++ [VFETCH]
        # census counts), so the numbers join against gpu_trace_replay.cpp.
        self.vf = dict(loads=0, parsed=0, failed=0, bindings=0, attrs=0,
                       must_be_one_violations=0, fmt=collections.Counter(),
                       naive_divergent_loads=0, naive_extra=0, naive_missing=0,
                       naive_extra_fmt=collections.Counter())

    # ── bookkeeping ──
    def warn(self, key, text):
        self.warning_counts[key] += 1
        if len(self.warnings) < MAX_WARNINGS:
            self.warnings.append(text)

    def write_reg(self, idx, val):
        if 0 <= idx < MAX_REGS:
            self.regs[idx] = val & 0xFFFFFFFF

    def rd(self, idx):
        return self.regs[idx]

    def mem_read(self, addr, n, what, offset):
        data, missing = self.mem.read(addr, n)
        if missing:
            self.unmapped_reads += 1
            self.unmapped_bytes += missing
            self.warn('unmapped_read', 'record@%d: %s read %d of %d bytes from '
                      'memory the trace never supplied (addr %08X)'
                      % (offset, what, missing, n, addr & PHYS_MASK))
        return data, missing

    def shader_load(self, is_vertex, ucode, missing, offset):
        if is_vertex:
            self.vs_loads += 1
        else:
            self.ps_loads += 1
        h = None
        if missing:
            self.warn('shader_unmapped', 'record@%d: %s shader ucode partly '
                      'unmapped (%d bytes) - not hashed'
                      % (offset, 'VS' if is_vertex else 'PS', missing))
        elif self.hash_shaders:
            h = hash_ucode(ucode, offset)
            self.ucodes[h] = ucode
        stage = 'vs' if is_vertex else 'ps'
        sha = hashlib.sha256(ucode).hexdigest()
        key = sha + ('-unmapped' if missing else '')
        entry = self.shader_table.get(key)
        if entry is None:
            analysis = analyze_texture_fetches(ucode)
            if missing:
                analysis['status'] = 'incomplete'
                analysis['reason'] = 'ucode partly unmapped (%d bytes): the walk saw a truncated program' % missing
            entry = collections.OrderedDict(analysis_key=key, ucode_sha256=sha,
                                            ucode_xxh3_64=('%016X' % h) if h is not None else None,
                                            ucode_bytes=len(ucode), unmapped_bytes=missing, stages_seen=[],
                                            analysis=analysis, post_translation=collections.OrderedDict())
            self.shader_table[key] = entry
        if h is not None and h in self.tex_analyses:
            analysis = self.tex_analyses[h]
        else:
            analysis = entry['analysis']
            if h is not None:
                self.tex_analyses[h] = analysis
        if stage not in entry['stages_seen']:
            entry['stages_seen'].append(stage)
        if stage not in entry['post_translation']:
            entry['post_translation'][stage] = post_translation_for(self.post_table, stage, sha, analysis['binding_mask'],
                                                                     missing)
        cur = dict(hash=h, analysis=analysis, key=key, post=entry['post_translation'][stage])
        if is_vertex:
            self.vs, self.vs_hash = ucode, h
            self.vs_tex = cur
            self._census_vs(ucode)
        else:
            self.ps, self.ps_hash = ucode, h
            self.ps_tex = cur

    def _fetch_constant_entry(self, raw):
        """The normalized fetch-constant entry for one exact raw value (created once)."""
        k = fetch_constant_key(raw)
        e = self.fetch_constants.get(k)
        if e is None:
            e = collections.OrderedDict(key=k, raw=[int(x) for x in raw], decoded=decode_tfetch(raw), requested=False,
                                        backing=None, backing_reason='not requested by any post-translation mask')
            self.fetch_constants[k] = e
        return e

    def _static_binding_row(self, d):
        """TOOL-3A/3B: the static texture binding row of one NORMAL draw from the
        VS/PS analyses bound at the draw and the fetch constants at the draw,
        normalized: stages reference static_texture_shaders, bindings reference
        texture_fetch_constants. Never falls back to the type-2 constants: a
        missing or incomplete stage stays named. The post-translation block
        marks the REQUESTED slots and derives backing only for those."""
        stages = collections.OrderedDict()
        post = collections.OrderedDict()
        for name, cur in (('vs', self.vs_tex), ('ps', self.ps_tex)):
            if cur is None:
                stages[name] = collections.OrderedDict(
                    status='missing', reason='no %s shader loaded before this draw' % ('vertex' if name == 'vs' else 'pixel'),
                    ucode_hash=None, analysis_key=None, binding_mask=0)
                post[name] = collections.OrderedDict(status='missing', mask=None)
                continue
            a = cur['analysis']
            stages[name] = collections.OrderedDict(status=a['status'], reason=a['reason'], ucode_hash=cur['hash'],
                                                   analysis_key=cur['key'], binding_mask=a['binding_mask'])
            # per-stage post status and mask per row; reason and the binding list live once in static_texture_shaders
            post[name] = collections.OrderedDict(status=cur['post']['status'], mask=cur['post']['mask'])
        vs_slots, ps_slots = set(mask_slots(stages['vs']['binding_mask'])), set(mask_slots(stages['ps']['binding_mask']))
        union = sorted(vs_slots | ps_slots)
        fetch = d['fetch']
        type2 = [s_ for s_ in range(32) if (fetch[s_ * 6] & 3) == 2]
        both_post = post['vs']['status'] == 'translated' and post['ps']['status'] == 'translated'
        rvs = set(mask_slots(post['vs']['mask'])) if both_post else set()
        rps = set(mask_slots(post['ps']['mask'])) if both_post else set()
        requested = sorted(rvs | rps)
        post['status'] = 'complete' if both_post else 'incomplete'
        post['requested_union'] = requested
        post['requested_overlap'] = sorted(rvs & rps)
        post['requested_equals_analysis_union'] = (requested == union) if both_post else None
        post['requested_minus_analysis'] = sorted(set(requested) - set(union))
        post['analysis_minus_requested'] = sorted(set(union) - set(requested)) if both_post else []
        bindings = []
        opportunities = []
        for slot in sorted(set(union) | set(requested)):
            raw = [int(x) for x in fetch[slot * 6:slot * 6 + 6]]
            e = self._fetch_constant_entry(raw)
            in_stages = [n for n in ('vs', 'ps') if slot in (vs_slots if n == 'vs' else ps_slots)]
            req_stages = [n for n in ('vs', 'ps') if slot in (rvs if n == 'vs' else rps)]
            is_req = slot in requested
            bindings.append(collections.OrderedDict(slot=slot, stages=in_stages, fetch_constant_key=e['key'],
                                                    is_texture_constant=e['decoded']['type'] == 2,
                                                    requested_stages=req_stages))
            if is_req:
                if not e['requested']:
                    e['requested'] = True
                    e['backing'] = derive_texture_backing(raw)
                    e['backing_reason'] = None
                opportunities.append(collections.OrderedDict(
                    slot=slot, stages=req_stages, fetch_constant_key=e['key'], backing_status=e['backing']['status'],
                    join_usable=e['backing']['join_usable']))
        both = stages['vs']['status'] == 'complete' and stages['ps']['status'] == 'complete'
        return collections.OrderedDict(
            draw_ord=d['ord'], frame=d['frame'], mode=d['mode'], vs=d['vs'], ps=d['ps'],
            analysis_status='complete' if both else 'incomplete',
            stages=stages, union_slots=union, overlap_slots=sorted(vs_slots & ps_slots),
            bindings=[b for b in bindings if b['slot'] in union], type2_slots=type2,
            unused_type2_slots=sorted(set(type2) - set(union)),
            referenced_non_texture_slots=[b['slot'] for b in bindings if b['slot'] in union and not b['is_texture_constant']],
            post_translation=post, requested_binding_opportunities=opportunities)

    def _census_vs(self, ucode):
        vf = self.vf
        vf['loads'] += 1
        cf = parse_vs_vfetch(ucode)
        if cf['ok']:
            vf['parsed'] += 1
        else:
            vf['failed'] += 1
        vf['must_be_one_violations'] += cf['must_be_one_violations']
        vf['bindings'] += len(cf['bindings'])
        for a in cf['attrs']:
            vf['attrs'] += 1
            vf['fmt'][a['fmt']] += 1
        naive = parse_vs_vfetch_naive(ucode)
        naive_slots = {h['slot'] for h in naive}
        extra = naive_slots - cf['vfetch_slots']
        missing = cf['vfetch_slots'] - naive_slots
        if extra or missing:
            vf['naive_divergent_loads'] += 1
        vf['naive_extra'] += len(extra)
        vf['naive_missing'] += len(missing)
        for h in naive:
            if h['slot'] in extra:
                vf['naive_extra_fmt'][h['fmt']] += 1

    # ── packets ──
    def execute(self, data, offset, packet_reads=None):
        """Execute one PacketStart record body. `offset` is the record's byte
        offset in the trace, carried into every refusal and warning.
        `packet_reads` is the PacketLocalReads of THIS packet's interval; it
        is visible only while this body executes."""
        self.packet_reads = packet_reads
        try:
            self._execute(data, offset)
        finally:
            self.packet_reads = None

    def _execute(self, data, offset):
        n = len(data) & ~3
        off = 0
        while off + 4 <= n:
            hdr = struct.unpack_from('>I', data, off)[0]
            typ = hdr >> 30
            if typ == 0:
                cnt = ((hdr >> 16) & 0x3FFF) + 1
                if off + 4 + 4 * cnt > n:
                    raise TraceError(EXIT_MALFORMED, offset,
                                     'type-0 packet at +%d needs %d dwords, record holds %d'
                                     % (off, cnt, (n - off - 4) // 4))
                base, one = hdr & 0x7FFF, (hdr >> 15) & 1
                vals = struct.unpack_from('>%dI' % cnt, data, off + 4)
                for i, v in enumerate(vals):
                    self.write_reg(base if one else base + i, v)
                off += 4 + 4 * cnt
            elif typ == 1:
                if off + 12 > n:
                    raise TraceError(EXIT_MALFORMED, offset,
                                     'type-1 packet at +%d needs 2 dwords' % off)
                v1, v2 = struct.unpack_from('>II', data, off + 4)
                self.write_reg(hdr & 0x7FF, v1)
                self.write_reg((hdr >> 11) & 0x7FF, v2)
                off += 12
            elif typ == 2:
                off += 4
            else:
                op = (hdr >> 8) & 0x7F
                cnt = ((hdr >> 16) & 0x3FFF) + 1
                if off + 4 + 4 * cnt > n:
                    # pm4_command_processor_implement.h L401-L403: the writer
                    # records an INDIRECT_BUFFER dispatch as header + list_ptr
                    # ONLY; the inner packets follow as their own records
                    # inside an IndirectBufferStart/End span. It is a marker,
                    # never something to execute from memory.
                    if op == OP_INDIRECT_BUFFER and off == 0:
                        self.ib_dispatch_records += 1
                        self.opcodes[op] += 1
                        return
                    raise TraceError(EXIT_MALFORMED, offset,
                                     'type-3 packet op=0x%02X at +%d needs %d dwords, '
                                     'record holds %d' % (op, off, cnt, (n - off - 4) // 4))
                body = data[off + 4: off + 4 + 4 * cnt]
                off += 4 + 4 * cnt
                self.opcodes[op] += 1
                self.type3(op, cnt, body, offset)
            self.packets_executed += 1

    def type3(self, op, cnt, body, offset):
        w = be32s(body)
        if op == OP_SET_CONSTANT:                     # L1191
            ot = w[0]
            idx, ct = ot & 0x7FF, (ot >> 16) & 0xFF
            base = CONSTANT_BASES.get(ct)
            if base is None:
                self.warn('set_constant_type', 'record@%d: SET_CONSTANT type %u ignored' % (offset, ct))
                return
            for i in range(1, cnt):
                self.write_reg(base + idx + i - 1, w[i])
        elif op in (OP_SET_CONSTANT2, OP_SET_SHADER_CONSTANTS):   # L1222 / L1300
            idx = w[0] & 0xFFFF
            for i in range(1, cnt):
                self.write_reg(idx + i - 1, w[i])
        elif op == OP_LOAD_ALU_CONSTANT:              # L1245
            if cnt < 3:
                return
            src, ot, size = w[0] & 0x3FFFFFFF, w[1], w[2] & 0xFFF
            idx, ct = ot & 0x7FF, (ot >> 16) & 0xFF
            base = CONSTANT_BASES.get(ct)
            if base is None:
                self.warn('load_alu_type', 'record@%d: LOAD_ALU_CONSTANT type %u ignored' % (offset, ct))
                return
            data, _missing = self.mem_read(src, size * 4, 'LOAD_ALU_CONSTANT', offset)
            for i, v in enumerate(be32s(data)):
                self.write_reg(base + idx + i, v)
        elif op == OP_REG_RMW:                        # L758
            if cnt < 3:
                return
            info, am, om = w[0], w[1], w[2]
            reg = info & 0x1FFF
            v = self.rd(reg)
            v &= self.rd(am & 0x1FFF) if (info >> 31) & 1 else am
            v |= self.rd(om & 0x1FFF) if (info >> 30) & 1 else om
            self.write_reg(reg, v)
        elif op == OP_IM_LOAD:                        # L1308
            if cnt < 2:
                return
            addr_type, start_size = w[0], w[1]
            size = start_size & 0xFFFF
            data, missing = self.mem_read(addr_type & ~3, size * 4, 'IM_LOAD', offset)
            self.shader_load((addr_type & 3) == 0, data, missing, offset)
        elif op == OP_IM_LOAD_IMMEDIATE:              # L1338
            if cnt < 2:
                return
            st, size = w[0] & 3, w[1] & 0xFFFF
            if 2 + size > cnt:
                raise TraceError(EXIT_MALFORMED, offset,
                                 'IM_LOAD_IMMEDIATE declares %d dwords but the packet holds %d'
                                 % (size, cnt - 2))
            self.shader_load(st == 0, bytes(body[8:8 + size * 4]), 0, offset)
        elif op == OP_DRAW_INDX:                      # L1165: viz dword, then the draw
            if cnt < 2:
                return
            self.draw(op, w[1], w[2:4] if cnt >= 4 else (), offset)
        elif op == OP_DRAW_INDX_2:                    # L1182
            if cnt < 1:
                return
            self.draw(op, w[0], w[1:3] if cnt >= 3 else (), offset)
        elif op in (OP_DRAW_INDX_BIN, OP_DRAW_INDX_2_BIN):
            self.warn('bin_draw', 'record@%d: binned draw op=0x%02X not decoded' % (offset, op))
        elif op in (OP_INDIRECT_BUFFER, OP_INDIRECT_BUFFER_PFD):
            # Reached only when a dispatch packet's End arrives with no
            # IndirectBufferStart in between (the writer always emits one, so
            # this is a defensive count). Its inner packets arrive as their own
            # records; executing it from memory would run them twice.
            self.ib_dispatch_records += 1   # dispatch counted, not executed
        elif op in (OP_EVENT_WRITE, OP_EVENT_WRITE_SHD):   # L867: initiator only
            if cnt >= 1:
                self.write_reg(REG_VGT_EVENT_INITIATOR, w[0] & 0x3F)
                self.event_initiators[w[0] & 0x3F] += 1
        elif op == OP_EVENT_WRITE_EXT:                # L921: screen extents, NOT a resolve
            if cnt >= 1:
                self.write_reg(REG_VGT_EVENT_INITIATOR, w[0] & 0x3F)
            self.screen_extent_events += 1
        elif op == OP_MEM_WRITE:
            self.mem_writes_pm4 += 1
        elif op in IGNORED_OPS:
            pass
        else:
            self.unknown_ops[op] += 1

    # ── draws ──
    def draw(self, op, init, dma, offset):
        """ExecutePacketType3Draw (L1049) + the backend's mode dispatch
        (d3d12_command_processor.cc L2515): mode kCopy => resolve."""
        self.write_reg(REG_VGT_DRAW_INITIATOR, init)
        src_sel = (init >> 6) & 3
        dma_base, dma_size = 0, 0
        if src_sel == 0 and len(dma) >= 2:
            dma_base, dma_size = dma[0], dma[1]
            self.write_reg(REG_VGT_DMA_BASE, dma_base)
            self.write_reg(REG_VGT_DMA_SIZE, dma_size)
        idx_count = init >> 16
        if idx_count == 0:
            self.zero_index_draws += 1
        mode = self.rd(REG_RB_MODECONTROL) & 7
        if mode == MODE_COPY:
            kind = 'resolve'
        elif mode in (MODE_COLOR_DEPTH, MODE_DEPTH_ONLY):
            kind = 'draw'
        elif mode == MODE_NOOP:
            kind = 'noop'
        else:
            kind = 'unknown_mode'
        d = dict(
            ord=len(self.draws) + 1, frame=self.frame, op=op, kind=kind, mode=mode,
            prim=init & 0x3F, src_sel=src_sel, idx_type=(init >> 11) & 1,
            count=idx_count, dma_base=dma_base & PHYS_MASK, dma_size=dma_size,
            vs=self.vs_hash, ps=self.ps_hash, vs_bytes=len(self.vs),
            ps_bytes=len(self.ps), in_ib=self.ib_depth > 0, record_offset=offset,
            mem_seq=self.mem.seq,
            regs={r: self.rd(r) for r in DRAW_REGS},
            vs_consts=self.regs[ALU_BASE:ALU_BASE + 0x400],
            ps_consts=self.regs[ALU_BASE + 0x400:ALU_BASE + 0x800],
            fetch=self.regs[FETCH_BASE:FETCH_BASE + 32 * 6],
            bools=self.regs[BOOL_BASE:BOOL_BASE + 8],
            loops=self.regs[LOOP_BASE:LOOP_BASE + 32],
        )
        self.draws.append(d)
        if kind == 'resolve':
            self.resolves.append(self._resolve_record(d))
        elif kind == 'draw':
            self.static_bindings.append(self._static_binding_row(d))

    def _resolve_record(self, d):
        copy_control = self.rd(REG_RB_COPY_CONTROL)
        copy = decode_copy_control(copy_control)
        r = dict(draw_ord=d['ord'] if d else None, frame=self.frame,
                 copy_control=copy_control,
                 dest_base=self.rd(REG_RB_COPY_DEST_BASE),
                 dest_pitch=self.rd(REG_RB_COPY_DEST_PITCH),
                 dest_info=self.rd(REG_RB_COPY_DEST_INFO),
                 surface_info=self.rd(REG_RB_SURFACE_INFO),
                 rt_info=[self.rd(reg) for reg in RT_INFO_REGS],
                 depth_info=self.rd(REG_RB_DEPTH_INFO))
        r['copy'] = copy
        r['dest'] = decode_copy_dest_info(r['dest_info'])
        r['dest_pitch_decoded'] = decode_copy_dest_pitch(r['dest_pitch'])
        # the SELECTED source, not RT0: draw_util.cc GetResolveInfo
        r['source'] = resolve_source(copy['copy_src_select'], self.rd)
        # TOOL-2A: the trace-resident GetResolveInfo inputs, captured now
        r['get_resolve_info_inputs'] = self._resolve_inputs(d['record_offset'] if d else None)
        # TOOL-2B: the pinned range derivation from the captured inputs only
        r['guest_destination_range'] = derive_for_row(r)
        self.range_status_counts[r['guest_destination_range']['status']] += 1
        return r

    def _resolve_inputs(self, offset):
        """Lossless capture of GetResolveInfo's trace-resident inputs at THIS
        draw (draw_util.cc L953-L1016, L1301-L1303; GetScissor L551-L724).
        The vertex payload is read from guest memory as it stands when the
        enclosing PacketEnd executes the draw - the writer's uncached
        MemoryRead of exactly these bytes (L959) precedes that PacketEnd."""
        d0, d1 = self.rd(FETCH_BASE), self.rd(FETCH_BASE + 1)   # GetVertexFetch(0), register_file.h L59-L67
        ftype, addr_dw = d0 & 3, d0 >> 2                        # xenos.h L1164-L1165
        endian, size_dw = d1 & 3, (d1 >> 2) & 0xFFFFFF          # xenos.h L1167-L1168
        vf = dict(raw=[d0, d1], type=ftype, type_name=FETCH_TYPE_NAMES.get(ftype, '?'),
                  address_dwords=addr_dw, address_bytes=(addr_dw * 4) & PHYS_MASK,
                  size_dwords=size_dw, size_bytes=size_dw * 4, endian=endian)
        rejected = []
        if ftype != FETCH_TYPE_VERTEX:
            rejected.append('type')
        if size_dw != RESOLVE_VERTEX_DWORDS:
            rejected.append('size')
        out = dict(draw_packet_record_offset=offset, vertex_fetch_0=vf,
                   registers={name: self.rd(reg) for name, reg in RESOLVE_INPUT_REGS},
                   not_in_trace=list(RESOLVE_INPUTS_NOT_IN_TRACE))
        if rejected:
            # draw_util.cc L954-L958: GetResolveInfo returns false - Xenia does
            # not perform this copy. No payload is read and none is invented.
            out.update(status='rejected',
                       rejection='GetResolveInfo rejects vertex fetch 0: bad ' + ' and '.join(rejected)
                                 + ' (draw_util.cc L954-L958)',
                       vertex_payload=dict(status='not_applicable', bytes_hex=None, dwords_be=None,
                                           missing_bytes=None))
            self.resolve_input_counts['rejected'] += 1
            return out
        # THE PROOF (r1): only decoded MemoryRead payloads recorded inside this
        # draw's own PacketStart/PacketEnd interval count - Xenia writes exactly
        # that read at L959. The trace-wide image, the packet body write and
        # MemoryWrite records never satisfy it, so a reused address with no (or
        # a partial) current read is INCOMPLETE, never a stale or mixed payload.
        local = self.packet_reads if self.packet_reads is not None else PacketLocalReads()
        data, missing = local.extract(vf['address_bytes'], RESOLVE_VERTEX_DWORDS * 4)
        # global accounting is unchanged: a payload the trace never supplied
        # anywhere is still an unmapped read, counted and warned as before
        _image, image_missing = self.mem_read(vf['address_bytes'], RESOLVE_VERTEX_DWORDS * 4,
                                              'resolve vertex fetch 0', offset)
        if missing and not image_missing:
            self.warn('resolve_payload_not_in_packet',
                      'record@%d: resolve vertex fetch 0: %d of %d bytes were not supplied by a '
                      'MemoryRead inside this draw packet interval (present only in the trace-wide '
                      'image) - INCOMPLETE' % (offset, missing, RESOLVE_VERTEX_DWORDS * 4))
        if missing:
            # never a zero-filled rectangle: the later join must refuse this row
            out.update(status='incomplete', rejection=None,
                       vertex_payload=dict(status='incomplete', bytes_hex=None, dwords_be=None,
                                           missing_bytes=missing))
            self.resolve_input_counts['incomplete'] += 1
        else:
            out.update(status='complete', rejection=None,
                       vertex_payload=dict(status='complete', bytes_hex=data.hex().upper(),
                                           dwords_be=list(be32s(data)), missing_bytes=0))
            self.resolve_input_counts['complete'] += 1
        return out

    def vfetch(self, slot):
        b = FETCH_BASE + slot * 2
        d0, d1 = self.rd(b), self.rd(b + 1)
        return dict(slot=slot, type=d0 & 3, addr=(d0 & ~3) & PHYS_MASK,
                    endian=d1 & 3, size=((d1 >> 2) & 0xFFFFFF) * 4)


# ── the fail-closed record reader ────────────────────────────────────────────

class Reader:
    """Exact-length reads. A refusal names the offset of the RECORD being
    read (`anchor`, set by the parser at each record) and the exact position
    of the short read in its reason."""

    def __init__(self, f, size):
        self.f, self.size = f, size
        self.anchor = 0

    def tell(self):
        return self.f.tell()

    def exact(self, n, what):
        pos = self.f.tell()
        if n < 0 or pos + n > self.size:
            raise TraceError(EXIT_MALFORMED, self.anchor, '%s: needs %d bytes at %d, %d remain'
                             % (what, n, pos, self.size - pos))
        b = self.f.read(n)
        if len(b) != n:
            raise TraceError(EXIT_MALFORMED, self.anchor, '%s: short read %d of %d bytes at %d'
                             % (what, len(b), n, pos))
        return b

    def u32s(self, n, what):
        return struct.unpack('<%dI' % n, self.exact(4 * n, what))


def ib_dispatch_opcode(data):
    """pm4_command_processor_implement.h L398-L404 + L434-L435: an
    INDIRECT_BUFFER (0x3F) dispatch is recorded as header + list_ptr only (two
    dwords of a three-dword packet); INDIRECT_BUFFER_PFD (0x37) is recorded in
    full. Both run ExecuteIndirectBuffer, so both are followed by an
    IndirectBufferStart span. Returns the opcode or None."""
    if len(data) < 4:
        return None
    hdr = struct.unpack_from('>I', data, 0)[0]
    if (hdr >> 30) != 3:
        return None
    op = (hdr >> 8) & 0x7F
    return op if op in (OP_INDIRECT_BUFFER, OP_INDIRECT_BUFFER_PFD) else None


def decode_payload(enc, payload, decoded_length, offset, what):
    """Exactly `decoded_length` bytes or a named refusal."""
    if enc == ENC_NONE:
        if len(payload) != decoded_length:
            raise TraceError(EXIT_MALFORMED, offset,
                             '%s: raw payload is %d bytes but the record claims %d'
                             % (what, len(payload), decoded_length))
        return payload
    if enc == ENC_SNAPPY:
        if cramjam is None:
            raise TraceError(EXIT_DEPENDENCY, offset,
                             '%s: snappy payload needs the cramjam package (pip install '
                             'cramjam); refusing rather than skipping the data' % what)
        try:
            out = bytes(cramjam.snappy.decompress_raw(payload))
        except Exception as e:  # cramjam raises its own error types
            raise TraceError(EXIT_MALFORMED, offset, '%s: snappy decode failed: %s' % (what, e))
        if len(out) != decoded_length:
            raise TraceError(EXIT_MALFORMED, offset,
                             '%s: snappy decoded %d bytes but the record claims %d'
                             % (what, len(out), decoded_length))
        return out
    raise TraceError(EXIT_UNSUPPORTED, offset, '%s: unknown memory encoding %d' % (what, enc))


def parse_header(r):
    off = r.tell()
    hdr = r.exact(HEADER_BYTES, 'trace header')
    ver, sha, title = struct.unpack('<I40sI', hdr)
    if ver != TRACE_VERSION:
        raise TraceError(EXIT_UNSUPPORTED, off,
                         'trace format version %d; this reader knows version %d '
                         '(a big-endian header reads as %d)' % (ver, TRACE_VERSION, 1 << 24))
    sha_s = sha.split(b'\0', 1)[0].decode('ascii', 'replace')
    return dict(version=ver, sha=sha_s, title=title)


class ParseStats:
    def __init__(self):
        self.records = collections.Counter()
        self.packet_records = 0
        self.mem_read_records = 0
        self.mem_read_bytes = 0
        self.mem_write_records = 0
        self.mem_write_bytes = 0
        self.registers_records = 0
        self.register_values = 0
        self.edram_snapshots = 0
        self.gamma_ramps = 0
        self.primary_spans = 0
        self.swaps = 0
        self.stopped_at_frame_limit = False
        self.bytes_parsed = 0


def parse(path, max_frames=0, hash_shaders=True, post_table=None):
    """Parse a trace. Returns (engine, header, stats). Raises TraceError."""
    if hash_shaders and xxhash is None:
        raise TraceError(EXIT_DEPENDENCY, 0,
                         'shader hashing needs the xxhash package (pip install xxhash); '
                         'pass --no-shader-hash to read the trace with hashes reported as null')
    eng = Engine(hash_shaders=hash_shaders, post_table=post_table)
    st = ParseStats()
    size = os.path.getsize(path)
    with open(path, 'rb') as f:
        r = Reader(f, size)
        hdr = parse_header(r)
        pending = None          # (data, offset) of the PacketStart awaiting its End
        wrapper_ends_owed = 0   # PacketEnds of INDIRECT_BUFFER dispatch packets
        while r.tell() < size:
            off = r.tell()
            r.anchor = off
            (tag,) = r.u32s(1, 'record tag')
            st.records[tag] += 1
            if tag in (K_PRIM_START, K_IB_START):
                base, count = r.u32s(2, 'buffer start')
                if count:
                    # trace_reader.cc: the writer records count=0 here; a
                    # nonzero count would be a format we do not know.
                    raise TraceError(EXIT_UNSUPPORTED, off,
                                     'buffer-start record with count=%d (writer emits 0)' % count)
                if tag == K_IB_START:
                    # pm4 L393-L406 + trace_reader.cc L121: the dispatch packet
                    # (header + list_ptr, recorded SHORT) is pending here, its
                    # inner packets follow as their own records, and its own
                    # PacketEnd comes after the IndirectBufferEnd. Consume the
                    # dispatch now, owe its End, never execute it from memory.
                    if pending is not None:
                        dispatch_op = ib_dispatch_opcode(pending[0])
                        if dispatch_op is None:
                            raise TraceError(EXIT_MALFORMED, off,
                                             'IndirectBufferStart while a non-INDIRECT_BUFFER '
                                             'packet (record@%d) is pending' % pending[1])
                        eng.ib_dispatch_records += 1
                        eng.opcodes[dispatch_op] += 1
                        pending = None   # consumed: never executed, never re-read from memory
                        eng.pending_reads = None   # the dispatch packet's interval is discarded
                        wrapper_ends_owed += 1
                    else:
                        eng.warn('ib_start_without_dispatch',
                                 'record@%d: IndirectBufferStart with no pending dispatch packet' % off)
                    eng.ib_depth += 1
                    eng.ib_spans += 1
                else:
                    st.primary_spans += 1
            elif tag == K_PRIM_END:
                pass
            elif tag == K_IB_END:
                if eng.ib_depth == 0:
                    # ExecuteIndirectBuffer (L12/L36) is balanced per call, but
                    # the writer opens the file at a swap, and the swap packet
                    # may lie INSIDE an indirect buffer: the title trace opens
                    # with 55 packets and then this End. Before any Start it is
                    # the mid-IB opening and is counted; after a Start it is a
                    # broken trace.
                    if eng.ib_spans:
                        raise TraceError(EXIT_MALFORMED, off,
                                         'IndirectBufferEnd without a matching start')
                    eng.leading_ib_ends += 1
                    wrapper_ends_owed += 1   # its dispatch packet predates the trace
                    eng.warn('leading_ib_end', 'record@%d: IndirectBufferEnd before any '
                             'IndirectBufferStart - the trace opens inside an indirect '
                             'buffer; packets before it are reported with in_ib=false' % off)
                else:
                    eng.ib_depth -= 1
            elif tag == K_PKT_START:
                base, count = r.u32s(2, 'packet start')
                if count == 0 or count * 4 > size - r.tell():
                    raise TraceError(EXIT_MALFORMED, off,
                                     'packet record claims %d dwords, %d bytes remain'
                                     % (count, size - r.tell()))
                data = r.exact(count * 4, 'packet body')
                if pending is not None:
                    raise TraceError(EXIT_MALFORMED, off,
                                     'PacketStart while packet record@%d is still pending '
                                     '(its End never came)' % pending[1])
                eng.mem.write(base, data)
                st.packet_records += 1
                pending = (data, off)
                eng.pending_reads = PacketLocalReads()   # reset: coverage is per packet
            elif tag == K_PKT_END:
                if pending is not None:
                    # the coverage object is handed ONLY to this execution; it
                    # is replaced at the next PacketStart (the single reset) and
                    # a read arriving between packets lands in a dead object
                    eng.execute(pending[0], pending[1], eng.pending_reads)
                    pending = None
                elif wrapper_ends_owed:
                    wrapper_ends_owed -= 1
                    eng.ib_wrapper_ends += 1
                else:
                    raise TraceError(EXIT_MALFORMED, off, 'PacketEnd without a PacketStart')
            elif tag in (K_MEM_READ, K_MEM_WRITE):
                base, enc, elen, dlen = r.u32s(4, 'memory record')
                if dlen > MAX_DECODED_BYTES:
                    raise TraceError(EXIT_MALFORMED, off,
                                     'memory record claims %d decoded bytes' % dlen)
                if elen > size - r.tell():
                    raise TraceError(EXIT_MALFORMED, off,
                                     'memory record claims %d encoded bytes, %d remain'
                                     % (elen, size - r.tell()))
                payload = r.exact(elen, 'memory payload')
                if tag == K_MEM_WRITE:
                    # A write-back of GPU output: validated like every payload
                    # (encoding, dependency, exact decoded length), then
                    # DISCARDED - it never becomes input memory.
                    decode_payload(enc, payload, dlen, off, 'memory write')
                    st.mem_write_records += 1
                    st.mem_write_bytes += dlen
                    continue   # discarded: never input memory
                st.mem_read_records += 1
                st.mem_read_bytes += dlen
                data = decode_payload(enc, payload, dlen, off, 'memory read')
                eng.mem.write(base, data)
                if eng.pending_reads is not None:
                    eng.pending_reads.add(base, data)   # supplied INSIDE the pending packet
            elif tag == K_EDRAM:
                enc, elen = r.u32s(2, 'edram record')
                payload = r.exact(elen, 'edram payload')
                # EdramSnapshotCommand carries no decoded length: the snapshot
                # is the whole EDRAM or the record is malformed.
                decode_payload(enc, payload, EDRAM_BYTES, off, 'edram snapshot')
                st.edram_snapshots += 1
            elif tag == K_EVENT:
                (et,) = r.u32s(1, 'event record')
                if et != 0:
                    raise TraceError(EXIT_UNSUPPORTED, off, 'event type %d (only kSwap=0 is known)' % et)
                st.swaps += 1
                eng.frame += 1
                if max_frames and eng.frame >= max_frames:
                    st.stopped_at_frame_limit = True
                    break
            elif tag == K_REGS:
                first, count, _cb, enc, elen = r.u32s(5, 'registers record')
                if first + count > MAX_REGS:
                    raise TraceError(EXIT_MALFORMED, off,
                                     'registers record covers 0x%04X..0x%04X (file has 0x%04X)'
                                     % (first, first + count, MAX_REGS))
                if elen > size - r.tell():
                    raise TraceError(EXIT_MALFORMED, off,
                                     'registers record claims %d encoded bytes, %d remain'
                                     % (elen, size - r.tell()))
                payload = r.exact(elen, 'registers payload')
                data = decode_payload(enc, payload, count * 4, off, 'registers')
                for i, v in enumerate(struct.unpack('<%dI' % count, data)):
                    eng.write_reg(first + i, v)
                st.registers_records += 1
                st.register_values += count
            elif tag == K_GAMMA:
                rw, enc, elen = struct.unpack('<BxxxII', r.exact(12, 'gamma record'))
                payload = r.exact(elen, 'gamma payload')
                # GammaRampCommand carries no decoded length: 256 + 3*128 dwords.
                decode_payload(enc, payload, GAMMA_BYTES, off, 'gamma ramp')
                st.gamma_ramps += 1
            else:
                raise TraceError(EXIT_UNSUPPORTED, off, 'unknown trace record tag %d' % tag)
        st.bytes_parsed = r.tell()
    if pending is not None and not st.stopped_at_frame_limit:
        eng.warn('pending_at_eof', 'record@%d: trace ends with a PacketStart and no End' % pending[1])
    if wrapper_ends_owed and not st.stopped_at_frame_limit:
        eng.warn('ib_wrapper_end_missing', 'trace ends owing %d INDIRECT_BUFFER packet End(s)'
                 % wrapper_ends_owed)
    if eng.ib_depth != 0 and not st.stopped_at_frame_limit:
        eng.warn('ib_unbalanced', 'trace ends inside %d unterminated indirect-buffer span(s)' % eng.ib_depth)
    return eng, hdr, st


# ── summaries ────────────────────────────────────────────────────────────────

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def resolve_detail(r):
    """JSON-safe copy of one resolve record (raw registers + decoded fields)."""
    return dict(draw_ord=r['draw_ord'], frame=r['frame'], copy_control=r['copy_control'],
                copy=dict(r['copy']), dest_base=r['dest_base'], dest_pitch=r['dest_pitch'],
                dest_pitch_decoded=dict(r['dest_pitch_decoded']), dest_info=r['dest_info'],
                dest=dict(r['dest']), surface_info=r['surface_info'],
                rt_info=list(r['rt_info']), depth_info=r['depth_info'], source=dict(r['source']),
                get_resolve_info_inputs=json.loads(json.dumps(r['get_resolve_info_inputs'])),
                guest_destination_range=json.loads(json.dumps(r['guest_destination_range'])))


def resolve_inputs_label(gi):
    vf, vp = gi['vertex_fetch_0'], gi['vertex_payload']
    if gi['status'] == 'rejected':
        return 'REJECTED vf0 type=%s(%d) size=%ddw (%s)' % (vf['type_name'], vf['type'], vf['size_dwords'],
                                                            gi['rejection'])
    head = 'vf0 type=%d addr=%08X size=%ddw endian=%d' % (vf['type'], vf['address_bytes'], vf['size_dwords'],
                                                         vf['endian'])
    if gi['status'] == 'incomplete':
        return 'INCOMPLETE %s missing=%d bytes' % (head, vp['missing_bytes'])
    return 'complete %s verts=%s' % (head, ' '.join('%08X' % v for v in vp['dwords_be']))


def summary_dict(path, hdr, eng, st, refusal=None):
    kinds = collections.Counter(d['kind'] for d in eng.draws) if eng else collections.Counter()
    # A draw is IDENTIFIED only when both shader hashes are known. An unhashed
    # (null) side is not an identity: such draws are counted separately and
    # never form a distinct pair.
    identified = [d for d in eng.draws if d['vs'] is not None and d['ps'] is not None] if eng else []
    out = dict(
        schema=SCHEMA, tool_version=TOOL_VERSION, xenia_pin=XENIA_PIN, mcla_pin=MCLA_PIN,
        input_path=os.path.abspath(path), input_bytes=os.path.getsize(path),
        input_sha256=sha256_file(path),
        status='refused' if refusal else 'ok',
        refusal=(dict(code=refusal.code, offset=refusal.offset, reason=refusal.reason)
                 if refusal else None),
    )
    if hdr:
        out.update(xenia_build_sha=hdr['sha'], title_id='%08X' % hdr['title'],
                   trace_version=hdr['version'])
    if eng is None:
        return out
    out.update(
        frames=eng.frame, stopped_at_frame_limit=st.stopped_at_frame_limit,
        bytes_parsed=st.bytes_parsed,
        records={str(k): v for k, v in sorted(st.records.items())},
        packet_records=st.packet_records, packets_executed=eng.packets_executed,
        ib_dispatch_records=eng.ib_dispatch_records, ib_spans=eng.ib_spans,
        leading_ib_ends=eng.leading_ib_ends, ib_wrapper_ends=eng.ib_wrapper_ends,
        primary_spans=st.primary_spans,
        draw_packets=len(eng.draws), draws=kinds.get('draw', 0),
        resolves=kinds.get('resolve', 0), noop_draws=kinds.get('noop', 0),
        unknown_mode_draws=kinds.get('unknown_mode', 0),
        zero_index_draws=eng.zero_index_draws,
        screen_extent_events=eng.screen_extent_events,
        event_writes={str(k): v for k, v in sorted(eng.event_initiators.items())},
        memory_read_records=st.mem_read_records, memory_read_bytes=st.mem_read_bytes,
        memory_write_records=st.mem_write_records, memory_write_bytes=st.mem_write_bytes,
        memory_pages=eng.mem.page_count(),
        unmapped_reads=eng.unmapped_reads, unmapped_bytes=eng.unmapped_bytes,
        registers_records=st.registers_records, register_values=st.register_values,
        edram_snapshots=st.edram_snapshots, gamma_ramps=st.gamma_ramps,
        shader_loads=dict(vs=eng.vs_loads, ps=eng.ps_loads),
        distinct_shaders=dict(vs=len({d['vs'] for d in eng.draws if d['vs'] is not None}),
                              ps=len({d['ps'] for d in eng.draws if d['ps'] is not None}),
                              pairs=len({(d['vs'], d['ps']) for d in identified}),
                              unidentified_draws=len(eng.draws) - len(identified)),
        resolve_details=[resolve_detail(r) for r in eng.resolves],
        resolve_inputs=dict(
            complete=eng.resolve_input_counts['complete'],
            rejected=eng.resolve_input_counts['rejected'],
            incomplete=eng.resolve_input_counts['incomplete'],
            total=sum(eng.resolve_input_counts.values()),
            distinct_vertex_payloads=len({r['get_resolve_info_inputs']['vertex_payload']['bytes_hex']
                                          for r in eng.resolves
                                          if r['get_resolve_info_inputs']['status'] == 'complete'}),
            distinct_vertex_addresses=len({r['get_resolve_info_inputs']['vertex_fetch_0']['address_bytes']
                                           for r in eng.resolves}),
            not_in_trace=list(RESOLVE_INPUTS_NOT_IN_TRACE)),
        derivation_manifest=json.loads(json.dumps(DERIVATION_MANIFEST)),
        guest_destination_ranges=dict(
            {st: eng.range_status_counts[st] for st in RANGE_STATUSES},
            total=sum(eng.range_status_counts.values()),
            join_usable=sum(1 for r in eng.resolves if r['guest_destination_range']['join_usable']),
            adjusted_base_differs_from_raw=sum(1 for r in eng.resolves
                                               if 'range' in r['guest_destination_range']
                                               and not r['guest_destination_range']['raw_vs_derived']['adjusted_base_equals_raw']),
            extent_start_differs_from_raw_base=sum(1 for r in eng.resolves
                                                   if 'range' in r['guest_destination_range']
                                                   and not r['guest_destination_range']['raw_vs_derived']['extent_start_equals_raw_base']),
            extent_differs_from_declared_linear_span=sum(1 for r in eng.resolves
                                                         if 'range' in r['guest_destination_range']
                                                         and not r['guest_destination_range']['raw_vs_derived']['extent_equals_declared_linear_span'])),
        static_texture_shaders={k: json.loads(json.dumps(e)) for k, e in sorted(eng.shader_table.items())},
        texture_fetch_constants={k: json.loads(json.dumps(e)) for k, e in sorted(eng.fetch_constants.items())},
        static_texture_bindings=[json.loads(json.dumps(dict(r, resolve_relation=relation_for(eng, True)['per_draw'][r['draw_ord']])))
                                 for r in eng.static_bindings],
        static_texture_binding_census=json.loads(json.dumps(static_binding_census(eng))),
        texture_backing_manifest=json.loads(json.dumps(TEXTURE_BACKING_MANIFEST)),
        requested_binding_census=json.loads(json.dumps(requested_binding_census(eng))),
        resolve_consumer_relation=json.loads(json.dumps(relation_report(eng))),
        opcode_histogram={'0x%02X' % k: v for k, v in sorted(eng.opcodes.items())},
        unknown_opcodes={'0x%02X' % k: v for k, v in sorted(eng.unknown_ops.items())},
        vfetch_census=dict(
            vs_loads=eng.vf['loads'], parsed=eng.vf['parsed'], failed=eng.vf['failed'],
            bindings=eng.vf['bindings'], attrs=eng.vf['attrs'],
            must_be_one_violations=eng.vf['must_be_one_violations'],
            per_format={VFMT.get(k, 'fmt%d' % k): v for k, v in sorted(eng.vf['fmt'].items())},
            naive_divergent_loads=eng.vf['naive_divergent_loads'],
            naive_extra=eng.vf['naive_extra'], naive_missing=eng.vf['naive_missing'],
            naive_extra_formats={VFMT.get(k, 'fmt%d' % k): v
                                 for k, v in sorted(eng.vf['naive_extra_fmt'].items())}),
        warning_counts=dict(eng.warning_counts), warnings=list(eng.warnings),
    )
    return out


def fmt_row(d):
    r = d['regs']
    ci = decode_color_info(r[0x2001])
    si = decode_surface_info(r[0x2000])
    di = r[0x2002]
    vs = '%016X' % d['vs'] if d['vs'] is not None else '-' * 16
    ps = '%016X' % d['ps'] if d['ps'] is not None else '-' * 16
    return (f"#{d['ord']:<5} f{d['frame']:<2} {d['kind']:<7} prim={d['prim']:<2} n={d['count']:<6} "
            f"src={d['src_sel']} it={d['idx_type']} vs={vs} ps={ps} "
            f"rt=base{ci['base']:03X}/fmt{ci['fmt']:<2} pitch={si['pitch']} msaa={si['msaa']} "
            f"depth=base{di & 0xFFF:03X}/fmt{(di >> 16) & 1} vte={r[0x2206]:03X} "
            f"vp=({f32(r[0x210F]):.1f},{f32(r[0x2111]):.1f}|{f32(r[0x2110]):.1f},{f32(r[0x2112]):.1f}) "
            f"mode={d['mode']} cm={r[0x2104]:04X} dc={r[0x2200]:08X} bc={r[0x2201]:08X} "
            f"cull={r[0x2205]:08X} pc={r[0x2180]:08X}{' ib' if d['in_ib'] else ''}")


def print_consts(d, which, rng):
    arr = d['vs_consts'] if which == 'vs' else d['ps_consts']
    lo, hi = rng
    for c in range(lo, min(hi, 256)):
        v = arr[c * 4:c * 4 + 4]
        if not any(v) and not (lo <= c < lo + 16):
            continue
        print(f"  {which} c{c:<3} = ({f32(v[0]):>12.6g}, {f32(v[1]):>12.6g}, "
              f"{f32(v[2]):>12.6g}, {f32(v[3]):>12.6g})  raw={v[0]:08X} {v[1]:08X} {v[2]:08X} {v[3]:08X}")


def print_summary(path, hdr, eng, st):
    s = summary_dict(path, hdr, eng, st)
    print(f"trace   : {path}  title={s['title_id']} xenia={hdr['sha'][:12]}  sha256={s['input_sha256'][:16]}")
    print(f"frames  : {s['frames']}  draw packets={s['draw_packets']} (draws={s['draws']} "
          f"resolves={s['resolves']} noop={s['noop_draws']} unknownMode={s['unknown_mode_draws']} "
          f"zeroIdx={s['zero_index_draws']})  screenExtentEvents={s['screen_extent_events']}")
    print(f"records : packetRecords={s['packet_records']} executed={s['packets_executed']} "
          f"ibDispatch={s['ib_dispatch_records']} ibSpans={s['ib_spans']} "
          f"leadingIbEnds={s['leading_ib_ends']} "
          f"memReads={s['memory_read_records']} ({s['memory_read_bytes'] >> 20} MB) "
          f"memWrites={s['memory_write_records']} regs={s['registers_records']} "
          f"edram={s['edram_snapshots']} gamma={s['gamma_ramps']}")
    print(f"memory  : pages={s['memory_pages']} ({s['memory_pages'] * PAGE >> 20} MB)  "
          f"unmappedReads={s['unmapped_reads']} ({s['unmapped_bytes']} B)")
    ri = s['resolve_inputs']
    gr = s['guest_destination_ranges']
    print(f"ranges  : complete={gr['complete']} joinUsable={gr['join_usable']} emptyRect={gr['empty_rectangle']} "
          f"nonResolvable={gr['non_resolvable_format']} rejCmd={gr['rejected_copy_command']} rejMsaa={gr['rejected_msaa']} "
          f"inputsRejected={gr['inputs_rejected']} inputsIncomplete={gr['inputs_incomplete']} of {gr['total']}  "
          f"adjBase!=raw={gr['adjusted_base_differs_from_raw']} start!=raw={gr['extent_start_differs_from_raw_base']} "
          f"extent!=linear={gr['extent_differs_from_declared_linear_span']}  derivation={DERIVATION_VERSION}")
    print(f"resolveIn: complete={ri['complete']} rejected={ri['rejected']} incomplete={ri['incomplete']} "
          f"of {ri['total']}  distinctPayloads={ri['distinct_vertex_payloads']} "
          f"distinctVfAddrs={ri['distinct_vertex_addresses']}  notInTrace={len(ri['not_in_trace'])} params")
    print(f"shaders : vsLoads={s['shader_loads']['vs']} psLoads={s['shader_loads']['ps']} "
          f"distinct vs={s['distinct_shaders']['vs']} ps={s['distinct_shaders']['ps']} "
          f"pairs={s['distinct_shaders']['pairs']} "
          f"unidentifiedDraws={s['distinct_shaders']['unidentified_draws']}")
    vf = s['vfetch_census']
    print(f"vfetch  : vsLoads={vf['vs_loads']} parsed={vf['parsed']} failed={vf['failed']} "
          f"bindings={vf['bindings']} attrs={vf['attrs']} mustBeOneViolations={vf['must_be_one_violations']} "
          f"| naive scan: divergentLoads={vf['naive_divergent_loads']} extra={vf['naive_extra']} "
          f"missing={vf['naive_missing']}")
    sb = s['static_texture_binding_census']
    sa = sb['shader_analysis']
    print(f"bindings: normalDraws={sb['normal_draws_total']} bothComplete={sa['draws_both_complete']} "
          f"withBinding={sb['draws_with_static_binding']} noBinding={sb['draws_with_no_static_binding']} "
          f"notComplete={sb['draws_analysis_not_complete']} conserved={int(sb['conservation_holds'])} | "
          f"slots vs={sb['referenced_slots']['vs']} ps={sb['referenced_slots']['ps']} union={sb['referenced_slots']['union']} "
          f"overlap={sb['referenced_slots']['overlap']} type2={sb['type2_slots_total']} "
          f"unusedType2={sb['unused_type2_slots_total']} nonTextureRefs={sb['draws_with_referenced_non_texture_slot']} | "
          f"T3A1 type2==union {sb['t3a1']['type2_set_equals_union']}/{sb['t3a1']['complete_draws']} draws  "
          f"analysis={STATIC_BINDING_VERSION}")
    rq = s['requested_binding_census']
    pt, rs, rv = rq['post_translation'], rq['requested_slots'], rq['requested_vs_analysis']
    print(f"requested: postTable={'yes' if rq['post_translation_table']['supplied'] else 'NO'} "
          f"postComplete={pt['draws_post_complete']}/{rq['normal_draws_total']} "
          f"vs={dict((k, v) for k, v in pt['vs'].items() if v)} ps={dict((k, v) for k, v in pt['ps'].items() if v)} | "
          f"slots vs={rs['vs']} ps={rs['ps']} union={rs['union']} overlap={rs['overlap']} "
          f"eqAnalysis={rv['draws_equal']} differ={rv['draws_differ']} | constants distinct={rq['distinct_fetch_constants']} "
          f"requested={rq['distinct_requested_constants']} backing={dict((k, v) for k, v in rq['constant_backing_status'].items() if v)} "
          f"usable={rq['constants_join_usable']} | draws allUsable={rq['draws_all_requested_join_usable']} "
          f"notUsable={rq['draws_with_requested_not_usable']} noRequested={rq['draws_no_requested_slots']} "
          f"conserved={int(rq['draws_conserved'] and rq['constant_backing_conserved'])}  backing={TEXTURE_BACKING_VERSION}")
    rr_ = s['resolve_consumer_relation']
    pc, cc, ce, mx = rr_['producers'], rr_['consumers'], rr_['census'], rr_['cvar_matrix']
    print(f"relation: producers={pc['eligible']}/{pc['resolves_total']} excluded={pc['excluded_rows_total']}{dict((k, v) for k, v in pc['excluded_checks'].items() if v)} | "
          f"consumers postComplete={cc['draws_post_complete']}/{cc['normal_draws']} occ={cc['occurrences_eligible']}/{cc['requested_occurrences']} "
          f"comps base={cc['components']['base']} mips={cc['components']['mips']} excluded={dict((k, v) for k, v in cc['occurrences_excluded'].items() if v)} | "
          f"pairs={ce['candidate_pairs']} equal={ce['relations']['equal']} pcc={ce['relations']['producer_contains_consumer']} "
          f"ccp={ce['relations']['consumer_contains_producer']} partial={ce['relations']['partial_overlap']} disjoint={ce['relations']['disjoint']} "
          f"conserved={int(ce['conservation_holds'] and ce['detail_rows_equal_non_disjoint'])} details={ce['detail_rows']} | "
          f"resolvesNoLaterOverlap={ce['producers_with_zero_later_overlap']['count']}/{pc['eligible']} "
          f"maxProducersPerComponent={ce['multiplicity']['max_producers_per_component']} | "
          f"T3C2 rawEq={rr_['t3c2']['raw_base_equal']['equals_full_relation']} adjEq={rr_['t3c2']['adjusted_start_equal']['equals_full_relation']} | "
          f"cvarFalse same={int(mx['same_as_default'])}  relation={RELATION_VERSION}")
    if s['unknown_opcodes']:
        print(f"unknown type-3 opcodes: {s['unknown_opcodes']}")
    if s['warning_counts']:
        print(f"warnings: {s['warning_counts']}")


def _missing_minus_resolves(eng, prods, a0, a1, draw):
    """(never, late, resolved) bytes of [a0, a1) for one draw. Temporal, as
    Xenia's TracePlayer plays it (iter-463 review): a MemoryRead applies when
    the stream reaches it and a draw runs at its PacketEnd, so a byte counts as
    supplied only if a record BEFORE the draw's execution wrote it. never = no
    record anywhere; late = supplied only after the draw (e.g. inside the swap
    packet, where the renderer's render-time pre-pass validation lands);
    resolved = unsupplied-before-the-draw bytes a resolve issued EARLIER in the
    trace wrote (Xenia produces those itself)."""
    seq = draw['mem_seq']
    total = eng.mem.missing_before(a0, a1, seq)
    if not total:
        return 0, 0, 0
    ivs = []
    for p in prods:
        if p['draw_ord'] < draw['ord']:
            s = max(a0, p['start'] & PHYS_MASK)
            t = min(a1, (p['start'] & PHYS_MASK) + p['length'])
            if t > s:
                ivs.append((s, t))
    ivs.sort()
    rest, pos = [], a0     # [a0, a1) minus the earlier resolves' union
    for s, t in ivs:
        if s > pos:
            rest.append((pos, s))
        pos = max(pos, t)
    if pos < a1:
        rest.append((pos, a1))
    never = sum(eng.mem.uncovered(s, t) for s, t in rest)
    miss = sum(eng.mem.missing_before(s, t, seq) for s, t in rest)
    return never, miss - never, total - miss


def coverage_report(eng):
    """iter-463 (D8): does the trace carry every byte Xenia needs to draw each
    normal draw? Per draw: the index buffer (DMA draws: count x 2/4 bytes at
    VGT_DMA_BASE), every vertex fetch constant the VS's CF walk binds
    (parse_vs_vfetch) and the base + mip backing of every texture fetch slot
    either shader statically binds (derive_texture_backing - Xenia's
    GetGuestTextureLayout, so cube / stacked mips count). TEMPORAL (iter-463
    review): a byte counts as supplied for draw N only if a record before N's
    execution wrote it or a resolve earlier in the trace produced it; bytes
    no record wrote are MISSING, bytes only a later record wrote are LATE, and
    either makes the draw incomplete. A draw whose RB_MODECONTROL is not
    kColorDepth runs without its pixel shader in Xenia, so only its VS slots
    count. Prototype:
    archive/v1_arena/xenia_oracle/tex_coverage.py."""
    prods, _, _ = relation_producers(eng)
    by_ord = {d['ord']: d for d in eng.draws}
    rows_by_ord = {r['draw_ord']: r for r in eng.static_bindings}
    cat = {k: collections.OrderedDict(ranges=0, bytes=0, missing=0, late=0, resolved=0, draws=set(),
                                      late_draws=set())
           for k in ('ib', 'vb', 'tex_base', 'tex_mips')}
    per_tex = collections.OrderedDict()
    per_vs = collections.defaultdict(lambda: collections.Counter())
    unanalysed = []
    vs_cache = {}

    last = {}
    ps_unused = [0]

    def account(kind, a0, length, draw):
        """Bytes this draw lacks (missing + late); per-kind totals updated."""
        last[kind] = (0, 0)
        if not length:
            return 0
        a0 &= PHYS_MASK
        c = cat[kind]
        c['ranges'] += 1
        c['bytes'] += length
        m, late, r = _missing_minus_resolves(eng, prods, a0, a0 + length, draw)
        c['missing'] += m
        c['late'] += late
        c['resolved'] += r
        if m or late:
            c['draws'].add(draw['ord'])
        if late:
            c['late_draws'].add(draw['ord'])
        last[kind] = (m, late)
        return m + late

    for d in eng.draws:
        if d['kind'] != 'draw':
            continue
        o = d['ord']
        if d['src_sel'] == 0 and d['count']:
            m = account('ib', d['dma_base'], d['count'] * (4 if d['idx_type'] else 2), d)
            if m:
                per_vs[d['vs']]['ib'] += 1
        if d['vs'] is not None:
            cf = vs_cache.get(d['vs'])
            if cf is None:
                cf = vs_cache[d['vs']] = parse_vs_vfetch(eng.ucodes.get(d['vs'], b''))
            for b in cf['bindings']:
                fc = b['fetch_const']
                d0, d1 = d['fetch'][fc * 2], d['fetch'][fc * 2 + 1]
                if (d0 & 3) != 3:
                    continue
                if account('vb', d0 & ~3, ((d1 >> 2) & 0xFFFFFF) * 4, d):
                    per_vs[d['vs']]['vb'] += 1
        r = rows_by_ord.get(o)
        if r is None or r['analysis_status'] != 'complete':
            unanalysed.append(o)
            continue
        slots = r['union_slots']
        if d['mode'] != MODE_COLOR_DEPTH:
            # Xenia runs a non-kColorDepth draw without its pixel shader
            # (draw_util.cc IsPixelShaderNeededWithRasterization), so it never
            # requests the PS-only slots' textures: they are not needed bytes.
            vs_slots = set(mask_slots(r['stages']['vs']['binding_mask']))
            ps_unused[0] += sum(1 for sl in slots if sl not in vs_slots)
            slots = [sl for sl in slots if sl in vs_slots]
        for slot in slots:
            raw = d['fetch'][slot * 6: slot * 6 + 6]
            key = fetch_constant_key(raw)
            e = per_tex.get(key)
            if e is None:
                b = derive_texture_backing(raw)
                e = per_tex[key] = dict(slot=slot, status=b['status'], backing=b, draws=[], ps=set(), missing=0,
                                        base_missing=0, mips_missing=0, total=0, late_draws=[])
                if b['status'] == 'complete':
                    k = b['key']
                    e['fmt'] = k.get('format_name', k.get('format'))
                    e['dims'] = (k['width_minus_1'] + 1, k['height_minus_1'] + 1,
                                 k['depth_or_array_size_minus_1'] + 1, k['dimension_name'], k['mip_max_level'] + 1)
            e['draws'].append(o)
            e['ps'].add(d['ps'])
            b = e['backing']
            if b['status'] != 'complete':
                continue
            for kind, iv in (('tex_base', b['base_interval']), ('tex_mips', b['mips_interval'])):
                if not iv or not iv['length']:
                    continue
                m = account(kind, iv['start'], iv['length'], d)
                never, late = last[kind]
                if len(e['draws']) == 1:
                    e['total'] += iv['length']
                    e['missing'] += never
                    e['base_missing' if kind == 'tex_base' else 'mips_missing'] += never
                if late and (not e['late_draws'] or e['late_draws'][-1] != o):
                    e['late_draws'].append(o)
                if m:
                    per_vs[d['vs']][kind] += 1
    for c in cat.values():
        c['draws'] = sorted(c['draws'])
        c['late_draws'] = sorted(c['late_draws'])
    draws_bad = sorted(set().union(*(set(c['draws']) for c in cat.values())))
    return collections.OrderedDict(
        normal_draws=sum(1 for d in eng.draws if d['kind'] == 'draw'), categories=cat, textures=per_tex,
        per_vs=per_vs, unanalysed=unanalysed, draws_incomplete=draws_bad,
        ps_slots_unused=ps_unused[0], complete=not draws_bad)


def print_coverage(rep):
    cat = rep['categories']
    print(f"coverage: {rep['normal_draws']} normal draws; {len(rep['draws_incomplete'])} with an unsupplied "
          f"IB / VB / texture byte; {len(rep['unanalysed'])} draws with texture analysis incomplete (not checked); "
          f"{rep['ps_slots_unused']} PS-only slot uses skipped (non-kColorDepth draws run without a PS in Xenia)")
    for k, c in cat.items():
        print(f"  {k:<8} ranges={c['ranges']:<6} bytes={c['bytes']:<11} MISSING={c['missing']:<11} "
              f"LATE={c['late']:<11} resolvedEarlier={c['resolved']:<9} draws={len(c['draws'])} "
              f"lateDraws={len(c['late_draws'])} first={c['draws'][:8]}")
    late = [e for e in rep['textures'].values() if e['late_draws']]
    if late:
        print(f"  textures sampled BEFORE the trace supplies them (LATE - Xenia's player samples what the "
              f"memory held then): {len(late)}")
        for e in sorted(late, key=lambda e: -len(e['late_draws']))[:20]:
            b = e['backing']
            bi, mi = b['base_interval'], b['mips_interval']
            print(f"    t{e['slot']:<2} {str(e.get('fmt')):<24} dims={e.get('dims')} "
                  f"base={'%08X' % bi['start'] if bi else '-'} mips={'%08X' % mi['start'] if mi else '-'} "
                  f"lateDraws={len(e['late_draws'])} first={e['late_draws'][:5]}")
    miss = [e for e in rep['textures'].values() if e['missing']]
    print(f"  textures: {len(rep['textures'])} distinct bound fetch constants, {len(miss)} with unsupplied bytes "
          f"({sum(1 for e in miss if e['base_missing'])} base, {sum(1 for e in miss if e['mips_missing'])} mips)")
    for e in sorted(miss, key=lambda e: (-len(e['draws']), -e['missing']))[:40]:
        b = e['backing']
        bi, mi = b['base_interval'], b['mips_interval']
        ps = ','.join('%016X' % p for p in sorted(p for p in e['ps'] if p is not None))
        print(f"    t{e['slot']:<2} {str(e.get('fmt')):<24} dims={e.get('dims')} "
              f"base={'%08X' % bi['start'] if bi else '-'} missing={e['base_missing']}/{bi['length'] if bi else 0} "
              f"mips={'%08X' % mi['start'] if mi else '-'} missing={e['mips_missing']}/{mi['length'] if mi else 0} "
              f"draws={len(e['draws'])} first={e['draws'][:5]} ps={ps[:52]}")
    if rep['per_vs']:
        print('  incomplete draws by VS (ib/vb/tex_base/tex_mips = draws missing that kind):')
        for vs, c in sorted(rep['per_vs'].items(), key=lambda kv: -sum(kv[1].values()))[:30]:
            print(f"    vs={'%016X' % vs if vs is not None else 'none':<16} ib={c['ib']} vb={c['vb']} "
                  f"tex_base={c['tex_base']} tex_mips={c['tex_mips']}")
    print('coverage: ' + ('COMPLETE' if rep['complete'] else
                          f"INCOMPLETE - Xenia's render of this trace reads zeros for the missing / late bytes "
                          f"(exit {EXIT_INCOMPLETE})"))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('trace')
    ap.add_argument('--frames', type=int, default=0, help='stop after N swap events')
    ap.add_argument('--json', help='write the machine-readable summary here')
    ap.add_argument('--no-shader-hash', action='store_true',
                    help='read without xxhash; hashes are reported as null, never 0')
    ap.add_argument('--list', action='store_true', help='one row per draw packet')
    ap.add_argument('--frame', type=int, help='restrict --list to this frame index')
    ap.add_argument('--shaders', action='store_true', help='per (vs,ps) census')
    ap.add_argument('--draw', type=int, help='select draw ordinal for detail views')
    ap.add_argument('--consts', action='store_true', help='print VS+PS float constants')
    ap.add_argument('--crange', default='0:256', help='constant range lo:hi')
    ap.add_argument('--verts', type=int, default=0, help='decode N vertices from the VS vfetches')
    ap.add_argument('--textures', action='store_true',
                    help='static texture bindings of the draw (VS+PS CF walk) with decoded fetch constants')
    ap.add_argument('--resolves', action='store_true', help='list kCopy-mode draws (resolves)')
    ap.add_argument('--vfetch-census', action='store_true',
                    help='XT1: per distinct VS, CF-walk fetches vs the naive triple scan')
    ap.add_argument('--dump-ucode', help="write the selected draw's VS/PS ucode to PREFIX_vs.bin/_ps.bin")
    ap.add_argument('--dump-verts', help="write the selected draw's slot-0 vertex bytes to FILE")
    ap.add_argument('--post-masks', help='recorded actual-translator post-translation mask table (mask_oracle.py JSON)')
    ap.add_argument('--expand-v5', help='write the /5 projection reconstructed from the /6 normalized tables to FILE')
    ap.add_argument('--relations', action='store_true', help='list every non-disjoint resolve/consumer interval pair (TOOL-3C)')
    ap.add_argument('--coverage', action='store_true',
                    help='iter-463: fail (exit %d) when a normal draw binds IB / VB / texture base or mip bytes '
                         'the trace never supplied' % EXIT_INCOMPLETE)
    a = ap.parse_args(argv)

    if not os.path.isfile(a.trace):
        print(f'REFUSED: {a.trace}: no such file', file=sys.stderr)
        return EXIT_USAGE
    post_table = None
    if a.post_masks:
        if not os.path.isfile(a.post_masks):
            print(f'REFUSED: {a.post_masks}: no such post-translation mask table', file=sys.stderr)
            return EXIT_USAGE
        post_table = load_post_mask_table(a.post_masks)
    try:
        eng, hdr, st = parse(a.trace, a.frames, hash_shaders=not a.no_shader_hash, post_table=post_table)
    except TraceError as e:
        print(f'REFUSED: {a.trace}: {e.reason} at byte offset {e.offset} (exit {e.code})',
              file=sys.stderr)
        if a.json:
            with open(a.json, 'w', encoding='utf-8') as jf:
                json.dump(summary_dict(a.trace, None, None, None, refusal=e), jf, indent=1)
        return e.code

    print_summary(a.trace, hdr, eng, st)
    if a.json or a.expand_v5:
        summary = summary_dict(a.trace, hdr, eng, st)
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as jf:
            json.dump(summary, jf, indent=1)
        print(f'json    : {a.json}')
    if a.expand_v5:
        with open(a.expand_v5, 'w', encoding='utf-8') as ef:
            json.dump(expand_static_bindings_v5(summary), ef, indent=1)
        print(f'expand  : {a.expand_v5} (the /5 projection rebuilt from the /6 tables)')

    if a.list:
        for d in eng.draws:
            if a.frame is not None and d['frame'] != a.frame:
                continue
            print(fmt_row(d))

    if a.shaders:
        cen = collections.Counter()
        verts = collections.Counter()
        rts = collections.defaultdict(set)
        unidentified = 0
        for d in eng.draws:
            if d['vs'] is None or d['ps'] is None:
                unidentified += 1      # no hash on one side: not an identity
                continue
            k = (d['vs'], d['ps'])
            cen[k] += 1
            verts[k] += d['count']
            ci = decode_color_info(d['regs'][0x2001])
            rts[k].add((ci['base'], ci['fmt']))
        print(f"{len(cen)} distinct (vs,ps) pairs among identified draws; "
              f"{unidentified} unidentified draw(s) (a null hash) excluded, not counted as a pair:")
        for k, n in cen.most_common():
            print(f"  draws={n:<5} verts={verts[k]:<8} vs={k[0]:016X} ps={k[1]:016X} rt0={sorted(rts[k])}")

    if a.resolves:
        print(f"{len(eng.resolves)} resolve(s) = draw packets issued with RB_MODECONTROL.edram_mode==kCopy:")
        for i, r in enumerate(eng.resolves):
            c, di, dp = r['copy'], r['dest'], r['dest_pitch_decoded']
            print(f"  resolve#{i} draw#{r['draw_ord']} f{r['frame']} copyCtl={r['copy_control']:08X} "
                  f"(src={c['copy_src_select']} sample={c['copy_sample_select']} cmd={c['copy_command_name']} "
                  f"clearC={c['color_clear_enable']} clearD={c['depth_clear_enable']}) "
                  f"dest={r['dest_base']:08X} pitch={dp['pitch']} h={dp['height']} "
                  f"destInfo={r['dest_info']:08X} (fmt={di['fmt']} endian={di['endian']} "
                  f"slice={di['slice']} array={di['array']}) "
                  f"source=[{resolve_source_label(r['source'])}] "
                  f"rtInfo={' '.join('%08X' % v for v in r['rt_info'])} depthInfo={r['depth_info']:08X} "
                  f"inputs=[{resolve_inputs_label(r['get_resolve_info_inputs'])}] "
                  f"@record{r['get_resolve_info_inputs']['draw_packet_record_offset']} "
                  f"range=[{range_label(r['guest_destination_range'])}]")

    if a.relations:
        rr_ = relation_report(eng)
        pc, ce = rr_['producers'], rr_['census']
        print(f"resolve/consumer interval relation ({RELATION_VERSION}): producers {pc['eligible']}/{pc['resolves_total']} eligible, "
              f"{ce['candidate_pairs']} candidate pairs, {ce['detail_rows']} non-disjoint (equal={ce['relations']['equal']} "
              f"pcc={ce['relations']['producer_contains_consumer']} ccp={ce['relations']['consumer_contains_producer']} "
              f"partial={ce['relations']['partial_overlap']}); opportunities, not executed copies or samples; every overlapping "
              f"earlier resolve listed, none selected")
        for x in pc['excluded_rows']:
            print(f"  excluded producer resolve#{x['resolve_index']} draw#{x['draw_ord']} status={x['status']} reasons={x['reasons']}")
        for p in pc['rows']:
            print(f"  producer resolve#{p['resolve_index']} draw#{p['draw_ord']} [{p['start']:08X},{p['end_exclusive']:08X}) +{p['length']} "
                  f"raw={p['raw_base']:08X} adj={p['adjusted_base']:08X} dest={p['destination_observed']['format_name']} "
                  f"{p['destination_observed']['tiling_mode']}")
        for d in rr_['details']:
            print('  ' + relation_line(d))
        print(f"  cvar matrix: true pairs={rr_['cvar_matrix']['true']['candidate_pairs']} details={rr_['cvar_matrix']['true']['detail_rows']} "
              f"| false pairs={rr_['cvar_matrix']['false']['candidate_pairs']} details={rr_['cvar_matrix']['false']['detail_rows']} "
              f"excludedByCvar={rr_['cvar_matrix']['false']['occurrences_excluded_by_cvar']} | same_as_default={rr_['cvar_matrix']['same_as_default']}")

    rc = EXIT_OK
    if a.coverage:
        rep = coverage_report(eng)
        print_coverage(rep)
        if not rep['complete']:
            rc = EXIT_INCOMPLETE

    if a.vfetch_census:
        seen = set()
        for d in eng.draws:
            if d['vs'] is None or d['vs'] in seen:
                continue
            seen.add(d['vs'])
            uc = eng.ucodes.get(d['vs'], b'')
            cf = parse_vs_vfetch(uc)
            naive = parse_vs_vfetch_naive(uc)
            nslots = {h['slot'] for h in naive}
            extra, missing = sorted(nslots - cf['vfetch_slots']), sorted(cf['vfetch_slots'] - nslots)
            print(f"  vs={d['vs']:016X} bytes={len(uc)} cfOk={int(cf['ok'])} "
                  f"vfetchSlots={sorted(cf['vfetch_slots'])} attrs={len(cf['attrs'])} "
                  f"bindings={len(cf['bindings'])} | naive={sorted(nslots)} "
                  f"EXTRA={extra} MISSING={missing}")
        vf = eng.vf
        print(f"  census over {vf['loads']} VS loads: divergent={vf['naive_divergent_loads']} "
              f"naiveExtra={vf['naive_extra']} naiveMissing={vf['naive_missing']} "
              f"naiveExtraFormats={dict(vf['naive_extra_fmt'])}")

    if a.draw is not None:
        if not (1 <= a.draw <= len(eng.draws)):
            print(f'draw {a.draw} out of range 1..{len(eng.draws)}', file=sys.stderr)
            return EXIT_USAGE
        d = eng.draws[a.draw - 1]
        print('\n' + fmt_row(d))
        r = d['regs']
        for reg in DRAW_REGS:
            v = r[reg]
            extra = f'  = {f32(v):g}' if 0x210F <= reg <= 0x2114 else ''
            print(f"  {REG_NAMES[reg]:<28} 0x{reg:04X} = {v:08X}{extra}")
        print(f"  bools = {' '.join('%08X' % b for b in d['bools'])}")
        vs_uc = eng.ucodes.get(d['vs'], b'') if d['vs'] is not None else b''
        cf = parse_vs_vfetch(vs_uc)
        if a.consts:
            lo, hi = (int(x) for x in a.crange.split(':'))
            print_consts(d, 'vs', (lo, hi))
            print_consts(d, 'ps', (lo, hi))
        if a.verts or a.dump_verts or cf['attrs']:
            print(f"  VS vfetches (Xenia CF walk; cfOk={int(cf['ok'])}):")
            slots = {}
            for v in cf['attrs']:
                fc = v['fetch_const']
                d0, d1 = d['fetch'][fc * 2], d['fetch'][fc * 2 + 1]
                addr, size, endian = (d0 & ~3) & PHYS_MASK, ((d1 >> 2) & 0xFFFFFF) * 4, d1 & 3
                slots.setdefault(fc, (addr, size, endian, v['stride']))
                print(f"    fc={fc:<2} fmt={VFMT.get(v['fmt'], v['fmt'])}({v['fmt']}) "
                      f"stride={v['stride']}dw offset={v['offset']}dw signed={v['signed']} "
                      f"nf={v['num_format']} dst=r{v['dst']} mini={v['mini']} slot={v['slot']} | "
                      f"fetchConst: type={d0 & 3} addr={addr:08X} size={size} endian={endian} "
                      f"mapped={eng.mem.has(addr)}")
            if a.verts and slots:
                for fc, (addr, size, endian, stride) in sorted(slots.items()):
                    sb = stride * 4
                    if not sb:
                        continue
                    n = min(a.verts, size // sb)
                    raw, missing = eng.mem.read(addr, n * sb)
                    print(f"  fc {fc}: first {n} vertices (stride {sb} B, BE dwords, and as f32)"
                          f"{'  [%d bytes UNMAPPED]' % missing if missing else ''}:")
                    for i in range(n):
                        dws = be32s(raw[i * sb:(i + 1) * sb])
                        print(f"    v{i}: {' '.join('%08X' % x for x in dws)} | f32: "
                              f"{' '.join(f'{f32(x):.4g}' for x in dws)}")
            if a.dump_verts and slots:
                fc0 = min(slots)
                addr, size, endian, stride = slots[fc0]
                raw, missing = eng.mem.read(addr, size)
                with open(a.dump_verts, 'wb') as f:
                    f.write(raw)
                print(f"  wrote {size} B of fetch-constant {fc0} vertex bytes -> {a.dump_verts}"
                      f"{'  [%d bytes UNMAPPED]' % missing if missing else ''}")
        if a.textures:
            row = next((r for r in eng.static_bindings if r['draw_ord'] == d['ord']), None)
            if row is None:
                print(f"  static texture bindings: none - draw kind '{d['kind']}' is not a normal draw in this census")
            else:
                st = row['stages']
                print(f"  static texture bindings ({row['analysis_status']}; vs={st['vs']['status']} ps={st['ps']['status']}) "
                      f"union={row['union_slots']} overlap={row['overlap_slots']} "
                      f"unusedType2={row['unused_type2_slots']} nonTexture={row['referenced_non_texture_slots']}")
                pt = row['post_translation']
                print(f"  post-translation ({pt['status']}; vs={pt['vs']['status']} ps={pt['ps']['status']}) "
                      f"requested={pt['requested_union']} overlap={pt['requested_overlap']} "
                      f"equalsAnalysis={pt['requested_equals_analysis_union']} "
                      f"reqNotAnalysis={pt['requested_minus_analysis']} analysisNotReq={pt['analysis_minus_requested']}")
                for name in ('vs', 'ps'):
                    if st[name]['reason']:
                        print(f"    {name}: {st[name]['status']} - {st[name]['reason']}")
                    key = st[name]['analysis_key']
                    preason = eng.shader_table[key]['post_translation'][name]['reason'] if key else st[name]['reason']
                    if preason:
                        print(f"    {name} post: {pt[name]['status']} - {preason}")
                    for ins in (eng.shader_table[key]['analysis']['instructions'] if key else []):
                        print(f"    {name} cf{ins['cf_index']} slot{ins['slot']} {ins['opcode_name']} fc={ins['fetch_constant']} "
                              f"dim={ins['dimension_name']} binding={int(ins['is_binding'])} "
                              f"writes={int(ins['writes_result'])} pred={ins['is_predicated']}")
                for b in row['bindings']:
                    t = eng.fetch_constants[b['fetch_constant_key']]['decoded']
                    nm = tfetch_names(t)
                    w, h, dp = tfetch_dims(t)
                    print(f"    t{b['slot']:<2} [{','.join(b['stages'])}] type={t['type_name']} {t['format_name']}({t['format']}) "
                          f"{t['dimension_name']} {w}x{h}x{dp} base={t['base_address_bytes']:08X} "
                          f"mip={t['mip_address_bytes']:08X} pitch={t['pitch_pixels']} tiled={t['tiled']} "
                          f"endian={nm['endian']} stacked={t['stacked']} packedMips={t['packed_mips']} "
                          f"swz={t['swizzle']:03X} mips={t['mip_min_level']}..{t['mip_max_level']} "
                          f"clamp=({t['clamp_x']},{t['clamp_y']},{t['clamp_z']}) "
                          f"filt=(mag{t['mag_filter']},min{t['min_filter']},mip{t['mip_filter']},aniso{t['aniso_filter']}) "
                          f"expAdj={t['exp_adjust']} lodBias={t['lod_bias']} mapped={eng.mem.has(t['base_address_bytes'])} "
                          f"requested={int(bool(b['requested_stages']))}{b['requested_stages']} raw={b['fetch_constant_key']}")
                for o in row['requested_binding_opportunities']:
                    bk = eng.fetch_constants[o['fetch_constant_key']]['backing']
                    bi, mi = bk['base_interval'], bk['mips_interval']
                    print(f"    requested t{o['slot']:<2} {o['stages']} backing={bk['status']} "
                          f"base={'[%08X,+%d)' % (bi['start'], bi['length']) if bi else '-'} "
                          f"mips={'[%08X,+%d)' % (mi['start'], mi['length']) if mi else '-'} usable={int(bk['join_usable'])}"
                          f"{'' if not bk['reason'] else ' - ' + bk['reason']}")
                rel = relation_for(eng, True)
                rs = rel['per_draw'][row['draw_ord']]
                print(f"  resolve relation ({rs['post_status']}): earlierProducers={rs['eligible_earlier_producers']} "
                      f"occ={rs['occurrences_eligible']}/{rs['requested_occurrences']} comps base={rs['components']['base']} "
                      f"mips={rs['components']['mips']} pairs={rs['candidate_pairs']} {dict((k, v) for k, v in rs['relations'].items() if v)} "
                      f"overlappingResolveDraws={rs['overlapping_producer_draw_ords']} (opportunities; no owner selected)")
                for d_ in rel['details']:
                    if d_['consumer']['draw_ord'] == row['draw_ord']:
                        print('    ' + relation_line(d_))
                if row['unused_type2_slots']:
                    print('  type-2 fetch constants NOT statically referenced by either shader (no consumer evidence):')
                    for slot in row['unused_type2_slots']:
                        t = decode_tfetch(d['fetch'][slot * 6:slot * 6 + 6])
                        w, h, dp = tfetch_dims(t)
                        print(f"    t{slot:<2} {t['format_name']}({t['format']}) {t['dimension_name']} {w}x{h}x{dp} "
                              f"base={t['base_address_bytes']:08X} raw={' '.join('%08X' % x for x in t['raw'])}")
        if a.dump_ucode:
            with open(a.dump_ucode + '_vs.bin', 'wb') as f:
                f.write(vs_uc)
            with open(a.dump_ucode + '_ps.bin', 'wb') as f:
                f.write(eng.ucodes.get(d['ps'], b'') if d['ps'] is not None else b'')
    return rc


if __name__ == '__main__':
    sys.exit(main())
