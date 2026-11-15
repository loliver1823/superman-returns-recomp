#include "native_record_executor.hpp"

namespace sr::gfx {

bool NativeRecordHandlers::set(uint32_t kind, NativeOpaqueHandler handler) {
  if (!sr::native::flush::recOpaque(kind)) return false;
  handlers_[kind] = handler;
  return true;
}

NativeOpaqueHandler NativeRecordHandlers::get(uint32_t kind) const {
  if (!sr::native::flush::recOpaque(kind)) return nullptr;
  return handlers_[kind] ? handlers_[kind] : resolver_ ? resolver_(kind) : nullptr;
}

bool executeNativeRecords(std::span<const uint32_t> words, NativeRenderContext& context,
                          const NativeRecordHandlers& handlers, NativeRecordStats& stats, bool timing) {
  namespace nf = sr::native::flush;
  auto reject = [&] {
    ++stats.bad;
    return false;
  };
  while (!words.empty()) {
    const uint32_t tag = words.front();
    words = words.subspan(1);
    if (!(tag & 0x80000000u)) {
      const uint32_t count = (tag >> 16) & 0x3FFFu;
      if (count > words.size()) return reject();
      if (!context.stateRun(tag & 0x7FFFu, words.first(count), (tag & 0x40000000u) != 0)) return reject();
      words = words.subspan(count);
      ++stats.runs;
      continue;
    }

    const uint32_t kind = (tag >> 24) & 0x7Fu, n = (tag >> 8) & 0xFFFFu;
    // A skipped predicate must never conceal a legacy packet escape hatch.
    if (kind == nf::kRkPm4) return reject();
    const uint32_t count = nf::recWords(kind, n);
    if (!count || count > words.size()) return reject();
    const auto payload = words.first(count);
    words = words.subspan(count);
    const bool predicated = (tag & 2u) != 0;
    if (kind == nf::kRkDraw) {
      const uint64_t before = timing ? context.drawTimestamp() : 0;
      if (predicated && !context.predicatePasses()) {
        context.predicatedSkipped();
      } else if (!context.draw(predicated, payload[0], payload[1], payload[2])) {
        return reject();
      }
      if (timing) stats.drawTsc += context.drawTimestamp() - before;
      ++stats.draws;
      continue;
    }
    ++stats.packets;
    const NativeOpaqueHandler handler = nf::recOpaque(kind) ? handlers.get(kind) : nullptr;
    if (nf::recOpaque(kind) && !handler) return reject();
    if (predicated && !context.predicatePasses()) {
      context.predicatedSkipped();
      continue;
    }
    if (nf::recOpaque(kind)) {
      if (!handler(static_cast<nf::RecKind>(kind), payload, context)) return reject();
    } else if (!context.command(static_cast<nf::RecKind>(kind), n, payload)) {
      return reject();
    }
  }
  return true;
}

}  // namespace sr::gfx
