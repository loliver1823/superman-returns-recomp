// iter-466 gfx: THE HOST OVERLAY KIT - the drawing layer the MODS library (gfx/mod_library_ui.cpp)
// and the PC SETTINGS screen (gfx/settings_ui.cpp) share. Moved out of mod_library_ui.cpp unchanged
// (iter-465 modlib lane's code): the game's palette, the 1280x720 design transform, the shared
// ImGui context / DX12 backend / descriptor heap, the image cache, the Saira fonts, the glass /
// chrome / focus primitives, the text helpers and the device-matched button prompts.
// Contract: overlay_kit.hpp.
#ifdef _WIN32

#include "overlay_kit.hpp"

#include "glyph_subst.hpp"
#include "../app/log.hpp"
#include "../kernel/pc_controls.hpp"   // lane E E3: the key caps follow the PC controls (kbLabel)

#include "backends/imgui_impl_dx12.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sr {
namespace gfx {
namespace overlay {

namespace fs = std::filesystem;

std::string upperAscii(std::string s) {
  for (char& c : s) c = char((c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c);
  return s;
}

double nowSec() {
  static const auto t0 = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

float approach(float cur, float target, float rate, float dt) {
  return cur + (target - cur) * (1.0f - std::exp(-rate * dt));
}
float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
float smooth01(float t) {
  t = clampf(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

std::wstring widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(std::max(n, 0)), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string exeDir() {
  static const std::string d = [] {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(sizeof buf / sizeof buf[0]));
    if (!n) return std::string(".");
    const fs::path p = fs::path(buf).parent_path();
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
  }();
  return d;
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// Palette (sampled from the game's own popups: the MAIN MENU and GAMEPLAY OPTIONS frames -
// deep-blue glass, bright crystal chrome, silver-white type with a dark drop shadow - plus the
// cape's red and the shield's gold for focus and selection)
// ═════════════════════════════════════════════════════════════════════════════════════════
float gAlpha = 1.0f;   // the open / close fade, applied to every colour

ImU32 col(float r, float g, float b, float a) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, clampf(a * gAlpha, 0.0f, 1.0f)));
}
ImU32 colRaw(float r, float g, float b, float a) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, clampf(a, 0.0f, 1.0f)));
}
ImU32 C(const Rgb& c, float a) { return col(c.r, c.g, c.b, a); }

// ═════════════════════════════════════════════════════════════════════════════════════════
// Design space: 1280x720 units over the game's 16:9 image
// ═════════════════════════════════════════════════════════════════════════════════════════
Xf X;

// ═════════════════════════════════════════════════════════════════════════════════════════
// GPU side: one shader-visible SRV heap (font atlas + images), a texture cache fed by a decode
// worker, and a graveyard that releases resources only after the frames using them retired.
// ═════════════════════════════════════════════════════════════════════════════════════════

Gpu G;

int allocSlot() {
  if (G.freeSlots.empty()) return -1;
  const int s = G.freeSlots.back();
  G.freeSlots.pop_back();
  return s;
}
void freeSlot(int s) {
  if (s >= 0) G.freeSlots.push_back(s);
}
D3D12_CPU_DESCRIPTOR_HANDLE cpuH(int s) {
  D3D12_CPU_DESCRIPTOR_HANDLE h = G.heap->GetCPUDescriptorHandleForHeapStart();
  h.ptr += SIZE_T(s) * G.inc;
  return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE gpuH(int s) {
  D3D12_GPU_DESCRIPTOR_HANDLE h = G.heap->GetGPUDescriptorHandleForHeapStart();
  h.ptr += UINT64(s) * G.inc;
  return h;
}
void retire(const ComPtr<ID3D12Resource>& r) {
  if (r) G.grave.push_back({G.frame + kFramesInFlight + 2, r});
}
void collectGrave() {
  auto it = std::remove_if(G.grave.begin(), G.grave.end(),
                           [](const auto& e) { return e.first <= G.frame; });
  G.grave.erase(it, G.grave.end());
}
// Waits until the queue has executed everything submitted so far (font rebuilds only).
void flushGpu() {
  if (!G.queue || !G.fence) return;
  ++G.fenceVal;
  if (FAILED(G.queue->Signal(G.fence.Get(), G.fenceVal))) return;
  if (G.fence->GetCompletedValue() < G.fenceVal) {
    G.fence->SetEventOnCompletion(G.fenceVal, G.ev);
    WaitForSingleObject(G.ev, 5000);
  }
  G.grave.clear();
}

void imguiSrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* c, D3D12_GPU_DESCRIPTOR_HANDLE* g) {
  const int s = allocSlot();
  *c = cpuH(std::max(s, 0));
  *g = gpuH(std::max(s, 0));
}
void imguiSrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE c, D3D12_GPU_DESCRIPTOR_HANDLE) {
  const SIZE_T base = G.heap->GetCPUDescriptorHandleForHeapStart().ptr;
  freeSlot(int((c.ptr - base) / G.inc));
}

// ── images: decode worker (WIC: PNG / JPG), uploads on the render thread ─────────────────
struct Tex {
  int state = 0;   // 0 queued, 1 ready, 2 failed
  int w = 0, h = 0;
  int slot = -1;
  ComPtr<ID3D12Resource> res;
};
struct Job {
  std::string key, path;
  int maxW = 0, maxH = 0;
  bool size = false;          // a folder-size job (path = folder)
  std::function<bool(std::vector<uint8_t>*)> fetchBytes;   // lane E L-a: an in-memory image (path unused)
  bool grey = false;                                       // ... decoded to grey
};
struct Done {
  std::string key;
  bool ok = false, size = false;
  int w = 0, h = 0;
  std::vector<uint8_t> px;
  uint64_t bytes = 0;
};
std::unordered_map<std::string, Tex> gTex;
std::unordered_map<std::string, uint64_t> gSizes;
std::set<std::string> gSizePending;
std::mutex gWMu;
std::condition_variable gWCv;
std::deque<Job> gJobs;
std::vector<Done> gDone;
std::thread gWorker;
bool gWorkerUp = false;

bool wicDecode(const std::string& path, int maxW, int maxH, Done* out) {
  ComPtr<IWICImagingFactory> f;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))))
    return false;
  ComPtr<IWICBitmapDecoder> dec;
  ComPtr<IWICBitmapFrameDecode> fr;
  const std::wstring w = widen(path);
  if (FAILED(f->CreateDecoderFromFilename(w.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
      FAILED(dec->GetFrame(0, &fr)))
    return false;
  UINT sw = 0, sh = 0;
  fr->GetSize(&sw, &sh);
  if (!sw || !sh || sw > 16384 || sh > 16384) return false;
  ComPtr<IWICBitmapSource> src = fr;
  UINT dw = sw, dh = sh;
  if (maxW > 0 && maxH > 0 && (sw > UINT(maxW) || sh > UINT(maxH))) {
    const double k = std::min(double(maxW) / sw, double(maxH) / sh);
    dw = std::max(1u, UINT(std::lround(sw * k)));
    dh = std::max(1u, UINT(std::lround(sh * k)));
    ComPtr<IWICBitmapScaler> sc;
    if (FAILED(f->CreateBitmapScaler(&sc)) ||
        FAILED(sc->Initialize(fr.Get(), dw, dh, WICBitmapInterpolationModeFant)))
      return false;
    src = sc;
  }
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(f->CreateFormatConverter(&conv)) ||
      FAILED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom)))
    return false;
  out->w = int(dw);
  out->h = int(dh);
  out->px.resize(size_t(dw) * dh * 4u);
  return SUCCEEDED(conv->CopyPixels(nullptr, dw * 4u, UINT(out->px.size()), out->px.data()));
}

