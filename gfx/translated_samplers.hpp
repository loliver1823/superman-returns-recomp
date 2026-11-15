#pragma once
#include "native_draw.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace sr::gfx {
// The translated shader ABI's sampler table. Indices 0-15 are the iter-411 point/linear and wrap/clamp variants:
// index = (wrapW ? 8 : 0) | (clampU ? 4 : 0) | (clampV ? 2 : 0) | (linear ? 1 : 0); every 2D, stacked and cube fetch
// uses them. Indices 16-63 (step-4 lane C round 2, SR_XLAT_SAMPLER_V2) are full per-fetch-constant samplers of the
// volume (k3D) fetches, assigned on first use (D3D12Backend::xlatVolumeSamplerIndex); an unassigned entry repeats
// index 0. Options change filtering and LOD bias without changing indices.
constexpr uint32_t kXlatLegacySamplers = 16;
constexpr uint32_t kXlatVolumeSamplers = 48;
constexpr uint32_t kXlatSamplerTable = kXlatLegacySamplers + kXlatVolumeSamplers;
using TranslatedSamplerRecipes = std::array<NativeViewRecipe, kXlatSamplerTable>;

// Xenia's SamplerParameters (d3d12_texture_cache.h) for a fetch constant read with every instruction override at
// kUseFetchConst: the clamp modes after NormalizeClampMode, the border colour (black unless a mode uses it), the
// filters, anisotropy and mip_min_level (texture_util.cc GetSubresourcesFromFetchConstant). Port of
// D3D12TextureCache::GetSamplerParameters (d3d12_texture_cache.cc).
struct VolumeSamplerParams {
  uint8_t clampX = 0, clampY = 0, clampZ = 0;  // xenos::ClampMode 0-7, normalized
  uint8_t borderColor = 0;                      // xenos::BorderColor
  uint8_t aniso = 0;                            // xenos::AnisoFilter (0 disabled, 1..5 = 1:1 .. 16:1)
  uint8_t mipMinLevel = 0;
  bool magLinear = false, minLinear = false, mipLinear = false, mipBaseMap = false;
  bool hasMips = false;                         // mip_max_level > mip_min_level (the anisotropy option's rule)
  bool fetchMinMagLinear = false, fetchMipPointOrLinear = false;  // the filters before anisotropy
  uint32_t key() const;
};
VolumeSamplerParams volumeSamplerParams(const uint32_t* fetch);
// D3D12TextureCache::WriteSampler for those parameters, plus this port's graphics options (as the legacy table):
// `anisotropy` (0 or 2..16) overrides like Xenia's anisotropic_override (mipmapped, not kBaseMap, linear min / mag,
// point or linear mip); `lodBias` is MipLODBias (Xenia biases in the shader; the option is ours).
D3D12_SAMPLER_DESC volumeSamplerDesc(const VolumeSamplerParams& p, uint32_t anisotropy, float lodBias);

// The whole table: the sixteen legacy recipes and one per entry of `volumes` (at most kXlatVolumeSamplers).
HRESULT translatedSamplerRecipes(uint32_t anisotropy, float lodBias, std::span<const VolumeSamplerParams> volumes,
                                 TranslatedSamplerRecipes& output);
inline HRESULT translatedSamplerRecipes(uint32_t anisotropy, float lodBias, TranslatedSamplerRecipes& output) {
  return translatedSamplerRecipes(anisotropy, lodBias, {}, output);
}

// Round 3 (SR_XLAT_SAMPLER_V3): the entry a full volume table reclaims - the least recently bound one whose last frame
// the GPU has completed (`lastUse` = per entry, the frame fence value of the frame that last bound it; `completed`
// = the frame fence's completed value). SIZE_MAX = none (every entry bound in a frame still in flight).
inline size_t reclaimableVolumeSampler(std::span<const uint64_t> lastUse, uint64_t completed) {
  size_t lru = SIZE_MAX;
  for (size_t i = 0; i < lastUse.size(); i++)
    if (lastUse[i] <= completed && (lru == SIZE_MAX || lastUse[i] < lastUse[lru])) lru = i;
  return lru;
}

// Default and latest option tables are cached, per volume-sampler generation (a new volume sampler is a new
// immutable version). Older versions belong to their recordings/submissions; no published descriptor is
// rewritten on a ring lap.
class TranslatedSamplerCache {
 public:
  static constexpr size_t kMaxLiveVersions = 64;
  HRESULT select(ID3D12Device* device, uint32_t anisotropy, float lodBias,
                 NativeDescriptorVersion& output) {
    return select(device, anisotropy, lodBias, {}, 0, output);
  }
  HRESULT select(ID3D12Device* device, uint32_t anisotropy, float lodBias,
                 std::span<const VolumeSamplerParams> volumes, uint64_t volumeGeneration,
                 NativeDescriptorVersion& output);
  size_t liveVersions() const;
 private:
  Microsoft::WRL::ComPtr<ID3D12Device> m_device;
  NativeDescriptorVersion m_default, m_options;
  uint64_t m_defaultGen = 0, m_optionsGen = 0;
  uint32_t m_anisotropy = 0;
  float m_lodBias = 0;
  std::array<std::weak_ptr<const void>, kMaxLiveVersions> m_live;
};
}  // namespace sr::gfx
