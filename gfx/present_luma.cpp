// Presented-luminance probe (SR_PRESENT_LUMA=1, default OFF). See present_luma.hpp.
// Observe-only: it copies rows of the back buffer the frame already drew and changes nothing the
// game or the present path sees. It never waits on the GPU: a ring slot whose copy is still in
// flight makes this present a counted skip.
#ifdef _WIN32

#include "present_luma.hpp"

#include "../app/log.hpp"
#include "../kernel/highfps.hpp"      // X2: the SR_DIAG_SIM_FREEZE hold a present was queued in
#include "../kernel/perf_stats.hpp"

#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sr::gfx::presentluma {
namespace {

using Microsoft::WRL::ComPtr;

constexpr int kRing = 8;   // copies in flight; a present waits on the frame-latency queue long before this fills

int envInt(const char* name, int def, int lo, int hi) {
  const char* e = std::getenv(name);
  if (!e || !*e) return def;
  const int v = std::atoi(e);
  return v < lo ? lo : v > hi ? hi : v;
}

struct Slot {
  ComPtr<ID3D12CommandAllocator> alloc;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Resource> buf;
  uint64_t bytes = 0;
  uint64_t fence = 0;   // the fence value its copy signals; 0 = nothing to read
  uint64_t pn = 0, tUs = 0;
  uint32_t hold = 0;     // the [SIM_FREEZE] #id while the sim was held at queue time, else 0
  float heldMs = 0;      // how long that hold had lasted
  int64_t stamp = 0;     // the render loop's time (render mgr +0x50, ns) at queue time: frozen content inside a hold
  uint32_t w = 0, h = 0, rows = 0, pitch = 0;
  bool bgra = true;
};

struct Rec {
  uint64_t pn, tUs;
  double luma;
  uint32_t w, h;
  uint64_t skipped;
  uint32_t hold;
  float heldMs;
  int64_t stamp;
};

struct State {
  std::mutex mu;                 // present sites may run on more than one thread
  ID3D12Device* device = nullptr;
  ComPtr<ID3D12Fence> fence;
  uint64_t fenceValue = 0;
  Slot slots[kRing];
  uint64_t pn = 0, skipped = 0;
  int rows = 64, step = 2;
  bool failed = false;
  std::mutex wmu;                // the writer's queue
  std::vector<Rec> pending;
};

State& state() {
  static State s;
  return s;
}

void writerMain(std::string path, int rows, int step) {
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[PRESENT_LUMA] cannot open %s - probe output lost", path.c_str());
    return;
  }
  std::fprintf(f, "# SR_PRESENT_LUMA rows=%d step=%d ring=%d luma=(299R+587G+114B)/1000 of 8-bit codes\n",
               rows, step, kRing);
  std::fprintf(f, "pn,t_us,luma,w,h,skipped,hold,held_ms,stamp\n");
  std::fflush(f);
  std::vector<Rec> batch;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    {
      std::lock_guard<std::mutex> lk(state().wmu);
      batch.swap(state().pending);
    }
    for (const Rec& r : batch)
      std::fprintf(f, "%llu,%llu,%.4f,%u,%u,%llu,%u,%.1f,%lld\n", (unsigned long long)r.pn, (unsigned long long)r.tUs,
                   r.luma, r.w, r.h, (unsigned long long)r.skipped, r.hold, double(r.heldMs), (long long)r.stamp);
    if (!batch.empty()) std::fflush(f);
    batch.clear();
  }
}

void fail(State& s, const char* why, long hr) {
  s.failed = true;
  SR_LOG_BOOT(sr::LogSeverity::Warn, "[PRESENT_LUMA] off from present %llu: %s (hr=0x%08lX)",
              (unsigned long long)s.pn, why, (unsigned long)hr);
}

// Average a completed slot's rows; queue the record for the writer.
void harvest(State& s, Slot& sl) {
  void* mp = nullptr;
  const D3D12_RANGE rr{0, size_t(sl.rows) * sl.pitch};
  if (SUCCEEDED(sl.buf->Map(0, &rr, &mp)) && mp) {
    const int ri = sl.bgra ? 2 : 0, bi = sl.bgra ? 0 : 2;
    uint64_t sum = 0, n = 0;
    for (uint32_t r = 0; r < sl.rows; ++r) {
      const uint8_t* row = static_cast<const uint8_t*>(mp) + size_t(r) * sl.pitch;
      for (uint32_t x = 0; x < sl.w; x += uint32_t(s.step)) {
        const uint8_t* px = row + size_t(x) * 4;
        sum += 299u * px[ri] + 587u * px[1] + 114u * px[bi];
        ++n;
      }
    }
    const D3D12_RANGE none{0, 0};
    sl.buf->Unmap(0, &none);
    if (n) {
      std::lock_guard<std::mutex> lk(s.wmu);
      s.pending.push_back(Rec{sl.pn, sl.tUs, double(sum) / (1000.0 * double(n)), sl.w, sl.h, s.skipped, sl.hold,
                              sl.heldMs, sl.stamp});
    }
  }
  sl.fence = 0;
}

void transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* rt, D3D12_RESOURCE_STATES a,
                D3D12_RESOURCE_STATES b) {
  D3D12_RESOURCE_BARRIER br{};
  br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  br.Transition.pResource = rt;
  br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  br.Transition.StateBefore = a;
  br.Transition.StateAfter = b;
  cl->ResourceBarrier(1, &br);
}

}  // namespace

bool enabled() noexcept {
  static const bool on = [] {
    const char* e = std::getenv("SR_PRESENT_LUMA");
    return e && e[0] == '1';
  }();
  return on;
}

