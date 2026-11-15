#include "xex_loader.hpp"
#include "../app/log.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <stdio.h>
#endif

namespace sr {
namespace kernel {

namespace {
  static inline uint32_t readBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
  }
  static inline uint16_t readBE16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
  }

  constexpr uint32_t kXexFileFormatInfo = 0x000003FFu;
  constexpr uint32_t kXexEntryPoint = 0x00010100u;
  constexpr uint16_t kXexCompressionNone = 0;
  constexpr uint16_t kXexEncryptionNone = 0;
  constexpr uint32_t kXexOptHeaderTable = 24;  // optional-header table follows the 24-byte xex2_header (Xenia xex2_info.h)
  constexpr uint32_t kXexResourceInfo = 0x000002FFu;  // XEX_HEADER_RESOURCE_INFO

  // iter-465 window: the title's XDBF resource database (Xenia emulator.cc reads the same
  // resource - named with the title id - for the window icon). Written once on the guest
  // thread right after the image load, published with a release store; never mutated after.
  std::vector<uint8_t> g_titleXdbf;
  std::atomic<bool> g_titleXdbfReady{false};

  // Copy the first resource whose bytes are an XDBF database. The resource table lives in the
  // plaintext XEX header (key 0x2FF -> {u32 size; {char name[8]; u32 va; u32 size}[]}); the
  // bytes live in the loaded image at their VA. Read-only: the guest image is not touched.
  void captureTitleResource(const uint8_t* data, size_t toRead, uint32_t headerCount,
                            const uint8_t* destBase, uint32_t loadAddress, uint32_t imageSize) {
    if (g_titleXdbfReady.load(std::memory_order_acquire)) return;
    const uint8_t* optBase = data + kXexOptHeaderTable;
    for (uint32_t i = 0; i < headerCount && kXexOptHeaderTable + (i + 1) * 8u <= toRead; i++) {
      const uint8_t* opt = optBase + i * 8;
      if (readBE32(opt) != kXexResourceInfo) continue;
      const uint32_t off = readBE32(opt + 4);
      if (off + 4u > toRead) return;
      const uint32_t total = readBE32(data + off);
      const uint32_t count = total >= 4 ? (total - 4) / 16 : 0;
      for (uint32_t r = 0; r < count && off + 4u + (r + 1) * 16u <= toRead; r++) {
        const uint8_t* ent = data + off + 4 + r * 16;
        const uint32_t va = readBE32(ent + 8);
        const uint32_t sz = readBE32(ent + 12);
        if (va < loadAddress || sz < 24 || sz > (4u << 20) ||
            uint64_t(va) + sz > uint64_t(loadAddress) + imageSize)
          continue;
        const uint8_t* src = destBase + va;
        if (std::memcmp(src, "XDBF", 4) != 0) continue;
        g_titleXdbf.assign(src, src + sz);
        g_titleXdbfReady.store(true, std::memory_order_release);
        SR_LOG_KERNEL_IMPORT("xex_loader: title resource '%.8s' (XDBF, %u bytes at VA 0x%08X)",
                             reinterpret_cast<const char*>(ent), sz, va);
        return;
      }
      return;
    }
  }
}

bool xexTitleDatabase(std::vector<uint8_t>& out) {
  if (!g_titleXdbfReady.load(std::memory_order_acquire)) return false;
  out = g_titleXdbf;
  return true;
}

static uint32_t getXexEntryPoint(const uint8_t* data, size_t toRead, uint32_t headerCount) {
  const uint8_t* optBase = data + kXexOptHeaderTable;
  for (uint32_t i = 0; i < headerCount && kXexOptHeaderTable + (i + 1) * 8u <= toRead; i++) {
    const uint8_t* opt = optBase + i * 8;
    uint32_t key = readBE32(opt);
    if (key != kXexEntryPoint) continue;
    return readBE32(opt + 4);
  }
  return 0;
}

