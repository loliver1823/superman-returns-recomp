#pragma once

// iter-463 runtime shader translation - the pure core (no logging, no ShaderCache, no guest
// memory, no threads). Built into the xenos_xlat library (cmake/xenos_xlat.cmake) next to
// XenosRecomp's recompiler; the game side is gfx/shader_runtime_xlat.cpp.
//
// Two translation paths, each a faithful copy of an offline tool:
//
//  EXACT  - a container whose ucode IS the missed ucode (every pixel shader: the XDK never
//           patches a PS; a VS the game binds unpatched). This is what tools/XenosRecomp-clone
//           main.cpp does per container: recompile -> DXC -> signed check -> (VS) the fetch-mode
//           variant, whose failure is non-fatal (VSF-SKIP).
//  PROVED - a vertex shader the XDK patched at bind time (vfetch stride/offset/format). This is
//           tools/stage_missing_vs.py: a template container proved to run the same program
//           (steps 1-3), the runtime ucode spliced into it (XXH3 re-checked), translated, and the
//           output checked (steps 4-6). Every check that stage_missing_vs.py makes is made here;
//           any failure refuses the shader.
//
// A shader that neither path proves is refused with a reason; it is never approximated.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sr {
namespace gfx {
namespace xlat {

// ── containers ───────────────────────────────────────────────────────────────
struct Container {
  std::vector<uint8_t> bytes;  // exactly virtualSize + physicalSize bytes
  std::string origin;          // file path, or "live VS #n" for a CreateShader capture
  bool isVs = false;
  uint32_t virt = 0, phys = 0, shOff = 0, po = 0, sz = 0;
  uint64_t ucodeHash = 0;           // XXH3-64 of the ucode: the shader cache key
  uint64_t fullHash = 0;            // XXH3-64 of all bytes (dedupe)
  uint64_t metaHash = 0;            // XXH3-64 of every byte outside the ucode
  bool keyed = false;               // VS: patch-masked ucode exists (control flow walks)
  std::vector<uint8_t> patchMasked; // VS: the ucode with the bind-time vfetch fields zeroed
  const uint8_t* ucode() const { return bytes.data() + virt + po; }
};

// Structural validation of a D3D9 Xbox 360 shader container, fault-free: every table
// XenosRecomp's recompile() walks (constant table and names, definition tables and their
// literal values, vertex elements / PS interpolators, the ucode) must lie inside the blob,
// and the ucode's control flow must walk (no exec slot past the ucode). n may exceed the
// container (a file with trailing bytes); out.bytes keeps only the container.
bool parseContainer(const uint8_t* p, size_t n, const std::string& origin, Container& out,
                    std::string* why);

// Every container bytes outside the ucode equal? (stage_missing_vs.py meta_bytes)
bool sameMetadata(const Container& a, const Container& b);

// stage_missing_vs.py PATCH_DW0/1/2: the fields the XDK rewrites at bind time in every
// EXECUTED vertex fetch - fetch constant (const_index, const_index_sel) and prefetch; dst_swiz,
// signedness, integer, signed_rf, format, exp_adjust, mini; stride, offset. Wider than
// gfx/shader_mkey.hpp's key on purpose (the mkey keeps dst_swiz/mini/prefetch); this one only
// finds candidates, the proof decides.
//
// iter-467 release/installer: the bind-time patch also (a) moves a stream's fetches to the
// vertex fetch constant D3D gives that stream (the compiler emits every fetch from constant 95,
// 31*3+2; a second stream's full fetch runs from 94), and (b) REORDERS the fetches of one
// contiguous fetch run so that each stream's full fetch comes first and its mini fetches follow
// by offset. Measured on the 25 metropol.AST world shaders the installed build left black
// (archive/r2/m1-rc1-iso): each runtime ucode is its disc container with exactly these changes,
// nothing else. So const_index/const_index_sel are patch fields, and the masked key sorts each
// contiguous run of executed vfetch slots (a permutation inside a run is the same program).
constexpr uint32_t kPatchDw0 = 0x38000000u | 0x07F00000u;  // prefetch | const_index_sel | const_index
constexpr uint32_t kPatchDw1 = 0x7F3F7FFFu & ~0x00008000u;
constexpr uint32_t kPatchDw2 = 0x7FFFFFFFu;
bool patchMaskedUcode(const uint8_t* u, size_t bytes, std::vector<uint8_t>& out);
// The executed vfetch slots grouped into runs of consecutive slot numbers (ascending).
std::vector<std::vector<uint32_t>> vfetchRuns(const std::vector<uint32_t>& slots);

class ContainerIndex {
 public:
  // Adds a validated container; false when an identical one is already indexed.
  bool add(Container&& c);
  size_t size() const { return all_.size(); }
  size_t vsCount() const { return nVs_; }
  // VS containers whose ucode has the same size and the same patch-masked bytes.
  void sameProgram(const std::vector<uint8_t>& masked, std::vector<const Container*>& out) const;
  // Containers of this kind whose ucode is exactly this.
  void exactUcode(uint64_t ucodeHash, bool isVs, std::vector<const Container*>& out) const;
  // iter-467: moves every container of `other` in (deduplicated as add() does); `other` is left
  // empty. Returns how many were new. (The runtime scans its corpus into a private index on a helper
  // thread and absorbs it, so a miss is never queued behind the scan.)
  size_t absorb(ContainerIndex& other);

