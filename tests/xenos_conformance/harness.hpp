#pragma once
// xenos_conformance - test harness (result bookkeeping, golden vector files,
// digests, deterministic RNG, parallel sweeps). Not Xenia code. See the usage
// block at the top of conformance_main.cpp.

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace xc {

// ---------------------------------------------------------------------------
// Result classes.
//   PASS   - checked, identical to the reference.
//   FAIL   - our gfx/ code differs from the Xenia reference (default config):
//            a defect to fix (or to waive with a written reason). Exit code 1.
//   GAP    - the reference does something our code has no implementation for.
//   WAIVED - a documented, intentional deviation of our code (reason printed).
//   INFO   - facts about the reference itself (Xenia CPU vs shader paths
//            disagreeing, census numbers) and golden-file creation.
// GAP and WAIVED do not fail the run unless --strict.
enum class Status { Pass, Fail, Gap, Waived, Info };

inline const char* StatusTag(Status s) {
  switch (s) {
    case Status::Pass: return "PASS  ";
    case Status::Fail: return "FAIL  ";
    case Status::Gap: return "GAP   ";
    case Status::Waived: return "WAIVED";
    default: return "INFO  ";
  }
}

struct Results {
  int counts[5] = {};
  std::vector<std::string> notable;  // every non-PASS line, for the summary
  bool verbose = false;

  void Report(Status s, const std::string& id, const std::string& detail) {
    counts[int(s)]++;
    std::string line = std::string("[") + StatusTag(s) + "] " + id;
    if (!detail.empty()) line += "  " + detail;
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
    if (s != Status::Pass) notable.push_back(line);
  }
};

inline std::string Fmt(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return buf;
}

// Mismatch collector: total count plus the first few examples.
struct Mismatches {
  uint64_t checked = 0;
  uint64_t count = 0;
  std::vector<std::string> examples;
  static constexpr size_t kMaxExamples = 6;

  void Check() { ++checked; }
  void Add(const std::string& example) {
    ++count;
    if (examples.size() < kMaxExamples) examples.push_back(example);
  }
  void Merge(const Mismatches& o) {
    checked += o.checked;
    count += o.count;
    for (const auto& e : o.examples) {
      if (examples.size() < kMaxExamples) examples.push_back(e);
    }
  }
  std::string Summary() const {
    std::string s = Fmt("%" PRIu64 " checked, %" PRIu64 " mismatch%s", checked,
                        count, count == 1 ? "" : "es");
    for (const auto& e : examples) s += "\n           e.g. " + e;
    return s;
  }
};

// FNV-1a 64 over a little-endian byte stream.
struct Fnv {
  uint64_t h = 0xCBF29CE484222325ull;
  void Byte(uint8_t b) {
    h ^= b;
    h *= 0x100000001B3ull;
  }
  void U32(uint32_t v) {
    for (int i = 0; i < 4; ++i) Byte(uint8_t(v >> (8 * i)));
  }
  void U64(uint64_t v) {
    U32(uint32_t(v));
    U32(uint32_t(v >> 32));
  }
};

// splitmix64: deterministic, seedable.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint64_t Next() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  uint32_t U32() { return uint32_t(Next() >> 32); }
  uint32_t Below(uint32_t n) { return n ? uint32_t((Next() >> 32) % n) : 0; }
  float Uniform(float lo, float hi) {
    return lo + (hi - lo) * float(double(Next() >> 11) * (1.0 / 9007199254740992.0));
  }
};

// Parallel sweep over [begin, end) with a stride; fn(value, local) per item.
// Each worker owns a Mismatches; they are merged in worker order.
template <class F>
Mismatches ParallelSweep(uint64_t begin, uint64_t end, uint64_t step,
                         unsigned threads, const F& fn) {
  threads = std::max(1u, threads);
  const uint64_t n = end > begin ? (end - begin + step - 1) / step : 0;
  if (n < 65536) threads = 1;
  std::vector<Mismatches> local(threads);
  auto work = [&](unsigned t) {
    // Collect on this worker's own stack (adjacent vector elements would
    // share a cache line and serialize the workers), publish once at the end.
    Mismatches mine;
    const uint64_t lo = n * t / threads, hi = n * (t + 1) / threads;
    for (uint64_t i = lo; i < hi; ++i) fn(begin + i * step, mine);
    local[t] = std::move(mine);
  };
  if (threads == 1) {
    work(0);
  } else {
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) pool.emplace_back(work, t);
    for (auto& th : pool) th.join();
  }
  Mismatches all;
  for (const auto& l : local) all.Merge(l);
  return all;
}

