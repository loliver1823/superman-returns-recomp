// Step-4 lane C round 2: the pure volume helpers behind SR_TEX3D_V2 (gfx/volume_layout.hpp) - Astra's P2-1 and
// P2-2 counterexamples on dev 7af4a541, each beside the old computation (the mutant) so the test shows both.
// Round 3 (SR_TEX3D_V3): the endian conversion by the format's block size (swapGuestUnits) - Astra's round-2 review
// showed the round-2 rule (swapGuestWords for every format) wrong for 8- and 16-bit blocks; those checks now test
// Xenia's rule and keep the round-2 result as the mutant.
#include "gfx/volume_layout.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace VL = sr::gfx::volume;
namespace {
unsigned passed = 0, failed = 0;
void check(bool ok, const char* name) {
  std::printf("[VOLUME_LAYOUT_TEST] %s %s\n", ok ? "PASS" : "FAIL", name);
  ok ? ++passed : ++failed;
}
// The old loader's conversion: every complete dword of the DECODED output, nothing for a shorter tail.
void oldOutputSwap(std::vector<uint8_t>& lin, uint32_t endian) {
  for (size_t i = 0; i + 4 <= lin.size(); i += 4) {
    uint32_t v;
    std::memcpy(&v, lin.data() + i, 4);
    if (endian == 1) v = ((v & 0xFF00FF00u) >> 8) | ((v & 0x00FF00FFu) << 8);
    std::memcpy(lin.data() + i, &v, 4);
  }
}
}  // namespace

