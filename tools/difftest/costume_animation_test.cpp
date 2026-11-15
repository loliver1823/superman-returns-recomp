// Bounded animation lifecycle experiment, NOT a shipping costume-switch API.
// Run: build/difftest.exe --costume-animation-test (exit = failed checks).
//
// Real recompiled bodies: project bind/attach/detach, player-buffer destructor,
// player reset, state-vector release, registration dispatch, skeleton getter,
// same-size pose-storage refresh, and pause-flag setter (addresses below).
// Substituted leaves ONLY: synthetic heap vt+44 records frees without freeing;
// synthetic runtime vt+24/+28 records registration. No strong guest overrides.
// Projects have a real-layout 103-bone skeleton and EMPTY resource iterators:
// this does NOT decode banks, evaluate clips, validate authored offsets, prove
// a natural action-boundary hook, or establish live FSM/action continuity.
// The host-authored candidate transaction stays local to this test file.
#include "ppc/ppc_recomp_shared.h"
#include "kernel/guest_context.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" {
PPC_FUNC(__imp__sub_8270D170);
PPC_FUNC(__imp__sub_8213D1F0);
PPC_FUNC(__imp__sub_82140230);
PPC_FUNC(__imp__sub_821487B0);
PPC_FUNC(__imp__sub_82709CB8);
PPC_FUNC(__imp__sub_8270EE50);
PPC_FUNC(__imp__sub_82709D88);
PPC_FUNC(__imp__sub_82709DE8);
PPC_FUNC(__imp__sub_8270A6D8);
PPC_FUNC(__imp__sub_8213D5E8);
PPC_FUNC(__imp__sub_8270ABE0);
}

namespace K = sr::kernel;
namespace {
constexpr uint32_t kArena = 0x90900000u, kArenaSize = 0x20000u;
constexpr uint32_t kActor = kArena, kController = kArena + 0x200;
constexpr uint32_t kPlayer = kArena + 0x400, kOtherPlayer = kArena + 0x800;
constexpr uint32_t kPose = kArena + 0xB00, kContext = kArena + 0xC00;
constexpr uint32_t kFsm = kArena + 0xD00, kHeap = kArena + 0xE00;
constexpr uint32_t kHeapVtable = kArena + 0xF00, kHeapRanges = kArena + 0x1000;
constexpr uint32_t kRuntime = kArena + 0x1100, kRuntimeVtable = kArena + 0x1200;
constexpr uint32_t kRegistry = kArena + 0x1400, kRegistryEntries = kArena + 0x1420;
constexpr uint32_t kBuffers = kArena + 0x10000, kStack = kArena + 0x1F000;
constexpr uint32_t kFreeLeaf = 0x82110040u, kRegisterLeaf = 0x82110048u;
constexpr uint32_t kUnregisterLeaf = 0x82110050u;
constexpr uint32_t kBones = 103;
constexpr std::array<uint32_t, 3> kTransitions{132, 184, 236};
constexpr std::array<uint32_t, 6> kGlobals{
    0x820013B8u, 0x820013C0u, 0x829A93F0u, 0x829A9424u,
    0x829A268Cu, 0x8295C608u};
int g_pass = 0, g_fail = 0;

void check(bool ok, const char* name) {
  std::printf("%s %s\n", ok ? "  ok " : "FAIL:", name);
  (ok ? g_pass : g_fail)++;
  std::fflush(stdout);
}
uint32_t read32(uint32_t va) {
  uint8_t* base = K::getGuestBase();
  return PPC_LOAD_U32(va);
}
uint16_t read16(uint32_t va) {
  uint8_t* base = K::getGuestBase();
  return PPC_LOAD_U16(va);
}
void write32(uint32_t va, uint32_t value) { K::writeGuestBE32(va, value); }
void write16(uint32_t va, uint16_t value) {
  uint8_t* base = K::getGuestBase();
  PPC_STORE_U16(va, value);
}

// Refuse a runner's small fallback allocation rather than faulting at our VAs.
bool writable(uint32_t va, size_t bytes) {
  const void* pointer = K::guestVAToHost(va);
  if (!pointer || !K::hostRangeReadable(pointer, bytes)) return false;
#ifdef _WIN32
  uintptr_t cursor = reinterpret_cast<uintptr_t>(pointer);
  const uintptr_t end = cursor + bytes;
  if (end < cursor) return false;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof info) ||
        info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD)) return false;
    const DWORD protection = info.Protect & 0xFF;
    if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
        protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
    const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    if (next <= cursor) return false;
    cursor = next;
  }
  return true;
