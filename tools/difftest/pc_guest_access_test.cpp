// Standalone SR_ORACLE=0 fixture: actual host feature implementations and actual
// hardware protection/ArenaLease, with only guest calls and arena registration
// supplied here. No game, GPU, or guest interpreter is run. Full release linkage
// and guest_protect_census separately verify the production lifetime contract.
#include "guest/ppc/ppc_recomp_shared.h"
#include "kernel/pc_guest_access.hpp"
#include "kernel/pc_controls_game.hpp"
#include "kernel/pc_actions.hpp"
#include "kernel/pc_moves.hpp"
#include "kernel/apt_call.hpp"
#include "kernel/native_hardware_memory.hpp"
#include "app/log.hpp"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

static_assert(SR_ORACLE == 0, "exercise the actual release admission path");
namespace K = sr::kernel;
namespace P = K::guest_protect;
namespace PC = K::pccontrols;
namespace PA = K::pcactions;
namespace M = K::pcmoves;
namespace {
uint8_t* arena = nullptr;
size_t arenaSize = 0x100000000ull;
unsigned passed = 0, failed = 0, cameraCalls = 0, nudgeCalls = 0, aptCalls = 0, tlsCalls = 0;
bool allDelegatesLeased = true, retireInAllocation = false, retirementBlocked = false;
void check(bool value, const char* name) {
  ++(value ? passed : failed);
  std::printf("%s %s\n", value ? "PASS" : "FAIL", name);
}
void w32(uint32_t va, uint32_t value) { value = __builtin_bswap32(value); std::memcpy(arena + va, &value, 4); }
uint32_t r32(uint32_t va) { uint32_t value; std::memcpy(&value, arena + va, 4); return __builtin_bswap32(value); }
void wf(uint32_t va, float value) { uint32_t bits; std::memcpy(&bits, &value, 4); w32(va, bits); }
float rf(uint32_t va) { const uint32_t bits = r32(va); float value; std::memcpy(&value, &bits, 4); return value; }
void leased() { allDelegatesLeased &= P::arenaLeases() != 0; }
void virtualCall(PPCContext&, uint8_t*) { leased(); }
}

void sr::Log::write(LogCategory, LogSeverity, const char* format, ...) {
  va_list args; va_start(args, format); std::vprintf(format, args); va_end(args); std::puts("");
}
namespace sr::kernel {
bool g_physUnifyActive = false;
PPCFunc* fixtureDispatch[] = {virtualCall};
PPCFunc** g_dispatchTable = fixtureDispatch;
uint8_t* getGuestBase() { return arena; }
void getGlobalGuestMemory(uint8_t** base, size_t* size) { *base = arena; *size = arenaSize; }
uint8_t* guestVAToHost(uint32_t va) {
  if (va >= 0xC0000000u) va -= 0x40000000u;
  return va < arenaSize ? arena + va : nullptr;
}
namespace pccontrols { bool enabled() { return true; } }
namespace automation { bool portFeaturesOn() { return true; } }
}
PPC_FUNC(__imp__sub_821855B8) { (void)ctx; (void)base; ++cameraCalls; }
PPC_FUNC(__imp__sub_8221D2C8) { (void)ctx; (void)base; }
PPC_FUNC(__imp__sub_82224A90) { (void)ctx; (void)base; }
PPC_FUNC(__imp__sub_82721E68) { (void)ctx; (void)base; }
PPC_FUNC(__imp__sub_82722DB8) { (void)ctx; (void)base; }
PPC_FUNC(sub_821855B8);
PPC_FUNC(sub_821EAA18) {
  (void)base; leased(); ++nudgeCalls;
  check(r32(ctx.r1.u32) == 0xBE001000u && rf(ctx.r6.u32) == 1.0f &&
        rf(ctx.r6.u32 + 12) == 0.0f, "actual move writes scratch backchain/direction before guest call");
}
PPC_FUNC(sub_8269A210) { (void)base; leased(); ctx.r3.u64 = 0x43000u; }
PPC_FUNC(sub_82693028) {
  (void)base; leased();
  if (retireInAllocation) retirementBlocked = !P::retireArena(0);
  ctx.r3.u64 = 0x43000u;
}
PPC_FUNC(sub_825F6AB8) { (void)ctx; (void)base; leased(); ++tlsCalls; }
PPC_FUNC(sub_8268F3E8) { (void)ctx; (void)base; leased(); }
PPC_FUNC(sub_826B05F0) { (void)base; leased(); ctx.r3.u64 = 0x42000u; }
PPC_FUNC(sub_826B3998) { (void)base; leased(); ctx.r3.u64 = 0x42000u; }
PPC_FUNC(sub_8229FD48) { (void)ctx; (void)base; leased(); }
PPC_FUNC(sub_8268FB58) { (void)ctx; (void)base; leased(); ++aptCalls; }
PPC_FUNC(sub_8267F2E8) { (void)ctx; (void)base; leased(); }

