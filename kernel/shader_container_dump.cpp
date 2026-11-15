// iter-370: capture the REAL D3D9 shader CONTAINER at creation time.
//
// WHY THIS EXISTS. The offline translation pipeline (tools/xenosrecomp/) needs
// containers, not bare ucode: the container carries the constant table, the
// vertex-element declarations and the interpolator table that map each export
// register to a semantic. None of that is derivable from the ucode alone —
// guessing it would silently misroute attributes between VS and PS.
//
// Until now containers could only come from the golden Xenia dump, which is a
// snapshot of ONE scene. Live boot loads shaders that are not in it: iter-370
// measured 5 vertex shaders missing from the cache, and cross-matching them
// against all 402 carved containers found 0 exact matches. The
// same-size-template synthesis in extract_shader_containers.py then produced
// garbage for them — it is meant to absorb the 1-3 dword bind-time vfetch
// patch, but these needed 18-30 dwords of a 27-33 dword shader changed, i.e.
// they are simply different shaders. All four "synthesized" containers failed
// DXIL validation ("Not all elements of output SV_Position were written").
//
// So capture the container from the only authority: the game handing it to D3D.
// ENGINE_MAP 6d: every shader funnels through sub_820F5148
// (D3DDevice_CreateVertexShader) / sub_820F4D90 (D3DDevice_CreatePixelShader)
// with r3 = the container.
//
// This is pure observation — it reads guest memory and writes host files. It
// never writes guest state and never calls a guest function, so it is outside
// the forcing rules (NATURAL_PATH_CHARTER). It is still gated and default-OFF
// because it writes files and hashes every created shader.
//
// NOTE ON PAIRING. The container holds the shader as COMPILED; the ucode the
// GPU executes is the bind-time vfetch-PATCHED variant delivered by IM_LOAD
// (ENGINE_MAP 6d: 0/54 VS match their container byte-exact, 56/56 PS do). So
// the cache key (XXH3 of the patched IM_LOAD bytes) is NOT the hash of what is
// dumped here. tools/match_shader_containers.py pairs the two by size + minimal
// dword diff and splices the patched ucode into the real container.
//
// SR_DUMP_SHADER_CONTAINERS=1 to enable; writes shader_containers/.
//
// iter-463: the same validated capture also feeds runtime shader translation
// (gfx/shader_runtime_xlat.cpp, SR_SHADER_RUNTIME_XLAT, default ON): the container bytes are
// COPIED into host memory (ShaderCache::noteContainer) independently of the file dump. Still
// observe-only - a read of guest memory the game just passed to CreateShader, at entry, before
// CreateShader fills +0x1C/+0x20.

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "phys_window.hpp"
#include "../app/log.hpp"
#include "../gfx/shader_cache.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include "native_brackets.hpp"

