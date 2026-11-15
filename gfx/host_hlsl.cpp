// Step 4 item 7a: host HLSL compiled ahead of time (see host_hlsl.hpp).

#include "host_hlsl.hpp"

#include "../app/log.hpp"
#include "../kernel/host_work.hpp"   // step 4 2k(4): a runtime compile is host work (SR_GPU_HANG_HOST_WORK)

#include <d3dcompiler.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sr::gfx {

namespace {

struct Registry {
  std::mutex mu;
  std::vector<std::pair<const HostHlslBlob*, uint32_t>> tables;
  std::unordered_map<uint64_t, const HostHlslBlob*> byKey;
  bool indexed = false;
};
Registry& registry() {
  static Registry r;  // function-local: safe from the generated units' static initializers
  return r;
}

struct Fnv {
  uint64_t h = 1469598103934665603ull;
  void bytes(const void* p, size_t n) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) {
      h ^= b[i];
      h *= 1099511628211ull;
    }
  }
  void str(const char* s) { bytes(s ? s : "", s ? std::strlen(s) : 0); }
  void byte(unsigned char c) { bytes(&c, 1); }
};

std::atomic<uint64_t> g_embedded{0}, g_runtime{0};

}  // namespace

bool registerHostHlslTable(const HostHlslBlob* table, uint32_t count) {
  Registry& r = registry();
  std::lock_guard<std::mutex> lk(r.mu);
  r.tables.push_back({table, count});
  r.indexed = false;
  return true;
}

uint64_t hostHlslKey(const void* src, size_t len, const char* name, const D3D_SHADER_MACRO* defines,
                     const char* entry, const char* target, unsigned flags1, unsigned flags2) {
  Fnv f;
  f.str("SRHLSL1");
  f.byte(0);
  f.str(name);
  f.byte(0);
  f.str(entry);
  f.byte(0);
  f.str(target);
  f.byte(0);
  char num[32];
  std::snprintf(num, sizeof(num), "%u", flags1);
  f.str(num);
  f.byte(0);
  std::snprintf(num, sizeof(num), "%u", flags2);
  f.str(num);
  f.byte(0);
  for (const D3D_SHADER_MACRO* m = defines; m && m->Name; ++m) {
    f.str(m->Name);
    f.byte('=');
    f.str(m->Definition);
    f.byte(0);
  }
  f.byte(1);
  f.bytes(src, len);
  return f.h;
}

bool hostHlslAotOn() {
  static const bool on = [] {
    const char* e = std::getenv("SR_HOST_HLSL_AOT");
    return !e || e[0] != '0';   // baked default ON (step 4 7a A/B, lane D NOTES); =0 = the runtime compile
  }();
  return on;
}

HRESULT hostHlslCompile(const void* src, size_t len, const char* name, const D3D_SHADER_MACRO* defines,
                        const char* entry, const char* target, unsigned flags1, unsigned flags2, ID3DBlob** code,
                        ID3DBlob** errors) {
  if (!hostHlslAotOn()) {
    sr::kernel::HostWorkScope work(sr::kernel::kHostWorkShaderCompile);
    return D3DCompile(src, len, name, defines, nullptr, entry, target, flags1, flags2, code, errors);
  }
  const uint64_t key = hostHlslKey(src, len, name, defines, entry, target, flags1, flags2);
  const HostHlslBlob* hit = nullptr;
  size_t blobs = 0;
  {
    Registry& r = registry();
    std::lock_guard<std::mutex> lk(r.mu);
    if (!r.indexed) {
      r.byKey.clear();
      for (const auto& t : r.tables)
        for (uint32_t i = 0; i < t.second; ++i) r.byKey.emplace(t.first[i].key, &t.first[i]);
      r.indexed = true;
    }
    blobs = r.byKey.size();
    auto it = r.byKey.find(key);
    if (it != r.byKey.end()) hit = it->second;
  }
  static std::once_flag s_hello;
  std::call_once(s_hello, [blobs] {
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[HOST_HLSL] AOT on (SR_HOST_HLSL_AOT default on; =0 compiles at runtime): %zu ahead-of-time blob(s) in this exe; a source without "
                "one compiles at runtime and is logged",
                blobs);
  });
  if (hit && code) {
    ID3DBlob* b = nullptr;
    const HRESULT hr = D3DCreateBlob(hit->size, &b);
    if (SUCCEEDED(hr) && b) {
      std::memcpy(b->GetBufferPointer(), hit->data, hit->size);
      *code = b;
      if (errors) *errors = nullptr;
      g_embedded.fetch_add(1, std::memory_order_relaxed);
      return S_OK;
    }
  }
  const uint64_t n = g_runtime.fetch_add(1, std::memory_order_relaxed) + 1;
  {
    static std::mutex s_mu;
    static std::unordered_set<uint64_t> s_logged;
    std::lock_guard<std::mutex> lk(s_mu);
    if (s_logged.insert(key).second && s_logged.size() <= 64)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[HOST_HLSL] runtime compile #%llu (no ahead-of-time blob for this source): %s %s %s key=%016llX "
                  "len=%zu",
                  (unsigned long long)n, name ? name : "?", entry ? entry : "?", target ? target : "?",
                  (unsigned long long)key, len);
  }
  sr::kernel::HostWorkScope work(sr::kernel::kHostWorkShaderCompile);
  return D3DCompile(src, len, name, defines, nullptr, entry, target, flags1, flags2, code, errors);
}

}  // namespace sr::gfx
