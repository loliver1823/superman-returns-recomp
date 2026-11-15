// Step 4 item 7a: gfx/host_hlsl.cpp - the key matches tools/regen/host_hlsl_aot.py key() (the reference values below
// are that function's, tools/regen/test_host_hlsl_aot.py asserts the same three), and a registered table answers a
// compile with its blob only under SR_HOST_HLSL_AOT=1 (this test sets the gate before the first call).
#include "gfx/host_hlsl.hpp"
#include "app/log.hpp"

#include <windows.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// app/log stand-in (host_hlsl.cpp logs through SR_LOG_BOOT): printed, so a fallback's line is visible.
void sr::Log::write(LogCategory, LogSeverity, const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  std::vprintf(fmt, a);
  va_end(a);
  std::printf("\n");
}

namespace {
int failures = 0;
void check(bool ok, const char* name) {
  std::printf("[HOST_HLSL_TEST] %s %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++failures;
}
const unsigned char kFake[] = {'D', 'X', 'B', 'C', 1, 2, 3, 4};
const char kSrc[] = "float4 main() : SV_Target { return 1; }\n";
}  // namespace

int main() {
  using sr::gfx::hostHlslKey;
  const D3D_SHADER_MACRO half[] = {{"SR_HALF_HEXCMP", "1"}, {nullptr, nullptr}};
  const D3D_SHADER_MACRO noneButArray[] = {{nullptr, "1"}, {nullptr, nullptr}};  // pm4CompileHlsl's "no macro" form
  check(hostHlslKey(kSrc, sizeof(kSrc) - 1, "k.hlsl", nullptr, "main", "ps_5_1", 1u << 15, 0) == 0x0032BB47F54857F6ull,
        "key of a plain source = host_hlsl_aot.py key()");
  check(hostHlslKey("x", 1, "n", half, "e", "t", 0, 0) == 0x1D8352E6F4E22EB7ull, "key with a define = the tool's");
  check(hostHlslKey("", 0, "", nullptr, "", "", 0, 0) == 0x6E3E1B0C8ECA9DEFull, "key of all-empty inputs = the tool's");
  check(hostHlslKey(kSrc, sizeof(kSrc) - 1, "k.hlsl", noneButArray, "main", "ps_5_1", 1u << 15, 0) ==
            hostHlslKey(kSrc, sizeof(kSrc) - 1, "k.hlsl", nullptr, "main", "ps_5_1", 1u << 15, 0),
        "a macro array whose first name is null keys as no macros");
  check(hostHlslKey(kSrc, sizeof(kSrc) - 2, "k.hlsl", nullptr, "main", "ps_5_1", 1u << 15, 0) !=
            hostHlslKey(kSrc, sizeof(kSrc) - 1, "k.hlsl", nullptr, "main", "ps_5_1", 1u << 15, 0),
        "one source byte changes the key");

  const uint64_t k = hostHlslKey(kSrc, sizeof(kSrc) - 1, "k.hlsl", nullptr, "main", "ps_5_1", 1u << 15, 0);
  static const sr::gfx::HostHlslBlob table[] = {{k, kFake, sizeof(kFake), "test"}};
  sr::gfx::registerHostHlslTable(table, 1);
  _putenv_s("SR_HOST_HLSL_AOT", "1");
  check(sr::gfx::hostHlslAotOn(), "the gate reads 1");
  ID3DBlob* code = nullptr;
  ID3DBlob* err = nullptr;
  HRESULT hr = sr::gfx::hostHlslCompile(kSrc, sizeof(kSrc) - 1, "k.hlsl", nullptr, "main", "ps_5_1", 1u << 15, 0,
                                        &code, &err);
  check(SUCCEEDED(hr) && code && code->GetBufferSize() == sizeof(kFake) &&
            std::memcmp(code->GetBufferPointer(), kFake, sizeof(kFake)) == 0,
        "a registered key returns the embedded blob (not a compile)");
  if (code) code->Release();
  code = nullptr;
  hr = sr::gfx::hostHlslCompile(kSrc, sizeof(kSrc) - 1, "other.hlsl", nullptr, "main", "ps_5_1", 1u << 15, 0, &code,
                                &err);
  check(SUCCEEDED(hr) && code && code->GetBufferSize() > sizeof(kFake), "an unknown key compiles at runtime (fallback)");
  if (code) code->Release();
  std::printf("[HOST_HLSL_TEST] %s (%d failure(s))\n", failures ? "FAIL" : "ALL PASS", failures);
  return failures ? 1 : 0;
}
