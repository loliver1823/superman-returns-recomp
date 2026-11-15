#include "translated_constants.hpp"
#include <cstring>

namespace sr::gfx {
HRESULT TranslatedConstantSnapshot::stage(NativeUploadArena& arena, std::span<const uint8_t> vs,
                                         std::span<const uint8_t> ps, std::span<const uint8_t> shared,
                                         std::span<const uint8_t> fetch,
                                         TranslatedConstantSnapshot& output) {
  auto floatExtent = [](size_t bytes) { return bytes && bytes <= kFloatBytes && bytes % kBlockBytes == 0; };
  if (!floatExtent(vs.size()) || !floatExtent(ps.size()) || shared.size() != kSharedBytes ||
      (!fetch.empty() && fetch.size() != kFetchBytes)) return E_INVALIDARG;
  // Written straight into the upload page (no packed intermediate copy).
  const uint64_t psAt = vs.size(), sharedAt = psAt + ps.size(), fetchAt = sharedAt + kSharedBytes;
  const NativeUploadArena::Part parts[] = {{0, vs}, {psAt, ps}, {sharedAt, shared}, {fetchAt, fetch}};
  TranslatedConstantSnapshot candidate;
  // The PS block starts last of the two float blocks: its reserve covers the VS one's.
  const HRESULT hr = arena.stageConstantParts({parts, fetch.empty() ? 3u : 4u}, fetchAt + fetch.size(),
                                              candidate.m_storage, psAt + kFloatBytes);
  if (FAILED(hr)) return hr;
  candidate.m_vsBytes = uint32_t(vs.size()); candidate.m_psBytes = uint32_t(ps.size());
  candidate.m_fetch = !fetch.empty();
  output = std::move(candidate);
  return S_OK;
}

uint32_t TranslatedConstantSnapshot::offset(uint32_t parameter) const {
  switch (parameter) {
    case 0: case 3: return 0;
    case 1: case 4: return m_vsBytes;
    case 2: return m_vsBytes + m_psBytes;
    case 9: return m_vsBytes + m_psBytes + kSharedBytes;
    default: return 0;
  }
}
uint32_t TranslatedConstantSnapshot::size(uint32_t parameter) const {
  if (!m_storage) return 0;
  switch (parameter) {
    case 0: case 3: return m_vsBytes;
    case 1: case 4: return m_psBytes;
    case 2: return kSharedBytes;
    case 9: return m_fetch ? kFetchBytes : 0;
    default: return 0;
  }
}

D3D12_GPU_VIRTUAL_ADDRESS TranslatedConstantSnapshot::address(uint32_t parameter) const {
  return size(parameter) ? m_storage.address() + offset(parameter) : 0;
}

HRESULT TranslatedConstantSnapshot::bind(NativeDrawRecording& recording) const {
  if (!m_storage) return E_INVALIDARG;
  const HRESULT hr = recording.keep(m_storage);
  if (FAILED(hr)) return hr;
  auto* commands = recording.commands();
  for (uint32_t parameter = 0; parameter < 5; ++parameter)
    commands->SetGraphicsRootConstantBufferView(parameter, address(parameter));
  if (m_fetch) commands->SetGraphicsRootConstantBufferView(9, address(9));
  return S_OK;
}
}  // namespace sr::gfx
