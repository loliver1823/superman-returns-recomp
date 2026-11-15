// iter-466 controls2: host -> ActionScript method calls, the game's own invoke sequence.
// Contract: apt_call.hpp. Every step below is a line of AsInvokeByPath_sub_826B8DC0 /
// AptAS2InvokeByPathCtxRaw_sub_826BB990 (IDA, 2026-10-06).
#include "apt_call.hpp"

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "collision_watch.hpp"
#include "pc_guest_access.hpp"
#include "host_automation.hpp"
#include "../app/log.hpp"

#include <cstring>

namespace sr::kernel::aptcall {

namespace {
constexpr uint32_t kExecutorGlobal = 0x8296EB80u;   // g_AptExecutor
constexpr uint32_t kExecCtxOff = 0xE0u;            // exec+224: its Apt context
constexpr uint32_t kPublishedCtx = 0x829A17BCu;    // the published Apt context
constexpr uint32_t kCtxTls = 0x829A1A5Cu;          // its TLS slot holder (sub_825F6AB8)
constexpr uint32_t kAsStack = 0x829A19B0u;         // AS operand stack: +0 count, +8 base
constexpr uint32_t kRegFrame = 0x829A1624u;        // register-frame top
constexpr uint32_t kRegFrameN = 0x829A1628u;       // registers in the current frame

using pcguest::Mem;

uint32_t rd(const Mem& memory, uint64_t va) { return memory.u32(va); }

// One guest call on the hook's thread: r1 = the scratch frame, r3.. = args.
template <typename F>
uint32_t call(const Mem& memory, PPCContext& ctx, uint8_t* base, uint32_t frame, F fn, uint32_t r3, uint32_t r4 = 0,
              uint32_t r5 = 0, uint32_t r6 = 0, uint32_t r7 = 0, uint32_t r8 = 0, uint32_t r9 = 0) {
  if (!memory.ok()) return 0;
  ctx.r1.u64 = frame;
  ctx.r3.u64 = r3; ctx.r4.u64 = r4; ctx.r5.u64 = r5; ctx.r6.u64 = r6;
  ctx.r7.u64 = r7; ctx.r8.u64 = r8; ctx.r9.u64 = r9;
  fn(ctx, base);
  return ctx.r3.u32;
}

// A virtual call obj->vt[slot](obj) (AddRef = slot 0, Release = slot 1).
void vcall(const Mem& memory, PPCContext& ctx, uint8_t* base, uint32_t frame, uint32_t obj, uint32_t slot) {
  const uint32_t vt = rd(memory, obj);
  const uint32_t fn = vt ? rd(memory, uint64_t(vt) + 4ull * slot) : 0;
  PPCFunc* f = fn ? PPC_LOOKUP_FUNC_SAFE(base, fn) : nullptr;
  if (!f || !memory.ok()) return;
  ctx.r1.u64 = frame;
  ctx.r3.u64 = obj;
  f(ctx, base);
}

bool invoke(PPCContext& ctx, uint8_t* base, const char* path, uint32_t obj, const char* method,
            const std::vector<Arg>& args) {
  // a port feature: never on natural rolls (charter 2a; the callers gate too)
  if (!automation::portFeaturesOn()) return false;
  const Mem memory(base, true);
  if (!memory.ready() || !method) return false;
  const uint32_t exec = rd(memory, kExecutorGlobal);
  const size_t nameLen = std::strlen(method);
  const size_t pathLen = path ? std::strlen(path) : 0;
  if (!exec || nameLen == 0 || nameLen > 60 || pathLen > 120 || args.size() > 6 ||
      ctx.r1.u32 < 0x4000u || sr::kernel::g_dispatchTable == nullptr) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[APT_CALL] %s.%s not called (executor=%08X)",
                path ? path : "obj", method, exec);
    return false;
  }
  const uint32_t savedR1 = ctx.r1.u32;
  const PPCContext saved = ctx;
  const uint32_t frame = (savedR1 - 0xC00u) & ~0xFu;
  if (!memory.range(frame, 0xC00u) || !memory.range(kPublishedCtx, 4) ||
      !memory.range(kAsStack, 12) || !memory.range(kRegFrame, 8)) return false;
  memory.put32(frame, savedR1);                          // back chain
  const uint32_t nameVA = frame + 0x300u;
  const uint32_t pathVA = frame + 0x380u;
  const uint32_t nameHolder = frame + 0x2F0u, pathHolder = frame + 0x2F4u;
  memory.write(nameVA, method, nameLen + 1);
  if (path) memory.write(pathVA, path, pathLen + 1);
  uint32_t strVA = frame + 0x400u;

