#pragma once
// iter-466 controls2: calling a method of one of the game's own ActionScript objects from the host,
// the way the game's own C++ does (AptExecutor vt+0x1C / vt+0x20 -> AptAS2InvokeByPathCtxRaw
// sub_826BB990 -> AsInvokeByPath sub_826B8DC0): publish the executor's Apt context (0x829A17BC + the
// TLS slot), push the argument values on the AS stack, look the method up on the target
// (AptAS2GetVariableByPath sub_826B3998), push a register frame, AptAS2CallFunction sub_8268FB58
// with `this` = the target, pop the frame and the return value, restore the context.
//
// The one difference from the game's entry: the target can be an object pointer the host already
// holds (a focus manager, a slider, an IconMenu the mouse cursor's reader found), not only a path
// from _level0. Everything else is the game's own sequence, step for step.
//
// Guest thread only, while the AS interpreter is idle (the mouse cursor calls it right after the
// executor pump, kernel/apt_parse_trace.cpp). `ctx` is the hook's context: a frame is built below
// its r1 and r1 is restored. Kernel-only header.
#include <cstdint>
#include <string>
#include <vector>

struct PPCContext;

namespace sr::kernel::aptcall {

struct Arg {
  bool isString = false;
  int32_t i = 0;
  std::string s;
  static Arg Int(int32_t v) { Arg a; a.i = v; return a; }
  static Arg Str(std::string v) { Arg a; a.isString = true; a.s = std::move(v); return a; }
};

// obj.method(args...). obj = an AS value object (a MovieClip wrapper, an Object). Returns false
// (and calls nothing) when the executor is missing or an argument could not be made.
bool callMethod(PPCContext& ctx, uint8_t* base, uint32_t obj, const char* method,
                const std::vector<Arg>& args);
// <path from _level0>.method(args...) - exactly AsInvokeByPath (e.g. path "gEH", "_global.gFM").
bool callPath(PPCContext& ctx, uint8_t* base, const char* path, const char* method,
              const std::vector<Arg>& args);

}  // namespace sr::kernel::aptcall
