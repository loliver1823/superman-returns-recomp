// iter-465 modlib: an OFFSCREEN render of the mod library overlay (gfx/mod_library_ui.cpp) for
// look iterations without a game process: a D3D12 device + an offscreen back buffer holding a
// background image (a capture of the game, the "frame behind"), the real mod manager over a test
// content root, the real overlay code, scripted input, BMP captures.
//
// Run: build/difftest.exe --mod-library-render <content root> <out dir> <WxH> <background image|-> "<script>"
//      build/difftest.exe --settings-render    <content root> <out dir> <WxH> <background image|-> "<script>"
//      (iter-466 gfx: the PC SETTINGS screen, gfx/settings_ui.cpp, through the same harness; the
//      content root is its SR_CONTENT_DIR, so settings/display.ini etc. are written THERE, never to
//      the player's own settings. Extra verbs: khold NAME / krelease NAME hold a key, e.g. RT = the
//      hold-to-preview key, with the modifier a pad name needs (LB = Shift+Tab).)
//      build/difftest.exe --achievements-render <content root> <out dir> <WxH> <background image|-> "<script>"
//      (lane E L-a: the ACHIEVEMENTS page, gfx/achievements_ui.cpp, drawn through the settings slot as in the game.
//      The data: SR_ACH_XDBF=<the title's XDBF> = the real table and icons (else 18 made-up rows, no icons);
//      SR_ACH_RENDER_UNLOCKS=<id,id,...> this save's unlocks; SR_ACH_RENDER_SLOT=<0..2|none|bad> (default 1);
//      SR_ACH_RENDER_NOTABLE=1 the page without a table.)
//   script: commands separated by ';'
//     frames N          render N frames (~16 ms apart; animations run on the real clock)
//     key NAME          UP DOWN LEFT RIGHT A B X Y LB RB LT RT ENTER ESC SPACE TAB SLASH BACK
//     type TEXT         text input (the search box)
//     move X,Y          mouse to stage coords (1280x720 over the 16:9 image)
//     click X,Y | rclick X,Y | down | up | wheel N
//     shot NAME         render one frame and write <out dir>/<NAME>_<W>x<H>.bmp
// Env: SR_MOD_LIBRARY_PROMPTS=xbox|ps4|ps5|switch|keyboard pins the prompt set (else the input
// layer's set: Xbox unless a device says otherwise).
// The content root's mods/mods.json IS written by toggles (use a test root).
#ifdef _WIN32
#include "gfx/mod_library_ui.hpp"
#include "gfx/settings_ui.hpp"
#include "gfx/achievements_ui.hpp"   // lane E L-a: --achievements-render
#include "kernel/achievements.hpp"
#include "kernel/mod_library.hpp"
#include "kernel/mod_manager.hpp"
#include "kernel/mod_overlay.hpp"
#include "kernel/pc_controls.hpp"   // lane E E3: pad names -> the host screens' keys (hostKeyFor)

#include <SDL3/SDL.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace ML = sr::kernel::modlib;

struct Ctx {
  ComPtr<ID3D12Device> dev;
  ComPtr<ID3D12CommandQueue> q;
  ComPtr<ID3D12CommandAllocator> alloc;
  ComPtr<ID3D12GraphicsCommandList> cl;
  ComPtr<ID3D12Resource> rt, bg, readback;
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  ComPtr<ID3D12Fence> fence;
  HANDLE ev = nullptr;
  UINT64 fv = 0;
  int w = 0, h = 0;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT64 rbBytes = 0;
};

bool g_settings = false;   // iter-466 gfx: --settings-render
bool g_ach = false;        // lane E L-a: --achievements-render (drawn through the settings slot)

void barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
  D3D12_RESOURCE_BARRIER br{};
  br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  br.Transition.pResource = r;
  br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  br.Transition.StateBefore = a;
  br.Transition.StateAfter = b;
  cl->ResourceBarrier(1, &br);
}

void waitGpu(Ctx& c) {
  ++c.fv;
  c.q->Signal(c.fence.Get(), c.fv);
  if (c.fence->GetCompletedValue() < c.fv) {
    c.fence->SetEventOnCompletion(c.fv, c.ev);
    WaitForSingleObject(c.ev, 10000);
  }
}

