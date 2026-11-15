#pragma once
// Step 4 item 7d (lane D, 2026-10-20; SR_MEM_SITES=1, diagnostic, default OFF): NAMES for the big host allocations.
//
// The memory census (perf_stats memmap_NNN.txt, tools/perf/mem_census.py) labels only the guest space; the release
// exe's other blocks >= 64 MB (512 / 512, 512 / 0, 192 / 192 x 2, 128 / 128 x 2 committed / resident MB) are "-". A
// MemSiteScope around an allocation site walks the process's address space (VirtualQuery) when it opens and when it
// closes and logs each allocation of >= 16 MB committed that appeared in between, once, under the innermost open
// scope's name: `[MEM_SITE] <name>: +<MB> MB <private|mapped> at <base> appeared during this scope`. One census roll
// with the gate then names every unlabeled block by its base address. An INTERVAL OBSERVATION, not ownership (Astra
// 10:2x P3): the walk is process-wide, so another thread's allocation inside the interval counts too - a name is a
// lead to confirm at the allocation site (its size and alignment), not proof. A base freed and reused, or grown by
// >= 16 MB, is named again. Off: one cached bool test.
namespace sr::kernel {

bool memSitesOn();

class MemSiteScope {
 public:
  explicit MemSiteScope(const char* name);
  ~MemSiteScope();
  MemSiteScope(const MemSiteScope&) = delete;
  MemSiteScope& operator=(const MemSiteScope&) = delete;

 private:
  const char* m_name = nullptr;
  void* m_before = nullptr;   // the allocation bases seen at open (an owned set; nullptr when off)
};

}  // namespace sr::kernel
