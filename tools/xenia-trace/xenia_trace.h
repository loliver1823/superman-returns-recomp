/*
 * xenia_trace.h  —  L9-A golden-oracle guest-function-entry trace writer.
 *
 * Part of the Superman Returns recomp project (docs/L9_DIFFERENTIAL_ORACLE.md §A).
 * This header is ADDED to xenia-canary by tools/xenia-trace/xenia_trace.patch
 * (reference/xenia-canary) or xenia_trace_oracle.patch (reference/xenia-canary-oracle).
 * It is intentionally self-contained (no Xenia headers) so it can be dropped into
 * src/xenia/cpu/ and #included from function.cc / x64_emitter.cc.
 *
 * Purpose: emit a ground-truth, ordered, per-guest-function-entry trace of a
 * *working* Xenia boot. The resulting file is the "golden" stream the L9-C
 * divergence bisector walks, and (iter-463 v2) the liveness oracle for "does
 * the console-faithful emulator ever enter guest VA X / make an indirect call
 * to a bad target?".
 *
 * ---------------------------------------------------------------------------
 * ENVIRONMENT (all read once, at first use)
 * ---------------------------------------------------------------------------
 *   XENIA_GUEST_TRACE=1            master switch (any non-empty, non-"0" value).
 *                                  Unset => JIT output is byte-identical to stock.
 *   XENIA_GUEST_TRACE_FILE=path    output file (default "xenia_boot.trace").
 *   XENIA_GUEST_TRACE_START=hexVA  stay silent until the first entry at this VA.
 *   XENIA_GUEST_TRACE_CAP=N        record cap (default 60,000,000).
 *
 *   v2 (iter-463 burndown oracle):
 *   XENIA_GUEST_TRACE_VAS=list     function-entry FILTER. Only guest functions whose
 *                                  entry VA is in the list get the prolog hook (decided
 *                                  at JIT time, so every other function is stock code:
 *                                  near-zero overhead, a full boot-to-gameplay run is
 *                                  practical). Unset => every function (v1 behaviour).
 *                                  A function is JIT'd at a VA only when something
 *                                  calls/branches to it as a function, so a hit at a
 *                                  mid-function VA means the guest really called it.
 *   XENIA_GUEST_TRACE_ICALL_FUNCS=list
 *                                  record EVERY indirect call (bctrl/bctr) made from
 *                                  inside these functions (list = function-start VAs):
 *                                  tag 2, guest_pc = call TARGET, lr = call-site return
 *                                  address, r3 = this/arg0.
 *   XENIA_GUEST_TRACE_ICALL_BAD=1  instrument every indirect call in every function but
 *                                  record only targets outside the recomp's code range
 *                                  [CODE_LO, CODE_HI) or not 4-aligned: tag 3. This is the
 *                                  exact condition our PPC_CALL_INDIRECT_FUNC reports as
 *                                  "out-of-range". Costs one host call per indirect call.
 *   XENIA_GUEST_TRACE_CODE_LO/HI   hex bounds for the above (default 0x820F0000 /
 *                                  0x8290F5B8 = PPC_CODE_BASE / +PPC_CODE_SIZE).
 *   Lists: hex VAs (0x optional) separated by commas/whitespace/semicolons, or
 *   "@path" to read them from a file ('#' starts a comment).
 *   With any v2 filter active every record is fflush'ed, so a crash or kill keeps it.
 *
 * ---------------------------------------------------------------------------
 * ON-DISK TRACE FORMAT  (little-endian; Xenia host is x86-64 LE) — UNCHANGED
 * ---------------------------------------------------------------------------
 *   Header (32 bytes, written once at open):
 *     offset 0  : char     magic[8]   = "XENTRC01"
 *     offset 8  : uint32_t record_size = 32
 *     offset 12 : uint32_t endian_tag  = 0x04030201
 *     offset 16 : uint64_t reserved0   = 0
 *     offset 24 : uint64_t reserved1   = 0
 *
 *   Record (32 bytes, in execution order):
 *     offset 0  : uint32_t guest_pc    function entry VA (tag 0) / call target (tag 2,3)
 *     offset 4  : uint32_t lr          link register lo32 (tag 2,3: call-site return addr)
 *     offset 8  : uint64_t r3          1st arg / this
 *     offset 16 : uint64_t r1          stack pointer
 *     offset 24 : uint32_t thread_id   guest thread id
 *     offset 28 : uint32_t tag         XeTraceTag
 * ---------------------------------------------------------------------------
 */
