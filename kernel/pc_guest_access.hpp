#pragma once

#include "collision_watch.hpp"
#include "guest_protect.hpp"
#include "sr_oracle.hpp"
#include <cstdio>

namespace sr::kernel {
void getGlobalGuestMemory(uint8_t** base, size_t* size);
uint8_t* guestVAToHost(uint32_t va);
extern bool g_physUnifyActive;

namespace pcguest {

// Retained PC features use the same arena/protection lifetime contract as
// native_transport::admit. The lease covers the operation, including its host
// reads/writes and guest calls. No page permission is changed here. Foreign
// protection is excluded before it can run; GPU write tracking retains its
// normal first-store handler. Oracle keeps the old flat-reader semantics.
class Mem {
 public:
  uint8_t* const base;
  explicit Mem(uint8_t* arena, bool mapped = false) noexcept : base(arena), mapped_(mapped) {
#if !SR_ORACLE
    uint8_t* registered = nullptr;
    getGlobalGuestMemory(&registered, &size_);
    admitted_ = bool(lease_) && base && base == registered && !g_physUnifyActive &&
                !guest_protect::diagnosticRequested() && !guest_protect::excludesAll();
#endif
  }
  bool ready() const noexcept {
#if SR_ORACLE
    return base != nullptr;
#else
    return admitted_;
#endif
  }
  bool ok() const noexcept {
#if SR_ORACLE
    return true;
#else
    return admitted_ && !failed_;
#endif
  }

  // Address expressions are widened BEFORE addition/multiplication at callers.
  // A wrapped guest pointer must never become an admitted low address.
  bool range(uint64_t va, size_t bytes) const noexcept { return address(va, bytes) != nullptr; }
  bool read(uint64_t va, void* out, size_t bytes) const noexcept {
    if (va < collwatch::kLo || va >= collwatch::kTop || bytes > collwatch::kTop - va) {
      failed_ = true;
      return false;
    }
    const uint8_t* from = address(va, bytes);
    if (!from) return false;
    std::memcpy(out, from, bytes);
    return true;
  }
  bool write(uint64_t va, const void* value, size_t bytes) const noexcept {
    if (!ok()) return false;
    uint8_t* to = address(va, bytes);
    if (!to) return false;
    std::memcpy(to, value, bytes);
    return true;
  }
  bool copy(uint64_t to, uint64_t from, size_t bytes) const noexcept {
    if (!ok()) return false;
    uint8_t* destination = address(to, bytes);
    const uint8_t* source = address(from, bytes);
    if (!destination || !source) return false;
    std::memcpy(destination, source, bytes);
    return true;
  }
  uint32_t u32(uint64_t va) const noexcept {
    uint32_t value = 0;
    return read(va, &value, sizeof value) ? __builtin_bswap32(value) : 0;
  }
  uint16_t u16(uint64_t va) const noexcept {
    uint16_t value = 0;
    return read(va, &value, sizeof value) ? __builtin_bswap16(value) : 0;
  }
  uint8_t u8(uint64_t va) const noexcept {
    uint8_t value = 0;
    read(va, &value, sizeof value);
    return value;
  }
  float f32(uint64_t va) const noexcept {
    const uint32_t bits = u32(va);
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
  }
  void vec3(uint64_t va, float out[3]) const noexcept {
    uint32_t bits[3];
    if (!read(va, bits, sizeof bits)) { out[0] = out[1] = out[2] = NAN; return; }
    for (unsigned i = 0; i < 3; ++i) {
      bits[i] = __builtin_bswap32(bits[i]);
      std::memcpy(out + i, bits + i, sizeof(float));
    }
  }
  bool put32(uint64_t va, uint32_t value) const noexcept {
    value = __builtin_bswap32(value);
    return write(va, &value, sizeof value);
  }
  // Cleanup of a previously saved value after another dynamic read failed.
  // Retain the original operation lease (retirement can refuse a NEW lease),
  // validate the destination again, and never clear the failed-read status.
  bool restore32(uint64_t va, uint32_t value) const noexcept {
    uint8_t* to = address(va, sizeof value);
    if (!to) return false;
    value = __builtin_bswap32(value);
    std::memcpy(to, &value, sizeof value);
    return true;
  }
  bool put16(uint64_t va, uint16_t value) const noexcept {
    value = __builtin_bswap16(value);
    return write(va, &value, sizeof value);
  }
  bool put8(uint64_t va, uint8_t value) const noexcept { return write(va, &value, sizeof value); }
  bool putFloat(uint64_t va, float value) const noexcept {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return put32(va, bits);
  }

 private:
#if !SR_ORACLE
  guest_protect::ArenaLease lease_;
  size_t size_ = 0;
  bool admitted_ = false;
#endif
  bool mapped_;
  mutable bool failed_ = false;
  uint8_t* address(uint64_t va, size_t bytes) const noexcept {
#if !SR_ORACLE
    if (!admitted_ || !bytes || va < collwatch::kLo || va > UINT32_MAX ||
        uint64_t(bytes) > uint64_t(UINT32_MAX) + 1u - va ||
        guest_protect::diagnosticRequested() || guest_protect::excludesAll()) {
      failed_ = true;
      return nullptr;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(base);
    if (va > UINTPTR_MAX - start) { failed_ = true; return nullptr; }
    if (!mapped_ && (va >= size_ || bytes > size_ - size_t(va))) {
      failed_ = true;
      return nullptr;
    }
    // Form raw addresses only after proving their complete extent. Mapped
    // access additionally proves the translated endpoints are contiguous.
    uint8_t* first = mapped_ ? guestVAToHost(uint32_t(va))
                             : reinterpret_cast<uint8_t*>(start + uintptr_t(va));
    const uintptr_t firstAddress = reinterpret_cast<uintptr_t>(first);
    if (bytes - 1u > UINTPTR_MAX - firstAddress) { failed_ = true; return nullptr; }
    const uint8_t* last = mapped_ ? guestVAToHost(uint32_t(va + bytes - 1u))
                                  : reinterpret_cast<uint8_t*>(firstAddress + bytes - 1u);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(first), hi = reinterpret_cast<uintptr_t>(last);
    if (!first || !last || lo < start || hi < lo || hi - lo != bytes - 1u ||
        hi - start >= size_ || guest_protect::intersects(first, bytes)) {
      failed_ = true;
      return nullptr;
    }
    return first;
#else
    (void)bytes;
    return mapped_ ? guestVAToHost(uint32_t(va)) : base + size_t(va);
#endif
  }
};

inline bool ready(uint8_t* base, char* why, size_t count) {
#if SR_ORACLE
  return collwatch::flatModelReadable(base, why, count);
#else
  Mem memory(base);
  std::snprintf(why, count, "%s", memory.ready()
      ? "native arena lease; exact ranges exclude foreign protection"
      : "native arena unavailable, retired, foreign, unified, or diagnostic-protected");
  return memory.ready();
#endif
}

}  // namespace pcguest
}  // namespace sr::kernel
