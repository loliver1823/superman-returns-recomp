#pragma once
// iter-465 viewer lane: the COSTUME VIEWER's 3D model pass (kernel/costume_viewer.hpp). Drawn by
// gfx/d3d12_overlay.cpp drawHostOverlay into this frame's swapchain buffer, inside the open centre
// of the game's own (re-laid-out) COSTUMES page: the suit's skinned mesh (CPU skinned each frame),
// its own textures, a key / rim / ambient light, the cape posed by its chains plus a slow sway.
#ifdef _WIN32
#include <d3d12.h>
#include <dxgiformat.h>

namespace sr::gfx::cviewdraw {

struct FrameCtx {
  ID3D12Device* device = nullptr;
  ID3D12GraphicsCommandList* cl = nullptr;   // open, the back buffer bound as RT 0
  D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
  DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN;
  int bbW = 0, bbH = 0;
  unsigned frameIndex = 0, frameCount = 2;   // the overlay's per-frame slot (its fence was waited)
};

bool wantsFrame();
// Records the model pass into ctx.cl; leaves the back buffer as RT 0 without a depth buffer.
bool render(const FrameCtx& ctx);

}  // namespace sr::gfx::cviewdraw
#endif
