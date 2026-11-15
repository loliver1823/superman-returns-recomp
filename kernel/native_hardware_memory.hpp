#pragma once
#include <cstddef>
#include <cstdint>

namespace sr::kernel {
// CPU MMIO aperture (Xenia memory.cc / GraphicsSystem::Setup). Below it,
// [0x40000000, 0x7F000000) is ordinary virtual RAM and [0x7F000000, 0x7FC80000)
// is GPU writeback/XPS RAM. The old fake-EDRAM comment at 0x7E800000 described
// an invalid CPU mapping: protecting it also protected real top-down heaps.
inline constexpr uint32_t kHardwareMemoryBase = 0x7FC80000u;
inline constexpr size_t kHardwareMemoryBytes = 0x00380000u;

// Called before guest execution in release builds. Failure must stop startup;
// arithmetic-admission consumers are excluded before protection is published.
bool protectNativeHardwareMemory(uint8_t* base, size_t size, uint32_t* error) noexcept;

// Once protected, diagnostics must not make any part of this reservation
// readable/writable again. Host arithmetic only; safe inside a VEH.
bool nativeHardwareMemoryIntersects(const void* address, size_t bytes) noexcept;
}
