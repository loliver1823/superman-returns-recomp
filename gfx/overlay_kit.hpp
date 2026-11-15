#pragma once
// iter-466 gfx: THE HOST OVERLAY KIT - the drawing layer shared by the host overlays drawn on the
// present path (gfx/d3d12_overlay.cpp drawHostOverlay): the MODS library (gfx/mod_library_ui.cpp)
// and the PC SETTINGS screen (gfx/settings_ui.cpp). Moved out of mod_library_ui.cpp (iter-465
// modlib lane) unchanged in behaviour.
//
// Dear ImGui (third_party/imgui, MIT) is only the drawing layer: one shared ImGui context, DX12
// renderer backend, shader-visible descriptor heap and font atlas. Every widget is our own draw
// code in the game's visual language: deep-blue glass panels with bevelled chrome, silver-white
// extended italic type (Saira, OFL), red and gold accents for focus and selection.
//
// Coordinates: 1280x720 DESIGN units over the game's 16:9 image (X maps them to pixels); fonts are
// rasterised at the output's pixel size (buildFonts), so 720p..4K text is crisp.
//
// Render thread only (the overlays draw on the present path).
#ifdef _WIN32
#include <d3d12.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include "imgui.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ImGui_ImplDX12_InitInfo;

namespace sr {
namespace gfx {
namespace overlay {

using Microsoft::WRL::ComPtr;

// ── small helpers ─────────────────────────────────────────────────────────────────────────
double nowSec();
float approach(float cur, float target, float rate, float dt);
float clampf(float v, float a, float b);
float smooth01(float t);
std::wstring widen(const std::string& s);
std::string exeDir();
std::string upperAscii(std::string s);

// ── palette (sampled from the game's own popups) ──────────────────────────────────────────
extern float gAlpha;   // the open / close fade, applied to every colour col() makes
ImU32 col(float r, float g, float b, float a = 1.0f);
ImU32 colRaw(float r, float g, float b, float a);
struct Rgb { float r, g, b; };
constexpr Rgb kWhite{0.96f, 0.97f, 1.0f};
constexpr Rgb kSilver{0.70f, 0.75f, 0.84f};
constexpr Rgb kSilverDim{0.52f, 0.60f, 0.74f};
constexpr Rgb kInk{0.02f, 0.04f, 0.10f};
constexpr Rgb kRed{0.80f, 0.09f, 0.12f};
constexpr Rgb kRedDeep{0.48f, 0.03f, 0.07f};
constexpr Rgb kGold{0.98f, 0.76f, 0.24f};
constexpr Rgb kGoldDeep{0.72f, 0.46f, 0.08f};
constexpr Rgb kBlueHi{0.55f, 0.78f, 1.0f};
constexpr Rgb kBlue{0.16f, 0.42f, 0.86f};
constexpr Rgb kOk{0.36f, 0.86f, 0.52f};
ImU32 C(const Rgb& c, float a = 1.0f);

// ── the design transform: 1280x720 units over the game's 16:9 image ───────────────────────
struct Xf {
  float ox = 0, oy = 0, s = 1;
  int bbW = 1280, bbH = 720;
  ImVec2 p(float x, float y) const { return ImVec2(ox + x * s, oy + y * s); }
  float d(float v) const { return v * s; }
};
extern Xf X;

// ── GPU side: the shared heap (font atlas + images), fence, ImGui context ─────────────────
constexpr int kHeapSlots = 256;
constexpr int kFramesInFlight = 3;
struct Gpu {
  ID3D12Device* dev = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  ComPtr<ID3D12DescriptorHeap> heap;
  UINT inc = 0;
  std::vector<int> freeSlots;
  ComPtr<ID3D12Fence> fence;
  UINT64 fenceVal = 0;
  HANDLE ev = nullptr;
  uint64_t frame = 0;
  std::vector<std::pair<uint64_t, ComPtr<ID3D12Resource>>> grave;
  bool imguiReady = false;
  bool failed = false;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
  ImGuiContext* ctx = nullptr;
};
extern Gpu G;
int allocSlot();
void freeSlot(int s);
D3D12_CPU_DESCRIPTOR_HANDLE cpuH(int s);
D3D12_GPU_DESCRIPTOR_HANDLE gpuH(int s);
void retire(const ComPtr<ID3D12Resource>& r);
void collectGrave();
void flushGpu();
// The heap, the fence and the ImGui context (once); makes the context current.
bool ensureContext(ID3D12Device* dev, ID3D12CommandQueue* queue);
// The DX12 renderer backend for this swap-chain format (rebuilt on a format or font change).
bool ensureBackend(DXGI_FORMAT rtvFormat);
void shutdownBackend();

// ── images (PNG / JPG through WIC on a worker; uploads on the render thread) ──────────────
// The texture of an image file, 0 until it is uploaded (key = path + size class).
ImTextureID image(const std::string& path, int maxW, int maxH, int* w = nullptr, int* h = nullptr);
bool imageFailed(const std::string& path, int maxW);
// lane E L-a: an image held in memory (an achievement's icon from the title database). the caller's function fills the encoded
// bytes (PNG / JPG): `fetch` runs ONCE, on the decode worker (so it must be thread-safe), and the result is decoded and
// uploaded like an image file's. 0 until it is uploaded; key = a stable name for those bytes (+ the size class).
// `grey` = decoded to grey (the luminance in R = G = B, alpha kept; a locked achievement's icon): a texture of its own.
ImTextureID imageFromMemory(const std::string& key, int maxW, int maxH,
                            const std::function<bool(std::vector<uint8_t>*)>& fetch, bool grey = false,
                            int* w = nullptr, int* h = nullptr);
bool imageFromMemoryFailed(const std::string& key, int maxW, bool grey = false);
// Test seam (difftest): the decode of encoded bytes as the worker does it (RGBA, scaled to fit maxW x maxH; grey).
bool decodeImageBytesForTest(const std::vector<uint8_t>& bytes, int maxW, int maxH, int* w, int* h,
                             std::vector<uint8_t>* rgba, bool grey = false);
// Test seam (difftest): wait up to timeoutMs for `want` decoded images from the worker and take them before any upload
// (no GPU needed; their cache entries stay queued). key = the cache key (imageFromMemory: "mem:<key>[|grey]|<maxW>").
struct DecodedForTest {
  std::string key;
  bool ok = false;
  int w = 0, h = 0;
  std::vector<uint8_t> px;
};
std::vector<DecodedForTest> takeDecodedForTest(int want, int timeoutMs);
// A folder's size in bytes (computed once on the worker; false until known).
bool folderSize(const std::string& id, const std::string& root, uint64_t* out);
void pumpUploads(ID3D12GraphicsCommandList* cl);
void releaseImages();

// ── fonts: Saira SR (OFL) at the output's pixel size ──────────────────────────────────────
struct Fonts {
  ImFont* display = nullptr;   // page titles                Black Italic, caps
  ImFont* title = nullptr;     // details title              Black Italic
  ImFont* tab = nullptr;       // tabs, section heads        Bold Italic, caps
  ImFont* card = nullptr;      // card titles, buttons       Bold Italic
  ImFont* label = nullptr;     // rail, meta, table          SemiBold
  ImFont* badge = nullptr;     // badges, counts             SemiBold, caps
  ImFont* body = nullptr;      // descriptions               Medium
  ImFont* smallF = nullptr;    // authors, hints             Medium
  float scale = 0;
  bool ok = false;
};
extern Fonts F;
// design sizes (720p px)
constexpr float kSzDisplay = 42, kSzTitle = 28, kSzTab = 17, kSzCard = 14.5f, kSzLabel = 14,
                kSzBadge = 10.5f, kSzBody = 14.5f, kSzSmall = 12;
std::string fontDir();
bool buildFonts(float s);

// ── drawing primitives (design coords in, pixels out) ─────────────────────────────────────
extern ImDrawList* DL;
struct RectD { float x0, y0, x1, y1; };
extern std::vector<RectD> gClip;   // design-space clip stack (hit tests honour it)
void pushClip(float x0, float y0, float x1, float y1);
void popClip();
ImU32 lerpCol(ImU32 a, ImU32 b, float t);
void gradPoly(const ImVec2* pts, int n, ImU32 top, ImU32 bot);
int chamfer(float x0, float y0, float x1, float y1, float tl, float tr, float br, float bl, ImVec2* o);
void glass(float x0, float y0, float x1, float y1, float cut, float a = 1.0f, bool raised = false);
void chrome(float x0, float y0, float x1, float y1, bool top);
void selBand(float x0, float y0, float x1, float y1, float a);
void focusFrame(float x0, float y0, float x1, float y1, float cut, float a, double t);
void sheen(float x0, float y0, float x1, float y1, double t, float a);

// ── text ──────────────────────────────────────────────────────────────────────────────────
float fontPx(float design);
float textW(ImFont* f, float size, const std::string& s);
void text(ImFont* f, float size, float x, float y, const std::string& s, ImU32 top, ImU32 bot,
          float shadow = 1.0f);
void textPlain(ImFont* f, float size, float x, float y, const std::string& s, ImU32 c);
void textTitle(ImFont* f, float size, float x, float y, const std::string& s, float a = 1.0f);
std::string ellipsize(ImFont* f, float size, const std::string& s, float maxW);
std::vector<std::string> wrap(ImFont* f, float size, const std::string& s, float width);

// ── icons and small widgets ───────────────────────────────────────────────────────────────
void shield(float cx, float cy, float r, float a = 1.0f);
void checkMark(float x0, float y0, float sz, ImU32 c, float th);
void checkbox(float x0, float y0, float sz, bool on, bool focus, float a = 1.0f);
float badge(float x, float y, const std::string& label, ImU32 bg, ImU32 fg, ImU32 rim);

// ── button prompts, matched to the device the player used last ────────────────────────────
// cView (lane E E3) = the pad's BACK / VIEW button (PS Share, Switch Minus; PC SETTINGS: REVERT).
enum Ctl { cA = 0, cB, cX, cY, cLB, cRB, cLT, cRT, cDpad, cRS, cStart, cView, cCount };
enum PromptSet { psXbox = 0, psPS4, psPS5, psSwitch, psKeyboard };
extern PromptSet gPromptSet;
extern bool gLastWasPad;
// Lane E E3 (Astra r2): MOVE (cDpad) on this screen also means LEFT / RIGHT (PC SETTINGS: adjust, binding slot), so
// its keyboard cap checks and names all four directions. Render thread; each screen sets it with its prompts.
extern bool gMoveLeftRight;
// Lane E E3 (Astra r3): a prompt for a mode that takes FIXED keys, not the bindings (the library's search box: Enter
// / Esc; PC SETTINGS' key capture: Esc), names those keys: while an override is set, the keyboard cap of `c` is
// `label` (nullptr clears it). Render thread; set around the one prompt's measure + draw.
void setKeyCapOverride(Ctl c, const char* label);
const char* glyphPng(PromptSet set, Ctl c);
std::string glyphRoot();
void xboxGlyph(Ctl c, float cx, float cy, float r);
const char* kbLabel(Ctl c);
float keyCapW(const char* l, float h);
void keyCap(const char* l, float cx, float cy, float h);
void keyCapArrows(float cx, float cy, float h);
float glyphW(Ctl c, float h);
float glyphAt(Ctl c, float x, float cy, float h);   // left edge at x; returns the width
float glyph(Ctl c, float cx, float cy, float h);    // centred; returns the width

}  // namespace overlay
}  // namespace gfx
}  // namespace sr
#endif  // _WIN32
