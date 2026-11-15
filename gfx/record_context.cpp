// The NativeRenderContext over the record engine (C1, docs/RELEASE_COMPOSITION.md 2.1, stage 2a; was the "development
// adapter" gfx/pm4_native_record_adapter.cpp, which bridged the record executor to the PM4 engine). Every record is
// validated here before it mutates anything - register ranges, constant ranges, payload sizes - and applied through
// RecordEngine's apply methods; the opaque kinds (strand B's op lists, the transport's blocks) through the handler their
// owner registered (RecordEngine::opaqueKnown / opaqueApply). A literal-PM4 record (kRkPm4) never reaches this context:
// gfx/native_record_executor.cpp rejects it (a native segment with a PM4 gap means a producer was not converted). The
// development oracle's parts - the constant-dedup CHECK of a bit-30 run, kept beside the shipping path - are #if SR_ORACLE.
#include "record_engine.hpp"
#include "kernel/sr_oracle.hpp"
#if SR_ORACLE
#include "kernel/native_const_dedup.hpp"
#endif
#include <intrin.h>
#include <iterator>

namespace sr::gfx {

bool (*RecordEngine::opaqueKnown)(uint32_t) = nullptr;
bool (*RecordEngine::opaqueApply)(RecordEngine&, uint32_t, const uint32_t*, uint32_t) = nullptr;

namespace {
namespace nf = sr::native::flush;

bool registerRange(uint32_t first, uint32_t count) {
  return first <= XenosRegisterFile::kNumRegisters && count <= XenosRegisterFile::kNumRegisters - first;
}

bool constantRange(uint32_t offsetType, uint32_t count, bool memoryLoad = false) {
  const uint32_t type = (offsetType >> 16) & 0xFFu;
  const uint32_t bases[] = {0x4000u, 0x4800u, 0x4900u, 0x4908u, 0x2000u};
  if (!memoryLoad && type >= std::size(bases)) return false;
  const uint32_t base = memoryLoad && type > 3u ? bases[0] : bases[type];
  return registerRange(base + (offsetType & 0x7FFu), count);
}

class RecordContext final : public NativeRenderContext {
 public:
  explicit RecordContext(RecordEngine& engine) : engine_(engine) {}
  bool predicatePasses() const override { return (engine_.binSelect & engine_.binMask) != 0; }
  void predicatedSkipped() override { ++engine_.predSkipped; }
  uint64_t drawTimestamp() const override { return __rdtsc(); }
  bool stateRun(uint32_t first, std::span<const uint32_t> values, bool dedup) override {
    if (engine_.nativeRecordFailed) return false;
    if (!registerRange(first, uint32_t(values.size()))) return false;
    const auto* bytes = reinterpret_cast<const uint8_t*>(values.data());
#if SR_ORACLE
    if (dedup) sr::native::cdedup::cpCheck(bytes, first, uint32_t(values.size()), getXenosRegisters().mutableData());
#else
    (void)dedup;  // a dedup CHECK run (bit 30) is an ordinary run without the development checker
#endif
    engine_.nativeRun(first, uint32_t(values.size()), bytes);
    return !engine_.nativeRecordFailed;
  }
  bool draw(bool predicated, uint32_t initiator, uint32_t base, uint32_t size) override {
    if (engine_.nativeRecordFailed) return false;
    engine_.nativeDraw(predicated, initiator, base, size);
    return !engine_.nativeRecordFailed;
  }
  bool command(nf::RecKind kind, uint32_t n, std::span<const uint32_t> words) override {
    if (engine_.nativeRecordFailed) return false;
    if (words.size() != nf::recWords(kind, n)) return false;
    switch (kind) {
      case nf::kRkShaderPtr:
        if ((words[1] & 0xFFFFu) > 4096u) return false;
        ++engine_.opcodeHist[0x27];
        engine_.applyImLoad(words[0], words[1]);
        break;
      case nf::kRkShaderImm:
        if ((words[1] & 0xFFFFu) > n) return false;
        ++engine_.opcodeHist[0x2B];
        engine_.applyImLoadImmediate(words[0] & 3u, words[1], reinterpret_cast<const uint8_t*>(words.data() + 2), n + 2u);
        break;
      case nf::kRkConstLoad:
        if (words[2] >= 4096u || !constantRange(words[1], words[2], true)) return false;
        ++engine_.opcodeHist[0x2F];
        engine_.applyLoadAluConstant(words[0], words[1], words[2]);
        break;
      case nf::kRkSetConst:
        if (!constantRange(words[0], n)) return false;
        ++engine_.opcodeHist[0x2D];
        engine_.applySetConstant(words[0], reinterpret_cast<const uint8_t*>(words.data() + 1), n);
        break;
      case nf::kRkBinReg:
        ++engine_.opcodeHist[0x60u + (n & 3u)];
        engine_.applyBinReg(n & 3u, words[0]);
        break;
      case nf::kRkEvent:
        ++engine_.opcodeHist[0x46];
        engine_.applyEventWrite(words[0]);
        break;
      case nf::kRkZpd:  // p2.zpd's bake: EVENT_WRITE_ZPD {initiator} - the host occlusion query (applyZpdEvent)
        ++engine_.opcodeHist[0x5B];
        engine_.applyZpdEvent(words[0], true);
        break;
      case nf::kRkFence:
        engine_.opcodeHist[0x58] += 2;
        engine_.applyEventWriteShd(3u, words[0], words[1]);
        engine_.applyEventWriteShd(3u, words[2], words[3]);
        break;
      case nf::kRkInvalidate:
        ++engine_.opcodeHist[0x3C];
        engine_.applyCoherency(words[0], words[1], words[2] != 0u);
        break;
      case nf::kRkRegRmw:
        ++engine_.opcodeHist[0x21];
        engine_.applyRegRmw(words[0], words[1], words[2]);
        break;
      case nf::kRkEventShd:
        ++engine_.opcodeHist[0x58];
        engine_.applyEventWriteShd(words[0], words[1], words[2]);
        break;
      default:
        if (!nf::recOpaque(kind)) return false;
        // The memory-backed constant payload also needs a bounded destination
        // before the existing helper can write it. Other opaque handlers retain
        // their existing mapping/lifetime prerequisites in this context.
        if (kind == nf::kRkSetConstMem &&
            (n != 3u || !constantRange(words[1], (words[0] >> 16) & 0x3FFFu))) return false;
        if (RecordEngine::opaqueApply && RecordEngine::opaqueKnown && RecordEngine::opaqueKnown(kind)) {
          const bool complete = RecordEngine::opaqueApply(engine_, kind, words.data(), n);
          return complete && !engine_.nativeRecordFailed;
        }
        return false;
    }
    return !engine_.nativeRecordFailed;
  }
 private:
  RecordEngine& engine_;
};

bool opaqueBridge(nf::RecKind kind, std::span<const uint32_t> words, NativeRenderContext& context) {
  return context.command(kind, uint32_t(words.size()), words);
}
NativeOpaqueHandler resolveOpaque(uint32_t kind) {
  // Resolve only the kind being consumed. Producers lazily register helpers
  // before publishing their record; scanning every slot could race an unrelated
  // producer initializing a different helper.
  return RecordEngine::opaqueKnown && RecordEngine::opaqueKnown(kind) ? opaqueBridge : nullptr;
}
}  // namespace

bool RecordEngine::nativeRecord(const uint32_t* begin, const uint32_t* end, NativeRecordStats& stats, bool timing) {
  if (nativeRecordFailed) return false;
  if (begin == end) return true;
  RecordContext context(*this);
  const NativeRecordHandlers handlers(resolveOpaque);
  const bool complete = executeNativeRecords({begin, size_t(end - begin)}, context, handlers, stats, timing) && !nativeRecordFailed;
  if (!complete) nativeRecordFailed = true;
  return complete;
}
}  // namespace sr::gfx