// lane E L-a: the same decode from encoded bytes in memory (a WIC stream over them) - additive; wicDecode above (the
// image files') is unchanged.
bool wicDecodeBytes(const std::vector<uint8_t>& bytes, int maxW, int maxH, Done* out) {
  if (bytes.empty() || bytes.size() > (64u << 20)) return false;
  ComPtr<IWICImagingFactory> f;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))))
    return false;
  ComPtr<IWICStream> st;
  if (FAILED(f->CreateStream(&st)) ||
      FAILED(st->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), DWORD(bytes.size()))))
    return false;
  ComPtr<IWICBitmapDecoder> dec;
  ComPtr<IWICBitmapFrameDecode> fr;
  if (FAILED(f->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) ||
      FAILED(dec->GetFrame(0, &fr)))
    return false;
  UINT sw = 0, sh = 0;
  fr->GetSize(&sw, &sh);
  if (!sw || !sh || sw > 16384 || sh > 16384) return false;
  ComPtr<IWICBitmapSource> src = fr;
  UINT dw = sw, dh = sh;
  if (maxW > 0 && maxH > 0 && (sw > UINT(maxW) || sh > UINT(maxH))) {
    const double k = std::min(double(maxW) / sw, double(maxH) / sh);
    dw = std::max(1u, UINT(std::lround(sw * k)));
    dh = std::max(1u, UINT(std::lround(sh * k)));
    ComPtr<IWICBitmapScaler> sc;
    if (FAILED(f->CreateBitmapScaler(&sc)) ||
        FAILED(sc->Initialize(fr.Get(), dw, dh, WICBitmapInterpolationModeFant)))
      return false;
    src = sc;
  }
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(f->CreateFormatConverter(&conv)) ||
      FAILED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom)))
    return false;
  out->w = int(dw);
  out->h = int(dh);
  out->px.resize(size_t(dw) * dh * 4u);
  return SUCCEEDED(conv->CopyPixels(nullptr, dw * 4u, UINT(out->px.size()), out->px.data()));
}

// lane E L-a: grey (Rec. 601 luminance, (77 R + 150 G + 29 B) / 256), alpha kept.
void toGrey(std::vector<uint8_t>* px) {
  for (size_t i = 0; i + 3 < px->size(); i += 4) {
    uint8_t* p = px->data() + i;
    const uint8_t y = uint8_t((77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8);
    p[0] = p[1] = p[2] = y;
  }
}

void workerMain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  for (;;) {
    Job j;
    {
      std::unique_lock<std::mutex> lk(gWMu);
      gWCv.wait(lk, [] { return !gJobs.empty(); });
      j = std::move(gJobs.front());
      gJobs.pop_front();
    }
    Done d;
    d.key = j.key;
    d.size = j.size;
    if (j.size) {
      std::error_code ec;
      uint64_t total = 0;
      for (fs::recursive_directory_iterator it(fs::u8path(j.path), fs::directory_options::skip_permission_denied, ec), end;
           !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) total += uint64_t(it->file_size(e2));
      }
      d.ok = true;
      d.bytes = total;
    } else if (j.fetchBytes) {   // lane E L-a: an in-memory image
      std::vector<uint8_t> bytes;
      d.ok = j.fetchBytes(&bytes) && wicDecodeBytes(bytes, j.maxW, j.maxH, &d);
      if (d.ok && j.grey) toGrey(&d.px);
    } else {
      d.ok = wicDecode(j.path, j.maxW, j.maxH, &d);
    }
    std::lock_guard<std::mutex> lk(gWMu);
    gDone.push_back(std::move(d));
  }
}

void enqueue(Job j) {
  if (!gWorkerUp) {
    gWorkerUp = true;
    gWorker = std::thread(workerMain);
    gWorker.detach();
  }
  {
    std::lock_guard<std::mutex> lk(gWMu);
    gJobs.push_back(std::move(j));
  }
  gWCv.notify_one();
}

// The texture for an image file (key = path + "|" + size class). 0 until it is uploaded.
ImTextureID image(const std::string& path, int maxW, int maxH, int* w, int* h) {
  if (path.empty()) return ImTextureID(0);
  const std::string key = path + "|" + std::to_string(maxW);
  auto it = gTex.find(key);
  if (it == gTex.end()) {
    gTex[key] = Tex{};
    enqueue(Job{key, path, maxW, maxH, false});
    return ImTextureID(0);
  }
  if (it->second.state != 1) return ImTextureID(0);
  if (w) *w = it->second.w;
  if (h) *h = it->second.h;
  return ImTextureID(gpuH(it->second.slot).ptr);
}

bool imageFailed(const std::string& path, int maxW) {
  auto it = gTex.find(path + "|" + std::to_string(maxW));
  return it != gTex.end() && it->second.state == 2;
}

// never a file path's key; the grey decode is a texture of its own
static std::string memKey(const std::string& key, int maxW, bool grey) {
  return "mem:" + key + (grey ? "|grey|" : "|") + std::to_string(maxW);
}

ImTextureID imageFromMemory(const std::string& key, int maxW, int maxH,
                            const std::function<bool(std::vector<uint8_t>*)>& fetch, bool grey, int* w, int* h) {
  if (key.empty() || !fetch) return ImTextureID(0);
  const std::string k = memKey(key, maxW, grey);
  auto it = gTex.find(k);
  if (it == gTex.end()) {
    gTex[k] = Tex{};
    Job j;
    j.key = k;
    j.maxW = maxW;
    j.maxH = maxH;
    j.fetchBytes = fetch;
    j.grey = grey;
    enqueue(std::move(j));
    return ImTextureID(0);
  }
  if (it->second.state != 1) return ImTextureID(0);
  if (w) *w = it->second.w;
  if (h) *h = it->second.h;
  return ImTextureID(gpuH(it->second.slot).ptr);
}

bool imageFromMemoryFailed(const std::string& key, int maxW, bool grey) {
  auto it = gTex.find(memKey(key, maxW, grey));
  return it != gTex.end() && it->second.state == 2;
}

bool decodeImageBytesForTest(const std::vector<uint8_t>& bytes, int maxW, int maxH, int* w, int* h,
                             std::vector<uint8_t>* rgba, bool grey) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  Done d;
  if (!wicDecodeBytes(bytes, maxW, maxH, &d)) return false;
  if (grey) toGrey(&d.px);
  if (w) *w = d.w;
  if (h) *h = d.h;
  if (rgba) *rgba = std::move(d.px);
  return true;
}