int main(int argc, char** argv) {
  const bool omitProtection = argc == 2 && !std::strcmp(argv[1], "--control=omit-protection");
  const bool wholeArena = argc == 2 && !std::strcmp(argv[1], "--control=whole-arena-refusal");
  if (argc > 1 && !omitProtection && !wholeArena) return 2;
  _putenv_s("SR_PC_MOVES", "1");
  for (const char* gate : P::kDiagnosticGates) _putenv_s(gate, "");
  _putenv_s(P::kDiagnosticAddress, "");
  arena = static_cast<uint8_t*>(VirtualAlloc(nullptr, arenaSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (!arena) { std::printf("fixture allocation failed %lu\n", GetLastError()); return 2; }
  constexpr uint32_t hole = K::kHardwareMemoryBase;
  w32(hole - 4, 0xAABBCCDDu);
  w32(0x10010u, 0xCAFEBABEu);
  w32(0x80000000u, 0x12345678u);
  // A camera starts in valid writeback RAM but its final fields cross MMIO.
  w32(hole - 0x100u, 0x82005DC8u);
  uint32_t error = 0;
  if (!omitProtection && !K::protectNativeHardwareMemory(arena, arenaSize, &error)) return 2;
  if (wholeArena) P::declare(arena, arenaSize, P::Scope::All);
  MEMORY_BASIC_INFORMATION mbi{};
  check(VirtualQuery(arena + hole, &mbi, sizeof mbi) && mbi.Protect == PAGE_NOACCESS,
        "actual hardware aperture stays PAGE_NOACCESS");
  char why[180]{};
  check(K::pcguest::ready(arena, why, sizeof why), "native PC admission survives unrelated protected aperture");
  {
    K::pcguest::Mem m(arena);
    check(m.u32(hole - 4) == 0xAABBCCDDu && m.u32(0x80000000u) == 0x12345678u,
          "both adjacent RAM regions remain readable");
    check(m.put32(0x10010u, 0x10203040u) && r32(0x10010u) == 0x10203040u,
          "ordinary BE write succeeds");
  }
  for (const auto [va, length] : std::vector<std::pair<uint64_t, size_t>>{
           {hole, 1}, {hole - 1u, 2}, {0x7FFFFFFFu, 2}, {0x100010010ull, 4},
           {0xFFFFFFFEull, 4}, {UINT64_MAX, 8}, {0x10010u, SIZE_MAX}}) {
    K::pcguest::Mem m(arena);
    check(!m.range(va, length) && !m.ok(), "protected/crossing/wrapped extent refused before access");
  }
  {
    K::pcguest::Mem m(arena);
    const uint32_t before = r32(0x10010u);
    check(!m.copy(0x10010u, hole - 2u, 4) && r32(0x10010u) == before,
          "failed camera-style copy leaves destination unchanged");
  }
  { K::pcguest::Mem m(arena + 4096); check(!m.ready(), "foreign arena refused"); }
  K::g_physUnifyActive = true;
  { K::pcguest::Mem m(arena); check(!m.ready(), "unified reservation refuses flat admission"); }
  K::g_physUnifyActive = false;
  arenaSize = 0x20000;
  { K::pcguest::Mem m(arena); check(!m.range(0x1FFFF, 2), "registered arena end bounds complete extent"); }
  arenaSize = 0x100000000ull;
  {
    K::pcguest::Mem m(arena, true);
    check(m.put32(0xC0010010u, 0x55667788u) && r32(0x80010010u) == 0x55667788u,
          "mapped host writes use the admitted translated endpoints");
  }

  // Actual readGameView: the mandatory aperture must not disable gameplay.
  w32(0x8296D884u, 0x12000u); w32(0x12000u + 28, 1); w32(0x12000u + 32, 0x13000u);
  w32(0x13000u + 8, 0); w32(0x8296E8C8u, 0x20200u); w32(0x20200u + 12, 0);
  auto view = PC::readGameView();
  check(view.valid && view.ctx == PC::Context::Gameplay, "actual game reader reports gameplay with MMIO closed");
  w32(0x12000u + 32, hole - 8u);
  check(!PC::readGameView().valid, "partial inaccessible slot cannot become zero-valued gameplay");
  w32(0x12000u + 32, 0xFFFFFFF8u);
  check(!PC::readGameView().valid, "derived slot offset cannot wrap into low memory");
  w32(0x12000u + 32, 0x13000u);

  constexpr uint32_t camera = 0x14000u;
  w32(camera, 0x82005DC8u); wf(camera + 160, 0); wf(camera + 164, 0);
  wf(camera + 628, -1); wf(camera + 632, 1);
  std::memcpy(arena + camera + 96, "camera-reference", 16);
  view = PC::readGameView();
  PC::publishFootLook(0.25f, 0.5f, view.poll);
  PPCContext ctx{}; ctx.r3.u64 = camera;
  sub_821855B8(ctx, arena);
  check(cameraCalls == 1 && rf(camera + 164) == 0.25f && rf(camera + 160) == 0.5f &&
        !std::memcmp(arena + camera + 64, arena + camera + 96, 16),
        "actual camera hook writes yaw/pitch/reference and delegates exactly once");
  PC::publishFootLook(1, 1, view.poll); ctx.r3.u64 = hole - 0x100u;
  sub_821855B8(ctx, arena);
  check(cameraCalls == 2 && r32(hole - 4) == 0xAABBCCDDu,
        "camera crossing protected span refuses writes and still delegates once");
  ctx.r3.u64 = camera; sub_821855B8(ctx, arena);

  // Real action mirror publication and restoration, including the direct u16 path.
  constexpr uint32_t input = 0x20000u, map = 0x20100u, context = 0x20300u, array = 0x20400u;
  constexpr uint32_t state = 0x20500u, bits = 0x20600u, axes = 0x20700u, command = 0x20800u;
  w32(0x829A2C04u, input); w32(input + 4, 1); w32(input + 12, map);
  arena[map + 16] = 1; w32(map + 20, context); w32(context + 4, 1); w32(context + 8, array);
  w32(array, command); w32(state + 8, 1); w32(state + 0x14, 16); w32(state + 0x20, bits); w32(state + 0x28, axes);
  w32(command, 0x8208CF34u); w32(command + 4, PA::kCtlX); arena[command + 18] = 1;
  check(PA::readyFor(arena), "actual action admission remains enabled");
  PA::noteContextUpdate(arena, context, state); PA::afterDispatch(arena);
  PA::ControlFrame down; down.buttons = 1u << PA::kCtlX; PA::publish(down);
  PA::noteContextUpdate(arena, context, state); PA::afterDispatch(arena);
  check(arena[command + 16] == 1 && arena[command + 17] == 255, "actual action publishes completed button writes");
  PA::beforeDispatch(arena);
  check(arena[command + 16] == 0 && arena[command + 17] == 0, "actual action restoration uses bounded writes");
  PA::resetForTest(); std::memset(arena + command, 0, 40);
  w32(command, 0x8208CE5Cu); w32(command + 20, 2); arena[command + 19] = 250;
  PA::noteContextUpdate(arena, context, state); PA::afterDispatch(arena);
  down = {}; down.axisDriven = 1; down.axis[0] = 1000; PA::publish(down);
  PA::noteContextUpdate(arena, context, state); PA::afterDispatch(arena);
  check(arena[command + 28] == 1 && arena[command + 16] == 3 && arena[command + 17] == 232,
        "actual action publishes BE axis halfword");
  PA::beforeDispatch(arena);
  check(arena[command + 16] == 0 && arena[command + 17] == 0, "actual action restores BE axis halfword");
  PA::resetForTest(); w32(array, hole - 16u);
  PA::noteContextUpdate(arena, context, state); PA::afterDispatch(arena);
  check(r32(hole - 4) == 0xAABBCCDDu, "partial protected command cannot produce writes");
  w32(array, command);

  // Real optional move path, including its writes into the high guest stack.
  constexpr uint32_t actor = 0x30000, proxy = 0x31000, velocity = 0x31100, mover = 0x32000;
  w32(actor + 0x70, proxy); w32(proxy + 4, velocity); arena[proxy + 15] = 4;
  wf(actor + 0x14, 1); w32(mover + 0x90, 2); wf(mover + 0x44, -1);
  w32(0x8296DF58u, 0x33000); w32(0x33000, 0x33100); w32(0x33004, 1);
  w32(0x33100, 0x33200); w32(0x33208, 0x33300); std::memcpy(arena + 0x33300, "SupermanFlight", 15);
  ctx = {}; ctx.r1.u64 = 0xBE001000u; ctx.r3.u64 = 0x87654321u; const PPCContext saved = ctx;
  M::requestAirDodge(1, 0, 20); M::onSupermanFly(ctx, arena, mover, actor, 20);
  check(nudgeCalls == 1 && !std::memcmp(&ctx, &saved, sizeof ctx), "actual move calls once under lease and restores registers");
  ctx.r1.u64 = hole + 0x100u;
  M::requestAirDodge(1, 0, 40); M::onSupermanFly(ctx, arena, mover, actor, 40);
  ctx.r1.u64 = 0x80;
  M::requestAirDodge(1, 0, 60); M::onSupermanFly(ctx, arena, mover, actor, 60);
  check(nudgeCalls == 1, "protected and underflowed move scratch refuse guest invocation");

  // Real apt_call host accesses and restoration, with guest behavior doubled.
  w32(0x8296EB80u, 0x40000); w32(0x400E0, 0x12340000); w32(0x829A17BCu, 0x56780000);
  w32(0x829A19B0u, 0); w32(0x829A19B8u, 0x45000); w32(0x829A1624u, 0x46000); w32(0x829A1628u, 0);
  w32(0x43000, 0x44000); w32(0x44000, 0x82001000);
  ctx = {}; ctx.r1.u64 = 0xBE002000u; ctx.r3.u64 = 99; const PPCContext aptSaved = ctx;
  check(K::aptcall::callMethod(ctx, arena, 0x42000, "Run", {}) && aptCalls == 1 && tlsCalls == 2 &&
        r32(0x829A17BCu) == 0x56780000 && !std::memcmp(&ctx, &aptSaved, sizeof ctx),
        "actual Apt invocation writes bounded scratch and restores known context");
  w32(0x8296EB80u, hole - 16u);
  check(!K::aptcall::callMethod(ctx, arena, 0x42000, "Run", {}) && aptCalls == 1,
        "Apt executor derived offset refuses protected read");
  w32(0x8296EB80u, 0x40000); w32(0x829A19B8u, 0xFFFFFFF0u); w32(0x829A19B0u, 0x4008u);
  const uint32_t lowBefore = r32(0x10010u);
  K::aptcall::Arg argument; argument.i = 1;
  check(!K::aptcall::callMethod(ctx, arena, 0x42000, "Run", {argument}) && aptCalls == 1 &&
        r32(0x10010u) == lowBefore && r32(0x829A17BCu) == 0x56780000,
        "Apt stack offset cannot wrap and known published context is restored after refusal");
  check(allDelegatesLeased, "every feature-initiated guest call retains the operation lease");
  check(VirtualQuery(arena + hole, &mbi, sizeof mbi) && mbi.Protect == PAGE_NOACCESS,
        "no feature operation reopens hardware protection");
  check(P::arenaLeases() == 0, "completed operations release every lease");
  // Retirement begins while an Apt operation is in flight, followed by a
  // dynamic stack refusal. Its known cleanup must use that original lease.
  retireInAllocation = true;
  check(!K::aptcall::callMethod(ctx, arena, 0x42000, "Run", {argument}) && retirementBlocked &&
        r32(0x829A17BCu) == 0x56780000 && !std::memcmp(&ctx, &aptSaved, sizeof ctx),
        "retiring failed Apt operation restores through its original lease");
  { K::pcguest::Mem retired(arena); check(!retired.ready(), "retired arena refuses new feature operation"); }
  check(P::retireArena(0), "teardown succeeds after operation lease drains");
  VirtualFree(arena, 0, MEM_RELEASE); arena = nullptr;
  std::printf("[PC_GUEST_ACCESS_TEST] passed=%u failed=%u\n", passed, failed);
  return failed ? 1 : 0;
}
