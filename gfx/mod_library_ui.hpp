#pragma once
// iter-465 modlib lane: the mod library OVERLAY (render thread). docs/MODS.md "The mod library";
// the model, input routing and actions are kernel/mod_library.hpp.
//
// Drawn by gfx/d3d12_overlay.cpp drawHostOverlay on the present path (after the frame dump,
// before Present, under the mouse cursor), into the swapchain buffer of this frame, with Dear
// ImGui (third_party/imgui, MIT) as the drawing layer: its font atlas, draw lists and DX12
// renderer backend. Every widget is our own draw code in the game's visual language: deep-blue
// glass panels with bevelled chrome, silver-white extended italic type (Saira, OFL), red and
// gold accents for focus and selection.
//
// Scaling: laid out in 1280x720 design units over the game's 16:9 image (the fit rectangle the
// mouse cursor maps through), scale = image height / 720, and the fonts are rasterised at that
// pixel size (rebuilt when the output height changes), so 720p..4K text is crisp, never a
// scaled bitmap.
#ifdef _WIN32
#include <d3d12.h>
#include <dxgiformat.h>

namespace sr {
namespace gfx {
namespace modlibui {

struct FrameCtx {
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;       // the present queue (font upload, flushes)
  ID3D12GraphicsCommandList* cl = nullptr;   // open, the back buffer bound as RT 0
  DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN;
  int bbW = 0, bbH = 0;
};

// Cheap: is there anything to draw this present (open, animating, or an open request)?
bool wantsFrame();
// Records the library into ctx.cl. Returns true when it drew.
bool render(const FrameCtx& ctx);
// SR_MOD_LIBRARY_SHOTS: F12 asked for a capture of this frame (true once per request).
bool shotRequested();

}  // namespace modlibui
}  // namespace gfx
}  // namespace sr
#endif
