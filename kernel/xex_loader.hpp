#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sr {
namespace kernel {

// Load xex image into guest memory. Reads xex or .img; fills outLoadAddress, outImageSize, outEntryPoint.
// Returns true if image was loaded. outEntryPoint is xex optional header ENTRY_POINT (0 if not found).
bool loadXexImage(const char* path, uint8_t* destBase, uint32_t destSize,
                  uint32_t& outLoadAddress, uint32_t& outImageSize, uint32_t& outEntryPoint);

// iter-465 window: the title's XDBF resource database (the SPA: title name, achievements,
// images incl. the title icon), copied from the loaded image by loadXexImage. False until the
// image has loaded or when the XEX has no XDBF resource. Thread-safe (written once).
bool xexTitleDatabase(std::vector<uint8_t>& out);

}  // namespace kernel
}  // namespace sr
