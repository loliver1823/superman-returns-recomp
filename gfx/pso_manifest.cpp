// Step 4 item 2k(3b): the pipeline manifest's file format (see pso_manifest.hpp). Little-endian, every field a
// u32 (or u64 / raw bytes), written field by field - no struct padding reaches the file.
#include "pso_manifest.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace sr::gfx::psom {

namespace {
constexpr char kMagic[8] = {'S', 'R', 'P', 'S', 'O', 'M', 'F', '1'};
constexpr uint32_t kMaxShaders = 262144;
constexpr uint32_t kMaxShaderBytes = 64u * 1024u * 1024u;

// The state fields, in one order for the file and the identity (NativePipelineCache::makeKey's fields).
template <class F>
void stateFields(const NativePipelineDesc& d, F&& f) {
  for (auto format : d.colorFormats) f(uint32_t(format));
  f(d.colorCount); f(uint32_t(d.depthFormat)); f(d.samples.Count); f(d.samples.Quality);
  f(d.sampleMask); f(d.nodeMask); f(uint32_t(d.topology)); f(uint32_t(d.stripCut)); f(uint32_t(d.flags));
  const auto& r = d.rasterizer;
  f(uint32_t(r.FillMode)); f(uint32_t(r.CullMode)); f(uint32_t(r.FrontCounterClockwise));
  f(uint32_t(r.DepthBias)); f(std::bit_cast<uint32_t>(r.DepthBiasClamp)); f(std::bit_cast<uint32_t>(r.SlopeScaledDepthBias));
  f(uint32_t(r.DepthClipEnable)); f(uint32_t(r.MultisampleEnable)); f(uint32_t(r.AntialiasedLineEnable));
  f(r.ForcedSampleCount); f(uint32_t(r.ConservativeRaster));
  f(uint32_t(d.blend.AlphaToCoverageEnable)); f(uint32_t(d.blend.IndependentBlendEnable));
  for (const auto& b : d.blend.RenderTarget) {
    f(uint32_t(b.BlendEnable)); f(uint32_t(b.LogicOpEnable)); f(uint32_t(b.SrcBlend)); f(uint32_t(b.DestBlend));
    f(uint32_t(b.BlendOp)); f(uint32_t(b.SrcBlendAlpha)); f(uint32_t(b.DestBlendAlpha)); f(uint32_t(b.BlendOpAlpha));
    f(uint32_t(b.LogicOp)); f(uint32_t(b.RenderTargetWriteMask));
  }
  const auto& z = d.depthStencil;
  f(uint32_t(z.DepthEnable)); f(uint32_t(z.DepthWriteMask)); f(uint32_t(z.DepthFunc));
  f(uint32_t(z.StencilEnable)); f(uint32_t(z.StencilReadMask)); f(uint32_t(z.StencilWriteMask));
  for (const auto& face : {z.FrontFace, z.BackFace}) {
    f(uint32_t(face.StencilFailOp)); f(uint32_t(face.StencilDepthFailOp)); f(uint32_t(face.StencilPassOp));
    f(uint32_t(face.StencilFunc));
  }
}
constexpr size_t kStateWords = 8 + 9 + 11 + 2 + 8 * 10 + 6 + 8;

// Reads the state fields back in stateFields' order.
bool readState(NativePipelineDesc& d, const uint32_t* w) {
  size_t i = 0;
  const auto n = [&]() { return w[i++]; };
  for (auto& format : d.colorFormats) format = DXGI_FORMAT(n());
  d.colorCount = n(); d.depthFormat = DXGI_FORMAT(n()); d.samples.Count = n(); d.samples.Quality = n();
  d.sampleMask = n(); d.nodeMask = n(); d.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE(n());
  d.stripCut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE(n()); d.flags = D3D12_PIPELINE_STATE_FLAGS(n());
  auto& r = d.rasterizer;
  r.FillMode = D3D12_FILL_MODE(n()); r.CullMode = D3D12_CULL_MODE(n()); r.FrontCounterClockwise = BOOL(n());
  r.DepthBias = INT(n()); r.DepthBiasClamp = std::bit_cast<float>(n()); r.SlopeScaledDepthBias = std::bit_cast<float>(n());
  r.DepthClipEnable = BOOL(n()); r.MultisampleEnable = BOOL(n()); r.AntialiasedLineEnable = BOOL(n());
  r.ForcedSampleCount = n(); r.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE(n());
  d.blend.AlphaToCoverageEnable = BOOL(n()); d.blend.IndependentBlendEnable = BOOL(n());
  for (auto& b : d.blend.RenderTarget) {
    b.BlendEnable = BOOL(n()); b.LogicOpEnable = BOOL(n()); b.SrcBlend = D3D12_BLEND(n()); b.DestBlend = D3D12_BLEND(n());
    b.BlendOp = D3D12_BLEND_OP(n()); b.SrcBlendAlpha = D3D12_BLEND(n()); b.DestBlendAlpha = D3D12_BLEND(n());
    b.BlendOpAlpha = D3D12_BLEND_OP(n()); b.LogicOp = D3D12_LOGIC_OP(n()); b.RenderTargetWriteMask = UINT8(n());
  }
  auto& z = d.depthStencil;
  z.DepthEnable = BOOL(n()); z.DepthWriteMask = D3D12_DEPTH_WRITE_MASK(n()); z.DepthFunc = D3D12_COMPARISON_FUNC(n());
  z.StencilEnable = BOOL(n()); z.StencilReadMask = UINT8(n()); z.StencilWriteMask = UINT8(n());
  for (auto* face : {&z.FrontFace, &z.BackFace}) {
    face->StencilFailOp = D3D12_STENCIL_OP(n()); face->StencilDepthFailOp = D3D12_STENCIL_OP(n());
    face->StencilPassOp = D3D12_STENCIL_OP(n()); face->StencilFunc = D3D12_COMPARISON_FUNC(n());
  }
  return i == kStateWords && d.colorCount <= d.colorFormats.size();
}

const NativeShaderCode& stage(const NativePipelineDesc& d, int i) {
  switch (i) {
    case 0: return d.vertex;
    case 1: return d.pixel;
    case 2: return d.geometry;
    case 3: return d.hull;
    default: return d.domain;
  }
}
NativeShaderCode& stage(NativePipelineDesc& d, int i) {
  return const_cast<NativeShaderCode&>(stage(static_cast<const NativePipelineDesc&>(d), i));
}

struct Writer {
  std::vector<uint8_t> out;
  void u32(uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i))); }
  void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
  void raw(const void* p, size_t n) { out.insert(out.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
};
struct Reader {
  std::span<const uint8_t> in;
  size_t at = 0;
  bool ok = true;
  bool need(size_t n) { if (!ok || in.size() - at < n) ok = false; return ok; }
  uint32_t u32() {
    if (!need(4)) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(in[at + i]) << (8 * i);
    at += 4;
    return v;
  }
  uint64_t u64() { const uint64_t lo = u32(); return lo | (uint64_t(u32()) << 32); }
  std::span<const uint8_t> raw(size_t n) {
    if (!need(n)) return {};
    auto s = in.subspan(at, n);
    at += n;
    return s;
  }
};
uint64_t fnv(uint64_t h, uint64_t v) {
  for (int i = 0; i < 8; ++i) h = (h ^ uint8_t(v >> (8 * i))) * 1099511628211ull;
  return h;
}
}  // namespace

