// step 6 I10p (Fable ruling 40; the coordinator's variant): the write-only upload twin of the CB thirds
// (gfx/xlat_cb_gpu.hpp) on WARP, and M1 - a translated VS / PS upload writes ONLY the 256-byte blocks its shader reads
// (an INVARIANT pinned here: every block-use mask). No guest, PM4 or EDRAM inputs; run serially through
// tools/game_slot.py like the other WARP difftests.
#include "gfx/xlat_cb_gpu.hpp"
#include <dxgi1_4.h>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace sr::gfx;
// The test's only route to the resource (a GPU-side copy into a readback buffer; the upload memory is never mapped
// for reading by the test either).
struct sr::gfx::XlatCBGpuTestAccess {
  static ID3D12Resource* resource(XlatCBGpu& g) { return g.m_resource.Get(); }
};
namespace {
unsigned passed = 0, failed = 0;
void check(bool value, const char* name) {
  std::printf("[XLAT_CB_GPU_TEST] %s %s\n", value ? "PASS" : "FAIL", name);
  value ? ++passed : ++failed;
}
void require(HRESULT hr, const char* name) {
  if (FAILED(hr)) { std::fprintf(stderr, "SETUP FAILED %s hr=%08lX\n", name, (unsigned long)hr); std::exit(2); }
}

// The write-only API (the coordinator's condition 1): no member hands out the mapped pointer or reads the resource.
template <class T> concept HasData = requires(T& t) { t.data(); };
template <class T> concept HasMapped = requires(T& t) { t.mapped(); };
template <class T> concept HasRead = requires(T& t, void* p) { t.read(uint64_t(0), p, size_t(0)); };
template <class T> concept HasIndex = requires(T& t) { t[0]; };
template <class T> concept HasSpan = requires(T& t) { t.span(); };
static_assert(!HasData<XlatCBGpu> && !HasMapped<XlatCBGpu> && !HasRead<XlatCBGpu> && !HasIndex<XlatCBGpu> &&
                  !HasSpan<XlatCBGpu>,
              "XlatCBGpu must stay write-only: no accessor of the mapped upload memory");
static_assert(std::is_same_v<decltype(std::declval<const XlatCBGpu&>().bytes()), uint64_t>, "bytes() is a size");
static_assert(std::is_same_v<decltype(std::declval<const XlatCBGpu&>().address(0)), D3D12_GPU_VIRTUAL_ADDRESS>,
              "address() is a GPU VA, never a CPU pointer");

// M1: every mask. A used-block upload writes exactly the blocks of `use` (each 256 bytes at 256 * b from source
// dword 64 * b) - or all 4096 bytes when the mask means all (bit 16 = relative addressing, 0 = unknown, 0xFFFF).
void m1AllMasks() {
  bool ok = true;
  uint64_t masksChecked = 0;
  for (uint32_t rel = 0; rel < 2; ++rel) {
    for (uint32_t m = 0; m <= 0xFFFFu; ++m) {
      const uint32_t use = m | (rel ? 0x10000u : 0u);
      std::vector<std::pair<uint64_t, uint64_t>> writes;   // (dst, bytes)
      bool srcOk = true;
      static uint32_t src[1024];
      const uint64_t dst = 0x40000ull;
      const uint32_t n = xlatUploadUsedBlocks(
          [&](uint64_t d, const void* p, size_t bytes) {
            writes.emplace_back(d, bytes);
            const uint64_t blockOff = d - dst;
            srcOk = srcOk && p == src + blockOff / 4u;   // the source block of the same index
          },
          dst, src, use);
      const bool all = rel || m == 0u || m == 0xFFFFu;
      if (all) {
        ok = ok && n == 4096u && writes.size() == 1 && writes[0].first == dst && writes[0].second == 4096u;
      } else {
        ok = ok && n == 256u * uint32_t(std::popcount(m)) && writes.size() == size_t(std::popcount(m));
        uint32_t k = 0;
        for (uint32_t b = 0; b < 16; ++b) {
          if (!(m & (1u << b))) continue;
          ok = ok && k < writes.size() && writes[k].first == dst + 256u * b && writes[k].second == 256u;
          ++k;
        }
      }
      ok = ok && srcOk;
      ++masksChecked;
    }
  }
  check(ok && masksChecked == 2u * 65536u, "m1-every-mask-writes-only-the-used-blocks");
  // The byte volume (Fable 40: today's 14.1 MB / frame, not the contiguous extent's 21.3): a two-block use at the
  // extent's end writes 512 bytes, not 4096.
  static uint32_t src[1024];
  check(xlatUploadUsedBlocks([](uint64_t, const void*, size_t) {}, 0, src, 0x8001u) == 512u,
        "m1-two-blocks-write-512-bytes-not-the-extent");
}

// The check hash covers exactly the used blocks: a byte outside them never changes it, one inside always does.
void hashUsedBlocks() {
  std::array<uint8_t, 4096> a{};
  for (size_t i = 0; i < a.size(); ++i) a[i] = uint8_t(i * 7u + 3u);
  const uint32_t use = 0x0021u;   // blocks 0 and 5
  const uint64_t h0 = xlatCbHash(a.data(), use, 4096);
  auto b = a;
  b[256 * 3 + 17] ^= 0x5Au;       // block 3: unused
  const bool unusedSame = xlatCbHash(b.data(), use, 4096) == h0;
  auto c = a;
  c[256 * 5 + 255] ^= 0x01u;      // block 5: used
  const bool usedDiffers = xlatCbHash(c.data(), use, 4096) != h0;
  auto d = a;
  d[4095] ^= 0x80u;               // any byte for a whole-range hash
  const bool wholeDiffers = xlatCbHash(d.data(), ~0u, 4096) != xlatCbHash(a.data(), ~0u, 4096);
  check(unusedSame && usedDiffers && wholeDiffers, "check-hash-covers-exactly-the-used-blocks");
}

// The twin on WARP: the GPU reads what write() put there (a copy to a readback buffer - the TEST reads the readback,
// never the upload resource), fill() poisons, bounds are refused, address() is base + offset, and the shadow mirrors.
void twinOnWarp() {
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter> warp;
  ComPtr<ID3D12Device> device;
  require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
  require(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
  require(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "WARP device");
  constexpr uint64_t kBytes = 3u * 65536u;
  XlatCBGpu twin;
  check(!twin && twin.address(0) == 0 && !twin.write(0, "x", 1), "empty-twin-refuses");
  require(twin.initialize(device.Get(), kBytes), "twin");
  check(bool(twin) && twin.bytes() == kBytes && twin.initialize(device.Get(), kBytes) == E_INVALIDARG,
        "twin-initialized-once");
  std::vector<uint8_t> shadow(kBytes, 0);
  std::mutex shadowMu;
  twin.setShadow(shadow.data(), &shadowMu);
  std::vector<uint8_t> want(kBytes, 0);
  // The twin's bytes are undefined until written: write all of it (a known baseline), then a block, then poison.
  std::vector<uint8_t> base(kBytes);
  for (size_t i = 0; i < base.size(); ++i) base[i] = uint8_t(i ^ (i >> 8));
  check(twin.write(0, base.data(), base.size()), "write-whole");
  want = base;
  std::array<uint8_t, 768> fetch{};
  for (size_t i = 0; i < fetch.size(); ++i) fetch[i] = uint8_t(0xC0u + i);
  check(twin.write(65536u + 256u, fetch.data(), fetch.size()), "write-fetch-block");
  std::memcpy(want.data() + 65536u + 256u, fetch.data(), fetch.size());
  check(twin.fill(131072u, 0xFF, 4096), "fill-poison");
  std::memset(want.data() + 131072u, 0xFF, 4096);
  check(!twin.write(kBytes - 8u, fetch.data(), 16) && !twin.write(kBytes + 1u, fetch.data(), 0) &&
            !twin.fill(kBytes - 1u, 0, 2),
        "out-of-bounds-refused");
  check(twin.address(65536u + 256u) == twin.address(0) + 65536u + 256u && twin.address(0) % 256u == 0,
        "address-is-base-plus-offset");
  check(shadow == want, "shadow-mirrors-every-write-and-fill");

  // The GPU's view: copy the upload buffer into a readback buffer and compare.
  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> alloc;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  require(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
  require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "allocator");
  require(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)),
          "list");
  require(list->Close(), "close");
  require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
  D3D12_HEAP_PROPERTIES rb{};
  rb.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = kBytes;
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> readback;
  require(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&readback)),
          "readback");
  uint64_t fenceValue = 0;
  // One GPU copy of [offset, offset + n) of the twin into the readback buffer, executed and waited for; the result is
  // the readback's bytes (the test never reads the upload memory).
  auto gpuRead = [&](uint64_t offset, uint64_t n) {
    require(alloc->Reset(), "allocator reset");
    require(list->Reset(alloc.Get(), nullptr), "list reset");
    list->CopyBufferRegion(readback.Get(), 0, XlatCBGpuTestAccess::resource(twin), offset, n);
    require(list->Close(), "close");
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    require(queue->Signal(fence.Get(), ++fenceValue), "signal");
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    require(fence->SetEventOnCompletion(fenceValue, event), "event");
    WaitForSingleObject(event, 10000);
    CloseHandle(event);
    if (fence->GetCompletedValue() < fenceValue) {
      std::fprintf(stderr, "SETUP FAILED gpu copy timeout\n");
      std::exit(2);
    }
    std::vector<uint8_t> out(static_cast<size_t>(n));
    void* mapped = nullptr;
    const D3D12_RANGE all{0, size_t(n)};
    require(readback->Map(0, &all, &mapped), "map readback");
    std::memcpy(out.data(), mapped, size_t(n));
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    return out;
  };
  check(gpuRead(0, kBytes) == want, "gpu-reads-exactly-the-written-bytes");

  // Astra I10p r2 P2: with the check mode's mutex installed, write() / fill() take it BEFORE either destination is
  // written. Deterministic: while the checker holds the mutex, a writer thread's write() / fill() must not have
  // reached the upload resource (the GPU still reads the old bytes) nor the shadow; after the release both hold it.
  auto shadowRange = [&](uint64_t off, uint64_t n) {
    std::lock_guard<std::mutex> lk(shadowMu);
    return std::vector<uint8_t>(shadow.begin() + ptrdiff_t(off), shadow.begin() + ptrdiff_t(off + n));
  };
  auto wantRange = [&](uint64_t off, uint64_t n) {
    return std::vector<uint8_t>(want.begin() + ptrdiff_t(off), want.begin() + ptrdiff_t(off + n));
  };
  constexpr uint64_t kHeldOff = 4096u, kHeldBytes = 4096u;
  std::vector<uint8_t> next(kHeldBytes);
  for (size_t i = 0; i < next.size(); ++i) next[i] = uint8_t(0x5Au ^ (i * 13u));
  for (const bool poison : {false, true}) {
    const auto before = wantRange(kHeldOff, kHeldBytes);
    std::unique_lock<std::mutex> held(shadowMu);
    bool done = false;
    std::thread writer([&] {
      done = poison ? twin.fill(kHeldOff, 0xFF, size_t(kHeldBytes))
                    : twin.write(kHeldOff, next.data(), next.size());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto during = gpuRead(kHeldOff, kHeldBytes);
    const bool shadowOld = std::memcmp(shadow.data() + kHeldOff, want.data() + kHeldOff, size_t(kHeldBytes)) == 0;
    held.unlock();
    writer.join();
    if (poison) std::memset(want.data() + kHeldOff, 0xFF, size_t(kHeldBytes));
    else std::memcpy(want.data() + kHeldOff, next.data(), next.size());
    const auto after = gpuRead(kHeldOff, kHeldBytes);
    check(during == before && before != wantRange(kHeldOff, kHeldBytes) && shadowOld && done &&
              after == wantRange(kHeldOff, kHeldBytes) &&
              shadowRange(kHeldOff, kHeldBytes) == after,
          poison ? "p2-fill-waits-for-the-shadow-mutex-before-the-upload-write"
                 : "p2-write-waits-for-the-shadow-mutex-before-the-upload-write");
  }
  // Concurrent write / poison (the mutant's render-slot write against the CP's poison of the same third): after both
  // threads finish, the GPU's bytes equal the shadow's - every byte has the same last writer in both.
  constexpr uint64_t kRaceOff = 65536u, kRaceBytes = 8192u;
  std::vector<std::vector<uint8_t>> slots(64, std::vector<uint8_t>(512));
  for (size_t k = 0; k < slots.size(); ++k)
    for (size_t i = 0; i < 512; ++i) slots[k][i] = uint8_t(k * 37u + i);
  bool raceOk = true;
  for (int round = 0; round < 8; ++round) {
    std::thread render([&] {
      for (int i = 0; i < 4000; ++i) {
        const size_t k = size_t(i) % slots.size();
        raceOk = twin.write(kRaceOff + 512u * (k % 16u), slots[k].data(), 512) && raceOk;
      }
    });
    std::thread cp([&] {
      for (int i = 0; i < 400; ++i) twin.fill(kRaceOff, 0xFF, size_t(kRaceBytes));
    });
    render.join();
    cp.join();
    raceOk = raceOk && gpuRead(kRaceOff, kRaceBytes) == shadowRange(kRaceOff, kRaceBytes);
  }
  check(raceOk, "p2-concurrent-write-and-poison-leave-twin-and-shadow-equal");
}
}  // namespace

int main() {
  m1AllMasks();
  hashUsedBlocks();
  twinOnWarp();
  std::printf("[XLAT_CB_GPU_TEST] passed=%u failed=%u\n", passed, failed);
  return failed ? 1 : 0;
}