extern "C" void __imp__sub_820F5148(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_820F4D90(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82108300(PPCContext& __restrict ctx, uint8_t* base);

namespace {

bool dumpEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_DUMP_SHADER_CONTAINERS");
    return e && e[0] == '1';
  }();
  return v;
}

uint32_t beAt(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint64_t fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}

// Read the container header, validate it, and write the whole blob once per
// unique ucode. Every field is bounds-checked before use: the harness VEH makes
// any host access violation fatal before an SEH filter could run, so this must
// be fault-free rather than fault-and-catch (reference_thread_registry).
void captureContainer(uint32_t containerVA, bool expectVertex) {
  const bool dump = dumpEnabled();
  const bool xlat = sr::gfx::ShaderCache::wantsContainers();  // iter-463
  if ((!dump && !xlat) || !containerVA) return;

  const uint8_t* hdr = sr::kernel::guestVAToHost(containerVA);
  if (!hdr) return;

  const uint32_t flags = beAt(hdr + 0x00);
  if ((flags & 0xFFFFFF00u) != 0x102A1100u) return;      // not a container
  const bool isVertex = (flags & 1u) != 0;
  if (isVertex != expectVertex) return;                   // wrong create path

  const uint32_t virtualSize = beAt(hdr + 0x04);
  const uint32_t physicalSize = beAt(hdr + 0x08);
  const uint32_t shaderOffset = beAt(hdr + 0x18);
  // Same sanity envelope extract_shader_containers.py applies when carving.
  if (virtualSize < 0x40 || virtualSize > 0x10000 || (virtualSize & 3u)) return;
  if (physicalSize == 0 || physicalSize > 0x100000) return;
  if (shaderOffset == 0 || shaderOffset >= virtualSize) return;

  const uint32_t total = virtualSize + physicalSize;
  // Confirm the tail is mapped before reading any of it.
  if (!sr::kernel::guestVAToHost(containerVA + total - 1)) return;

  const uint32_t physOffset = beAt(hdr + shaderOffset + 0);
  const uint32_t ucodeSize = beAt(hdr + shaderOffset + 4);
  if (ucodeSize == 0 || ucodeSize > 0x40000) return;
  if (uint64_t(physOffset) + ucodeSize > physicalSize) return;

  // iter-463: runtime translation copies the validated container (it dedupes by content).
  if (xlat) sr::gfx::ShaderCache::noteContainer(hdr, total, isVertex);
  if (!dump) return;

  const uint8_t* ucode = hdr + virtualSize + physOffset;
  const uint64_t uhash = fnv1a(ucode, ucodeSize);

  static std::mutex mu;
  static std::unordered_set<uint64_t> seen;
  {
    std::lock_guard<std::mutex> lk(mu);
    if (seen.size() >= 4096 || !seen.insert(uhash).second) return;
  }

  char path[192];
  std::snprintf(path, sizeof(path), "shader_containers/%s_%016llx_%uB.xpu",
                isVertex ? "vs" : "ps", (unsigned long long)uhash, ucodeSize);
  FILE* f = std::fopen(path, "wb");
  if (!f) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[SHADER_CONTAINER] cannot write %s - create the "
                  "shader_containers/ directory first", path);
    return;
  }
  std::fwrite(hdr, 1, total, f);
  std::fclose(f);

  static std::atomic<uint32_t> nLog{0};
  const uint32_t n = nLog.fetch_add(1, std::memory_order_relaxed);
  if (n < 16 || (n % 64) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[SHADER_CONTAINER] %s VA=0x%08X virt=%u phys=%u ucode=%uB "
                "ctab=0x%X -> %s (total %u)",
                isVertex ? "VS" : "PS", containerVA, virtualSize, physicalSize,
                ucodeSize, beAt(hdr + 0x10), path, n + 1);
}

}  // namespace

// D3DDevice_CreateVertexShader — r3 = container VA (ENGINE_MAP 6d).
PPC_FUNC(sub_820F5148) {
  SR_NATIVE_SCOPE(0x820F5148);  // NATIVE N0 (scripts/gen_native_brackets.py)
  captureContainer(ctx.r3.u32, /*expectVertex=*/true);
  // iter-397q [SHCREATE] census (always-on, capped): every shader creation
  // with caller lr — zero non-NULL SetShader binds all run means creations
  // themselves are the scarce event worth naming.
  static std::atomic<int> s_n{0};
  const int n = s_n.fetch_add(1, std::memory_order_relaxed);
  const uint32_t blob = ctx.r3.u32;
  const uint32_t callerLr = (uint32_t)ctx.lr;
  __imp__sub_820F5148(ctx, base);
  if (n < 60)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[SHCREATE] VS #%d blob=%08X -> obj=%08X lr=%08X",
                n + 1, blob, ctx.r3.u32, callerLr);
  // iter-460: a VS object whose ucode pointer (+0x20) is null draws with an
  // IM_LOAD of address 0 (GpuEmitDrawState_sub_82108668) - seen 130K times
  // under SR_PHYS_LIFO_FREE. Name the creations that produce one.
  if (const uint32_t obj = ctx.r3.u32) {
    const uint32_t ucode = sr::kernel::readGuestU32BE(obj + 0x20u);
    // iter-460: does the object's ucode copy hold the container's bytes?
    {
      const uint32_t virt = sr::kernel::readGuestU32BE(blob + 4u);
      const uint32_t shOff = sr::kernel::readGuestU32BE(blob + 0x18u);
      if (virt >= 0x40u && virt <= 0x10000u && shOff && shOff < virt && ucode) {
        const uint32_t physOff = sr::kernel::readGuestU32BE(blob + shOff);
        const uint32_t src = blob + virt + physOff, dst = ucode + physOff;
        uint32_t a4[4], b4[4];
        for (int i = 0; i < 4; i++) {
          a4[i] = sr::kernel::readGuestU32BE(src + 4u * i);
          b4[i] = sr::kernel::readGuestU32BE(dst + 4u * i);
        }
        const bool same = std::memcmp(a4, b4, sizeof(a4)) == 0;
        const bool zero = !(b4[0] | b4[1] | b4[2] | b4[3]);
        static std::atomic<int> s_bad{0};
        if ((!same || zero) && s_bad.fetch_add(1, std::memory_order_relaxed) < 40)
          SR_LOG_BOOT(sr::LogSeverity::Warn,
                      "[SHCREATE_UCODE] VS #%d obj=%08X container ucode@%08X=%08X %08X | "
                      "object ucode@%08X=%08X %08X%s",
                      n + 1, obj, src, a4[0], a4[1], dst, b4[0], b4[1],
                      zero ? " ZERO" : " DIFFERS");
      }
    }
    static std::atomic<int> s_null{0};
    if (!ucode && s_null.fetch_add(1, std::memory_order_relaxed) < 40)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[SHCREATE_NULLUCODE] VS #%d blob=%08X obj=%08X lr=%08X blob+0=%08X +4=%08X +8=%08X",
                  n + 1, blob, obj, callerLr, sr::kernel::readGuestU32BE(blob),
                  sr::kernel::readGuestU32BE(blob + 4u), sr::kernel::readGuestU32BE(blob + 8u));
  }
}