uint64_t identity(const NativePipelineDesc& d) {
  uint64_t h = 14695981039346656037ull;
  for (int i = 0; i < 5; ++i) h = fnv(h, stage(d, i).hash());
  stateFields(d, [&](uint32_t v) { h = fnv(h, v); });
  h = fnv(h, d.inputLayout.size());
  for (const auto& e : d.inputLayout) {
    for (unsigned char c : e.semantic) h = fnv(h, c);
    h = fnv(h, e.semanticIndex); h = fnv(h, uint32_t(e.format)); h = fnv(h, e.inputSlot); h = fnv(h, e.byteOffset);
    h = fnv(h, uint32_t(e.classification)); h = fnv(h, e.instanceStepRate);
  }
  return h;
}

namespace {
// fix12 (Astra on fix11): the writer enforces the reader's limits (parse below) - what it writes, parse accepts.
bool entryFits(const NativePipelineDesc& d) {
  if (!d.vertex || d.inputLayout.size() > 32) return false;
  for (int i = 0; i < 5; ++i)
    if (stage(d, i).bytes().size() > kMaxShaderBytes) return false;
  for (const auto& e : d.inputLayout)
    if (e.semantic.empty() || e.semantic.size() > 255) return false;
  std::vector<uint32_t> words;
  words.reserve(kStateWords);
  stateFields(d, [&](uint32_t v) { words.push_back(v); });
  NativePipelineDesc back;
  return words.size() == kStateWords && readState(back, words.data());
}
}  // namespace

