#include "native_draw.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <unordered_set>

namespace sr::gfx {
namespace {
bool sameDevice(ID3D12DeviceChild* child, ID3D12Device* device) {
  Microsoft::WRL::ComPtr<ID3D12Device> owner;
  Microsoft::WRL::ComPtr<IUnknown> expected, actual;
  return child && SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&owner))) &&
      SUCCEEDED(owner.As(&actual)) && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&expected))) &&
      expected.Get() == actual.Get();
}
struct RootRule {
  D3D12_ROOT_PARAMETER_TYPE kind;
  D3D12_DESCRIPTOR_HEAP_TYPE heap = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  uint32_t count = 0;
  uint32_t requiredBufferBytes = 0;
};
bool finiteViewport(const D3D12_VIEWPORT& v) {
  return std::isfinite(v.TopLeftX) && std::isfinite(v.TopLeftY) &&
      std::isfinite(v.Width) && std::isfinite(v.Height) &&
      std::isfinite(v.MinDepth) && std::isfinite(v.MaxDepth) && v.Width > 0 && v.Height > 0 &&
      v.MinDepth >= 0 && v.MaxDepth <= 1 && v.MinDepth <= v.MaxDepth &&
      v.TopLeftX >= D3D12_VIEWPORT_BOUNDS_MIN && v.TopLeftY >= D3D12_VIEWPORT_BOUNDS_MIN &&
      v.TopLeftX + v.Width <= D3D12_VIEWPORT_BOUNDS_MAX &&
      v.TopLeftY + v.Height <= D3D12_VIEWPORT_BOUNDS_MAX;
}
D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType(D3D_PRIMITIVE_TOPOLOGY t) {
  switch (t) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST: case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ: case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
      return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST: case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ: case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
      return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    default: return t >= D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST &&
        t <= D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH
                                                            : D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
  }
}
uint32_t elementBytes(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT: return 16;
    case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32_SINT: return 12;
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SINT: return 8;
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT: return 4;
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SINT: return 2;
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SINT: return 1;
    default: return 0;
  }
}
bool samplerValid(const D3D12_SAMPLER_DESC& s) {
  switch (s.Filter) {
    case D3D12_FILTER_MIN_MAG_MIP_POINT:
    case D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR:
    case D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT:
    case D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR:
    case D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT:
    case D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
    case D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT:
    case D3D12_FILTER_MIN_MAG_MIP_LINEAR:
    case D3D12_FILTER_ANISOTROPIC:
    case D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT:
    case D3D12_FILTER_COMPARISON_MIN_MAG_POINT_MIP_LINEAR:
    case D3D12_FILTER_COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT:
    case D3D12_FILTER_COMPARISON_MIN_POINT_MAG_MIP_LINEAR:
    case D3D12_FILTER_COMPARISON_MIN_LINEAR_MAG_MIP_POINT:
    case D3D12_FILTER_COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
    case D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT:
    case D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR:
    case D3D12_FILTER_COMPARISON_ANISOTROPIC: break;
    default: return false;
  }
  const auto addressValid = [](D3D12_TEXTURE_ADDRESS_MODE mode) {
    return mode >= D3D12_TEXTURE_ADDRESS_MODE_WRAP && mode <= D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
  };
  if (!addressValid(s.AddressU) || !addressValid(s.AddressV) || !addressValid(s.AddressW) ||
      !std::isfinite(s.MipLODBias) || s.MipLODBias < D3D12_MIP_LOD_BIAS_MIN ||
      s.MipLODBias > D3D12_MIP_LOD_BIAS_MAX || !std::isfinite(s.MinLOD) ||
      !std::isfinite(s.MaxLOD) || s.MinLOD > s.MaxLOD || !s.MaxAnisotropy || s.MaxAnisotropy > 16 ||
      s.ComparisonFunc < D3D12_COMPARISON_FUNC_NEVER || s.ComparisonFunc > D3D12_COMPARISON_FUNC_ALWAYS)
    return false;
  for (float value : s.BorderColor) if (!std::isfinite(value) || value < 0 || value > 1) return false;
  return true;
}
// Explicit DXGI families prevent same-width but incompatible reinterpretation.
// A fully typeless format maps to itself and is never a shader view.
DXGI_FORMAT typelessFamily(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT: return DXGI_FORMAT_R32G32B32A32_TYPELESS;
    case DXGI_FORMAT_R32G32B32_TYPELESS: case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32_SINT: return DXGI_FORMAT_R32G32B32_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT: return DXGI_FORMAT_R32G32_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_SINT: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_SINT: return DXGI_FORMAT_R16G16_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT: return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_SINT: return DXGI_FORMAT_R8G8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT: return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_R8_SINT: return DXGI_FORMAT_R8_TYPELESS;
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: return DXGI_FORMAT_BC1_TYPELESS;
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: return DXGI_FORMAT_BC2_TYPELESS;
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: return DXGI_FORMAT_BC3_TYPELESS;
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM: return DXGI_FORMAT_BC4_TYPELESS;
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM: return DXGI_FORMAT_BC5_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16: return DXGI_FORMAT_BC6H_TYPELESS;
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: return DXGI_FORMAT_BC7_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}
bool srvValid(ID3D12Device* device, const NativeViewRecipe& recipe) {
  const auto& view = recipe.srv;
  constexpr uint32_t mappingBit = D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;
  if (!(view.Shader4ComponentMapping & mappingBit) ||
      (view.Shader4ComponentMapping & ~(mappingBit | 0xfffu))) return false;
  for (uint32_t i = 0; i < 4; ++i)
    if (D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(i, view.Shader4ComponentMapping) >
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1) return false;
  auto* resource = recipe.buffer ? recipe.buffer.resource() : recipe.resource.Get();
  const auto desc = resource ? resource->GetDesc() : D3D12_RESOURCE_DESC{};
  if (resource && (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) return false;
  if (view.ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
    if (resource && desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return false;
    uint64_t stride = 0;
    if (view.Buffer.Flags == D3D12_BUFFER_SRV_FLAG_RAW) {
      if (view.Format != DXGI_FORMAT_R32_TYPELESS || view.Buffer.StructureByteStride) return false;
      stride = 4;
    } else if (view.Buffer.Flags != D3D12_BUFFER_SRV_FLAG_NONE) return false;
    else if (view.Buffer.StructureByteStride) {
      if (view.Format != DXGI_FORMAT_UNKNOWN || view.Buffer.StructureByteStride % 4 ||
          view.Buffer.StructureByteStride > D3D12_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES) return false;
      stride = view.Buffer.StructureByteStride;
    } else {
      stride = elementBytes(view.Format);
      D3D12_FEATURE_DATA_FORMAT_SUPPORT support{view.Format};
      if (!stride || FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
          !(support.Support1 & D3D12_FORMAT_SUPPORT1_BUFFER) || !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD))
        return false;
    }
    if (!view.Buffer.NumElements || view.Buffer.FirstElement > UINT64_MAX / stride) return false;
    const uint64_t start = view.Buffer.FirstElement * stride;
    const uint64_t bytes = uint64_t(view.Buffer.NumElements) * stride;
    const uint64_t begin = recipe.buffer ? recipe.buffer.offset() : 0;
    const uint64_t size = recipe.buffer ? recipe.buffer.size() : resource ? desc.Width : UINT64_MAX;
    return start >= begin && start - begin <= size && bytes <= size - (start - begin);
  }
  if (recipe.buffer || view.Format == DXGI_FORMAT_UNKNOWN || typelessFamily(view.Format) == view.Format)
    return false;
  uint32_t firstMip = 0, mipCount = 0, firstLayer = 0, layerCount = 1, plane = 0;
  float minLOD = 0;
  D3D12_FORMAT_SUPPORT1 dimensionSupport = D3D12_FORMAT_SUPPORT1_TEXTURE2D;
  D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  switch (view.ViewDimension) {
    case D3D12_SRV_DIMENSION_TEXTURE2D:
      firstMip = view.Texture2D.MostDetailedMip; mipCount = view.Texture2D.MipLevels;
      plane = view.Texture2D.PlaneSlice; minLOD = view.Texture2D.ResourceMinLODClamp; break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
      firstMip = view.Texture2DArray.MostDetailedMip; mipCount = view.Texture2DArray.MipLevels;
      firstLayer = view.Texture2DArray.FirstArraySlice; layerCount = view.Texture2DArray.ArraySize;
      plane = view.Texture2DArray.PlaneSlice; minLOD = view.Texture2DArray.ResourceMinLODClamp; break;
    case D3D12_SRV_DIMENSION_TEXTURE3D:
      firstMip = view.Texture3D.MostDetailedMip; mipCount = view.Texture3D.MipLevels;
      minLOD = view.Texture3D.ResourceMinLODClamp;
      dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D; dimensionSupport = D3D12_FORMAT_SUPPORT1_TEXTURE3D; break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE:
      firstMip = view.TextureCube.MostDetailedMip; mipCount = view.TextureCube.MipLevels;
      minLOD = view.TextureCube.ResourceMinLODClamp; layerCount = 6;
      dimensionSupport = D3D12_FORMAT_SUPPORT1_TEXTURECUBE; break;
    default: return false;
  }
  // Explicit finite mip ranges and zero clamp are the currently checked ABI.
  // UINT_MAX mip sentinels, cube arrays, MSAA and 1D views remain unsupported.
  if (!mipCount || uint64_t(firstMip) + mipCount > D3D12_REQ_MIP_LEVELS || minLOD != 0 ||
      !layerCount || uint64_t(firstLayer) + layerCount > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION)
    return false;
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{view.Format};
  if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
      !(support.Support1 & dimensionSupport) ||
      !(support.Support1 & (D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE))) return false;
  const uint32_t formatPlane = view.Format == DXGI_FORMAT_X24_TYPELESS_G8_UINT ||
      view.Format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT ? 1 : 0;
  if (plane != formatPlane) return false;
  // Null stencil-plane recipes need separate coverage before admission.
  if (!resource) return plane == 0;
  if (desc.Dimension != dimension || desc.SampleDesc.Count != 1 ||
      firstMip >= desc.MipLevels || mipCount > desc.MipLevels - firstMip ||
      (desc.Format != view.Format && desc.Format != typelessFamily(view.Format))) return false;
  if (dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
      (firstLayer >= desc.DepthOrArraySize || layerCount > desc.DepthOrArraySize - firstLayer)) return false;
  if (view.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D && desc.DepthOrArraySize != 1) return false;
  if (view.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE && desc.Width != desc.Height) return false;
  D3D12_FEATURE_DATA_FORMAT_INFO info{desc.Format};
  if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info))) || plane >= info.PlaneCount)
    return false;
  if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) {
    switch (desc.Format) {
      case DXGI_FORMAT_R16_TYPELESS: return view.Format == DXGI_FORMAT_R16_UNORM;
      case DXGI_FORMAT_R32_TYPELESS: return view.Format == DXGI_FORMAT_R32_FLOAT;
      case DXGI_FORMAT_R24G8_TYPELESS:
        return view.Format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS || view.Format == DXGI_FORMAT_X24_TYPELESS_G8_UINT;
      case DXGI_FORMAT_R32G8X24_TYPELESS:
        return view.Format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS || view.Format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
      default: return false;
    }
  }
  return true;
}
}
struct NativeRootAllocation {
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
  std::vector<RootRule> rules;
};
// A page is a fixed range [base, base + pageDescriptors) of the arena's ONE
// shader-visible heap. Every version therefore shares that heap, so tables from
// different pages bind together, and recycling a range never creates a heap.
struct NativeDescriptorPage {
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
  uint32_t base = 0, used = 0;
  uint64_t serial = 0;
};
struct NativeDescriptorArenaState {
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
  uint32_t pageDescriptors = 0, maxPages = 0, stride = 0;
  uint64_t serials = 0;
  std::shared_ptr<NativeDescriptorPage> tail;
  std::vector<std::weak_ptr<NativeDescriptorPage>> pages;
};
struct NativeDescriptorAllocation {
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
  std::vector<NativeViewRecipe> recipes;
  std::shared_ptr<NativeDescriptorPage> page;
  D3D12_DESCRIPTOR_HEAP_TYPE type;
  uint32_t stride = 0, firstDescriptor = 0;
};
// Step 6 R1a: the recording serials (monotonic, 64-bit, never reused) and the tests' upload-keep control.
namespace {
std::atomic<uint64_t> g_recordingSerial{0};
#if SR_ORACLE
std::atomic<int> g_uploadKeepMutant{0};
#endif
}  // namespace

