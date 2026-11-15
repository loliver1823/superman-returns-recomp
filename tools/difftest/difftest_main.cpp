// difftest_main: L6 differential CPU-tester — recomp-side execution half.
//
// Runs ONE recompiled guest PowerPC function (a Tier-0 "pure leaf": no calls, no
// memory, no spills) in isolation inside our XenonRecomp lowering, then prints the
// resulting register deltas in the canonical golden format (docs/DIFFTEST_DESIGN.md §4).
// scripts/difftest/compare.py diffs this against the Ghidra/hand "golden" oracle so a
// codegen divergence (shift/rotate/sign/mask bug) is caught at the function that
// produced it — not 200 ms later when it crashes.
//
// Build: standalone `difftest` CMake target (see CMakeLists.txt). It links the SAME
// recomp static library (SupermanReturnsGuest) and the SAME PPC_CONFIG_* compile
// defines as the game, so the PPCContext layout is bit-identical. Tier-0 leaves need
// only valid guest memory + a guest base, so this TU provides a tiny self-contained
// kernel shim (setGuestMemory/getGuestBase + no-op store watchpoints) instead of
// dragging in the gfx/SDL harness. The leaf functions never store or call, so the
// shimmed watchpoint symbols are linked-but-never-invoked.
//
//   difftest_main <guest_va_hex> [rN=value ...]        # human form, prints JSON
//   difftest_main --case '<json>'                       # one case object
//   difftest_main --cases <file.cases.json> [--json]    # all cases in a file
//   difftest_main --self-test                           # built-in 3-fn sanity check
//
// NOT part of the game harness.

#define _CRT_SECURE_NO_WARNINGS  // fopen/strtoull etc. in this standalone tool

#if defined(_WIN32)
#include <windows.h>  // VirtualAlloc for the flat guest-memory base
#endif

#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "ppc/ppc_recomp_shared.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>

// ─────────────────────────────────────────────────────────────────────────────
// Guest memory + the sr::kernel symbols that ppc_context.h's load/store/lookup
// macros reference are provided by the REAL kernel/* objects we link (same as the
// game), so the PPCContext layout AND the memory semantics are identical. A Tier-0
// pure leaf executes none of the store/load/call macros, so we only actually drive
// setGuestMemory/getGuestBase here — but linking the real kernel resolves every
// symbol the recomp objects pull in (import glue, watchpoints, etc.).
// ─────────────────────────────────────────────────────────────────────────────
#include "kernel/guest_context.hpp"
#include "kernel/vpad_filter.hpp"   // iter-466 --vpad-filter-test

