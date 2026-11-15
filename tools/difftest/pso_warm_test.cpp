// Step 4 2k (lane D, Astra 10:2x #4 / #5): the pipeline warm-up's lifecycle (gfx/pso_warm.cpp) on a WARP device -
// the exit never waits unbounded, and attach / shutdown never interleave.
//   1. a warm create stuck in the driver: psoShutdown(300) returns false at its deadline (the worker left running with
//      everything it uses); once the create returns, a second psoShutdown joins and returns true
//   2. a shutdown during an attach waits for the WHOLE attach (no half-made state), then stops and joins its workers
//   3. a shutdown whose deadline ends while an attach still runs returns false; a later one returns true
//   4. an attach after a shutdown starts nothing
//   5-7. the warm list (user + shipped), the recording generation, oversized / corrupt / two-writer manifests
//   8. fix12: the stale cleanup (exact names, 7 days, under each file's manifest lock, its lock file removed)
//   9. fix12: the exit write waits for a manifest read still running (within the shutdown deadline)
//  10. a reset after a shutdown that returned false stops and joins the worker it left running, so a re-attach starts
//      clean (before the fix the reset left the std::thread joinable and the re-attach's thread assignment called
//      std::terminate - the intermittent 0xC0000409 of 10-24 00:34, when case 2's shutdown hit its deadline)
#include "gfx/native_pipeline.hpp"
#include "gfx/pso_manifest.hpp"
#include "gfx/pso_warm.hpp"

#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace sr::gfx;

