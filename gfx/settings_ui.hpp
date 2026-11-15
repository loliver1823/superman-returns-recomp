#pragma once
// iter-466 gfx (PORT FEATURE, docs/GRAPHICS_SETTINGS.md): THE PC SETTINGS SCREEN - a real PC options
// screen drawn as a host overlay (gfx/d3d12_overlay.cpp drawHostOverlay, after the frame, before
// Present) with the shared overlay kit (gfx/overlay_kit.hpp: the game's glass / chrome / Saira look).
//
// lane E L-b (step 5): the screen's title is OPTIONS (the owner's one options screen, q14d).
// Tabs GAME / CONTROLS / DISPLAY / GRAPHICS / WORLD / AUDIO / CHEATS (CHEATS in game only; GAME and CHEATS are the
// game's own options and cheats, kernel/game_options.hpp, kernel/cheats_menu.hpp); every row a real control (drop-down, slider
// with its value, checkbox, button); a description pane (what it does, its cost, when it applies,
// what the Xbox 360 did); presets; APPLY / REVERT / RESET TO DEFAULTS; a keep-or-revert countdown
// for display changes; RESTART NOW for restart-only options. Mouse, keyboard and pad, with
// device-matched prompts. The model (options, stores, apply) is kernel/pc_settings.hpp; the
// overlay routing (open requests, input, the neutral pad) is the mod library's
// (kernel/mod_library.hpp, OverlayPage::Settings).
//
// Also draws the PERFORMANCE OVERLAY (display.ini perf_overlay) when it is on.
#ifdef _WIN32
#include <d3d12.h>
#include <dxgiformat.h>

namespace sr {
namespace gfx {
namespace settingsui {

struct FrameCtx {
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  ID3D12GraphicsCommandList* cl = nullptr;   // open, the back buffer bound as RT 0
  DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN;
  int bbW = 0, bbH = 0;
};

// Is there anything to draw this present (the screen, its close animation, the perf overlay)?
// `otherOverlay` = another ImGui overlay (the mod library) draws this frame: the perf overlay
// then stays out (one ImGui frame per present).
bool wantsFrame(bool otherOverlay);
bool render(const FrameCtx& ctx, bool otherOverlay);
// F12 (real or harness keyboard) with SR_MOD_LIBRARY_SHOTS set: capture this composited frame.
bool shotRequested();

}  // namespace settingsui
}  // namespace gfx
}  // namespace sr
#endif
