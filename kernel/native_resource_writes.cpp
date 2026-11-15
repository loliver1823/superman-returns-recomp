#include "native_resource_writes.hpp"
#include "../gfx/resource_memory.hpp"
#include <stdexcept>
#include <vector>

namespace sr::kernel {
namespace {
struct Pending {
  uint32_t device = 0, allocation = 0, bytes = 0, matches = 0;
  bool began = false;
};
thread_local Pending raw;
thread_local std::vector<Pending> up;
[[noreturn]] void invalid(const char* reason) {
  throw std::logic_error(reason);
}
void complete(Pending& pending) {
  gfx::memres::resourceMemory().noteInlineWrite(pending.allocation, pending.bytes);
  ++pending.matches;
}
}  // namespace

void resourceInlineBegin(uint32_t device, uint32_t allocation, uint64_t bytes) {
  if (!gfx::memres::resourceMode()) return;
  Pending& pending = up.empty() ? raw : up.back();
  if (pending.began || (!up.empty() && pending.device != device))
    invalid("native inline Begin is nested or belongs to another device");
  if (bytes > UINT32_MAX || (bytes & 3u) ||
      (allocation && (allocation < 0x80000000u || uint64_t(allocation) + bytes > 0x100000000ull ||
                      uint64_t(gfx::memres::resourcePhysOfVa(allocation)) + bytes > gfx::memres::kPhysBytes)))
    invalid("native inline Begin has an invalid allocation extent");
  pending = {device, allocation, uint32_t(bytes), 0, true};
  // A failed raw Begin has no End and must not leave a stale latch.
  if (up.empty() && !allocation) pending = {};
}

void resourceInlineEnd(uint32_t device) {
  if (!gfx::memres::resourceMode()) return;
  if (!up.empty() || !raw.began || !raw.allocation || raw.device != device)
    invalid("native inline End has no matching completed allocation");
  complete(raw);  // The caller's writes precede End; its cursor store follows.
  raw = {};
}

void resourceInlineMemcpyComplete(uint32_t destination, uint32_t bytes) {
  if (!gfx::memres::resourceMode() || up.empty()) return;
  Pending& pending = up.back();
  if (!pending.began) return;  // A helper inside Begin has not returned its allocation yet.
  if (!pending.allocation || destination != pending.allocation || bytes != pending.bytes || pending.matches)
    invalid("native DrawVerticesUP copy is missing, repeated or mismatched");
  complete(pending);  // Before memcpy returns to the SDK's cursor publication.
}

ResourceInlineUpScope::ResourceInlineUpScope(uint32_t device) : on_(gfx::memres::resourceMode()) {
  if (!on_) return;
  depth_ = up.size();
  up.push_back(Pending{device});
}
ResourceInlineUpScope::~ResourceInlineUpScope() {
  if (on_) up.resize(depth_);  // Also discards an exceptional, unpublished operation.
}
void ResourceInlineUpScope::finish() {
  if (!on_) return;
  if (up.size() != depth_ + 1u || !up.back().began ||
      up.back().matches != (up.back().allocation ? 1u : 0u))
    invalid("native DrawVerticesUP returned without its exact completed copy");
}
}  // namespace sr::kernel
