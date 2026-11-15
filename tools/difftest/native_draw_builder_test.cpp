// Full command comparison against pre-extraction assembly. No GPU/resources.
#include "gfx/native_draw_builder.hpp"
#include "gfx/xenos_state.hpp"
#include "gfx/xenos_ucode.hpp"
#include <cstdio>
#include <cstring>
#include <string>

using namespace sr::gfx;
namespace {
#include "native_draw_builder_oracle.inc"
int passed = 0, failed = 0;
void check(bool ok, const std::string& label) {
  std::printf("%s%s\n", ok ? "  ok  " : "FAIL: ", label.c_str());
  ok ? ++passed : ++failed;
}
void compare(const XenosRegisterFile& state, NativeDrawInput input, const ucode::VertexBinding* binding,
             const std::string& label) {
  GuestDrawCommand actual, expected;
  std::memset(&actual, 0xCD, sizeof(actual));
  std::memset(&expected, 0xCD, sizeof(expected));
  originalDrawState(expected, state, input, binding);
  const bool complete = buildNativeDrawState({state.data(), XenosRegisterFile::kNumRegisters}, input, actual);
  // Complete the producer's separate late shader capture, as emitDraw does.
  std::memcpy(actual.boolConsts, state.data() + 0x4900u, sizeof(actual.boolConsts));
  std::memcpy(actual.fetchRaw, state.data() + 0x4800u, sizeof(actual.fetchRaw));
  check(complete && std::memcmp(&actual, &expected, sizeof(actual)) == 0, label);
}
}

int main() {
  XenosRegisterFile state;
  for (uint32_t i = 0; i < XenosRegisterFile::kNumRegisters; ++i)
    state.write(i, i * 0x1020305u + 0xFFEEDDCCu);
  // Raw float bit patterns, including signed zero and a NaN payload, must
  // survive capture without numerical conversion or canonicalization.
  state.write(0x210Fu, 0x80000000u);
  state.write(0x2110u, 0x7FC12345u);
  NativeDrawInput input{3u << 16 | 4u, 0x1234567u, 0u, {3u, 0x01020304u, 60u, 2u},
                       {80u}, UINT64_C(0x1122334455667788), 0xF1234567u,
                       UINT64_C(0x123456789ABCDEF0), UINT64_C(0xFEDCBA9876543210),
                       0x12340000u, 0x55556666u, 0x77778888u};
  {
    GuestDrawCommand output, before;
    std::memset(&output, 0xCD, sizeof(output));
    std::memcpy(&before, &output, sizeof(output));
    const bool complete = buildNativeDrawState({state.data(), XenosRegisterFile::kNumRegisters}, input, output);
    check(complete && std::memcmp(output.aluVs, before.aluVs, sizeof(output.aluVs)) == 0 &&
              std::memcmp(output.aluPs, before.aluPs, sizeof(output.aluPs)) == 0 &&
              std::memcmp(output.boolConsts, before.boolConsts, sizeof(output.boolConsts)) == 0 &&
              std::memcmp(output.fetchRaw, before.fetchRaw, sizeof(output.fetchRaw)) == 0,
          "fixed state preserves caller-owned shader arrays until late capture");
  }
  ucode::VertexBinding regular, noPosition, empty;
  regular.strideWords = 12;
  regular.attributes.push_back({static_cast<ucode::VertexFormat>(0x39u), 0});
  regular.attributes.push_back({static_cast<ucode::VertexFormat>(0x7u), 3});
  noPosition.strideWords = 7;
  noPosition.attributes.push_back({static_cast<ucode::VertexFormat>(0x1Fu), 4});
  noPosition.attributes.push_back({ucode::VertexFormat::kUndefined, 8});
  empty.strideWords = 2;
  for (uint32_t layout = 0; layout < 4u; ++layout) {
    const ucode::VertexBinding* binding = nullptr;
    input.layout = {80u};
    if (layout == 1u) { binding = &regular; input.layout = {48u, 0u, 0x39u, 12u, 0x7u}; }
    if (layout == 2u) { binding = &noPosition; input.layout = {28u, 16u, 0x1Fu, 16u, 0x1Fu}; }
    if (layout == 3u) { binding = &empty; input.layout = {8u}; }
    for (uint32_t source = 0; source < 4u; ++source)
      for (uint32_t size32 = 0; size32 < 2u; ++size32)
        for (uint32_t endian = 0; endian < 4u; ++endian) {
          input.initiator = (3u << 16) | (source << 6) | (size32 << 11) | 4u;
          input.indexSize = endian << 30;
          compare(state, input, binding, "original assembly layout/source/index-size/endian " +
                  std::to_string(layout) + "/" + std::to_string(source) + "/" +
                  std::to_string(size32) + "/" + std::to_string(endian));
        }
  }
  input.initiator = 3u << 16;
  input.layout = {80u};
  const uint32_t aliases[] = {0x2208u, 0x2202u, 0x210Eu, 0x2318u, 0x2003u, 0x2004u, 0x2005u,
                              0x2209u, 0x220Au, 0x220Bu, 0x2204u, 0x2302u, 0x231Du, 0x231Eu, 0x231Fu};
  for (uint32_t word : aliases) state.write(word, 0u);
  compare(state, input, nullptr, "original assembly chooses low aliases only when high words are zero");
  for (uint32_t word : aliases) state.write(word - 0x2000u, 0u);
  compare(state, input, nullptr, "original assembly preserves zero values and masked state");
  for (uint32_t words : {0u, 2u, 0xFFFFFFu}) {
    input.vertexFetch.sizeWords = words;
    input.layout = {words == 2u ? 12u : 44u};
    compare(state, input, nullptr, "development adapter stride boundary " + std::to_string(words));
  }
  for (uint32_t size : {0u, 0x4907u}) {
    GuestDrawCommand output, before;
    std::memset(&output, 0xCD, sizeof(output));
    std::memcpy(&before, &output, sizeof(output));
    check(!buildNativeDrawState({state.data(), size}, input, output) &&
              std::memcmp(&output, &before, sizeof(output)) == 0,
          "short state declines before output mutation " + std::to_string(size));
  }
  for (bool noCount : {false, true}) {
    auto invalid = input;
    if (noCount) invalid.initiator = 0u;
    else invalid.vertexFetch.addressDwords = 0u;
    GuestDrawCommand output, before;
    std::memset(&output, 0xCD, sizeof(output));
    std::memcpy(&before, &output, sizeof(output));
    check(!buildNativeDrawState({state.data(), XenosRegisterFile::kNumRegisters}, invalid, output) &&
              std::memcmp(&output, &before, sizeof(output)) == 0,
          noCount ? "null draw declines before mutation" : "absent vertex address declines before mutation");
  }
  std::printf("[NATIVE_DRAW_BUILDER_TEST] %d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