std::vector<uint8_t> serialize(const std::vector<const NativePipelineDesc*>& all, std::string* why, size_t* skipped) {
  if (skipped) *skipped = 0;
  const auto fail = [&](const char* reason) {
    if (why) *why = reason;
    return std::vector<uint8_t>{};
  };
  std::vector<const NativePipelineDesc*> descs;   // an entry parse() would reject is left out, never the whole file
  descs.reserve(all.size());
  for (const auto* d : all)
    if (d && entryFits(*d)) descs.push_back(d);
  if (skipped) *skipped = all.size() - descs.size();
  if (descs.size() > kMaxEntries) return fail("more pipelines than the reader's entry limit");
  std::vector<const NativeShaderCode*> shaders;
  std::unordered_map<uint64_t, uint32_t> index;   // content hash -> table slot (equal hash = equal bytes, as in the cache)
  uint64_t size = sizeof(kMagic) + 12;            // the reader's byte cap, counted before anything is written
  const auto slot = [&](const NativeShaderCode& code) -> int32_t {
    if (!code) return -1;
    const auto [it, fresh] = index.try_emplace(code.hash(), uint32_t(shaders.size()));
    if (fresh) {
      shaders.push_back(&code);
      size += 12 + code.bytes().size();
    }
    return int32_t(it->second);
  };
  std::vector<std::array<int32_t, 5>> stages;
  stages.reserve(descs.size());
  for (const auto* d : descs) {
    std::array<int32_t, 5> s{};
    for (int i = 0; i < 5; ++i) s[i] = slot(stage(*d, i));
    stages.push_back(s);
    size += 4 * (5 + 1 + kStateWords);
    for (const auto& e : d->inputLayout) size += 4 + e.semantic.size() + 24;
  }
  if (shaders.size() > kMaxShaders) return fail("more shaders than the reader's shader limit");
  if (size > kMaxFileBytes) return fail("larger than the reader's 64 MB manifest cap");
  Writer w;
  w.out.reserve(size_t(size));
  w.raw(kMagic, sizeof(kMagic));
  w.u32(kVersion);
  w.u32(uint32_t(shaders.size()));
  w.u32(uint32_t(descs.size()));
  for (const auto* code : shaders) {
    const auto bytes = code->bytes();
    w.u64(code->hash());
    w.u32(uint32_t(bytes.size()));
    w.raw(bytes.data(), bytes.size());
  }
  for (size_t k = 0; k < descs.size(); ++k) {
    const auto& d = *descs[k];
    for (int32_t s : stages[k]) w.u32(uint32_t(s));
    w.u32(uint32_t(d.inputLayout.size()));
    for (const auto& e : d.inputLayout) {
      w.u32(uint32_t(e.semantic.size()));
      w.raw(e.semantic.data(), e.semantic.size());
      w.u32(e.semanticIndex); w.u32(uint32_t(e.format)); w.u32(e.inputSlot); w.u32(e.byteOffset);
      w.u32(uint32_t(e.classification)); w.u32(e.instanceStepRate);
    }
    stateFields(d, [&](uint32_t v) { w.u32(v); });
  }
  return std::move(w.out);
}

namespace {
bool parseImpl(std::span<const uint8_t> bytes, std::vector<NativePipelineDesc>& out, std::string* why);
}  // namespace