std::vector<DecodedForTest> takeDecodedForTest(int want, int timeoutMs) {
  std::vector<DecodedForTest> out;
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    {
      std::lock_guard<std::mutex> lk(gWMu);
      for (auto it = gDone.begin(); it != gDone.end();) {
        if (it->size) {
          ++it;
          continue;
        }
        DecodedForTest d;
        d.key = it->key;
        d.ok = it->ok;
        d.w = it->w;
        d.h = it->h;
        d.px = std::move(it->px);
        out.push_back(std::move(d));
        it = gDone.erase(it);
      }
    }
    if (int(out.size()) >= want || std::chrono::steady_clock::now() >= until) return out;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

// Size of a mod folder (computed once on the worker).
bool folderSize(const std::string& id, const std::string& root, uint64_t* out) {
  auto it = gSizes.find(id);
  if (it != gSizes.end()) {
    *out = it->second;
    return true;
  }
  if (!root.empty() && !gSizePending.count(id)) {
    gSizePending.insert(id);
    enqueue(Job{"size:" + id, root, 0, 0, true});
  }
  return false;
}

bool uploadTexture(ID3D12GraphicsCommandList* cl, const Done& d, Tex* t) {
  const int slot = allocSlot();
  if (slot < 0) return false;
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = UINT64(d.w);
  rd.Height = UINT(d.h);
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  ComPtr<ID3D12Resource> tex;
  if (FAILED(G.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, IID_PPV_ARGS(&tex)))) {
    freeSlot(slot);
    return false;
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT rows = 0;
  UINT64 rowBytes = 0, total = 0;
  G.dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rowBytes, &total);
  D3D12_HEAP_PROPERTIES up{};
  up.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = total;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> upload;
  if (FAILED(G.dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, IID_PPV_ARGS(&upload)))) {
    freeSlot(slot);
    return false;
  }
  uint8_t* mp = nullptr;
  D3D12_RANGE none{0, 0};
  if (FAILED(upload->Map(0, &none, reinterpret_cast<void**>(&mp)))) {
    freeSlot(slot);
    return false;
  }
  for (int y = 0; y < d.h; ++y)
    std::memcpy(mp + fp.Offset + size_t(y) * fp.Footprint.RowPitch, d.px.data() + size_t(y) * d.w * 4u,
                size_t(d.w) * 4u);
  upload->Unmap(0, nullptr);
  D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
  dst.pResource = tex.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.pResource = upload.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint = fp;
  cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = tex.Get();
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  cl->ResourceBarrier(1, &b);
  D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
  sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sv.Texture2D.MipLevels = 1;
  G.dev->CreateShaderResourceView(tex.Get(), &sv, cpuH(slot));
  retire(upload);   // alive until this frame's copy has executed
  t->res = tex;
  t->slot = slot;
  t->w = d.w;
  t->h = d.h;
  t->state = 1;
  return true;
}

void pumpUploads(ID3D12GraphicsCommandList* cl) {
  std::vector<Done> done;
  {
    std::lock_guard<std::mutex> lk(gWMu);
    // a few per frame, so a page of thumbnails never stalls one present
    const size_t n = std::min<size_t>(gDone.size(), 4);
    done.assign(std::make_move_iterator(gDone.begin()), std::make_move_iterator(gDone.begin() + long(n)));
    gDone.erase(gDone.begin(), gDone.begin() + long(n));
  }
  for (Done& d : done) {
    if (d.size) {
      gSizes[d.key.substr(5)] = d.bytes;
      continue;
    }
    auto it = gTex.find(d.key);
    if (it == gTex.end() || it->second.state == 1) continue;   // released meanwhile / already up
    if (!d.ok || !uploadTexture(cl, d, &it->second)) {
      it->second.state = 2;
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[OVERLAY] image not loaded: %s", d.key.c_str());
    }
  }
}

// Releases every image (the library closed): the GPU memory goes back after the frames retire.
void releaseImages() {
  for (auto& kv : gTex) {
    if (kv.second.res) retire(kv.second.res);
    freeSlot(kv.second.slot);
  }
  gTex.clear();
  std::lock_guard<std::mutex> lk(gWMu);
  gJobs.clear();
  gDone.clear();
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// Fonts: Saira SR (OFL), rasterised at the output's pixel size
// ═════════════════════════════════════════════════════════════════════════════════════════
Fonts F;

std::string fontDir() {
  static const std::string d = [] {
    for (const std::string& c : {exeDir() + "/data/fonts/saira", std::string("data/fonts/saira")}) {
      std::error_code ec;
      if (fs::is_regular_file(fs::u8path(c + "/SairaSR-Medium.ttf"), ec)) return c;
    }
    return exeDir() + "/data/fonts/saira";
  }();
  return d;
}

bool buildFonts(float s) {
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();
  static const ImWchar kFull[] = {0x0020, 0x017F, 0x2013, 0x2014, 0x2018, 0x201D, 0x2022, 0x2022,
                                  0x2026, 0x2026, 0x2039, 0x203A, 0};
  static const ImWchar kCaps[] = {0x0020, 0x0060, 0x007B, 0x007E, 0x00B7, 0x00B7, 0x00C0, 0x00DE,
                                  0x2013, 0x2014, 0x2026, 0x2026, 0x2039, 0x203A, 0};
  const std::string dir = fontDir();
  auto add = [&](const char* file, float design, const ImWchar* ranges) -> ImFont* {
    ImFontConfig cfg;
    const float px = std::max(6.0f, std::round(design * s));
    cfg.OversampleH = px < 28.0f ? 2 : 1;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = false;
    const std::string path = dir + "/" + file;
    ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), px, &cfg, ranges);
    if (!f) SR_LOG_BOOT(sr::LogSeverity::Error, "[OVERLAY] font not loaded: %s", path.c_str());
    return f;
  };
  F.display = add("SairaSR-ExpandedBlackItalic.ttf", kSzDisplay, kCaps);
  F.title = add("SairaSR-ExpandedBlackItalic.ttf", kSzTitle, kFull);
  F.tab = add("SairaSR-ExpandedBoldItalic.ttf", kSzTab, kCaps);
  F.card = add("SairaSR-ExpandedBoldItalic.ttf", kSzCard, kFull);
  F.label = add("SairaSR-ExpandedSemiBold.ttf", kSzLabel, kFull);
  F.badge = add("SairaSR-ExpandedSemiBold.ttf", kSzBadge, kCaps);
  F.body = add("SairaSR-Medium.ttf", kSzBody, kFull);
  F.smallF = add("SairaSR-Medium.ttf", kSzSmall, kFull);
  F.ok = F.display && F.title && F.tab && F.card && F.label && F.badge && F.body && F.smallF;
  if (!F.ok) {
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.SizePixels = std::round(13.0f * s);
    ImFont* d = io.Fonts->AddFontDefault(&cfg);
    F.display = F.title = F.tab = F.card = F.label = F.badge = F.body = F.smallF = d;
  }
  io.Fonts->Build();
  F.scale = s;
  int tw = 0, th = 0;
  unsigned char* px = nullptr;
  io.Fonts->GetTexDataAsAlpha8(&px, &tw, &th);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[OVERLAY] fonts %s at scale %.3f (atlas %dx%d, %s)", F.ok ? "Saira SR" : "DEFAULT",
              s, tw, th, dir.c_str());
  return F.ok;
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// Drawing primitives (design coords in, pixels out)
// ═════════════════════════════════════════════════════════════════════════════════════════
ImDrawList* DL = nullptr;