// ─────────────────────────────────────────────────────────────────────────────
// Function table: VA -> recomp entrypoint. Tier-0 calls the symbol DIRECTLY (no
// dispatch table / PPCFuncMappings), so the static lib only pulls the leaf members
// plus their (none) callees — keeping the link small and harness-free.
// Extend this list to test more functions.
// ─────────────────────────────────────────────────────────────────────────────
struct DiffEntry { uint32_t va; PPCFunc* fn; const char* name; };
static const DiffEntry kFns[] = {
  // ── Validated pure leaves (Tier-0): no calls, no memory. Safe to trust. ──
  { 0x822949D8u, sub_822949D8, "sub_822949D8" },  // 1 if r5==r4 (cntlzw+rotate)
  { 0x826690D0u, sub_826690D0, "sub_826690D0" },  // align_up(r3*8,32)+r3*8
  { 0x82670774u, sub_82670774, "sub_82670774" },  // r3*r4*3
  { 0x82294C20u, sub_82294C20, "sub_82294C20" },  // 1 if r5==r4 (sibling of 949D8)
  { 0x82294C34u, sub_82294C34, "sub_82294C34" },  // 1 if (u32)r4>=(u32)r5 (subfc/subfe)
  { 0x822BCC78u, sub_822BCC78, "sub_822BCC78" },  // (r4+1) if (u32)(r4+1)<6 else -1
  { 0x82644698u, sub_82644698, "sub_82644698" },  // (r4-4) if (int)r4!=0 else 0
  { 0x82686E50u, sub_82686E50, "sub_82686E50" },  // (r3-r4) if (u32)r3!=0 else 0
  { 0x82686E68u, sub_82686E68, "sub_82686E68" },  // (r3+r4) if (u32)r3!=0 else 0
  // ── Validated pure STORES (Tier-1): no calls, read-modify-write one guest field. ──
  // These touch guest memory, so the case must seed input `mem` and `watch` the output
  // region; the delta is reported in deltas.mem (big-endian, as stored in guest memory).
  { 0x8213CA30u, sub_8213CA30, "sub_8213CA30" },  // *(u32)(r3+0)    += 1
  { 0x82165F80u, sub_82165F80, "sub_82165F80" },  // *(u32)(r3+1480) -= r4
  // ── Render-sweep branch probe: exits before the first non-leaf call. ──
  { 0x82776EB0u, sub_82776EB0, "sub_82776EB0" },  // not-ready slot -> 0
  // ── XMV bitreader refill paths: finite loop and empty non-callback exit. ──
  { 0x824C4428u, sub_824C4428, "sub_824C4428" },
  // ── NON-pure NEGATIVE case (intentionally unsafe): mtctr r4; bctr. ──
  // Reaching it dispatches through sr::kernel::g_dispatchTable, which the difftest
  // world leaves null (only the game's boot populates it). Calling it with an
  // in-range r4 dereferences the null table → access violation. Proves the SEH
  // guard reports "faulted" cleanly instead of crashing difftest.exe.
  { 0x8279E494u, sub_8279E494, "sub_8279E494" },  // NON-LEAF: indirect tail-call (bctr)
  // ── iter-458: luaH_mainposition numeric-key path - r19 preservation regression ──
  // (tools/test_lua_hashnum_r19.py). R1 deleted the frozen pre-fix copy of the old splice
  // (pre458_luahash_baseline.cpp): it was translated game code, which the public repo
  // never carries. Its mutation check ran green in iter-458.
  { 0x82767688u, sub_82767688, "sub_82767688" },
};
static const DiffEntry* lookup(uint32_t va) {
  for (const DiffEntry& e : kFns) if (e.va == va) return &e;
  return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// SEH SAFETY NET (PART 1).  A non-pure guest function reached by difftest will
// dereference unmapped guest memory or the (null in this world) dispatch table and
// raise a structured exception. Without a guard that kills difftest.exe with no
// output. We isolate the guest call in an `extern "C"` helper that contains ONLY the
// __try/__except and NO C++ objects with destructors — clang on x86_64-pc-windows-msvc
// supports SEH there (a C++ object needing unwind in the same frame as __try is the
// one thing it rejects, so we keep this frame POD-only). The filter records the
// exception code + faulting instruction address; the handler returns cleanly so the
// caller can emit a JSON "faulted" verdict instead of crashing.
// ─────────────────────────────────────────────────────────────────────────────
#if defined(_WIN32)
struct FaultInfo { uint32_t code; uint64_t at; };

static int sehFilter(unsigned int code, struct _EXCEPTION_POINTERS* ep, FaultInfo* out) {
  out->code = (uint32_t)code;
  out->at = ep ? (uint64_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress : 0;
  return EXCEPTION_EXECUTE_HANDLER;
}

// Returns 0 if the guest function ran to completion, 1 if it raised a structured
// exception (out filled in). POD-only frame: no destructors, so SEH is legal here.
extern "C" int callGuarded(PPCFunc* fn, PPCContext* ctx, uint8_t* base, FaultInfo* out) {
  __try {
    fn(*ctx, base);
    return 0;
  } __except (sehFilter(GetExceptionCode(), GetExceptionInformation(), out)) {
    return 1;
  }
}
#else
struct FaultInfo { uint32_t code; uint64_t at; };
extern "C" int callGuarded(PPCFunc* fn, PPCContext* ctx, uint8_t* base, FaultInfo*) {
  fn(*ctx, base); return 0;  // no SEH off-Windows; pure leaves don't fault anyway
}
#endif

// ─── GPR name → PPCContext field ────────────────────────────────────────────
// Only the registers a Tier-0 leaf can read/write (r0..r12 args/scratch). The
// PPCContext stores GPRs host-native (not byte-swapped), so we set/read u64 directly.
static PPCRegister* gpr(PPCContext& ctx, const std::string& n) {
  if (n == "r0")  return &ctx.r0;   if (n == "r1")  return &ctx.r1;
  if (n == "r2")  return &ctx.r2;   if (n == "r3")  return &ctx.r3;
  if (n == "r4")  return &ctx.r4;   if (n == "r5")  return &ctx.r5;
  if (n == "r6")  return &ctx.r6;   if (n == "r7")  return &ctx.r7;
  if (n == "r8")  return &ctx.r8;   if (n == "r9")  return &ctx.r9;
  if (n == "r10") return &ctx.r10;  if (n == "r11") return &ctx.r11;
  if (n == "r12") return &ctx.r12;  if (n == "r13") return &ctx.r13;
  // iter-458: non-volatiles, so a case can seed them and assert preservation.
  if (n == "r14") return &ctx.r14;  if (n == "r15") return &ctx.r15;
  if (n == "r16") return &ctx.r16;  if (n == "r17") return &ctx.r17;
  if (n == "r18") return &ctx.r18;  if (n == "r19") return &ctx.r19;
  if (n == "r20") return &ctx.r20;  if (n == "r21") return &ctx.r21;
  if (n == "r22") return &ctx.r22;  if (n == "r23") return &ctx.r23;
  if (n == "r24") return &ctx.r24;  if (n == "r25") return &ctx.r25;
  if (n == "r26") return &ctx.r26;  if (n == "r27") return &ctx.r27;
  if (n == "r28") return &ctx.r28;  if (n == "r29") return &ctx.r29;
  if (n == "r30") return &ctx.r30;  if (n == "r31") return &ctx.r31;
  return nullptr;
}

static const char* kGprNames[] = {
  "r0","r1","r2","r3","r4","r5","r6","r7","r8","r9","r10","r11","r12","r13"
};

static uint64_t parseHex(const std::string& s) {
  return (uint64_t)strtoull(s.c_str(), nullptr, 0);  // 0x.. or decimal
}

// One input case: id + input register map (name -> value).
//   mem   — Tier-1 INPUT seed: guest VA -> raw bytes (big-endian, as stored in guest
//           memory). Written verbatim into the aliased guest region BEFORE the call.
//   watch — Tier-1 OUTPUT capture regions: {VA,size} snapshotted before+after the call;
//           changed bytes become deltas.mem. Empty -> register-only (Tier-0) behavior.
struct MemSeed { uint32_t va; std::vector<uint8_t> bytes; };
struct WatchRegion { uint32_t va; uint32_t size; };
struct Case {
  std::string id;
  uint32_t va = 0;
  std::map<std::string, uint64_t> regs;
  std::vector<MemSeed> mem;        // Tier-1 input memory seed
  std::vector<WatchRegion> watch;  // Tier-1 output regions to diff
  // iter-458: registers the callee must PRESERVE, compared full-width (u64) before/after.
  // The r3/r4 gate cannot see a clobbered non-volatile (the luaH_mainposition r19 defect).
  std::vector<std::string> preserve;
};

// Parse a hex-bytes string ("00000011" / "0x00000011") into raw bytes (verbatim,
// big-endian as written in the guest image). An odd nibble count drops the trailing one.
static std::vector<uint8_t> parseHexBytes(const std::string& in) {
  std::string s = in;
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
  std::vector<uint8_t> out;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    int hi = nib(s[i]), lo = nib(s[i + 1]);
    if (hi < 0 || lo < 0) break;
    out.push_back((uint8_t)((hi << 4) | lo));
  }
  return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// PURITY PRE-CHECK (PART 1, the heads-up).  Before running, scan the recomp source
// for the target sub_XXXX and classify what it touches:
//   "leaf"   — no calls, no stores, no spills → the delta is trustworthy.
//   "stores" — writes guest memory (PPC_STORE_U*) → Tier-1, delta may be incomplete.
//   "calls"  — calls another fn / spills (PPC_CALL*, sub_(ctx, direct call, save/rest
//              thunks, indirect bctr) → NOT a pure leaf; the SEH guard is the only
//              thing standing between you and a crash. Distrust the delta.
//   "unknown"— source not found / could not classify.
// This is advisory only (the SEH guard is the actual safety net), so it never fails;
// it just tells the user whether to trust the number BEFORE they read it.
// ─────────────────────────────────────────────────────────────────────────────
static std::string fileSlurp(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return "";
  std::string s; char buf[8192]; size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}

// Locate the repo's guest/ppc dir by walking up from a few candidate roots. The
// difftest exe lives at <repo>/build/difftest.exe, so ../guest/ppc and ./guest/ppc
// (when run from repo root) both work; we also accept the SR_DIFFTEST_RECOMP_DIR env.
static std::string recompDir() {
  if (const char* env = getenv("SR_DIFFTEST_RECOMP_DIR")) {
    if (env[0]) return env;
  }
  const char* cands[] = {
    "guest/ppc",            // run from repo root
    "../guest/ppc",         // run from build/
    "../../guest/ppc",      // run from build/sub/
  };
  for (const char* c : cands) {
    std::string probe = std::string(c) + "/ppc_recomp.0.cpp";
    FILE* f = fopen(probe.c_str(), "rb");
    if (f) { fclose(f); return c; }
  }
  return "";
}

// Extract the body text of PPC_FUNC_IMPL(__imp__sub_XXXX) from a TU's text.
static std::string extractBody(const std::string& text, const std::string& fnName) {
  std::string marker = "PPC_FUNC_IMPL(__imp__" + fnName + ")";
  size_t start = text.find(marker);
  if (start == std::string::npos) return "";
  // Body runs until the next PPC_FUNC_IMPL( or end of file.
  size_t next = text.find("PPC_FUNC_IMPL(", start + marker.size());
  return text.substr(start, (next == std::string::npos ? text.size() : next) - start);
}

// Returns "leaf" | "stores" | "calls" | "unknown".
static const char* classifyPurity(uint32_t va, const std::string& fnName) {
  std::string dir = recompDir();
  if (dir.empty()) return "unknown";
  // The recomp is split across ppc_recomp.0.cpp .. ppc_recomp.N.cpp; we don't know
  // which TU holds this VA, so scan in order until we find the symbol. (Cheap: each
  // file is read once; we stop at the first hit.)
  for (int i = 0; i < 256; ++i) {
    std::string path = dir + "/ppc_recomp." + std::to_string(i) + ".cpp";
    std::string text = fileSlurp(path);
    if (text.empty()) {
      if (i == 0) return "unknown";   // dir wrong / no recomp at all
      continue;                        // gap in numbering; keep scanning a bit
    }
    std::string body = extractBody(text, fnName);
    if (body.empty()) continue;
    // Disqualifiers, most-severe first (mirrors scan_candidates.py classify()).
    bool hasCall = (body.find("PPC_CALL_INDIRECT_FUNC") != std::string::npos)
                || (body.find("PPC_CALL_FUNC") != std::string::npos);
    // direct call sub_XXXX(ctx — exclude our own header marker line.
    if (!hasCall) {
      size_t p = 0;
      while ((p = body.find("sub_", p)) != std::string::npos) {
        size_t paren = body.find('(', p);
        if (paren != std::string::npos && body.compare(paren, 5, "(ctx") == 0) { hasCall = true; break; }
        p += 4;
      }
    }
    const char* spills[] = {"__savegprlr","__restgprlr","savefpr","restfpr","savevmx","restvmx"};
    for (const char* s : spills) if (body.find(s) != std::string::npos) { hasCall = true; break; }
    if (hasCall) return "calls";
    bool hasStore = body.find("PPC_STORE_U") != std::string::npos
                 || body.find("PPC_MM_STORE") != std::string::npos;
    if (hasStore) return "stores";
    return "leaf";
  }
  (void)va;
  return "unknown";
}

// Run a single case, print one golden-format JSON object (oracle="recomp").
// Deltas = GPRs whose 32-bit value changed vs the zeroed-then-seeded input.
static int runCase(uint8_t* base, const Case& c) {
  const DiffEntry* e = lookup(c.va);
  if (!e) { fprintf(stderr, "difftest: no mapping for 0x%08X\n", c.va); return 2; }

  PPCContext ctx;
  std::memset(&ctx, 0, sizeof(ctx));
  ctx.r1.u32 = 0x70000000u;   // sane stack slot (unused by pure leaves)
  ctx.fpscr.csr = 0x1F80;     // exceptions masked, matches harness

  for (const auto& kv : c.regs) {
    PPCRegister* r = gpr(ctx, kv.first);
    if (r) r->u64 = kv.second;
    else fprintf(stderr, "difftest: [warn] case %s: unknown input reg %s\n",
                 c.id.c_str(), kv.first.c_str());
  }

  // Heads-up advisory: is this actually a pure leaf? (Source scan; never fails.)
  const char* purity = classifyPurity(c.va, e->name);

  // ── Tier-1 INPUT memory seed ──────────────────────────────────────────────
  // Write each case's `mem` bytes verbatim (big-endian, as stored in guest memory)
  // into the SAME aliased guest region the recomp's PPC_STORE/LOAD macros use. We
  // resolve via sr::kernel::guestVAToHost (identical aliasing) so the seed lands at
  // the exact host bytes the guest function will read. Out-of-range VAs are skipped
  // with a warning (the whole 4 GB is committed, so this only trips on a typo'd VA).
  for (const MemSeed& s : c.mem) {
    uint8_t* host = sr::kernel::guestVAToHost(s.va);
    if (!host) {
      fprintf(stderr, "difftest: [warn] case %s: seed VA 0x%08X out of guest range\n",
              c.id.c_str(), s.va);
      continue;
    }
    std::memcpy(host, s.bytes.data(), s.bytes.size());
  }

  // ── Tier-1 OUTPUT capture: snapshot watched regions BEFORE the call ────────
  // before[i] holds a copy of each watch region's bytes; after the call we re-read
  // and emit only the bytes that changed as deltas.mem.
  std::vector<std::vector<uint8_t>> watchBefore(c.watch.size());
  for (size_t i = 0; i < c.watch.size(); ++i) {
    uint8_t* host = sr::kernel::guestVAToHost(c.watch[i].va);
    watchBefore[i].resize(c.watch[i].size);
    if (host) std::memcpy(watchBefore[i].data(), host, c.watch[i].size);
  }

  // Snapshot the 32-bit GPR values before the call.
  uint32_t before[14];
  for (int i = 0; i < 14; ++i) before[i] = gpr(ctx, kGprNames[i])->u32;
  std::vector<uint64_t> preBefore;
  for (const std::string& n : c.preserve) {
    PPCRegister* r = gpr(ctx, n);
    preBefore.push_back(r ? r->u64 : 0);
  }

  // SEH-guarded call: a non-pure function faults here instead of killing the process.
  FaultInfo fi{0, 0};
  int faulted = callGuarded(e->fn, &ctx, base, &fi);

  if (faulted) {
    // Clean "faulted" verdict — never a delta we'd mistake for a real result.
    printf("{\"guest_va\":\"0x%08X\",\"fn\":\"%s\",\"case_id\":\"%s\",", c.va, e->name, c.id.c_str());
    printf("\"oracle\":\"recomp\",\"purity\":\"%s\",\"status\":\"faulted\",", purity);
    printf("\"exception\":\"0x%08X\",\"at\":\"0x%016llX\"}\n",
           fi.code, (unsigned long long)fi.at);
    return 0;  // a clean verdict, not a harness error — compare.py treats it as non-gating
  }

  // ── Tier-1 OUTPUT capture: re-read watched regions AFTER the call ──────────
  // Compare against watchBefore; emit each maximal run of changed bytes as one
  // deltas.mem entry { "<VA hex>": "<hex bytes, big-endian as in guest memory>" }.
  // Only changed bytes are reported (a region the fn didn't touch yields nothing),
  // so a clean register-only function still produces deltas.mem == {}.
  std::string memJson;
  {
    bool firstMem = true;
    char vabuf[16];
    for (size_t i = 0; i < c.watch.size(); ++i) {
      uint8_t* host = sr::kernel::guestVAToHost(c.watch[i].va);
      if (!host) continue;
      const std::vector<uint8_t>& bef = watchBefore[i];
      uint32_t sz = c.watch[i].size;
      uint32_t j = 0;
      while (j < sz) {
        if (host[j] == bef[j]) { j++; continue; }
        uint32_t runStart = j;
        while (j < sz && host[j] != bef[j]) j++;   // extend over the changed run
        snprintf(vabuf, sizeof(vabuf), "0x%08X", c.watch[i].va + runStart);
        if (!firstMem) memJson += ",";
        memJson += "\"";
        memJson += vabuf;
        memJson += "\":\"";
        char hb[4];
        for (uint32_t k = runStart; k < j; ++k) {
          snprintf(hb, sizeof(hb), "%02x", host[k]);
          memJson += hb;
        }
        memJson += "\"";
        firstMem = false;
      }
    }
  }

  // Emit canonical golden format (docs/DIFFTEST_DESIGN.md §4), oracle="recomp".
  printf("{\"guest_va\":\"0x%08X\",\"fn\":\"%s\",\"case_id\":\"%s\",", c.va, e->name, c.id.c_str());
  printf("\"oracle\":\"recomp\",\"purity\":\"%s\",\"status\":\"ok\",\"deltas\":{\"regs\":{", purity);
  bool first = true;
  for (int i = 0; i < 14; ++i) {
    uint32_t after = gpr(ctx, kGprNames[i])->u32;
    // r3/r4 are the ABI output set — always emit r3 even if unchanged (== input);
    // any other GPR is emitted only if it changed (volatile scratch is informational).
    bool isOutput = (i == 3 /*r3*/ || i == 4 /*r4*/);
    if (after != before[i] || (i == 3)) {
      if (!first) printf(",");
      printf("\"%s\":\"0x%08X\"", kGprNames[i], after);
      first = false;
    }
    (void)isOutput;
  }
  printf("},\"mem\":{%s}}", memJson.c_str());
  // iter-458: full-width preservation verdict per requested register.
  if (!c.preserve.empty()) {
    printf(",\"preserve\":{");
    for (size_t i = 0; i < c.preserve.size(); ++i) {
      PPCRegister* r = gpr(ctx, c.preserve[i]);
      const uint64_t after = r ? r->u64 : 0;
      printf("%s\"%s\":{\"before\":\"0x%016llX\",\"after\":\"0x%016llX\",\"ok\":%s}",
             i ? "," : "", c.preserve[i].c_str(), (unsigned long long)preBefore[i],
             (unsigned long long)after, (r && after == preBefore[i]) ? "true" : "false");
    }
    printf("}");
  }
  printf("}\n");
  return 0;
}

// ─── tiny JSON-ish case-file reader (stdlib only) ────────────────────────────
// The cases files are simple flat objects: {"case_id":"..","va":"0x..","regs":{...}}.
// We don't need a full JSON parser; extract the fields we use with light scanning.
static std::string jsonStr(const std::string& obj, const std::string& key) {
  std::string pat = "\"" + key + "\"";
  size_t k = obj.find(pat);
  if (k == std::string::npos) return "";
  size_t colon = obj.find(':', k + pat.size());
  if (colon == std::string::npos) return "";
  size_t q1 = obj.find('"', colon);
  if (q1 == std::string::npos) return "";
  size_t q2 = obj.find('"', q1 + 1);
  if (q2 == std::string::npos) return "";
  return obj.substr(q1 + 1, q2 - q1 - 1);
}

// Parse the "regs":{...} sub-object into name->value pairs.
static void parseRegs(const std::string& obj, std::map<std::string, uint64_t>& out) {
  size_t k = obj.find("\"regs\"");
  if (k == std::string::npos) return;
  size_t brace = obj.find('{', k);
  if (brace == std::string::npos) return;
  size_t end = obj.find('}', brace);
  if (end == std::string::npos) return;
  std::string body = obj.substr(brace + 1, end - brace - 1);
  size_t pos = 0;
  while (pos < body.size()) {
    size_t q1 = body.find('"', pos);
    if (q1 == std::string::npos) break;
    size_t q2 = body.find('"', q1 + 1);
    if (q2 == std::string::npos) break;
    std::string name = body.substr(q1 + 1, q2 - q1 - 1);
    size_t colon = body.find(':', q2);
    if (colon == std::string::npos) break;
    size_t v1 = body.find('"', colon);
    size_t v2 = (v1 == std::string::npos) ? std::string::npos : body.find('"', v1 + 1);
    if (v1 == std::string::npos || v2 == std::string::npos) break;
    std::string val = body.substr(v1 + 1, v2 - v1 - 1);
    out[name] = parseHex(val);
    pos = v2 + 1;
  }
}

// Split a cases-file (a top-level JSON array of flat objects) into object strings.
static std::vector<std::string> splitObjects(const std::string& text) {
  std::vector<std::string> objs;
  int depth = 0;
  size_t start = std::string::npos;
  bool inStr = false;
  for (size_t i = 0; i < text.size(); ++i) {
    char ch = text[i];
    if (inStr) { if (ch == '"' && text[i-1] != '\\') inStr = false; continue; }
    if (ch == '"') { inStr = true; continue; }
    if (ch == '{') { if (depth == 0) start = i; depth++; }
    else if (ch == '}') { depth--; if (depth == 0 && start != std::string::npos) objs.push_back(text.substr(start, i - start + 1)); }
  }
  return objs;
}

// Parse the "mem":{...} input-seed sub-object: {"<VA hex>":"<hex bytes>", ...}.
// Same shape as "regs" but values are byte strings, not scalars.
static void parseMem(const std::string& obj, std::vector<MemSeed>& out) {
  size_t k = obj.find("\"mem\"");
  if (k == std::string::npos) return;
  size_t brace = obj.find('{', k);
  if (brace == std::string::npos) return;
  // Match the closing brace of THIS sub-object (mem values contain no braces).
  size_t end = obj.find('}', brace);
  if (end == std::string::npos) return;
  std::string body = obj.substr(brace + 1, end - brace - 1);
  size_t pos = 0;
  while (pos < body.size()) {
    size_t q1 = body.find('"', pos);
    if (q1 == std::string::npos) break;
    size_t q2 = body.find('"', q1 + 1);
    if (q2 == std::string::npos) break;
    std::string va = body.substr(q1 + 1, q2 - q1 - 1);
    size_t colon = body.find(':', q2);
    if (colon == std::string::npos) break;
    size_t v1 = body.find('"', colon);
    size_t v2 = (v1 == std::string::npos) ? std::string::npos : body.find('"', v1 + 1);
    if (v1 == std::string::npos || v2 == std::string::npos) break;
    std::string bytes = body.substr(v1 + 1, v2 - v1 - 1);
    MemSeed s;
    s.va = (uint32_t)parseHex(va);
    s.bytes = parseHexBytes(bytes);
    if (!s.bytes.empty()) out.push_back(s);
    pos = v2 + 1;
  }
}

// Parse the "watch":[{"addr":"<VA>","size":N}, ...] output-region list.
static void parseWatch(const std::string& obj, std::vector<WatchRegion>& out) {
  size_t k = obj.find("\"watch\"");
  if (k == std::string::npos) return;
  size_t bracket = obj.find('[', k);
  if (bracket == std::string::npos) return;
  size_t endb = obj.find(']', bracket);
  if (endb == std::string::npos) return;
  std::string body = obj.substr(bracket + 1, endb - bracket - 1);
  // Each element is a small flat object {"addr":"..","size":N}; scan them in turn.
  size_t pos = 0;
  while (true) {
    size_t ob = body.find('{', pos);
    if (ob == std::string::npos) break;
    size_t cb = body.find('}', ob);
    if (cb == std::string::npos) break;
    std::string elem = body.substr(ob, cb - ob + 1);
    std::string addr = jsonStr(elem, "addr");
    if (addr.empty()) addr = jsonStr(elem, "va");
    // size may be a bare number, not a string — pull it out manually.
    uint32_t size = 0;
    size_t sk = elem.find("\"size\"");
    if (sk != std::string::npos) {
      size_t colon = elem.find(':', sk);
      if (colon != std::string::npos) {
        size_t p = colon + 1;
        while (p < elem.size() && (elem[p] == ' ' || elem[p] == '"')) p++;
        size = (uint32_t)strtoul(elem.c_str() + p, nullptr, 0);
      }
    }
    if (!addr.empty() && size > 0) {
      WatchRegion w;
      w.va = (uint32_t)parseHex(addr);
      w.size = size;
      out.push_back(w);
    }
    pos = cb + 1;
  }
}

static Case caseFromObj(const std::string& obj) {
  Case c;
  c.id = jsonStr(obj, "case_id");
  std::string va = jsonStr(obj, "va");
  c.va = va.empty() ? 0u : (uint32_t)parseHex(va);
  if (c.id.empty()) c.id = va;
  parseRegs(obj, c.regs);
  parseMem(obj, c.mem);
  parseWatch(obj, c.watch);
  // "preserve":["r19","r22"] - flat string array, no nesting.
  size_t pk = obj.find("\"preserve\"");
  if (pk != std::string::npos) {
    size_t lb = obj.find('[', pk), rb = obj.find(']', pk);
    if (lb != std::string::npos && rb != std::string::npos && lb < rb) {
      std::string arr = obj.substr(lb + 1, rb - lb - 1);
      size_t p = 0;
      while ((p = arr.find('"', p)) != std::string::npos) {
        size_t q = arr.find('"', p + 1);
        if (q == std::string::npos) break;
        c.preserve.push_back(arr.substr(p + 1, q - p - 1));
        p = q + 1;
      }
    }
  }
  return c;
}

static std::string slurp(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "difftest: cannot open %s\n", path); return ""; }
  std::string s; char buf[4096]; size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}