namespace {
unsigned passed = 0, failed = 0;
void check(bool ok, const char* name) {
  std::printf("[PSO_WARM_TEST] %s %s\n", ok ? "PASS" : "FAIL", name);
  std::fflush(stdout);   // a fail-fast (0xC0000409) does not flush a redirected stdout: keep every line
  ok ? ++passed : ++failed;
}
void require(HRESULT hr, const char* name) {
  if (FAILED(hr)) {
    std::fprintf(stderr, "SETUP FAILED %s hr=%08lX\n", name, (unsigned long)hr);
    std::exit(2);
  }
}
NativeShaderCode compile(const char* source, const char* entry, const char* profile) {
  ComPtr<ID3DBlob> blob, errors;
  require(D3DCompile(source, std::strlen(source), "pso_warm_fixture", nullptr, nullptr, entry, profile,
                     D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors), entry);
  NativeShaderCode code;
  require(NativeShaderCode::copy({(const uint8_t*)blob->GetBufferPointer(), blob->GetBufferSize()}, code), "own");
  return code;
}
double msSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
void waitFor(const std::atomic<bool>& flag) {
  for (int i = 0; i < 5000 && !flag; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

std::atomic<bool> g_gateIn{false}, g_gateOpen{true};
std::atomic<bool> g_hookIn{false};
std::atomic<int> g_hookMs{0};
}  // namespace

int main() {
  char dir[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, dir);
  const std::string manifest = std::string(dir) + "sr_pso_warm_test_" + std::to_string(GetCurrentProcessId()) + ".bin";
  _putenv_s("SR_PSO_WARM", "1");
  _putenv_s("SR_PSO_MANIFEST", manifest.c_str());
  _putenv_s("SR_PSO_SHIPPED_MANIFEST", "0");   // 1-4: the user manifest only (the tree's real shipped one is not WARP's)

  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter> warp;
  ComPtr<ID3D12Device> device;
  require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
  require(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
  require(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
  D3D12_ROOT_PARAMETER parameter{};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameter.Constants = {0, 0, 4};
  parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rd{1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  ComPtr<ID3DBlob> rb, re;
  require(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rb, &re), "root serialize");
  ComPtr<ID3D12RootSignature> root;
  require(device->CreateRootSignature(0, rb->GetBufferPointer(), rb->GetBufferSize(), IID_PPV_ARGS(&root)), "root");
  static constexpr char shader[] = R"(
    cbuffer D : register(b0) { float4 rgba; };
    float4 VS(uint id : SV_VertexID) : SV_Position { return float4(float(id & 1), float(id >> 1), 0, 1); }
    float4 PS() : SV_Target0 { return rgba; })";
  NativePipelineDesc base;
  base.rootSignature = root;
  base.vertex = compile(shader, "VS", "vs_5_0");
  base.pixel = compile(shader, "PS", "ps_5_0");
  base.colorCount = 1;
  base.colorFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  base.rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
  base.rasterizer.CullMode = D3D12_CULL_MODE_NONE;
  base.rasterizer.DepthClipEnable = TRUE;
  for (auto& rt : base.blend.RenderTarget) {
    rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  }
  base.depthStencil.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  base.depthStencil.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  base.depthStencil.StencilReadMask = base.depthStencil.StencilWriteMask = 255;
  base.depthStencil.FrontFace = base.depthStencil.BackFace = {
      D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
  auto v = [&](INT bias) { auto d = base; d.rasterizer.DepthBias = bias; return d; };
  {
    const auto d1 = v(301), d2 = v(302), d3 = v(303);
    const auto image = psom::serialize({&d1, &d2, &d3});
    require(psom::writeFileAtomic(manifest, image) ? S_OK : E_FAIL, "manifest write");
  }
  // the warm create of DepthBias 301 blocks inside the cache's create while g_gateOpen is false
  NativePipelineCache::setCreateGateForTest(+[](const NativePipelineDesc& d) -> HRESULT {
    if (d.rasterizer.DepthBias != 301) return S_OK;
    g_gateIn = true;
    while (!g_gateOpen) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return S_OK;
  });
  psoSetAttachHookForTest(+[] {
    g_hookIn = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(g_hookMs.load()));
  });

  // 1. a stuck warm create bounds the shutdown; a later shutdown joins once it returns
  {
    NativePipelineCache cache;
    require(cache.initialize(device.Get(), 64), "cache 1");
    g_gateOpen = false; g_gateIn = false; g_hookMs = 0;
    psoAttachTo(&cache, root.Get());
    waitFor(g_gateIn);
    const auto t0 = std::chrono::steady_clock::now();
    const bool first = psoShutdown(300);
    const double ms = msSince(t0);
    check(g_gateIn && !first && ms >= 250.0 && ms < 2000.0,
          "1_a_warm_create_stuck_in_the_driver_bounds_the_shutdown_to_its_deadline");
    check(psoWorkersRunningForTest(), "1_the_stuck_worker_is_left_running_not_detached_or_torn_down");
    g_gateOpen = true;
    const bool second = psoShutdown(3000);
    check(second && !psoWorkersRunningForTest(), "1_once_the_create_returns_a_later_shutdown_joins");
    psoResetForTest();
  }
  // 2. a shutdown during an attach waits for the whole attach, then stops and joins its workers
  {
    NativePipelineCache cache;
    require(cache.initialize(device.Get(), 64), "cache 2");
    g_hookIn = false; g_hookMs = 400;
    std::thread attacher([&] { psoAttachTo(&cache, root.Get()); });
    waitFor(g_hookIn);
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = psoShutdown(3000);
    const double ms = msSince(t0);
    attacher.join();
    check(g_hookIn && ok && ms >= 300.0, "2_a_shutdown_during_an_attach_waits_for_the_whole_attach");
    check(!psoWorkersRunningForTest(), "2_then_the_attach_s_workers_are_stopped_and_joined");
    psoResetForTest();
  }
  // 3. a shutdown whose deadline ends inside an attach returns false; a later one returns true
  {
    NativePipelineCache cache;
    require(cache.initialize(device.Get(), 64), "cache 3");
    g_hookIn = false; g_hookMs = 700;
    std::thread attacher([&] { psoAttachTo(&cache, root.Get()); });
    waitFor(g_hookIn);
    const auto t0 = std::chrono::steady_clock::now();
    const bool first = psoShutdown(100);
    const double ms = msSince(t0);
    check(g_hookIn && !first && ms < 500.0, "3_a_shutdown_deadline_inside_a_slow_attach_returns_false_in_time");
    const bool second = psoShutdown(3000);
    attacher.join();
    check(second && !psoWorkersRunningForTest(), "3_a_later_shutdown_waits_for_the_attach_and_joins");
    psoResetForTest();
  }
  // 5. the warm list = the user manifest, then the SHIPPED manifest's entries the user's lacks; the shipped file is
  //    read-only (nothing recorded into it), and a warm-only pipeline (no draw used it) is not recorded
  {
    const std::string shipped = std::string(dir) + "sr_pso_warm_test_shipped_" + std::to_string(GetCurrentProcessId()) +
                                ".bin";
    {
      const auto d3 = v(303), d4 = v(304), d5 = v(305);
      const auto image = psom::serialize({&d3, &d4, &d5});   // 303 is in the user's too
      require(psom::writeFileAtomic(shipped, image) ? S_OK : E_FAIL, "shipped manifest write");
    }
    std::vector<uint8_t> shippedBefore, userBefore;
    psom::readFile(shipped, shippedBefore);
    psom::readFile(manifest, userBefore);
    _putenv_s("SR_PSO_SHIPPED_MANIFEST", shipped.c_str());
    NativePipelineCache cache;
    require(cache.initialize(device.Get(), 64), "cache 5");
    g_hookMs = 0;
    psoAttachTo(&cache, root.Get());
    for (int i = 0; i < 5000 && cache.counters().warmCreates < 5; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool shut = psoShutdown(3000);
    std::vector<uint8_t> shippedAfter, userAfter;
    psom::readFile(shipped, shippedAfter);
    psom::readFile(manifest, userAfter);
    check(shut && cache.counters().warmCreates == 5 && cache.size() == 5,
          "5_the_warm_list_is_the_user_manifest_then_the_shipped_entries_it_lacks");
    check(shippedAfter == shippedBefore, "5_the_shipped_manifest_is_read_only");
    check(userAfter == userBefore, "5_warm_only_pipelines_are_not_recorded_into_the_user_manifest");
    _putenv_s("SR_PSO_SHIPPED_MANIFEST", "0");
    DeleteFileA(shipped.c_str());
    psoResetForTest();
  }
  // 6. Astra (10:5x) #5: a WARM-made pipeline that a draw uses AFTER the last write is recorded at the next write
  //    (here the exit's), though no newer pipeline was created in between; and the merge keeps what is on disk
  {
    const std::string user6 = std::string(dir) + "sr_pso_warm_test_user6_" + std::to_string(GetCurrentProcessId()) +
                              ".bin";
    const std::string ship6 = std::string(dir) + "sr_pso_warm_test_ship6_" + std::to_string(GetCurrentProcessId()) +
                              ".bin";
    DeleteFileA(user6.c_str());
    {
      const auto a = v(601);
      const auto image = psom::serialize({&a});
      require(psom::writeFileAtomic(ship6, image) ? S_OK : E_FAIL, "ship6 write");
    }
    _putenv_s("SR_PSO_MANIFEST", user6.c_str());
    _putenv_s("SR_PSO_SHIPPED_MANIFEST", ship6.c_str());
    NativePipelineCache cache;
    require(cache.initialize(device.Get(), 64), "cache 6");
    psoAttachTo(&cache, root.Get());
    for (int i = 0; i < 5000 && cache.counters().warmCreates < 1; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // B: a draw-made pipeline, written now (A is warm-only: not recorded)
    NativePipelineLease lb, la;
    const bool madeB = cache.acquire(v(602), lb) == S_OK;
    psoWriteManifestForTest();
    std::vector<uint8_t> bytes;
    std::vector<NativePipelineDesc> got;
    psom::readFile(user6, bytes);
    psom::parse(bytes, got);
    const bool firstHasOnlyB = got.size() == 1 && got[0].rasterizer.DepthBias == 602;
    // A: the warm-made pipeline drawn now - its first draw use dirties the recording
    const bool hitA = cache.acquire(v(601), la) == S_FALSE;
    const bool shut = psoShutdown(3000);   // the exit write
    psom::readFile(user6, bytes);
    psom::parse(bytes, got);
    bool hasA = false, hasB = false;
    for (const auto& d : got) {
      hasA |= d.rasterizer.DepthBias == 601;
      hasB |= d.rasterizer.DepthBias == 602;
    }
    check(madeB && firstHasOnlyB, "6_a_draw_made_pipeline_is_recorded_and_a_warm_only_one_is_not");
    check(hitA && shut && hasA && hasB && got.size() == 2,
          "6_a_warm_made_pipeline_drawn_after_the_last_write_is_recorded_at_the_next");
    _putenv_s("SR_PSO_MANIFEST", manifest.c_str());
    _putenv_s("SR_PSO_SHIPPED_MANIFEST", "0");
    DeleteFileA(user6.c_str());
    DeleteFileA((user6 + ".lock").c_str());
    DeleteFileA(ship6.c_str());
    psoResetForTest();
  }
  // 7. Astra (10:5x) #3 / #4: an oversized or corrupt manifest is ignored whole; two writers merge, never lose
  {
    const std::string big = std::string(dir) + "sr_pso_warm_test_big_" + std::to_string(GetCurrentProcessId()) + ".bin";
    {
      std::vector<uint8_t> junk(size_t(psom::kMaxFileBytes) + 4096, 0x5A);
      require(psom::writeFileAtomic(big, junk) ? S_OK : E_FAIL, "big write");
    }
    std::vector<uint8_t> bytes;
    std::string why;
    const bool readBig = psom::readFile(big, bytes, &why);
    check(!readBig && bytes.empty() && why.find("cap") != std::string::npos, "7_an_oversized_manifest_is_not_read");
    DeleteFileA(big.c_str());
    // a corrupt header claiming 65536 entries in a few bytes is rejected before it sizes a vector
    std::vector<uint8_t> lie;
    {
      const auto one = v(701);
      lie = psom::serialize({&one});
      lie.resize(20);   // magic (8) + version (4) + the two counts (4 + 4) only
      const uint32_t n = 65536;
      std::memcpy(lie.data() + 16, &n, 4);   // the entry count
    }
    std::vector<NativePipelineDesc> out;
    const bool parsedLie = psom::parse(lie, out, &why);
    check(!parsedLie && out.empty(), "7_a_count_larger_than_the_file_is_rejected_before_any_allocation");
    // two writers: each merges with the file as it is NOW (under the lock) - neither loses the other's entry
    const std::string two = std::string(dir) + "sr_pso_warm_test_two_" + std::to_string(GetCurrentProcessId()) + ".bin";
    DeleteFileA(two.c_str());
    const auto x = v(702), y = v(703);
    psom::MergeResult r1, r2;
    const bool w1 = psom::mergeIntoFile(two, {}, {&x}, &r1, nullptr);
    const bool w2 = psom::mergeIntoFile(two, {}, {&y}, &r2, nullptr);   // knows nothing of x: the file does
    psom::readFile(two, bytes);
    psom::parse(bytes, out);
    check(w1 && w2 && r2.onDisk == 1 && out.size() == 2, "7_a_second_writer_keeps_the_first_writer_s_entries");
    DeleteFileA(two.c_str());
    DeleteFileA((two + ".lock").c_str());
  }
  // 8. fix12: the older builds' manifests removed at the first write - exact names only, 7+ days unwritten, never
  //    one whose build holds its manifest lock now; their lock files go with them (the default user path)
  {
    const std::string la = std::string(dir) + "sr_pso_warm_test_la_" + std::to_string(GetCurrentProcessId());
    const std::string folder = la + "\\SupermanReturnsRecomp\\";
    std::filesystem::create_directories(folder);
    char oldLa[32768] = {};
    GetEnvironmentVariableA("LOCALAPPDATA", oldLa, sizeof oldLa);
    _putenv_s("LOCALAPPDATA", la.c_str());
    _putenv_s("SR_PSO_MANIFEST", "");   // removed: the default per-build path under LOCALAPPDATA
    const std::string h16 = "0123456789abcdef", h40 = "0123456789abcdef0123456789abcdef01234567";
    const auto make = [&](const std::string& name, int hoursOld) {
      const std::string p = folder + name;
      const auto one = v(800);
      require(psom::writeFileAtomic(p, psom::serialize({&one})) ? S_OK : E_FAIL, "stale fixture");
      std::filesystem::last_write_time(p, std::filesystem::file_time_type::clock::now() - std::chrono::hours(hoursOld));
      return p;
    };
    const std::string oldCv = make("pso_manifest_v1_" + h16 + "_" + h40 + ".bin", 24 * 8);
    const std::string oldSum = make("pso_manifest_v1_fedcba9876543210_" + h16 + ".bin", 24 * 8);
    const std::string recent = make("pso_manifest_v1_1111111111111111_" + h40 + ".bin", 1);
    const std::string threeDays = make("pso_manifest_v1_2222222222222222_" + h40 + ".bin", 24 * 3);
    const std::string shortName = make("pso_manifest_v1_" + h16.substr(1) + "_" + h40 + ".bin", 24 * 8);
    const std::string upper = make("pso_manifest_v1_44444444444ABCDE_" + h40 + ".bin", 24 * 8);   // a distinct name:
                                                                                         // NTFS names ignore case
    const std::string held = make("pso_manifest_v1_3333333333333333_" + h40 + ".bin", 24 * 8);
    { HANDLE f = CreateFileA((oldCv + ".lock").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
      CloseHandle(f); }
    HANDLE heldLock = CreateFileA((held + ".lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, 0,
                                  nullptr);
    OVERLAPPED ov{};
    const bool lockedHeld = LockFileEx(heldLock, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov) != 0;
    NativePipelineCache cache;
    require(cache.initialize(device.Get(), 64), "cache 8");
    g_hookMs = 0;
    psoAttachTo(&cache, root.Get());
    NativePipelineLease l;
    const bool made = cache.acquire(v(801), l) == S_OK;
    const bool shut = psoShutdown(3000);   // the exit write: the first write runs the cleanup
    UnlockFileEx(heldLock, 0, 1, 0, &ov);
    CloseHandle(heldLock);
    const auto there = [](const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
    size_t mine = 0;
    for (const auto& e : std::filesystem::directory_iterator(folder))
      if (e.path().filename().string().rfind("pso_manifest_v1_", 0) == 0 && e.path().extension() == ".bin" &&
          e.path().string() != oldCv && e.path().string() != oldSum && e.path().string() != recent &&
          e.path().string() != threeDays && e.path().string() != shortName && e.path().string() != upper &&
          e.path().string() != held)
        ++mine;
    check(lockedHeld && made && shut && mine == 1, "8_this_build_s_manifest_written_under_the_default_path");
    check(!there(oldCv) && !there(oldCv + ".lock") && !there(oldSum),
          "8_an_older_build_s_manifest_unwritten_7_days_is_removed_with_its_lock_file");
    check(there(recent) && there(threeDays), "8_a_manifest_written_in_the_last_7_days_is_kept");
    check(there(shortName) && there(upper), "8_only_exact_names_are_removed");
    check(there(held), "8_a_manifest_whose_build_holds_its_lock_is_kept");
    _putenv_s("LOCALAPPDATA", oldLa);
    _putenv_s("SR_PSO_MANIFEST", manifest.c_str());
    std::error_code ec;
    std::filesystem::remove_all(la, ec);
    psoResetForTest();
  }
  // 9. fix12: the exit write waits for a manifest read still running on the warm thread (within the deadline)
  {
    const std::string user9 = std::string(dir) + "sr_pso_warm_test_user9_" + std::to_string(GetCurrentProcessId()) +
                              ".bin";
    DeleteFileA(user9.c_str());
    _putenv_s("SR_PSO_MANIFEST", user9.c_str());
    static std::atomic<bool> loadIn{false};
    loadIn = false;
    psoSetLoadHookForTest(+[] {
      loadIn = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(800));   // a slow read of a large manifest
    });
    NativePipelineCache cache;
    require(cache.initialize(device.Get(), 64), "cache 9");
    g_hookMs = 0;
    psoAttachTo(&cache, root.Get());
    waitFor(loadIn);
    NativePipelineLease l;
    const bool made = cache.acquire(v(901), l) == S_OK;
    const auto t0 = std::chrono::steady_clock::now();
    const bool shut = psoShutdown(3000);
    const double ms = msSince(t0);
    std::vector<uint8_t> bytes;
    std::vector<NativePipelineDesc> got;
    const bool read = psom::readFile(user9, bytes) && psom::parse(bytes, got);
    check(loadIn && made && shut && read && got.size() == 1 && got[0].rasterizer.DepthBias == 901 && ms < 3000.0,
          "9_the_exit_write_waits_for_a_manifest_read_still_running");
    psoSetLoadHookForTest(nullptr);
    _putenv_s("SR_PSO_MANIFEST", manifest.c_str());
    DeleteFileA(user9.c_str());
    DeleteFileA((user9 + ".lock").c_str());
    psoResetForTest();
  }
  // 10. a shutdown that returns false (the manifest read outlasts its deadline), then a reset, then a re-attach
  {
    static std::atomic<bool> loadIn10{false};
    loadIn10 = false;
    psoSetLoadHookForTest(+[] {
      loadIn10 = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1500));   // the warm thread's read, past the deadline
    });
    NativePipelineCache cache;   // outlives the reset below: a worker left running still uses it
    require(cache.initialize(device.Get(), 64), "cache 10");
    g_hookMs = 0;
    psoAttachTo(&cache, root.Get());
    waitFor(loadIn10);
    const bool first = psoShutdown(200);
    check(loadIn10 && !first && psoWorkersRunningForTest(), "10_a_shutdown_deadline_inside_a_slow_read_returns_false");
    psoSetLoadHookForTest(nullptr);
    psoResetForTest();
    const bool clean = !psoWorkersRunningForTest();
    check(clean, "10_a_reset_stops_and_joins_the_worker_a_false_shutdown_left_running");
    std::fflush(stdout);
    // before the fix: the re-attach move-assigned onto the joinable std::thread -> std::terminate (0xC0000409)
    psoAttachTo(&cache, root.Get());
    for (int i = 0; i < 5000 && cache.counters().warmCreates < 3; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool second = psoShutdown(3000);
    check(second && !psoWorkersRunningForTest() && cache.counters().warmCreates == 3,
          "10_then_a_re_attach_starts_clean_and_its_shutdown_joins");
    psoResetForTest();
  }
  // 4. an attach after a shutdown starts nothing
  {
    NativePipelineCache cache;
    require(cache.initialize(device.Get(), 64), "cache 4");
    g_hookIn = false; g_hookMs = 0;
    const bool shut = psoShutdown(100);
    psoAttachTo(&cache, root.Get());
    check(shut && !g_hookIn && !psoWorkersRunningForTest() && cache.size() == 0,
          "4_an_attach_after_a_shutdown_starts_nothing");
  }
  psoSetAttachHookForTest(nullptr);
  NativePipelineCache::setCreateGateForTest(nullptr);
  DeleteFileA(manifest.c_str());
  std::printf("[PSO_WARM_TEST] passed=%u failed=%u\n", passed, failed);
  return failed ? 1 : 0;
}