// ---------------------------------------------------------------------------
// Golden vector files (data/conformance/<name>.tsv).
//
// Written ONLY from the Xenia reference functions, never from our code. A
// file is a deterministic text table:
//   # comment lines (description, sources, columns)
//   @digest <name> count=<n> fnv1a64=<hex>   (hash of a dense/exhaustive sweep)
//   <hex>\t<hex>...                          (sampled rows)
// Modes: missing file -> created (INFO); --regen -> rewritten; otherwise the
// freshly computed table must match the file byte for byte (ignoring '\r').
enum class GoldenMode { CreateMissing, Regen, Verify };

class Golden {
 public:
  Golden(std::string name, std::string description)
      : name_(std::move(name)) {
    lines_.push_back("# xenos_conformance golden vectors: " + name_);
    lines_.push_back("# " + description);
    lines_.push_back(
        "# Generated from the Xenia REFERENCE ports only (tests/xenos_conformance/"
        "xenia_ref_*.hpp); regenerate with: xenos_conformance --regen");
  }
  void Comment(const std::string& c) { lines_.push_back("# " + c); }
  void Digest(const std::string& what, uint64_t count, uint64_t hash) {
    lines_.push_back(Fmt("@digest %s count=%" PRIu64 " fnv1a64=%016" PRIx64,
                         what.c_str(), count, hash));
  }
  void Row(const std::string& row) { lines_.push_back(row); }
  size_t RowCount() const { return lines_.size(); }

  // Writes or verifies the file and reports one golden.<name> line.
  void Finish(Results& res, const std::filesystem::path& dir, GoldenMode mode) {
    const auto path = dir / (name_ + ".tsv");
    std::string expected;
    for (const auto& l : lines_) expected += l + "\n";
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (mode == GoldenMode::Regen || (!exists && mode == GoldenMode::CreateMissing)) {
      std::filesystem::create_directories(dir, ec);
      std::ofstream f(path, std::ios::binary);
      f << expected;
      if (!f) {
        res.Report(Status::Fail, "golden." + name_,
                   "cannot write " + path.generic_string());
        return;
      }
      res.Report(Status::Info, "golden." + name_,
                 Fmt("%s %s (%zu lines, %zu bytes)",
                     exists ? "regenerated" : "created", path.generic_string().c_str(),
                     lines_.size(), expected.size()));
      return;
    }
    if (!exists) {
      res.Report(Status::Fail, "golden." + name_, "missing " + path.generic_string());
      return;
    }
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string actual = ss.str();
    actual.erase(std::remove(actual.begin(), actual.end(), '\r'), actual.end());
    if (actual == expected) {
      res.Report(Status::Pass, "golden." + name_,
                 Fmt("%zu lines match %s", lines_.size(), path.generic_string().c_str()));
      return;
    }
    // First differing line.
    std::vector<std::string> got;
    std::stringstream as(actual);
    for (std::string l; std::getline(as, l);) got.push_back(l);
    size_t i = 0;
    while (i < got.size() && i < lines_.size() && got[i] == lines_[i]) ++i;
    res.Report(Status::Fail, "golden." + name_,
               Fmt("reference output no longer matches %s at line %zu:\n"
                   "           file: %s\n           now:  %s",
                   path.generic_string().c_str(), i + 1,
                   i < got.size() ? got[i].c_str() : "<eof>",
                   i < lines_.size() ? lines_[i].c_str() : "<eof>"));
  }

 private:
  std::string name_;
  std::vector<std::string> lines_;
};

}  // namespace xc