static int selfTest(uint8_t* base) {
  // Sanity: each pure-leaf canary on a known input, hand-derived expectations.
  struct { uint32_t va; uint64_t r3, r4, r5; uint32_t expect; const char* note; } t[] = {
    { 0x82670774u, 7, 5, 0, 0x69,      "r3*r4*3 = 7*5*3 = 105" },
    { 0x826690D0u, 4, 0, 0, 0x58,      "align_up(r3*8,32)+r3*8 for r3=4" },
    { 0x822949D8u, 0, 5, 5, 0x1,       "1 if r5==r4 (r4=r5=5)" },
    { 0x82294C20u, 0, 5, 5, 0x1,       "1 if r5==r4 (sibling)" },
    { 0x82294C34u, 0, 7, 5, 0x1,       "1 if u32(r4)>=u32(r5) (7>=5)" },
    { 0x822BCC78u, 0, 4, 0, 0x5,       "(r4+1) if <6 else -1 (=5)" },
    { 0x82644698u, 0, 0x10, 0, 0xC,    "(r4-4) if r4!=0 (=0xC)" },
    { 0x82686E50u, 0xA, 3, 0, 0x7,     "(r3-r4) if r3!=0 (=7)" },
    { 0x82686E68u, 0xA, 3, 0, 0xD,     "(r3+r4) if r3!=0 (=0xD)" },
  };
  int fails = 0;
  for (auto& x : t) {
    PPCContext ctx; std::memset(&ctx, 0, sizeof(ctx));
    ctx.r1.u32 = 0x70000000u; ctx.fpscr.csr = 0x1F80;
    ctx.r3.u64 = x.r3; ctx.r4.u64 = x.r4; ctx.r5.u64 = x.r5;
    FaultInfo fi{0, 0};
    int faulted = callGuarded(lookup(x.va)->fn, &ctx, base, &fi);
    bool ok = !faulted && ctx.r3.u32 == x.expect;
    if (faulted)
      printf("[FAIL] 0x%08X -> FAULTED 0x%08X  %s\n", x.va, fi.code, x.note);
    else
      printf("[%s] 0x%08X -> r3=0x%08X (expect 0x%08X)  %s\n",
             ok ? "PASS" : "FAIL", x.va, ctx.r3.u32, x.expect, x.note);
    if (!ok) fails++;
  }
  // Negative probe: the SEH guard MUST catch the non-pure function's fault cleanly.
  {
    PPCContext ctx; std::memset(&ctx, 0, sizeof(ctx));
    ctx.r1.u32 = 0x70000000u; ctx.fpscr.csr = 0x1F80;
    ctx.r4.u64 = 0x82670774u;  // in-range code VA -> dispatch through null table -> AV
    FaultInfo fi{0, 0};
    int faulted = callGuarded(lookup(0x8279E494u)->fn, &ctx, base, &fi);
    bool ok = faulted != 0;     // we WANT a caught fault, not a clean return
    printf("[%s] 0x8279E494 -> %s (SEH guard %s)  non-leaf bctr negative case\n",
           ok ? "PASS" : "FAIL",
           faulted ? "faulted-and-caught" : "returned-without-fault",
           ok ? "OK" : "DID NOT FIRE");
    if (faulted) printf("       caught exception 0x%08X at 0x%016llX\n",
                        fi.code, (unsigned long long)fi.at);
    if (!ok) fails++;
  }
  return fails ? 1 : 0;
}

