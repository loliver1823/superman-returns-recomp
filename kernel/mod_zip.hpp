#pragma once
// iter-464 modcore: reading .zip mods (docs/MODS.md section 2). A zip mod is extracted once
// into <content root>/mods/.cache/zip/<id>-<size>-<mtime>/ and then read like a folder mod.
// Methods: stored (0) and deflate (8, RFC 1951 - our own inflater, no third-party source);
// CRC-32 checked; encrypted entries, zip64 and unsafe paths ('..', ':', absolute) are refused.
// Kernel-only header.
#include <cstdint>
#include <string>
#include <vector>

namespace sr {
namespace kernel {

struct ZipMember {
  std::string name;       // as stored ('/'-separated)
  uint16_t method = 0, flags = 0;
  uint32_t crc = 0;
  uint64_t csize = 0, usize = 0, localOffset = 0;
};

bool zipList(const std::string& zipPath, std::vector<ZipMember>* out, std::string* err);
// The mod root inside the zip: "" when mod.json or any file sits at the zip root (or there
// are several top-level names), else "<the single top folder>/" (tools/mods/srmod.py _zip_root).
std::string zipModRoot(const std::vector<ZipMember>& members);
// Extracts every file under the mod root into dstDir (created). False + *err on any error
// (the caller deletes the partial folder).
bool zipExtractMod(const std::string& zipPath, const std::string& dstDir, std::string* err);
// Raw DEFLATE (RFC 1951) of a whole buffer (tests).
bool inflateBuffer(const uint8_t* in, size_t n, std::vector<uint8_t>* out, std::string* err);
uint32_t crc32Update(uint32_t crc, const uint8_t* p, size_t n);

}  // namespace kernel
}  // namespace sr