// Design-space clip stack (hit tests honour it, like the draw list's clip rects).
std::vector<RectD> gClip;
void pushClip(float x0, float y0, float x1, float y1) {
  RectD r{x0, y0, x1, y1};
  if (!gClip.empty()) {
    const RectD& c = gClip.back();
    r = {std::max(r.x0, c.x0), std::max(r.y0, c.y0), std::min(r.x1, c.x1), std::min(r.y1, c.y1)};
  }
  gClip.push_back(r);
  DL->PushClipRect(X.p(r.x0, r.y0), X.p(r.x1, r.y1), false);
}
void popClip() {
  gClip.pop_back();
  DL->PopClipRect();
}

ImU32 lerpCol(ImU32 a, ImU32 b, float t) {
  const ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a), cb = ImGui::ColorConvertU32ToFloat4(b);
  return ImGui::ColorConvertFloat4ToU32(ImVec4(ca.x + (cb.x - ca.x) * t, ca.y + (cb.y - ca.y) * t,
                                               ca.z + (cb.z - ca.z) * t, ca.w + (cb.w - ca.w) * t));
}

// A convex polygon (pixels) filled with a vertical gradient.
void gradPoly(const ImVec2* pts, int n, ImU32 top, ImU32 bot) {
  if (n < 3) return;
  float y0 = pts[0].y, y1 = pts[0].y;
  for (int i = 1; i < n; ++i) {
    y0 = std::min(y0, pts[i].y);
    y1 = std::max(y1, pts[i].y);
  }
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  const unsigned base = DL->_VtxCurrentIdx;
  DL->PrimReserve((n - 2) * 3, n);
  for (int i = 0; i < n; ++i) {
    const float t = y1 > y0 ? (pts[i].y - y0) / (y1 - y0) : 0.0f;
    DL->PrimWriteVtx(pts[i], uv, lerpCol(top, bot, t));
  }
  for (int i = 2; i < n; ++i) {
    DL->PrimWriteIdx(ImDrawIdx(base));
    DL->PrimWriteIdx(ImDrawIdx(base + unsigned(i) - 1));
    DL->PrimWriteIdx(ImDrawIdx(base + unsigned(i)));
  }
}

// A chamfered rectangle (design coords): corner cuts tl, tr, br, bl.
int chamfer(float x0, float y0, float x1, float y1, float tl, float tr, float br, float bl, ImVec2* o) {
  int n = 0;
  auto P = [&](float x, float y) { o[n++] = X.p(x, y); };
  if (tl > 0) { P(x0, y0 + tl); P(x0 + tl, y0); } else P(x0, y0);
  if (tr > 0) { P(x1 - tr, y0); P(x1, y0 + tr); } else P(x1, y0);
  if (br > 0) { P(x1, y1 - br); P(x1 - br, y1); } else P(x1, y1);
  if (bl > 0) { P(x0 + bl, y1); P(x0, y1 - bl); } else P(x0, y1);
  return n;
}

// The game's glass panel: a deep-blue gradient, a crystal top edge and a fine chrome border.
void glass(float x0, float y0, float x1, float y1, float cut, float a, bool raised) {
  ImVec2 pts[8];
  const int n = chamfer(x0, y0, x1, y1, cut, cut * 0.35f, cut, cut * 0.35f, pts);
  const ImU32 top = raised ? col(0.13f, 0.33f, 0.72f, 0.92f * a) : col(0.07f, 0.21f, 0.52f, 0.86f * a);
  const ImU32 bot = raised ? col(0.04f, 0.13f, 0.38f, 0.94f * a) : col(0.02f, 0.07f, 0.24f, 0.90f * a);
  gradPoly(pts, n, top, bot);
  // inner sheen: the upper third is lighter (the game's popup glass)
  const float hy = y0 + (y1 - y0) * 0.38f;
  ImVec2 hp[8];
  const int hn = chamfer(x0 + 1, y0 + 1, x1 - 1, hy, std::max(0.0f, cut - 1), cut * 0.35f, 0, 0, hp);
  gradPoly(hp, hn, col(0.55f, 0.75f, 1.0f, 0.10f * a), col(0.55f, 0.75f, 1.0f, 0.0f));
  DL->AddPolyline(pts, n, col(0.45f, 0.66f, 1.0f, 0.42f * a), ImDrawFlags_Closed, std::max(1.0f, X.d(1.0f)));
  // a crisp highlight along the top edge
  DL->AddLine(X.p(x0 + cut + 2, y0 + 1.2f), X.p(x1 - cut * 0.35f - 2, y0 + 1.2f), col(0.80f, 0.90f, 1.0f, 0.55f * a),
              std::max(1.0f, X.d(1.0f)));
}

// The game's bright crystal chrome band (the top and bottom of every popup).
void chrome(float x0, float y0, float x1, float y1, bool top) {
  const float h = y1 - y0;
  const float slant = h * 1.6f;
  ImVec2 band[4] = {X.p(x0 + (top ? slant : 0), y0), X.p(x1 - (top ? slant : 0), y0),
                    X.p(x1 - (top ? 0 : slant), y1), X.p(x0 + (top ? 0 : slant), y1)};
  gradPoly(band, 4, top ? col(0.42f, 0.70f, 1.0f, 0.95f) : col(0.06f, 0.24f, 0.70f, 0.95f),
           top ? col(0.05f, 0.22f, 0.66f, 0.95f) : col(0.36f, 0.64f, 1.0f, 0.95f));
  // facets: lighter angled plates, like the game's bevelled crystal
  const float cx = (x0 + x1) * 0.5f;
  for (int side = -1; side <= 1; side += 2) {
    const float a0 = cx + side * (x1 - x0) * 0.16f, a1 = cx + side * (x1 - x0) * 0.43f;
    const float lo = std::min(a0, a1), hi = std::max(a0, a1);
    ImVec2 f[4] = {X.p(lo + h, y0 + h * 0.18f), X.p(hi - h, y0 + h * 0.18f), X.p(hi, y1 - h * 0.18f),
                   X.p(lo, y1 - h * 0.18f)};
    if (!top) std::swap(f[0], f[3]), std::swap(f[1], f[2]);
    gradPoly(f, 4, col(0.75f, 0.88f, 1.0f, 0.30f), col(0.75f, 0.88f, 1.0f, 0.04f));
  }
  DL->AddPolyline(band, 4, col(0.80f, 0.92f, 1.0f, 0.70f), ImDrawFlags_Closed, std::max(1.0f, X.d(1.0f)));
}

