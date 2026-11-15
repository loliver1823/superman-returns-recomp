// iter-463 runtime shader translation - see xenos_bridge.hpp.
// Compiled ONLY inside the xenos_xlat library (cmake/xenos_xlat.cmake): XenosRecomp's pch.h is
// force-included, so this file sees ShaderRecompiler exactly as the offline main.cpp does.
#include "shader_recompiler.h"

#include "gfx/xenos_bridge.hpp"

#include <atomic>
#include <exception>

namespace sr {
namespace gfx {
namespace xenos {

namespace {
std::atomic<bool> g_perOperandConstRelative{true};  // the offline tool's value (0004)
}  // namespace

void setConstRelativePerOperand(bool on) { g_perOperandConstRelative.store(on, std::memory_order_relaxed); }
bool constRelativePerOperand() { return g_perOperandConstRelative.load(std::memory_order_relaxed); }

extern const unsigned char kShaderCommonH[];
extern const size_t kShaderCommonHSize;
extern const char kTranslatorSourceId[];

std::string_view includeText() {
  return std::string_view(reinterpret_cast<const char*>(kShaderCommonH), kShaderCommonHSize);
}

const char* translatorSourceId() { return kTranslatorSourceId; }

bool recompile(const uint8_t* container, bool fetchMode, RecompileOut& out, bool alphaTestMode) {
  out = RecompileOut{};
  // main.cpp: `recompiler = {};` (then `fetchRecompiler.fetchMode = true;` for the vsf variant,
  // `atRecompiler.alphaTestMode = true;` for a pixel shader's alpha-test variant, 0005).
  ShaderRecompiler r{};
  r.fetchMode = fetchMode;
  r.alphaTestMode = alphaTestMode;
  r.perOperandConstRelative = constRelativePerOperand();
  try {
    r.recompile(container, includeText());
  } catch (const std::exception& e) {
    out.error = e.what();
    return false;
  } catch (...) {
    out.error = "unknown C++ exception";
    return false;
  }
  out.hlsl = std::move(r.out);
  out.sidecar = std::move(r.vfetchSidecar);
  out.unsupported = std::move(r.unsupported);
  out.isPixelShader = r.isPixelShader;
  out.specConstantsMask = r.specConstantsMask;
  out.filterOverrides = r.filterOverrides;
  out.alphaTestEmitted = r.alphaTestEmitted;
  return true;
}

}  // namespace xenos
}  // namespace gfx
}  // namespace sr
