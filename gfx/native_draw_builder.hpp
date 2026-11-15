#pragma once

#include "render_draw_command.hpp"
#include <span>

namespace sr::gfx {

struct NativeVertexLayout {
  uint32_t strideBytes = 0;
  uint32_t posOffsetBytes = 0, posFormat = 0;
  uint32_t colorOffsetBytes = 0, colorFormat = 0;
};

struct NativeDrawInput {
  uint32_t initiator = 0, indexBasePhys = 0, indexSize = 0;
  NativeVertexFetch vertexFetch{};
  NativeVertexLayout layout;
  uint64_t effectSequence = 0;
  uint32_t ordinal = 0;
  uint64_t vertexShader = 0, pixelShader = 0;
  uint32_t copyDestBase = 0, copyDestPitch = 0, copyDestInfo = 0;
};

// Immutable shader/device ABI state, without a register-file implementation or
// hardware side effects. Copies fixed draw state and preserves all shader arrays.
// The caller supplies a resolved layout, captures float/bool/fetch constants and
// any resolve payload, then publishes the complete command. This function owns no queue,
// mappings, resource lifetimes or GPU. Rejected admission leaves output intact.
bool buildNativeDrawState(std::span<const uint32_t> state, const NativeDrawInput& input,
                          GuestDrawCommand& output);

}  // namespace sr::gfx
