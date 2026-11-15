#pragma once
#include <cstddef>
#include <cstdint>

namespace sr::kernel {
// Tracks the SDK's actual Begin/End protocol, never a page's inferred lifetime.
// Call Begin only after its allocation result; it announces no completed write.
void resourceInlineBegin(uint32_t device, uint32_t allocation, uint64_t bytes);
void resourceInlineEnd(uint32_t device);
// Called after either implementation of the guest memcpy has finished. Only
// the exact copy inside a scoped DrawVerticesUP can complete an inline write.
void resourceInlineMemcpyComplete(uint32_t destination, uint32_t bytes);

class ResourceInlineUpScope {
 public:
  explicit ResourceInlineUpScope(uint32_t device);
  ~ResourceInlineUpScope();
  ResourceInlineUpScope(const ResourceInlineUpScope&) = delete;
  ResourceInlineUpScope& operator=(const ResourceInlineUpScope&) = delete;
  void finish();
 private:
  bool on_ = false;
  size_t depth_ = 0;
};
}  // namespace sr::kernel