// The background: an image file scaled to fill (WIC), or a dark-blue gradient.
std::vector<uint8_t> loadBackground(const std::string& path, int w, int h) {
  std::vector<uint8_t> px(size_t(w) * h * 4, 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint8_t* p = &px[(size_t(y) * w + x) * 4];
      p[0] = uint8_t(20 + 40 * y / h); p[1] = uint8_t(30 + 50 * y / h); p[2] = uint8_t(60 + 60 * y / h); p[3] = 255;
    }
  if (path.empty() || path == "-") return px;
  ComPtr<IWICImagingFactory> f;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return px;
  ComPtr<IWICBitmapDecoder> dec;
  ComPtr<IWICBitmapFrameDecode> fr;
  const std::wstring wp(path.begin(), path.end());
  if (FAILED(f->CreateDecoderFromFilename(wp.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
      FAILED(dec->GetFrame(0, &fr)))
    return px;
  ComPtr<IWICBitmapScaler> sc;
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(f->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(fr.Get(), UINT(w), UINT(h), WICBitmapInterpolationModeFant)) ||
      FAILED(f->CreateFormatConverter(&conv)) ||
      FAILED(conv->Initialize(sc.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
    return px;
  conv->CopyPixels(nullptr, UINT(w) * 4, UINT(px.size()), px.data());
  return px;
}

bool init(Ctx& c, int w, int h, const std::string& bgPath) {
  c.w = w;
  c.h = h;
  if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&c.dev)))) return false;
  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(c.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&c.q))) ||
      FAILED(c.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.alloc))) ||
      FAILED(c.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.alloc.Get(), nullptr, IID_PPV_ARGS(&c.cl))) ||
      FAILED(c.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence))))
    return false;
  c.cl->Close();
  c.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = UINT64(w);
  rd.Height = UINT(h);
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  rd.SampleDesc.Count = 1;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  if (FAILED(c.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&c.rt))))
    return false;
  rd.Flags = D3D12_RESOURCE_FLAG_NONE;
  if (FAILED(c.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&c.bg))))
    return false;
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = 1;
  if (FAILED(c.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&c.rtvHeap)))) return false;
  c.dev->CreateRenderTargetView(c.rt.Get(), nullptr, c.rtvHeap->GetCPUDescriptorHandleForHeapStart());
  // readback + upload (same footprint)
  UINT rows = 0;
  UINT64 rowBytes = 0;
  c.dev->GetCopyableFootprints(&rd, 0, 1, 0, &c.fp, &rows, &rowBytes, &c.rbBytes);
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = c.rbBytes;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  D3D12_HEAP_PROPERTIES rb{};
  rb.Type = D3D12_HEAP_TYPE_READBACK;
  if (FAILED(c.dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&c.readback))))
    return false;
  ComPtr<ID3D12Resource> up;
  D3D12_HEAP_PROPERTIES uh{};
  uh.Type = D3D12_HEAP_TYPE_UPLOAD;
  if (FAILED(c.dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&up))))
    return false;
  const std::vector<uint8_t> px = loadBackground(bgPath, w, h);
  uint8_t* mp = nullptr;
  up->Map(0, nullptr, reinterpret_cast<void**>(&mp));
  for (int y = 0; y < h; ++y) std::memcpy(mp + c.fp.Offset + size_t(y) * c.fp.Footprint.RowPitch, &px[size_t(y) * w * 4], size_t(w) * 4);
  up->Unmap(0, nullptr);
  c.alloc->Reset();
  c.cl->Reset(c.alloc.Get(), nullptr);
  D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
  dst.pResource = c.bg.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.pResource = up.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint = c.fp;
  c.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  barrier(c.cl.Get(), c.bg.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  c.cl->Close();
  ID3D12CommandList* l[] = {c.cl.Get()};
  c.q->ExecuteCommandLists(1, l);
  waitGpu(c);
  return true;
}

