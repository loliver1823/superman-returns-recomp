// golden_call_harness: the RECOMP side of the golden-call differential (docs/TOOLKIT.md 1.3b).
// NOT part of the game build: built by build_harness.py into archive/golden_call/.
//
// Loads guest memory from a golden-dump blob (export format: [u32 va, u32 size, bytes] LE headers),
// fills a host dispatch table from PPCFuncMappings, and executes a small script of calls into the
// RECOMPILED guest code (SupermanReturnsGuest.lib). The oracle side (run_script.py, the clean-room
// PPC interpreter xenon_interp.py, same directory) runs the SAME script on the SAME memory and
// must print the SAME text.
//
// Script (one command per line, hex numbers without 0x):
//   CASE <name>                     start a case (echoed)
//   W <va> <hexbytes>               write bytes
//   CALL <va> [rN=<hex>] [fN=<hex64 bits>]   call guest function at va (via the dispatch table),
//                                   r1 = stack top, lr = sentinel; prints nothing
//   PR <rN>                         print "rN=<hex64>"  (after the last CALL)
//   PF <fN>                         print "fN=<hex64>"
//   PV <vN>                         print "vN=<32 hex, guest byte order>"  (CALL also takes vN=<32 hex>)
//   PCR <n>                         print "crN=<LT GT EQ SO as one hex digit>"
//   D <va> <len>                    print "D <va> <hex>"
//   LOG                             print the harness-collector records, then clear them
//   END                             print the list of changed 4 KiB pages outside the scratch
//                                   window (with an FNV-1a hash of each), restore memory
//   HEAP <lo> <hi>                  (iter-467) until END, the guest heap entry points hand out
//                                   blocks from a bump arena [lo, hi) in scratch, the same on
//                                   both sides: RtlAllocateHeap sub_8246CA80 (16-aligned, zeroed
//                                   with HEAP_ZERO_MEMORY), RtlFreeHeap sub_8246D368 (succeeds),
//                                   XMemAllocPhysical sub_82468008 (aligned to max(16, r5)),
//                                   XMemFreePhysical sub_824680A8 (no-op). The golden dump holds no
//                                   usable process heap (its LocalAlloc returns 0x8), so code that
//                                   allocates needs this to run alike on both sides.
//
// Harness collector (a guest object in scratch whose vtable slot 1 points at kVaCollect):
//   +0 vtable ptr, +4 earlyOutDistance (float), +8 mode (0 = keep all, 1 = closest: lower
//   earlyOut to each reported distance), +12 count (u32, maintained by the harness).
//   addCdPoint(this r3, hkCdPoint* r4): records pos[4], normalDist[4] (raw u32 bits) and the
//   (shape, shapeKey) chains of cdBodyA (+32) and cdBodyB (+36), following hkCdBody::m_parent (+12).
//   Every other vtable slot points at kVaTrap+4*i and records "VCALL i".
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "ppc/ppc_recomp_shared.h"
#include "kernel/guest_context.hpp"
#include "kernel/export_registry.hpp"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

static uint8_t* g_base;
static const uint64_t kSize = 0x100000000ull;
static const uint32_t kScratchLo = 0x70000000u, kScratchHi = 0x71000000u;
static const uint32_t kStackTop = 0x70F00000u;
static const uint32_t kPcr = 0x70E00000u;
static const uint32_t kVaCollect = 0x8290F500u;
static const uint32_t kVaTrap = 0x8290F504u;   // vtable slot i (i != 1) -> kVaTrap + 4*i

static uint32_t rbe32(uint32_t va) { uint32_t b; memcpy(&b, g_base + va, 4); return _byteswap_ulong(b); }
static void wbe32(uint32_t va, uint32_t v) { uint32_t b = _byteswap_ulong(v); memcpy(g_base + va, &b, 4); }