struct NativeDrawPayload {
  std::vector<PreparedNativeDraw> draws;
  std::vector<NativeImageLease> images;
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> resources;
  std::vector<NativeBufferSlice> uploads;
  std::vector<NativeDescriptorVersion> descriptors;
  // Allocations already in `descriptors`. A retained allocation stays alive, so
  // its address cannot name a different version during this recording.
  std::unordered_set<const void*> descriptorIds;
};

HRESULT NativeRootSignature::create(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& desc,
                                    NativeRootSignature& output,
                                    std::span<const uint32_t> requiredBufferBytes) {
  if (!device || desc.NumParameters > 64 || (desc.NumParameters && !desc.pParameters) ||
      (desc.NumStaticSamplers && !desc.pStaticSamplers) ||
      (!requiredBufferBytes.empty() && requiredBufferBytes.size() != desc.NumParameters)) return E_INVALIDARG;
  try {
    auto root = std::make_shared<NativeRootAllocation>(); root->device = device;
    for (uint32_t i = 0; i < desc.NumParameters; ++i) {
      const auto& p = desc.pParameters[i]; RootRule rule{p.ParameterType};
      const uint32_t extent = requiredBufferBytes.empty() ? 0 : requiredBufferBytes[i];
      const bool buffer = p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV ||
                          p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV;
      if (buffer ? !extent : extent != 0) return E_INVALIDARG;
      rule.requiredBufferBytes = extent;
      if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV && extent > 65536) return E_INVALIDARG;
      if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) rule.count = p.Constants.Num32BitValues;
      else if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        if (!p.DescriptorTable.NumDescriptorRanges || !p.DescriptorTable.pDescriptorRanges) return E_INVALIDARG;
        uint64_t end = 0;
        bool sampler = false;
        for (uint32_t j = 0; j < p.DescriptorTable.NumDescriptorRanges; ++j) {
          const auto& range = p.DescriptorTable.pDescriptorRanges[j];
          // This descriptor service creates SRVs and samplers only. It cannot
          // satisfy CBV/UAV ranges or prove an unbounded shader index domain.
          if (!range.NumDescriptors || range.NumDescriptors == UINT32_MAX ||
              (range.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_SRV &&
               range.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)) return E_INVALIDARG;
          const bool s = range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
          if (j && sampler != s) return E_INVALIDARG;
          sampler = s;
          const uint64_t start = range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
              ? end : range.OffsetInDescriptorsFromTableStart;
          const uint64_t count = range.NumDescriptors;
          end = std::max(end, start + count);
          if (end > UINT32_MAX) return E_INVALIDARG;
        }
        rule.count = uint32_t(end); rule.heap = sampler ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
      } else if (!buffer)
        return E_INVALIDARG;
      root->rules.push_back(rule);
    }
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (FAILED(hr)) return hr;
    hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root->root));
    if (FAILED(hr)) return hr;
    output.m_root = std::move(root); return S_OK;
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
ID3D12RootSignature* NativeRootSignature::root() const { return m_root ? m_root->root.Get() : nullptr; }
ID3D12Device* NativeRootSignature::device() const { return m_root ? m_root->device.Get() : nullptr; }
std::weak_ptr<const void> NativeRootSignature::ownership() const { return m_root; }

