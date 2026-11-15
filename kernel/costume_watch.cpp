// iter-465 fortress lane: OBSERVE-ONLY log of Superman costume switches.
//
// Retail strips LuaDebugPrint from its scripts (LuaStripDebugCalls_sub_82167A50), so a costume
// switch leaves no trace. These wrappers log, and change nothing:
//   821C6C00 RegistryEntrySetChildSlot108  actor+0x6C := VisRep     (ReplaceEntityVisRep's write)
//   821C6940 RegistryEntrySetChildSlot100  actor+0x64 := AnimSys    (actor construction only)
//   821D7830 EntitySpawner_SpawnSingle      the Lua SpawnEntity*/SpawnEntityActive spawn
// [COSTUME_WATCH] lines name the actor, the old/new pointer and the AnimSys project hash
// (+36, docs/ENGINE_MAP.md "Additive costume animation associations"), so a run proves which
// association a WEAR changed. Logged only around costume activity (a spawn of a Superman /
// SRM_ entity or a ReplaceEntityVisRep script call arms a 20 s window) and capped.
// SR_COSTUME_WATCH=0 silences it. Reads are hostRangeReadable-checked (fault-free).
#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>

extern "C" void __imp__sub_821C6C00(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821C6940(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821D7830(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821CCED0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82207C00(PPCContext& __restrict ctx, uint8_t* base);

namespace {
std::atomic<uint32_t> g_lines{0};
std::atomic<int64_t> g_armedUntilMs{0};

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool enabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_COSTUME_WATCH");
    return !e || e[0] != '0';
  }();
  return v;
}
// 400 lines per armed window: a world load's actor constructions must not use up the budget of
// a later costume switch (iter-465 charswitch).
bool budget() { return g_lines.fetch_add(1, std::memory_order_relaxed) < 400; }
void arm() {
  g_armedUntilMs.store(nowMs() + 20000, std::memory_order_relaxed);
  g_lines.store(0, std::memory_order_relaxed);
}
uint32_t rd(uint32_t va) {
  if (va < 0x10000u || !sr::kernel::isGuestVAInRange(va) ||
      !sr::kernel::hostRangeReadable(sr::kernel::guestVAToHost(va), 4)) return 0;
  return sr::kernel::readGuestU32BE(va);
}
void str(uint32_t va, char* out, size_t cap) {
  out[0] = 0;
  if (!va || !sr::kernel::isGuestVAInRange(va)) return;
  const uint8_t* p = sr::kernel::guestVAToHost(va);
  if (!sr::kernel::hostRangeReadable(p, 1)) return;
  size_t i = 0;
  for (; i + 1 < cap; ++i) {
    if (!sr::kernel::hostRangeReadable(p + i, 1) || !p[i] || p[i] < 32 || p[i] > 126) break;
    out[i] = static_cast<char>(p[i]);
  }
  out[i] = 0;
}
}  // namespace

PPC_FUNC(sub_821D7830) {
  char name[96];
  str(ctx.r4.u32, name, sizeof name);
  const bool costume = enabled() && (std::strstr(name, "Superman") || std::strncmp(name, "SRM_", 4) == 0);
  if (costume) arm();  // before: construction logs
  __imp__sub_821D7830(ctx, base);
  if (!costume) return;
  if (budget())
    SR_LOG_BOOT(sr::LogSeverity::Info, "[COSTUME_WATCH] spawn '%s' -> entity id %u", name, ctx.r3.u32);
}

// ScriptCmd ReplaceEntityVisRep (Lua): rare (the costume state, LoadMetropolis, Warworld).
PPC_FUNC(sub_821CCED0) {
  if (enabled()) {
    arm();
    if (budget()) SR_LOG_BOOT(sr::LogSeverity::Info, "[COSTUME_WATCH] Lua ReplaceEntityVisRep");
  }
  __imp__sub_821CCED0(ctx, base);
}

PPC_FUNC(sub_821C6940) {
  const uint32_t actor = ctx.r3.u32, anim = ctx.r4.u32;
  const uint32_t before = rd(actor + 100u);
  __imp__sub_821C6940(ctx, base);
  if (!enabled() || nowMs() > g_armedUntilMs.load(std::memory_order_relaxed) || !budget()) return;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[COSTUME_WATCH] actor %08X AnimSys +0x64 %08X -> %08X (project hash +36 %08X, visrep +0x6C %08X)",
              actor, before, rd(actor + 100u), anim ? rd(anim + 36u) : 0, rd(actor + 108u));
}

PPC_FUNC(sub_821C6C00) {
  const uint32_t actor = ctx.r3.u32;
  const uint32_t before = rd(actor + 108u), anim = rd(actor + 100u);
  __imp__sub_821C6C00(ctx, base);
  if (!enabled() || nowMs() > g_armedUntilMs.load(std::memory_order_relaxed) || !budget()) return;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[COSTUME_WATCH] actor %08X VisRep +0x6C %08X -> %08X (AnimSys +0x64 %08X project hash +36 %08X)",
              actor, before, rd(actor + 108u), anim, anim ? rd(anim + 36u) : 0);
}

// ScriptCmd GetWorldFlowBitfieldValue (Lua): the worn-suit check reads a save's costumeUnlocked
// bit with it (the game reads kittens / objectiveStatus bits with it too; those are not logged).
// Logs the arguments, the result count and the number pushed (lua_State
// +8 = top, TObject = {tt, float}: sub_8275CA38). The worn-suit check asks every tick, so only a
// changed answer is logged (capped at 200 lines).
PPC_FUNC(sub_82207C00) {
  static std::atomic<uint32_t> calls{0}, lines{0};
  static std::atomic<int> last{-2};
  const uint32_t L = ctx.r3.u32;
  // the 3 arguments sit just below top on entry (TObject {tt, value}; a TString's text at +16)
  const uint32_t top0 = rd(L + 8u);
  char flow[40] = {0}, attr[40] = {0};
  float bitf = -1.0f;
  if (top0 >= 24u) {
    str(rd(top0 - 20u) + 16u, flow, sizeof flow);
    str(rd(top0 - 12u) + 16u, attr, sizeof attr);
    const uint32_t b = rd(top0 - 4u);
    std::memcpy(&bitf, &b, 4);
  }
  __imp__sub_82207C00(ctx, base);
  const uint32_t n = calls.fetch_add(1, std::memory_order_relaxed);
  if (!enabled() || std::strcmp(attr, "costumeUnlocked") != 0) return;  // the game asks for other bits too
  const uint32_t pushed = ctx.r3.u32, top = rd(L + 8u);
  float v = -1.0f;
  if (pushed == 1 && top >= 8u) {
    const uint32_t bits = rd(top - 4u);
    std::memcpy(&v, &bits, 4);
  }
  const int now = static_cast<int>(v) * 100 + static_cast<int>(bitf);
  if (last.exchange(now, std::memory_order_relaxed) == now || lines.fetch_add(1, std::memory_order_relaxed) >= 200) return;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[COSTUME_WATCH] GetWorldFlowBitfieldValue('%s', '%s', %.0f) #%u -> %u value(s), %.0f (changed)",
              flow, attr, bitf, n, pushed, v);
}