// Astra (10:5x) #3: the input is bounded by kMaxFileBytes; every count is checked against the bytes left BEFORE it
// sizes an allocation, and an allocation failure rejects the file (never terminates the game).
bool parse(std::span<const uint8_t> bytes, std::vector<NativePipelineDesc>& out, std::string* why) {
  if (bytes.size() > kMaxFileBytes) {
    if (why) *why = "larger than the 64 MB manifest cap";
    out.clear();
    return false;
  }
  try {
    return parseImpl(bytes, out, why);
  } catch (const std::bad_alloc&) {
    out.clear();
    if (why) *why = "out of memory while parsing";
    return false;
  }
}

namespace {
bool parseImpl(std::span<const uint8_t> bytes, std::vector<NativePipelineDesc>& out, std::string* why) {
  out.clear();
  const auto fail = [&](const char* reason) {
    if (why) *why = reason;
    out.clear();
    return false;
  };
  Reader r{bytes};
  const auto magic = r.raw(sizeof(kMagic));
  if (!r.ok || std::memcmp(magic.data(), kMagic, sizeof(kMagic)) != 0) return fail("not a pipeline manifest");
  if (r.u32() != kVersion) return fail("another manifest version");
  const uint32_t nShaders = r.u32(), nEntries = r.u32();
  if (!r.ok || nShaders > kMaxShaders || nEntries > kMaxEntries) return fail("counts out of range");
  // each shader needs >= 13 bytes (hash, size, one byte), each entry >= (5 + 1 + kStateWords) words: a count the
  // remaining bytes cannot hold is rejected before it sizes a vector
  const uint64_t left = bytes.size() - r.at;
  if (uint64_t(nShaders) * 13u > left ||
      uint64_t(nEntries) * (uint64_t(6 + kStateWords) * 4u) > left)
    return fail("counts larger than the file");
  std::vector<NativeShaderCode> shaders(nShaders);
  for (auto& code : shaders) {
    const uint64_t hash = r.u64();
    const uint32_t size = r.u32();
    if (!r.ok || !size || size > kMaxShaderBytes) return fail("shader size out of range");
    const auto data = r.raw(size);
    if (!r.ok || FAILED(NativeShaderCode::copy(data, code)) || code.hash() != hash)
      return fail("shader bytes do not match their hash");
  }
  out.reserve(nEntries);
  std::vector<uint32_t> words(kStateWords);
  for (uint32_t k = 0; k < nEntries; ++k) {
    NativePipelineDesc d;
    for (int i = 0; i < 5; ++i) {
      const int32_t s = int32_t(r.u32());
      if (s < -1 || s >= int32_t(nShaders)) return fail("shader index out of range");
      if (s >= 0) stage(d, i) = shaders[size_t(s)];
    }
    const uint32_t nInput = r.u32();
    if (!r.ok || nInput > 32) return fail("input layout out of range");
    d.inputLayout.resize(nInput);
    for (auto& e : d.inputLayout) {
      const uint32_t len = r.u32();
      if (!r.ok || !len || len > 255) return fail("semantic out of range");
      const auto name = r.raw(len);
      if (!r.ok) return fail("truncated");
      e.semantic.assign((const char*)name.data(), name.size());
      e.semanticIndex = r.u32(); e.format = DXGI_FORMAT(r.u32()); e.inputSlot = r.u32(); e.byteOffset = r.u32();
      e.classification = D3D12_INPUT_CLASSIFICATION(r.u32()); e.instanceStepRate = r.u32();
    }
    for (auto& v : words) v = r.u32();
    if (!r.ok) return fail("truncated");
    if (!readState(d, words.data())) return fail("state out of range");
    if (!d.vertex) return fail("an entry without a vertex shader");
    out.push_back(std::move(d));
  }
  if (r.at != bytes.size()) return fail("trailing bytes");
  return true;
}
}  // namespace