static std::vector<std::string> g_log;
static std::map<uint32_t, uint32_t> g_tls;   // harness TLS (the oracle's KeTlsGetValue hook mirrors it)
static uint32_t k_TlsGet(uint32_t r3, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
  auto it = g_tls.find(r3); return it == g_tls.end() ? 0u : it->second;
}
static uint32_t k_TlsSet(uint32_t r3, uint32_t r4, uint32_t, uint32_t, uint32_t, uint32_t) { g_tls[r3] = r4; return 1; }
static uint32_t k_Unexpected(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
  g_log.push_back("IMPORT-UNEXPECTED"); return 0;
}
// handle duplication (anim/sound paths in the actor tick): the duplicate IS the source handle,
// closing succeeds (the oracle mirrors this; run_script.py IMPORTS)
static uint32_t k_DupObj(uint32_t r3, uint32_t r4, uint32_t, uint32_t, uint32_t, uint32_t) {
  if (r4) wbe32(r4, r3);
  g_log.push_back("IMPORT NtDuplicateObject");
  return 0;
}
static uint32_t k_Close(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
  g_log.push_back("IMPORT NtClose");
  return 0;
}
// single-threaded harness: critical sections always succeed (the oracle mirrors this)
static uint32_t g_csCalls = 0;
static uint32_t k_CsEnter(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { ++g_csCalls; return 0; }
static uint32_t k_CsTry(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { ++g_csCalls; return 1; }
static std::vector<std::pair<uint32_t, std::vector<uint8_t>>> g_pristine;   // loaded ranges

// HEAP <lo> <hi> (see the header): the bump arena, active until END (g_heapHi == 0 = off).
static uint32_t g_heapCur = 0, g_heapHi = 0;
static uint32_t bumpAlloc(uint32_t size, uint32_t align, bool zero) {
  if (align < 16) align = 16;
  uint32_t p = (g_heapCur + align - 1) & ~(align - 1);
  if (p < g_heapCur || p > g_heapHi || size > g_heapHi - p) return 0;
  g_heapCur = p + ((size + 15u) & ~15u);
  if (zero) memset(g_base + p, 0, size);
  return p;
}
// RtlAllocateHeap(heap, flags, size). guest/ppc_import_glue.cpp strong-defines it too; the link
// keeps this one (build_harness.py: /force:multiple, harness object first). With no HEAP active it
// runs the game's allocator, which is what the glue's definition does outside forcing.
extern "C" void __imp__sub_8246CA80(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_8246CA80) {
  if (!g_heapHi) { __imp__sub_8246CA80(ctx, base); return; }
  ctx.r3.u64 = bumpAlloc(ctx.r5.u32, 16, (ctx.r4.u32 & 8) != 0);
}
extern "C" void __imp__sub_8246D368(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_8246D368) {   // RtlFreeHeap(heap, flags, block)
  if (!g_heapHi) { __imp__sub_8246D368(ctx, base); return; }
  ctx.r3.u64 = 1;
}
extern "C" void __imp__sub_82468008(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_82468008) {   // XMemAllocPhysical(size, address, alignment, protect)
  if (!g_heapHi) { __imp__sub_82468008(ctx, base); return; }
  const uint32_t al = ctx.r5.u32;
  ctx.r3.u64 = bumpAlloc(ctx.r3.u32, (al && al <= 0x10000u) ? al : 16, false);
}
extern "C" void __imp__sub_824680A8(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_824680A8) {   // XMemFreePhysical(block)
  if (!g_heapHi) { __imp__sub_824680A8(ctx, base); return; }
  ctx.r3.u64 = 0;
}

static std::string chain(uint32_t body) {
  std::string s;
  char buf[64];
  for (int d = 0; d < 8 && body; ++d) {
    snprintf(buf, sizeof buf, "%s%08X,%08X", d ? "/" : "", rbe32(body), rbe32(body + 4));
    s += buf;
    body = rbe32(body + 12);
  }
  return s;
}

static void collectFn(PPCContext& __restrict ctx, uint8_t* base) {
  (void)base;
  uint32_t self = ctx.r3.u32, pt = ctx.r4.u32;
  char buf[512];
  snprintf(buf, sizeof buf, "P c=%08X pos=%08X,%08X,%08X,%08X nd=%08X,%08X,%08X,%08X A=%s B=%s",
           self, rbe32(pt), rbe32(pt + 4), rbe32(pt + 8), rbe32(pt + 12),
           rbe32(pt + 16), rbe32(pt + 20), rbe32(pt + 24), rbe32(pt + 28),
           chain(rbe32(pt + 32)).c_str(), chain(rbe32(pt + 36)).c_str());
  g_log.push_back(buf);
  wbe32(self + 12, rbe32(self + 12) + 1);
  if (rbe32(self + 8) == 1) {
    uint32_t du = rbe32(pt + 28), eu = rbe32(self + 4);
    float d, e; memcpy(&d, &du, 4); memcpy(&e, &eu, 4);
    if (d < e) wbe32(self + 4, du);
  }
}

template <int I>
static void trapFn(PPCContext& __restrict ctx, uint8_t* base) {
  (void)base; (void)ctx;
  char buf[32]; snprintf(buf, sizeof buf, "VCALL %d", I);
  g_log.push_back(buf);
  ctx.r3.u64 = 0;
}

static bool loadBlob(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); return false; }
  uint32_t hdr[2];
  size_t tot = 0;
  while (fread(hdr, 4, 2, f) == 2) {
    std::vector<uint8_t> data(hdr[1]);
    if (fread(data.data(), 1, hdr[1], f) != hdr[1]) { fclose(f); return false; }
    memcpy(g_base + hdr[0], data.data(), hdr[1]);
    tot += hdr[1];
    g_pristine.emplace_back(hdr[0], std::move(data));
  }
  fclose(f);
  fprintf(stderr, "loaded %zu bytes of golden guest memory\n", tot);
  return true;
}

