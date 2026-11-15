#pragma once
// iter-465 physmem PLATFORM FIX: the kernel's per-thread memory where Xenia puts it.
//
// SR_THREAD_MEM_XENIA (default ON since the iter-465 rolls; =0 restores the old placement). Needs
// SR_NTAVM_XENIA_HEAP (the Xenia heaps, kernel/ntavm_xenia_heap.hpp); off when that is off.
//
// THE DEFECT (archive/v1_physmem, the fortress lane's nomodsAB1 roll): every guest thread's
// 256 KB stack came from the PHYSICAL allocator (guestPhysAlloc, iter-388 SR_STACK_PHYS) and
// was never freed. The allocator places grants top-down, first fit, into the freed space. Every
// cutscene movie starts four MoviePipeline worker threads (0x8247F9D0 / FA00 / FA30 / FA60,
// created suspended), so the movies played while the game leaves Warworld put 4 x 256 KB stacks
// at the top of the freed 224 MB world pool (0xAD040000..0xBB030000, stacks at 0xBAF36000..
// 0xBB036010 and 0xB8580000..0xB8680000). Back in Metropolis the game asks for the world pool again
// (MmAllocatePhysicalMemoryEx 0xDFF0000); no 224 MB run was left (0x100E9000 bytes free in
// pieces), [PHYS_CAP] refused it and the FM worker post called through the null result
// ([ICALL_FAIL] 8260527C). Nobody could play past Warworld.
//
// Xenia (the oracle; reference/xenia-canary, BSD-3, ported placement):
//   XThread::AllocateStack (xthread.cc:273-299): the v40000000 heap, AllocRange(0x70000000,
//     0x7F000000, round_up(size, 64 KB) + 2 guard pages, 64 KB alignment, bottom-up);
//     XThread::FreeStack releases it when the thread object dies (~XThread).
//   The PCR and the KTHREAD: Memory::SystemHeapAlloc (memory.cc:689) -> the v00000000
//     heap's system region [0x30000000, 0x40000000) (BaseHeap::AllocSystemHeap), 4 KB pages.
// None of them is physical memory; MmAllocatePhysicalMemoryEx serves the title alone.
//
// Here: stacks come from v40000000 [0x70000000, 0x7F000000) (a 64 KB guard below and above,
// so stackVA = the stack limit, the top = stackVA + size) and go back when the thread ends
// (its function returns, it faults, or ExTerminateThread). The KTHREAD / KPCR blocks keep
// their fixed arenas; when an arena is full the next block comes from the system region
// instead of guestPhysAlloc (never freed - the arenas' lifetime policy).
// Kernel-only header (never include from guest/, never add to guest_context.hpp).
#include <cstdint>

namespace sr::kernel::threadmem {

constexpr uint32_t kStackLo = 0x70000000u, kStackHi = 0x7F000000u;   // xthread.h:387-388
constexpr uint32_t kStackGuard = 0x10000u;                          // one v40000000 page
constexpr uint32_t kSystemLo = 0x30000000u, kSystemHi = 0x40000000u;

bool enabled();                       // SR_THREAD_MEM_XENIA && xheap::enabled()
void forceEnabledForTest(int mode);   // -1 env, 0 off, 1 on

// A stack of `size` bytes: returns the stack LIMIT (lowest usable VA, 0 = none) and the
// allocation base for releaseStack (*allocBase). The memory is zeroed (Xenia commits fresh
// pages). Does not consult enabled(); the caller decides.
uint32_t allocStack(uint32_t size, uint32_t* allocBase);
void releaseStack(uint32_t allocBase);

// The calling host thread runs on the stack whose allocation base is `allocBase` (0 = none);
// releaseCurrentStack() gives it back (once) when the thread ends.
void bindCurrentStack(uint32_t allocBase);
void releaseCurrentStack();

// A system-heap block (KTHREAD / KPCR overflow): zeroed, 4 KB granular; 0 = none.
uint32_t allocSystem(uint32_t size);

struct Stats {
  uint64_t stacks = 0, stackReleases = 0, stackFailures = 0, systemBlocks = 0;
  uint32_t liveStacks = 0;
};
Stats stats();

}  // namespace sr::kernel::threadmem
