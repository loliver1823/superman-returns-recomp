#pragma once
#include "sr_oracle.hpp"
#if SR_ORACLE
#include <cstdint>
#include <memory>
#if defined(SR_DIFFTEST)
#include "../gfx/resource_reader_probe.hpp"
#endif
namespace sr::kernel::reader_observe {
bool enabled() noexcept;
uint32_t stream() noexcept;
uint64_t recording(uint32_t stream, std::weak_ptr<const void> identity, uint32_t ownerAllocationBytes) noexcept;
void submitted(uint64_t recording, const std::weak_ptr<const void>& identity, uint64_t ticket) noexcept;
void completion(uint32_t stream, uint64_t completed, bool valid) noexcept;
void gpuWrite(uint64_t recording, uint32_t phys, uint32_t bytes, uint32_t generation) noexcept;
void nativeReadExecuted(uint32_t source, uint32_t destination, uint32_t bytes) noexcept;
void textureLock(uint32_t texture, uint32_t level, uint32_t output, uint32_t flags) noexcept;
void lockIntent(uint32_t resource, uint32_t reason, uint32_t address, uint32_t bytes, uint32_t flags) noexcept;
// The same lock after the lock core's body (the fence wait done, the pointer handed out): graded over its range.
void lockGraded(uint32_t resource, uint32_t reason, uint32_t address, uint32_t bytes, uint32_t flags) noexcept;
void preCoherency(uint32_t status, uint32_t phys, uint32_t bytes) noexcept;
void report(const char* why) noexcept;
#if defined(SR_DIFFTEST)
sr::gfx::reader_probe::Counts snapshotForTest();
#endif
}
#endif