// The game's selection bar: a pale band that fades out at both ends, fine bright edges.
void selBand(float x0, float y0, float x1, float y1, float a) {
  if (a <= 0.01f) return;
  const float m = (x0 + x1) * 0.5f;
  const ImU32 c0 = col(0.62f, 0.80f, 1.0f, 0.0f), c1 = col(0.62f, 0.80f, 1.0f, 0.30f * a);
  DL->AddRectFilledMultiColor(X.p(x0, y0), X.p(m, y1), c0, c1, c1, c0);
  DL->AddRectFilledMultiColor(X.p(m, y0), X.p(x1, y1), c1, c0, c0, c1);
  const ImU32 e0 = col(0.85f, 0.93f, 1.0f, 0.0f), e1 = col(0.85f, 0.93f, 1.0f, 0.75f * a);
  const float t = std::max(1.0f, X.d(1.0f)) / X.s;
  DL->AddRectFilledMultiColor(X.p(x0, y0), X.p(m, y0 + t), e0, e1, e1, e0);
  DL->AddRectFilledMultiColor(X.p(m, y0), X.p(x1, y0 + t), e1, e0, e0, e1);
  DL->AddRectFilledMultiColor(X.p(x0, y1 - t), X.p(m, y1), e0, e1, e1, e0);
  DL->AddRectFilledMultiColor(X.p(m, y1 - t), X.p(x1, y1), e1, e0, e0, e1);
}

// Red-and-gold focus frame (cards, buttons): a gold rim over a red glow.
void focusFrame(float x0, float y0, float x1, float y1, float cut, float a, double t) {
  if (a <= 0.01f) return;
  ImVec2 pts[8];
  for (int k = 3; k >= 1; --k) {
    const float g = float(k) * 2.2f;
    const int n = chamfer(x0 - g, y0 - g, x1 + g, y1 + g, cut + g * 0.6f, (cut + g) * 0.35f, cut + g * 0.6f,
                          (cut + g) * 0.35f, pts);
    DL->AddPolyline(pts, n, col(kRed.r, kRed.g, kRed.b, 0.20f * a / float(k)), ImDrawFlags_Closed, X.d(2.4f));
  }
  const int n = chamfer(x0, y0, x1, y1, cut, cut * 0.35f, cut, cut * 0.35f, pts);
  const float pulse = 0.82f + 0.18f * float(std::sin(t * 3.2));
  DL->AddPolyline(pts, n, col(kGold.r, kGold.g, kGold.b, pulse * a), ImDrawFlags_Closed, std::max(1.5f, X.d(2.0f)));
}

// A diagonal light sweep across a rect (the focused card's sheen).
void sheen(float x0, float y0, float x1, float y1, double t, float a) {
  const float w = x1 - x0, h = y1 - y0;
  const float period = 3.2f;
  const float k = float(std::fmod(t, double(period))) / 1.1f;   // sweeps in the first 1.1 s
  if (k > 1.0f) return;
  const float cx = x0 - h + (w + 2 * h) * k;
  const float bw = w * 0.16f;
  pushClip(x0, y0, x1, y1);
  ImVec2 p[4] = {X.p(cx - bw, y1), X.p(cx + h - bw, y0), X.p(cx + h + bw, y0), X.p(cx + bw, y1)};
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  const ImU32 c0 = col(1, 1, 1, 0.0f), c1 = col(1, 1, 1, 0.22f * a);
  const unsigned base = DL->_VtxCurrentIdx;
  DL->PrimReserve(12, 6);
  const ImVec2 m0 = X.p(cx, y1), m1 = X.p(cx + h, y0);
  DL->PrimWriteVtx(p[0], uv, c0);
  DL->PrimWriteVtx(p[1], uv, c0);
  DL->PrimWriteVtx(m1, uv, c1);
  DL->PrimWriteVtx(m0, uv, c1);
  DL->PrimWriteVtx(p[2], uv, c0);
  DL->PrimWriteVtx(p[3], uv, c0);
  const unsigned idx[12] = {0, 1, 2, 0, 2, 3, 3, 2, 4, 3, 4, 5};
  for (unsigned i : idx) DL->PrimWriteIdx(ImDrawIdx(base + i));
  popClip();
}

// ── text ─────────────────────────────────────────────────────────────────────────────────
float fontPx(float design) { return std::max(6.0f, std::round(design * X.s)); }
float textW(ImFont* f, float size, const std::string& s) {
  return f->CalcTextSizeA(fontPx(size), FLT_MAX, 0.0f, s.c_str()).x / X.s;
}
// Text with the game's look: a dark drop shadow and a vertical white -> silver gradient.
void text(ImFont* f, float size, float x, float y, const std::string& s, ImU32 top, ImU32 bot,
          float shadow) {
  if (s.empty()) return;
  const float px = fontPx(size);
  if (shadow > 0) {
    const ImVec4 tc = ImGui::ColorConvertU32ToFloat4(top);
    const float o = clampf(size * 0.055f, 0.55f, 2.4f) * shadow;
    DL->AddText(f, px, X.p(x + o * 0.9f, y + o), colRaw(0, 0, 0.02f, 0.72f * tc.w), s.c_str());
  }
  const int v0 = DL->VtxBuffer.Size;
  DL->AddText(f, px, X.p(x, y), top, s.c_str());
  if (top != bot) {
    const float ya = X.p(x, y).y, yb = ya + px;
    for (int i = v0; i < DL->VtxBuffer.Size; ++i) {
      ImDrawVert& v = DL->VtxBuffer[i];
      v.col = lerpCol(top, bot, clampf((v.pos.y - ya) / (yb - ya), 0.0f, 1.0f));
    }
  }
}
void textPlain(ImFont* f, float size, float x, float y, const std::string& s, ImU32 c) {
  if (!s.empty()) DL->AddText(f, fontPx(size), X.p(x, y), c, s.c_str());
}
// The game's title type: white over silver, shadowed.
void textTitle(ImFont* f, float size, float x, float y, const std::string& s, float a) {
  text(f, size, x, y, s, C(kWhite, a), col(0.66f, 0.72f, 0.82f, a), 1.0f);
}
std::string ellipsize(ImFont* f, float size, const std::string& s, float maxW) {
  if (textW(f, size, s) <= maxW) return s;
  std::string t = s;
  while (!t.empty()) {
    // drop one UTF-8 code point
    do t.pop_back();
    while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (textW(f, size, t + "\xE2\x80\xA6") <= maxW) return t + "\xE2\x80\xA6";
  }
  return "\xE2\x80\xA6";
}
// Word-wrapped lines of `s` in `width` design units.
std::vector<std::string> wrap(ImFont* f, float size, const std::string& s, float width) {
  std::vector<std::string> out;
  const float px = fontPx(size);
  const float wpx = width * X.s;
  size_t start = 0;
  while (start <= s.size()) {
    size_t nl = s.find('\n', start);
    if (nl == std::string::npos) nl = s.size();
    const char* b = s.c_str() + start;
    const char* e = s.c_str() + nl;
    if (b == e) out.emplace_back();
    while (b < e) {
      const char* cut = f->CalcWordWrapPositionA(px / f->FontSize, b, e, wpx);
      if (cut == b) cut = b + 1;
      std::string line(b, cut);
      while (!line.empty() && line.back() == ' ') line.pop_back();
      out.push_back(line);
      b = cut;
      while (b < e && *b == ' ') ++b;
    }
    start = nl + 1;
  }
  return out;
}