#else
  return false;  // This project's supported difftest platform is Windows.
#endif
}

struct Ledger {
  std::array<unsigned, 32> frees{};
  unsigned grants = 0, calls = 0, duplicates = 0, invalid = 0;
  unsigned registrations = 0, unregistrations = 0;
  bool registered = true, arguments = true;

  uint32_t grant() {
    if (grants == frees.size()) { ++invalid; return 0; }
    // Fixture-owned storage; NOT evidence of the game's allocation routine.
    return kBuffers + 0x40 * grants++;
  }
  void release(uint32_t pointer) {
    ++calls;
    if (pointer < kBuffers || (pointer - kBuffers) % 0x40 != 0 ||
        (pointer - kBuffers) / 0x40 >= grants) { ++invalid; return; }
    unsigned& count = frees[(pointer - kBuffers) / 0x40];
    if (count++) ++duplicates;
  }
  bool balanced() const {
    if (invalid || duplicates) return false;
    for (unsigned i = 0; i < grants; ++i) if (frees[i] != 1) return false;
    return true;
  }
};
Ledger g_ledger;

void recordFree(PPCContext& __restrict ctx, uint8_t*) {
  g_ledger.arguments &= ctx.r3.u32 == kHeap;
  g_ledger.release(ctx.r4.u32);
  ctx.r3.u32 = 0;
}
void recordRegister(PPCContext& __restrict ctx, uint8_t*) {
  g_ledger.arguments &= ctx.r3.u32 == kRuntime && ctx.r4.u32 == kPlayer;
  if (g_ledger.registered) ++g_ledger.invalid;
  g_ledger.registered = true;
  ++g_ledger.registrations;
  ctx.r3.u32 = 1;
}
void recordUnregister(PPCContext& __restrict ctx, uint8_t*) {
  g_ledger.arguments &= ctx.r3.u32 == kRuntime && ctx.r4.u32 == kPlayer;
  if (!g_ledger.registered) ++g_ledger.invalid;
  g_ledger.registered = false;
  ++g_ledger.unregistrations;
  ctx.r3.u32 = 0;
}

struct Project {
  uint32_t hash, descriptor, root, skeleton, bones;
  // Stand-ins for a FUTURE preload validator's result, not bank verification.
  std::array<uint32_t, kBones> boneKeys{};
  bool ownOffsets = true;
};
enum class Refusal { None, Missing, Incompatible, Busy, Ownership, SharedHead };

struct Fixture {
  std::vector<PPCFunc*> table;
  std::vector<uint8_t> savedArena;
  std::array<uint32_t, kGlobals.size()> savedGlobals{};
  PPCFunc** savedTable = nullptr;
  std::array<Project, 3> projects{};  // retail, independent source rig A, rig B
  bool stackIntact = true;

