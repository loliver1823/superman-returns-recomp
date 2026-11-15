#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "kernel/native_record_format.hpp"

namespace sr::gfx {

struct NativeRecordStats {
  uint64_t runs = 0, draws = 0, packets = 0, bad = 0, drawTsc = 0;
};

// Implementation boundary for native render services. Validate an operation
// before mutating it; false stops consumption. Earlier records may already have
// completed, so failure is never permission to replay the stream via a fallback.
// Hardware interpretation and CHECK reconstruction belong in a separate
// development adapter, never in this executor.
class NativeRenderContext {
 public:
  virtual ~NativeRenderContext() = default;
  virtual bool predicatePasses() const = 0;
  virtual void predicatedSkipped() = 0;
  virtual bool stateRun(uint32_t first, std::span<const uint32_t> valuesBE, bool checkDedup) = 0;
  virtual bool draw(bool predicated, uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize) = 0;
  virtual bool command(sr::native::flush::RecKind kind, uint32_t n, std::span<const uint32_t> payload) = 0;
  virtual uint64_t drawTimestamp() const = 0;
};

using NativeOpaqueHandler = bool (*)(sr::native::flush::RecKind kind, std::span<const uint32_t> payload,
                                    NativeRenderContext& context);

// Configure before consumer threads start. A resolver may supply an externally
// owned handler for the specific kind being consumed. No PM4 renderer or engine
// dependency is registered here; oracle reconstruction has a separate registry.
class NativeRecordHandlers {
 public:
  using Resolver = NativeOpaqueHandler (*)(uint32_t kind);
  explicit NativeRecordHandlers(Resolver resolver = nullptr) : resolver_(resolver) {}
  bool set(uint32_t kind, NativeOpaqueHandler handler);
  NativeOpaqueHandler get(uint32_t kind) const;

 private:
  std::array<NativeOpaqueHandler, sr::native::flush::kRkKinds> handlers_{};
  Resolver resolver_ = nullptr;
};

// Words and scalar payload fields are host order; state/constant/shader arrays
// retain their big-endian byte representation. Validate every payload before
// reading it. Failure stops at the offending record, with no packet fallback.
bool executeNativeRecords(std::span<const uint32_t> words, NativeRenderContext& context,
                          const NativeRecordHandlers& handlers, NativeRecordStats& stats, bool timing);

}  // namespace sr::gfx