static void usage() {
  fprintf(stderr,
    "difftest_main — L6 recomp-side differential CPU-tester\n"
    "  difftest_main <guest_va_hex> [rN=value ...]   run one ad-hoc case\n"
    "  difftest_main --cases <file> [--json]         run every case in a .cases.json\n"
    "  difftest_main --case '<json-object>'          run one inline case object\n"
    "  difftest_main --self-test                     built-in 3-function check\n"
    "Output is ALWAYS the canonical golden JSON (docs/DIFFTEST_DESIGN.md §4); --json is\n"
    "accepted for explicitness and as a no-op (machine-readable is the default).\n");
}

int runArenaWaitTest();   // tools/difftest/arena_wait_test.cpp
int runTraceIdleBench(long long calls);   // tools/difftest/trace_idle_bench.cpp
int runPhysWinTest();   // tools/difftest/physwin_test.cpp
int runPhysResolveGrantTest();   // tools/difftest/physwin_test.cpp (Astra 044 grant mode)
int runVfetchPairTest();         // tools/difftest/vfetch_pair_test.cpp (iter-458 H-PAIR)
int runEdramTest();              // tools/difftest/edram_test.cpp (P1 EDRAM model)
namespace sr::gfx { int runNativeResolveCompileTest(); }
int runEdramSurface(int argc, char** argv);  // same file: per-draw EDRAM oracle decode
int runPhysWinCaptureTest();     // tools/difftest/physwin_test.cpp (Astra 054 PWS2)
int runPhysAllocTest();   // tools/difftest/physalloc_test.cpp
int runPhysLifoTest();    // tools/difftest/physlifo_test.cpp (Astra 058 H-LIFO)
int runPhysUniqTest();    // tools/difftest/physlifo_test.cpp (Astra 058 H-UNIQ)
int runUniqStickyDump(const char* path);   // tools/difftest/physlifo_test.cpp (Astra 060)
int runPhysLifoStackTest();   // tools/difftest/physlifo_test.cpp (Astra 065, hlifo1)
int runMemcpyGuardTest();     // tools/difftest/physlifo_test.cpp (Astra 066, hlifo2)
int runHeapAlignTest();       // tools/difftest/physlifo_test.cpp (iter-462 heap cursor alignment)
int runPhysLifoReuseTest();   // tools/difftest/physlifo_test.cpp (iter-463 F1 reuse + floor)
int runThreadMemTest();       // tools/difftest/physlifo_test.cpp (iter-465 physmem SR_THREAD_MEM_XENIA)
int runPhysXeniaPlaceTest();  // tools/difftest/physlifo_test.cpp (iter-465 physmem SR_PHYS_XENIA_PLACE)
int runGuestAllocFailTest();  // tools/difftest/guest_alloc_fail_test.cpp (step4 stuck-load [GUEST_ALLOC_FAIL])
int runLoadMarkerTest();      // tools/difftest/load_marker_test.cpp (step4 ruling 12 [LOAD_BEGIN] / [LOAD_DONE])
int runTexCensusTest();       // tools/difftest/tex_census_test.cpp (Astra 068)
int runGpuPhysRouteTest();    // tools/difftest/gpu_phys_route_test.cpp (Astra 071 HR1)
int runPhysIbTest();   // tools/difftest/physib_test.cpp
int runHrrTest();   // tools/difftest/hrr_test.cpp
int runObRefTest();   // tools/difftest/obref_test.cpp (iter-462 SR_OB_KTHREAD)
int runKpcrTest();    // tools/difftest/kpcr_test.cpp (iter-463 SR_KPCR / SR_KE_SPINLOCK)
int runXamContentTest();   // tools/difftest/xam_content_test.cpp (iter-463 SR_XAM_CONTENT_FS)
int runGpuHangWdTest();   // tools/difftest/gpu_hang_wd_test.cpp (iter-463 GPU_HANG_WD)
int runNativeMeterTest();  // tools/difftest/native_meter_test.cpp (iter-467 native-a: the meter's columns, dispreg)
int resource_reader_hooks_test_main(int argc, char** argv);
int runMemResourcesTest();  // tools/difftest/mem_resources_test.cpp (mem.resources step 2a: the registry + its census)
int resource_memory_d3d12_test_main(int argc, char** argv);
int xlat_cb_zerocopy_test_main(int argc, char** argv);   // tools/difftest/xlat_cb_zerocopy_test.cpp (step 6 I10p)
int native_census_test_main(int argc, char** argv);
int resource_consumer_test_main(int argc, char** argv);
int runMemResourcesFamilyTest(int argc, char** argv);  // ... memres6: the world family's traced draws
int runShaderBootLoadTest(int argc, char** argv);  // tools/difftest/shader_boot_load_test.cpp (render/boot-attach)
int runDevtoolsTest();    // tools/difftest/devtools_test.cpp (iter-463 phase 2 dev-tool gate)
int runHostAutomationTest();   // tools/difftest/host_automation_test.cpp (iter-464 bgruns)
int runQuitTest();             // tools/difftest/quit_test.cpp (tools/graceful-quit: the terminal snapshot)
int runStdioGuardTest();       // tools/difftest/stdio_guard_test.cpp (iter-465 player)
int runStdioGuardChild(const char* mode);
int runLogAsyncTest();         // tools/difftest/log_async_test.cpp (app/log-async: the asynchronous log writer)
int runLogAsyncChild(int argc, char** argv);
int runPcControlsTest();       // tools/difftest/pc_controls_test.cpp (iter-464 controls)
int runGameOptionsTest();      // tools/difftest/game_options_test.cpp (lane E L-b: the OPTIONS GAME / CHEATS tabs)
int runAssetReceiptTest();     // tools/difftest/asset_receipt_test.cpp (lane E step 5 E3-E6 r3)
int runPcActionsTest();        // tools/difftest/pc_actions_test.cpp (iter-466 controls2)
int runHostInputTest();   // tools/difftest/host_input_test.cpp (iter-464 input lane: SDL pads, hot-plug)
int runHostInputLive(int seconds, bool vpad);   // same file: real devices (or ViGEm only) for N s
int runSdl3PlatformTest();   // tools/difftest/sdl3_platform_test.cpp (RELEASE_PLAN step 3: SDL2 semantics on SDL3)
int runGlyphTest(const char* previewDir);   // tools/difftest/glyph_test.cpp (iter-464 button prompts)
int runModOverlayTest();   // tools/difftest/mod_overlay_test.cpp (iter-463 mods/ overlay)
int runModManagerTest();   // tools/difftest/mod_manager_test.cpp (iter-464 drop-in mods)
int runModManagerDump(int argc, char** argv);
int runCostumeAnimationTest();  // structural lifecycle checks, not live gameplay acceptance
int runClothCapacityTest();    // scratch-capacity differential and stack ownership
int runMouseUiTest();      // tools/difftest/mouse_ui_test.cpp (iter-464 mouse cursor)
int runModLibraryTest();   // tools/difftest/mod_library_test.cpp (iter-465 mod library)
int runMapMarkersTest();   // tools/difftest/map_markers_test.cpp (iter-466 useful pause map)
int runMapValidate(const char* path);
// iter-466 gfx: the graphics-settings pure helpers (gfx/gfx_options, kernel/pc_settings).
namespace sr { namespace gfx { namespace gfxopt { int selfTest(); } } }
namespace sr { namespace kernel { namespace pcsettings { int selfTest(); } } }
static int runGfxSettingsTest() {
  const int a = sr::gfx::gfxopt::selfTest(), b = sr::kernel::pcsettings::selfTest();
  std::printf("[gfx-settings-test] gfxopt %d failed, pcsettings %d failed%s", a, b, "\n");
  return (a || b) ? 1 : 0;
}
int runModLibraryRender(int argc, char** argv);  // tools/difftest/mod_library_render.cpp (offscreen look renders)
int runSettingsRender(int argc, char** argv);    // iter-466 gfx: the PC SETTINGS screen, same harness
int runAchievementsRender(int argc, char** argv);  // lane E L-a: the ACHIEVEMENTS page, same harness
int runWaitSemTest();      // tools/difftest/wait_sem_test.cpp (iter-463 wait/semaphore platform fixes)
int runCsReinitTest();     // tools/difftest/cs_reinit_test.cpp (step 6 I8: SR_CS_REINIT_REUSE)
int runEntryPoolTest(const char* mode);  // tools/difftest/entry_pool_test.cpp (step 6 I6: SR_ENTRY_POOL; N4 controls)
int runSegArenaTest(const char* mode);   // tools/difftest/seg_arena_test.cpp (step 6 I7: SR_SEG_ARENA; its ASan control)
int runApcGuardTest(const char* mode);  // tools/difftest/apc_guard_test.cpp (step 6 G14b: the APC spin guard)
int runDiagG6aTest(const char* mode);  // tools/difftest/diag_g6a_test.cpp (step 6 G6a: the DIAG, SR_ORACLE)
int runImportFixesTest();  // tools/difftest/import_fixes_test.cpp (step4/audit: Xenia-equal import answers)
int runAchievementsTest();  // tools/difftest/achievements_test.cpp (step5 E14a: the achievements core)
int runAchievementsPageTest();  // tools/difftest/achievements_page_test.cpp (lane E L-a: the ACHIEVEMENTS page)
int runImportAuditTest();  // tools/difftest/import_audit_test.cpp (step4/audit: SR_AUDIT_IMPORTS census; audit builds)
int runAtomicReadTest();   // tools/difftest/wait_sem_test.cpp (iter-463 SR_ATOMIC_READ / SR_READ_EOF_STATUS)
int runAsyncReadTest();    // tools/difftest/wait_sem_test.cpp (iter-466 stutter SR_ASYNC_READ)
int runFpPlatformTest();         // tools/difftest/fp_platform_test.cpp (iter-463 roundevenf + APC MXCSR)
int runCallRingTest();           // tools/difftest/callring_test.cpp (fix/postgame-apt: the import ring slot)
int runHeapDumpTest();           // tools/difftest/heap_dump_test.cpp (step4/relcrash: SR_CRASH_DUMP_HEAP + the UserBlocks sentinel)
int runCollisionWatchTest();  // tools/difftest/collision_watch_test.cpp (iter-463 SR_COLLISION_WATCH read layer)
int runTimebaseTest();          // tools/difftest/timebase_test.cpp (iter-463 SR_TIMEBASE_50MHZ + splice MXCSR)
int runShtimeHoldTest(bool on); // tools/difftest/shtime_hold_test.cpp (iter-467 native-a: shader time 0x29 hold)
int runHdrAdaptationTest(int argc, char** argv);
int runSegTileTest();  // tools/difftest/seg_tile_test.cpp (iter-467 native-a: SR_NATIVE_SEG_TILE, the strict transport witness)
int runCallbackTest();  // tools/difftest/callback_test.cpp (iter-467 native-a: p2.transport step 1, kRkCallback)
int runNativeBindlessTest(const char* mode);  // tools/difftest/native_bindless_test.cpp (step 6 I3-B, SR_ORACLE)
int runNativeZpdTest(const char* mode);  // tools/difftest/native_zpd_test.cpp (p2.zpd: the occlusion-query lifecycle, SR_NATIVE_ZPD; mode "latch" = the bake's fresh-process child)
int runNativeDisplayTest(const char* mode);
int runNativeVblankTest(const char* mode);  // display.registers (tools/difftest/native_vblank_test.cpp; "latch": E2's child)
int runGraphicsInterruptRegistrationTest(const char* mode);  // VD / CP delivery leases
int runPresentGateTest();
int runPresentGateSdkTest(const char* requested);
int runSwapBlockTest(const char* mode);
int runSwapSdkTest(const char* mode);
int runPresentComposedTest(const char* route);  // p2.transport composed present: InsertFence + gate + Swap
int runNativeMemoryTest();
int runNativeTransportTest(const char* mode);
int runNativePumpTest(const char* mode);
int runNativePumpDriverTest(const char* mode);
int runNativeRecordingTest(const char* mode);
int runNativeRecordAdapterTest();
namespace sr::native::flush { int runNativeRecordFailureTest(); }
namespace sr::native::flush { int runSegmentOwnershipTest(); }
namespace sr::native::flush { int runNativeBracketTest(const char* mode); }  // p2.transport BRACKET (kernel/native_flush.cpp)
namespace sr::native::flush { int runNativeResidueTest(const char* mode); }  // p2.transport RESIDUE (kernel/native_flush.cpp)
namespace sr::native::flush { int runCoherSentinelTest(const char* mode); }
namespace sr::native::flush { int runNativeCensusTest(const char* mode); }  // the PM4 owner census (kernel/native_flush.cpp)
namespace sr::native::flush { int runSetConstRenderTest(const char* mode); }  // P2-B item 5 (kernel/native_flush.cpp)
namespace sr::native::flush { int runFlushSplitTest(const char* mode); }  // p1.flush_residue (kernel/native_flush.cpp)
namespace sr::native::flush { int runZpdPortTest(const char* mode); }  // p2.zpd's bake: 82101E08's port (kernel/native_flush.cpp)
int runShaderConstRunsTest(const char* route);
int runHighfpsTest();           // tools/difftest/highfps_test.cpp (iter-464 PORT FEATURE SR_HIGHFPS)
int runFpRoundevenExhaustive();  // same file: all 2^32 patterns
int runIcallTest(const char* mode);   // tools/difftest/icall_test.cpp (iter-463 burndown item 10)
int runCmdqTest();         // tools/difftest/cmdq_test.cpp (iter-463 D-AUDIO-CMDBUF probes)
int runNativeHardwareAllocationTest(bool oldRangeMutant);
int runXmaTest();          // tools/difftest/xma_test.cpp (iter-463 SR_XMA_NEW / SR_MM_PHYSADDR)
int runAudioOutTest();     // tools/difftest/audio_out_test.cpp (iter-463 SR_AUDIO_OUT)
int runKeWaitMultiTest();  // tools/difftest/audio_out_test.cpp (iter-463 SR_KE_WAITMULTI)
int runProcTypeTest();     // tools/difftest/audio_out_test.cpp (iter-463 SR_PROCTYPE)
int runAudioSdlReplay(int argc, char** argv);   // tools/difftest/audio_out_test.cpp (lane sdl3-audio)