  Fixture() : table(K::kDispatchTableEntryCount, nullptr), savedArena(kArenaSize) {
    std::memcpy(savedArena.data(), K::guestVAToHost(kArena), kArenaSize);
    for (size_t i = 0; i < kGlobals.size(); ++i) savedGlobals[i] = read32(kGlobals[i]);
    savedTable = K::g_dispatchTable;
    table[(kFreeLeaf - PPC_CODE_BASE) >> 2] = recordFree;
    table[(kRegisterLeaf - PPC_CODE_BASE) >> 2] = recordRegister;
    table[(kUnregisterLeaf - PPC_CODE_BASE) >> 2] = recordUnregister;
    K::g_dispatchTable = table.data();
    write32(0x820013B8u, 0);           // reset's 0.0f constant
    write32(0x820013C0u, 0x3F800000u); // reset/bind's 1.0f constant
    write32(0x829A93F0u, 1);
    write32(0x829A9424u, kHeapRanges);
    write32(0x829A268Cu, kRuntime);
    write32(0x8295C608u, kRegistry);
    seed();
  }
  ~Fixture() {
    K::g_dispatchTable = savedTable;
    for (size_t i = 0; i < kGlobals.size(); ++i) write32(kGlobals[i], savedGlobals[i]);
    std::memcpy(K::guestVAToHost(kArena), savedArena.data(), kArenaSize);
  }
  uint32_t call(PPCFunc* body, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0) {
    PPCContext ctx{};
    ctx.r1.u32 = kStack;
    ctx.r14.u32 = 0x829C0000u;
    ctx.r3.u32 = r3; ctx.r4.u32 = r4; ctx.r5.u32 = r5;
    ctx.lr = 0x82110058u;
    body(ctx, K::getGuestBase());
    stackIntact &= ctx.r1.u32 == kStack && ctx.r14.u32 == 0x829C0000u && ctx.lr == 0x82110058u;
    return ctx.r3.u32;
  }
  void seed() {
    std::memset(K::guestVAToHost(kArena), 0, kArenaSize);
    g_ledger = Ledger{};
    stackIntact = true;
    write32(kHeap, kHeapVtable);
    write32(kHeapVtable + 44, kFreeLeaf);
    write32(kHeapRanges, kHeap);
    write32(kHeapRanges + 4, kBuffers);
    write32(kHeapRanges + 8, kBuffers + 0x1000);
    write32(kRuntime, kRuntimeVtable);
    write32(kRuntimeVtable + 24, kRegisterLeaf);
    write32(kRuntimeVtable + 28, kUnregisterLeaf);
    write32(kRegistry, kRegistryEntries);
    write32(kRegistry + 4, kRegistryEntries + 24);
    for (size_t i = 0; i < projects.size(); ++i) {
      Project& p = projects[i];
      p.hash = 0x10001000u + static_cast<uint32_t>(i);
      p.descriptor = kArena + 0x2000 + 0x100 * static_cast<uint32_t>(i);
      const uint32_t resource = kArena + 0x3000 + 0x100 * static_cast<uint32_t>(i);
      p.root = kArena + 0x4000 + 0x100 * static_cast<uint32_t>(i);
      const uint32_t iterator = kArena + 0x5000 + 0x40 * static_cast<uint32_t>(i);
      p.skeleton = kArena + 0x6000 + 0x100 * static_cast<uint32_t>(i);
      p.bones = kArena + 0x8000 + 0x1000 * static_cast<uint32_t>(i);
      p.ownOffsets = true;
      write32(kRegistryEntries + 8 * static_cast<uint32_t>(i), p.hash);
      write32(kRegistryEntries + 8 * static_cast<uint32_t>(i) + 4, p.descriptor);
      write32(p.descriptor + 8, resource);
      write32(resource + 12, p.root);
      write32(p.root + 24, 1);
      write32(p.root + 28, p.skeleton);
      write32(p.root + 44, iterator); // non-null iterator, empty first link
      write32(p.root + 56, 0x41F00000u); // 30.0f, not zero
      write32(p.skeleton + 4, kBones);
      write32(p.skeleton + 8, p.bones);
      for (uint32_t bone = 0; bone < kBones; ++bone) {
        p.boneKeys[bone] = 0xB000u + bone;
        write32(p.bones + 16 * bone, bone ? bone - 1 : 0);
      }
    }
    write32(projects[0].descriptor + 28, 1);
    write32(projects[0].descriptor + 32, kPlayer);
    write32(kActor + 0x64, kController);
    write32(kActor + 0x84, kFsm);
    write32(kController, 0x82001698u);
    write32(kController + 20, kPlayer);
    write32(kController + 24, kActor);
    write32(kController + 36, projects[0].hash);
    write32(kController + 40, kBones);
    // Empty ability vector with retained capacity; reset must not reconstruct it.
    write32(kController + 112, kArena + 0x1300);
    write32(kController + 116, kArena + 0x1300);
    write32(kController + 120, kArena + 0x1310);
    write32(kPlayer + 12, kContext);
    write32(kPlayer + 20, kController);
    write32(kPlayer + 24, kPose);
    write32(kPlayer + 28, projects[0].root);
    write16(kPlayer + 32, kBones);
    write16(kPlayer + 526, 0);
    seedTrees(projects[0]);
  }
  void seedTrees(const Project& p) {
    // Opaque tree markers deliberately never evaluated as real bank trees.
    write32(kPlayer + 500, p.root + 0x60);
    write32(kPlayer + 516, 0xAABBCCDDu);
    write32(kPlayer + 504, g_ledger.grant());
    write32(kPlayer + 508, 1); write32(kPlayer + 512, 1);
    for (uint32_t offset : kTransitions) {
      write32(kPlayer + offset, p.root + 0x70);
      write32(kPlayer + offset + 4, g_ledger.grant());
      write32(kPlayer + offset + 8, 1); write32(kPlayer + offset + 12, 1);
    }
  }
  const Project* currentProject() const {
    for (const Project& p : projects) if (read32(kController + 36) == p.hash) return &p;
    return nullptr;
  }
  bool listed(const Project& p, uint32_t player) const {
    uint32_t cursor = read32(p.descriptor + 32);
    for (unsigned i = 0; cursor && i < 4; ++i) {
      if (cursor != kPlayer && cursor != kOtherPlayer) return false;
      if (cursor == player) return true;
      cursor = read32(cursor + 16);
    }
    return false;
  }
  // TEST-LOCAL candidate: no game calls this yet. Preconditions intentionally
  // exclude cloth/aimer, layers, live cached nodes, powers and unknown overrides.
  // atActionBoundary represents an UNPROVEN future game-owned drain point.
  Refusal migrate(const Project* target, bool atActionBoundary = true) {
    const Project* old = currentProject();
    bool registered = false;
    for (size_t i = 0; i < projects.size(); ++i)
      registered |= target == &projects[i] && read32(kRegistryEntries + 8 * static_cast<uint32_t>(i) + 4) == projects[i].descriptor;
    if (!target || !registered) return Refusal::Missing;
    if (!old || read32(kActor + 0x64) != kController || read32(kActor + 0x84) != kFsm ||
        read32(kController + 20) != kPlayer || read32(kPlayer + 20) != kController ||
        read32(kPlayer + 24) != kPose || read32(kPlayer + 28) != old->root ||
        !listed(*old, kPlayer)) return Refusal::Ownership;
    if (!target->ownOffsets || target->boneKeys != old->boneKeys ||
        read32(target->skeleton + 4) != kBones || read32(old->skeleton + 4) != kBones ||
        read32(kController + 40) != kBones) return Refusal::Incompatible;
    if (!atActionBoundary || read32(kPlayer + 572) || read16(kPlayer + 496) ||
        read32(kController + 28) || read32(kController + 32) || read32(kController + 100) ||
        read32(kController + 112) != read32(kController + 116)) return Refusal::Busy;
    if (read32(old->descriptor + 32) == kPlayer && read32(kPlayer + 16)) return Refusal::SharedHead;
    if (old == target) return Refusal::None;

    const uint32_t context = read32(kPlayer + 12), pose = read32(kPlayer + 24);
    call(__imp__sub_82140230, old->descriptor, kPlayer);
    call(__imp__sub_82709DE8, kPlayer);
    call(__imp__sub_8270EE50, kPlayer + 504); // NOT 821487B0: it leaves dangling buffers
    call(__imp__sub_82709CB8, kPlayer);      // frees/nulls transitions and clears backlinks
    write32(kController + 36, target->hash);
    call(__imp__sub_82709D88, kPlayer);
    call(__imp__sub_8213D1F0, target->descriptor, kPlayer);
    write32(kPlayer + 12, context);
    write32(kPlayer + 20, kController);
    write32(kPlayer + 24, pose);
    call(__imp__sub_8213D5E8, kController); // SAME-count branch only, no allocations
    call(__imp__sub_8270ABE0, kPlayer, 0);
    return Refusal::None;
  }
  bool stateDrained() const {
    if (read32(kPlayer + 500) || read32(kPlayer + 504) || read32(kPlayer + 508) ||
        read32(kPlayer + 512) || read32(kPlayer + 572) || read32(kPlayer + 516) != 0xFFFFFFFFu) return false;
    for (uint32_t offset : kTransitions)
      for (uint32_t field : {0u, 4u, 8u, 12u}) if (read32(kPlayer + offset + field)) return false;
    return true;
  }
  std::vector<uint8_t> snapshot() const {
    const uint8_t* first = K::guestVAToHost(kArena);
    return std::vector<uint8_t>(first, first + kArenaSize);
  }
};
}  // namespace