int main() {
  // P2-2: an R16 1x1x1 level with 8-in-16 endian holds the guest bytes 12 34 = the value 0x1234.
  {
    const uint8_t guest[2] = {0x12, 0x34};
    std::vector<uint8_t> lanes;
    VL::swapGuestWords(guest, 2, 1u, lanes);
    uint16_t texel;
    std::memcpy(&texel, lanes.data(), 2);
    check(lanes.size() == 4 && texel == 0x1234u, "r16_1x1x1_8in16_tail_converts_by_guest_lanes");
    std::vector<uint8_t> old(guest, guest + 2);
    oldOutputSwap(old, 1u);
    std::memcpy(&texel, old.data(), 2);
    check(texel == 0x3412u, "mutant_old_dword_only_swap_leaves_the_tail_unconverted");
  }
  // The 32-bit word rule (32 / 64 / 128-bit blocks, XeEndianSwap32): on the guest word A0 A1 B0 B1, 8in32 reverses
  // the bytes, 16in32 swaps the halves.
  {
    const uint8_t guest[4] = {0xA0, 0xA1, 0xB0, 0xB1};
    std::vector<uint8_t> lanes;
    VL::swapGuestWords(guest, 4, 2u, lanes);
    check(lanes[0] == 0xB1 && lanes[1] == 0xB0 && lanes[2] == 0xA1 && lanes[3] == 0xA0, "32bit_word_8in32_reverses");
    VL::swapGuestWords(guest, 4, 3u, lanes);
    check(lanes[0] == 0xB0 && lanes[1] == 0xB1 && lanes[2] == 0xA0 && lanes[3] == 0xA1, "32bit_word_16in32_swaps_halves");
    VL::swapGuestWords(guest, 3, 0u, lanes);
    check(lanes.size() == 4 && lanes[2] == 0xB0 && lanes[3] == 0, "endian_none_copies_and_zero_pads");
    for (uint32_t bb : {4u, 8u, 16u}) {  // swapGuestUnits keeps the word rule for every block of 4+ bytes
      std::vector<uint8_t> units;
      VL::swapGuestUnits(guest, 4, 2u, bb, units);
      check(units.size() == 4 && units[0] == 0xB1 && units[1] == 0xB0 && units[2] == 0xA1 && units[3] == 0xA0,
            bb == 4u ? "units_32bit_block_8in32_reverses"
                     : bb == 8u ? "units_64bit_block_8in32_reverses" : "units_128bit_block_8in32_reverses");
    }
  }
  // Round 3 (Astra round-2 review): Xenia's 8-bit loader never swaps; its 16-bit loader applies XeEndianSwap16 -
  // 8in16 only. Each case beside the round-2 rule's result (the mutant).
  {
    const uint8_t r8[1] = {0x7F};
    for (uint32_t e = 1; e <= 3; e++) {
      std::vector<uint8_t> units, words;
      VL::swapGuestUnits(r8, 1, e, 1u, units);
      VL::swapGuestWords(r8, 1, e, words);
      check(units.size() == 4 && units[0] == 0x7F,
            e == 1 ? "r8_7F_8in16_unchanged" : e == 2 ? "r8_7F_8in32_unchanged" : "r8_7F_16in32_unchanged");
      check(words[0] != 0x7F, e == 1 ? "mutant_round2_r8_8in16_moves_the_byte"
                                     : e == 2 ? "mutant_round2_r8_8in32_moves_the_byte"
                                              : "mutant_round2_r8_16in32_moves_the_byte");
    }
    const uint8_t r16[2] = {0x12, 0x34};
    for (uint32_t e = 2; e <= 3; e++) {
      std::vector<uint8_t> units, words;
      VL::swapGuestUnits(r16, 2, e, 2u, units);
      VL::swapGuestWords(r16, 2, e, words);
      check(units[0] == 0x12 && units[1] == 0x34, e == 2 ? "r16_1234_8in32_unchanged" : "r16_1234_16in32_unchanged");
      check(words[0] == 0x00 && words[1] == 0x00, e == 2 ? "mutant_round2_r16_8in32_gives_0000"
                                                         : "mutant_round2_r16_16in32_gives_0000");
    }
    std::vector<uint8_t> units;
    VL::swapGuestUnits(r16, 2, 1u, 2u, units);
    uint16_t texel;
    std::memcpy(&texel, units.data(), 2);
    check(units.size() == 4 && texel == 0x1234u, "r16_1234_8in16_swaps_each_16bit_unit");
    // two 16-bit texels A0 A1 B0 B1 with 8in32: Xenia's 16-bit loader leaves them; round 2 paired them across texels.
    const uint8_t two[4] = {0xA0, 0xA1, 0xB0, 0xB1};
    VL::swapGuestUnits(two, 4, 2u, 2u, units);
    check(units[0] == 0xA0 && units[1] == 0xA1 && units[2] == 0xB0 && units[3] == 0xB1,
          "two_16bit_texels_8in32_unchanged");
    VL::swapGuestUnits(two, 4, 1u, 2u, units);
    check(units[0] == 0xA1 && units[1] == 0xA0 && units[2] == 0xB1 && units[3] == 0xB0,
          "two_16bit_texels_8in16_swap_within_each");
  }
  // P2-1: a 1x1x64 k_8_8_8_8 volume (tiled, pitch 32) with a mip page and mip_max_level 6 has levels 1..6: the
  // longest axis is the depth (texture_util.cc GetSubresourcesFromFetchConstant).
  {
    const VL::Spans s = VL::fetchSpans(1, 1, 64, 32, true, 0, 4, true, false, 0, 6);
    check(s.ok && s.maxLevel == 6 && s.mipBytes > 0, "1x1x64_mip_max_6_has_a_mip_span");
    const VL::Spans none = VL::fetchSpans(1, 1, 64, 32, true, 0, 4, false, false, 0, 6);
    check(none.ok && none.maxLevel == 0 && none.mipBytes == 0, "no_mip_page_no_mip_span");
    // mip_min above mip_max: GetSubresources keeps max(min(mip_max, size), mip_min).
    const VL::Spans lifted = VL::fetchSpans(64, 64, 64, 64, true, 0, 4, true, false, 3, 1);
    check(lifted.ok && lifted.maxLevel == 3, "mip_max_never_below_mip_min");
    const VL::Spans big = VL::fetchSpans(64, 64, 64, 64, true, 0, 4, false, false, 0, 0);
    check(big.ok && big.baseBytes == 1050752u, "64cube_k8888_tiled_base_extent_1050752");
  }
  std::printf("[VOLUME_LAYOUT_TEST] RESULT checks=%u passed=%u failed=%u\n", passed + failed, passed, failed);
  return failed ? 1 : 0;
}
