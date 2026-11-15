// XXH3-64 compiled for AVX2 (CMake: -mavx2, no PCH). The SAME function as the SSE2 build in
// gfx/shader_cache.cpp - only the vector width differs - selected at run time when the CPU
// has AVX2 (ShaderCache::hashUcode). The PM4 shared-memory page validation hashes ~36K 4 KB
// pages per gameplay frame on the command processor (8.6% of it, perf2 r_wc_prof).
#define XXH_INLINE_ALL
#define XXH_VECTOR XXH_AVX2
#include "thirdparty/xxhash.h"

#include <cstddef>
#include <cstdint>

namespace sr::gfx {
uint64_t srXxh3Avx2(const void* data, size_t bytes) { return XXH3_64bits(data, bytes); }
}  // namespace sr::gfx