// The shield emblem (red and gold), our own pentagon art.
void shield(float cx, float cy, float r, float a) {
  auto P = [&](float x, float y) { return X.p(cx + x * r, cy + y * r); };
  ImVec2 o[5] = {P(-0.72f, -0.78f), P(0.72f, -0.78f), P(1.02f, -0.36f), P(0, 0.92f), P(-1.02f, -0.36f)};
  gradPoly(o, 5, col(kGold.r, kGold.g, kGold.b, a), col(kGoldDeep.r, kGoldDeep.g, kGoldDeep.b, a));
  ImVec2 i[5] = {P(-0.58f, -0.64f), P(0.58f, -0.64f), P(0.82f, -0.33f), P(0, 0.70f), P(-0.82f, -0.33f)};
  gradPoly(i, 5, col(0.92f, 0.16f, 0.18f, a), col(kRedDeep.r, kRedDeep.g, kRedDeep.b, a));
  ImVec2 d[4] = {P(0, -0.46f), P(0.36f, -0.12f), P(0, 0.34f), P(-0.36f, -0.12f)};
  gradPoly(d, 4, col(1.0f, 0.86f, 0.42f, 0.95f * a), col(0.86f, 0.56f, 0.12f, 0.95f * a));
  DL->AddPolyline(o, 5, col(1.0f, 0.95f, 0.75f, 0.85f * a), ImDrawFlags_Closed, std::max(1.0f, X.d(1.0f)));
}

void checkMark(float x0, float y0, float sz, ImU32 c, float th) {
  DL->AddLine(X.p(x0 + sz * 0.18f, y0 + sz * 0.52f), X.p(x0 + sz * 0.42f, y0 + sz * 0.76f), c, X.d(th));
  DL->AddLine(X.p(x0 + sz * 0.42f, y0 + sz * 0.76f), X.p(x0 + sz * 0.84f, y0 + sz * 0.26f), c, X.d(th));
}

// A checkbox in the game's style: a chamfered crystal square, gold when on.
void checkbox(float x0, float y0, float sz, bool on, bool focus, float a) {
  ImVec2 p[8];
  const int n = chamfer(x0, y0, x0 + sz, y0 + sz, sz * 0.22f, 0, sz * 0.22f, 0, p);
  if (on) gradPoly(p, n, col(1.0f, 0.86f, 0.40f, a), col(0.80f, 0.52f, 0.10f, a));
  else gradPoly(p, n, col(0.04f, 0.10f, 0.28f, 0.85f * a), col(0.02f, 0.05f, 0.16f, 0.9f * a));
  DL->AddPolyline(p, n, focus ? C(kGold, a) : col(0.62f, 0.78f, 1.0f, 0.75f * a), ImDrawFlags_Closed,
                  std::max(1.0f, X.d(focus ? 1.8f : 1.2f)));
  if (on) checkMark(x0, y0, sz, col(0.18f, 0.06f, 0.02f, a), std::max(1.6f, sz * 0.13f));
}

// A badge pill; returns its width.
float badge(float x, float y, const std::string& label, ImU32 bg, ImU32 fg, ImU32 rim) {
  const std::string s = upperAscii(label);
  const float w = textW(F.badge, kSzBadge, s) + 12.0f, h = 16.0f;
  ImVec2 p[8];
  const int n = chamfer(x, y, x + w, y + h, 5, 0, 5, 0, p);
  DL->AddConvexPolyFilled(p, n, bg);
  DL->AddPolyline(p, n, rim, ImDrawFlags_Closed, std::max(1.0f, X.d(1.0f)));
  textPlain(F.badge, kSzBadge, x + 6.0f, y + (h - kSzBadge) * 0.5f - 0.5f, s, fg);
  return w;
}

// ═════════════════════════════════════════════════════════════════════════════════════════
// Button prompts, matched to the device the player used last
// ═════════════════════════════════════════════════════════════════════════════════════════

const char* glyphPng(PromptSet set, Ctl c) {
  static const char* ps4[cCount] = {"P4Gamepad/T_P4_Cross_Color.png", "P4Gamepad/T_P4_Circle_Color.png",
                                    "P4Gamepad/T_P4_Square_Color.png", "P4Gamepad/T_P4_Triangle_Color.png",
                                    "P4Gamepad/T_P4_L1.png", "P4Gamepad/T_P4_R1.png", "P4Gamepad/T_P4_L2.png",
                                    "P4Gamepad/T_P4_R2.png", "P4Gamepad/T_P4_Dpad.png", "P4Gamepad/T_P4_R.png",
                                    "P4Gamepad/T_P4_Options.png", "P4Gamepad/T_P4_Share.png"};
  static const char* ps5[cCount] = {"P5Gamepad/T_P5_Cross_Color.png", "P5Gamepad/T_P5_Circle_Color.png",
                                    "P5Gamepad/T_P5_Square_Color.png", "P5Gamepad/T_P5_Triangle_Color.png",
                                    "P5Gamepad/T_P5_L1.png", "P5Gamepad/T_P5_R1.png", "P5Gamepad/T_P5_L2.png",
                                    "P5Gamepad/T_P5_R2.png", "P5Gamepad/T_P5_Dpad.png", "P5Gamepad/T_P5_R.png",
                                    "P5Gamepad/T_P5_Options.png", "P5Gamepad/T_P5_Share.png"};
  // positional face buttons (docs/INPUT_GLYPHS.md): the 360's A is the Switch's B
  static const char* sw[cCount] = {"SGamepad/T_S_B.png", "SGamepad/T_S_A.png", "SGamepad/T_S_Y.png",
                                   "SGamepad/T_S_X.png", "SGamepad/T_S_LB.png", "SGamepad/T_S_RB.png",
                                   "SGamepad/T_S_LT.png", "SGamepad/T_S_RT.png", "SGamepad/T_S_Dpad.png",
                                   "SGamepad/T_S_R.png", "SGamepad/T_S_Plus.png", "SGamepad/T_S_Minus.png"};
  static const char* kb[cCount] = {"Keyboard_Mouse/White/T_Enter_Alt_Key_White.png",
                                   "Keyboard_Mouse/White/T_Esc_Key_White.png",
                                   "Keyboard_Mouse/White/T_Space_Key_White.png",
                                   "Keyboard_Mouse/White/T_R_Key_White.png",
                                   "Keyboard_Mouse/White/T_Q_Key_White.png",
                                   "Keyboard_Mouse/White/T_E_Key_White.png",
                                   "Keyboard_Mouse/White/T_PageUp_Key_White.png",
                                   "Keyboard_Mouse/White/T_PageDown_Key_White.png",
                                   "Keyboard_Mouse/White/T_Cursor_Key_White.png",
                                   "Keyboard_Mouse/White/T_Mouse_Scroll_Key_Dark_Key_White.png",
                                   "Keyboard_Mouse/White/T_Enter_Key_White.png",
                                   nullptr};   // cView: always a drawn key cap (kbLabel)
  switch (set) {
    case psPS4: return ps4[c];
    case psPS5: return ps5[c];
    case psSwitch: return sw[c];
    case psKeyboard: return kb[c];
    default: return nullptr;
  }
}

