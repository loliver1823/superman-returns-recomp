// iter-465 window: the game's own title icon on the window (see window_icon.hpp).
// The XDBF layout (header, 18-byte entry table, free table, data) and the title-icon key
// (section 2 = image, id 0x8000) follow Xenia: src/xenia/kernel/xam/xdbf/xdbf_io.{h,cc} and
// spa_info.{h,cc} (BSD-3-Clause, Copyright Ben Vanik and the Xenia project; CREDITS.md).
#include "window_icon.hpp"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../kernel/xex_loader.hpp"
#include "log.hpp"

#ifdef _WIN32
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>
#endif

namespace sr {
namespace {

uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// The bytes of XDBF entry (section, id), or {nullptr, 0}.
bool xdbfEntry(const std::vector<uint8_t>& db, uint16_t section, uint64_t id, const uint8_t** out,
               uint32_t* outSize) {
  constexpr uint32_t kHeader = 24, kEntry = 18, kFree = 8;
  if (db.size() < kHeader || std::memcmp(db.data(), "XDBF", 4) != 0) return false;
  const uint8_t* d = db.data();
  const uint64_t entryCount = be32(d + 8), entryUsed = be32(d + 12), freeCount = be32(d + 16);
  const uint64_t dataBase = kHeader + entryCount * kEntry + freeCount * kFree;
  if (entryUsed > entryCount || dataBase > db.size()) return false;
  for (uint64_t i = 0; i < entryUsed; i++) {
    const uint8_t* e = d + kHeader + i * kEntry;
    const uint16_t sec = uint16_t((e[0] << 8) | e[1]);
    const uint64_t eid = (uint64_t(be32(e + 2)) << 32) | be32(e + 6);
    const uint64_t off = be32(e + 10), sz = be32(e + 14);
    if (sec != section || eid != id) continue;
    if (dataBase + off + sz > db.size() || sz == 0) return false;
    *out = d + dataBase + off;
    *outSize = uint32_t(sz);
    return true;
  }
  return false;
}

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

// PNG bytes -> BGRA pixels at size (w x h); w = h = 0 keeps the image's own size.
bool decodePngBgra(IWICImagingFactory* f, const uint8_t* png, uint32_t size, uint32_t want,
                   std::vector<uint8_t>* bgra, uint32_t* w, uint32_t* h) {
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapDecoder> dec;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(f->CreateStream(&stream)) ||
      FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(png), size)) ||
      FAILED(f->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &dec)) ||
      FAILED(dec->GetFrame(0, &frame)) || FAILED(f->CreateFormatConverter(&conv)) ||
      FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                              nullptr, 0.0, WICBitmapPaletteTypeCustom)))
    return false;
  UINT pw = 0, ph = 0;
  if (FAILED(conv->GetSize(&pw, &ph)) || !pw || !ph || pw > 1024 || ph > 1024) return false;
  IWICBitmapSource* src = conv.Get();
  ComPtr<IWICBitmapScaler> scaler;
  if (want && (want != pw || want != ph)) {
    if (FAILED(f->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(conv.Get(), want, want, WICBitmapInterpolationModeHighQualityCubic)))
      return false;
    src = scaler.Get();
    pw = ph = want;
  }
  bgra->resize(size_t(pw) * ph * 4);
  if (FAILED(src->CopyPixels(nullptr, pw * 4, UINT(bgra->size()), bgra->data()))) return false;
  *w = pw;
  *h = ph;
  return true;
}

HICON makeIcon(const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h) {
  BITMAPV5HEADER bi{};
  bi.bV5Size = sizeof(bi);
  bi.bV5Width = LONG(w);
  bi.bV5Height = -LONG(h);  // top-down
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask = 0x00FF0000;
  bi.bV5GreenMask = 0x0000FF00;
  bi.bV5BlueMask = 0x000000FF;
  bi.bV5AlphaMask = 0xFF000000;
  void* bits = nullptr;
  HDC dc = GetDC(nullptr);
  HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, dc);
  if (!color || !bits) return nullptr;
  std::memcpy(bits, bgra.data(), bgra.size());
  HBITMAP mask = CreateBitmap(int(w), int(h), 1, 1, nullptr);
  ICONINFO ii{};
  ii.fIcon = TRUE;
  ii.hbmMask = mask;
  ii.hbmColor = color;
  HICON icon = CreateIconIndirect(&ii);
  DeleteObject(color);
  DeleteObject(mask);
  return icon;
}
#endif

// The title icon from the XDBF database onto the window. True = applied.
bool applyTitleIcon(SDL_Window* win, const std::vector<uint8_t>& db) {
  const uint8_t* png = nullptr;
  uint32_t pngSize = 0;
  if (!xdbfEntry(db, /*SpaSection::kImage*/ 2, /*kXdbfIdTitle*/ 0x8000, &png, &pngSize)) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[WINDOW] the XEX's XDBF has no title icon (image 0x8000); keeping the exe icon");
    return false;
  }
#ifdef _WIN32
  const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  bool ok = false;
  {
    ComPtr<IWICImagingFactory> f;
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        decodePngBgra(f.Get(), png, pngSize, 0, &px, &w, &h)) {
      // SDL keeps its own copy (it also sets the class icon it falls back to).
      if (SDL_Surface* s = SDL_CreateSurfaceFrom(int(w), int(h), SDL_PIXELFORMAT_BGRA32, px.data(), int(w * 4))) {
        SDL_SetWindowIcon(win, s);
        SDL_DestroySurface(s);
      }
      // Crisp per-size icons for the title bar (small) and the taskbar / Alt+Tab (big).
      HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(win),
                                                           SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
      const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 96;
      const int sizes[2] = {GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CXSMICON, dpi)};
      const WPARAM kinds[2] = {ICON_BIG, ICON_SMALL};
      for (int i = 0; i < 2 && hwnd; i++) {
        std::vector<uint8_t> sp;
        uint32_t sw = 0, sh = 0;
        if (!decodePngBgra(f.Get(), png, pngSize, uint32_t(sizes[i]), &sp, &sw, &sh)) continue;
        // The icons live for the process (a window keeps using the handle it was given).
        if (HICON ic = makeIcon(sp, sw, sh)) SendMessageW(hwnd, WM_SETICON, kinds[i], LPARAM(ic));
      }
      ok = true;
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[WINDOW] title icon from the game's XDBF (PNG %ux%u, %u bytes) set on the window "
                  "(title bar %dpx, taskbar %dpx)",
                  w, h, pngSize, sizes[1], sizes[0]);
    }
  }
  if (SUCCEEDED(co)) CoUninitialize();
  if (!ok)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[WINDOW] the title icon PNG did not decode; keeping the exe icon");
  return ok;
#else
  (void)win;
  return false;
#endif
}

}  // namespace

void windowIconTick(void* sdlWindow) {
  static bool s_done = false;
  static std::chrono::steady_clock::time_point s_first{};
  if (s_done || !sdlWindow) return;
  const auto now = std::chrono::steady_clock::now();
  if (s_first == std::chrono::steady_clock::time_point{}) s_first = now;
  std::vector<uint8_t> db;
  if (sr::kernel::xexTitleDatabase(db)) {
    s_done = true;
    applyTitleIcon(static_cast<SDL_Window*>(sdlWindow), db);
  } else if (now - s_first > std::chrono::seconds(120)) {
    s_done = true;  // no image loaded (replay modes) or no XDBF: the exe icon stays
  }
}

}  // namespace sr
