#pragma once
// iter-464 input lane: DEVICE-MATCHED BUTTON PROMPTS (docs/INPUT_GLYPHS.md).
//
// The game draws every controller prompt from a small set of Apt image characters in ui.AST
// (HINTA..HINTY, HINTSHOULDER1-4, HINTSTART/SELECT, HINTDPAD*, the big animated A/B/X/Y
// buttons, ...): DXT5 textures the PM4 path samples like any other. This module recognises
// those textures by CONTENT (FNV-1a-64 of the guest texture bytes, the identities listed in
// data/glyphs/glyph_map.txt) and supplies the replacement image for the active glyph set
// (kernel/host_input.hpp): PlayStation 4/5, Switch or the key the player bound. The backend
// (gfx/d3d12_backend.cpp glyphNote / glyphRemap) swaps the bound SRV at draw time, so a
// device change shows on the next frame on every surface that uses the icon. Xbox / generic
// pads keep the game's own 360 art.
//
// Renderer-side only: no guest memory is written, no guest code is called; the game draws
// exactly what it drew, sampled from a different texture (the mod texture-replacement hook
// surface, docs/MODS.md). Recipes and identities are data (glyph_map.txt), so a texture pack
// can restyle prompts without code.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sr::gfx::glyphs {

// Loads <exe dir>/data/glyphs/glyph_map.txt (else ./data/glyphs/) once. False = no manifest
// (the feature is then inert). Thread-safe, idempotent.
bool init();
bool loaded();
// Root folder the manifest was read from (for logs/tests).
const std::string& root();

// Cheap pre-filter: does any icon identity have this guest format and size?
bool candidate(uint32_t fmt, uint32_t w, uint32_t h);
// The identity of the texture whose guest bytes start at `src` (`avail` readable bytes):
// 1-based icon index, 0 = not a prompt icon.
uint16_t identify(uint32_t fmt, uint32_t w, uint32_t h, const uint8_t* src, size_t avail);
// iter-465 polish: the same for a TILED guest texture (the live Apt texture: fmt 20 32x32,
// pitch 128 texels, tiled). The identities hash the LINEAR payload (ui.AST stores it row-major),
// so the base level is de-tiled first (Xenia's 2D tiling, gfx/guest_image.hpp, no endian swap:
// the guest bytes are the payload bytes). `avail` = the readable bytes of the tiled slice
// (tiledSliceBytes). *detiledHash (optional) = FNV-1a-64 of the de-tiled bytes, for the log.
uint16_t identifyTiled(uint32_t fmt, uint32_t w, uint32_t h, const uint8_t* src, size_t avail,
                       uint32_t pitchTexels, uint32_t blockW, uint32_t blockBytes,
                       uint64_t* detiledHash = nullptr);
// The bytes a tiled base level occupies (32x32-block macro tiles): what identifyTiled reads.
uint32_t tiledSliceBytes(uint32_t w, uint32_t h, uint32_t pitchTexels, uint32_t blockW, uint32_t blockBytes);
const char* iconName(uint16_t icon);
uint32_t iconCount();

// A replacement image: straight-alpha RGBA8, square, full mip chain (level 0 first).
struct Image {
  uint32_t size = 0;
  std::vector<std::vector<uint8_t>> levels;
};
// The image `icon` shows under glyph set `set` (hostinput::GlyphSet value), or nullptr to keep
// the game's own art. Built on first use (PNG decode + composition) and cached; a keyboard
// recipe shows the inputs hostinput::promptInputFor names NOW (the controls lane's bindings,
// in the current context).
const Image* replacement(uint16_t icon, uint8_t set);
// Which variant of (icon, set) replacement() would build now: 0 for a fixed recipe, else a
// signature of the input names a keyboard recipe shows (they change with a rebind or with
// the context, e.g. Right mouse in gameplay vs Escape in menus). Cheap, lock-free; the
// backend keys its uploaded replacements by it.
uint64_t variant(uint16_t icon, uint8_t set);

// The art a keyboard prompt uses for an input name (the controls lane's vocabulary, see
// hostinput::promptInputFor): true + its path relative to root() when the input has art of
// its own, false (and the keydefault path) for an unknown or unbound input.
bool keyArtFor(const std::string& input, std::string* png);

// ── pure helpers (tests) ───────────────────────────────────────────────────────
uint64_t fnv1a64(const uint8_t* p, size_t n);
// Parses manifest text (no file access) - returns the number of problems, details in *errors.
int parseManifestForTest(const std::string& text, std::string* errors);
// Decodes a PNG file to straight RGBA8 (WIC). False on failure.
bool decodePng(const std::string& path, uint32_t* w, uint32_t* h, std::vector<uint8_t>* rgba);

}  // namespace sr::gfx::glyphs