// D3DDevice_CreatePixelShader — r3 = container VA.
PPC_FUNC(sub_820F4D90) {
  SR_NATIVE_SCOPE(0x820F4D90);  // NATIVE N0 (scripts/gen_native_brackets.py)
  captureContainer(ctx.r3.u32, /*expectVertex=*/false);
  static std::atomic<int> s_n{0};
  const int n = s_n.fetch_add(1, std::memory_order_relaxed);
  const uint32_t blob = ctx.r3.u32;
  const uint32_t callerLr = (uint32_t)ctx.lr;
  __imp__sub_820F4D90(ctx, base);
  if (n < 60)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[SHCREATE] PS #%d blob=%08X -> obj=%08X lr=%08X",
                n + 1, blob, ctx.r3.u32, callerLr);
}

// GpuLoadShaderImmediate_sub_82108300 (observe only): the ring copy of a busy
// VS's ucode reads (phys - 0x40000000), the 0xC alias (ENGINE_MAP). Name the
// source's owners and which window actually holds non-zero bytes.
PPC_FUNC(sub_82108300) {
  const uint32_t vs = ctx.r5.u32, variant = ctx.r10.u32;
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (vs && (n <= 24 || (n & (n - 1)) == 0)) {
    const uint32_t x = sr::kernel::readGuestU32BE(vs + 896u + 8u * variant);
    const uint32_t physOfs = sr::kernel::readGuestU32BE(vs + x + 872u);
    const uint32_t size = sr::kernel::readGuestU32BE(vs + x + 876u);
    const uint32_t ucode = sr::kernel::readGuestU32BE(vs + 32u);
    const uint32_t va = ucode + physOfs;
    const uint32_t p = (((va >> 20) + 0x200u) & 0x1000u) + (va & 0x1FFFFFFFu);
    const uint32_t a0 = sr::kernel::readGuestU32BE(0xA0000000u + p);
    const uint32_t e0 = sr::kernel::readGuestU32BE(0x80000000u + p);
    const uint32_t v0 = sr::kernel::readGuestU32BE(va);
    const int gf = sr::kernel::physwin::gpuGrantFlags(p, 4u);
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[IMMLOAD_SRC] #%u vs=%08X ucodeVA=%08X+%X size=%u phys=%08X pageOwner=%d "
                "grants(heap8=%d phys8=%d physA=%d) | @va=%08X @0xA=%08X @0x8=%08X",
                n, vs, ucode, physOfs, size, p, sr::kernel::physwin::physPageOwner(p), gf & 1,
                (gf >> 1) & 1, (gf >> 2) & 1, v0, a0, e0);
  }
  __imp__sub_82108300(ctx, base);
}
