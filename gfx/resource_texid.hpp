#pragma once
// mem.resources residual unknown `textures_from_resolves` (lane D resproof, 2026-10-17): THE TEXTURE-IDENTITY CENSUS.
//
// SR_MEM_RES_TEXID=1 (dev / Oracle build only; meaningful with SR_MEM_RESOURCES=1, the native route): every texture
// bind (2D: getOrCreateTexture2D, its clean-mark fast hits included; stacked / cube: pm4ShmLoadArray) is classified by
// whether its source pages hold resolve output NOW - a page the GPU wrote and the CPU has not rewritten since
// (the registry's gpuGen != 0 && gpuGen >= cpuGen; the page model's m_shmGpuWritten when the model decides) - and, for
// those RESOLVE BINDS, by where the bound texture's content came from:
//   identity     the resolve kernel wrote the bound texture itself (a native target, gfx/native_targets.inc nbTgtStamp)
//                and the bind uses it as it stands - the host target the resolve wrote, by identity;
//   arena        the GPU texture loader decoded it from the resource arena's GPU-written pages (the resolve's output
//                reaches the texture GPU-side; at 1x without SR_NB_TARGETS_1X this is the design: Xenia's texture cache
//                loads render-to-texture from its shared memory the same way);
//   cpu_snapshot the CPU decoder decoded a private snapshot of the arena (resourceCpuSnapshot: the GPU's bytes, read back);
//   guest_bytes  the CPU decoder decoded GUEST RAM over resolve output (the page model's path: pm4ShmSyncToGuest wrote the
//                GPU's bytes back first) - "a mirror decoded from guest memory", the class the ledger forbids;
//   stale        the bind kept an older texture although its pages hold newer resolve output (no refresh possible, a
//                transient upload failure, a pre-iter-408 cache entry);
//   unknown      a cached texture whose content origin was never recorded (loaded before the census could see it);
//   cpu_after_gpu a page of the bind was GPU-written (a resolve destination) and the CPU wrote it since (a later registry
//                note: cpuGen > gpuGen != 0) - the bind reads CPU bytes over a resolve destination. Counted with the
//                by-bytes classes: the orchestrator's criterion (2026-10-17) is "every resolve-destination binding comes
//                from the GPU-produced resolve output (the host target by identity, or the arena range the native
//                resolve wrote, with no CPU write between) - 0 decoded from guest bytes", and this is the "no CPU write
//                between" half. Its samples name the first such page's last note source (gfx/resource_registry.hpp Src).
// A bind whose pages hold no resolve output is counted as `plain` only. The 2D clean-mark fast path (nothing it reads
// changed since its last full check) repeats the class that check recorded.
// Lines ([MEM_RES_TEXID], app/log.cpp kMilestoneTags): totals every 5 s of wall clock (at a bind) and the graceful quit's
// terminal family `resource_texid`; the first 16 resolve binds of each class and every guest_bytes / stale / unknown
// bind up to 64 as samples. tools/perf/mem_res_texid.py grades per era: resolve binds > 0, guest_bytes = stale =
// unknown = 0 (PASS), with the identity / arena / cpu_snapshot split reported; a strict reading (identity only) is
// printed beside it.
#include <cstdint>

#include "../kernel/sr_oracle.hpp"

namespace sr::gfx::memres {

enum class TexOrigin : uint8_t { kNone = 0, kArena, kCpuSnapshot, kGuestBytes, kTarget };
enum class TexIdClass : uint8_t {
  kPlain = 0, kIdentity, kArena, kCpuSnapshot, kGuestBytes, kStale, kUnknown, kCpuAfterGpu, kCount
};
enum class TexIdKind : uint8_t { k2D = 0, k2DClean, kArray, kCount };

// cpuAfterPages: pages of the bind a CPU note reached after their GPU write (the class wins over any origin).
inline TexIdClass texIdClassOf(TexOrigin origin, uint32_t gpuPages, uint32_t cpuAfterPages = 0) noexcept {
  if (cpuAfterPages) return TexIdClass::kCpuAfterGpu;
  if (!gpuPages) return TexIdClass::kPlain;
  switch (origin) {
    case TexOrigin::kArena: return TexIdClass::kArena;
    case TexOrigin::kCpuSnapshot: return TexIdClass::kCpuSnapshot;
    case TexOrigin::kGuestBytes: return TexIdClass::kGuestBytes;
    case TexOrigin::kTarget: return TexIdClass::kIdentity;
    default: return TexIdClass::kUnknown;
  }
}

#if SR_ORACLE
bool texIdOn() noexcept;
// One bind: its class, the pages it reads and how many of them hold resolve output; base = its guest base (samples);
// cpuAfterPages / src: its pages a CPU note reached after their GPU write, the first one's last note source (samples).
void texIdBind(TexIdKind kind, TexIdClass cls, uint32_t pages, uint32_t gpuPages, uint32_t base, uint32_t fmt,
               uint32_t cpuAfterPages = 0, uint32_t src = 0) noexcept;
void texIdReport(const char* why) noexcept;
#else
inline bool texIdOn() noexcept { return false; }
inline void texIdBind(TexIdKind, TexIdClass, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t = 0,
                      uint32_t = 0) noexcept {}
inline void texIdReport(const char*) noexcept {}
#endif

}  // namespace sr::gfx::memres