int runCostumeAnimationTest() {
  g_pass = g_fail = 0;
  std::puts("[COSTUME_ANIMATION_TEST] real guest lifecycle bodies; controlled heap/runtime vtable leaves");
  std::puts("INFO: synthetic empty-bank projects; no clip/pose/proportion/runtime acceptance claim");
  bool ready = writable(kArena, kArenaSize);
  for (uint32_t global : kGlobals) ready &= writable(global, 4);
  check(ready, "A0 complete writable fixture/global spans exist (no fallback-allocation vacuous pass)");
  if (!ready) return g_fail;
  Fixture f;

  // Actual-function negative controls assert the defect is OBSERVED, not that
  // an unsafe operation has become acceptable.
  const uint32_t oldTree = read32(kPlayer + 500), oldState = read32(kPlayer + 504);
  f.call(__imp__sub_8270D170, kPlayer, 0, f.projects[1].root);
  check(read32(kPlayer + 28) == f.projects[1].root && read16(kPlayer + 32) == kBones,
        "N1 real bare bind installs the independent target root/skeleton");
  check(read32(kPlayer + 500) == oldTree && read32(kPlayer + 504) == oldState &&
            read32(kPlayer + 132) == f.projects[0].root + 0x70 && g_ledger.calls == 0,
        "N2 witness: bare bind retains old tree/current/transition state without freeing it");
  check(f.call(__imp__sub_8270A6D8, kPlayer) == f.projects[1].skeleton,
        "N3 real skeleton getter changes while the old tree remains (no evaluator invoked)");

  f.seed();
  write32(f.projects[0].descriptor + 28, 2);
  write32(kPlayer + 16, kOtherPlayer);
  f.call(__imp__sub_82140230, f.projects[0].descriptor, kPlayer);
  check(read32(f.projects[0].descriptor + 28) == 1 && read32(f.projects[0].descriptor + 32) == 0 &&
            read32(kPlayer + 16) == kOtherPlayer,
        "N4 witness: real head detach loses the remaining project-list head");
  f.seed();
  write32(f.projects[0].descriptor + 28, 2);
  write32(f.projects[0].descriptor + 32, kOtherPlayer);
  write32(kOtherPlayer + 16, kPlayer);
  f.call(__imp__sub_82140230, f.projects[0].descriptor, kPlayer);
  check(read32(f.projects[0].descriptor + 28) == 1 && read32(f.projects[0].descriptor + 32) == kOtherPlayer &&
            read32(kOtherPlayer + 16) == 0,
        "N5 control: real non-head detach preserves the other player");

  f.seed();
  f.call(__imp__sub_821487B0, kPlayer);
  check(g_ledger.calls == 4 && g_ledger.duplicates == 0 && read32(kPlayer + 504) == kBuffers &&
            read32(kPlayer + 136) == kBuffers + 0x40,
        "N6 real player-buffer destructor requests four frees but leaves pointer fields non-null");
  f.call(__imp__sub_82709CB8, kPlayer);
  check(g_ledger.calls == 7 && g_ledger.duplicates == 3 && g_ledger.invalid == 0 && g_ledger.arguments,
        "N7 witness: real destructor+reset requests three duplicate frees (recording leaf never frees)");
  check(read32(kPlayer + 12) == 0 && read32(kPlayer + 20) == 0 && read32(kPlayer + 24) == 0,
        "N8 real reset also erases context/controller/pose ownership");

  f.seed();
  auto refusedWithoutMutation = [&](const Project* target, Refusal expected, const char* label,
                                   bool boundary = true) {
    const auto before = f.snapshot();
    const unsigned calls = g_ledger.calls, registrations = g_ledger.registrations;
    const unsigned unregistrations = g_ledger.unregistrations;
    check(f.migrate(target, boundary) == expected && f.snapshot() == before &&
              g_ledger.calls == calls && g_ledger.registrations == registrations &&
              g_ledger.unregistrations == unregistrations, label);
  };
  refusedWithoutMutation(nullptr, Refusal::Missing, "R1 missing project refuses before any guest mutation/callback");
  const Project missing = f.projects[1];
  refusedWithoutMutation(&missing, Refusal::Missing, "R2 unregistered descriptor identity refuses before mutation");
  write32(f.projects[1].skeleton + 4, kBones - 1);
  refusedWithoutMutation(&f.projects[1], Refusal::Incompatible, "R3 incompatible bone count refuses; no retail-offset fallback");
  write32(f.projects[1].skeleton + 4, kBones);
  f.projects[1].boneKeys[5] ^= 1;
  refusedWithoutMutation(&f.projects[1], Refusal::Incompatible, "R4 incompatible bone ordering refuses before mutation");
  f.projects[1].boneKeys[5] ^= 1;
  f.projects[1].ownOffsets = false;
  refusedWithoutMutation(&f.projects[1], Refusal::Incompatible, "R5 absent own-offset preload result refuses, never borrows retail offsets");
  f.projects[1].ownOffsets = true;
  refusedWithoutMutation(&f.projects[1], Refusal::Busy, "R6 unproven/not-ready action boundary refuses", false);
  for (uint32_t field : {kPlayer + 572, kController + 28, kController + 32, kController + 100}) {
    write32(field, kOtherPlayer);
    refusedWithoutMutation(&f.projects[1], Refusal::Busy, "R7 layer/cloth/aimer/cached-node busy case refuses unchanged");
    write32(field, 0);
  }
  write16(kPlayer + 496, 1);
  refusedWithoutMutation(&f.projects[1], Refusal::Busy, "R8 live unknown bone override refuses unchanged");
  write16(kPlayer + 496, 0);
  write32(kController + 116, kArena + 0x1304);
  refusedWithoutMutation(&f.projects[1], Refusal::Busy, "R9 active power vector refuses unchanged");
  write32(kController + 116, kArena + 0x1300);
  write32(f.projects[0].descriptor + 28, 2);
  write32(kPlayer + 16, kOtherPlayer);
  refusedWithoutMutation(&f.projects[1], Refusal::SharedHead, "R10 shared-head hazard is refused before real detach");

  f.seed();
  std::array<uint8_t, 128> originalController{};
  std::memcpy(originalController.data(), K::guestVAToHost(kController), originalController.size());
  for (size_t target : {size_t{1}, size_t{2}, size_t{0}}) {
    const Project* old = f.currentProject();
    const bool migrated = f.migrate(&f.projects[target]) == Refusal::None;
    check(migrated && read32(old->descriptor + 28) == 0 && read32(old->descriptor + 32) == 0 &&
              read32(f.projects[target].descriptor + 28) == 1 && read32(f.projects[target].descriptor + 32) == kPlayer,
          "T1 real detach/reset/attach moves exactly one player: retail -> A -> B -> retail");
    check(f.stateDrained() && g_ledger.balanced(),
          "T2 each current/transition state buffer released exactly once, all old trees drained");
    check(read32(kActor + 0x64) == kController && read32(kActor + 0x84) == kFsm &&
              read32(kController + 20) == kPlayer && read32(kController + 24) == kActor &&
              read32(kPlayer + 12) == kContext && read32(kPlayer + 20) == kController && read32(kPlayer + 24) == kPose,
          "T3 controller/player/actor/FSM identities and ownership remain intact (structural only)");
    check(f.call(__imp__sub_8270A6D8, kPlayer) == f.projects[target].skeleton &&
              read32(kPlayer + 28) == f.projects[target].root && !(read32(kPlayer) & 0x200),
          "T4 real skeleton getter and constructor-equivalent flag now use the selected project");
    std::array<uint8_t, 128> currentController{};
    std::memcpy(currentController.data(), K::guestVAToHost(kController), currentController.size());
    std::memcpy(currentController.data() + 36, originalController.data() + 36, 4);
    check(currentController == originalController,
          "T5 controller storage is unchanged except project hash; no constructor/destructor used");
    if (target != 0) f.seedTrees(f.projects[target]);
  }
  check(g_ledger.grants == 12 && g_ledger.calls == 12 && g_ledger.balanced() &&
            g_ledger.registrations == 3 && g_ledger.unregistrations == 3 &&
            g_ledger.registered && g_ledger.arguments && f.stackIntact,
        "T6 three transitions balance all fixture buffers/runtime leaves and preserve guest stack/LR/r14");
  const auto unchanged = f.snapshot();
  check(f.migrate(&f.projects[0]) == Refusal::None && f.snapshot() == unchanged && g_ledger.calls == 12,
        "T7 selecting the already-installed project is an allocation/callback-free no-op");
  std::printf("[COSTUME_ANIMATION_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
