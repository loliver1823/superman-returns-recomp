#pragma once
#include "native_draw.hpp"
#include <list>
#include <unordered_map>

namespace sr::gfx {
// Compact version of the existing translated-shader ABI. The second half of
// the 2D table is reserved for 3D fetches that actually select a 2D texture.
// Their bindings must not alias the sixteen ordinary 2D selections.
// The raw vertex-fetch buffer is a separate one-descriptor version: it changes
// with every staged draw, while the 64 texture descriptors repeat across draws.
// Both come from one arena heap, so they bind together.
struct TranslatedTextureSnapshot {
  static constexpr uint32_t kLayeredBase = 32, kCubeBase = 48, kDescriptors = 64;
  std::array<uint32_t, 48> indices{};
  NativeDescriptorVersion descriptors, raw;
  HRESULT bind(NativeDrawRecording& recording, const NativeDescriptorVersion& samplers) const;
};

// Takes native resource/view recipes, not legacy heap indices. Cache identity
// includes the exact SRV and resource of every texture; hits are compared after
// hashing. The raw view is matched against the previous one only. The bounded
// cache and recordings own immutable page versions; tables migrate to newer
// pages as they age, so old arena pages drain (see translated_textures.cpp).
// E_OUTOFMEMORY means the arena is full of pages still owned by recordings in
// flight: clear(), wait for them, and select again.
class TranslatedTextureCache {
 public:
  HRESULT select(ID3D12Device* device, std::span<const NativeViewRecipe, 48> textures,
                 const NativeViewRecipe& raw, TranslatedTextureSnapshot& output);
  // The same, reading the caller's recipes in place: a lookup copies none of them
  // (no resource/slice owner traffic); only a miss copies them into the new entry.
  HRESULT select(ID3D12Device* device, std::span<const NativeViewRecipe* const, 48> textures,
                 const NativeViewRecipe& raw, TranslatedTextureSnapshot& output);
  // The caller proved the textures equal those of `previous` (same views): keep its
  // indices and table, and select only the raw view. No table lookup or aging.
  HRESULT reuse(ID3D12Device* device, const TranslatedTextureSnapshot& previous,
                const NativeViewRecipe& raw, TranslatedTextureSnapshot& output);
  // Page ages (see translated_textures.cpp). A caller that keeps tables for reuse()
  // drops them at the same ages, so it never pins a page the cache would drain.
  static constexpr uint64_t kMigrateAge = 6, kKeepAge = 2;
  static constexpr size_t kPressurePages = 36;
  uint64_t tailSerial() const { return m_arena.tailSerial(); }
  size_t livePages() const { return m_arena.livePages(); }
  uint64_t tableHits() const { return m_tableHits; }
  uint64_t tableMisses() const { return m_tableMisses; }
  uint64_t rawCreates() const { return m_rawCreates; }
  uint64_t tableMigrations() const { return m_tableMigrations; }
  uint64_t tableEvictions() const { return m_tableEvictions; }
  uint64_t tableReuses() const { return m_tableReuses; }
  void clear();
 private:
  struct Entry {
    std::array<NativeViewRecipe, 48> recipes;
    NativeDescriptorVersion descriptors;
    uint64_t hash = 0;
  };
  Microsoft::WRL::ComPtr<ID3D12Device> m_device;
  NativeDescriptorArena m_arena;
  // Most recently used first; m_index maps a recipe hash to its list entries.
  std::list<Entry> m_cache;
  std::unordered_multimap<uint64_t, std::list<Entry>::iterator> m_index;
  NativeViewRecipe m_lastRawRecipe;
  NativeDescriptorVersion m_lastRaw;
  uint64_t m_tableHits = 0, m_tableMisses = 0, m_rawCreates = 0, m_tableMigrations = 0, m_tableEvictions = 0,
           m_tableReuses = 0;
  HRESULT selectRaw(const NativeViewRecipe& raw, NativeDescriptorVersion& output, bool& created);
};
}  // namespace sr::gfx