  // the argument values (the callers of the executor entry build them first, as here)
  std::vector<uint32_t> vals;
  for (const Arg& a : args) {
    uint32_t v = 0;
    if (a.isString) {
      const size_t n = std::min<size_t>(a.s.size(), 200);
      if (n) memory.write(strVA, a.s.data(), n);
      memory.put32(strVA + uint32_t(n), 0);
      v = call(memory, ctx, base, frame, sub_8269A210, strVA);   // AptObjectRecycleOrAlloc (C string)
      strVA += uint32_t((n + 16) & ~size_t(15));
    } else {
      v = call(memory, ctx, base, frame, sub_82693028, uint32_t(a.i));   // AptAS2AllocIntObj
    }
    if (!v) { ctx = saved; return false; }
    vals.push_back(v);
  }

  // AptAS2InvokeByPathCtxRaw: publish the executor's context
  const uint32_t prevCtx = rd(memory, kPublishedCtx);
  const uint32_t aptCtx = rd(memory, uint64_t(exec) + kExecCtxOff);
  if (!memory.ok()) { ctx = saved; return false; }
  memory.put32(kPublishedCtx, aptCtx);
  call(memory, ctx, base, frame, sub_825F6AB8, kCtxTls, aptCtx);

  // AsInvokeByPath: the target
  uint32_t target = obj;
  if (path) {
    call(memory, ctx, base, frame, sub_8268F3E8, pathHolder, pathVA);           // AptStringAssignCStr
    const uint32_t level0 = call(memory, ctx, base, frame, sub_826B05F0, 0);    // AptGetOrCreateLevelClip
    target = call(memory, ctx, base, frame, sub_826B3998, kAsStack, level0, 0, pathHolder, 1, 1, 0);
    call(memory, ctx, base, frame, sub_8229FD48, pathHolder);                  // the string's release
  }
  bool ok = target != 0;
  if (ok) {
    // push the arguments, last first, each with an AddRef (vt slot 0)
    for (size_t k = vals.size(); k-- > 0;) {
      const uint32_t n = rd(memory, kAsStack);
      const uint32_t stackBase = rd(memory, kAsStack + 8);
      const uint64_t destination = uint64_t(stackBase) + 4ull * n;
      if (n == UINT32_MAX || !memory.ok() || !memory.range(destination, 4)) { ok = false; break; }
      memory.put32(destination, vals[k]);
      memory.put32(kAsStack, n + 1);
      vcall(memory, ctx, base, frame, vals[k], 0);
    }
    if (ok && memory.ok()) {
      // the method, looked up on the target
      call(memory, ctx, base, frame, sub_8268F3E8, nameHolder, nameVA);
      const uint32_t fn = call(memory, ctx, base, frame, sub_826B3998, kAsStack, target, 0, nameHolder, 1, 1, 0);
      // a register frame for the call, the call, the frame's pop
      const uint32_t regTop = rd(memory, kRegFrame);
      const uint64_t nextRegTop = uint64_t(regTop) + 4ull * rd(memory, kRegFrameN);
      if (nextRegTop <= UINT32_MAX && memory.ok()) {
        memory.put32(kRegFrame, uint32_t(nextRegTop));
        memory.put32(kRegFrameN, 0);
        call(memory, ctx, base, frame, sub_8268FB58, kAsStack, target, fn, uint32_t(args.size()), 0);
        call(memory, ctx, base, frame, sub_8267F2E8, regTop);                      // AptAS2RegisterFramePop
      } else ok = false;
      // the return value: Release (vt slot 1) and pop
      const uint32_t n = rd(memory, kAsStack);
      if (static_cast<int32_t>(n) > 0) {
        const uint32_t top = rd(memory, uint64_t(rd(memory, kAsStack + 8)) + 4ull * (n - 1));
        if (top) vcall(memory, ctx, base, frame, top, 1);
        const uint32_t currentCount = rd(memory, kAsStack);
        if (memory.ok() && currentCount) memory.put32(kAsStack, currentCount - 1);
        else ok = false;
      }
      call(memory, ctx, base, frame, sub_8229FD48, nameHolder);
    }
  }

  ok = ok && memory.ok();
  // Restore the known saved context even if a later dynamic read was refused.
  if (memory.restore32(kPublishedCtx, prevCtx)) {
    // Only the saved context and fixed TLS holder enter this cleanup call.
    // The original operation lease still protects its scratch and guest call.
    ctx.r1.u64 = frame;
    ctx.r3.u64 = kCtxTls; ctx.r4.u64 = prevCtx;
    ctx.r5.u64 = 0; ctx.r6.u64 = 0; ctx.r7.u64 = 0; ctx.r8.u64 = 0; ctx.r9.u64 = 0;
    sub_825F6AB8(ctx, base);
  } else ok = false;
  ctx = saved;
  ctx.r1.u64 = savedR1;
  return ok;
}
}  // namespace

bool callMethod(PPCContext& ctx, uint8_t* base, uint32_t obj, const char* method,
                const std::vector<Arg>& args) {
  if (!obj) return false;
  return invoke(ctx, base, nullptr, obj, method, args);
}

bool callPath(PPCContext& ctx, uint8_t* base, const char* path, const char* method,
              const std::vector<Arg>& args) {
  return invoke(ctx, base, path, 0, method, args);
}

}  // namespace sr::kernel::aptcall
