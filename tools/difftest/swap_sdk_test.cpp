// Normal SDK swap contract against the real recompiled body. No game/CP worker.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "ppc/ppc_recomp_shared.h"
#include "guest/guest_harness.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/gpu_write_watch.hpp"
#include "gfx/resource_memory.hpp"  // memres::resourceMode (the native resource route, default ON since the bake)
#include "gfx/resource_ab.hpp"      // memres::resourceAbDual (the frozen-pass A/B keeps the tracker)
#include "kernel/native_flush.hpp"
#include "kernel/stubs.hpp"
#include "gfx/pm4_engine.hpp"
#include "gfx/gamma_ramp.hpp"
#include "gfx/xenos_state.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <tuple>
#include <vector>
#include <windows.h>

extern "C" void __imp__sub_82112050(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8210BBE8(PPCContext& __restrict, uint8_t*);
// boot gaps (native/boot-gaps): the SDK bodies of the display emitters and the mode state, for the direct sections.
extern "C" void __imp__sub_821056E0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_821057C0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82111CB8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8210A788(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82103238(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_820FD8C0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82105458(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82105278(PPCContext& __restrict, uint8_t*);
namespace K = sr::kernel;
namespace N = sr::native::flush;
namespace W = sr::kernel::gww;
namespace {
constexpr uint32_t dev = 0x70600000u, buf = 0x90640000u, list = 0x70650000u;
constexpr uint32_t front = 0x70655000u, rect = 0x70655100u, fb = 0x70655200u;
constexpr uint32_t recorder = 0x70655300u, cell = 0x70655400u, stack = 0x70580000u;
constexpr uint32_t reentryDev = 0x70610000u, reentryBuf = 0x90660000u;
// boot gaps: the data-import cell VdGpuClockInMHz (0x82000844) and the variable it points at, 500 as the kernel resolves
// it (kernel/guest_context.cpp's data imports) - GpuScalerConfigureState 8210A0F8 reads it; unresolved (the raw import
// token) it returns before building its filter table
constexpr uint32_t gpuClockCell = 0x82000844u, gpuClock = 0x70655500u;
constexpr uint32_t allocator = PPC_CODE_BASE + 4u, bytes = 0x10000u;
int passed = 0, failed = 0;
unsigned allocations = 0, queries = 0, retrains = 0;
unsigned reentries = 0;
unsigned epilogCalls = 0;
uint64_t epilogR4 = 0;
// boot gaps: the planes each display ramp builder call was handed (the observers sub_82105458 / sub_82105278 below),
// recorded while `recordPlanes` is set (one arm of a direct section)
bool recordPlanes = false;
std::vector<std::vector<uint8_t>> builderPlanes;
void notePlanes(uint32_t which,uint32_t planes) {
  if (!recordPlanes) return;
  std::vector<uint8_t> v{uint8_t(which)};
  if (const uint8_t* p = K::guestVAToHost(planes)) v.insert(v.end(),p,p + 0x600u);
  builderPlanes.push_back(std::move(v));
}
bool reenter = false, mutate = false;
bool breakEntry = false;  // VdSwap's export sets dev+0x2A38 bit 0x02: every later Entry declines (the fallbacks)
uint32_t writeback = 0;
K::ExportFn originalQuery = nullptr, originalRetrain = nullptr;
K::ExportFn originalSwap = nullptr;
void check(bool ok, const char* name) { std::printf("%s %s\n", ok ? "  ok " : "FAIL:", name); ok ? ++passed : ++failed; }
uint32_t query(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f) {
  ++queries; return originalQuery(a,b,c,d,e,f);
}
uint32_t retrain(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f) {
  ++retrains; return originalRetrain(a,b,c,d,e,f);
}
uint32_t swap(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f) {
  writeback = c;
  if (breakEntry) K::guestVAToHost(dev)[0x2A38u] |= 0x02u;  // tile auto-predication: native_flush declined()
  return originalSwap(a,b,c,d,e,f);
}
// ---- boot gaps: the display exports the SDK bodies call, captured (arguments, the words their stack arguments point at)
// so the native leaves' ports of the CPU prelude are compared argument for argument, not only by their effects ----
struct DisplayCalls {
  std::vector<std::vector<uint32_t>> scaler;  // VdInitializeScalerCommandBuffer: r3..r10, the 4 stack words, buffers
  std::vector<std::array<uint32_t,2>> persist, frees;  // VdPersistDisplay (r3, *r3 words) / MmFreePhysicalMemory (r3, r4)
  std::vector<uint32_t> modes;                // VdSetDisplayMode r3
  bool operator==(const DisplayCalls& o) const {
    return scaler == o.scaler && persist == o.persist && frees == o.frees && modes == o.modes;
  }
};
DisplayCalls calls;
uint32_t scalerProgram = 0;  // > 0: the scaler export writes a program of that many dwords (the literal fallback)
unsigned scalerTablesBuilt = 0, scalerTablesStale = 0;  // the scaler export's calls with 8210A0F8's table built / not
K::ExportFn originalScaler = nullptr, originalPersist = nullptr, originalFree = nullptr, originalMode = nullptr;
uint32_t scalerInit(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f) {
  // The caller's frame is r9 - 0x9C (8210A788: r9 = sp + 0x9C): its stack arguments at sp + 0x54 .. 0x6C.
  const uint32_t r9 = K::ExportRegistry::getCurrentArgR9(), r10 = K::ExportRegistry::getCurrentArgR10(), sp = r9 - 0x9Cu;
  std::vector<uint32_t> v = {a,b,c,d,e,f,r9,r10};
  for (uint32_t o : {0x54u,0x5Cu,0x64u,0x6Cu}) v.push_back(K::readGuestU32BE(sp + o));
  const uint32_t floats = K::readGuestU32BE(sp + 0x54u), table = K::readGuestU32BE(sp + 0x5Cu);
  const uint32_t out = K::readGuestU32BE(sp + 0x64u);
  for (uint32_t i = 0; i < 4u; ++i) v.push_back(K::readGuestU32BE(r9 + 4u*i));        // v54
  for (uint32_t i = 0; i < 5u; ++i) v.push_back(K::readGuestU32BE(floats + 4u*i));    // v55
  // 8210A0F8's 1408-byte table, when it built one (memset, the two filter tables, words 325..351 - [340] = 1 and
  // [347] = 0x1000000 always); its early returns (a scale over 4, too few taps) leave the frame's stale stack there,
  // which the native path, running no guest frames before it, does not reproduce: not compared (the pointer is). On
  // this platform it never builds one, live or here: it divides VdGpuClockInMHz by VdQueryVideoMode's word +0x1C, which
  // kernel/stubs.cpp leaves 0 (Xenia answers 0x4A), and returns 0 (sw3: built 0 in every row).
  const bool built = K::readGuestU32BE(table + 4u*340u) == 1u && K::readGuestU32BE(table + 4u*347u) == 0x1000000u;
  ++(built ? scalerTablesBuilt : scalerTablesStale);
  if (built)
    for (uint32_t i = 0; i < 352u; ++i) v.push_back(K::readGuestU32BE(table + 4u*i));
  else
    v.push_back(0x5CA1AB1Eu);
  for (uint32_t i = 0; i < 200u; ++i) v.push_back(K::readGuestU32BE(out + 4u*i));     // the 800-byte output (prefilled)
  calls.scaler.push_back(v);
  originalScaler(a,b,c,d,e,f);
  // A well-formed platform program: one-register type-0 writes {0x1960 + k, 0xC0DE0000 + k} (scalerProgram is even).
  for (uint32_t i = 0; i + 1u < scalerProgram; i += 2u) {
    K::writeGuestBE32(out + 4u*i,0x00001960u + i/2u);
    K::writeGuestBE32(out + 4u*i + 4u,0xC0DE0000u + i/2u);
  }
  return scalerProgram;
}
uint32_t persistDisplay(uint32_t a,uint32_t b,uint32_t,uint32_t,uint32_t,uint32_t) {
  calls.persist.push_back({a,K::readGuestU32BE(a) | K::readGuestU32BE(a + 4u) | K::readGuestU32BE(a + 8u)});
  K::writeGuestBE32(b,0x7E5F0000u);  // Xenia's VdPersistDisplay hands back a block for MmFreePhysicalMemory(1, *r4)
  return 1u;
}
uint32_t freePhysical(uint32_t a,uint32_t b,uint32_t,uint32_t,uint32_t,uint32_t) { calls.frees.push_back({a,b}); return 0u; }
uint32_t displayMode(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f) {
  calls.modes.push_back(a); return originalMode(a,b,c,d,e,f);
}
bool envOn(const char* name) {  // a default-ON gate as its reader decides it
  const char* e = std::getenv(name);
  return !e || e[0] != '0';
}
void allocate(PPCContext& ctx,uint8_t*) {
  ++allocations;
  const uint32_t at = K::readGuestU32BE(dev + 0x34C0u);
  // BeginNewSegment passes a minimum byte count by reference, including its
  // reserved tail. Gamma alone needs 2309 dwords: a fixed 8 KiB answer would
  // overlap the next allocation and corrupt the oracle's closed segment.
  const uint32_t requested = K::readGuestU32BE(ctx.r5.u32);
  const uint64_t aligned = (uint64_t(requested) + 31u) & ~uint64_t(31u);
  const uint64_t size = aligned < 8192u ? 8192u : aligned;
  if (ctx.r3.u32 != dev || ctx.r4.u32 != 2u || ctx.r6.u32 != 32u || at < buf ||
      uint64_t(at) + size > uint64_t(buf) + bytes) {
    ++failed; ctx.r3.u64 = 0u; return;
  }
  if (reenter) {
    ++reentries;
    // A second recorder exercises the shared recorder mutex without recursively
    // allocating from the same exhausted segment inside its allocator callback.
    std::memset(K::guestVAToHost(reentryDev),0,0x5000u);
    std::memset(K::guestVAToHost(reentryBuf),0xEE,4096u);
    K::writeGuestBE32(reentryDev + 40u,reentryBuf);
    K::writeGuestBE32(reentryDev + 44u,reentryBuf + 4092u);
    K::writeGuestBE32(reentryDev + 48u,reentryBuf + 4092u);
    K::writeGuestBE32(reentryDev + 0x34C4u,reentryBuf + 4u);
    PPCContext nested = ctx; nested.r3.u64 = reentryDev; nested.r4.u64 = 1u;
    sub_821075E0(nested,K::getGuestBase());
  }
  if (mutate) {
    K::writeGuestBE32(dev + 0x2A10u,fb + 0x300u);
    K::writeGuestBE32(fb + 0x300u,0xFFu);
    K::writeGuestBE32(rect + 8u,640u);
    std::memcpy(K::guestVAToHost(dev + 0x33D0u),K::guestVAToHost(rect),28u);
  }
  K::writeGuestBE32(ctx.r5.u32,uint32_t(size));
  K::writeGuestBE32(dev + 0x34C0u,at + uint32_t(size));
  ctx.r3.u64 = at;
}
// boot gaps: persist = the persisted display (byte 0x2A39 bit 0x10, the first Swap's: 82105DD0's ramp, REG_RMW 0x1E4E,
// VdPersistDisplay); wide = the display format 0x28280136 (low bits 54: 82105DD0's PWL ramp; 82111CB8's wide mode).
struct Case { uint32_t format; bool changedFront, changedRect, scaler, gamma, previous, kick; bool persist = false, wide = false; };
void fixture(const Case& c,uint32_t offset) {
  std::memset(K::guestVAToHost(dev),0,0x5000u);
  std::memset(K::guestVAToHost(buf),0xEE,bytes);
  std::memset(K::guestVAToHost(list),0xEE,0x4000u);
  std::memset(K::guestVAToHost(front),0,0x800u);
  std::memset(K::guestVAToHost(stack - 0x2000u),0xCD,0x2000u);
  K::writeGuestBE32(0x82000948u,cell); K::writeGuestBE32(cell,dev);
  K::writeGuestBE32(gpuClockCell,gpuClock); K::writeGuestBE32(gpuClock,500u);
  K::writeGuestBE32(0x829AC0ECu,0u); K::writeGuestBE32(0x829AC100u,0u);
  K::guestVAToHost(0x829ABD8Cu)[0] = 0u;
  K::writeGuestBE32(dev + 40u,buf + offset);
  K::writeGuestBE32(dev + 44u,buf + 8192u - 4u);
  if (mutate) K::writeGuestBE32(dev + 44u,buf + 128u);
  K::writeGuestBE32(dev + 48u,c.kick ? buf : buf + 4096u);
  K::writeGuestBE32(dev + 0x3290u,buf + bytes - 4u);
  K::writeGuestBE32(dev + 0x3294u,1u);
  K::writeGuestBE32(dev + 0x32D0u,list); K::writeGuestBE32(dev + 0x32D4u,list + 0x4000u);
  K::writeGuestBE32(dev + 0x34ACu,buf); K::writeGuestBE32(dev + 0x34B0u,buf + bytes);
  K::writeGuestBE32(dev + 0x34B4u,8192u); K::writeGuestBE32(dev + 0x34C0u,buf + 8192u);
  K::writeGuestBE32(dev + 0x34C4u,buf + offset + 4u);
  K::writeGuestBE32(dev + 0x34C8u,buf);
  K::writeGuestBE32(dev + 0x4FC0u,1u);
  K::writeGuestBE32(dev + 0x2A10u,fb); K::writeGuestBE32(dev + 0x2A1Cu,0x101u);
  K::writeGuestBE32(fb,0xFFu);
  K::writeGuestBE32(dev + 0x34D0u,c.previous ? 0xFFu : 0u);
  K::writeGuestBE32(dev + 0x3354u,recorder);
  K::writeGuestBE32(recorder + 0xA4u,dev); K::writeGuestBE32(recorder + 0xA8u,8192u);
  K::writeGuestBE32(recorder + 0xACu,allocator);
  K::writeGuestBE32(dev + 0x33B0u,c.wide ? 0x28280136u : 0x00120000u);
  K::writeGuestBE32(dev + 0x33B4u,0x89ABCDEFu);
  K::guestVAToHost(dev)[0x2A39u] = c.persist ? 0x30u : 0x20u;
  // the persisted display: dev+0x3510 (the last ramp 82105DD0 emitted) set apart from any ramp its builders make, so
  // 82105DD0's compare finds a difference and its emitter runs (on the zeroed device it did not: bg1, ramp+0)
  if (c.persist) std::memset(K::guestVAToHost(dev + 0x3510u),0x5A,1536u);
  K::guestVAToHost(dev)[0x2A3Au] = c.scaler ? 0x10u : 0u;
  K::guestVAToHost(dev)[0x2A3Bu] = c.gamma ? 0x40u : 0u;
  K::writeGuestBE32(dev + 0x4E0Cu,c.changedFront ? 0u : 0xBF130000u);
  const uint32_t fetch[6] = {0x80000002u | (40u << 22),0xBF130000u | c.format,
    (719u << 13) | 1279u,0xA5001414u,0xABCDEF12u,0x12345678u};
  for (uint32_t i = 0; i < 6u; ++i) K::writeGuestBE32(front + 28u + 4u*i,fetch[i]);
  for (uint32_t i = 0; i < 7u; ++i) K::writeGuestBE32(rect + 4u*i,i == 2u ? 1280u : i == 3u ? 720u : 0u);
  if (!c.changedRect) std::memcpy(K::guestVAToHost(dev + 0x33D0u),K::guestVAToHost(rect),28u);
  K::ExportRegistry::instance().callOrStub("RtlInitializeCriticalSection",dev + 0x34F0u,0,0,0,0,0);
  allocations = queries = retrains = reentries = 0; writeback = 0u;
  epilogCalls = 0; epilogR4 = 0;
  calls = DisplayCalls{};
}
bool sameDevice(const std::array<uint8_t,0x5000>& expected) {
  bool same = true;
  for (uint32_t i = 0; i < expected.size(); ++i) {
    // QPC-derived swap times cannot match between sequential executions.
    if (i >= 0x4E24u && i < 0x4E30u) continue;
    if (expected[i] != K::guestVAToHost(dev)[i]) {
      if (same) std::printf("  device mismatch at +%04X expected=%02X actual=%02X\n",i,expected[i],K::guestVAToHost(dev)[i]);
      same = false;
    }
  }
  return same;
}
struct Execution {
  std::vector<uint32_t> registers;
  std::vector<std::array<uint32_t,4>> order;
  uint64_t swaps = 0, waits = 0;
  // boot gaps: the display gamma ramp the DC_LUT writes left (gfx/gamma_ramp.hpp, the scan-out's) and the literal-PM4
  // gap parts the consumer applied (native executions only)
  std::vector<uint32_t> ramp;
  uint64_t gaps = 0;
  bool bad = false;
  static void flush(void* user) { static_cast<Execution*>(user)->order.push_back({0u,0u,0u,0u}); }
  static void swap(void* user,uint32_t address,uint32_t width,uint32_t height) {
    static_cast<Execution*>(user)->order.push_back({1u,address,width,height});
  }
};
Execution execute(const std::vector<uint8_t>& savedList,const std::vector<uint8_t>& savedBytes,bool native,
                  bool requireSwap = true) {
  Execution result;
  auto engine = std::make_unique<sr::gfx::Pm4Engine>();
  engine->cpMode = true; engine->submitDraws = false;
  engine->cp.user = &result; engine->cp.flush = Execution::flush; engine->cp.swap = Execution::swap;
  auto& registers = sr::gfx::getXenosRegisters();
  for (uint32_t i = 0; i < sr::gfx::XenosRegisterFile::kNumRegisters; ++i) registers.write(i,0xA5000000u | i);
  registers.write(0x1973u,0u);
  registers.write(0x1E4Eu,0xA5001E4Fu);  // boot gaps: bit 0 set, so the persisted display's REG_RMW (&= ~1) shows
  auto& gammaRamp = sr::gfx::gamma::displayRamp();
  gammaRamp.reset();
  const uint64_t gaps0 = N::segGapPartsApplied();
  const uint32_t end = K::readGuestU32BE(dev + 0x32D0u);
  if (end < list || end > list + savedList.size() || ((end-list) & 7u)) { result.bad = true; return result; }
  for (uint32_t p = list; p < end; p += 8u) {
    const uint32_t header = K::readGuestU32BE(p), phys = K::readGuestU32BE(p + 4u), count = header & 0x01FFFFFFu;
    const uint32_t va = (buf & 0xE0000000u) | (phys & 0x1FFFFFFFu);
    if ((header & 0xFE000000u) != 0x82000000u || !count || va < buf || uint64_t(va) + 4ull*count > buf + bytes) {
      result.bad = true; break;
    }
    if (native) {
      const uint64_t id = N::segmentPublishId(phys,count);
      if (!id || !N::cpSegment(*engine,id,phys,count)) result.bad = true;
    } else {
      engine->execute(savedBytes.data() + va-buf,4u*count);
      if (engine->lastConsumed != 4u*count) {
        std::printf("  oracle segment va=%08X dw=%u consumed=%zu expected=%u\n",va,count,engine->lastConsumed,4u*count);
        result.bad = true;
      }
    }
  }
  result.registers.assign(registers.data(),registers.data() + sr::gfx::XenosRegisterFile::kNumRegisters);
  result.swaps = engine->cpSwaps; result.waits = engine->cpWaits;
  result.ramp.assign(gammaRamp.table,gammaRamp.table + 256);
  for (const auto& segment : gammaRamp.pwl) result.ramp.insert(result.ramp.end(),segment,segment + 3);
  result.gaps = N::segGapPartsApplied() - gaps0;
  if (requireSwap &&
      (result.swaps != 1u || result.order.size() != 2u || result.order[0][0] != 0u || result.order[1][0] != 1u))
    result.bad = true;
  if (!requireSwap && result.swaps) result.bad = true;
  return result;
}

// ---- Tracker mode (the tcmp1 diagnosis, 2026-10-14; `--swap-sdk-test tracker`, SWAP on) ----
// The GPU write tracker (kernel/gpu_write_watch.hpp) makes a page it validated PAGE_READONLY in both physical copies;
// the first CPU store, guest or host, faults into its handler, which releases the block. The cursor's reserved block on
// such a page is as writable as the SDK body finds it: the leaf must admit it, and its own stores must release the
// protection exactly as the SDK body's do. Pages protected as the CP's shared-memory validation does (the transport
// pair's fixtures, native_transport_test.cpp).
bool protectionIs(uint32_t va,DWORD protection) {
  MEMORY_BASIC_INFORMATION region{};
  return VirtualQuery(K::guestVAToHost(va),&region,sizeof(region)) && region.State == MEM_COMMIT &&
         region.Protect == protection;
}
uint32_t pageBits(uint32_t page) {  // 1 owned (protGen), 2 validated (clean), 4 the 0x8 copy RO, 8 the 0xA copy RO
  return (W::protGen(page) ? 1u : 0u) | (W::clean(page,8u) ? 2u : 0u) |
         (protectionIs(0x80000000u | (page << 12),PAGE_READONLY) ? 4u : 0u) |
         (protectionIs(0xA0000000u | (page << 12),PAGE_READONLY) ? 8u : 0u);
}
// The native resource route (mem.resources' bake: SR_MEM_RESOURCES default ON in the dev build): the GPU write tracker
// is off there (gww::enabled() false); the frozen-pass A/B's dual mode keeps it.
bool nativeResourceRoute() { return sr::gfx::memres::resourceMode() && !sr::gfx::memres::resourceAbDual(); }
bool trackPages(const uint32_t* pages,uint32_t n) {
  for (unsigned i = 0; i < 64u; ++i) W::beginFrame();  // past the cooldown of every block that faulted before
  W::protectForValidation(pages,n);
  bool owned = true;
  for (uint32_t i = 0; i < n; ++i) { W::markValidated(pages[i],8u); owned &= pageBits(pages[i]) == 15u; }
  std::vector<uint32_t> pending; W::takeReleasedBlocks(pending);  // the call starts with no release pending
  return owned;
}
// The tracked pages' bytes before a call (the 0x8 copy, where the fixture and the stores write).
std::vector<std::vector<uint8_t>> pageBytes(const uint32_t* pages,uint32_t n) {
  std::vector<std::vector<uint8_t>> bytes;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* at = K::guestVAToHost(0x80000000u | (pages[i] << 12));
    bytes.emplace_back(at,at + 4096u);
  }
  return bytes;
}
// After a call: every tracked page whose bytes changed was released by the tracker's handler (a fault, not a
// host-write scope) and its block published; a page the call did not store to may stay owned (a native record span
// writes no guest bytes). `stored` = how many pages changed; the state is printed when the rule fails.
bool storesReleased(const W::Stats& before,const uint32_t* pages,uint32_t n,
                    const std::vector<std::vector<uint8_t>>& prior,unsigned& stored) {
  const W::Stats now = W::stats();
  std::vector<uint32_t> released; W::takeReleasedBlocks(released);
  stored = 0;
  bool ok = now.hostWrites == before.hostWrites;
  for (uint32_t i = 0; i < n; ++i) {
    const bool changed = std::memcmp(prior[i].data(),K::guestVAToHost(0x80000000u | (pages[i] << 12)),4096u) != 0;
    stored += changed;
    ok &= !changed || pageBits(pages[i]) == 0u;
  }
  ok &= !stored || (now.faults > before.faults && !released.empty());
  if (!ok)
    std::printf("  tracker release: pages=%u stored=%u faults+%llu hostWrites+%llu released=%zu bits=%u/%u\n",n,stored,
                (unsigned long long)(now.faults - before.faults),(unsigned long long)(now.hostWrites - before.hostWrites),
                released.size(),pageBits(pages[0]),n > 1u ? pageBits(pages[1]) : 0u);
  return ok;
}
void trackerChecks(bool native) {
  check(native,"tracker mode: the SWAP leaf is on");
  if (!native) return;
  // The real tracker takes the guest arena from the process-global registration (native_transport_test.cpp registers
  // it the same way); held for these checks only, the prior registration restored after.
  uint8_t* priorBase = nullptr; size_t priorSize = 0;
  const K::ExCreateThreadRunnerFn priorRunner = K::getExCreateThreadRunner(&priorBase,&priorSize);
  K::registerExCreateThreadRunner(priorRunner,K::getGuestBase(),K::getGuestSize());
  // no pre-swap kick: the reserved block is at the tracked cursor (a kick would move it into a fresh segment)
  const Case cases[] = {{6u,true,false,false,false,false,false},{50u,true,true,true,true,true,false}};
  // offset 0xF00: the reserved block [cursor+4, cursor+300) crosses into the next page (kick threshold raised)
  // mem.resources bake (native/mem-native a1fda568, 2026-10-17): on the native resource route (the dev default) the GPU
  // write tracker is off and no page is ever tracker-owned (only the page model, SR_MEM_RESOURCES=0, protects pages; the
  // runners pin =0 for the tracker cases). There the admission is writableRange alone (docs/RELEASE_COMPOSITION.md 9.6):
  // the native statement - the block on ordinary writable pages is admitted, a block on pages protected through the
  // tracker's API declines before any store.
  if (nativeResourceRoute()) {
    check(!W::enabled(),"native route: the GPU write tracker is off (gww::enabled() false)");
    for (uint32_t offset : {0u,4u,0xF00u}) for (const Case& c : cases) {
      const uint32_t lo = ((buf + offset + 4u) & 0x1FFFFFFFu) >> 12, hi = ((buf + offset + 299u) & 0x1FFFFFFFu) >> 12;
      const uint32_t pages[2] = {lo,hi};
      const uint32_t n = hi == lo ? 1u : 2u;
      char name[200];
      auto label = [&](const char* what) {
        std::snprintf(name,sizeof(name),"native route: block fmt=%u all=%u offset=%u pages=%u: %s",c.format,
                      unsigned(c.scaler),offset,n,what);
        return name;
      };
      auto prepare = [&] {
        fixture(c,offset);
        if (offset >= 0xF00u) K::writeGuestBE32(dev + 48u,buf + 0x1800u);
      };
      PPCContext before{}; before.r1.u64 = stack; before.r3.u64 = dev; before.r4.u64 = front; before.r5.u64 = rect;
      before.r13.u64 = stack - 0x1000u; before.lr = 0x12345678u;
      prepare();
      const uint64_t count = N::swapSdkCalls(), cursorDecl = N::swapSdkDeclWhy(N::kSwCursorWrite);
      PPCContext actual = before;
      sub_82112050(actual,K::getGuestBase());
      check(N::swapSdkCalls() == count + 1u && N::swapSdkDeclWhy(N::kSwCursorWrite) == cursorDecl,
            label("the native SDK body admits the block on ordinary writable pages (writableRange)"));
      prepare();
      const bool owned = trackPages(pages,n);
      const uint64_t decl1 = N::swapSdkDeclWhy(N::kSwCursorWrite);
      PPCContext leaf = before;
      const bool handled = N::swapLeaf(leaf,K::getGuestBase());
      bool kept = true;
      for (uint32_t i = 0; i < n; ++i) kept &= pageBits(pages[i]) == 15u;
      check(owned && !handled && N::swapSdkDeclWhy(N::kSwCursorWrite) == decl1 + 1u && kept,
            label("a block on pages protected through the tracker's API declines (cursorWrite) before any store"));
      for (uint32_t i = 0; i < n; ++i) {  // a store releases each page (the tracker's handler)
        const uint32_t a = 0x80000000u | (pages[i] << 12);
        K::writeGuestBE32(a,K::readGuestU32BE(a));
      }
      std::vector<uint32_t> released; W::takeReleasedBlocks(released);
    }
  } else
  for (uint32_t offset : {0u,4u,0xF00u}) for (const Case& c : cases) {
    const uint32_t lo = ((buf + offset + 4u) & 0x1FFFFFFFu) >> 12, hi = ((buf + offset + 299u) & 0x1FFFFFFFu) >> 12;
    const uint32_t pages[2] = {lo,hi};
    const uint32_t n = hi == lo ? 1u : 2u;
    char name[200];
    auto label = [&](const char* what) {
      std::snprintf(name,sizeof(name),"tracker-owned block fmt=%u all=%u offset=%u pages=%u: %s",c.format,
                    unsigned(c.scaler),offset,n,what);
      return name;
    };
    auto prepare = [&] {
      fixture(c,offset);
      if (offset >= 0xF00u) K::writeGuestBE32(dev + 48u,buf + 0x1800u);  // the block crosses a page, no kick before it
      return trackPages(pages,n);
    };
    PPCContext before{}; before.r1.u64 = stack; before.r3.u64 = dev; before.r4.u64 = front; before.r5.u64 = rect;
    before.r13.u64 = stack - 0x1000u; before.lr = 0x12345678u;
    // the oracle: the SDK body inside the scoped oracles; its stores release the tracked block
    const bool ownedOracle = prepare();
    const auto oracleBytes = pageBytes(pages,n);
    const W::Stats s0 = W::stats();
    PPCContext oracle = before;
    { N::PresentGateOracle guard; N::ShaderConstRunsOracle shader; __imp__sub_82112050(oracle,K::getGuestBase()); }
    unsigned oracleStored = 0, stored = 0;
    const bool oracleReleased = storesReleased(s0,pages,n,oracleBytes,oracleStored);
    const unsigned expectedAllocations = allocations;
    std::array<uint8_t,0x5000> expectedDevice;
    std::memcpy(expectedDevice.data(),K::guestVAToHost(dev),expectedDevice.size());
    std::vector<uint8_t> expectedList(K::guestVAToHost(list),K::guestVAToHost(list) + 0x4000u);
    // the production route on the same tracked block
    const bool owned = prepare();
    const auto actualBytes = pageBytes(pages,n);
    const uint64_t count = N::swapSdkCalls(), cursorDecl = N::swapSdkDeclWhy(N::kSwCursorWrite);
    const uint64_t swaps = K::getVdSwapCount();
    const W::Stats s1 = W::stats();
    PPCContext actual = before;
    sub_82112050(actual,K::getGuestBase());
    const bool released = storesReleased(s1,pages,n,actualBytes,stored);
    check(ownedOracle && owned,label("the tracker owns the reserved block's pages read-only, both copies"));
    check(N::swapSdkCalls() == count + 1u && N::swapSdkDeclWhy(N::kSwCursorWrite) == cursorDecl,
          label("the native SDK body admits the tracker-owned block"));
    check(sameDevice(expectedDevice) && actual.r3.u64 == oracle.r3.u64 && actual.r1.u64 == before.r1.u64 &&
          actual.r13.u64 == before.r13.u64 && actual.lr == before.lr && allocations == expectedAllocations &&
          K::getVdSwapCount() == swaps + 1u &&
          !std::memcmp(expectedList.data(),K::guestVAToHost(list),expectedList.size()),
          label("device, ABI, cursor and list equal the SDK oracle's"));
    // the SDK body stores into the block (PM4); the native body stores where it writes guest bytes (a record span
    // writes none): either way a page stored to is released by the tracker's handler, not left owned or host-scoped
    check(oracleReleased && oracleStored >= 1u && released,
          label("every tracked page a store changed is released by the tracker's handler (the SDK body's and the native "
                "body's)"));
  }
  // A block mixing a tracker-owned page with one the fixture made read-only (no tracker ownership) still declines,
  // before any store: the tracked page stays owned.
  {
    const Case plain{6u,true,false,false,false,false,false};
    fixture(plain,0xF00u); K::writeGuestBE32(dev + 48u,buf + 0x1800u);
    const uint32_t lo = ((buf + 0xF04u) & 0x1FFFFFFFu) >> 12, hiVA = buf + 0x1000u;
    const bool owned = trackPages(&lo,1u);
    DWORD old = 0, ignored = 0;
    const bool readOnly = VirtualProtect(K::guestVAToHost(hiVA),4096u,PAGE_READONLY,&old) != 0;
    const uint64_t cursorDecl = N::swapSdkDeclWhy(N::kSwCursorWrite);
    PPCContext context{}; context.r1.u64 = stack; context.r3.u64 = dev; context.r4.u64 = front;
    const bool handled = N::swapLeaf(context,K::getGuestBase());
    if (readOnly) VirtualProtect(K::guestVAToHost(hiVA),4096u,old,&ignored);
    check(owned && readOnly && !handled && N::swapSdkDeclWhy(N::kSwCursorWrite) == cursorDecl + 1u &&
          pageBits(lo) == 15u && !queries && !retrains,
          "a block mixing a tracker-owned page with an unowned read-only one declines (cursorWrite) before any store");
    K::writeGuestBE32(buf + 0xF04u,K::readGuestU32BE(buf + 0xF04u));  // a store releases the tracked page
    std::vector<uint32_t> released; W::takeReleasedBlocks(released);
  }
  // The two post-swap literal fallbacks (Astra round 1, P3): an Entry that declines after VdSwap (here VdSwap's export
  // sets the tile auto-predication bit every Entry declines on) keeps the call taken through the literal bytes, each
  // fallback counted once - total and reason - and sampled (SWAP_DECL why=fallbackFront / fallbackScaler).
  {
    const Case all{50u,true,true,true,true,true,false};
    fixture(all,0u);
    auto sum = [] { uint64_t s = 0; for (int w = 0; w < N::kSwWhyN; ++w) s += N::swapSdkDeclWhy(w); return s; };
    const uint64_t calls = N::swapSdkCalls(), total = sum();
    const uint64_t front0 = N::swapSdkDeclWhy(N::kSwFallbackFront), scaler0 = N::swapSdkDeclWhy(N::kSwFallbackScaler);
    const uint64_t sampledFront = N::swapSdkDeclSampled(N::kSwFallbackFront);
    const uint64_t sampledScaler = N::swapSdkDeclSampled(N::kSwFallbackScaler);
    PPCContext context{}; context.r1.u64 = stack; context.r3.u64 = dev; context.r4.u64 = front; context.r5.u64 = rect;
    context.r13.u64 = stack - 0x1000u; context.lr = 0x12345678u;
    breakEntry = true;
    const bool handled = N::swapLeaf(context,K::getGuestBase());
    breakEntry = false;
    K::guestVAToHost(dev)[0x2A38u] &= ~0x02u;
    check(handled && N::swapSdkCalls() == calls + 1u && N::swapSdkDeclWhy(N::kSwFallbackFront) == front0 + 1u &&
          N::swapSdkDeclWhy(N::kSwFallbackScaler) == scaler0 + 1u && sum() == total + 2u,
          "the post-swap fallbacks keep the call taken, each counted once (total and reason)");
    check(N::swapSdkDeclSampled(N::kSwFallbackFront) == sampledFront + 1u &&
          N::swapSdkDeclSampled(N::kSwFallbackScaler) == sampledScaler + 1u,
          "each post-swap fallback writes its SWAP_DECL sample");
  }
  K::registerExCreateThreadRunner(priorRunner,priorBase,priorSize);
}

// ---- boot gaps: the emitters directly (`difftest --boot-gaps-test` = `--swap-sdk-test bootgaps`; native/boot-gaps) ----
// Each case runs twice from the same fixture: the SDK body inside both scoped oracles (every native leaf declines there:
// the SDK's own bytes) then sub_820FD8C0 (inside the scope the segmentless close declines too: the SDK's close); and the
// production entry (the override: the leaf under its gate) then the production close. Compared: the device, the
// volatile registers the asm leaves, the segment list, the literal bytes (SR_NATIVE_FLUSH_PM4=1 or a gate off: the
// renderers / the guest bodies against the SDK, byte for byte), the consumer's registers, waits and display gamma ramp
// from the two closed streams (the native one applied twice), the display exports' calls argument for argument; the
// leaf's own counter; with records no literal gap part (one exactly: a platform scaler program, written as the SDK).
constexpr uint32_t rampVA = 0x70656000u;
void writeRamp(uint32_t seed) {
  uint8_t* p = K::guestVAToHost(rampVA);
  for (uint32_t i = 0; i < 768u; ++i) {
    const uint32_t v = (i * 0x9E37u + seed * 0x85EBu + (i >> 3) * 0x101u) & 0xFFFFu;
    p[2u*i] = uint8_t(v >> 8); p[2u*i + 1u] = uint8_t(v);
  }
}
struct Arm {
  std::array<uint8_t,0x5000> device;
  std::vector<uint8_t> list, buffer;
  PPCContext ctx;
  DisplayCalls calls;
  std::vector<std::vector<uint8_t>> planes;  // each ramp builder call's input planes, in call order
};
// r3 .. r10, the argument registers the generated SDK bodies keep in ctx. r0 / r11 / r12 are local to a generated body
// (tools/regen/superman_regen.toml [localize] non_argument_as_local): neither arm's ctx copy is part of the contract (a
// ctx-style callee inside the SDK arm - 820FC910's reserve - may leave one there; no caller reads it after a call).
uint64_t gpr(const PPCContext& c, int i) {
  switch (i) {
    case 3: return c.r3.u64; case 4: return c.r4.u64; case 5: return c.r5.u64; case 6: return c.r6.u64;
    case 7: return c.r7.u64; case 8: return c.r8.u64; case 9: return c.r9.u64; case 10: return c.r10.u64;
    default: return 0u;
  }
}
constexpr uint32_t kR3 = 1u << 3, kR3to10 = 0x7F8u, kModeRegs = (1u << 3) | (1u << 9) | (1u << 10);
int bootGapsDirect(bool display, bool noGaps, bool literalSplit) {
  const Case plain{6u,true,false,false,false,false,false};
  const bool modeOn = envOn("SR_NATIVE_MODE_STATE") && N::on() && N::segmentlessOn();
  const bool modeRecords = modeOn && !literalSplit;
  uint8_t* base = K::getGuestBase();
  std::printf("[BOOT_GAPS_TEST] display=%d modeState=%d records=%d literalSplit=%d\n",int(display),int(modeOn),
              int(noGaps),int(literalSplit));
  // One case: `setup` after the fixture, `sdk` the SDK body (the oracle arm) and `production` the production entry,
  // `regs` = a mask of the volatile registers compared (bit n = rn).
  auto run = [&](const char* what, uint32_t offset, const std::function<void()>& setup,
                 void (*sdk)(PPCContext&, uint8_t*), void (*production)(PPCContext&, uint8_t*), const PPCContext& in,
                 uint32_t regs, bool leafOn, bool records, uint64_t (*counter)(), uint32_t gapsExpected) {
    auto arm = [&](bool oracle) {
      Arm a;
      fixture(plain,offset);
      setup();
      a.ctx = in;
      PPCContext close = in;
      close.r3.u64 = dev;
      builderPlanes.clear();
      recordPlanes = true;
      if (oracle) {
        N::PresentGateOracle guard; N::ShaderConstRunsOracle shader;
        sdk(a.ctx,base);
        sub_820FD8C0(close,base);
      } else {
        production(a.ctx,base);
        sub_820FD8C0(close,base);
      }
      recordPlanes = false;
      a.planes = std::move(builderPlanes);
      builderPlanes.clear();
      std::memcpy(a.device.data(),K::guestVAToHost(dev),a.device.size());
      a.list.assign(K::guestVAToHost(list),K::guestVAToHost(list) + 0x4000u);
      a.buffer.assign(K::guestVAToHost(buf),K::guestVAToHost(buf) + bytes);
      a.calls = calls;
      return a;
    };
    const Arm want = arm(true);
    const Execution expected = execute(want.list,want.buffer,false,false);
    const uint64_t n0 = counter ? counter() : 0u;
    const Arm got = arm(false);
    const uint64_t n1 = counter ? counter() : 0u;
    const bool nativeStream = N::on() && N::segmentlessOn();
    const Execution observed = execute(want.list,want.buffer,nativeStream,false);
    const Execution replay = execute(want.list,want.buffer,nativeStream,false);
    bool same = want.device == got.device && want.list == got.list && want.calls == got.calls &&
                want.planes == got.planes && got.ctx.r1.u64 == in.r1.u64 && got.ctx.lr == in.lr;
    if (want.planes != got.planes) {
      std::printf("  ramp builder calls sdk=%zu production=%zu\n",want.planes.size(),got.planes.size());
      for (size_t c = 0; c < want.planes.size() && c < got.planes.size(); ++c)
        for (size_t i = 0; i < want.planes[c].size() && i < got.planes[c].size(); ++i)
          if (want.planes[c][i] != got.planes[c][i]) {
            std::printf("  ramp builder call %zu (%s) planes byte %zu sdk=%02X production=%02X\n",c,
                        want.planes[c][0] ? "82105458" : "82105278",i - 1u,want.planes[c][i],got.planes[c][i]);
            c = want.planes.size();
            break;
          }
    }
    for (int r = 3; r <= 10; ++r) {
      if (!(regs & (1u << r))) continue;
      const uint64_t wv = gpr(want.ctx,r), gv = gpr(got.ctx,r);
      if (wv != gv) { std::printf("  r%d sdk=%llX production=%llX\n",r,(unsigned long long)wv,(unsigned long long)gv); same = false; }
    }
    if (!leafOn || literalSplit) same &= want.buffer == got.buffer;
    if (want.device != got.device)
      for (size_t i = 0; i < want.device.size(); ++i)
        if (want.device[i] != got.device[i]) {
          std::printf("  device +%04zX sdk=%02X production=%02X\n",i,want.device[i],got.device[i]); break;
        }
    if (want.buffer != got.buffer && (!leafOn || literalSplit))
      for (size_t i = 0; i + 4u <= want.buffer.size(); i += 4u)
        if (std::memcmp(want.buffer.data() + i,got.buffer.data() + i,4u)) {
          std::printf("  buffer +%05zX sdk=%02X%02X%02X%02X production=%02X%02X%02X%02X\n",i,want.buffer[i],
                      want.buffer[i+1],want.buffer[i+2],want.buffer[i+3],got.buffer[i],got.buffer[i+1],got.buffer[i+2],
                      got.buffer[i+3]);
          break;
        }
    bool applied = !expected.bad && !observed.bad && !replay.bad && expected.registers == observed.registers &&
                   expected.registers == replay.registers && expected.waits == observed.waits &&
                   expected.ramp == observed.ramp && expected.ramp == replay.ramp;
    if (!applied) {
      std::printf("  apply bad=%u/%u/%u waits=%llu/%llu ramp=%d\n",expected.bad,observed.bad,replay.bad,
                  (unsigned long long)expected.waits,(unsigned long long)observed.waits,int(expected.ramp == observed.ramp));
      for (size_t i = 0; i < expected.registers.size() && i < observed.registers.size(); ++i)
        if (expected.registers[i] != observed.registers[i]) {
          std::printf("  first register mismatch reg=%04X expected=%08X actual=%08X\n",unsigned(i),expected.registers[i],
                      observed.registers[i]);
          break;
        }
    }
    const bool counted = !counter || n1 - n0 == uint64_t(leafOn ? 1u : 0u);
    const bool gapsOk = !(leafOn && records) || (observed.gaps == gapsExpected && replay.gaps == gapsExpected);
    if (!counted || !gapsOk)
      std::printf("  counter +%llu (leaf %d) gaps %llu/%llu expected %u\n",(unsigned long long)(n1 - n0),int(leafOn),
                  (unsigned long long)observed.gaps,(unsigned long long)replay.gaps,gapsExpected);
    char name[240];
    std::snprintf(name,sizeof(name),"%s offset=%u leaf=%d: device, registers, list, %sexports, ramp builder planes, "
                  "applied registers / waits / "
                  "gamma ramp, counted, gaps=%llu",what,offset,int(leafOn),(!leafOn || literalSplit) ? "literal bytes, " : "",
                  (unsigned long long)observed.gaps);
    check(same && applied && counted && gapsOk,name);
  };
  PPCContext in{}; in.r1.u64 = stack; in.r3.u64 = dev; in.r13.u64 = stack - 0x1000u; in.lr = 0x12345678u;
  char what[160];
  // 1. GpuRingEmitModeStateReg_82103238(dev, mode): every mode, with and without the kick before its two runs
  for (uint32_t mode = 0; mode < 4u; ++mode) for (bool kick : {false,true}) {
    PPCContext c = in; c.r4.u64 = mode | 0xFFFFFFF0u;  // the asm inserts only the low two bits
    std::snprintf(what,sizeof(what),"mode state 82103238 mode=%u kick=%d",mode,int(kick));
    run(what,4u,[&] {
      K::writeGuestBE32(dev + 0x2A24u,0x5A5A1A5Au);
      if (kick) K::writeGuestBE32(dev + 48u,buf);
    },__imp__sub_82103238,sub_82103238,c,kModeRegs,modeOn,modeRecords,[] { return N::modeStateCalls(); },0u);
  }
  // 2. the ramp emitters 821056E0 (table) / 821057C0 (PWL): two ramps, the reserve fitting (the 64 KB limit) and not
  //    (the default 8 KB: 820FC910's flush and new segment from the allocator)
  for (bool wide : {false,true}) for (uint32_t seed : {1u,2u}) for (bool fits : {true,false}) {
    PPCContext c = in; c.r4.u64 = rampVA;
    std::snprintf(what,sizeof(what),"gamma ramp %s seed=%u fits=%d",wide ? "821057C0 (PWL)" : "821056E0 (table)",seed,
                  int(fits));
    run(what,0u,[&] {
      writeRamp(seed);
      if (fits) K::writeGuestBE32(dev + 44u,buf + bytes - 4u);
    },wide ? __imp__sub_821057C0 : __imp__sub_821056E0,wide ? sub_821057C0 : sub_821056E0,c,kR3to10,display,noGaps,
       wide ? +[] { return N::displayCounters().rampWide; } : +[] { return N::displayCounters().ramp; },0u);
  }
  // 3. GpuSetGammaRamp_82111CB8(dev, mode, ramp | 0): the wide mode 0x28280136, two table modes; a ramp and the asm's own
  //    linear default (built in its frame by 82105458 / 82105278)
  for (uint32_t mode : {0x28280136u,0x00120000u,0x28280106u}) for (bool given : {true,false}) {
    PPCContext c = in; c.r4.u64 = mode; c.r5.u64 = given ? rampVA : 0u;
    std::snprintf(what,sizeof(what),"set gamma 82111CB8 mode=%08X ramp=%s",mode,given ? "given" : "default");
    run(what,4u,[&] {
      writeRamp(3u);
      K::writeGuestBE32(dev + 44u,buf + bytes - 4u);
    },__imp__sub_82111CB8,sub_82111CB8,c,kR3to10,display,noGaps,[] { return N::displayCounters().setGamma; },0u);
  }
  // 4. GpuScalerEmitCommandBuffer_8210A788(dev, w, h, dev+0x3424): two output sizes, the display information zero and set
  //    (8210A788's clamp branch: dev+0x4E04 != dev+0x4DFC), a source rectangle and an output height under the display's
  //    (600 of 720: 8210A788's half-margin r7 = (dev+0x4E00 - out h) >> 1 is non-zero - with equal heights it is 0 for
  //    any shift, boot_gaps_mutation_run's scaler_prelude_arg passed unseen); the platform export returning no program
  //    (the record) and a 4-dword program (the SDK's bytes written as a literal gap, counted)
  for (uint32_t size : {0u,1u}) for (bool info : {false,true}) for (uint32_t program : {0u,4u}) {
    PPCContext c = in; c.r4.u64 = size ? 640u : 1280u; c.r5.u64 = size ? 480u : 720u; c.r6.u64 = dev + 0x3424u;
    std::snprintf(what,sizeof(what),"scaler 8210A788 %ux%u info=%d program=%u",unsigned(c.r4.u64),unsigned(c.r5.u64),
                  int(info),program);
    const uint64_t programs0 = N::displayCounters().scalerProgram;
    scalerProgram = program;
    run(what,0u,[&] {
      K::writeGuestBE32(dev + 44u,buf + bytes - 4u);
      if (info) {
        K::writeGuestBE32(dev + 0x4DFCu,1280u); K::writeGuestBE32(dev + 0x4E00u,720u); K::writeGuestBE32(dev + 0x4E04u,1024u);
        K::writeGuestBE32(dev + 0x3424u + 8u,1280u); K::writeGuestBE32(dev + 0x3424u + 12u,720u);
        K::writeGuestBE32(dev + 0x3424u + 0x14u,600u);  // the output height (8210A660's a5[5] when non-zero)
      }
    },__imp__sub_8210A788,sub_8210A788,c,kR3,display,noGaps,[] { return N::displayCounters().scaler; },program ? 1u : 0u);
    scalerProgram = 0u;
    check(N::displayCounters().scalerProgram - programs0 == (display && program ? 1u : 0u),
          "the scaler leaf's platform program is counted (scalerProgram) exactly when the leaf took one");
  }
  // 5. VdGetCurrentDisplayGamma(DWORD* type, float* power), the ramp builders' export (kernel/stubs.cpp): exactly its
  //    two 4-byte out-parameters - type 2 (BT.709), power 2.22222233f (Xenia's defaults) - in the builders' frame layout
  //    (power at sp+0x50, type at sp+0x58), every other word of a 0x300-byte span left as its canary (until
  //    native/boot-gaps the stub wrote a 512-byte ramp through each pointer: the builders' frame over sp+0x50..0x257)
  {
    constexpr uint32_t frame = 0x70655600u, span = 0x300u, canary = 0xC0DEC0DEu;
    for (uint32_t o = 0; o < span; o += 4u) K::writeGuestBE32(frame + o,canary);
    const K::ExportFn gamma = K::ExportRegistry::instance().getExport("VdGetCurrentDisplayGamma");
    if (gamma) gamma(frame + 0x58u,frame + 0x50u,0u,0u,0u,0u);
    const float power = 2.22222233f;
    uint32_t powerBits = 0;
    std::memcpy(&powerBits,&power,sizeof(powerBits));
    uint32_t touched = 0;
    for (uint32_t o = 0; o < span; o += 4u)
      if (o != 0x50u && o != 0x58u && K::readGuestU32BE(frame + o) != canary) ++touched;
    const uint32_t type = K::readGuestU32BE(frame + 0x58u), bits = K::readGuestU32BE(frame + 0x50u);
    if (!gamma || type != 2u || bits != powerBits || touched)
      std::printf("  VdGetCurrentDisplayGamma export=%d type=%u power=%08X (want %08X) canaries overwritten %u\n",
                  int(gamma != nullptr),type,bits,powerBits,touched);
    check(gamma && type == 2u && bits == powerBits && !touched,
          "VdGetCurrentDisplayGamma writes its two out-parameters only: type 2 (BT.709), power 2.22222233, every "
          "canary around them intact");
  }
  std::printf("[BOOT_GAPS_TEST] scaler export calls: 8210A0F8's table built %u, stale %u (information)\n",
              scalerTablesBuilt,scalerTablesStale);
  std::printf("[BOOT_GAPS_TEST] %d passed, %d failed (the display emitters and the mode state against their SDK bodies; "
              "no live acceptance)\n",passed,failed);
  return failed ? 1 : 0;
}
}
// Difftest-only observer: both real callers reach this weak SDK alias. Forward
// unchanged to its actual body; the production game does not link this file.
void sub_8210BBE8(PPCContext& __restrict ctx, uint8_t* base) {
  ++epilogCalls;
  epilogR4 = ctx.r4.u64;
  __imp__sub_8210BBE8(ctx,base);
}
// Difftest-only observers of the display ramp builders GpuBuildDisplayGammaRampPWL_82105458 /
// GpuBuildDisplayGammaRamp_82105278 (r3 out, r4 the three 0x200-byte planes in): the difftest loads no guest image, so
// the transfer curves they build from .rdata constants are flat here and their output does not show the planes they
// were handed - 82111CB8's default ramp (the leaf builds it in its frame) is compared by its planes, recorded per call.
// Forwarded unchanged.
void sub_82105458(PPCContext& __restrict ctx, uint8_t* base) {
  notePlanes(1u,ctx.r4.u32);
  __imp__sub_82105458(ctx,base);
}
void sub_82105278(PPCContext& __restrict ctx, uint8_t* base) {
  notePlanes(0u,ctx.r4.u32);
  __imp__sub_82105278(ctx,base);
}
int runSwapSdkTest(const char* mode) {
  std::setvbuf(stdout,nullptr,_IONBF,0);
  K::registerStubs(); sr::guest::initializeDispatchTable();
  reenter = !std::strcmp(mode,"reentry"); mutate = !std::strcmp(mode,"mutating");
  auto& registry = K::ExportRegistry::instance();
  originalQuery = registry.getExport("VdGetSystemCommandBuffer"); originalRetrain = registry.getExport("VdRetrainEDRAM");
  registry.registerExport("VdGetSystemCommandBuffer",query); registry.registerExport("VdRetrainEDRAM",retrain);
  originalSwap = registry.getExport("VdSwap"); registry.registerExport("VdSwap",swap);
  originalScaler = registry.getExport("VdInitializeScalerCommandBuffer");
  originalPersist = registry.getExport("VdPersistDisplay"); originalFree = registry.getExport("MmFreePhysicalMemory");
  originalMode = registry.getExport("VdSetDisplayMode");
  registry.registerExport("VdInitializeScalerCommandBuffer",scalerInit); registry.registerExport("VdPersistDisplay",persistDisplay);
  registry.registerExport("MmFreePhysicalMemory",freePhysical); registry.registerExport("VdSetDisplayMode",displayMode);
  auto restore = [&] {
    registry.registerExport("VdGetSystemCommandBuffer",originalQuery); registry.registerExport("VdRetrainEDRAM",originalRetrain);
    registry.registerExport("VdSwap",originalSwap);
    registry.registerExport("VdInitializeScalerCommandBuffer",originalScaler); registry.registerExport("VdPersistDisplay",originalPersist);
    registry.registerExport("MmFreePhysicalMemory",originalFree); registry.registerExport("VdSetDisplayMode",originalMode);
  };
  uint8_t* dispatchBase = K::getGuestBase();
  PPCFunc* saved = PPC_LOOKUP_FUNC(dispatchBase,allocator); PPC_LOOKUP_FUNC(dispatchBase,allocator) = allocate;
  const bool native = N::swapOn();
  const char* pm4 = std::getenv("SR_NATIVE_FLUSH_PM4");
  const bool literal = !native || (pm4 && pm4[0] == '1');
  // boot gaps: SR_NATIVE_DISPLAY's moment test outside the oracles (the persisted-display cases need it on: with it off
  // the first Swap declines as before - the byte cases below), and whether the native Swap's segments hold no literal
  // PM4 at all (every producer it calls a record: the display leaves, 820F0048 under SR_NATIVE_DEVCREATE, records not
  // rendered back by SR_NATIVE_FLUSH_PM4).
  const bool display = N::displayActive();
  const bool noGaps = native && display && envOn("SR_NATIVE_DEVCREATE") && N::on() && N::segmentlessOn() &&
                      !(pm4 && pm4[0] == '1');
  if (!std::strcmp(mode,"bootgaps")) {
    const int rc = bootGapsDirect(display,noGaps,pm4 && pm4[0] == '1');
    restore();
    PPC_LOOKUP_FUNC(dispatchBase,allocator) = saved;
    return rc;
  }
  std::vector<Case> cases = {
    {6u,false,false,false,false,false,false}, {6u,true,false,false,false,false,false},
    {7u,true,false,false,false,false,false}, {50u,true,false,false,false,false,false},
    {6u,true,true,false,false,false,false}, {6u,true,false,true,false,false,false},
    {6u,true,false,false,true,false,false}, {6u,true,true,true,true,false,false},
    {6u,true,false,false,false,true,false}, {6u,true,false,false,false,true,true},
    {50u,true,true,true,true,true,true},
  };
  if (display) {
    // the persisted display (the first Swap): alone, with the scaler (the real first Swap's 0x2A3A bit 0x10 from the
    // device create), in the PWL display format, everything at once (82111CB8 in its wide mode); and 82111CB8's wide
    // mode without the persisted display
    cases.push_back({6u,true,false,false,false,false,false,true,false});
    cases.push_back({6u,true,false,true,false,false,false,true,false});
    cases.push_back({6u,true,false,true,false,false,false,true,true});
    cases.push_back({50u,true,true,true,true,true,true,true,true});
    cases.push_back({6u,true,false,false,true,false,false,false,true});
  }
  for (uint32_t offset : {0u,4u}) for (const Case& c : cases) {
    fixture(c,offset);
    PPCContext before{}; before.r1.u64 = stack; before.r3.u64 = dev; before.r4.u64 = front; before.r5.u64 = rect;
    before.r13.u64 = stack - 0x1000u; before.lr = 0x12345678u;
    PPCContext oracle = before;
    // SWAP on: the oracle is the SDK body inside both scoped SDK oracles (every transport leaf and the baked rows 5 /
    // 3b decline there, so its bytes are the SDK's own). SWAP off: the production route IS the unscoped SDK body - the
    // baked leaves take its calls (row 5 takes Swap's 820F0048 -> 820F0208 re-emit) - so the oracle runs unscoped too:
    // a scoped oracle would compare the SDK's literal tile rects with row 5's record span (the bare suite's 22 failures
    // since 5dc52749; the transport-fix lane, 2026-10-14).
    if (native) { N::PresentGateOracle guard; N::ShaderConstRunsOracle shader; __imp__sub_82112050(oracle,K::getGuestBase()); }
    else __imp__sub_82112050(oracle,K::getGuestBase());
    check(queries == 1u && retrains == 1u,"original SDK executes both real hardware-facing exports");
    const unsigned expectedAllocations = allocations;
    const unsigned expectedReentries = reentries;
    const unsigned expectedEpilogCalls = epilogCalls;
    const uint64_t expectedEpilogR4 = epilogR4;
    const uint32_t expectedWriteback = writeback;
    std::array<uint8_t,0x5000> expectedDevice;
    std::memcpy(expectedDevice.data(),K::guestVAToHost(dev),expectedDevice.size());
    std::vector<uint8_t> expectedBuffer(K::guestVAToHost(buf),K::guestVAToHost(buf) + bytes);
    std::vector<uint8_t> expectedList(K::guestVAToHost(list),K::guestVAToHost(list) + 0x4000u);
    // SWAP off: the unscoped oracle is the production route, whose display emitters (SR_NATIVE_DISPLAY on) write
    // records into its segments - executed as the production stream is. SWAP on (scoped) or the display off: literal.
    const Execution expected = execute(expectedList,expectedBuffer,!native && display && N::on() && N::segmentlessOn());
    const DisplayCalls expectedCalls = calls;
    fixture(c,offset);
    const uint64_t count = N::swapSdkCalls(), swaps = K::getVdSwapCount();
    const N::DisplayCounters d0 = N::displayCounters();
    PPCContext actual = before;
    if (!std::strcmp(mode,"leaf")) {  // the production override's own shape (kernel/hle_api_observe.cpp): unscoped
      if (!N::swapLeaf(actual,K::getGuestBase())) __imp__sub_82112050(actual,K::getGuestBase());
    } else if (!std::strcmp(mode,"indirect")) {
      actual.ctr.u32 = 0x82112050u; uint8_t* base = K::getGuestBase(); PPCContext& ctx = actual;
      PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
    } else sub_82112050(actual,K::getGuestBase());
    if (!std::strcmp(mode,"fault-fence")) K::writeGuestBE32(dev + 0x34D0u,0xDEADBEEFu);
    check(N::swapSdkCalls() == count + unsigned(native) && queries == unsigned(!native) && retrains == unsigned(!native),
          "production route retires both exports only when native SDK body executes");
    check(expectedEpilogCalls == 1u && epilogCalls == 1u && epilogR4 == expectedEpilogR4,
          "actual SDK and native caller invoke epilog once with identical full r4");
    bool same = sameDevice(expectedDevice) && actual.r3.u64 == oracle.r3.u64 &&
      actual.r1.u64 == before.r1.u64 && actual.r13.u64 == before.r13.u64 && actual.lr == before.lr &&
      allocations == expectedAllocations && reentries == expectedReentries && writeback == expectedWriteback &&
      K::getVdSwapCount() == swaps + 1u &&
      !std::memcmp(expectedList.data(),K::guestVAToHost(list),expectedList.size());
    if (literal) same &= !std::memcmp(expectedBuffer.data(),K::guestVAToHost(buf),expectedBuffer.size());
    char name[200];
    std::snprintf(name,sizeof(name),"SDK swap fmt=%u front=%u rect=%u scaler=%u gamma=%u previous=%u kick=%u offset=%u: device, ABI, cursor, list and literal bytes",
      c.format,c.changedFront,c.changedRect,c.scaler,c.gamma,c.previous,c.kick,offset);
    if (!same) {
      std::printf("  sdk return=%llX/%llX alloc=%u/%u lr=%llX/%llX\n",(unsigned long long)actual.r3.u64,
        (unsigned long long)oracle.r3.u64,allocations,expectedAllocations,(unsigned long long)actual.lr,(unsigned long long)before.lr);
      // the first differing word of the list and (literal) the buffer, oracle / actual (sameDevice prints the device's)
      auto first = [](const char* what,const uint8_t* want,uint32_t va,size_t n) {
        for (size_t i = 0; i + 4u <= n; i += 4u) {
          const uint32_t oracleWord = uint32_t(want[i]) << 24 | uint32_t(want[i + 1u]) << 16 |
                                      uint32_t(want[i + 2u]) << 8 | want[i + 3u];
          const uint32_t actualWord = K::readGuestU32BE(uint32_t(va + i));
          if (oracleWord != actualWord) {
            std::printf("  first %s difference at %08X: %08X / %08X\n",what,unsigned(va + i),oracleWord,actualWord);
            return;
          }
        }
      };
      first("list",expectedList.data(),list,expectedList.size());
      if (literal) first("buffer",expectedBuffer.data(),buf,expectedBuffer.size());
    }
    check(same,name);
    // Inputs and SDK scratch may be reused before the consumer reaches SWAP.
    std::memset(K::guestVAToHost(stack - 0x1B0u + 0x80u),0x77,24u);
    const Execution observed = execute(expectedList,expectedBuffer,N::on() && N::segmentlessOn());
    const Execution replay = execute(expectedList,expectedBuffer,N::on() && N::segmentlessOn());
    if (expected.bad || observed.bad || replay.bad || expected.registers != observed.registers ||
        expected.order != observed.order || expected.waits != observed.waits) {
      std::printf("  apply bad=%u/%u/%u waits=%llu/%llu order=%zu/%zu\n",expected.bad,observed.bad,replay.bad,
        (unsigned long long)expected.waits,(unsigned long long)observed.waits,expected.order.size(),observed.order.size());
      for (size_t i = 0; i < expected.registers.size() && i < observed.registers.size(); ++i)
        if (expected.registers[i] != observed.registers[i]) {
          std::printf("  first register mismatch reg=%04X expected=%08X actual=%08X\n",unsigned(i),expected.registers[i],observed.registers[i]); break;
        }
    }
    check(!expected.bad && !observed.bad && !replay.bad && expected.registers == observed.registers &&
      expected.registers == replay.registers && expected.order == observed.order && expected.order == replay.order &&
      expected.waits == observed.waits && expected.waits == replay.waits,
      "real closed segments: owned fetch, all registers, scaler suffix, ordered present and two applications");
    // boot gaps: the display emitters and the persisted display, against the SDK body. The scaler ran when the SDK arm
    // called its export (the fixture's bit, or a changed rectangle through 8210A9F8 - which the mutating allocator's
    // copy of the rectangle into dev+0x33D0 can turn off).
    const bool scalerRan = !expectedCalls.scaler.empty();
    const N::DisplayCounters d1 = N::displayCounters();
    std::snprintf(name,sizeof(name),"boot gaps fmt=%u scaler=%u gamma=%u persist=%u wide=%u offset=%u: the display exports "
      "called as the SDK calls them, the gamma ramp the scan-out reads, no literal PM4 (gaps=%llu)",c.format,scalerRan,
      c.gamma,c.persist,c.wide,offset,(unsigned long long)observed.gaps);
    bool display_ok = calls == expectedCalls && expected.ramp == observed.ramp && expected.ramp == replay.ramp;
    if (noGaps) display_ok &= observed.gaps == 0u && replay.gaps == 0u;
    if (native && display)  // the production route took each emitter natively, exactly once per SDK call
      display_ok &= d1.persist - d0.persist == unsigned(c.persist) && d1.scaler - d0.scaler == unsigned(scalerRan) &&
                     d1.setGamma - d0.setGamma == unsigned(c.gamma) &&
                     d1.ramp - d0.ramp == unsigned(c.persist && !c.wide) + unsigned(c.gamma && !c.wide) &&
                     d1.rampWide - d0.rampWide == unsigned(c.persist && c.wide) + unsigned(c.gamma && c.wide) &&
                     d1.rampDecl == d0.rampDecl && d1.scalerDecl == d0.scalerDecl && d1.setGammaDecl == d0.setGammaDecl &&
                     d1.scalerProgram == d0.scalerProgram;
    if (!display_ok)
      std::printf("  calls %d (scaler %zu/%zu persist %zu/%zu frees %zu/%zu modes %zu/%zu) ramp %d/%d gaps %llu/%llu "
                  "persist+%llu scaler+%llu setGamma+%llu ramp+%llu wide+%llu | decl ramp+%llu setGamma+%llu scaler+%llu "
                  "program+%llu reserveFail+%llu\n",int(calls == expectedCalls),
                  calls.scaler.size(),expectedCalls.scaler.size(),calls.persist.size(),expectedCalls.persist.size(),
                  calls.frees.size(),expectedCalls.frees.size(),calls.modes.size(),expectedCalls.modes.size(),
                  int(expected.ramp == observed.ramp),int(expected.ramp == replay.ramp),
                  (unsigned long long)observed.gaps,(unsigned long long)replay.gaps,
                  (unsigned long long)(d1.persist - d0.persist),(unsigned long long)(d1.scaler - d0.scaler),
                  (unsigned long long)(d1.setGamma - d0.setGamma),(unsigned long long)(d1.ramp - d0.ramp),
                  (unsigned long long)(d1.rampWide - d0.rampWide),(unsigned long long)(d1.rampDecl - d0.rampDecl),
                  (unsigned long long)(d1.setGammaDecl - d0.setGammaDecl),(unsigned long long)(d1.scalerDecl - d0.scalerDecl),
                  (unsigned long long)(d1.scalerProgram - d0.scalerProgram),
                  (unsigned long long)(d1.reserveFail - d0.reserveFail));
    if (calls.scaler.size() == expectedCalls.scaler.size())
      for (size_t k = 0; k < calls.scaler.size(); ++k)
        for (size_t i = 0; i < calls.scaler[k].size() && i < expectedCalls.scaler[k].size(); ++i)
          if (calls.scaler[k][i] != expectedCalls.scaler[k][i]) {
            std::printf("  scaler call %zu word %zu sdk=%08X production=%08X\n",k,i,expectedCalls.scaler[k][i],
                        calls.scaler[k][i]);
            break;
          }
    check(display_ok,name);
  }
  // Callback mutation/reentry are the 22 SDK parity fixtures above. Admission
  // fixtures have fixed input lifetimes, including the read-only front page.
  reenter = mutate = false;
  const Case plain{6u,true,false,false,false,false,false};
  // Each admission decline is counted once, by its reason (the tcmp1 diagnosis's SwapDeclWhy): the named reason moves
  // by one and no other (SWAP off: the leaf declines at its gate, nothing counted).
  auto whyTotal = [] { uint64_t t = 0; for (int w = 0; w < N::kSwWhyN; ++w) t += N::swapSdkDeclWhy(w); return t; };
  auto countedAs = [&](int why, uint64_t whyBefore, uint64_t totalBefore) {
    return N::swapSdkDeclWhy(why) == whyBefore + unsigned(native) && whyTotal() == totalBefore + unsigned(native);
  };
  const std::pair<uint32_t,int> modeCases[] = {
    {dev + 0x4E58u,N::kSwTimestamps}, {dev + 0x4DF4u,N::kSwPix}, {dev + 0x4E84u,N::kSwPix}, {dev + 0x3B28u,N::kSw3B28},
    {dev + 0x4E70u,N::kSwTimestamps}, {0x829AC0ECu,N::kSwHsio}, {cell,N::kSwRoot}, {0x82000948u,N::kSwRootRead}};
  for (const auto& [address,why] : modeCases) {
    // (the root pointer 0: its cell cannot be read - a read failure, counted apart from the other device's cell)
    fixture(plain,0u); K::writeGuestBE32(address,address == 0x82000948u ? 0u : 1u);
    std::array<uint8_t,0x5000> initial; std::memcpy(initial.data(),K::guestVAToHost(dev),initial.size());
    PPCContext context{}; context.r1.u64 = stack; context.r3.u64 = dev; context.r4.u64 = front;
    const PPCContext savedContext = context;
    const uint64_t whyBefore = N::swapSdkDeclWhy(why), totalBefore = whyTotal();
    check(!N::swapLeaf(context,K::getGuestBase()) && !std::memcmp(&context,&savedContext,sizeof(context)) &&
      !std::memcmp(initial.data(),K::guestVAToHost(dev),initial.size()) && !queries && !retrains &&
      countedAs(why,whyBefore,totalBefore),
      "unsupported device/global mode declines before SDK effects, counted by its reason");
  }
  // boot gaps: the persisted display (0x10) declines only with SR_NATIVE_DISPLAY off (=0, the A/B arm); no-GPU (0x02) and
  // the allocation failure (0x40) decline either way.
  const std::array<std::tuple<uint32_t,uint8_t,int>,5> byteCases = {{
    {dev + 0x2A39u,0x02u,N::kSwFlags2A39}, {dev + 0x2A39u,0x40u,N::kSwFlags2A39}, {dev + 0x2A39u,0x10u,N::kSwFlags2A39},
    {dev + 0x4FB0u,8u,N::kSwMovieCapture}, {0x829ABD8Cu,8u,N::kSwMovieGlobal}}};
  for (const auto& [address,value,why] : byteCases) {
    const bool persistByte = address == dev + 0x2A39u && value == 0x10u;
    if (persistByte && display) continue;  // taken natively: the persisted-display cases above
    fixture(plain,0u); K::guestVAToHost(address)[0] = value;
    const uint64_t count = N::swapSdkCalls();
    PPCContext context{}; context.r1.u64 = stack; context.r3.u64 = dev; context.r4.u64 = front;
    const uint64_t whyBefore = N::swapSdkDeclWhy(why), totalBefore = whyTotal();
    check(!N::swapLeaf(context,K::getGuestBase()) && N::swapSdkCalls() == count && !queries && !retrains &&
      countedAs(why,whyBefore,totalBefore),
      persistByte ? "the persisted display with SR_NATIVE_DISPLAY off declines before helpers, counted as flags2A39"
                  : "movie / no-GPU / allocation-failure byte modes decline before helpers, counted by their reason");
  }
  const std::pair<uint32_t,int> pageCases[] = {
    {dev + 0x4000u,N::kSwDevWrite}, {front,-1}, {stack - 0x1000u,N::kSwStack}, {buf,N::kSwCursorWrite}};
  for (const auto& [page,why] : pageCases) {
    fixture(plain,0u);
    uint8_t* pointer = K::guestVAToHost(page);
    K::hostRangeReadableCached(pointer,4096u);
    DWORD old = 0;
    const bool protectedPage = VirtualProtect(pointer,4096u,PAGE_READONLY,&old) != 0;
    PPCContext context{}; context.r1.u64 = stack; context.r3.u64 = dev; context.r4.u64 = front;
    const uint64_t whyBefore = why >= 0 ? N::swapSdkDeclWhy(why) : 0u, totalBefore = whyTotal();
    const bool handled = protectedPage && N::swapLeaf(context,K::getGuestBase());
    DWORD ignored = 0;
    if (protectedPage) VirtualProtect(pointer,4096u,old,&ignored);
    // Read-only front input is valid; writable device/stack/output pages are required (a page the fixture made
    // read-only is not the tracker's: tracker mode below covers the tracker's own).
    check(protectedPage && (page == front ? handled == native && whyTotal() == totalBefore
                                          : !handled && countedAs(why,whyBefore,totalBefore)),
          "cached mapping respects required output write permissions, a refusal counted by its reason");
  }
  if (!std::strcmp(mode,"tracker")) trackerChecks(native);
  restore();
  PPC_LOOKUP_FUNC(dispatchBase,allocator) = saved;
  std::printf("[SWAP_SDK_TEST] scaler export calls: 8210A0F8's table built %u, stale %u (information)\n",
              scalerTablesBuilt,scalerTablesStale);
  std::printf("[SWAP_SDK_TEST] %d passed, %d failed (normal SDK contract only; no live or full-native acceptance)\n",passed,failed);
  return failed ? 1 : 0;
}