std::vector<const NativePipelineDesc*> merge(const std::vector<NativePipelineDesc>& existing,
                                             const std::vector<const NativePipelineDesc*>& added) {
  std::vector<const NativePipelineDesc*> out;
  std::unordered_set<uint64_t> seen;
  for (const auto& d : existing)
    if (out.size() < kMaxEntries && seen.insert(identity(d)).second) out.push_back(&d);
  for (const auto* d : added)
    if (out.size() < kMaxEntries && seen.insert(identity(*d)).second) out.push_back(d);
  return out;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out, std::string* why, uint64_t maxBytes) {
  out.clear();
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (why) *why = "missing or unreadable";
    return false;
  }
  bool ok = true;
  try {
    uint8_t buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
      if (out.size() + n > maxBytes) {   // never read past the cap, never allocate for it
        ok = false;
        if (why) *why = "larger than the manifest cap";
        break;
      }
      out.insert(out.end(), buf, buf + n);
    }
    if (ok && std::ferror(f)) {
      ok = false;
      if (why) *why = "read error";
    }
  } catch (const std::bad_alloc&) {
    ok = false;
    if (why) *why = "out of memory while reading";
  }
  std::fclose(f);
  if (!ok) out.clear();
  return ok;
}

bool writeFileAtomic(const std::string& path, std::span<const uint8_t> bytes) {
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  // Astra (10:5x) #4: an instance-unique temporary, created exclusively - two writers never share one
  const std::filesystem::path tmp =
      p.string() + "." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(GetTickCount64()) + ".tmp";
  HANDLE h = CreateFileW(tmp.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  bool wrote = true;
  for (size_t at = 0; wrote && at < bytes.size();) {
    const DWORD chunk = DWORD(std::min<size_t>(bytes.size() - at, 1u << 20));
    DWORD done = 0;
    wrote = WriteFile(h, bytes.data() + at, chunk, &done, nullptr) && done == chunk;
    at += done;
  }
  const bool closed = CloseHandle(h) != 0;
  if (!wrote || !closed ||
      !MoveFileExW(tmp.wstring().c_str(), p.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::filesystem::remove(tmp, ec);
    return false;
  }
  return true;
}

bool mergeIntoFile(const std::string& path, const std::vector<NativePipelineDesc>& base,
                   const std::vector<const NativePipelineDesc*>& added, MergeResult* result, std::string* why,
                   uint32_t lockMs) {
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  const std::wstring lockPath = p.wstring() + L".lock";
  HANDLE lock = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (lock == INVALID_HANDLE_VALUE) {
    if (why) *why = "the lock file cannot be opened";
    return false;
  }
  OVERLAPPED ov{};
  bool locked = false;
  const ULONGLONG until = GetTickCount64() + lockMs;
  for (;;) {
    if (LockFileEx(lock, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
      locked = true;
      break;
    }
    if (GetTickCount64() >= until) break;
    Sleep(20);
  }
  bool ok = false;
  if (!locked) {
    if (why) *why = "another instance holds the manifest lock";
  } else {
    try {
      std::vector<uint8_t> bytes;
      std::vector<NativePipelineDesc> disk;
      std::string readWhy;
      if (readFile(path, bytes, &readWhy) && !parse(bytes, disk, &readWhy)) disk.clear();   // corrupt: rebuilt
      bytes.clear();
      // the file NOW first (other instances' entries), then this instance's boot-time read, then its new entries
      std::vector<const NativePipelineDesc*> extra;
      extra.reserve(base.size() + added.size());
      for (const auto& d : base) extra.push_back(&d);
      extra.insert(extra.end(), added.begin(), added.end());
      const auto merged = merge(disk, extra);
      // fix12 (Astra on fix11): an image over the reader's limits is never written - the previous file stays
      std::string imageWhy;
      size_t skipped = 0;
      const auto image = serialize(merged, &imageWhy, &skipped);
      if (image.empty()) {
        if (why) *why = "the merged manifest is " + imageWhy + " - the previous file is kept";
      } else {
        ok = writeFileAtomic(path, image);
        if (!ok && why) *why = "the write failed";
      }
      if (result) *result = MergeResult{disk.size(), merged.size() - skipped, image.size(), skipped};
    } catch (const std::bad_alloc&) {
      ok = false;
      if (why) *why = "out of memory while merging";
    }
    UnlockFileEx(lock, 0, 1, 0, &ov);
  }
  CloseHandle(lock);
  return ok;
}

}  // namespace sr::gfx::psom
