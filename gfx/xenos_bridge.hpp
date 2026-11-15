#pragma once

// iter-463 runtime shader translation: the one call into XenosRecomp's recompiler.
//
// xenos_bridge.cpp is compiled inside the xenos_xlat library with XenosRecomp's own settings
// (its pch.h force-included: full Windows.h, be<>, fmt) so no XenosRecomp header leaks into a
// game translation unit. Everything else (container proof, DXC, cache admission) lives in
// shader_xlat_core / shader_runtime_xlat and only sees this interface.
//
// Fidelity: recompile() does exactly what the offline tool (tools/XenosRecomp-clone,
// XenosRecomp/main.cpp) does per container - a value-initialised ShaderRecompiler, fetchMode set
// for the .vs.vsf.dxil variant, recompile(container, <shader_common.h bytes>) - with the same
// shader_common.h bytes (embedded at build time by cmake/xenos_embed.cmake).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sr {
namespace gfx {
namespace xenos {

struct RecompileOut {
  std::string hlsl;         // ShaderRecompiler::out (shader_common.h prepended)
  std::string sidecar;      // ShaderRecompiler::vfetchSidecar (<hash>.vs.vfetch text)
  std::string unsupported;  // ShaderRecompiler::unsupported (offline: a named SKIP)
  std::string error;        // a C++ exception the recompiler threw (offline: a SKIP)
  bool isPixelShader = false;
  uint32_t specConstantsMask = 0;
  uint32_t filterOverrides = 0;  // ShaderRecompiler::filterOverrides (0004 round 3): tfetch filter overrides, ignored
  bool alphaTestEmitted = false;  // ShaderRecompiler::alphaTestEmitted (0005): the alpha-test variant holds the test
};

// The caller has validated the container (magic, sizes, offsets in bounds): the recompiler
// itself does not bounds-check, and in-process an access violation is fatal (harness VEH).
// Returns false when the recompiler threw (error set). unsupported may be set on true.
// alphaTestMode (step 4 lane C, tools/xenosrecomp/0005): the pixel shader's alpha-test variant (<hash>.ps.at.dxil;
// main.cpp's second recompile of a pixel shader) - out.alphaTestEmitted says whether it holds the test.
bool recompile(const uint8_t* container, bool fetchMode, RecompileOut& out, bool alphaTestMode = false);

// Step 4 lane C (tools/xenosrecomp/0004): ShaderRecompiler::perOperandConstRelative for every later
// recompile() - true (the offline tool's value, a value-initialised recompiler) decides each constant
// operand's a0 / aL indexing per operand as Xenia does; false = the earlier rule. The game sets it once from
// SR_XLAT_CONST_REL before its first translation (shader_runtime_xlat.cpp); its runtime disk cache id
// includes the value.
void setConstRelativePerOperand(bool on);
bool constRelativePerOperand();

// The embedded shader_common.h bytes and the translator source id (SHA-256, hex).
std::string_view includeText();
const char* translatorSourceId();

}  // namespace xenos
}  // namespace gfx
}  // namespace sr