#ifndef XENIA_TRACE_H_
#define XENIA_TRACE_H_

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

namespace xe {
namespace trace {

enum XeTraceTag : uint32_t {
  kXeTraceFuncEntry = 0,        // guest_pc/lr/r3/r1 are a function-entry sample
  kXeTraceMemWrite = 1,         // reserved for future guest-store records
  kXeTraceIndirectCall = 2,     // v2: indirect call from an ICALL_FUNCS function
  kXeTraceIndirectCallBad = 3,  // v2: indirect call to an out-of-code-range target
};

#pragma pack(push, 1)
struct XeTraceHeader {
  char magic[8];
  uint32_t record_size;
  uint32_t endian_tag;
  uint64_t reserved0;
  uint64_t reserved1;
};
struct XeTraceRecord {
  uint32_t guest_pc;
  uint32_t lr;
  uint64_t r3;
  uint64_t r1;
  uint32_t thread_id;
  uint32_t tag;
};
#pragma pack(pop)

static_assert(sizeof(XeTraceHeader) == 32, "trace header must be 32 bytes");
static_assert(sizeof(XeTraceRecord) == 32, "trace record must be 32 bytes");

// Process-wide singleton writer. Lazily opened on first record if enabled.
class GuestTracer {
 public:
  static GuestTracer& Get() {
    static GuestTracer instance;
    return instance;
  }

  // True only when XENIA_GUEST_TRACE is set to an enabling value.
  bool enabled() const { return enabled_; }

  // JIT-time: does the function starting at `va` get the prolog hook?
  bool WantProlog(uint32_t va) const {
    return enabled_ && (!prolog_filter_ || prolog_set_.count(va) != 0);
  }

  // JIT-time: do indirect calls inside the function starting at `fn_va` get the
  // call-site hook?
  bool WantIcall(uint32_t fn_va) const {
    return enabled_ && (icall_bad_all_ || icall_set_.count(fn_va) != 0);
  }

  void EmitFuncEntry(uint32_t guest_pc, uint32_t lr, uint64_t r3, uint64_t r1,
                     uint32_t thread_id) {
    if (!enabled_) {
      return;
    }
    // PC start-gate (mirrors recomp SR_GUEST_TRACE_START): stay silent until the first
    // guest-function entry at start_va_, then emit from there on. start_va_==0 => from start.
    if (!armed_) {
      if (guest_pc != start_va_) {
        return;
      }
      armed_ = true;
    }
    Write(guest_pc, lr, r3, r1, thread_id, kXeTraceFuncEntry);
  }

  // Runtime half of the call-site hook. fn_va is the JIT-time constant of the
  // calling function; target is the (low 32 bits of the) indirect-call target.
  void EmitIndirectCall(uint32_t fn_va, uint32_t target, uint32_t lr,
                        uint64_t r3, uint64_t r1, uint32_t thread_id) {
    if (!enabled_ || !armed_) {
      return;
    }
    const bool bad = target < code_lo_ || target >= code_hi_ || (target & 3u);
    if (icall_set_.count(fn_va) != 0) {
      Write(target, lr, r3, r1, thread_id,
            bad ? kXeTraceIndirectCallBad : kXeTraceIndirectCall);
    } else if (icall_bad_all_ && bad) {
      Write(target, lr, r3, r1, thread_id, kXeTraceIndirectCallBad);
    }
  }

 private:
  GuestTracer() {
    const char* v = std::getenv("XENIA_GUEST_TRACE");
    enabled_ = (v && v[0] && std::strcmp(v, "0") != 0);
    const char* s = std::getenv("XENIA_GUEST_TRACE_START");
    start_va_ = (s && s[0]) ? (uint32_t)std::strtoul(s, nullptr, 16) : 0;
    armed_ = (start_va_ == 0);  // no start VA -> emit from the start
    const char* c = std::getenv("XENIA_GUEST_TRACE_CAP");
    cap_ = (c && c[0]) ? std::strtoull(c, nullptr, 10) : 60000000ull;
    prolog_filter_ = ParseList("XENIA_GUEST_TRACE_VAS", &prolog_set_);
    ParseList("XENIA_GUEST_TRACE_ICALL_FUNCS", &icall_set_);
    const char* b = std::getenv("XENIA_GUEST_TRACE_ICALL_BAD");
    icall_bad_all_ = (b && b[0] && std::strcmp(b, "0") != 0);
    const char* lo = std::getenv("XENIA_GUEST_TRACE_CODE_LO");
    const char* hi = std::getenv("XENIA_GUEST_TRACE_CODE_HI");
    code_lo_ = (lo && lo[0]) ? (uint32_t)std::strtoul(lo, nullptr, 16) : 0x820F0000u;
    code_hi_ = (hi && hi[0]) ? (uint32_t)std::strtoul(hi, nullptr, 16) : 0x8290F5B8u;
    flush_each_ = prolog_filter_ || !icall_set_.empty() || icall_bad_all_;
  }
  ~GuestTracer() {
    if (file_) {
      std::fflush(file_);
      std::fclose(file_);
      file_ = nullptr;
    }
  }