// One frame: background -> RT, the overlay, optionally a readback to <path>.
void frame(Ctx& c, const std::string& shotPath) {
  c.alloc->Reset();
  c.cl->Reset(c.alloc.Get(), nullptr);
  c.cl->CopyResource(c.rt.Get(), c.bg.Get());
  barrier(c.cl.Get(), c.rt.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtvHeap->GetCPUDescriptorHandleForHeapStart();
  c.cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT vp{0, 0, float(c.w), float(c.h), 0, 1};
  const D3D12_RECT sc{0, 0, LONG(c.w), LONG(c.h)};
  c.cl->RSSetViewports(1, &vp);
  c.cl->RSSetScissorRects(1, &sc);
  if (g_settings || g_ach) {
    sr::gfx::settingsui::FrameCtx sc2;
    sc2.device = c.dev.Get();
    sc2.queue = c.q.Get();
    sc2.cl = c.cl.Get();
    sc2.rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc2.bbW = c.w;
    sc2.bbH = c.h;
    sr::gfx::settingsui::render(sc2, false);
  } else {
    sr::gfx::modlibui::FrameCtx fc;
    fc.device = c.dev.Get();
    fc.queue = c.q.Get();
    fc.cl = c.cl.Get();
    fc.rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    fc.bbW = c.w;
    fc.bbH = c.h;
    sr::gfx::modlibui::render(fc);
  }
  barrier(c.cl.Get(), c.rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  if (!shotPath.empty()) {
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = c.readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = c.fp;
    src.pResource = c.rt.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    c.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  barrier(c.cl.Get(), c.rt.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  c.cl->Close();
  ID3D12CommandList* l[] = {c.cl.Get()};
  c.q->ExecuteCommandLists(1, l);
  waitGpu(c);
  if (shotPath.empty()) return;
  uint8_t* mp = nullptr;
  c.readback->Map(0, nullptr, reinterpret_cast<void**>(&mp));
  const int w = c.w, h = c.h;
  const uint32_t rowOut = (uint32_t(w) * 3u + 3u) & ~3u;
  std::vector<uint8_t> out(size_t(rowOut) * h + 54, 0);
  auto le32 = [&](size_t o, uint32_t v) { std::memcpy(&out[o], &v, 4); };
  out[0] = 'B';
  out[1] = 'M';
  le32(2, uint32_t(out.size()));
  le32(10, 54);
  le32(14, 40);
  le32(18, uint32_t(w));
  le32(22, uint32_t(h));
  out[26] = 1;
  out[28] = 24;
  for (int y = 0; y < h; ++y) {
    const uint8_t* s = mp + c.fp.Offset + size_t(y) * c.fp.Footprint.RowPitch;
    uint8_t* d = &out[54 + size_t(h - 1 - y) * rowOut];
    for (int x = 0; x < w; ++x) {
      d[x * 3 + 0] = s[x * 4 + 2];
      d[x * 3 + 1] = s[x * 4 + 1];
      d[x * 3 + 2] = s[x * 4 + 0];
    }
  }
  c.readback->Unmap(0, nullptr);
  if (std::FILE* f = std::fopen(shotPath.c_str(), "wb")) {
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    std::printf("[mod-library-render] wrote %s\n", shotPath.c_str());
  }
}

// Lane E E3: the pad names (A B X Y LB RB LT RT) are the key that does that act on the host screens under the
// keyboard rule the screens use for this (real-keyboard) source (kernel/pc_controls.hpp hostKeyFor), so a script
// presses what a player would and the drawn caps match; *mods = Shift for Shift+Tab. Literal key names stay literal.
int scancodeFor(const std::string& n, uint16_t* mods = nullptr) {
  namespace PC = sr::kernel::pccontrols;
  if (mods) *mods = 0;
  static const std::pair<const char*, PC::HostAct> pad[] = {
      {"A", PC::HostAct::Accept}, {"B", PC::HostAct::Back}, {"X", PC::HostAct::X},   {"Y", PC::HostAct::Y},
      {"LB", PC::HostAct::LB},    {"RB", PC::HostAct::RB},  {"LT", PC::HostAct::LT}, {"RT", PC::HostAct::RT}};
  for (const auto& e : pad)
    if (n == e.first) {
      bool shift = false;
      const uint16_t sc = PC::hostKeyFor(e.second, PC::hostKeymap(false), PC::config().bindings, &shift);
      if (mods && shift) *mods = SDL_KMOD_LSHIFT;
      return sc;
    }
  static const std::pair<const char*, int> k[] = {
      {"UP", SDL_SCANCODE_UP}, {"DOWN", SDL_SCANCODE_DOWN}, {"LEFT", SDL_SCANCODE_LEFT}, {"RIGHT", SDL_SCANCODE_RIGHT},
      {"ENTER", SDL_SCANCODE_RETURN}, {"ESC", SDL_SCANCODE_ESCAPE}, {"SPACE", SDL_SCANCODE_SPACE},
      {"TAB", SDL_SCANCODE_TAB}, {"SLASH", SDL_SCANCODE_SLASH}, {"BACK", SDL_SCANCODE_BACKSPACE},
      {"P", SDL_SCANCODE_P}, {"R", SDL_SCANCODE_R}, {"E", SDL_SCANCODE_E}, {"F", SDL_SCANCODE_F},
      {"Q", SDL_SCANCODE_Q}, {"Z", SDL_SCANCODE_Z}};
  for (const auto& e : k)
    if (n == e.first) return e.second;
  return 0;
}

bool parseXY(const std::string& s, float* x, float* y) { return std::sscanf(s.c_str(), "%f,%f", x, y) == 2; }

}  // namespace

// iter-466 gfx: the PC SETTINGS screen through the same harness. The content root becomes
// SR_CONTENT_DIR first, so every settings file the screen writes lands in the test root.
int runModLibraryRender(int argc, char** argv);
int runSettingsRender(int argc, char** argv) {
  if (argc < 7) {
    std::puts("usage: --settings-render <content root> <out dir> <WxH> <background|-> <script>");
    return 2;
  }
  std::error_code ec;
  const fs::path root = fs::absolute(fs::u8path(argv[2]));
  fs::create_directories(root / "settings", ec);
  _wputenv_s(L"SR_CONTENT_DIR", root.wstring().c_str());
  g_settings = true;
  return runModLibraryRender(argc, argv);
}

// lane E L-a: the ACHIEVEMENTS page through the same harness (the data: see the header).
int runAchievementsRender(int argc, char** argv) {
  if (argc < 7) {
    std::puts("usage: --achievements-render <content root> <out dir> <WxH> <background|-> <script>");
    return 2;
  }
  namespace A = sr::ach;
  std::error_code ec;
  const fs::path root = fs::absolute(fs::u8path(argv[2]));
  fs::create_directories(root / "settings", ec);
  _wputenv_s(L"SR_CONTENT_DIR", root.wstring().c_str());
  A::Snapshot s;
  std::vector<uint8_t> db;
  if (const char* x = std::getenv("SR_ACH_XDBF")) {
    std::ifstream f(fs::u8path(x), std::ios::binary);
    db.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (db.empty()) {
      std::printf("[achievements-render] SR_ACH_XDBF '%s' could not be read\n", x);
      return 1;
    }
  }
  if (!db.empty()) {
    A::setTableForTest(db);   // the icons (iconPng) come from the same bytes
    const A::Table t = A::parseTable(db, 0);
    if (!t.ok) {
      std::printf("[achievements-render] SR_ACH_XDBF unreadable: %s\n", t.why.c_str());
      return 1;
    }
    s.list = t.list;
  } else {
    for (uint32_t i = 1; i <= 18; ++i) {
      A::Achievement a;
      a.id = i;
      a.name = "Achievement " + std::to_string(i);
      a.unlockedText = "What was done for achievement " + std::to_string(i) + ".";
      a.lockedText = "How to earn achievement " + std::to_string(i) + ".";
      a.gamerscore = i % 4 == 0 ? 100 : 50;
      a.secret = i % 3 == 0;
      s.list.push_back(a);
    }
  }
  s.table = A::TableStatus::Ok;
  if (const char* u = std::getenv("SR_ACH_RENDER_UNLOCKS")) {
    std::stringstream us(u);
    std::string tok;
    int64_t when = 1729670000000LL;
    while (std::getline(us, tok, ','))
      if (!tok.empty()) s.unlocked.push_back({uint32_t(std::strtoul(tok.c_str(), nullptr, 10)), when += 60000});
  }
  const char* sl = std::getenv("SR_ACH_RENDER_SLOT");
  const std::string slot = sl ? sl : "1";
  if (slot == "none") {
    s.save = A::SaveStatus::NoSave;
    s.slot = -1;
  } else if (slot == "bad") {
    s.save = A::SaveStatus::SlotUnreadable;
    s.slot = 0;
  } else {
    s.save = A::SaveStatus::Slot;
    s.slot = std::atoi(slot.c_str());
  }
  if (const char* nt = std::getenv("SR_ACH_RENDER_NOTABLE"); nt && nt[0] == '1') s = A::Snapshot{};
  s.generation = 1;
  sr::gfx::achui::setSnapshotForTest(s);
  g_ach = true;
  return runModLibraryRender(argc, argv);
}

int runModLibraryRender(int argc, char** argv) {
  if (argc < 7) {
    std::printf("usage: --mod-library-render <content root> <out dir> <WxH> <background|-> \"<script>\"\n");
    return 2;
  }
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const std::string root = argv[2], outDir = argv[3], bg = argv[5], script = argv[6];
  int w = 1920, h = 1080;
  std::sscanf(argv[4], "%dx%d", &w, &h);
  std::error_code ec;
  fs::create_directories(fs::u8path(outDir), ec);
  sr::kernel::ModBootEnv e;
  e.contentRoot = sr::kernel::modPathU8(fs::absolute(fs::u8path(root))) + "/";
  wchar_t buf[MAX_PATH * 2] = {};
  GetModuleFileNameW(nullptr, buf, DWORD(sizeof buf / sizeof buf[0]));
  e.exeDir = sr::kernel::modPathU8(fs::path(buf).parent_path());
  e.automated = false;
  e.allowStateWrite = true;
  e.hasPort = true;
  e.srPortFeatures = "1";
  sr::kernel::modOverlayResetForTestEnv(e);
  Ctx c;
  if (!init(c, w, h, bg)) {
    std::printf("[mod-library-render] D3D12 init failed\n");
    return 1;
  }
  if (g_ach) ML::requestOpenPage(ML::OverlayPage::Achievements, "IG");
  else if (g_settings) ML::requestOpenPage(ML::OverlayPage::Settings, "FE");
  else ML::requestOpen("FE");
  auto frames = [&](int n) {
    for (int i = 0; i < n; ++i) {
      frame(c, "");
      std::this_thread::sleep_for(std::chrono::milliseconds(14));
    }
  };
  auto ev = [&](ML::InputEvent::Kind k, float x, float y, int b, int wh) {
    ML::InputEvent e2;
    e2.kind = k;
    e2.x = x;
    e2.y = y;
    e2.button = b;
    e2.wheel = wh;
    ML::injectForTest(e2);
  };
  float mx = 640, my = 360;
  std::stringstream ss(script);
  std::string cmd;
  while (std::getline(ss, cmd, ';')) {
    std::istringstream cs(cmd);
    std::string verb, arg;
    cs >> verb;
    std::getline(cs, arg);
    while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
    if (verb.empty()) continue;
    if (verb == "frames") frames(std::max(1, std::atoi(arg.c_str())));
    else if (verb == "key") {
      ML::InputEvent k;
      k.kind = ML::InputEvent::Key;
      uint16_t mods = 0;
      k.scancode = scancodeFor(arg, &mods);
      k.mods = mods;
      k.down = true;
      ML::injectForTest(k);
      frames(1);
      k.down = false;
      ML::injectForTest(k);
      frames(6);
    } else if (verb == "khold" || verb == "krelease") {
      ML::InputEvent k;
      k.kind = ML::InputEvent::Key;
      uint16_t mods = 0;
      k.scancode = scancodeFor(arg, &mods);   // Astra E3 r1: a held LB is Shift+Tab, as "key LB" is
      k.mods = mods;
      k.down = verb == "khold";
      ML::injectForTest(k);
      frames(1);
    } else if (verb == "type") {
      ML::InputEvent t;
      t.kind = ML::InputEvent::Text;
      std::snprintf(t.text, sizeof t.text, "%s", arg.c_str());
      ML::injectForTest(t);
      frames(4);
    } else if (verb == "move") {
      float x, y;
      if (parseXY(arg, &x, &y)) {
        for (int i = 1; i <= 8; ++i) {
          ev(ML::InputEvent::MouseMove, mx + (x - mx) * i / 8.0f, my + (y - my) * i / 8.0f, 0, 0);
          frames(1);
        }
        mx = x;
        my = y;
      }
    } else if (verb == "click" || verb == "rclick") {
      float x = mx, y = my;
      parseXY(arg, &x, &y);
      ev(ML::InputEvent::MouseMove, x, y, 0, 0);
      mx = x;
      my = y;
      frames(1);
      const int b = verb == "click" ? 1 : 3;
      ev(ML::InputEvent::MouseDown, x, y, b, 0);
      frames(1);
      ev(ML::InputEvent::MouseUp, x, y, b, 0);
      frames(6);
    } else if (verb == "down") {
      ev(ML::InputEvent::MouseDown, mx, my, 1, 0);
      frames(1);
    } else if (verb == "up") {
      ev(ML::InputEvent::MouseUp, mx, my, 1, 0);
      frames(2);
    } else if (verb == "wheel") {
      ev(ML::InputEvent::Wheel, mx, my, 0, std::atoi(arg.c_str()));
      frames(10);
    } else if (verb == "request") {   // lane E L-a: another page asked (settings | achievements)
      ML::requestOpenPage(arg == "settings" ? ML::OverlayPage::Settings : ML::OverlayPage::Achievements, "IG");
      frames(2);
    } else if (verb == "shot") {
      char name[256];
      std::snprintf(name, sizeof name, "%s/%s_%dx%d.bmp", outDir.c_str(), arg.c_str(), w, h);
      frame(c, name);
    } else {
      std::printf("[mod-library-render] unknown command '%s'\n", verb.c_str());
      return 2;
    }
  }
  return 0;
}
#endif