namespace {
HRESULT validateViews(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                      std::span<const NativeViewRecipe> recipes) {
  if (!device || recipes.empty() || (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
      type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) || recipes.size() >
      (type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 2048u : 1000000u)) return E_INVALIDARG;
  // Compact translated tables contain 65 entries, including repeated nulls
  // and views. Reuse validation only for exact recipes in this call, without
  // retaining owners or changing the admission of larger descriptor tables.
  std::array<const NativeViewRecipe*, 65> validated{};
  size_t validatedCount = 0;
  const bool memoize = recipes.size() <= validated.size();
  for (const auto& r : recipes) {
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
      if (r.resource || r.buffer || !samplerValid(r.sampler)) return E_INVALIDARG;
      continue;
    }
    if (memoize && std::any_of(validated.begin(), validated.begin() + validatedCount, [&](const auto* prior) {
      return prior->resource.Get() == r.resource.Get() && prior->buffer.resource() == r.buffer.resource() &&
          prior->buffer.device() == r.buffer.device() && prior->buffer.offset() == r.buffer.offset() &&
          prior->buffer.size() == r.buffer.size() && std::memcmp(&prior->srv, &r.srv, sizeof(r.srv)) == 0;
    })) continue;
    if (r.buffer && (r.resource || r.buffer.device() != device)) return E_INVALIDARG;
    auto* resource = r.buffer ? r.buffer.resource() : r.resource.Get();
    if (resource && !sameDevice(resource, device)) return E_INVALIDARG;
    if (resource) {
      D3D12_HEAP_PROPERTIES heap{}; D3D12_HEAP_FLAGS flags{};
      if (FAILED(resource->GetHeapProperties(&heap, &flags)) ||
          (heap.Type != D3D12_HEAP_TYPE_DEFAULT && !r.buffer)) return E_INVALIDARG;
    }
    if (!srvValid(device, r)) return E_INVALIDARG;
    if (memoize) validated[validatedCount++] = &r;
  }
  return S_OK;
}
}