  // Parses a VA list from env var `name` into *out. Returns true when the variable
  // is set (even if it parsed to an empty set: an explicit empty filter traces nothing).
  static bool ParseList(const char* name, std::unordered_set<uint32_t>* out) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) {
      return false;
    }
    std::string text;
    if (v[0] == '@') {
      if (std::FILE* f = std::fopen(v + 1, "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        std::fclose(f);
      }
    } else {
      text = v;
    }
    size_t i = 0;
    while (i < text.size()) {
      const char ch = text[i];
      if (ch == '#') {  // comment to end of line
        while (i < text.size() && text[i] != '\n') ++i;
        continue;
      }
      if (std::isxdigit(static_cast<unsigned char>(ch))) {
        size_t j = i;
        if (ch == '0' && j + 1 < text.size() && (text[j + 1] == 'x' || text[j + 1] == 'X')) {
          j += 2;
        }
        size_t k = j;
        while (k < text.size() && std::isxdigit(static_cast<unsigned char>(text[k]))) ++k;
        if (k > j) {
          out->insert(static_cast<uint32_t>(std::strtoul(text.substr(j, k - j).c_str(), nullptr, 16)));
        }
        i = (k > i) ? k : i + 1;
        continue;
      }
      ++i;
    }
    return true;
  }

  void Write(uint32_t guest_pc, uint32_t lr, uint64_t r3, uint64_t r1,
             uint32_t thread_id, uint32_t tag) {
    XeTraceRecord rec;
    rec.guest_pc = guest_pc;
    rec.lr = lr;
    rec.r3 = r3;
    rec.r1 = r1;
    rec.thread_id = thread_id;
    rec.tag = tag;

    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ >= cap_) {
      return;
    }
    ++count_;
    if (!file_) {
      OpenLocked();
      if (!file_) {
        enabled_ = false;  // give up quietly; never crash the boot
        return;
      }
    }
    std::fwrite(&rec, sizeof(rec), 1, file_);
    if (flush_each_) {
      std::fflush(file_);
    }
  }

  void OpenLocked() {
    const char* path = std::getenv("XENIA_GUEST_TRACE_FILE");
    if (!path || !path[0]) {
      path = "xenia_boot.trace";
    }
    file_ = std::fopen(path, "wb");
    if (!file_) {
      return;
    }
    XeTraceHeader hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    std::memcpy(hdr.magic, "XENTRC01", 8);
    hdr.record_size = sizeof(XeTraceRecord);
    hdr.endian_tag = 0x04030201u;
    std::fwrite(&hdr, sizeof(hdr), 1, file_);
    std::fflush(file_);
  }

  bool enabled_ = false;
  std::FILE* file_ = nullptr;
  std::mutex mutex_;
  // L9-C start-gate state (XENIA_GUEST_TRACE_START / _CAP), set in the ctor.
  uint32_t start_va_ = 0;        // anchor VA; 0 = no gate (emit from start)
  bool armed_ = true;            // gate open? (ctor sets false when start_va_ != 0)
  unsigned long long cap_ = 60000000ull;  // runaway backstop (mirrors recomp kTraceCap)
  unsigned long long count_ = 0;
  // v2 filters.
  bool prolog_filter_ = false;
  std::unordered_set<uint32_t> prolog_set_;
  std::unordered_set<uint32_t> icall_set_;
  bool icall_bad_all_ = false;
  uint32_t code_lo_ = 0x820F0000u;
  uint32_t code_hi_ = 0x8290F5B8u;
  bool flush_each_ = false;
};

}  // namespace trace
}  // namespace xe

#endif  // XENIA_TRACE_H_