bool loadXexImage(const char* path, uint8_t* destBase, uint32_t destSize,
                  uint32_t& outLoadAddress, uint32_t& outImageSize, uint32_t& outEntryPoint) {
  outEntryPoint = 0;
  if (!path || !destBase || destSize == 0) return false;

  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    SR_LOG_KERNEL_IMPORT("xex_loader: cannot open %s", path);
    return false;
  }
  std::streamsize fileSize = f.tellg();
  f.seekg(0);
  const size_t kMaxRead = 32 * 1024 * 1024;
  size_t toRead = static_cast<size_t>(std::min(fileSize, static_cast<std::streamsize>(kMaxRead)));
  std::vector<uint8_t> buf(toRead);
  if (!f.read(reinterpret_cast<char*>(buf.data()), toRead)) return false;

  const uint8_t* data = buf.data();
  if (toRead < 64 || std::memcmp(data, "XEX2", 4) != 0) {
    SR_LOG_KERNEL_IMPORT("xex_loader: not XEX2 or too small");
    return false;
  }

  // Xex2Header: magic(4), moduleFlags(4), headerSize(4), reserved(4), securityOffset(4), headerCount(4)
  uint32_t headerSize = readBE32(data + 8);
  uint32_t securityOffset = readBE32(data + 16);
  uint32_t headerCount = readBE32(data + 20);

  if (toRead < securityOffset + 32u) return false;
  const uint8_t* sec = data + securityOffset;
  // Xex2SecurityInfo: headerSize(4), imageSize(4), ...
  uint32_t imageSize = readBE32(sec + 4);
  uint32_t loadAddress = readBE32(sec + 24);  // loadAddress at offset 0x18
  
  // The recompiled code expects the image at PPC_IMAGE_BASE (0x82000000).
  // If the XEX's embedded load address differs, override it to match the recompiler.
  // This is necessary because XenonRecomp generates code with hardcoded addresses.
  constexpr uint32_t kExpectedLoadAddress = 0x82000000u;  // Must match PPC_IMAGE_BASE
  if (loadAddress != kExpectedLoadAddress) {
    SR_LOG_KERNEL_IMPORT("xex_loader: overriding XEX load address 0x%08X -> 0x%08X (recompiler expects PPC_IMAGE_BASE)",
                         loadAddress, kExpectedLoadAddress);
    loadAddress = kExpectedLoadAddress;
  }

  // Optional headers: after the 24-byte Xex2Header, each 8 bytes (key, value/offset).
  // FILE_FORMAT_INFO -> {u32 size; u16 encryption; u16 compression} (Xenia xex2_opt_file_format_info).
  const uint8_t* optBase = data + kXexOptHeaderTable;
  uint16_t compType = 0xFFFFu;
  uint16_t encType = 0xFFFFu;
  for (uint32_t i = 0; i < headerCount && kXexOptHeaderTable + (i + 1) * 8u <= toRead; i++) {
    const uint8_t* opt = optBase + i * 8;
    uint32_t key = readBE32(opt);
    if (key != kXexFileFormatInfo) continue;
    uint32_t off = readBE32(opt + 4);
    if (off + 8u <= toRead) {
      encType = readBE16(data + off + 4);
      compType = readBE16(data + off + 6);
    }
    break;
  }

  outEntryPoint = getXexEntryPoint(data, toRead, headerCount);

  // The raw copy below is only valid for a plain image; anything else needs XenonDecompress's .img.
  if (compType != kXexCompressionNone || encType != kXexEncryptionNone) {
    SR_LOG_KERNEL_IMPORT("xex_loader: xex is compressed/encrypted (compression %u, encryption %u), trying .img fallback",
                         compType, encType);
    // XenonDecompress writes to <input.xex>.img (e.g. default.xex.img). Try that first, then <name>.img.
    std::string imgPathA = std::string(path) + ".img";
    std::string imgPathB = path;
    size_t dot = imgPathB.find_last_of('.');
    if (dot != std::string::npos) imgPathB.resize(dot);
    imgPathB += ".img";
    for (const std::string* imgPath : { &imgPathA, &imgPathB }) {
      if (*imgPath == imgPathB && imgPathB == imgPathA) continue;  // same path, skip duplicate
      std::ifstream img(*imgPath, std::ios::binary | std::ios::ate);
      if (img) {
        std::streamsize fsize = img.tellg();
        if (fsize == static_cast<std::streamsize>(imageSize)) {
          img.seekg(0);
          uint64_t destOffset = loadAddress;
          if (destOffset + imageSize <= destSize) {
            uint8_t* destPtr = destBase + destOffset;
            if (img.read(reinterpret_cast<char*>(destPtr), imageSize)) {
              outLoadAddress = loadAddress;
              outImageSize = imageSize;
              captureTitleResource(data, toRead, headerCount, destBase, loadAddress, imageSize);
              SR_LOG_KERNEL_IMPORT("xex_loader: loaded %u bytes at VA 0x%08X from %s (entry=0x%08X)", imageSize, loadAddress, imgPath->c_str(), outEntryPoint);
              return true;
            }
          }
        } else {
          SR_LOG_KERNEL_IMPORT("xex_loader: %s size %lld != expected %u", imgPath->c_str(), static_cast<long long>(fsize), imageSize);
        }
      }
    }
    SR_LOG_KERNEL_IMPORT("xex_loader: no uncompressed image; run XenonDecompress to produce %s (recommended) or %s", imgPathA.c_str(), imgPathB.c_str());
    return false;
  }

  if (headerSize + imageSize > toRead) return false;
  uint64_t destOffset = loadAddress;
  if (destOffset + imageSize > destSize) return false;
  uint8_t* destPtr = destBase + destOffset;
  std::memcpy(destPtr, data + headerSize, imageSize);
  outLoadAddress = loadAddress;
  outImageSize = imageSize;
  if (!outEntryPoint) outEntryPoint = getXexEntryPoint(data, toRead, headerCount);
  captureTitleResource(data, toRead, headerCount, destBase, loadAddress, imageSize);
  SR_LOG_KERNEL_IMPORT("xex_loader: loaded %u bytes at VA 0x%08X from %s (entry=0x%08X)", imageSize, loadAddress, path, outEntryPoint);
  return true;
}

}  // namespace kernel
}  // namespace sr