std::string glyphRoot() {
  static const std::string r = [] {
    sr::gfx::glyphs::init();
    std::string root = sr::gfx::glyphs::root();
    if (root.empty()) root = exeDir() + "/data/glyphs";
    return root + "/juliocacko_input_prompts/";
  }();
  return r;
}

// The Xbox 360 prompts, drawn: the game's own colours (green A, red B, blue X, yellow Y).
void xboxGlyph(Ctl c, float cx, float cy, float r) {
  auto face = [&](const char* l, Rgb fill) {
    DL->AddCircleFilled(X.p(cx, cy + 0.6f), X.d(r), col(0, 0, 0, 0.45f), 28);
    DL->AddCircleFilled(X.p(cx, cy), X.d(r), col(fill.r * 0.55f, fill.g * 0.55f, fill.b * 0.55f), 28);
    DL->AddCircleFilled(X.p(cx, cy - r * 0.06f), X.d(r * 0.86f), col(fill.r, fill.g, fill.b), 28);
    DL->AddCircle(X.p(cx, cy), X.d(r), col(1, 1, 1, 0.55f), 28, std::max(1.0f, X.d(0.9f)));
    const float w = textW(F.card, r * 1.15f, l);
    textPlain(F.card, r * 1.15f, cx - w * 0.5f - r * 0.05f, cy - r * 0.62f, l, col(1, 1, 1));
  };
  auto pill = [&](const char* l, float wMul) {
    const float w = r * wMul, h = r * 0.78f;
    DL->AddRectFilled(X.p(cx - w, cy - h), X.p(cx + w, cy + h), col(0.16f, 0.17f, 0.20f), X.d(h * 0.7f));
    DL->AddRect(X.p(cx - w, cy - h), X.p(cx + w, cy + h), col(0.85f, 0.88f, 0.95f, 0.8f), X.d(h * 0.7f), 0,
                std::max(1.0f, X.d(0.9f)));
    const float tw = textW(F.badge, r * 0.95f, l);
    textPlain(F.badge, r * 0.95f, cx - tw * 0.5f, cy - r * 0.52f, l, col(0.95f, 0.96f, 1.0f));
  };
  switch (c) {
    case cA: face("A", {0.30f, 0.72f, 0.16f}); break;
    case cB: face("B", {0.86f, 0.14f, 0.12f}); break;
    case cX: face("X", {0.16f, 0.44f, 0.92f}); break;
    case cY: face("Y", {0.96f, 0.74f, 0.08f}); break;
    case cLB: pill("LB", 1.3f); break;
    case cRB: pill("RB", 1.3f); break;
    case cLT: pill("LT", 1.3f); break;
    case cRT: pill("RT", 1.3f); break;
    case cStart: pill("START", 1.9f); break;
    case cView: pill("BACK", 1.9f); break;   // the 360 pad's BACK button (lane E E3)
    case cRS: {
      DL->AddCircleFilled(X.p(cx, cy), X.d(r), col(0.16f, 0.17f, 0.20f), 24);
      DL->AddCircle(X.p(cx, cy), X.d(r), col(0.85f, 0.88f, 0.95f, 0.8f), 24, std::max(1.0f, X.d(0.9f)));
      const float tw = textW(F.badge, r * 0.9f, "RS");
      textPlain(F.badge, r * 0.9f, cx - tw * 0.5f, cy - r * 0.48f, "RS", col(0.95f, 0.96f, 1.0f));
      break;
    }
    case cDpad:
    default: {
      const float a = r * 0.34f;
      DL->AddRectFilled(X.p(cx - a, cy - r), X.p(cx + a, cy + r), col(0.85f, 0.88f, 0.95f), X.d(a * 0.4f));
      DL->AddRectFilled(X.p(cx - r, cy - a), X.p(cx + r, cy + a), col(0.85f, 0.88f, 0.95f), X.d(a * 0.4f));
      break;
    }
  }
}

PromptSet gPromptSet = psXbox;
bool gLastWasPad = false;
bool gMoveLeftRight = false;
namespace {
const char* g_capOverride[cCount] = {};
}
void setKeyCapOverride(Ctl c, const char* label) {
  if (c >= 0 && c < cCount) g_capOverride[c] = label;
}

// Keyboard prompts are drawn key caps as wide as their label (iter-465 review: the square PNG
// caps let "Enter" spill out of its key and shrank "Page Up" past reading). The arrows and the
// mouse wheel stay pictures.
// Lane E E3: the cap names the key that does the act on the host screens (kernel/pc_controls.hpp hostKeyCap: the
// player's menu bindings, the Enter / Esc failsafes, Tab / Shift+Tab, PgUp / PgDn; the test keys for the harness), so
// a rebind shows at once. "-" = no key does it. Render thread only (the strings live in a per-control cache).
// MOVE (cDpad) is the arrows picture only while the arrow keys move (Astra E3 r1; all four on a screen that sets
// gMoveLeftRight, r2); otherwise its cap names the keys that do (hostMoveCap).
const char* kbLabel(Ctl c) {
  namespace PC = sr::kernel::pccontrols;
  static std::string cache[cCount];
  if (c >= 0 && c < cCount && g_capOverride[c]) return g_capOverride[c];   // a fixed-key mode's prompt
  if (c == cDpad) {
    std::string& s = cache[cDpad];
    s = PC::hostMoveCap(PC::hostCapKeymap(), PC::config().bindings, gMoveLeftRight);
    return s.empty() ? nullptr : s.c_str();
  }
  PC::HostAct a = PC::HostAct::None;
  switch (c) {
    case cA: case cStart: a = PC::HostAct::Accept; break;
    case cB: a = PC::HostAct::Back; break;
    case cX: a = PC::HostAct::X; break;
    case cY: a = PC::HostAct::Y; break;
    case cLB: a = PC::HostAct::LB; break;
    case cRB: a = PC::HostAct::RB; break;
    case cLT: a = PC::HostAct::LT; break;
    case cRT: a = PC::HostAct::RT; break;
    case cView: a = PC::HostAct::View; break;
    default: return nullptr;
  }
  std::string& s = cache[c];
  s = PC::hostKeyCap(a, PC::hostCapKeymap(), PC::config().bindings);
  if (s.empty()) s = "-";
  return s.c_str();
}
float keyCapW(const char* l, float h) {
  const float capH = h * 0.92f, fs = h * 0.64f;
  return std::max(capH, textW(F.body, fs, l) + h * 0.62f);
}
void keyCap(const char* l, float cx, float cy, float h) {
  const float capH = h * 0.92f, fs = h * 0.64f, w = keyCapW(l, h);
  const float x0 = cx - w * 0.5f, x1 = cx + w * 0.5f, y0 = cy - capH * 0.5f, y1 = cy + capH * 0.5f;
  const float r = capH * 0.2f, lip = std::max(1.6f, capH * 0.12f);
  DL->AddRectFilled(X.p(x0, y0 + 1.0f), X.p(x1, y1 + 1.0f), col(0, 0, 0, 0.5f), X.d(r));          // shadow
  DL->AddRectFilled(X.p(x0, y0), X.p(x1, y1), col(0.62f, 0.67f, 0.77f), X.d(r));                    // the cap's side
  DL->AddRectFilled(X.p(x0 + 0.8f, y0 + 0.6f), X.p(x1 - 0.8f, y1 - lip), col(0.97f, 0.98f, 1.0f), X.d(r * 0.85f));
  const float tw = textW(F.body, fs, l);
  textPlain(F.body, fs, cx - tw * 0.5f, y0 + (capH - lip - fs) * 0.5f + fs * 0.03f, l, col(0.07f, 0.09f, 0.15f));
}