int main(int argc, char** argv) {
  // iter-463: difftest never mounts the player's mods (mods/enabled.txt): a test
  // that reads disc files through NtCreateFile must read the disc. SR_MODS=0
  // unless the caller named mods itself (docs/MODS.md "Automated runs").
  if (!std::getenv("SR_MODS")) _putenv_s("SR_MODS", "0");
  // iter-463 phase 2: nor a dev tool (docs/DEVTOOLS.md; --devtools-test sets its own).
  if (!std::getenv("SR_DEVTOOLS")) _putenv_s("SR_DEVTOOLS", "0");
  if (argc > 1 && std::string(argv[1]) == "--replay-census-test")
    return native_census_test_main(argc, argv);
  if (argc > 1 && std::string(argv[1]) == "--resource-reader-hooks-test")
    return resource_reader_hooks_test_main(argc, argv);
  if (argc == 2 && std::string(argv[1]) == "--resource-consumer-test")
    return resource_consumer_test_main(argc, argv);
  if (argc > 1 && std::string(argv[1]) == "--resource-memory-d3d12-test")
    return resource_memory_d3d12_test_main(argc, argv);
  if (argc > 1 && std::string(argv[1]) == "--xlat-cb-zerocopy-test")   // step 6 I10p (Astra r2): no guest memory
    return xlat_cb_zerocopy_test_main(argc, argv);
  // app/log-async: the log writer's child processes need no guest memory (it commits 4 GB per process);
  // only the hang-dump case reads the watchdog's guest-facing state.
  if (argc > 2 && std::string(argv[1]) == "--log-async-child" && std::string(argv[2]) != "hangdump")
    return runLogAsyncChild(argc, argv);
  // One-time world: a flat guest memory + a 4 GB-aligned base so PPC_GUEST_BASE()
  // and the low-32 address math behave exactly like the game. Tier-0 leaves never
  // touch it, but we still allocate so any accidental access is valid host memory.
  const size_t kSize = 0x100000000ull;  // == PPC_MEMORY_SIZE
  uint8_t* base = nullptr;
#if defined(_WIN32)
  base = (uint8_t*)VirtualAlloc((LPVOID)0x2A00000000ULL, kSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!base) base = (uint8_t*)VirtualAlloc(nullptr, kSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#endif
  if (!base) {
    // Fallback: small heap block (Tier-0 needs no real guest RAM, only a non-null base).
    base = (uint8_t*)calloc(1, 0x1000);
  }
  sr::kernel::setGuestMemory(base, kSize);

  if (argc < 2) { usage(); return 2; }

  std::string a1 = argv[1];

  if (a1 == "--self-test") {
    return selfTest(base);
  }

  // iter-458 (Astra 017): SR_HLE_ARENA_WAIT backpressure contract.
  if (a1 == "--arena-wait-test") return runArenaWaitTest();

  // iter-458 (H-REC): recorder cost while enabled but not open (measurement).
  // iter-458 H-WIN: read-window census contract.
  if (a1 == "--physwin-test") return runPhysWinTest();
  if (a1 == "--physresolve-grant-test") return runPhysResolveGrantTest();
  if (a1 == "--vfetch-pair-test") return runVfetchPairTest();
  if (a1 == "--edram-test") return runEdramTest();
  if (a1 == "--native-resolve-compile-test") return sr::gfx::runNativeResolveCompileTest();
  if (a1 == "--edram-surface") return runEdramSurface(argc, argv);
  if (a1 == "--physwin-win-test") return runPhysWinCaptureTest();
  // iter-458 H-FREE: reusable physical allocator + export wiring.
  if (a1 == "--physalloc-test") return runPhysAllocTest();
  // iter-458 H-LIFO / H-UNIQ (Astra 058): LIFO physical free + uniqueness census.
  if (a1 == "--phys-lifo-test") return runPhysLifoTest();
  // step4 stuck-load: the guest allocation-failure breadcrumb (rate rule, player profile, the real exports).
  if (a1 == "--guest-alloc-fail-test") return runGuestAllocFailTest();
  // step4 ruling 12: the load-screen markers (player profile keeps them; the overrides read, call once, log).
  if (a1 == "--load-marker-test") return runLoadMarkerTest();
  if (a1 == "--phys-uniq-test") return runPhysUniqTest();
  if (a1 == "--uniq-sticky-dump" && argc > 2) return runUniqStickyDump(argv[2]);
  if (a1 == "--phys-lifo-stack-test") return runPhysLifoStackTest();
  if (a1 == "--memcpy-guard-test") return runMemcpyGuardTest();
  if (a1 == "--heap-align-test") return runHeapAlignTest();
  // iter-463 F1: LIFO free-extent reuse, cascade and the 0xA0000000 floor (fresh process).
  if (a1 == "--phys-lifo-reuse-test") return runPhysLifoReuseTest();
  if (a1 == "--threadmem-test") return runThreadMemTest();
  if (a1 == "--phys-xenia-place-test") return runPhysXeniaPlaceTest();
  if (a1 == "--tex-census-test") return runTexCensusTest();
  if (a1 == "--gpu-phys-route-test") return runGpuPhysRouteTest();
  // iter-458 (Astra 029 A): ownership-resolved IB reads through the real entry.
  if (a1 == "--physib-test") return runPhysIbTest();
  // iter-458 H-VQ (Astra 035): the hostRangeReadable interval cache.
  if (a1 == "--hrr-test") return runHrrTest();
  // iter-462: guest KTHREADs + ObReferenceObjectByHandle through the real
  // GetExitCodeThread (sub_82469E98) and the real exports.
  if (a1 == "--obref-test") return runObRefTest();
  // iter-463: per-thread KPCR (real r13) + Ke/Kf spinlocks through the real glue.
  if (a1 == "--kpcr-test") return runKpcrTest();
  // iter-463: real save storage (XAM content packages) through the real guest wrappers.
  if (a1 == "--xam-content-test" || a1 == "--xamcontent-test") return runXamContentTest();
  // iter-463: the GPU_HANG_WD trigger / episode logic and its latches.
  if (a1 == "--gpu-hang-wd-test") return runGpuHangWdTest();
  // iter-467 native-a: the native meter's columns + the display-register column's feed.
  if (a1 == "--native-meter-test") return runNativeMeterTest();
  // mem.resources step 2a: the resource registry's generations and the shadow census's classification and line.
  if (a1 == "--mem-resources-test") return runMemResourcesTest();
  // memres6 (e): the world family's draws replayed through the census, each exact index range inside its bound.
  if (a1 == "--mem-resources-family-test") return runMemResourcesFamilyTest(argc, argv);
  // render/boot-attach: the shader cache's staged boot load and its wait points (docs/NATIVE_ROADMAP.md "Boot attach").
  if (a1 == "--shader-boot-load-test") return runShaderBootLoadTest(argc, argv);
  // iter-463: mods/<name>/ asset overlay through CreateFileA/ReadFile and the Nt* exports.
  if (a1 == "--mod-overlay-test") return runModOverlayTest();
  if (a1 == "--mod-manager-test") return runModManagerTest();
  if (a1 == "--costume-animation-test") return runCostumeAnimationTest();
  if (a1 == "--cloth-capacity-test") return runClothCapacityTest();
  if (a1 == "--mod-manager-dump") return runModManagerDump(argc, argv);
  if (a1 == "--mouse-ui-test") return runMouseUiTest();
  if (a1 == "--mod-library-test") return runModLibraryTest();
  if (a1 == "--map-test") return runMapMarkersTest();
  if (a1 == "--map-validate" && argc >= 3) return runMapValidate(argv[2]);
  if (a1 == "--gfx-settings-test") return runGfxSettingsTest();
  if (a1 == "--mod-library-render") return runModLibraryRender(argc, argv);
  if (a1 == "--settings-render") return runSettingsRender(argc, argv);
  if (a1 == "--achievements-render") return runAchievementsRender(argc, argv);
  // iter-463 phase 2: the dev-tool gate grammar, predicates and tags (kernel/devtools.cpp).
  if (a1 == "--devtools-test") return runDevtoolsTest();
  // iter-464 bgruns: automated runs stay in the background (policy, harness keyboard channel).
  if (a1 == "--host-automation-test") return runHostAutomationTest();
  // tools/graceful-quit: the harness-requested quit, its safe point and the terminal snapshot (kernel/quit.cpp).
  if (a1 == "--quit-test") return runQuitTest();
  // iter-465 player: no thread blocks on a stalled stdout/stderr (app/stdio_guard.cpp).
  if (a1 == "--stdio-guard-test") return runStdioGuardTest();
  if (a1 == "--stdio-guard-child") return runStdioGuardChild(argc > 2 ? argv[2] : "");
  // app/log-async: no logging thread waits on the disk; order, no loss, the drains (docs/TOOLKIT.md 3.2).
  if (a1 == "--log-async-test") return runLogAsyncTest();
  if (a1 == "--log-async-child") return runLogAsyncChild(argc, argv);
  if (a1 == "--pc-controls-test") return runPcControlsTest();
  if (a1 == "--game-options-test") return runGameOptionsTest();
  if (a1 == "--asset-receipt-test") return runAssetReceiptTest();
  if (a1 == "--pc-actions-test") return runPcActionsTest();
  // iter-464 input lane: SDL controllers, hot-plug, player 1, bindings, isolation.
  if (a1 == "--host-input-test") return runHostInputTest();
  // RELEASE_PLAN step 3: key names, input.ini, display modes and the window handle on SDL3.
  if (a1 == "--sdl3-platform-test") return runSdl3PlatformTest();
  if (a1 == "--host-input-live") return runHostInputLive(argc > 2 ? std::atoi(argv[2]) : 30, false);
  // iter-466 tooling: SR_HARNESS_VPAD - an automated run opens only ViGEm virtual pads.
  if (a1 == "--host-input-vpad") return runHostInputLive(argc > 2 ? std::atoi(argv[2]) : 30, true);
  if (a1 == "--vpad-filter-test") {
    int acc = 0;
    const int bad = sr::kernel::vpad::scanForTest(&acc);
    std::printf("[vpad-filter-test] %s\n", bad == 0 ? "PASS (nothing off the ViGEm bus accepted)" : "FAIL");
    return bad == 0 ? 0 : 1;
  }
  if (a1 == "--glyph-test") return runGlyphTest(nullptr);
  if (a1 == "--glyph-preview") return runGlyphTest(argc > 2 ? argv[2] : "glyph_preview");
  // iter-463: wait / semaphore status codes and NtReadFile positioned reads vs Xenia.
  if (a1 == "--wait-sem-test") return runWaitSemTest();
  if (a1 == "--cs-reinit-test") return runCsReinitTest();
  if (a1 == "--entry-pool-test") return runEntryPoolTest(argc > 2 ? argv[2] : nullptr);
  if (a1 == "--seg-arena-test") return runSegArenaTest(argc > 2 ? argv[2] : nullptr);
  if (a1 == "--apc-guard-test") return runApcGuardTest(argc > 2 ? argv[2] : nullptr);
  if (a1 == "--diag-g6a-test") return runDiagG6aTest(argc > 2 ? argv[2] : nullptr);
  if (a1 == "--import-fixes-test") return runImportFixesTest();
  if (a1 == "--achievements-test") return runAchievementsTest();
  if (a1 == "--achievements-page-test") return runAchievementsPageTest();
  if (a1 == "--import-audit-test") return runImportAuditTest();
  if (a1 == "--atomic-read-test") return runAtomicReadTest();
  if (a1 == "--async-read-test") return runAsyncReadTest();
  if (a1 == "--fp-platform-test") return runFpPlatformTest();
  if (a1 == "--heap-dump-test") return runHeapDumpTest();   // step4/relcrash: the heap dump + the UserBlocks sentinel
  if (a1 == "--callring-test") return runCallRingTest();   // fix/postgame-apt: the import ring never writes out of bounds
  if (a1 == "--collision-watch-test") return runCollisionWatchTest();
  if (a1 == "--timebase-test") return runTimebaseTest();
  if (a1 == "--shtime-hold-test") return runShtimeHoldTest(false);
  if (a1 == "--hdr-adaptation-test") return runHdrAdaptationTest(argc, argv);
  if (a1 == "--shtime-hold-test-on") return runShtimeHoldTest(true);
  if (a1 == "--seg-tile-test") return runSegTileTest();
  if (a1 == "--callback-test") return runCallbackTest();
  // p2.zpd: Issue / GetData guest bodies vs the native occlusion queries (slot words, fence ordering, latency model).
  if (a1 == "--native-zpd-test") return runNativeZpdTest(argc > 2 ? argv[2] : nullptr);
  if (a1 == "--native-bindless-test") return runNativeBindlessTest(argc > 2 ? argv[2] : nullptr);
  if (a1 == "--native-display-test") return runNativeDisplayTest(argc > 2 ? argv[2] : "native");
  if (a1 == "--native-vblank-test") return runNativeVblankTest(argc > 2 ? argv[2] : nullptr);
  if (a1 == "--graphics-interrupt-registration-test")
    return runGraphicsInterruptRegistrationTest(argc > 2 ? argv[2] : "native");
  if (a1 == "--present-gate-test") return runPresentGateTest();
  if (a1 == "--present-gate-sdk-test") return runPresentGateSdkTest(argc > 2 ? argv[2] : "leaf");
  if (a1 == "--present-composed-test") return runPresentComposedTest(argc > 2 ? argv[2] : "direct");
  if (a1 == "--swap-block-test") return runSwapBlockTest(argc > 2 ? argv[2] : "normal");
  if (a1 == "--swap-sdk-test") return runSwapSdkTest(argc > 2 ? argv[2] : "direct");
  // boot gaps (native/boot-gaps): the display emitters (821056E0 / 821057C0 / 82111CB8 / 8210A788) and the mode state
  // 82103238 against their SDK bodies (swap_sdk_test.cpp bootGapsDirect; tools/difftest/boot_gaps_run.py)
  if (a1 == "--boot-gaps-test") return runSwapSdkTest("bootgaps");
  if (a1 == "--native-memory-test") return runNativeMemoryTest();
  if (a1 == "--native-transport-test") return runNativeTransportTest(argc > 2 ? argv[2] : "native");
  if (a1 == "--native-pump-test") return runNativePumpTest(argc > 2 ? argv[2] : "native");
  if (a1 == "--native-pump-driver-test") return runNativePumpDriverTest(argc > 2 ? argv[2] : "native");
  if (a1 == "--native-recording-test") return runNativeRecordingTest(argc > 2 ? argv[2] : "native");
  if (a1 == "--native-record-adapter-test") return runNativeRecordAdapterTest();
  if (a1 == "--native-record-failure-test") return sr::native::flush::runNativeRecordFailureTest();
  if (a1 == "--native-segment-ownership-test") return sr::native::flush::runSegmentOwnershipTest();
  if (a1 == "--native-bracket-test") return sr::native::flush::runNativeBracketTest(argc > 2 ? argv[2] : "native");
  if (a1 == "--native-residue-test") return sr::native::flush::runNativeResidueTest(argc > 2 ? argv[2] : "native");
  if (a1 == "--coher-sentinel-test") return sr::native::flush::runCoherSentinelTest(argc > 2 ? argv[2] : "records");
  if (a1 == "--native-census-test") return sr::native::flush::runNativeCensusTest(argc > 2 ? argv[2] : "native");
  if (a1 == "--setconst-render-test") return sr::native::flush::runSetConstRenderTest(argc > 2 ? argv[2] : "cppm4");
  if (a1 == "--flush-split-test") return sr::native::flush::runFlushSplitTest(argc > 2 ? argv[2] : "records");
  if (a1 == "--zpd-port-test") return sr::native::flush::runZpdPortTest(argc > 2 ? argv[2] : "records");
  if (a1 == "--shader-const-runs-test") return runShaderConstRunsTest(argc > 2 ? argv[2] : "entry");
  // iter-464 PORT FEATURE high frame rate: config, the real render mgr + StateStream, pacing.
  if (a1 == "--highfps-test") return runHighfpsTest();
  if (a1 == "--fp-roundeven-exhaustive") return runFpRoundevenExhaustive();
  // iter-463 burndown item 10: PPC_CALL_INDIRECT_FUNC -> indirectCallUnresolved
  // (`continue` = under SR_ALLOW_FORCING; `fail` = default, must terminate).
  if (a1 == "--icall-test") return runIcallTest(argc > 2 ? argv[2] : "fail");
  // iter-463 audio: the native XMA port (Xenia xma_decoder=new) + MmGetPhysicalAddress.
  if (a1 == "--cmdq-test") return runCmdqTest();
  if (a1 == "--native-hardware-allocation-test")
    return runNativeHardwareAllocationTest(argc > 2 && std::string(argv[2]) == "old-range");
  if (a1 == "--xma-test") return runXmaTest();
  // iter-463 audio steps 4-8: render-driver output, KeWaitForMultipleObjects, process type.
  if (a1 == "--audio-out-test") return runAudioOutTest();
  if (a1 == "--ke-waitmulti-test") return runKeWaitMultiTest();
  if (a1 == "--proctype-test") return runProcTypeTest();
  if (a1 == "--audio-sdl-replay") return runAudioSdlReplay(argc, argv);   // lane sdl3-audio: SDL replay harness

  if (a1 == "--trace-idle-bench")
    return runTraceIdleBench(argc > 2 ? std::atoll(argv[2]) : 0);

  if (a1 == "--cases") {
    if (argc < 3) { usage(); return 2; }
    std::string text = slurp(argv[2]);
    if (text.empty()) return 2;
    int rc = 0;
    auto objs = splitObjects(text);
    printf("[\n");
    for (size_t i = 0; i < objs.size(); ++i) {
      // each runCase prints exactly one JSON object; we add commas + wrap in an
      // array so the whole output is valid JSON for compare.py.
      printf("  ");
      int r = runCase(base, caseFromObj(objs[i]));
      if (r) rc = r;
      if (i + 1 < objs.size()) printf("  ,\n");
    }
    printf("]\n");
    return rc;
  }

  if (a1 == "--case") {
    if (argc < 3) { usage(); return 2; }
    return runCase(base, caseFromObj(argv[2]));
  }

  // Ad-hoc form:  <va_hex> r3=0x7 r4=0x5
  Case c;
  c.va = (uint32_t)parseHex(a1);
  c.id = a1;
  for (int i = 2; i < argc; ++i) {
    std::string kv = argv[i];
    size_t eq = kv.find('=');
    if (eq == std::string::npos) continue;
    c.regs[kv.substr(0, eq)] = parseHex(kv.substr(eq + 1));
  }
  return runCase(base, c);
}
