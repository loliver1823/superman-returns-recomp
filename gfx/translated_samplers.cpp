#include "translated_samplers.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>

namespace sr::gfx {
namespace {
bool validOptions(uint32_t anisotropy, float lodBias) {
  return (anisotropy == 0 || anisotropy == 2 || anisotropy == 4 || anisotropy == 8 || anisotropy == 16) &&
      std::isfinite(lodBias) && lodBias >= -2 && lodBias <= 2;
}
uint32_t log2Floor(uint32_t v) { uint32_t r = 0; while (v >>= 1) ++r; return r; }
// xenos::ClampMode
constexpr uint8_t kClampToEdge = 2, kMirrorClampToEdge = 3, kClampToHalfway = 4, kMirrorClampToHalfway = 5,
                  kClampToBorder = 6, kMirrorClampToBorder = 7;
// D3D12TextureCache::NormalizeClampMode (d3d12_texture_cache.cc): no Direct3D 12 equivalents for these three.
uint8_t normalizeClamp(uint32_t m) {
  if (m == kClampToHalfway) return kClampToEdge;
  if (m == kMirrorClampToHalfway || m == kMirrorClampToBorder) return kMirrorClampToEdge;
  return uint8_t(m & 7u);
}
}  // namespace

uint32_t VolumeSamplerParams::key() const {
  return uint32_t(clampX) | (uint32_t(clampY) << 3) | (uint32_t(clampZ) << 6) | (uint32_t(borderColor) << 9) |
         (uint32_t(aniso) << 11) | (uint32_t(mipMinLevel) << 14) | (uint32_t(magLinear) << 18) |
         (uint32_t(minLinear) << 19) | (uint32_t(mipLinear) << 20) | (uint32_t(mipBaseMap) << 21) |
         (uint32_t(hasMips) << 22) | (uint32_t(fetchMinMagLinear) << 23) | (uint32_t(fetchMipPointOrLinear) << 24);
}

// D3D12TextureCache::GetSamplerParameters with the binding's overrides at kUseFetchConst (our translated shaders
// pick one sampler per fetch slot), GetClampModesForDimension and GetSubresourcesFromFetchConstant's mip range.
// Fetch constant fields (xenos.h xe_gpu_texture_fetch_t): dword_0 clamp_x/y/z bits 10/13/16; dword_3 mag / min /
// mip filter bits 19 / 21 / 23, aniso_filter bits 25-27; dword_4 mip_min_level bits 2-5, mip_max_level 6-9;
// dword_5 border_color bits 0-1, dimension bits 9-10, mip_address bits 12-31; dword_1 base_address bits 12-31.
VolumeSamplerParams volumeSamplerParams(const uint32_t* f) {
  VolumeSamplerParams p;
  const uint32_t dim = (f[5] >> 9) & 3u;
  uint32_t cx = kClampToEdge, cy = kClampToEdge, cz = kClampToEdge;
  if (dim == 2u) cz = (f[0] >> 16) & 7u;
  if (dim == 2u || dim == 1u) cy = (f[0] >> 13) & 7u;
  if (dim != 3u) cx = (f[0] >> 10) & 7u;
  p.clampX = normalizeClamp(cx);
  p.clampY = normalizeClamp(cy);
  p.clampZ = normalizeClamp(cz);
  const auto usesBorder = [](uint8_t m) { return m == kClampToBorder || m == kMirrorClampToBorder; };
  p.borderColor = (usesBorder(p.clampX) || usesBorder(p.clampY) || usesBorder(p.clampZ)) ? uint8_t(f[5] & 3u) : 0u;
  // GetSubresourcesFromFetchConstant: the longest axis (depth included for k3D), the mip page decides.
  uint32_t w1 = 0, h1 = 0, d1 = 0;
  if (dim == 2u) {
    w1 = f[2] & 0x7FFu; h1 = (f[2] >> 11) & 0x7FFu; d1 = (f[2] >> 22) & 0x3FFu;
  } else if (dim == 0u) {
    w1 = f[2] & 0xFFFFFFu;
  } else {
    w1 = f[2] & 0x1FFFu; h1 = (f[2] >> 13) & 0x1FFFu;
  }
  uint32_t longest = std::max(w1, h1);
  if (dim == 2u) longest = std::max(longest, d1);
  const uint32_t sizeMax = log2Floor(longest + 1u);
  const uint32_t basePage = (f[1] >> 12) & 0x1FFFFu, mipPage = (f[5] >> 12) & 0x1FFFFu;
  uint32_t mipMin = 0, mipMax = 0;
  if (mipPage) {
    mipMin = std::min((f[4] >> 2) & 0xFu, sizeMax);
    mipMax = std::max(std::min((f[4] >> 6) & 0xFu, sizeMax), mipMin);
  }
  if (mipMax != 0u && basePage == 0u) mipMin = std::max(mipMin, 1u);
  p.mipMinLevel = uint8_t(mipMin);
  p.hasMips = mipMax > mipMin;
  const uint32_t mag = (f[3] >> 19) & 3u, min = (f[3] >> 21) & 3u, mip = (f[3] >> 23) & 3u;  // TextureFilter
  p.fetchMinMagLinear = mag == 1u && min == 1u;
  p.fetchMipPointOrLinear = mip == 0u || mip == 1u;
  p.mipBaseMap = mip == 2u;
  p.aniso = uint8_t(std::min((f[3] >> 25) & 7u, 5u));  // std::min(aniso_filter, kMax_16_1)
  if (p.aniso) {
    p.magLinear = p.minLinear = p.mipLinear = true;
  } else {
    p.magLinear = mag == 1u;
    p.minLinear = min == 1u;
    p.mipLinear = mip == 1u;
  }
  return p;
}

// D3D12TextureCache::WriteSampler (d3d12_texture_cache.cc) + the graphics options.
D3D12_SAMPLER_DESC volumeSamplerDesc(const VolumeSamplerParams& p, uint32_t anisotropy, float lodBias) {
  D3D12_SAMPLER_DESC d{};
  uint32_t aniso = p.aniso;
  bool magL = p.magLinear, minL = p.minLinear, mipL = p.mipLinear;
  // Xenia's anisotropic_override: only for mipmapped textures already filtered bilinear / trilinear.
  if (anisotropy >= 2u && p.hasMips && !p.mipBaseMap && p.fetchMinMagLinear && p.fetchMipPointOrLinear) {
    aniso = 1u + log2Floor(anisotropy);  // 2 -> kMax_2_1 (2) ... 16 -> kMax_16_1 (5)
    magL = minL = mipL = true;
  }
  if (aniso) {
    d.Filter = D3D12_FILTER_ANISOTROPIC;
    d.MaxAnisotropy = 1u << (aniso - 1u);
  } else {
    d.Filter = D3D12_ENCODE_BASIC_FILTER(minL ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                         magL ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                         mipL ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                         D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    d.MaxAnisotropy = 1;
  }
  static constexpr D3D12_TEXTURE_ADDRESS_MODE kAddressModeMap[] = {
      D3D12_TEXTURE_ADDRESS_MODE_WRAP,         // kRepeat
      D3D12_TEXTURE_ADDRESS_MODE_MIRROR,       // kMirroredRepeat
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP,        // kClampToEdge
      D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,  // kMirrorClampToEdge
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP,        // kClampToHalfway
      D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,  // kMirrorClampToHalfway
      D3D12_TEXTURE_ADDRESS_MODE_BORDER,       // kClampToBorder
      D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,  // kMirrorClampToBorder
  };
  d.AddressU = kAddressModeMap[p.clampX & 7u];
  d.AddressV = kAddressModeMap[p.clampY & 7u];
  d.AddressW = kAddressModeMap[p.clampZ & 7u];
  d.MipLODBias = lodBias;
  d.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  switch (p.borderColor) {
    case 1:  // k_ABGR_White
      d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = d.BorderColor[3] = 1.0f;
      break;
    case 2:  // k_ACBYCR_Black
      d.BorderColor[0] = 0.5f; d.BorderColor[1] = 0.0f; d.BorderColor[2] = 0.5f; d.BorderColor[3] = 0.0f;
      break;
    case 3:  // k_ACBCRY_Black
      d.BorderColor[0] = 0.0f; d.BorderColor[1] = 0.5f; d.BorderColor[2] = 0.5f; d.BorderColor[3] = 0.0f;
      break;
    default:  // k_ABGR_Black
      break;
  }
  d.MinLOD = float(p.mipMinLevel);
  if (p.mipBaseMap) {
    // Xenia: point mip sampling and MaxLOD = MinLOD + 0.25 keeps magnification vs minification; with anisotropy
    // the filter is linear for both, so MaxLOD = MinLOD.
    d.MaxLOD = d.MinLOD + (aniso ? 0.0f : 0.25f);
  } else {
    d.MaxLOD = FLT_MAX;  // the maximum level is in the resource itself
  }
  return d;
}

HRESULT translatedSamplerRecipes(uint32_t anisotropy, float lodBias, std::span<const VolumeSamplerParams> volumes,
                                 TranslatedSamplerRecipes& output) {
  if (!validOptions(anisotropy, lodBias) || volumes.size() > kXlatVolumeSamplers) return E_INVALIDARG;
  TranslatedSamplerRecipes recipes{};
  for (uint32_t i = 0; i < kXlatLegacySamplers; ++i) {
    auto& sampler = recipes[i].sampler;
    const bool linear = (i & 1u) != 0;
    sampler.Filter = linear ? (anisotropy ? D3D12_FILTER_ANISOTROPIC : D3D12_FILTER_MIN_MAG_MIP_LINEAR)
                            : D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = (i & 4u) ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = (i & 2u) ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    // Bit 3 = W repeats (clamp_z of a volume's fetch constant, SR_XLAT_SAMPLER_W). Indices 0-7 keep W CLAMP, as
    // every sampler had before; only that gate produces an index >= 8 (d3d12_backend.cpp pm4SamplerIndex).
    sampler.AddressW = (i & 8u) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    // These fields were zero in non-anisotropic/non-comparison legacy samplers.
    // Their legal values have no effect on those filter modes.
    sampler.MaxAnisotropy = linear && anisotropy ? anisotropy : 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MipLODBias = lodBias;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
  }
  for (uint32_t i = 0; i < kXlatVolumeSamplers; ++i)
    recipes[kXlatLegacySamplers + i].sampler =
        i < volumes.size() ? volumeSamplerDesc(volumes[i], anisotropy, lodBias) : recipes[0].sampler;
  output = std::move(recipes);
  return S_OK;
}

HRESULT TranslatedSamplerCache::select(ID3D12Device* device, uint32_t anisotropy, float lodBias,
                                      std::span<const VolumeSamplerParams> volumes, uint64_t volumeGeneration,
                                      NativeDescriptorVersion& output) {
  if (!device || (m_device && m_device.Get() != device) || !validOptions(anisotropy, lodBias) ||
      volumes.size() > kXlatVolumeSamplers)
    return E_INVALIDARG;
  const bool defaults = !anisotropy && lodBias == 0;
  auto& cached = defaults ? m_default : m_options;
  auto& cachedGen = defaults ? m_defaultGen : m_optionsGen;
  if (cached && cachedGen == volumeGeneration &&
      (defaults || (anisotropy == m_anisotropy && lodBias == m_lodBias))) {
    output = cached;  // a hit builds no recipes: this runs for every draw
    return S_OK;
  }
  TranslatedSamplerRecipes recipes;
  const HRESULT valid = translatedSamplerRecipes(anisotropy, lodBias, volumes, recipes);
  if (FAILED(valid)) return valid;
  auto available = m_live.end();
  for (auto it = m_live.begin(); it != m_live.end(); ++it)
    if (it->expired()) { available = it; break; }
  if (available == m_live.end()) return E_OUTOFMEMORY;
  NativeDescriptorVersion next;
  const HRESULT hr = NativeDescriptorVersion::create(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, recipes, next);
  if (FAILED(hr)) return hr;
  *available = next.ownership();
  m_device = device;
  cached = next;
  cachedGen = volumeGeneration;
  if (!defaults) { m_anisotropy = anisotropy; m_lodBias = lodBias; }
  output = std::move(next);
  return S_OK;
}

size_t TranslatedSamplerCache::liveVersions() const {
  size_t live = 0;
  for (const auto& version : m_live) if (!version.expired()) ++live;
  return live;
}
}  // namespace sr::gfx