static const uint8_t* pristinePage(uint32_t va) {   // 4 KiB page content, nullptr = zero page
  for (auto& r : g_pristine)
    if (va >= r.first && va + 0x1000 <= r.first + r.second.size()) return r.second.data() + (va - r.first);
  return nullptr;
}

static uint64_t fnv(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}

static void endCase(FILE* out) {
  // changed pages via Windows write-watch, compared against pristine content
  static std::vector<PVOID> addrs(1 << 20);
  ULONG_PTR count = addrs.size();
  DWORD gran = 0;
  if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_base, kSize, addrs.data(), &count, &gran) != 0) {
    fprintf(stderr, "GetWriteWatch failed\n");
    count = 0;
  }
  std::vector<uint32_t> pages;
  for (ULONG_PTR i = 0; i < count; ++i) pages.push_back((uint32_t)((uint8_t*)addrs[i] - g_base));
  std::sort(pages.begin(), pages.end());
  static const uint8_t zero[0x1000] = {};
  for (uint32_t pg : pages) {
    const uint8_t* orig = pristinePage(pg);
    if (!orig) orig = zero;
    bool scratch = pg >= kScratchLo && pg < kScratchHi;
    if (memcmp(g_base + pg, orig, 0x1000) != 0) {
      if (!scratch) fprintf(out, "CHANGED %08X %016llX\n", pg, (unsigned long long)fnv(g_base + pg, 0x1000));
      memcpy(g_base + pg, orig, 0x1000);
    }
  }
  GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_base, kSize, addrs.data(), &count, &gran);   // clear our restores
}

static PPCRegister* gpr(PPCContext& c, int n) {
  PPCRegister* t[32] = {&c.r0, &c.r1, &c.r2, &c.r3, &c.r4, &c.r5, &c.r6, &c.r7, &c.r8, &c.r9, &c.r10,
                        &c.r11, &c.r12, &c.r13, &c.r14, &c.r15, &c.r16, &c.r17, &c.r18, &c.r19, &c.r20,
                        &c.r21, &c.r22, &c.r23, &c.r24, &c.r25, &c.r26, &c.r27, &c.r28, &c.r29, &c.r30, &c.r31};
  return t[n];
}
static PPCRegister* fpr(PPCContext& c, int n) {
  PPCRegister* t[14] = {&c.f0, &c.f1, &c.f2, &c.f3, &c.f4, &c.f5, &c.f6, &c.f7, &c.f8, &c.f9, &c.f10,
                        &c.f11, &c.f12, &c.f13};
  return n < 14 ? t[n] : nullptr;
}

