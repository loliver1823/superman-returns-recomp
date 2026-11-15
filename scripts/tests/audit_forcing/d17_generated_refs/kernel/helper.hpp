#pragma once
#include <cstdint>
namespace sr { namespace kernel {
void headerOnly(uint32_t va);
void neverCalled(uint32_t va);
inline void headerInline(uint32_t va) { headerOnly(va); }
} }
