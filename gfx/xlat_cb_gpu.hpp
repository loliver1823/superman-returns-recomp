#pragma once
// step 6 I10p (Fable ruling 40; the coordinator's variant, 2026-10-23; frozen bisect2e/I10p_FROZEN_20261023.txt):
// the GPU-visible twin of the CP's constant scratch (m_xlatConstants: three thirds) for the translated VS / PS float
// blocks and the vsf fetch block. The CP writes each CHANGED block once, straight into this upload resource at the
// scratch's own offsets; the DrawBatch's offsets (xlatVsCbOff / xlatPsCbOff / vsfFetchCbOff) are the borrowed views
// (GPU VA = base + offset); page-level ownership is the THIRD and its existing fence (m_thirdFence / pm4WaitThirdFree:
// a third is rewritten only after the GPU completed the last frame drained into it). No per-draw allocation, refcount
// or keep.
//
// WRITE-ONLY (the coordinator's condition 1; iter-465's precedent: a mapped UPLOAD heap is write-combined - a CPU read
// is prohibitively slow and never needed): the mapped pointer is private, and write() / fill() are the ONLY CPU
// accesses. In SR_ORACLE check mode the same calls also write a cacheable SHADOW at the same offsets; every check and
// every diagnostic reads the shadow, never the resource.
#include <d3d12.h>
#include <wrl/client.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace sr::gfx {
struct XlatCBGpuTestAccess;
class XlatCBGpu {
 public:
  // One committed UPLOAD buffer of `bytes`, persistently mapped with an empty read range (the CPU never reads it).
  HRESULT initialize(ID3D12Device* device, uint64_t bytes) {
    if (!device || !bytes || m_mapped) return E_INVALIDARG;
    D3D12_HEAP_PROPERTIES up{};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT hr = device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_PPV_ARGS(&m_resource));
    if (FAILED(hr)) return hr;
    const D3D12_RANGE noRead{0, 0};
    void* p = nullptr;
    hr = m_resource->Map(0, &noRead, &p);
    if (FAILED(hr)) { m_resource.Reset(); return hr; }
    m_mapped = static_cast<uint8_t*>(p);
    m_bytes = bytes;
    m_base = m_resource->GetGPUVirtualAddress();
    return S_OK;
  }
  // The cacheable shadow (SR_ORACLE check mode only; nullptr = none): written by the same calls, at the same offsets,
  // under `mu` (the checker's readers take the same mutex: the CP and the render thread write and hash it concurrently).
  void setShadow(uint8_t* shadow, std::mutex* mu) { m_shadow = shadow; m_shadowMu = mu; }
  // The ONLY CPU accesses: write-only.
  // Astra I10p r2 P2: with a check-mode mutex installed, it is taken BEFORE either destination is written and held
  // across both (the upload write and its shadow mirror), so concurrent writers (the CP's poison, the render thread's
  // shared slot) leave the twin and the shadow with the same last writer. No mutex (release; no shadow): no lock.
  bool write(uint64_t offset, const void* src, size_t n) {
    if (!m_mapped || offset > m_bytes || n > m_bytes - offset) return false;
    std::unique_lock<std::mutex> lk;
    if (m_shadowMu) lk = std::unique_lock<std::mutex>(*m_shadowMu);
    std::memcpy(m_mapped + offset, src, n);
    if (m_shadow) std::memcpy(m_shadow + offset, src, n);
    return true;
  }
  bool fill(uint64_t offset, uint8_t value, size_t n) {   // the check mode's poison of a retired third's used range
    if (!m_mapped || offset > m_bytes || n > m_bytes - offset) return false;
    std::unique_lock<std::mutex> lk;
    if (m_shadowMu) lk = std::unique_lock<std::mutex>(*m_shadowMu);
    std::memset(m_mapped + offset, value, n);
    if (m_shadow) std::memset(m_shadow + offset, value, n);
    return true;
  }
  D3D12_GPU_VIRTUAL_ADDRESS address(uint64_t offset) const { return m_mapped ? m_base + offset : 0; }
  uint64_t bytes() const { return m_bytes; }
  explicit operator bool() const { return m_mapped != nullptr; }
 private:
  friend struct XlatCBGpuTestAccess;   // tools/difftest/xlat_cb_gpu_test.cpp: a GPU copy into a readback buffer only
  Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
  uint8_t* m_mapped = nullptr;   // write-combined: never read
  uint8_t* m_shadow = nullptr;   // cacheable (check mode)
  std::mutex* m_shadowMu = nullptr;
  uint64_t m_bytes = 0;
  D3D12_GPU_VIRTUAL_ADDRESS m_base = 0;
};

// M1 (Fable 40: an INVARIANT that must never regress): a translated VS / PS upload writes ONLY the 256-byte blocks the
// shader reads (use = vsConstUse / psConstUse: bit b = block b; bit 16 = relative addressing -> all; 0 / 0xFFFF -> all),
// never the contiguous extent or the page. Returns the bytes written. `write(offset, src, n)` is the destination.
template <class Write>
inline uint32_t xlatUploadUsedBlocks(Write&& write, uint64_t dst, const uint32_t* src, uint32_t use) {
  if ((use & 0x10000u) || (use & 0xFFFFu) == 0xFFFFu || (use & 0xFFFFu) == 0u) {
    write(dst, src, 4096);
    return 4096;
  }
  uint32_t n = 0;
  for (uint32_t m = use & 0xFFFFu; m;) {
    const uint32_t b = uint32_t(__builtin_ctz(m));
    write(dst + 256u * b, src + 64u * b, 256);
    n += 256;
    m &= m - 1u;
  }
  return n;
}

// The check mode's hash (SR_XLAT_CB_ZEROCOPY_CHECK): 64-bit words mixed FNV-style over an upload's USED blocks (the
// bytes M1 writes, the bytes the shader reads), or the whole range (use = ~0u, or a mask meaning all). Callers hash
// cacheable memory only (the source arrays, or the shadow) - never the upload resource. Sizes are multiples of 8.
inline uint64_t xlatCbHash(const uint8_t* base, uint32_t use, uint32_t bytes) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](const uint8_t* p, size_t n) {
    for (size_t i = 0; i + 8 <= n; i += 8) {
      uint64_t w;
      std::memcpy(&w, p + i, 8);
      h = (h ^ w) * 1099511628211ull;
      h ^= h >> 29;
    }
  };
  if (use == ~0u || (use & 0x10000u) || (use & 0xFFFFu) == 0xFFFFu || (use & 0xFFFFu) == 0u) {
    mix(base, bytes);
    return h;
  }
  for (uint32_t m = use & 0xFFFFu; m;) {
    const uint32_t b = uint32_t(__builtin_ctz(m));
    if (256u * b < bytes) mix(base + 256u * b, 256);
    h = (h ^ b) * 1099511628211ull;
    m &= m - 1u;
  }
  return h;
}
}  // namespace sr::gfx