void onPresent(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* rt) noexcept {
  if (!enabled() || !device || !queue || !rt) return;
  State& s = state();
  std::lock_guard<std::mutex> lk(s.mu);
  if (s.failed) return;
  if (!s.fence) {
    const HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence));
    if (FAILED(hr)) return fail(s, "CreateFence", hr);
    s.device = device;
    s.rows = envInt("SR_PRESENT_LUMA_ROWS", 64, 1, 256);
    s.step = envInt("SR_PRESENT_LUMA_STEP", 2, 1, 16);
    const char* d = std::getenv("SR_PRESENT_LUMA_DIR");
    if (!d || !*d) d = std::getenv("SR_PERF_DIR");
    if (!d || !*d) d = "archive";
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    const std::string path = std::string(d) + "/present_luma.csv";
    std::thread(writerMain, path, s.rows, s.step).detach();
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PRESENT_LUMA] on: rows=%d step=%d ring=%d csv=%s", s.rows, s.step, kRing,
                path.c_str());
  }
  if (device != s.device) return fail(s, "the device changed", 0);

  // 1) every completed copy, oldest first (the CSV stays in present order)
  const uint64_t done = s.fence->GetCompletedValue();
  Slot* ready[kRing];
  int nReady = 0;
  for (Slot& sl : s.slots)
    if (sl.fence && sl.fence <= done) ready[nReady++] = &sl;
  std::sort(ready, ready + nReady, [](const Slot* a, const Slot* b) { return a->pn < b->pn; });
  for (int i = 0; i < nReady; ++i) harvest(s, *ready[i]);

  // 2) this present
  const uint64_t pn = ++s.pn;
  Slot& sl = s.slots[pn % kRing];
  if (sl.fence) {   // its copy is still on the GPU: never wait
    ++s.skipped;
    return;
  }
  const D3D12_RESOURCE_DESC rd = rt->GetDesc();
  bool bgra;
  switch (rd.Format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM:
      bgra = true; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      bgra = false; break;
    default: {
      static bool s_said = false;
      if (!s_said) {
        s_said = true;
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[PRESENT_LUMA] back-buffer format %d is not 8-bit RGBA: presents skipped",
                    int(rd.Format));
      }
      ++s.skipped;
      return;
    }
  }
  if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd.Width == 0 || rd.Height == 0 || rd.Width > 16384) {
    ++s.skipped;
    return;
  }
  const uint32_t w = uint32_t(rd.Width), h = rd.Height;
  const uint32_t rows = std::min<uint32_t>(uint32_t(s.rows), h);
  // placed footprints need 512-byte offsets (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT); rows sit one pitch apart
  const uint32_t pitch = (w * 4u + 511u) & ~511u;
  const uint64_t need = uint64_t(rows) * pitch;
  HRESULT hr;
  if (!sl.buf || sl.bytes < need) {
    sl.buf.Reset();
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = need;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&sl.buf));
    if (FAILED(hr)) return fail(s, "readback buffer", hr);
    sl.bytes = need;
  }
  if (!sl.alloc) {
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&sl.alloc));
    if (FAILED(hr)) return fail(s, "command allocator", hr);
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, sl.alloc.Get(), nullptr, IID_PPV_ARGS(&sl.list));
    if (FAILED(hr)) return fail(s, "command list", hr);
    sl.list->Close();
  }
  // The slot's previous copy has completed (sl.fence == 0 after its harvest), so its allocator is free.
  if (FAILED(hr = sl.alloc->Reset())) return fail(s, "allocator reset", hr);
  if (FAILED(hr = sl.list->Reset(sl.alloc.Get(), nullptr))) return fail(s, "list reset", hr);
  transition(sl.list.Get(), rt, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
  src.pResource = rt;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  dst.pResource = sl.buf.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint.Format = rd.Format;
  dst.PlacedFootprint.Footprint.Width = w;
  dst.PlacedFootprint.Footprint.Height = 1;
  dst.PlacedFootprint.Footprint.Depth = 1;
  dst.PlacedFootprint.Footprint.RowPitch = pitch;
  for (uint32_t r = 0; r < rows; ++r) {
    const uint32_t y = uint32_t((uint64_t(2 * r + 1) * h) / (2ull * rows));   // evenly spaced row centres
    dst.PlacedFootprint.Offset = uint64_t(r) * pitch;
    const D3D12_BOX box{0, y, 0, w, y + 1, 1};
    sl.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
  }
  transition(sl.list.Get(), rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
  if (FAILED(hr = sl.list->Close())) return fail(s, "list close", hr);
  ID3D12CommandList* lists[] = {sl.list.Get()};
  queue->ExecuteCommandLists(1, lists);
  if (FAILED(hr = queue->Signal(s.fence.Get(), ++s.fenceValue))) return fail(s, "signal", hr);
  sl.fence = s.fenceValue;
  sl.pn = pn;
  sl.tUs = sr::perf::nowUs();
  const double heldMs = sr::kernel::highfps::freezeHeldMs();   // lock-free; 0 = the sim is not held
  sl.heldMs = float(heldMs);
  sl.hold = heldMs > 0 ? sr::kernel::highfps::freezeClock().holdId : 0u;
  sl.stamp = sr::kernel::highfps::renderLoopTimeNs();
  sl.w = w;
  sl.h = h;
  sl.rows = rows;
  sl.pitch = pitch;
  sl.bgra = bgra;
}

}  // namespace sr::gfx::presentluma

#endif
