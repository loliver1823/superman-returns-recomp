// ntavm_xenia_heap.hpp - Xenia's two guest-virtual heaps for kernel-chosen
// NtAllocateVirtualMemory (iter-463 D-AUDIO-CMDBUF root cause).
//
// Xenia (memory.cc:211-214, xboxkrnl_memory.cc:111-183) serves an
// NtAllocateVirtualMemory with BaseAddress == 0 from
//   v00000000  [0x00000000, 0x40000000)  4 KB pages   (no X_MEM_LARGE_PAGES)
//   v40000000  [0x40000000, 0x7F000000)  64 KB pages  (X_MEM_LARGE_PAGES)
// bottom-up first fit (X_MEM_TOP_DOWN: top-down), base aligned to the heap's page
// size, size rounded up to 64 KB, the first 64 KB of v00000000 reserved
// (protect_zero). So every title arena the kernel places is a POSITIVE int32.
// Ours came from the 0x8xxxxxxx bump; EA's ParseTagStreamAndAlignData_sub_82330CA8
// aligns the SNR header with a SIGNED `p % 16`, which is never in 1..15 for a
// negative p, so the header stays unaligned on our addresses and the play request
// is misparsed (see docs/ENGINE_MAP.md s7a).
//
// Default ON (iter-463, after the cmdbuf2 rolls); SR_NTAVM_XENIA_HEAP=0 restores the
// legacy 0x8xxxxxxx bump for the A/B.
#pragma once
#include <cstdint>

namespace sr::kernel::xheap {

constexpr uint32_t kV00Base = 0x00000000u, kV00End = 0x40000000u, kV00Page = 0x1000u;
constexpr uint32_t kV40Base = 0x40000000u, kV40End = 0x7F000000u, kV40Page = 0x10000u;
constexpr uint32_t kZeroGuard = 0x10000u;     // protect_zero: [0, 0x10000) never handed out
constexpr uint32_t kSizeRound = 0x10000u;     // base == 0: round_up(size, 64 KB)

bool enabled();

// Allocate `size` bytes (rounded to 64 KB) from v00000000 (large=false) or
// v40000000 (large=true). Returns 0 when the heap is full. *grantOut = rounded size.
uint32_t alloc(uint32_t size, bool large, bool topDown, uint32_t* grantOut);

// iter-465 physmem: Xenia BaseHeap::AllocRange (memory.cc) inside [lo, hi) of one
// heap (large=false: v00000000, 4 KB pages; large=true: v40000000, 64 KB pages).
// The size is rounded up to the heap's page, the base aligned to max(align, page),
// first fit bottom-up (topDown: highest fit). The callers are the kernel's own
// objects Xenia places here: thread stacks (XThread::AllocateStack, v40000000
// [0x70000000, 0x7F000000), bottom-up) and system-heap blocks (Memory::
// SystemHeapAlloc -> BaseHeap::AllocSystemHeap, v00000000 [0x30000000,
// 0x40000000), bottom-up). Works whether or not enabled() (the gate governs the
// NtAllocateVirtualMemory route only); returns 0 when no gap fits.
uint32_t allocRange(uint32_t lo, uint32_t hi, uint32_t size, uint32_t align, bool large,
                    bool topDown, uint32_t* grantOut);

// MEM_RELEASE of a region this heap granted (exact base). Returns its size, 0 if
// `base` is not the base of a live region here.
uint32_t release(uint32_t base);

// True when va lies inside a live region of either heap.
bool owns(uint32_t va);

// True when va lies inside either heap's address range (granted or not).
inline bool inRange(uint32_t va) { return va >= kZeroGuard && va < kV40End; }

// Test hooks.
void resetForTest();
void forceEnabledForTest(int mode);   // -1 env, 0 off, 1 on
uint32_t liveRegionsForTest();

}  // namespace sr::kernel::xheap