HRESULT NativeDescriptorVersion::create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                        std::span<const NativeViewRecipe> recipes,
                                        NativeDescriptorVersion& output) {
  const HRESULT valid = validateViews(device, type, recipes);
  if (FAILED(valid)) return valid;
  try {
    auto d = std::make_shared<NativeDescriptorAllocation>(); d->device = device; d->type = type;
    d->recipes.assign(recipes.begin(), recipes.end());
    D3D12_DESCRIPTOR_HEAP_DESC desc{type, UINT(recipes.size()), D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&d->heap));
    if (FAILED(hr)) return hr;
    d->stride = device->GetDescriptorHandleIncrementSize(type);
    auto handle = d->heap->GetCPUDescriptorHandleForHeapStart();
    for (const auto& recipe : d->recipes) {
      if (type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) device->CreateSampler(&recipe.sampler, handle);
      else device->CreateShaderResourceView(recipe.buffer ? recipe.buffer.resource() : recipe.resource.Get(), &recipe.srv, handle);
      handle.ptr += d->stride;
    }
    output.m_descriptors = std::move(d); return S_OK;
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
ID3D12DescriptorHeap* NativeDescriptorVersion::heap() const { return m_descriptors ? m_descriptors->heap.Get() : nullptr; }
ID3D12Device* NativeDescriptorVersion::device() const { return m_descriptors ? m_descriptors->device.Get() : nullptr; }
D3D12_DESCRIPTOR_HEAP_TYPE NativeDescriptorVersion::type() const {
  return m_descriptors ? m_descriptors->type : D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
}
uint32_t NativeDescriptorVersion::count() const { return m_descriptors ? uint32_t(m_descriptors->recipes.size()) : 0; }
D3D12_GPU_DESCRIPTOR_HANDLE NativeDescriptorVersion::gpu(uint32_t first) const {
  if (!m_descriptors || first >= count()) return {};
  auto h = m_descriptors->heap->GetGPUDescriptorHandleForHeapStart();
  h.ptr += (uint64_t(m_descriptors->firstDescriptor) + first) * m_descriptors->stride;
  return h;
}
std::weak_ptr<const void> NativeDescriptorVersion::ownership() const { return m_descriptors; }
uint64_t NativeDescriptorVersion::pageSerial() const {
  return m_descriptors && m_descriptors->page ? m_descriptors->page->serial : 0;
}

HRESULT NativeDescriptorArena::initialize(ID3D12Device* device, uint32_t pageDescriptors,
                                         uint32_t maxPages) {
  if (!device || !pageDescriptors || pageDescriptors > 1000000 || !maxPages ||
      uint64_t(pageDescriptors) * maxPages > 1000000) return E_INVALIDARG;
  std::lock_guard lock(m_mutex);
  if (m_state) return m_state->device.Get() == device && m_state->pageDescriptors == pageDescriptors &&
      m_state->maxPages == maxPages ? S_FALSE : E_UNEXPECTED;
  try {
    auto state = std::make_shared<NativeDescriptorArenaState>();
    state->device = device; state->pageDescriptors = pageDescriptors; state->maxPages = maxPages;
    state->stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        pageDescriptors * maxPages, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&state->heap));
    if (FAILED(hr)) return hr;
    state->pages.resize(maxPages);
    m_state = std::move(state);
    return S_OK;
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT NativeDescriptorArena::create(std::span<const NativeViewRecipe> recipes,
                                     NativeDescriptorVersion& output) {
  std::lock_guard lock(m_mutex);
  if (!m_state) return E_ABORT;
  const HRESULT valid = validateViews(m_state->device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, recipes);
  if (FAILED(valid)) return valid;
  if (recipes.size() > m_state->pageDescriptors) return E_OUTOFMEMORY;
  try {
    auto version = std::make_shared<NativeDescriptorAllocation>();
    version->device = m_state->device; version->type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    version->stride = m_state->stride;
    version->recipes.assign(recipes.begin(), recipes.end());
    auto page = m_state->tail;
    if (!page || recipes.size() > m_state->pageDescriptors - page->used) {
      // Only the arena's tail owner may be dropped here. A recorded version
      // keeps its page and budget slot live until checked queue completion.
      m_state->tail.reset(); page.reset();
      auto slot = std::find_if(m_state->pages.begin(), m_state->pages.end(),
                              [](const auto& p) { return p.expired(); });
      if (slot == m_state->pages.end()) return E_OUTOFMEMORY;
      // An expired slot's range is free: its last version retired after
      // checked queue completion, so the GPU no longer reads it.
      page = std::make_shared<NativeDescriptorPage>();
      page->heap = m_state->heap;
      page->base = uint32_t(slot - m_state->pages.begin()) * m_state->pageDescriptors;
      page->serial = ++m_state->serials;
      *slot = page; m_state->tail = page;
    }
    version->heap = page->heap; version->page = page; version->firstDescriptor = page->base + page->used;
    auto cpu = page->heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(version->firstDescriptor) * version->stride;
    for (const auto& recipe : recipes) {
      m_state->device->CreateShaderResourceView(recipe.buffer ? recipe.buffer.resource() : recipe.resource.Get(),
                                               &recipe.srv, cpu);
      cpu.ptr += version->stride;
    }
    page->used += uint32_t(recipes.size());
    output.m_descriptors = std::move(version);
    return S_OK;
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

size_t NativeDescriptorArena::livePages() const {
  std::lock_guard lock(m_mutex);
  if (!m_state) return 0;
  return std::count_if(m_state->pages.begin(), m_state->pages.end(), [](const auto& p) { return !p.expired(); });
}

uint64_t NativeDescriptorArena::tailSerial() const {
  std::lock_guard lock(m_mutex);
  return m_state && m_state->tail ? m_state->tail->serial : 0;
}

void NativeDescriptorArena::discardTail() {
  std::lock_guard lock(m_mutex);
  if (m_state) m_state->tail.reset();
}

HRESULT NativeDrawRecording::begin(ID3D12Device* device) {
  if (!device) return E_INVALIDARG;
  if (m_phase != Phase::Empty) return E_UNEXPECTED;
  try {
    auto payload = std::make_shared<NativeDrawPayload>();
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(hr)) return hr;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands));
    if (FAILED(hr)) return hr;
    m_device = device; m_allocator = std::move(allocator); m_commands = std::move(commands);
    m_payload = std::move(payload); m_phase = Phase::Recording;
    m_serial = g_recordingSerial.fetch_add(1, std::memory_order_relaxed) + 1;  // R1a: never 0, never reused
    return S_OK;
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
ID3D12GraphicsCommandList* NativeDrawRecording::commands() const { return m_phase == Phase::Recording ? m_commands.Get() : nullptr; }
HRESULT NativeDrawRecording::keep(ID3D12Resource* resource) {
  if (m_phase != Phase::Recording) return E_ABORT;
  if (!resource) return E_INVALIDARG;
  // A retained resource already passed validation. Its strong owner keeps its
  // device and heap identity fixed, so repeated references need no COM queries.
  for (const auto& retained : m_payload->resources)
    if (retained.Get() == resource) return S_FALSE;
  if (!sameDevice(resource, m_device.Get())) return E_INVALIDARG;
  D3D12_HEAP_PROPERTIES heap{}; D3D12_HEAP_FLAGS flags{};
  if (FAILED(resource->GetHeapProperties(&heap, &flags)) || heap.Type == D3D12_HEAP_TYPE_UPLOAD) return E_INVALIDARG;
  try { m_payload->resources.emplace_back(resource); return S_OK; } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
HRESULT NativeDrawRecording::keep(const NativeBufferSlice& buffer) {
  if (m_phase != Phase::Recording) return E_ABORT;
  if (!buffer || buffer.device() != m_device.Get()) return E_INVALIDARG;
#if SR_ORACLE
  const int mutant = g_uploadKeepMutant.load(std::memory_order_relaxed);
  if (mutant == 2) return S_OK;   // CONTROL release-at-slice-death: nothing retained
#endif
  if (renderKeepLean()) {
    // R1a F2b: the slice owns its page; one retained slice per page covers every slice on it (append-only offsets,
    // released only with the payload after the fence). The stamp is written after the push succeeded.
    if (buffer.pageRetainedIn(m_serial)) return S_OK;
#if SR_ORACLE
    if (mutant == 1) { buffer.markPageRetainedIn(m_serial); return S_OK; }   // CONTROL skip-page-keep
#endif
    try { m_payload->uploads.push_back(buffer); } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    buffer.markPageRetainedIn(m_serial);
    return S_OK;
  }
  // Consecutive draws often bind the same staged range: it is already retained.
  if (!m_payload->uploads.empty() && m_payload->uploads.back().sameStorage(buffer)) return S_OK;
#if SR_ORACLE
  if (mutant == 1) return S_OK;   // CONTROL skip-page-keep (=0: the slice's own keep skipped)
#endif
  try { m_payload->uploads.push_back(buffer); return S_OK; } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
HRESULT NativeDrawRecording::keep(const NativeImageLease& image) {
  if (m_phase != Phase::Recording) return E_ABORT;
  if (!image || image.device() != m_device.Get()) return E_INVALIDARG;
  // R1a F1: one lease per image allocation per recording (the payload and the submission's copy hold it to the
  // same collect as every per-bind copy did). The stamp is written after the push succeeded.
  const bool lean = renderKeepLean();
  if (lean && image.retainedIn(m_serial)) return S_OK;
  try { m_payload->images.push_back(image); } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
  if (lean) image.markRetainedIn(m_serial);
  return S_OK;
}

HRESULT NativeDrawRecording::keep(const NativeDescriptorVersion& descriptors) {
  if (m_phase != Phase::Recording) return E_ABORT;
  if (!descriptors || descriptors.device() != m_device.Get()) return E_INVALIDARG;
  // One immutable version may be rebound repeatedly in a recording. Retain it
  // once; different ranges on one page still have independent resource owners.
  const void* const id = descriptors.m_descriptors.get();
  try {
    if (m_payload->descriptorIds.contains(id)) return S_OK;
    m_payload->descriptors.push_back(descriptors);
    m_payload->descriptorIds.insert(id);  // on failure a later keep only retains it twice
    return S_OK;
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT NativeDrawRecording::draw(const PreparedNativeDraw& d) {
  if (m_phase != Phase::Recording) return E_ABORT;
  const auto* p = d.pipeline.description();
  if (!p || !d.signature || d.signature.device() != m_device.Get() ||
      p->rootSignature.Get() != d.signature.root() || !d.count || !finiteViewport(d.viewport) ||
      topologyType(d.topology) != p->topology || d.scissor.right <= d.scissor.left ||
      d.scissor.bottom <= d.scissor.top || d.bindings.size() != d.signature.m_root->rules.size()) return E_INVALIDARG;
  std::array<bool, 64> seen{};
  ID3D12DescriptorHeap* heaps[2]{};
  for (const auto& b : d.bindings) {
    if (b.parameter >= d.bindings.size() || seen[b.parameter]) return E_INVALIDARG;
    seen[b.parameter] = true;
    const auto& rule = d.signature.m_root->rules[b.parameter];
    if (rule.kind == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
      if (!b.table || b.buffer || !b.constants.empty() || b.table.device() != m_device.Get() ||
          b.table.type() != rule.heap || b.descriptorCount < rule.count ||
          b.firstDescriptor >= b.table.count() || b.descriptorCount > b.table.count() - b.firstDescriptor) return E_INVALIDARG;
      const uint32_t slot = rule.heap == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 1 : 0;
      if (heaps[slot] && heaps[slot] != b.table.heap()) return E_INVALIDARG;
      heaps[slot] = b.table.heap();
    } else if (rule.kind == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
      if (b.buffer || b.table || b.constants.size() != rule.count) return E_INVALIDARG;
    } else {
      if (!b.buffer || b.table || !b.constants.empty() || b.buffer.device() != m_device.Get() ||
          b.buffer.size() < rule.requiredBufferBytes ||
          (rule.kind == D3D12_ROOT_PARAMETER_TYPE_CBV &&
           (b.buffer.address() % 256 || b.buffer.size() % 256 || b.buffer.size() > 65536))) return E_INVALIDARG;
      if (rule.kind == D3D12_ROOT_PARAMETER_TYPE_CBV) {
        const uint32_t padded = (rule.requiredBufferBytes + 255u) & ~255u;
        if (b.buffer.size() < padded) return E_INVALIDARG;
        if (!b.buffer.hasZeroPadding(rule.requiredBufferBytes, padded)) return E_INVALIDARG;
      } else if (b.buffer.address() % 16) return E_INVALIDARG;
      // Immutable upload storage cannot serve a writable root UAV.
      if (rule.kind == D3D12_ROOT_PARAMETER_TYPE_UAV) return E_INVALIDARG;
    }
  }
  D3D12_CPU_DESCRIPTOR_HANDLE views[8]{};
  uint32_t minWidth = UINT32_MAX, minHeight = UINT32_MAX;
  for (uint32_t i = 0; i < 8; ++i) {
    if (i >= p->colorCount) { if (d.colors[i]) return E_INVALIDARG; continue; }
    const auto* c = d.colors[i].description();
    if (!c || c->kind != NativeImageKind::Color || c->clear.Format != p->colorFormats[i] ||
        c->samples != p->samples.Count || c->sampleQuality != p->samples.Quality ||
        d.colors[i].device() != m_device.Get()) return E_INVALIDARG;
    views[i] = d.colors[i].view();
    if (d.colors[i].isNullColor()) {
      const auto& blend = p->blend.RenderTarget[p->blend.IndependentBlendEnable ? i : 0];
      if (blend.RenderTargetWriteMask) return E_INVALIDARG;
    } else { minWidth = std::min(minWidth, c->width); minHeight = std::min(minHeight, c->height); }
  }
  if (p->depthFormat != DXGI_FORMAT_UNKNOWN) {
    const auto* z = d.depth.description();
    if (!z || z->kind != NativeImageKind::DepthStencil || z->clear.Format != p->depthFormat ||
        z->samples != p->samples.Count || z->sampleQuality != p->samples.Quality ||
        !sameDevice(d.depth.resource(), m_device.Get())) return E_INVALIDARG;
    minWidth = std::min(minWidth, z->width); minHeight = std::min(minHeight, z->height);
  } else if (d.depth) return E_INVALIDARG;
  if (int64_t(d.scissor.right) > minWidth || int64_t(d.scissor.bottom) > minHeight) return E_INVALIDARG;
  if (d.kind == NativeDrawKind::DecodedVertices) {
    if (!d.vertices || d.indices || d.vertices.device() != m_device.Get() || !d.vertexStride ||
        d.vertexStride > D3D12_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES ||
        d.vertices.size() > UINT32_MAX || uint64_t(d.first) + d.count > d.vertices.size() / d.vertexStride) return E_INVALIDARG;
    for (const auto& element : p->inputLayout) {
      const auto size = elementBytes(element.format);
      // Native APPEND rules include format alignment. Require explicit offsets
      // until the adapter resolves the complete native layout correctly.
      if (element.byteOffset == D3D12_APPEND_ALIGNED_ELEMENT) return E_INVALIDARG;
      const uint32_t offset = element.byteOffset;
      if (!size || element.inputSlot || element.classification != D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA ||
          offset > d.vertexStride || size > d.vertexStride - offset) return E_INVALIDARG;
    }
  } else {
    if (d.vertices || !p->inputLayout.empty()) return E_INVALIDARG;
    if (d.kind == NativeDrawKind::FetchIndexedVertices) {
      const uint32_t width = d.indexFormat == DXGI_FORMAT_R32_UINT ? 4 : d.indexFormat == DXGI_FORMAT_R16_UINT ? 2 : 0;
      if (!width || !d.indices || d.indices.device() != m_device.Get() || d.indices.address() % width ||
          d.indices.size() > UINT32_MAX || uint64_t(d.first) + d.count > d.indices.size() / width) return E_INVALIDARG;
    } else if (d.kind != NativeDrawKind::FetchVertices || d.indices || uint64_t(d.first) + d.count > UINT32_MAX) return E_INVALIDARG;
  }
  // Deep copy and retain everything before the first native recording command.
  // No potentially throwing allocation or discovery occurs after this point.
  try {
    PreparedNativeDraw retained = d;
    const size_t imageCount = p->colorCount + (d.depth ? 1u : 0u);
    const auto reserveGrowth = [](auto& values, size_t extra) {
      if (extra > values.max_size() - values.size()) return false;
      const size_t needed = values.size() + extra;
      if (needed <= values.capacity()) return true;
      const size_t grown = values.capacity() > values.max_size() / 2 ? values.max_size() :
          std::max<size_t>(32, values.capacity() * 2);
      values.reserve(std::max(needed, std::min(values.max_size(), grown)));
      return true;
    };
    if (!reserveGrowth(m_payload->draws, 1) || !reserveGrowth(m_payload->images, imageCount)) return E_OUTOFMEMORY;
    m_payload->draws.push_back(std::move(retained));
    // R1a F1: as keep(image) - the capacity reserved above makes these pushes non-throwing, so each stamp follows
    // its push.
    const bool lean = renderKeepLean();
    const auto retain = [&](const NativeImageLease& lease) {
      if (lean && lease.retainedIn(m_serial)) return;
      m_payload->images.push_back(lease);
      if (lean) lease.markRetainedIn(m_serial);
    };
    for (uint32_t i = 0; i < p->colorCount; ++i) retain(d.colors[i]);
    if (d.depth) retain(d.depth);
  } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
  ID3D12DescriptorHeap* bound[2]{}; uint32_t heapCount = 0;
  for (auto* heap : heaps) if (heap) bound[heapCount++] = heap;
  m_commands->SetDescriptorHeaps(heapCount, heapCount ? bound : nullptr);
  m_commands->SetGraphicsRootSignature(d.signature.root());
  m_commands->SetPipelineState(d.pipeline.pipeline());
  const D3D12_CPU_DESCRIPTOR_HANDLE depthView = d.depth.view();
  m_commands->OMSetRenderTargets(p->colorCount, p->colorCount ? views : nullptr, FALSE, d.depth ? &depthView : nullptr);
  m_commands->OMSetStencilRef(d.stencilReference);
  m_commands->RSSetViewports(1, &d.viewport); m_commands->RSSetScissorRects(1, &d.scissor);
  for (const auto& b : d.bindings) {
    switch (d.signature.m_root->rules[b.parameter].kind) {
      case D3D12_ROOT_PARAMETER_TYPE_CBV: m_commands->SetGraphicsRootConstantBufferView(b.parameter, b.buffer.address()); break;
      case D3D12_ROOT_PARAMETER_TYPE_SRV: m_commands->SetGraphicsRootShaderResourceView(b.parameter, b.buffer.address()); break;
      case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
        m_commands->SetGraphicsRoot32BitConstants(b.parameter, UINT(b.constants.size()), b.constants.data(), 0); break;
      case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
        m_commands->SetGraphicsRootDescriptorTable(b.parameter, b.table.gpu(b.firstDescriptor)); break;
      default: break;  // Writable UAV inputs were rejected before recording.
    }
  }
  m_commands->IASetPrimitiveTopology(d.topology);
  if (d.kind == NativeDrawKind::DecodedVertices) {
    const D3D12_VERTEX_BUFFER_VIEW view{d.vertices.address(), UINT(d.vertices.size()), d.vertexStride};
    m_commands->IASetVertexBuffers(0, 1, &view); m_commands->DrawInstanced(d.count, 1, d.first, 0);
  } else if (d.kind == NativeDrawKind::FetchIndexedVertices) {
    const D3D12_INDEX_BUFFER_VIEW view{d.indices.address(), UINT(d.indices.size()), d.indexFormat};
    m_commands->IASetIndexBuffer(&view); m_commands->DrawIndexedInstanced(d.count, 1, d.first, 0, 0);
  } else m_commands->DrawInstanced(d.count, 1, d.first, 0);
  return S_OK;
}
HRESULT NativeDrawRecording::close() {
  if (m_phase != Phase::Recording) return E_ABORT;
  const HRESULT hr = m_commands->Close();
  m_phase = SUCCEEDED(hr) ? Phase::Closed : Phase::Spent; return hr;
}
HRESULT NativeDrawRecording::submit(NativeImageStore& queue, uint64_t& submission) {
  if (m_phase != Phase::Closed) return E_ABORT;
  const std::array<std::shared_ptr<const void>, 1> owners{m_payload};
  m_phase = Phase::Spent;
  return queue.submit(m_commands.Get(), m_allocator.Get(), m_payload->images, submission, owners);
}
#if SR_ORACLE
size_t NativeDrawRecording::ownershipMetadataBytes() {
#if defined(_MSVC_STL_VERSION)
  // begin() uses make_shared<NativeDrawPayload>; its vector/set allocations
  // are destroyed at strong-count zero. Weak ownership retains only this block.
  static_assert(sizeof(std::_Ref_count_obj2<NativeDrawPayload>) <= 1024);
  return sizeof(std::_Ref_count_obj2<NativeDrawPayload>);
#else
  return 0;
#endif
}
#endif
size_t NativeDrawRecording::drawCount() const { return m_payload ? m_payload->draws.size() : 0; }
size_t NativeDrawRecording::retainedImageCount() const { return m_payload ? m_payload->images.size() : 0; }
size_t NativeDrawRecording::retainedUploadCount() const { return m_payload ? m_payload->uploads.size() : 0; }
bool NativeDrawRecording::payloadRetainsUpload(const std::weak_ptr<const void>& payload,
                                               const std::weak_ptr<const void>& owner) {
  const auto held = std::static_pointer_cast<const NativeDrawPayload>(payload.lock());
  if (!held) return false;
  for (const auto& upload : held->uploads) {
    const std::weak_ptr<const void> retained = upload.ownership();
    if (!retained.owner_before(owner) && !owner.owner_before(retained)) return true;   // the same control block
  }
  return false;
}
#if SR_ORACLE
void NativeDrawRecording::uploadKeepMutantForTest(int mutant) {
  g_uploadKeepMutant.store(mutant, std::memory_order_relaxed);
}
#endif
std::weak_ptr<const void> NativeDrawRecording::ownership() const { return m_payload; }
}  // namespace sr::gfx
