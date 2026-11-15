// present_selftest.cpp - the calibration target for tools/perf/present_probe.js (canary lane, iter-467).
//
// A minimal D3D12 flip-model window that presents a known number of frames and logs its OWN QPC
// stamp right before and right after every IDXGISwapChain::Present call. tools/perf/canary_compare.py
// `selftest` attaches the present probe to it and checks the probe's per-present stamps against this
// log (count, order, offset). It is the same DXGI path the game and Xenia use (D3D12, flip-discard,
// windowed), so it proves the instrument before any game roll uses it.
//
//   present_selftest.exe <out.csv> <frames> <sync 0|1> <sleep_ms> [<start_delay_ms>]
//
// The window opens without activation at the bottom of the z-order (automated runs never take the
// foreground). No input is read. Build (no game slot needed):
//   clang++ -O2 -std=c++17 tools/perf/present_selftest.cpp -o <out>/present_selftest.exe
//           -ld3d12 -ldxgi -luser32
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <timeapi.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv) {
  if (argc < 5) { std::fprintf(stderr, "usage: %s out.csv frames sync sleep_ms [start_delay_ms]\n", argv[0]); return 2; }
  const char* outPath = argv[1];
  const int frames = std::atoi(argv[2]);
  const UINT sync = (UINT)std::atoi(argv[3]);
  const int sleepMs = std::atoi(argv[4]);
  const int startDelay = argc > 5 ? std::atoi(argv[5]) : 3000;
  timeBeginPeriod(1);

  WNDCLASSW wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"SrPresentSelftest";
  RegisterClassW(&wc);
  RECT r{0, 0, 640, 360};
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"present_selftest", WS_OVERLAPPEDWINDOW,
                              40, 40, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

  IDXGIFactory4* factory = nullptr;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return 3;
  ID3D12Device* dev = nullptr;
  if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return 4;
  D3D12_COMMAND_QUEUE_DESC qd{};
  ID3D12CommandQueue* q = nullptr;
  dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
  BOOL tearing = FALSE;
  IDXGIFactory5* f5 = nullptr;
  if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f5)))) {
    f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
    f5->Release();
  }
  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Width = 640; sd.Height = 360;
  sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
  IDXGISwapChain1* sc1 = nullptr;
  if (FAILED(factory->CreateSwapChainForHwnd(q, hwnd, &sd, nullptr, nullptr, &sc1))) return 5;
  IDXGISwapChain3* sc = nullptr;
  sc1->QueryInterface(IID_PPV_ARGS(&sc));

  ID3D12DescriptorHeap* rtvHeap = nullptr;
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.NumDescriptors = 2; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
  const UINT rtvInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  ID3D12Resource* bufs[2]{};
  for (UINT i = 0; i < 2; ++i) {
    sc->GetBuffer(i, IID_PPV_ARGS(&bufs[i]));
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)i * rtvInc;
    dev->CreateRenderTargetView(bufs[i], nullptr, h);
  }
  ID3D12CommandAllocator* alloc = nullptr;
  dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
  ID3D12GraphicsCommandList* cl = nullptr;
  dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl));
  cl->Close();
  ID3D12Fence* fence = nullptr;
  dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  UINT64 fv = 0;

  LARGE_INTEGER f; QueryPerformanceFrequency(&f);
  std::vector<LONGLONG> before(frames), after(frames);
  // Give the probe time to attach (the caller waits for this process's "ready" line on stdout).
  std::printf("ready pid=%lu qpf=%lld\n", GetCurrentProcessId(), (long long)f.QuadPart);
  std::fflush(stdout);
  DWORD t0 = GetTickCount();
  MSG msg;
  while (GetTickCount() - t0 < (DWORD)startDelay) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    Sleep(10);
  }
  for (int i = 0; i < frames; ++i) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    const UINT bi = sc->GetCurrentBackBufferIndex();
    alloc->Reset();
    cl->Reset(alloc, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bufs[bi];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cl->ResourceBarrier(1, &b);
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)bi * rtvInc;
    const float c[4] = {(i % 64) / 64.0f, 0.2f, 0.4f, 1.0f};
    cl->ClearRenderTargetView(h, c, 0, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cl->ResourceBarrier(1, &b);
    cl->Close();
    ID3D12CommandList* lists[] = {cl};
    q->ExecuteCommandLists(1, lists);
    LARGE_INTEGER a, z;
    QueryPerformanceCounter(&a);
    sc->Present(sync, (sync == 0 && tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0);
    QueryPerformanceCounter(&z);
    before[i] = a.QuadPart;
    after[i] = z.QuadPart;
    q->Signal(fence, ++fv);
    if (fence->GetCompletedValue() < fv) { fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, INFINITE); }
    if (sleepMs > 0) Sleep(sleepMs);
  }
  FILE* o = std::fopen(outPath, "w");
  if (!o) return 6;
  std::fprintf(o, "frame,qpc_before,qpc_after,qpf\n");
  for (int i = 0; i < frames; ++i)
    std::fprintf(o, "%d,%lld,%lld,%lld\n", i, (long long)before[i], (long long)after[i], (long long)f.QuadPart);
  std::fclose(o);
  std::printf("done frames=%d tearing=%d\n", frames, (int)tearing);
  std::fflush(stdout);
  // Linger so the probe can drain its ring before the process exits.
  Sleep(1500);
  return 0;
}
