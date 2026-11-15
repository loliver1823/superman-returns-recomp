// Replay bounded, captured NativeImageStore API operations against the production
// implementation and a real WARP queue. The Python case driver owns JSON/provenance.
#include "gfx/native_image_store.hpp"
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace sr::gfx;
using Microsoft::WRL::ComPtr;

namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void hrcheck(HRESULT hr, const char* why) { require(SUCCEEDED(hr), why); }
template <class T> T unsignedNumber(std::istream& input) {
  std::string token;
  input >> token;
  require(!token.empty() && token.front() >= '0' && token.front() <= '9', "invalid unsigned integer");
  T value = 0;
  const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
  require(parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size(), "invalid unsigned integer");
  return value;
}
uint32_t expected(const std::string& text) {
  if (text == "S_OK") return uint32_t(S_OK);
  if (text == "S_FALSE") return uint32_t(S_FALSE);
  if (text == "E_INVALIDARG") return uint32_t(E_INVALIDARG);
  if (text == "E_OUTOFMEMORY") return uint32_t(E_OUTOFMEMORY);
  if (text == "E_ABORT") return uint32_t(E_ABORT);
  if (text == "WAIT_TIMEOUT") return uint32_t(HRESULT_FROM_WIN32(WAIT_TIMEOUT));
  throw std::runtime_error("unknown expected HRESULT");
}
bool identifier(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  for (char c : name) if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
                         !(c >= '0' && c <= '9') && c != '_') return false;
  return true;
}
struct Image { NativeImageLease lease; NativeImageHandle handle; };
struct Flight {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> commands;
  uint64_t fence = 0;
};
struct Session {
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter> warp;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> blocker;
  NativeImageStore store;
  std::map<std::string, Image> images;
  std::map<std::string, Flight> flights;
  bool blocked = false, initialized = false;
  uint64_t createdPixels = 0;
  Session() {
    hrcheck(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory");
    hrcheck(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
    hrcheck(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12 device");
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hrcheck(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "queue");
  }
  ~Session() {
    // Even a deliberately failing assertion must unblock the real queue before destruction.
    if (blocked && blocker) blocker->Signal(1);
    if (initialized) store.shutdown(2000);
  }
  Image& image(const std::string& id) {
    auto found = images.find(id);
    require(found != images.end(), "unknown image id");
    return found->second;
  }
};
}

int wmain(int argc, wchar_t** argv) {
  if (argc != 2) { std::cerr << "usage: sr_native_image_replay operations.txt\n"; return 2; }
  try {
    std::ifstream input(std::filesystem::path(argv[1]), std::ios::binary);
    require(bool(input), "cannot read operations");
    std::string header;
    std::getline(input, header);
    if (!header.empty() && header.back() == '\r') header.pop_back();
    require(header == "SR_IMAGE_CASE 1", "invalid case header");
    Session s;
    size_t line = 0;
    bool closed = false;
    for (std::string text; std::getline(input, text);) {
      require(++line <= 4096 && text.size() <= 4096, "case limit exceeded");
      std::istringstream row(text);
      std::string op, id, other, want;
      row >> op;
      require(!op.empty() && !closed, "empty or trailing operation");
      HRESULT actual = S_OK;
      bool compareHr = true;
      if (op == "init") {
        const auto colors = unsignedNumber<uint32_t>(row), depths = unsignedNumber<uint32_t>(row);
        row >> want;
        require(!s.initialized && colors <= 256 && depths <= 256 && colors + depths > 0, "invalid initialization");
        actual = s.store.initialize(s.device.Get(), s.queue.Get(), colors, depths);
        s.initialized = SUCCEEDED(actual);
      } else {
        require(s.initialized, "case must initialize first");
        if (op == "create") {
          std::array<float, 4> color{};
          row >> id;
          const auto width = unsignedNumber<uint32_t>(row), height = unsignedNumber<uint32_t>(row);
          row >> color[0] >> color[1] >> color[2] >> color[3] >> want;
          require(identifier(id) && !s.images.count(id) && width <= 2048 && height <= 2048, "invalid create");
          s.createdPixels += uint64_t(width) * height;
          require(s.createdPixels <= 16 * 1024 * 1024, "case allocation budget exceeded");
          for (auto value : color) require(std::isfinite(value) && value >= 0 && value <= 1, "invalid clear color");
          NativeImageDesc desc;
          desc.width = width; desc.height = height;
          desc.resourceFormat = desc.clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
          desc.initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
          for (size_t i = 0; i < 4; ++i) desc.clear.Color[i] = color[i];
          Image image;
          actual = s.store.create(desc, image.lease);
          image.handle = image.lease.handle();
          s.images.emplace(id, std::move(image));
        } else if (op == "acquire") {
          row >> id >> other >> want;
          require(identifier(other) && !s.images.count(other), "duplicate acquisition output");
          Image image;
          actual = s.store.acquire(s.image(id).handle, image.lease);
          image.handle = image.lease.handle();
          s.images.emplace(other, std::move(image));
        } else if (op == "release") {
          row >> id >> want;
          actual = s.store.release(s.image(id).handle);
        } else if (op == "drop") {
          row >> id;
          s.image(id).lease = {};
          compareHr = false;
        } else if (op == "block") {
          require(!s.blocked && s.flights.empty(), "block must precede submissions");
          hrcheck(s.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.blocker)), "blocker");
          hrcheck(s.queue->Wait(s.blocker.Get(), 1), "queue wait");
          s.blocked = true;
          compareHr = false;
        } else if (op == "unblock") {
          require(s.blocked, "queue is not blocked");
          hrcheck(s.blocker->Signal(1), "unblock");
          s.blocked = false;
          compareHr = false;
        } else if (op == "submit") {
          row >> id >> other >> want;
          require(identifier(id) && !s.flights.count(id), "duplicate flight");
          auto& image = s.image(other);
          require(bool(image.lease), "submission needs a held lease");
          Flight flight;
          hrcheck(s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&flight.allocator)), "allocator");
          hrcheck(s.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, flight.allocator.Get(), nullptr,
                                            IID_PPV_ARGS(&flight.commands)), "commands");
          hrcheck(s.store.recordClear(flight.commands.Get(), image.lease), "recordClear");
          hrcheck(flight.commands->Close(), "close list");
          std::array<NativeImageLease, 1> leases{image.lease};
          actual = s.store.submit(flight.commands.Get(), flight.allocator.Get(), leases, flight.fence);
          s.flights.emplace(id, std::move(flight));
        } else if (op == "wait") {
          row >> id;
          const auto timeout = unsignedNumber<uint32_t>(row);
          row >> want;
          require(timeout <= 2000 && s.flights.count(id), "invalid wait");
          actual = s.store.wait(s.flights.at(id).fence, timeout);
        } else if (op == "collect") {
          row >> want;
          actual = s.store.collect();
        } else if (op == "stats") {
          const auto live = unsignedNumber<uint64_t>(row), pending = unsignedNumber<uint64_t>(row),
                     colors = unsignedNumber<uint64_t>(row), depths = unsignedNumber<uint64_t>(row);
          auto got = s.store.stats();
          require(got.live == live && got.pending == pending && got.colorDescriptors == colors && got.depthDescriptors == depths,
                  "stats differ from captured expectation");
          compareHr = false;
        } else if (op == "new_generation") {
          row >> id >> other;
          auto before = s.image(id).handle, after = s.image(other).handle;
          require(before.slot && after.slot == before.slot && after.generation > before.generation, "generation was not advanced");
          compareHr = false;
        } else if (op == "shutdown") {
          row >> want;
          require(!s.blocked, "case leaves queue blocked");
          actual = s.store.shutdown(2000);
          closed = SUCCEEDED(actual);
        } else throw std::runtime_error("unknown operation");
      }
      require(!row.fail(), "missing or invalid operation argument");
      std::string extra;
      require(!(row >> extra), "trailing operation arguments");
      if (compareHr) require(uint32_t(actual) == expected(want), "HRESULT differs from captured expectation");
      const auto stats = s.store.stats();
      std::cout << "{\"step\":" << line << ",\"operation\":\"" << op << "\",\"hr\":" << uint32_t(actual)
                << ",\"live\":" << stats.live << ",\"pending\":" << stats.pending
                << ",\"colors\":" << stats.colorDescriptors << ",\"depths\":" << stats.depthDescriptors << "}\n";
    }
    require(!input.bad() && closed && line >= 2, "incomplete case; successful shutdown required");
    std::cout << "{\"complete\":true,\"steps\":" << line << "}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "native image replay: " << error.what() << '\n';
    return 2;
  }
}