int main(int argc, char** argv) {
  if (!getenv("SR_MODS")) _putenv_s("SR_MODS", "0");
  if (argc < 4) { fprintf(stderr, "usage: golden_call_harness blob script out\n"); return 2; }
  g_base = (uint8_t*)VirtualAlloc((LPVOID)0x2A00000000ULL, kSize, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
  if (!g_base) g_base = (uint8_t*)VirtualAlloc(nullptr, kSize, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
  if (!g_base) { fprintf(stderr, "alloc failed\n"); return 2; }
  sr::kernel::setGuestMemory(g_base, kSize);
  if (!loadBlob(argv[1])) return 2;

  // dispatch table
  const size_t n = sr::kernel::kDispatchTableEntryCount;
  PPCFunc** table = new PPCFunc*[n]();
  for (size_t i = 0; PPCFuncMappings[i].host != nullptr; ++i) {
    size_t slot = (PPCFuncMappings[i].guest - PPC_CODE_BASE) / 4;
    if (slot < n) table[slot] = PPCFuncMappings[i].host;
  }
  auto setSlot = [&](uint32_t va, PPCFunc* fn) { table[(va - PPC_CODE_BASE) / 4] = fn; };
  setSlot(kVaCollect, collectFn);
  setSlot(kVaTrap + 0, trapFn<0>); setSlot(kVaTrap + 8, trapFn<2>); setSlot(kVaTrap + 12, trapFn<3>);
  setSlot(kVaTrap + 16, trapFn<4>); setSlot(kVaTrap + 20, trapFn<5>); setSlot(kVaTrap + 24, trapFn<6>);
  setSlot(kVaTrap + 28, trapFn<7>);
  sr::kernel::g_dispatchTable = table;
  {
    auto& reg = sr::kernel::ExportRegistry::instance();
    reg.registerExport("KeTlsGetValue", k_TlsGet);
    reg.registerExport("KeTlsSetValue", k_TlsSet);
    reg.registerExport("RtlEnterCriticalSection", k_CsEnter);
    reg.registerExport("RtlLeaveCriticalSection", k_CsEnter);
    reg.registerExport("RtlTryEnterCriticalSection", k_CsTry);
    reg.registerExport("NtDuplicateObject", k_DupObj);
    reg.registerExport("NtClose", k_Close);
  }
  // clear write-watch state after loading
  { std::vector<PVOID> a(1 << 20); ULONG_PTR c = a.size(); DWORD g; GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_base, kSize, a.data(), &c, &g); }

  FILE* in = fopen(argv[2], "r");
  FILE* out = fopen(argv[3], "w");
  if (!in || !out) { fprintf(stderr, "io\n"); return 2; }
  static char line[1 << 20];
  PPCContext ctx;
  memset(&ctx, 0, sizeof ctx);
  int ncalls = 0;
  while (fgets(line, sizeof line, in)) {
    char* p = line;
    size_t L = strlen(p);
    while (L && (p[L - 1] == '\n' || p[L - 1] == '\r')) p[--L] = 0;
    if (!L || p[0] == '#') continue;
    char cmd[16] = {};
    sscanf(p, "%15s", cmd);
    std::string c = cmd;
    if (c == "CASE") {
      fprintf(out, "%s\n", p);
    } else if (c == "W") {
      uint32_t va; char hex[1 << 16];
      char* rest = p + 2;
      va = strtoul(rest, &rest, 16);
      while (*rest == ' ') ++rest;
      size_t hl = strlen(rest);
      for (size_t i = 0; i + 1 < hl; i += 2) {
        char b[3] = {rest[i], rest[i + 1], 0};
        g_base[va + i / 2] = (uint8_t)strtoul(b, nullptr, 16);
      }
      (void)hex;
    } else if (c == "CALL") {
      memset(&ctx, 0, sizeof ctx);
      ctx.r1.u64 = kStackTop;
      ctx.r13.u64 = kPcr;
      ctx.fpscr.csr = 0x1F80;
      ctx.fpscr.setcsr(ctx.fpscr.csr);
      ctx.lr = 0x7FFFFFF0u;
      char* rest = p + 5;
      uint32_t va = strtoul(rest, &rest, 16);
      while (*rest) {
        while (*rest == ' ') ++rest;
        if (!*rest) break;
        char kind = rest[0];
        int idx = strtol(rest + 1, &rest, 10);
        if (*rest == '=') ++rest;
        if (kind == 'v') {   // vN=<32 hex>: guest big-endian bytes; the recomp keeps VRs byte-reversed
          char* e = rest;
          while (isxdigit((unsigned char)*e)) ++e;
          size_t n = (size_t)(e - rest);
          uint8_t be[16] = {};
          for (size_t i = 0; i < n && i < 32; ++i) {
            size_t pos = 32 - n + i;   // right-align short values
            unsigned d = isdigit((unsigned char)rest[i]) ? rest[i] - '0' : (tolower(rest[i]) - 'a' + 10);
            be[pos / 2] |= (uint8_t)(d << ((pos & 1) ? 0 : 4));
          }
          for (int i = 0; i < 16; ++i) (&ctx.v0)[idx].u8[15 - i] = be[i];
          rest = e;
          continue;
        }
        uint64_t v = _strtoui64(rest, &rest, 16);
        if (kind == 'r') gpr(ctx, idx)->u64 = v;
        else if (kind == 'f') fpr(ctx, idx)->u64 = v;
      }
      PPCFunc* fn = table[(va - PPC_CODE_BASE) / 4];
      if (!fn) { fprintf(out, "NOFUNC %08X\n", va); continue; }
      fn(ctx, g_base);
      ++ncalls;
    } else if (c == "HEAP") {  // HEAP <lo> <hi>: the bump arena until END
      sscanf(p + 5, "%x %x", &g_heapCur, &g_heapHi);
    } else if (c == "TLS") {   // TLS <index> <value>: KeTlsSetValue through the kernel import
      uint32_t idx, val;
      sscanf(p + 4, "%x %x", &idx, &val);
      g_tls[idx] = val;
    } else if (c == "CS") {    // print and reset the critical-section call count
      fprintf(out, "CS %u\n", g_csCalls); g_csCalls = 0;
    } else if (c == "PR") {
      int idx = atoi(p + 4);
      fprintf(out, "r%d=%016llX\n", idx, (unsigned long long)gpr(ctx, idx)->u64);
    } else if (c == "PV") {   // PV vN: the 16 guest (big-endian) bytes
      int idx = atoi(p + 4);
      fprintf(out, "v%d=", idx);
      for (int i = 0; i < 16; ++i) fprintf(out, "%02X", (&ctx.v0)[idx].u8[15 - i]);
      fprintf(out, "\n");
    } else if (c == "PCR") {  // PCR <field>: LT GT EQ SO of one CR field as one hex digit
      int idx = atoi(p + 4);
      const PPCCRRegister& f = (&ctx.cr0)[idx];
      fprintf(out, "cr%d=%X\n", idx, (f.lt << 3) | (f.gt << 2) | (f.eq << 1) | f.so);
    } else if (c == "PF") {
      int idx = atoi(p + 4);
      fprintf(out, "f%d=%016llX\n", idx, (unsigned long long)fpr(ctx, idx)->u64);
    } else if (c == "D") {
      uint32_t va, len;
      sscanf(p + 2, "%x %x", &va, &len);
      fprintf(out, "D %08X ", va);
      for (uint32_t i = 0; i < len; ++i) fprintf(out, "%02X", g_base[va + i]);
      fprintf(out, "\n");
    } else if (c == "DARR") {   // hkArray dump: DARR <va> <elemsize hex>
      uint32_t va, es;
      sscanf(p + 5, "%x %x", &va, &es);
      uint32_t data = rbe32(va), size = rbe32(va + 4);
      fprintf(out, "DARR %08X n=%u ", va, size);
      if (size > 65536) size = 65536;
      for (uint32_t i = 0; i < size * es; ++i) fprintf(out, "%02X", g_base[data + i]);
      fprintf(out, "\n");
    } else if (c == "LOG") {
      for (auto& s : g_log) fprintf(out, "%s\n", s.c_str());
      g_log.clear();
    } else if (c == "END") {
      endCase(out);
      g_log.clear();
      g_tls.clear();
      g_heapCur = g_heapHi = 0;
    }
  }
  fclose(out);
  fprintf(stderr, "calls=%d\n", ncalls);
  return 0;
}