 private:
  std::vector<std::unique_ptr<Container>> all_;
  std::unordered_multimap<uint64_t, size_t> byFull_, byMasked_, byUcode_;
  size_t nVs_ = 0;
};

// Every *.xpu file of one directory (sorted, not recursive), each file one container at offset
// 0 - the corpus rule of tools/shader_mkey.py iter_vs_containers (VS) extended to PS. A line
// "<count> <dir>" / "<dir>" file (stage_missing_vs.py corpus.txt) names several directories.
struct ScanStats { size_t files = 0, added = 0, invalid = 0; };
ScanStats scanContainerDir(const std::string& dir, ContainerIndex& index, int depth = 0);

// ── the control-flow walk (tools/xtr_draws.py cf_exec_walk; Xenia AnalyzeUcode) ──
struct CfSlot { uint32_t cfIndex, opcode, slot; bool isFetch, serialize; };
struct CfWalk {
  uint32_t bound = 0;
  bool sawExec = false, malformed = false;
  std::vector<CfSlot> slots;
};
CfWalk cfExecWalk(const uint8_t* u, size_t bytes);

// render/shader-coverage (Astra 2026-10-17, P1 on the ported reorder rule): a permutation of vertex fetches is the
// same program only where it cannot change what they compute. vfetchRunsByClause splits vfetchRuns at exec-clause
// boundaries (a run across a clause would move a fetch into or out of a conditional or predicated clause; a slot
// two clauses execute ends its run). fetchOrderKeepsDependences: the pairing of a run (runtime fetch i <- template
// fetch of the run at index from[i]) keeps the relative order of every two runtime fetches that depend on each other
// - one reads the index component (src_swiz) of a register the other writes, or both write one register in
// overlapping components (the runtime dst swizzles; every component but 7 = masked is a write, constants included),
// or either uses relative register addressing - so the reorder is a dependence-preserving permutation of one basic
// block: the same computation. *why names the first pair that breaks it.
std::vector<std::vector<uint32_t>> vfetchRunsByClause(const std::vector<uint32_t>& slots, const CfWalk& w);
bool fetchOrderKeepsDependences(const uint8_t* u, size_t bytes, const std::vector<uint32_t>& run,
                                const std::vector<size_t>& from, std::string* why);

// ── the same-program proof (stage_missing_vs.py prove_same_program) ─────────────
struct VfetchDiff {
  uint32_t instr = 0;
  std::string fields;  // "stride 7->9, fmt 25->37"
};
struct ProofReport {
  bool ok = true;
  uint32_t cfDwords = 0, alu = 0, tfetch = 0, vfetch = 0;
  std::vector<VfetchDiff> diffs;
  std::vector<std::string> problems;
};
ProofReport proveSameProgram(const uint8_t* t, const uint8_t* u, size_t bytes);

// VS vertex elements: instruction address -> (usage, usageIndex).
bool vertexElements(const Container& c, std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>>& out);

// ── recipes (iter-467 release/installer; data/shader_recipes.txt, tools/release/gen_shader_recipes.py) ──
// A release ships no game bytes, and the installer sees only the COMPILED vertex shaders, while the
// game draws with bind-time PATCHED variants (kPatchDw0). A "patch" recipe names a variant's key,
// the key of its template ucode, and per executed vertex fetch of the variant the template fetch it
// comes from plus the bits of the patch fields: applied to the player's own template (carved from
// the disc at install, or created by the game at run time) it rebuilds the variant, checked by hash,
// so the variant is translated before the game first draws it. An "xdk" recipe names an
// XDK-internal vertex shader D3D binds as raw ucode from default.xex (no container exists).
struct RecipeFetch { uint32_t slot = 0, from = 0, b0 = 0, b1 = 0, b2 = 0; };
struct Recipe {
  bool xdk = false;           // false: "patch"
  bool liveTemplate = false;  // the template is built by the game at run time (not on the disc)
  uint64_t key = 0, tmpl = 0;
  uint32_t bytes = 0;
  std::vector<RecipeFetch> fetches;
};
// Every well-formed line of the text; malformed lines are counted in *bad and skipped.
std::vector<Recipe> parseRecipes(const std::string& text, size_t* bad);
// The variant a patch recipe makes from this template ucode. Fails (with a reason) unless every
// named slot is an executed vertex fetch of the template, each "from" lies in the same contiguous
// fetch run as its slot, only patch-field bits are supplied, and the result hashes to r.key.
bool applyRecipe(const uint8_t* tmplUcode, size_t bytes, const Recipe& r, std::vector<uint8_t>& out,
                 std::string* why);
// A container around raw ucode that has none (an XDK-internal vertex shader): no constant table, no
// literal definitions, one vertex element per executed vertex fetch that writes a component, in
// program order POSITION0 then TEXCOORD0..15. The usage only NAMES the input: XenosRecomp reads the
// fetch through the element at its address and writes the same element into the sidecar the host
// fills it from, so any one-to-one naming is the same program; the fetch-mode variant reads none.
bool synthesizeRawVsContainer(const uint8_t* ucode, size_t bytes, const std::string& origin, Container& out,
                              std::string* why);

// ── post-translation checks ───────────────────────────────────────────────────
struct SidecarRow { int slot, offset, stride, format, isSigned, isInteger; };
std::vector<SidecarRow> parseSidecarRows(const std::string& text);
std::vector<SidecarRow> deriveFetches(const uint8_t* u, size_t bytes);  // derive_fetches(u)

// analyse_vsf over a DXIL disassembly: index multipliers, fetch-constant cbuffer rows, vertex
// memory load offsets (all sorted, unique).
struct VsfAnalysis { std::vector<int64_t> muls, regs, offs; };
VsfAnalysis analyseVsf(const std::string& disasm);
int fmtWords(int fmt);

// The input-signature rows (ISG1) of a DXIL container: name, index, mask, register, system
// value, component type (what the dxc -dumpbin "Input signature" table declares), plus the
// read mask ("used", informational). false when the container has no ISG1 part.
struct SigRow {
  std::string name;
  uint32_t index = 0, sysValue = 0, compType = 0, reg = 0, mask = 0, rwMask = 0;
  bool sameDecl(const SigRow& o) const {
    return name == o.name && index == o.index && sysValue == o.sysValue && compType == o.compType &&
           reg == o.reg && mask == o.mask;
  }
};
bool inputSignature(const std::vector<uint8_t>& dxil, std::vector<SigRow>& out);

// Lines added/removed between two texts (a minimal LCS line diff; each entry "+line"/"-line").
std::vector<std::string> diffLines(const std::string& a, const std::string& b);
bool isVfetchLine(const std::string& line);  // stage_missing_vs.py VF_LINE

// ── DXC (dxcompiler.dll loaded at runtime) ──────────────────────────────────────
// Process-wide load of <dir>/dxil.dll then <dir>/dxcompiler.dll (dxil.dll first, so the
// compiler signs with the copy next to it). Never throws; false + why when absent.
bool dxcLoad(const std::wstring& dir, std::string* why);
// dcb (observe-only): the first dxcLoad's own wall times on its calling thread - each LoadLibraryW, and hashing
// both DLLs (std::ifstream, 1 MiB reads; wrapper time, not disk service time) with the bytes hashed. All 0 until a
// dxcLoad got that far (a load that failed early leaves the later fields 0).
struct DxcLoadTiming {
  uint64_t loadDxilUs = 0, loadDxcUs = 0, hashUs = 0, hashBytes = 0;
};
DxcLoadTiming dxcLoadTiming();
bool dxcLoaded();
uint64_t dxcDllHash();        // XXH3-64 over dxcompiler.dll then dxil.dll bytes
std::string dxcVersion();     // "1.8 (commit ...)" when the DLL reports it

class Dxc {  // one IDxcCompiler3; use one per thread (offline: thread_local)
 public:
  Dxc();
  ~Dxc();
  Dxc(const Dxc&) = delete;
  Dxc& operator=(const Dxc&) = delete;
  bool ok() const { return impl_ != nullptr; }
  // dxc_compiler.cpp args, exactly: -T vs_6_0|ps_6_0, -HV 2021, -all-resources-bound,
  // -Wno-ignored-attributes, -Qstrip_reflect, -Qstrip_debug.
  bool compile(const std::string& hlsl, bool pixel, std::vector<uint8_t>& out, std::string* errors);
  bool disassemble(const std::vector<uint8_t>& dxil, std::string& out);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

// ── translation ───────────────────────────────────────────────────────────────
struct Files {
  bool isVs = false;
  std::vector<uint8_t> dxil;   // <h>.vs.dxil / <h>.ps.dxil
  std::string sidecar;         // <h>.vs.vfetch
  std::vector<uint8_t> vsf;    // <h>.vs.vsf.dxil (empty = VSF-SKIP)
  std::string vsfFailHlsl;     // the fetch-mode HLSL when its compile failed (offline .vsf.fail.hlsl)
  std::vector<uint8_t> at;     // <h>.ps.at.dxil, the alpha-test variant (0005; empty = none: see atSkip)
  std::string atSkip;          // step 4 lane C round 4: why a PS that writes colour 0 got no variant (offline:
                               // AT-SKIP); empty with `at` empty = the PS writes no colour 0. Not part of the
                               // agreement rule (the variant bytes are).
  std::string hlsl;            // the input-layout HLSL (not persisted by default)
  uint64_t mkey = 0;           // gfx/shader_mkey.hpp key of the ucode (0 = none)
  uint32_t filterOverrides = 0;  // texture fetches whose filter fields override the fetch constant (ignored:
                                 // one sampler per fetch slot; step-4 lane C round 3)
};

// What XenosRecomp's recompile() will read, checked without reading out of bounds (exec slots
// inside the ucode, a vertex element for every written vertex fetch - a PS has none - and element
// usages inside the recompiler's tables). translateContainer runs it first; false + why
// ("preflight: ...") = a container the release-build recompiler would read out of bounds on.
bool preflightContainer(const Container& c, std::string* why);

// Offline main.cpp per container. false + why = the preflight, or the offline SKIP reasons
// (recompiler threw, unsupported, dxc compile failed, DXIL not signed).
bool translateContainer(const Container& c, Dxc& dxc, Files& out, std::string* why);

// iter-467 release/installer (THE LITERAL RULE): `c` with its float4 literal definitions removed
// and the values they pointed at zeroed, so XenosRecomp reads those registers from the live
// constant buffer instead of baking them (Xenos has no immediate operand: the GPU takes a literal
// from the constant file, as Xenia does). `regs` names them ("c252..c255"). False = no float4
// literal, or a malformed definition table.
bool stripFloatLiterals(const Container& c, Container& out, std::string* regs);

// The installed (offline) cache, for the stage rules that prefer a cached sibling as template
// and reference, and for the reference re-translation control.
struct InstalledFiles {
  std::vector<uint8_t> dxil, vsf;
  std::string sidecar;
};
using InstalledLookup = std::function<bool(uint64_t hash, bool isVs, InstalledFiles* out)>;

enum class Status { Ok, NoTemplate, Refused, Failed };
const char* statusName(Status s);

struct Result {
  Status status = Status::Failed;
  std::string path;            // "exact" / "proved"
  std::string reason;          // why not Ok
  Files files;
  std::string manifest;        // provenance (<h>.vs.xlat / <h>.ps.xlat)
  std::vector<uint8_t> container;  // the container translated (spliced for "proved")
};

Result translate(uint64_t hash, bool isVs, const uint8_t* ucode, size_t bytes,
                 const ContainerIndex& index, Dxc& dxc, const InstalledLookup& installed);

}  // namespace xlat
}  // namespace gfx
}  // namespace sr
