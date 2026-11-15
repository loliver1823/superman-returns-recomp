#pragma once
// iter-466 gfx (PORT FEATURE, docs/GRAPHICS_SETTINGS.md): the HOST graphics options - the
// settings the PC SETTINGS screen changes that are drawn by the host, never decided by the
// game. Every option changes how the host draws; none reads or writes guest memory, calls
// guest code or changes a game decision. Each default reproduces today's output exactly
// (the stage or path it controls records nothing at its default).
//
// Store: <save root>/settings/display.ini (beside game_options.ini; the resolution and frame
// rate keys of gfx/res_scale.hpp and kernel/highfps.hpp live in the same file). Every write
// keeps every other line byte for byte.
//
// Precedence (as kernel/highfps.hpp): the option's own SR_GFX_* env override (replay A/B,
// rolls) > an automated / natural roll (port features off: the defaults, display.ini is not
// read) > the player's display.ini > the default.
//
//   key              values                          default   env override
//   post_aa          off | fxaa | fxaa_extreme       off       SR_GFX_POST_AA
//   brightness       -50 .. 50                       0         SR_GFX_BRIGHTNESS
//   gamma            0.50 .. 1.50                    1.00      SR_GFX_GAMMA
//   upscale_filter   bilinear | cas | fsr            bilinear  SR_GFX_UPSCALE   (res4k lane)
//   sharpness        0 .. 100                        0         SR_GFX_SHARPNESS (res4k lane)
//   anisotropic      off | 2 | 4 | 8 | 16            off       SR_GFX_ANISO
//   lod_bias         -2.00 .. 2.00                   0.00      SR_GFX_LOD_BIAS
//   bloom            0 .. 200 (percent)              100       SR_GFX_BLOOM
//   edge_blur        on | off                        on        SR_GFX_EDGE_BLUR
// anisotropic / lod_bias override the PM4 path's sampler table (Xenia's anisotropic_override): off
// keeps the game's own filtering (the gameplay traces' fetch constants ask for trilinear with
// aniso_filter = 0), so off is the Xbox 360 look.
// bloom scales one constant of the game's tone map in the HOST copy of the constants the draw
// reads (patchPsConstants; the guest's registers are untouched): ps 2679C172F83DE85D,
// colour = scene + bloom * c1.x. edge_blur off leaves out the edge blur draw after it
// (ps DB6A2996BA148383, colour = centre + c255.x * w * (blurred - centre); c255 is a literal of
// the shader's definition table, compiled into the translation, so there is no constant to
// scale). That draw reads a resolve of the render target the tone map just wrote and rewrites
// all of it, so without it the target keeps the tone map's output: exactly strength 0. Every
// gameplay trace (gp1, gp2, lg1, lg2, 16099) runs this post chain; at 100 / on nothing changes.
#include <cstdint>
#include <string>
#include <vector>

namespace sr {
namespace gfx {
namespace gfxopt {

enum class PostAa : uint8_t { Off = 0, Fxaa = 1, FxaaExtreme = 2 };
enum class Upscale : uint8_t { Bilinear = 0, Cas = 1, Fsr = 2 };

struct Settings {
  PostAa postAa = PostAa::Off;
  int brightness = 0;     // -50..50; 0 = unchanged
  float gamma = 1.0f;     // 0.50..1.50; 1.00 = unchanged
  Upscale upscale = Upscale::Bilinear;
  int sharpness = 0;      // 0..100
  int anisotropic = 0;    // 0 = off (the game's own filtering), 2, 4, 8, 16
  float lodBias = 0.0f;   // -2.00..2.00 (texture LOD bias added to every sampler)
  int bloom = 100;        // 0..200 percent of the game's bloom weight; 100 = unchanged
  bool edgeBlur = true;   // the game's edge blur (its own anti-aliasing); true = unchanged
};
bool operator==(const Settings& a, const Settings& b);
inline bool operator!=(const Settings& a, const Settings& b) { return !(a == b); }

// The keys this module owns, in display order.
const std::vector<std::string>& keys();

// ---- pure helpers (unit-tested by selfTest) ------------------------------------------
// Apply `key = value` to s. false = unknown key or a malformed / out-of-range value (s kept).
bool applyKey(Settings& s, const std::string& key, const std::string& value);
// The canonical text of one key's value in s ("" for an unknown key).
std::string formatKey(const Settings& s, const std::string& key);
// Settings from the text of a display.ini (unknown keys and bad values ignored).
Settings fromIniText(const std::string& text);
int selfTest();   // 0 = pass

// ---- the live settings (thread-safe) --------------------------------------------------
// Resolved once on first use (env, port features, display.ini); set() changes them live.
Settings current();
// Bumped on every live change (the renderer re-reads current() when it moves).
uint64_t version();
// Validate, apply live and write display.ini. A key with an active SR_GFX_* override is
// applied live but the override keeps winning at the next start. false + *err when refused.
// persist = false: the settings screen's live preview (applied, not written; APPLY commits it).
bool set(const std::string& key, const std::string& value, std::string* err = nullptr,
         bool persist = true);
// The text of one key's live value.
std::string get(const std::string& key);
// The default text of one key (RESET TO DEFAULTS).
std::string defaultValue(const std::string& key);

// The game post shaders bloom / edge_blur act on (see the header comment).
constexpr uint64_t kPsToneMapBloom = 0x2679C172F83DE85Dull;   // c1.x = bloom weight
constexpr uint64_t kPsEdgeBlur = 0xDB6A2996BA148383ull;       // the edge blur draw
// Scale the host copy of a draw's pixel-shader constants (c = 256 float4, `use` = the uploaded
// 16-constant blocks, bit 16 or all/none = everything) for the options above. Draw thread;
// returns at once for every other shader and at the defaults.
void patchPsConstants(uint64_t psHash, float* c, uint32_t use);
// true = the host leaves this draw out (edge_blur off and the edge blur shader). Draw thread.
bool skipDraw(uint64_t psHash);

}  // namespace gfxopt
}  // namespace gfx
}  // namespace sr