// The arrow keys (the D-pad on a keyboard): a key cap with an up and a down arrow.
void keyCapArrows(float cx, float cy, float h) {
  const float capH = h * 0.92f, w = capH;
  const float x0 = cx - w * 0.5f, x1 = cx + w * 0.5f, y0 = cy - capH * 0.5f, y1 = cy + capH * 0.5f;
  const float r = capH * 0.2f, lip = std::max(1.6f, capH * 0.12f);
  DL->AddRectFilled(X.p(x0, y0 + 1.0f), X.p(x1, y1 + 1.0f), col(0, 0, 0, 0.5f), X.d(r));
  DL->AddRectFilled(X.p(x0, y0), X.p(x1, y1), col(0.62f, 0.67f, 0.77f), X.d(r));
  DL->AddRectFilled(X.p(x0 + 0.8f, y0 + 0.6f), X.p(x1 - 0.8f, y1 - lip), col(0.97f, 0.98f, 1.0f), X.d(r * 0.85f));
  const float my = (y0 + y1 - lip) * 0.5f, a = capH * 0.17f;
  const ImU32 ink = col(0.07f, 0.09f, 0.15f);
  DL->AddTriangleFilled(X.p(cx, my - a * 2.1f), X.p(cx + a, my - a * 0.5f), X.p(cx - a, my - a * 0.5f), ink);
  DL->AddTriangleFilled(X.p(cx - a, my + a * 0.5f), X.p(cx + a, my + a * 0.5f), X.p(cx, my + a * 2.1f), ink);
}

// The width a prompt glyph takes at height `h`.
float glyphW(Ctl c, float h) {
  if (gPromptSet == psXbox)
    return h * 0.5f * ((c == cLB || c == cRB || c == cLT || c == cRT) ? 2.6f
                       : ((c == cStart || c == cView) ? 3.8f : 2.0f));
  if (gPromptSet == psKeyboard) {
    if (const char* l = kbLabel(c)) return keyCapW(l, h);
    if (c == cDpad) return h * 0.92f;
  }
  return h;
}

float glyph(Ctl c, float cx, float cy, float h);
// Draws a prompt glyph with its left edge at x; returns its width.
float glyphAt(Ctl c, float x, float cy, float h) {
  const float w = glyphW(c, h);
  glyph(c, x + w * 0.5f, cy, h);
  return w;
}

// Draws a prompt glyph centred at (cx, cy), `h` tall; returns its width.
float glyph(Ctl c, float cx, float cy, float h) {
  if (gPromptSet == psXbox) {
    xboxGlyph(c, cx, cy, h * 0.5f);
    return glyphW(c, h);
  }
  if (gPromptSet == psKeyboard) {
    if (const char* l = kbLabel(c)) {
      keyCap(l, cx, cy, h);
      return keyCapW(l, h);
    }
    if (c == cDpad) {   // the arrow keys (Julio Cacko's "Cursor" key is a mouse pointer)
      keyCapArrows(cx, cy, h);
      return h * 0.92f;
    }
  }
  const char* rel = glyphPng(gPromptSet, c);
  const ImTextureID t = rel ? image(glyphRoot() + rel, 128, 128) : ImTextureID(0);
  // the Julio Cacko art fills ~3/4 of its 128 px canvas
  const float s = h * 1.34f;
  if (t) DL->AddImage(t, X.p(cx - s * 0.5f, cy - s * 0.5f), X.p(cx + s * 0.5f, cy + s * 0.5f), ImVec2(0, 0),
                      ImVec2(1, 1), col(1, 1, 1));
  else DL->AddCircle(X.p(cx, cy), X.d(h * 0.45f), col(1, 1, 1, 0.6f), 20, X.d(1.0f));
  return h;
}


// ═════════════════════════════════════════════════════════════════════════════════════════
// ImGui context + backend
// ═════════════════════════════════════════════════════════════════════════════════════════
// The heap, the fence and the ImGui context (once).
bool ensureContext(ID3D12Device* dev, ID3D12CommandQueue* queue) {
  if (G.failed) return false;
  G.dev = dev;
  G.queue = queue;
  if (!G.heap) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kHeapSlots;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(G.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&G.heap))) ||
        FAILED(G.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&G.fence)))) {
      G.failed = true;
      SR_LOG_BOOT(sr::LogSeverity::Error, "[OVERLAY] descriptor heap / fence creation failed - overlay not drawn");
      return false;
    }
    G.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    G.inc = G.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (int i = kHeapSlots - 1; i >= 0; --i) G.freeSlots.push_back(i);
  }
  if (!G.ctx) {
    IMGUI_CHECKVERSION();
    G.ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  }
  ImGui::SetCurrentContext(G.ctx);
  return true;
}

// The DX12 renderer backend. Its Init allocates the font atlas SRV, so a font rebuild or a
// swapchain format change is a full Shutdown + Init (never InvalidateDeviceObjects alone: that
// frees the font's descriptor slot while the backend keeps writing to it).
void shutdownBackend() {
  if (!G.imguiReady) return;
  flushGpu();
  ImGui_ImplDX12_Shutdown();
  G.imguiReady = false;
}

bool ensureBackend(DXGI_FORMAT rtvFormat) {
  if (G.failed) return false;
  if (G.imguiReady && G.fmt == rtvFormat) return true;
  shutdownBackend();
  ImGui_ImplDX12_InitInfo ii;
  ii.Device = G.dev;
  ii.CommandQueue = G.queue;
  ii.NumFramesInFlight = kFramesInFlight;
  ii.RTVFormat = rtvFormat;
  ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
  ii.SrvDescriptorHeap = G.heap.Get();
  ii.SrvDescriptorAllocFn = imguiSrvAlloc;
  ii.SrvDescriptorFreeFn = imguiSrvFree;
  if (!ImGui_ImplDX12_Init(&ii)) {
    G.failed = true;
    SR_LOG_BOOT(sr::LogSeverity::Error, "[OVERLAY] ImGui DX12 backend init failed - overlay not drawn");
    return false;
  }
  G.fmt = rtvFormat;
  G.imguiReady = true;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[OVERLAY] Dear ImGui %s ready (rtv fmt %u)", IMGUI_VERSION, unsigned(rtvFormat));
  return true;
}


}  // namespace overlay
}  // namespace gfx
}  // namespace sr

#endif  // _WIN32
