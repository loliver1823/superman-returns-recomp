#pragma once
#include "native_draw.hpp"

namespace sr::gfx {
// Explicit translated-shader ABI: float constants at b0/b1, shared data at b2,
// dynamic aliases at b3/b4 (space4), and optional vertex-fetch data at b5 space5.
// The CPU adapter finishes texture/sampler indices before publishing this copy.
//
// Float extents: vs / ps hold c0 up to the end of the last 16-register block the
// stage's shader reads (kBlockBytes..kFloatBytes, a multiple of kBlockBytes). The
// caller takes the extent from the shader's block-use mask (the CP's vsConstUse /
// psConstUse: gfx/xenos_ucode constantBlocksRead, relative addressing = all), the same
// mask that already limits which blocks the CP copies. Layout: VS, PS, shared, fetch,
// packed at 256-byte offsets. The page also spans kFloatBytes from each float block's
// start (an arena reserve, not owned bytes), so a read past an extent stays inside the
// buffer resource.
class TranslatedConstantSnapshot {
 public:
  static constexpr uint32_t kFloatBytes = 4096, kBlockBytes = 256, kSharedBytes = 512, kFetchBytes = 768;
  static HRESULT stage(NativeUploadArena& arena, std::span<const uint8_t> vs,
                       std::span<const uint8_t> ps, std::span<const uint8_t> shared,
                       std::span<const uint8_t> fetch, TranslatedConstantSnapshot& output);
  // Caller has already bound the matching root signature. Retain the immutable
  // storage before recording any root address; invalid/closed recordings refuse.
  HRESULT bind(NativeDrawRecording& recording) const;
  D3D12_GPU_VIRTUAL_ADDRESS address(uint32_t parameter) const;
  // Byte offset of a parameter's block within storage() and its staged size (0 = unbound).
  uint32_t offset(uint32_t parameter) const;
  uint32_t size(uint32_t parameter) const;
  const NativeBufferSlice& storage() const { return m_storage; }
  explicit operator bool() const { return bool(m_storage); }
 private:
  NativeBufferSlice m_storage;
  uint32_t m_vsBytes = 0, m_psBytes = 0;
  bool m_fetch = false;
};
}  // namespace sr::gfx
