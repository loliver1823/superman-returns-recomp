#pragma once
#include <cstdint>
#include <string>

struct PPCContext;
namespace sr::kernel {
// Opt-in port feature: larger cloth scratch, preserving the game's arithmetic.
// The original guest stack bounds still apply. No allocation or guest call is added.
constexpr uint32_t kClothCapacityLimit = 4096;
struct ClothScratchLayout {
  uint32_t tangent1, tangent2, accumulationBytes, frame;
};
bool clothScratchLayout(uint32_t vertices, ClothScratchLayout* out);
bool clothTangentsWithCapacity(PPCContext& ctx, uint8_t* base, std::string* error);
}
