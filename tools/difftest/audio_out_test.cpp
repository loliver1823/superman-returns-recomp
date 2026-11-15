// iter-463 audio arc steps 4-8: the native XAudio render-driver output path (SR_AUDIO_OUT,
// kernel/apu/audio_system.hpp), KeWaitForMultipleObjects (SR_KE_WAITMULTI, kernel/stubs.cpp) and
// KeGetCurrentProcessType from the KTHREAD (SR_PROCTYPE, kernel/kpcr.hpp).
// Reference: xenia-canary @0bd090dbe apu/audio_system.cc, apu/sdl/sdl_audio_driver.cc,
// apu/conversion.h, kernel/xboxkrnl/xboxkrnl_audio.cc, xboxkrnl_threading.cc:429-437 + 1057-1088,
// kernel/xobject.cc:265-340, kernel/xthread.cc:207-217.
//
// Run: build/difftest.exe --audio-out-test     (exit = failed checks; ~5 s, no sound: null sinks)
//      build/difftest.exe --ke-waitmulti-test  (exit = failed checks; sets SR_KE_WAITMULTI=1 if unset)
//      build/difftest.exe --proctype-test      (exit = failed checks)
//      build/difftest.exe --audio-sdl-replay --in <x.afr|synthetic> --out <dir> [--ch N] [--frames N]
//        [--skip N] [--mutant chanmap|sdl_downmix] [--env-from-caller]   (lane sdl3-audio; exit 0 = PASS)
// Attribution / mutation:
//   SR_KE_WAITMULTI=0 --ke-waitmulti-test -> the generic stub answers: W1..W9 + W11 + W12 FAIL.
//   --proctype-test P5 runs the gate-off world in-process (0 -> no XAudio worker config).
//   --audio-out-test A2m re-creates the non-SSE conversion (back channels swapped) and the check's
//   own predicate must see it differ.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/apu/audio_system.hpp"
#include "kernel/audio_native_census.hpp"
#include "kernel/export_registry.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/kpcr.hpp"
#include "kernel/kthread_objects.hpp"
#include "kernel/stubs.hpp"

PPC_EXTERN_FUNC(sub_82117DF8);   // AudioBuildSpeakerConfig: KeGetCurrentProcessType()==1 -> worker cfg
PPC_EXTERN_FUNC(__imp__KeGetCurrentProcessType);
PPC_EXTERN_FUNC(__imp__KeWaitForMultipleObjects);

#include <SDL3/SDL.h>   // lane sdl3-audio: the SDL3 environment / events the SDL sink tests drive

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace sr::kernel {
std::string legacyAudioOpenForTest();   // kernel/audio_render_driver.cpp (SR_DIFFTEST): the SR_AUDIO_OUT=0 device open
}

namespace K = sr::kernel;
namespace A = sr::kernel::apu;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}
int finish(const char* tag) {
  std::printf("[%s] %d passed, %d failed\n", tag, g_pass, g_fail);
  return g_fail;
}

uint8_t* g_base = nullptr;
constexpr size_t kGuestSize = 0x100000000ull;
K::ExportFn getExp(const char* name) { return K::ExportRegistry::instance().getExport(name); }
uint32_t call(const char* name, uint32_t a3, uint32_t a4 = 0, uint32_t a5 = 0, uint32_t a6 = 0,
              uint32_t a7 = 0, uint32_t a8 = 0, uint32_t a9 = 0, uint32_t a10 = 0) {
  K::ExportRegistry::setCurrentExtraArgs(a9, a10);
  return getExp(name)(a3, a4, a5, a6, a7, a8);
}
double msSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
uint32_t fbits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
// The size of a file, 0 when it is missing (std::filesystem::file_size(p, ec) returns uintmax_t(-1) on error, which
// passed every "> 0" check - Astra P2, 2026-10-18).
uint64_t fsize(const std::filesystem::path& p) {
  std::error_code ec;
  const auto n = std::filesystem::file_size(p, ec);
  return ec ? 0 : uint64_t(n);
}
// Lane sdl3-audio: SDL3 reads its own snapshot of the process environment (SDL_GetEnvironment), so a
// _putenv after SDL started is invisible to it. Tests set both copies: the CRT one (our getenv reads it) and
// SDL's. An empty value removes the variable from both.
void setEnv(const char* name, const std::string& value) {
  _putenv((std::string(name) + "=" + value).c_str());
  if (value.empty()) SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), name);
  else SDL_SetEnvironmentVariable(SDL_GetEnvironment(), name, value.c_str(), true);
}
// The SDL disk driver into `raw` (SDL3 names), the SDL2 names cleared.
void diskDriver(const std::string& raw) {
  setEnv("SDL_AUDIO_DRIVER", "disk");
  setEnv("SDL_AUDIO_DISK_OUTPUT_FILE", raw);
  setEnv("SDL_AUDIODRIVER", "");
  setEnv("SDL_DISKAUDIOFILE", "");
}
void putBEFloat(uint8_t* p, float f) {
  const uint32_t u = __builtin_bswap32(fbits(f));
  std::memcpy(p, &u, 4);
}

// ── audio ──────────────────────────────────────────────────────────────────────────────────
constexpr uint32_t kClientStruct = 0x90700000u;   // {callback, arg}
constexpr uint32_t kDriverOut = 0x90700010u;
constexpr uint32_t kVolOut = 0x90700020u;
constexpr uint32_t kCells = 0x90700100u;          // 8 wrapped-arg cells
constexpr uint32_t kFrameVA = 0x90710000u;        // one 6144-byte guest frame

// A test sink: records submitted frames; consumes only when told to.
struct RecSink final : A::AudioSink {
  std::function<void()> consumed;
  std::mutex mu;
  std::vector<std::array<uint8_t, A::kAudioFrameBytes>> frames;
  explicit RecSink(std::function<void()> c) : consumed(std::move(c)) {}
  void submitFrame(const uint8_t* be) override {
    std::array<uint8_t, A::kAudioFrameBytes> a;
    std::memcpy(a.data(), be, a.size());
    std::lock_guard<std::mutex> lk(mu);
    frames.push_back(a);
  }
  void shutdown() override {}
  int deviceChannels() const override { return 2; }
  const char* kind() const override { return "rec"; }
};
RecSink* g_lastRec = nullptr;

void makeFrame(uint8_t* f) {
  // channel c, sample s = (c + 1) * 0.01 + s * 1e-4 (planar, BE)
  for (unsigned c = 0; c < 6; ++c)
    for (unsigned s = 0; s < 256; ++s)
      putBEFloat(f + (c * 256 + s) * 4, float(c + 1) * 0.01f + float(s) * 1e-4f);
}

// Run the worker on a host thread for `ms` with the given invoke; returns stats delta.
A::AudioStats runWorkerFor(int ms, const std::function<bool(uint32_t, uint32_t)>& inv) {
  auto& as = A::AudioSystem::instance();
  const A::AudioStats s0 = as.stats();
  std::thread t([&] { as.runWorker(inv); });
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  as.stopWorker();
  t.join();
  A::AudioStats s1 = as.stats();
  A::AudioStats d{};
  d.callbacks = s1.callbacks - s0.callbacks;
  d.submits = s1.submits - s0.submits;
  d.consumed = s1.consumed - s0.consumed;
  d.noSubmitCallbacks = s1.noSubmitCallbacks - s0.noSubmitCallbacks;
  d.creditMisses = s1.creditMisses - s0.creditMisses;
  d.invalidSubmits = s1.invalidSubmits - s0.invalidSubmits;
  d.callbackExceptions = s1.callbackExceptions - s0.callbackExceptions;
  return d;
}
}  // namespace

// ── lane sdl3-audio: the SDL replay harness ────────────────────────────────────────────────────
// Feeds recorded guest frames (AFR captures, tools/audio_oracle_compare.py format, or a synthetic signal)
// through the REAL SDL sink on SDL's disk driver and checks what came out against the frames themselves:
//   wav.wav   (SR_AUDIO_WAV, what pull() produced)  == the converted frames, bit for bit, from frame 0;
//   disk.raw  (the device's own bytes)              == the same, after the silence the device played
//                                                      while paused (2 / 6 ch devices; others: SDL's
//                                                      converter, reported for tools/audio_sdl_compare.py).
// The queue is kept 32 frames deep (credits never run out, no underrun), so the output is exactly the
// frames in order. The same harness built on the SDL2 tree is the control (archive NOTES).
// CLI: difftest --audio-sdl-replay --in <x.afr|synthetic> --out <dir> [--ch N] [--frames N] [--skip N]
//      [--mutant chanmap|sdl_downmix] [--env-from-caller]     (exit 0 = PASS)
namespace {
using Frame = std::array<uint8_t, A::kAudioFrameBytes>;

std::vector<Frame> syntheticFrames(size_t n) {
  // Per channel a different tone, level and DC offset, so a swapped or dropped channel changes the bytes.
  std::vector<Frame> out(n);
  for (size_t f = 0; f < n; ++f)
    for (unsigned c = 0; c < 6; ++c)
      for (unsigned s = 0; s < 256; ++s) {
        const double t = double(f * 256 + s) / 48000.0;
        const float v = float(0.05 * (c + 1) * std::sin(2.0 * 3.14159265358979 * 110.0 * (c + 1) * t) +
                              0.001 * (c + 1));
        putBEFloat(out[f].data() + (c * 256 + s) * 4, v);
      }
  return out;
}

bool loadAfr(const std::string& path, size_t skip, size_t maxFrames, std::vector<Frame>* out, std::string* why) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) { *why = "cannot open " + path; return false; }
  _fseeki64(f, (long long)skip * (16 + A::kAudioFrameBytes), SEEK_SET);
  uint8_t rec[16 + A::kAudioFrameBytes];
  while (out->size() < maxFrames && std::fread(rec, 1, sizeof(rec), f) == sizeof(rec)) {
    uint32_t magic;
    std::memcpy(&magic, rec, 4);
    if (magic != 0x31524641u) { *why = "bad AFR magic"; std::fclose(f); return false; }
    Frame fr;
    std::memcpy(fr.data(), rec + 16, A::kAudioFrameBytes);
    out->push_back(fr);
  }
  std::fclose(f);
  if (out->empty()) { *why = "no frames read"; return false; }
  return true;
}

std::vector<uint8_t> readFile(const std::string& p) {
  std::vector<uint8_t> v;
  if (FILE* f = std::fopen(p.c_str(), "rb")) {
    _fseeki64(f, 0, SEEK_END);
    const long long n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    if (n > 0) {
      v.resize(size_t(n));
      v.resize(std::fread(v.data(), 1, v.size(), f));
    }
    std::fclose(f);
  }
  return v;
}

struct ReplayResult {
  std::string driver, why;
  int inCh = 0, hwCh = 0;
  uint64_t frames = 0, consumed = 0, consumedAfterTail = 0, underrunsBody = 0, wavBytes = 0, diskBytes = 0,
           expectedBytes = 0;
  long long diskOffsetBytes = -1, firstMismatch = -1;
  bool haveExpected = false, wavExact = false, diskExact = false, ok = false;
  double maxAbsDiff = 0;
};

long long firstNonZeroFloat(const uint8_t* p, size_t bytes) {
  for (size_t i = 0; i + 4 <= bytes; i += 4) {
    uint32_t u;
    std::memcpy(&u, p + i, 4);
    if (u & 0x7FFFFFFFu) return (long long)i;
  }
  return -1;
}
bool allZero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (p[i]) return false;
  return true;
}
double maxDiff(const uint8_t* a, const uint8_t* b, size_t bytes, long long* first) {
  double m = 0;
  *first = -1;
  for (size_t i = 0; i + 4 <= bytes; i += 4) {
    float x, y;
    std::memcpy(&x, a + i, 4);
    std::memcpy(&y, b + i, 4);
    if (std::memcmp(a + i, b + i, 4) != 0 && *first < 0) *first = (long long)i;
    m = std::max(m, double(std::fabs(x - y)));
  }
  return m;
}

ReplayResult replayThroughSdl(const std::vector<Frame>& frames, int reqCh, const std::string& outDir,
                              const char* mutant, bool envFromCaller) {
  namespace fs = std::filesystem;
  ReplayResult r;
  r.frames = frames.size();
  std::error_code ec;
  fs::create_directories(outDir, ec);
  const std::string disk = (fs::path(outDir) / "disk.raw").string();
  const std::string wav = (fs::path(outDir) / "wav.wav").string();
  fs::remove(disk, ec);
  fs::remove(wav, ec);
  if (!envFromCaller) diskDriver(disk);
  _putenv(("SR_AUDIO_WAV=" + wav).c_str());
  _putenv("SR_AUDIO_SINK=");
  _putenv(("SR_AUDIO_TEST_DEVICE_CH=" + std::to_string(reqCh)).c_str());
  _putenv((std::string("SR_AUDIO_TEST_MUTANT=") + (mutant ? mutant : "")).c_str());
  auto& as = A::AudioSystem::instance();
  as.resetForTest();
  as.setSinkFactoryForTest(nullptr);
  size_t idx = 0;
  const uint64_t c0 = as.stats().consumed, u0 = as.stats().underruns;
  const uint32_t reg = as.registerClient(0x821209F0u, 0, &idx);
  const A::AudioSystem::SinkInfo info = as.sinkInfoForTest(idx);
  if (const char* d = SDL_GetCurrentAudioDriver()) r.driver = d;
  r.inCh = info.channels;
  r.hwCh = info.hostChannels;
  bool fed = false;
  if (reg == 0 && info.kind && std::strcmp(info.kind, "sdl") == 0) {
    size_t sent = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const double budgetMs = double(frames.size()) * 5.333 * 3.0 + 5000.0;
    while (msSince(t0) < budgetMs) {
      // Underruns first, then the consumed count: every underrun read here happened while a frame was still
      // to come (a pull found the queue empty before the last frame was popped) = a real gap. The device's next
      // pull after the last frame also "underruns" (the trailing silence); it is not counted.
      const uint64_t under = as.stats().underruns - u0;
      const uint64_t done = as.stats().consumed - c0;
      if (done >= frames.size()) { fed = true; break; }
      r.underrunsBody = under;
      if (sent < frames.size() && sent - done < 32) {
        as.submitFrame(idx, frames[sent].data());
        ++sent;
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      // The game's main thread pumps SDL events; SDL3 handles a lost device (zombie + REMOVED) in that pump
      // (SDL_RunOnMainThread). This is the thread that initialised SDL's events here, so it pumps the same way.
      SDL_PumpEvents();
    }
    r.consumed = as.stats().consumed - c0;
    std::this_thread::sleep_for(std::chrono::milliseconds(60));   // a little trailing silence
    r.consumedAfterTail = as.stats().consumed - c0;                  // silence is never credited
  }
  as.unregisterClient(idx);   // closes the stream: the disk file and the WAV are complete
  _putenv("SR_AUDIO_WAV=");
  _putenv("SR_AUDIO_TEST_DEVICE_CH=");
  _putenv("SR_AUDIO_TEST_MUTANT=");
  if (!info.kind || std::strcmp(info.kind, "sdl") != 0) { r.why = "no SDL sink (null fallback)"; return r; }
  if (!fed) { r.why = "the device did not consume every frame in time"; return r; }

  const std::vector<uint8_t> w = readFile(wav), d = readFile(disk);
  r.wavBytes = w.size() > 44 ? w.size() - 44 : 0;
  r.diskBytes = d.size();
  // What the frames become in a given channel count (our conversion.h port; 0 = not ours to predict).
  auto convertAll = [&](int ch) {
    std::vector<uint8_t> v;
    if (ch != 2 && ch != 6) return v;
    std::vector<float> one(256 * size_t(ch));
    v.reserve(frames.size() * one.size() * 4);
    for (const Frame& fr : frames) {
      if (ch == 6) A::convertSeq6BEToInterleaved6LE(one.data(), fr.data(), 256);
      else A::convertSeq6BEToInterleaved2LE(one.data(), fr.data(), 256);
      const uint8_t* b = reinterpret_cast<const uint8_t*>(one.data());
      v.insert(v.end(), b, b + one.size() * 4);
    }
    return v;
  };
  // The observer sees our input format; the device must play what ITS channel count calls for: a stereo
  // device the Xenia downmix, a 5.1 device the frames (a 1 / 4 / 8 ch device: SDL's converter, offline).
  const std::vector<uint8_t> expIn = convertAll(r.inCh), expDev = convertAll(r.hwCh);
  r.haveExpected = !expDev.empty();
  r.expectedBytes = expDev.size();
  if (FILE* f = std::fopen((fs::path(outDir) / "expected.raw").string().c_str(), "wb")) {
    const std::vector<uint8_t>& e = expDev.empty() ? expIn : expDev;
    std::fwrite(e.data(), 1, e.size(), f);
    std::fclose(f);
  }
  // The observer: from frame 0, then only the silence after the last frame.
  r.wavExact = !expIn.empty() && r.wavBytes >= expIn.size() &&
               std::memcmp(w.data() + 44, expIn.data(), expIn.size()) == 0 &&
               allZero(w.data() + 44 + expIn.size(), r.wavBytes - expIn.size());
  // The device: silence while paused, the frames, silence.
  if (r.haveExpected) {
    const long long k = firstNonZeroFloat(expDev.data(), expDev.size());
    const long long j = firstNonZeroFloat(d.data(), d.size());
    if (k >= 0 && j >= k) {
      const long long off = j - k;
      r.diskOffsetBytes = off;
      if (size_t(off) + expDev.size() <= d.size()) {
        r.maxAbsDiff = maxDiff(d.data() + off, expDev.data(), expDev.size(), &r.firstMismatch);
        r.diskExact = r.firstMismatch < 0 && off % (4 * r.hwCh) == 0 && allZero(d.data(), size_t(off)) &&
                      allZero(d.data() + off + expDev.size(), d.size() - size_t(off) - expDev.size());
      }
    }
    r.ok = r.wavExact && r.diskExact && r.underrunsBody == 0;
  } else {
    r.ok = r.wavExact && r.diskBytes > 0 && r.underrunsBody == 0;   // SDL's converter: tools/audio_sdl_compare.py
  }
  // Exactly one credit per frame, none for the silence after the last one.
  if (r.consumed != r.frames || r.consumedAfterTail != r.frames) {
    r.ok = false;
    r.why = "credits != frames (consumed " + std::to_string(r.consumed) + ", after the tail " +
            std::to_string(r.consumedAfterTail) + ")";
  }
  if (!r.ok && r.why.empty())
    r.why = !r.wavExact ? "WAV != the converted frames" : !r.diskExact ? "device bytes != the converted frames"
                                                                          : "underrun while frames were queued";
  if (FILE* f = std::fopen((fs::path(outDir) / "result.json").string().c_str(), "w")) {
    std::fprintf(f,
                 "{\"driver\": \"%s\", \"requested_ch\": %d, \"input_ch\": %d, \"device_ch\": %d, \"frames\": %llu, "
                 "\"consumed\": %llu, \"underruns_body\": %llu, \"wav_bytes\": %llu, \"disk_bytes\": %llu, "
                 "\"expected_bytes\": %llu, \"disk_offset_bytes\": %lld, \"first_mismatch_byte\": %lld, "
                 "\"max_abs_diff\": %.9g, \"wav_exact\": %s, \"disk_exact\": %s, \"mutant\": \"%s\", \"ok\": %s, "
                 "\"why\": \"%s\"}\n",
                 r.driver.c_str(), reqCh, r.inCh, r.hwCh, (unsigned long long)r.frames, (unsigned long long)r.consumed,
                 (unsigned long long)r.underrunsBody, (unsigned long long)r.wavBytes, (unsigned long long)r.diskBytes,
                 (unsigned long long)r.expectedBytes, r.diskOffsetBytes, r.firstMismatch, r.maxAbsDiff,
                 r.wavExact ? "true" : "false", r.diskExact ? "true" : "false", mutant ? mutant : "",
                 r.ok ? "true" : "false", r.why.c_str());
    std::fclose(f);
  }
  return r;
}

void printReplay(const char* tag, const ReplayResult& r) {
  std::printf("      %s: driver=%s in=%d dev=%d frames=%llu consumed=%llu underruns=%llu wav=%llu disk=%llu "
              "off=%lld wav_exact=%d disk_exact=%d maxdiff=%.3g%s%s\n",
              tag, r.driver.c_str(), r.inCh, r.hwCh, (unsigned long long)r.frames, (unsigned long long)r.consumed,
              (unsigned long long)r.underrunsBody, (unsigned long long)r.wavBytes, (unsigned long long)r.diskBytes,
              r.diskOffsetBytes, r.wavExact, r.diskExact, r.maxAbsDiff, r.why.empty() ? "" : " why=",
              r.why.c_str());
}

void setupGuestForAudio() {
  g_base = K::getGuestBase();
  K::setGuestMemory(g_base, kGuestSize);
  A::AudioSystem::instance().setCellBaseForTest(kCells);
}
}  // namespace

int runAudioSdlReplay(int argc, char** argv) {
  std::string in = "synthetic", out;
  int ch = 6;
  size_t frames = 2000, skip = 0;
  const char* mutant = nullptr;
  bool envFromCaller = false;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--in") in = next();
    else if (a == "--out") out = next();
    else if (a == "--ch") ch = std::atoi(next().c_str());
    else if (a == "--frames") frames = size_t(std::atoll(next().c_str()));
    else if (a == "--skip") skip = size_t(std::atoll(next().c_str()));
    else if (a == "--mutant") { static std::string m; m = next(); mutant = m.c_str(); }
    else if (a == "--env-from-caller") envFromCaller = true;
  }
  if (out.empty()) { std::printf("usage: --audio-sdl-replay --in <afr|synthetic> --out <dir> [--ch N] ...\n"); return 2; }
  setupGuestForAudio();
  std::vector<Frame> fr;
  std::string why;
  if (in == "synthetic") fr = syntheticFrames(frames);
  else if (!loadAfr(in, skip, frames, &fr, &why)) { std::printf("[AUDIO_REPLAY] FAIL %s\n", why.c_str()); return 2; }
  std::printf("[AUDIO_REPLAY] in=%s frames=%zu skip=%zu ch=%d mutant=%s env=%s -> %s\n", in.c_str(), fr.size(), skip,
              ch, mutant ? mutant : "-", envFromCaller ? "caller" : "disk", out.c_str());
  const ReplayResult r = replayThroughSdl(fr, ch, out, mutant, envFromCaller);
  printReplay("replay", r);
  std::printf("[AUDIO_REPLAY] %s\n", r.ok ? "PASS" : "FAIL");
  return r.ok ? 0 : 1;
}

int runAudioOutTest() {
  std::printf("[AUDIO_OUT_TEST] XAudio render-driver output path vs Xenia AudioSystem (SR_AUDIO_OUT)\n");
  g_base = K::getGuestBase();
  K::setGuestMemory(g_base, kGuestSize);
  A::audioOutSetEnabledForTest(1);
  K::registerStubs();   // registerAudioExports sees the gate
  auto& as = A::AudioSystem::instance();
  as.setCellBaseForTest(kCells);

  // ── A1/A2: conversion (conversion.h AMD64 path) ─────────────────────────────────────────
  {
    std::vector<uint8_t> fr(A::kAudioFrameBytes);
    makeFrame(fr.data());
    std::vector<float> o6(256 * 6), o2(256 * 2);
    A::convertSeq6BEToInterleaved6LE(o6.data(), fr.data(), 256);
    bool ok6 = true;
    for (unsigned s = 0; s < 256 && ok6; ++s)
      for (unsigned c = 0; c < 6; ++c)
        if (fbits(o6[s * 6 + c]) != fbits(float(c + 1) * 0.01f + float(s) * 1e-4f)) { ok6 = false; break; }
    check(ok6, "A1 6 ch: planar BE -> interleaved LE, bit-exact");

    // Hand value: FL=1 FR=0.2 FC=0.4 LFE=9 ch4=0.5 ch5=0.1 -> L=(1+0.5+0.2)/2.5 R=(0.2+0.1+0.2)/2.5
    std::vector<uint8_t> h(A::kAudioFrameBytes, 0);
    const float v[6] = {1.0f, 0.2f, 0.4f, 9.0f, 0.5f, 0.1f};
    for (unsigned c = 0; c < 6; ++c) putBEFloat(h.data() + c * 256 * 4, v[c]);
    A::convertSeq6BEToInterleaved2LE(o2.data(), h.data(), 256);
    check(std::fabs(o2[0] - 0.68f) < 1e-6f && std::fabs(o2[1] - 0.2f) < 1e-6f,
          "A2 2 ch downmix: L=(FL+BL+FC/2)/2.5, R=(FR+BR+FC/2)/2.5, BL=ch4, LFE dropped");
    check(o2[2] == 0.0f && o2[3] == 0.0f, "A2b silent samples stay 0");
    // A2m: the generic (non-SSE) path swaps ch4/ch5; the predicate must see the difference.
    const float mutL = (1.0f + 0.1f + 0.2f) * (1.0f / 2.5f);
    check(std::fabs(o2[0] - mutL) > 1e-3f, "A2m mutation (back channels swapped) is detected");
  }

  // ── A3: registration (xboxkrnl_audio.cc:55-81) ───────────────────────────────────────────
  RecSink* rec[A::kAudioMaxClients] = {};
  bool recConsume = false;
  as.setSinkFactoryForTest([&](size_t i, std::function<void()> c) -> std::unique_ptr<A::AudioSink> {
    auto s = std::make_unique<RecSink>(std::move(c));
    rec[i] = s.get();
    g_lastRec = s.get();
    return s;
  });
  {
    K::writeGuestBE32(kClientStruct + 0, 0x821209F0u);
    K::writeGuestBE32(kClientStruct + 4, 0x12345678u);
    K::writeGuestBE32(kDriverOut, 0);
    const uint32_t st = as.registerClientExport(kClientStruct, kDriverOut);
    check(st == 0 && K::readGuestU32BE(kDriverOut) == 0x41550000u, "A3a handle = 0x41550000 | index 0");
    check(as.wrappedArgCell(0) == kCells && K::readGuestU32BE(kCells) == 0x12345678u,
          "A3b wrapped-arg cell holds arg big-endian (r3 of the callback)");
    check(as.creditsAvailable(0) == 64u, "A3c registration releases 64 credits (kMaximumQueuedFrames)");
    check(as.registerClientExport(0, kDriverOut) == A::kXEInvalidArg, "A3d null pClient -> X_E_INVALIDARG");
    K::writeGuestBE32(kClientStruct + 0, 0);
    check(as.registerClientExport(kClientStruct, kDriverOut) == A::kXEInvalidArg,
          "A3e null callback -> X_E_INVALIDARG");
    K::writeGuestBE32(kClientStruct + 0, 0x821209F0u);
  }
  // ── A4: 8 slots, index reuse ─────────────────────────────────────────────────────────────
  {
    size_t idx = 99;
    bool all = true;
    for (int i = 1; i < 8; ++i) all = all && as.registerClient(0x82000000u + i, i, &idx) == 0 && idx == size_t(i);
    check(all, "A4a indices 1..7 handed out in order");
    check(as.registerClient(0x82000100u, 0, &idx) == A::kXEFail, "A4b a 9th client is refused");
    as.unregisterClient(3);
    check(as.registerClient(0x82000103u, 0, &idx) == 0 && idx == 3, "A4c a freed slot is reused");
    for (size_t i = 1; i < 8; ++i) as.unregisterClient(i);
  }
  // ── A5: pacing (audio_system.cc WorkerThreadMain: 5333 us deadline per client) ──────────
  {
    as.resetForTest();
    as.setSinkFactoryForTest(A::makeNullSink);
    size_t idx = 0;
    as.registerClient(0x821209F0u, 0, &idx);
    std::vector<uint8_t> fr(A::kAudioFrameBytes);
    makeFrame(fr.data());
    auto inv = [&](uint32_t cb, uint32_t arg) {
      (void)cb; (void)arg;
      as.submitFrame(idx, fr.data());
      return true;
    };
    const A::AudioStats d = runWorkerFor(1000, inv);
    std::printf("      pacing: callbacks=%llu submits=%llu consumed=%llu misses=%llu (expect ~187.5/s)\n",
                (unsigned long long)d.callbacks, (unsigned long long)d.submits,
                (unsigned long long)d.consumed, (unsigned long long)d.creditMisses);
    check(d.callbacks >= 170 && d.callbacks <= 200, "A5a ~187.5 callbacks per second (256 samples @ 48 kHz)");
    check(d.submits == d.callbacks, "A5b every callback's frame reaches the sink");
    // + 3: frames in flight; + 3 more: SR_AUDIO_PREROLL keeps its 3 pre-rolled frames queued.
    check(d.consumed + 6 >= d.callbacks && d.creditMisses == 0,
          "A5c the null sink consumes at the same rate: credits never run out");
    as.unregisterClient(idx);
  }
  // ── A6: back-pressure: a sink that never consumes stops the callbacks at 64 ─────────────
  {
    as.resetForTest();
    as.setSinkFactoryForTest([&](size_t i, std::function<void()> c) -> std::unique_ptr<A::AudioSink> {
      auto s = std::make_unique<RecSink>(std::move(c));
      rec[i] = s.get();
      return s;
    });
    size_t idx = 0;
    as.registerClient(0x821209F0u, 0, &idx);
    std::vector<uint8_t> fr(A::kAudioFrameBytes, 0);
    auto inv = [&](uint32_t, uint32_t) { as.submitFrame(idx, fr.data()); return true; };
    const A::AudioStats d = runWorkerFor(600, inv);   // ~112 deadlines
    check(d.callbacks == 64 && d.creditMisses > 20,
          "A6 no consumption -> exactly 64 callbacks, then the deadline passes with no credit");
    check(as.creditsAvailable(idx) == 0, "A6b credits exhausted");
    // One consumption returns one credit.
    rec[idx]->consumed();
    check(as.creditsAvailable(idx) == 1, "A6c a consumed frame returns exactly one credit");
    // ── A7: unregister drains ───────────────────────────────────────────────────────────
    as.unregisterClient(idx);
    check(as.creditsAvailable(idx) == 0, "A7a unregister drains the credits");
    const A::AudioStats d2 = runWorkerFor(100, inv);
    check(d2.callbacks == 0, "A7b no callbacks after unregister");
  }
  // ── A8/A9: exports through the registry ───────────────────────────────────────────────────
  {
    as.resetForTest();
    as.setSinkFactoryForTest([&](size_t i, std::function<void()> c) -> std::unique_ptr<A::AudioSink> {
      auto s = std::make_unique<RecSink>(std::move(c));
      rec[i] = s.get();
      return s;
    });
    K::writeGuestBE32(kDriverOut, 0);
    as.registerClientExport(kClientStruct, kDriverOut);
    const uint32_t h = K::readGuestU32BE(kDriverOut);
    makeFrame(K::guestVAToHost(kFrameVA));
    const uint32_t st = call("XAudioSubmitRenderDriverFrame", h, kFrameVA);
    const bool same = rec[0] && rec[0]->frames.size() == 1 &&
                      std::memcmp(rec[0]->frames[0].data(), K::guestVAToHost(kFrameVA), A::kAudioFrameBytes) == 0;
    check(st == 0 && same, "A8 XAudioSubmitRenderDriverFrame export hands the exact 6144 guest bytes to the client");
    const uint64_t inv0 = as.stats().invalidSubmits;
    check(call("XAudioSubmitRenderDriverFrame", 0x41550005u, kFrameVA) == 0 &&
              as.stats().invalidSubmits == inv0 + 1,
          "A8b submit on an unregistered handle returns 0 and is counted (no crash)");
    K::writeGuestBE32(kVolOut, 0xDEADBEEFu);
    check(call("XAudioGetVoiceCategoryVolume", 0, kVolOut) == 0 && K::readGuestU32BE(kVolOut) == 0x3F800000u,
          "A9a XAudioGetVoiceCategoryVolume writes 1.0f");
    K::writeGuestBE32(kVolOut, 0xDEADBEEFu);
    check(call("XAudioGetVoiceCategoryVolumeChangeMask", h, kVolOut) == 0 && K::readGuestU32BE(kVolOut) == 0u,
          "A9b XAudioGetVoiceCategoryVolumeChangeMask writes 0");
    // ── A11: a callback that does not submit is named and keeps its credit ──────────────
    const uint32_t c0 = as.creditsAvailable(0);
    auto invNo = [&](uint32_t, uint32_t) { return true; };
    const A::AudioStats d = runWorkerFor(100, invNo);
    check(d.callbacks > 5 && d.noSubmitCallbacks == d.callbacks && as.creditsAvailable(0) == c0 - d.callbacks,
          "A11 [AUDIO_CB_NOSUBMIT]: every non-submitting callback is counted and its credit stays taken");
    // ── A12: unregister while a callback runs (critique A2 lock order) ──────────────────
    std::atomic<bool> inCb{false};
    auto invSlow = [&](uint32_t, uint32_t) {
      inCb = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
      as.submitFrame(0, K::guestVAToHost(kFrameVA));
      return true;
    };
    std::thread w([&] { as.runWorker(invSlow); });
    while (!inCb) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto t0 = std::chrono::steady_clock::now();
    std::thread u([&] { as.unregisterClient(0); });
    u.join();
    const double uMs = msSince(t0);
    as.stopWorker();
    w.join();
    check(uMs < 2000.0, "A12 unregister during a running callback completes (no lock-order deadlock)");
  }
  // ── A10: the WAV dump through the real null sink ──────────────────────────────────────────
  {
    as.resetForTest();
    as.setSinkFactoryForTest(A::makeNullSink);
    const std::filesystem::path wav =
        std::filesystem::temp_directory_path() / "sr_audio_out_test.wav";
    std::error_code ec;
    std::filesystem::remove(wav, ec);
    const std::string env = "SR_AUDIO_WAV=" + wav.string();
    _putenv(env.c_str());
    size_t idx = 0;
    as.registerClient(0x821209F0u, 0, &idx);
    std::vector<uint8_t> fr(A::kAudioFrameBytes);
    makeFrame(fr.data());
    auto inv = [&](uint32_t, uint32_t) { as.submitFrame(idx, fr.data()); return true; };
    runWorkerFor(300, inv);
    as.unregisterClient(idx);
    _putenv("SR_AUDIO_WAV=");
    FILE* f = std::fopen(wav.string().c_str(), "rb");
    uint8_t hdr[44] = {};
    size_t n = f ? std::fread(hdr, 1, 44, f) : 0;
    if (f) std::fclose(f);
    uint32_t dataBytes = 0;
    std::memcpy(&dataBytes, hdr + 40, 4);
    uint16_t fmt = 0, ch = 0;
    std::memcpy(&fmt, hdr + 20, 2);
    std::memcpy(&ch, hdr + 22, 2);
    const auto fsz = fsize(wav);
    std::printf("      wav: %s data=%u bytes file=%llu\n", wav.string().c_str(), dataBytes,
                (unsigned long long)fsz);
    check(n == 44 && std::memcmp(hdr, "RIFF", 4) == 0 && fmt == 3 && ch == 2 && dataBytes > 0 &&
              uint64_t(dataBytes) + 44 == fsz,
          "A10 SR_AUDIO_WAV: a valid float WAV of what the sink played");
    std::filesystem::remove(wav, ec);
  }
  // ── A13: the real SDL sink, SILENT (SDL disk driver into a temp file - never a speaker) ────
  {
    as.resetForTest();
    as.setSinkFactoryForTest(nullptr);   // default factory: SDL, null on failure
    const auto tmp = std::filesystem::temp_directory_path();
    const std::string raw = (tmp / "sr_audio_out_test_sdl.raw").string();
    const std::string wav = (tmp / "sr_audio_out_test_sdl.wav").string();
    std::error_code ec;
    std::filesystem::remove(raw, ec);
    std::filesystem::remove(wav, ec);
    diskDriver(raw);
    _putenv(("SR_AUDIO_WAV=" + wav).c_str());
    _putenv("SR_AUDIO_SINK=");
    size_t idx = 0;
    const uint32_t r = as.registerClient(0x821209F0u, 0, &idx);
    std::vector<uint8_t> fr(A::kAudioFrameBytes);
    makeFrame(fr.data());
    auto inv = [&](uint32_t, uint32_t) { as.submitFrame(idx, fr.data()); return true; };
    const A::AudioStats d = runWorkerFor(700, inv);
    as.unregisterClient(idx);
    _putenv("SR_AUDIO_WAV=");
    const auto rawSz = fsize(raw);
    const auto wavSz = fsize(wav);
    std::printf("      sdl(disk): callbacks=%llu consumed=%llu raw=%llu wav=%llu\n",
                (unsigned long long)d.callbacks, (unsigned long long)d.consumed,
                (unsigned long long)rawSz, (unsigned long long)wavSz);
    check(r == 0 && d.consumed >= 20 && rawSz > 0 && wavSz > 44,
          "A13 SDL sink (SDL_AUDIO_DRIVER=disk): the device pulls frames, credits flow, WAV + raw written");
    std::filesystem::remove(raw, ec);
    std::filesystem::remove(wav, ec);
  }
  // ── A14/A15 (iter-467): the device guard, SILENT (disk driver). A device that stops pulling
  // must not stop the guest's audio clock; a STOPPED device is re-opened. ────────────────────
  {
    const auto tmp = std::filesystem::temp_directory_path();
    const std::string raw = (tmp / "sr_audio_out_test_guard.raw").string();
    std::error_code ec;
    diskDriver(raw);
    _putenv("SR_AUDIO_SINK=");
    std::vector<uint8_t> fr(A::kAudioFrameBytes);
    makeFrame(fr.data());
    // A device that pulls one buffer every 600 ms = a stalled host device. SDL3's disk driver waits
    // (480 * 1000 / 48000 = 10 ms) x SDL_AUDIO_DISK_TIMESCALE per 480-frame buffer (SDL2: SDL_DISKAUDIODELAY=600).
    const std::filesystem::path cwd0 = std::filesystem::current_path(ec);
    std::filesystem::current_path(tmp, ec);
    auto stalled = [&](int guard) {
      setEnv("SDL_AUDIO_DISK_TIMESCALE", "60");
      A::audioDeviceGuardSetForTest(guard);
      as.resetForTest();
      as.setSinkFactoryForTest(nullptr);
      size_t idx = 0;
      as.registerClient(0x821209F0u, 0, &idx);
      auto inv = [&](uint32_t, uint32_t) { as.submitFrame(idx, fr.data()); return true; };
      const A::AudioStats d = runWorkerFor(2000, inv);
      as.unregisterClient(idx);
      setEnv("SDL_AUDIO_DISK_TIMESCALE", "");
      return d;
    };
    const A::AudioDeviceGuardStats g0 = A::audioDeviceGuardStats();
    const A::AudioStats on = stalled(1);
    const A::AudioDeviceGuardStats g1 = A::audioDeviceGuardStats();
    const A::AudioStats off = stalled(0);
    std::filesystem::current_path(cwd0, ec);
    std::printf("      guard: stalled device 2 s: guard ON callbacks=%llu consumed=%llu bridged=%llu | "
                "guard OFF callbacks=%llu consumed=%llu\n",
                (unsigned long long)on.callbacks, (unsigned long long)on.consumed,
                (unsigned long long)(g1.bridgedFrames - g0.bridgedFrames),
                (unsigned long long)off.callbacks, (unsigned long long)off.consumed);
    check(on.callbacks >= 300 && g1.bridges > g0.bridges,
          "A14a guard ON: a device that stops pulling -> console-rate bridge, ~187.5 guest callbacks/s go on");
    check(off.callbacks <= 64 + 20,
          "A14b guard OFF (mutation control): the same stalled device stops the guest callbacks at the 64 credits");
    // A15: a device reported STOPPED is re-opened and pulls again.
    A::audioDeviceGuardSetForTest(1);
    as.resetForTest();
    as.setSinkFactoryForTest(nullptr);
    size_t idx = 0;
    as.registerClient(0x821209F0u, 0, &idx);
    auto inv = [&](uint32_t, uint32_t) { as.submitFrame(idx, fr.data()); return true; };
    const A::AudioDeviceGuardStats r0 = A::audioDeviceGuardStats();
    A::audioDeviceForceStoppedForTest();
    const A::AudioStats d = runWorkerFor(1200, inv);
    const A::AudioDeviceGuardStats r1 = A::audioDeviceGuardStats();
    as.unregisterClient(idx);
    std::printf("      guard: forced STOPPED: reopens=%llu callbacks=%llu consumed=%llu\n",
                (unsigned long long)(r1.reopens - r0.reopens), (unsigned long long)d.callbacks,
                (unsigned long long)d.consumed);
    check(r1.reopens == r0.reopens + 1 && d.callbacks >= 150 && d.consumed + 70 >= d.callbacks,
          "A15 a STOPPED device is re-opened once and the new device pulls (credits flow)");
    // A20 (lane sdl3-audio): SDL3 says "lost" with SDL_EVENT_AUDIO_DEVICE_REMOVED for our logical device. The
    // sink's event watch runs inside SDL_PushEvent, so a pushed event takes the real path from SDL's event
    // queue to the guard. Control: the same event for another device id re-opens nothing.
    {
      as.resetForTest();
      as.setSinkFactoryForTest(nullptr);
      size_t i2 = 0;
      as.registerClient(0x821209F0u, 0, &i2);
      const uint32_t dev = as.sinkInfoForTest(i2).hostDevice;
      auto inv2 = [&](uint32_t, uint32_t) { as.submitFrame(i2, fr.data()); return true; };
      auto pushRemoved = [](uint32_t which) {
        SDL_Event ev{};
        ev.type = SDL_EVENT_AUDIO_DEVICE_REMOVED;
        ev.adevice.which = which;
        ev.adevice.recording = false;
        return SDL_PushEvent(&ev);
      };
      const A::AudioDeviceGuardStats e0 = A::audioDeviceGuardStats();
      const bool pushedOther = pushRemoved(dev + 1000u);
      runWorkerFor(400, inv2);
      const A::AudioDeviceGuardStats e1 = A::audioDeviceGuardStats();
      const bool pushedMine = pushRemoved(dev);
      const A::AudioStats d2 = runWorkerFor(1200, inv2);
      const A::AudioDeviceGuardStats e2 = A::audioDeviceGuardStats();
      const uint32_t dev2 = as.sinkInfoForTest(i2).hostDevice;
      as.unregisterClient(i2);
      SDL_FlushEvents(SDL_EVENT_AUDIO_DEVICE_ADDED, SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED);
      std::printf("      sdl3 REMOVED event: device %u, other-id reopens=%llu, own-id reopens=%llu -> device %u, "
                  "callbacks=%llu consumed=%llu (pushed %d/%d)\n",
                  dev, (unsigned long long)(e1.reopens - e0.reopens), (unsigned long long)(e2.reopens - e1.reopens),
                  dev2, (unsigned long long)d2.callbacks, (unsigned long long)d2.consumed, pushedOther, pushedMine);
      check(dev != 0 && pushedMine && e2.reopens == e1.reopens + 1 && dev2 != 0 && dev2 != dev &&
                d2.consumed + 70 >= d2.callbacks && d2.callbacks >= 150,
            "A20 SDL_EVENT_AUDIO_DEVICE_REMOVED for our device -> the guard re-opens it and the new device pulls");
      check(pushedOther && e1.reopens == e0.reopens,
            "A20b control: REMOVED for another device id re-opens nothing");
    }
    A::audioDeviceGuardSetForTest(-1);
    std::filesystem::remove(raw, ec);
  }
  // A17 (iter-467): pre-roll. The client registers and the device opens 300 ms before the
  // guest's first frame (as at boot). Pre-roll ON: the device starts on the first 3 queued frames,
  // so nothing counts as an underrun; OFF (the control): the device pulls the empty queue first.
  {
    const auto tmp = std::filesystem::temp_directory_path();
    const std::string raw = (tmp / "sr_audio_out_test_preroll.raw").string();
    std::error_code ec;
    diskDriver(raw);
    _putenv("SR_AUDIO_SINK=");
    std::vector<uint8_t> fr(A::kAudioFrameBytes);
    makeFrame(fr.data());
    auto boot = [&](int mode, uint64_t* underruns, uint64_t* after) {
      A::audioPrerollSetForTest(mode);
      as.resetForTest();
      as.setSinkFactoryForTest(nullptr);
      size_t idx = 0;
      as.registerClient(0x821209F0u, 0, &idx);
      const uint64_t u0 = as.stats().underruns;
      std::this_thread::sleep_for(std::chrono::milliseconds(300));   // device open, guest silent
      const uint64_t u1 = as.stats().underruns;
      *underruns = u1 - u0;                                           // before the first frame
      auto inv = [&](uint32_t, uint32_t) { as.submitFrame(idx, fr.data()); return true; };
      const A::AudioStats d = runWorkerFor(1000, inv);
      *after = as.stats().underruns - u1;                             // once frames flow
      as.unregisterClient(idx);
      return d;
    };
    uint64_t uOn = 0, uOff = 0, aOn = 0, aOff = 0;
    const A::AudioStats on = boot(1, &uOn, &aOn);
    const A::AudioStats off = boot(0, &uOff, &aOff);
    A::audioPrerollSetForTest(-1);
    std::filesystem::remove(raw, ec);
    std::printf("      pre-roll: ON underruns before/after the first frame=%llu/%llu callbacks=%llu consumed=%llu | "
                "OFF %llu/%llu callbacks=%llu\n",
                (unsigned long long)uOn, (unsigned long long)aOn, (unsigned long long)on.callbacks,
                (unsigned long long)on.consumed, (unsigned long long)uOff, (unsigned long long)aOff,
                (unsigned long long)off.callbacks);
    // Once frames flow, a worker made late by host load can still starve the device once or twice.
    // Frames flow: the device consumed at least ~half the console rate and the guest ran ahead of it by no more than
    // the credits. The rate itself is the device's: the PC's disk driver ~179 frames/s, the VM's ~122 (2026-10-18:
    // its disk writes are slow, MsMpEng busy) - an absolute 150 failed there with 0 underruns, i.e. on the host.
    check(uOn == 0 && aOn <= 2 && on.consumed >= 90 && on.callbacks >= on.consumed &&
              on.callbacks <= on.consumed + A::kAudioMaxQueuedFrames + 2,
          "A17a SR_AUDIO_PREROLL: the stream starts on the guest's first frames - 0 underruns, frames flow");
    check(uOff > 0, "A17b pre-roll OFF (mutation control): the device pulls the empty queue before the first frame");
  }
  // ── lane sdl3-audio (RELEASE_PLAN step 3): the SDL3 environment, the channel rule, the replay ──────
  // A18: a harness that still sets only the SDL2 names (SDL_AUDIODRIVER / SDL_DISKAUDIOFILE) gets the disk
  // driver AND its file (prepareSdlAudioEnv). Control: SDL3 alone ignores SDL_DISKAUDIOFILE (sdlaudio.raw in
  // the working directory instead) - the mapping is what routes the file.
  {
    namespace fs = std::filesystem;
    const auto tmp = fs::temp_directory_path();
    const std::string raw = (tmp / "sr_audio_out_test_envmap.raw").string();
    std::error_code ec;
    fs::remove(raw, ec);
    setEnv("SDL_AUDIO_DRIVER", "");
    setEnv("SDL_AUDIO_DISK_OUTPUT_FILE", "");
    SDL_ResetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE);
    setEnv("SDL_AUDIODRIVER", "disk");
    setEnv("SDL_DISKAUDIOFILE", raw);
    _putenv("SR_AUDIO_SINK=");
    A::prepareSdlAudioEnvResetLogForTest();
    as.resetForTest();
    as.setSinkFactoryForTest(nullptr);
    size_t idx = 0;
    as.registerClient(0x821209F0u, 0, &idx);
    const A::AudioSystem::SinkInfo info = as.sinkInfoForTest(idx);
    const char* drv = SDL_GetCurrentAudioDriver();
    const std::string drvS = drv ? drv : "";
    std::vector<uint8_t> fr(A::kAudioFrameBytes);
    makeFrame(fr.data());
    auto inv = [&](uint32_t, uint32_t) { as.submitFrame(idx, fr.data()); return true; };
    const A::AudioStats d = runWorkerFor(400, inv);
    as.unregisterClient(idx);
    const A::SdlAudioEnvReport rep = A::prepareSdlAudioEnv();
    const auto rawSz = fsize(raw);
    std::printf("      env map: SDL2 names only -> sink=%s driver=%s consumed=%llu file=%llu B mappedDriver=%d mappedFile=%d\n",
                info.kind ? info.kind : "-", drvS.c_str(), (unsigned long long)d.consumed, (unsigned long long)rawSz,
                rep.mappedDriver, rep.mappedFile);
    check(info.kind && std::strcmp(info.kind, "sdl") == 0 && drvS == "disk" && rawSz > 0 && d.consumed > 20 &&
              rep.mappedDriver && rep.mappedFile,
          "A18 SDL2 env names only: the SDL3 sink plays to the disk driver and writes the SDL_DISKAUDIOFILE file");
    // Control, raw SDL3 with the hint reset (the mapping undone), from an empty working directory.
    SDL_ResetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE);
    const fs::path ctl = tmp / "sr_audio_out_test_envmap_ctl";
    fs::remove_all(ctl, ec);
    fs::create_directories(ctl, ec);
    const fs::path cwd0 = fs::current_path(ec);
    fs::current_path(ctl, ec);
    bool ctlOpened = false;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      SDL_AudioSpec sp{};
      sp.format = SDL_AUDIO_F32;
      sp.channels = 2;
      sp.freq = 48000;
      if (SDL_AudioStream* st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &sp, nullptr, nullptr)) {
        ctlOpened = true;
        std::vector<float> sil(4096 * 2, 0.0f);
        SDL_PutAudioStreamData(st, sil.data(), int(sil.size() * sizeof(float)));
        SDL_ResumeAudioStreamDevice(st);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        SDL_DestroyAudioStream(st);
      }
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    fs::current_path(cwd0, ec);
    const auto ctlSz = fsize(ctl / "sdlaudio.raw");
    const auto rawSz2 = fsize(raw);
    std::printf("      env map control (hint reset): cwd sdlaudio.raw=%llu B, SDL_DISKAUDIOFILE file %llu -> %llu B\n",
                (unsigned long long)ctlSz, (unsigned long long)rawSz, (unsigned long long)rawSz2);
    check(ctlOpened && ctlSz > 0 && rawSz > 0 && rawSz2 == rawSz,
          "A18b control: SDL3 without the mapping ignores SDL_DISKAUDIOFILE (writes sdlaudio.raw in the cwd)");
    fs::remove_all(ctl, ec);
    fs::remove(raw, ec);
    setEnv("SDL_AUDIODRIVER", "");
    setEnv("SDL_DISKAUDIOFILE", "");
  }
  // A19: a harness run (SR_INSTANCE_ID) with no audio driver named is reported (it would open the real
  // default device); a driver by either name, the null sink, or not being a harness run is not.
  {
    const char* inst0 = std::getenv("SR_INSTANCE_ID");
    const std::string instSaved = inst0 ? inst0 : "";
    setEnv("SDL_AUDIO_DRIVER", "");
    setEnv("SDL_AUDIODRIVER", "");
    SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
    _putenv("SR_AUDIO_SINK=");
    _putenv("SR_AUDIO_DEVICE=");
    _putenv("SR_INSTANCE_ID=a19");
    const bool none = A::prepareSdlAudioEnv().harnessRealDevice;
    setEnv("SDL_AUDIODRIVER", "disk");
    const bool old = A::prepareSdlAudioEnv().harnessRealDevice;
    setEnv("SDL_AUDIODRIVER", "");
    setEnv("SDL_AUDIO_DRIVER", "disk");
    const bool neu = A::prepareSdlAudioEnv().harnessRealDevice;
    setEnv("SDL_AUDIO_DRIVER", "");
    _putenv("SR_AUDIO_SINK=null");
    const bool nul = A::prepareSdlAudioEnv().harnessRealDevice;
    _putenv("SR_AUDIO_SINK=");
    _putenv("SR_INSTANCE_ID=");
    const bool notHarness = A::prepareSdlAudioEnv().harnessRealDevice;
    _putenv(("SR_INSTANCE_ID=" + instSaved).c_str());
    std::printf("      harness check: no driver=%d SDL2 name=%d SDL3 name=%d null sink=%d no SR_INSTANCE_ID=%d\n", none,
                old, neu, nul, notHarness);
    check(none && !old && !neu && !nul && !notHarness,
          "A19 a harness run with no audio driver is reported (ERROR [AUDIO_ENV]); any driver name / null sink is not");
  }
  // A21: the open rule. SDL2 kept a 2 or 6 ch device and converted from 6 otherwise; SDL3 the same: a
  // stereo device gets the 2 ch Xenia downmix, every other device 6 ch frames for SDL to convert.
  {
    const auto tmp = std::filesystem::temp_directory_path();
    const std::string raw = (tmp / "sr_audio_out_test_rule.raw").string();
    std::error_code ec;
    diskDriver(raw);
    _putenv("SR_AUDIO_SINK=");
    std::vector<uint8_t> fr(A::kAudioFrameBytes);
    makeFrame(fr.data());
    struct Want { int req, in, dev; };
    const Want wants[] = {{1, 2, 2}, {2, 2, 2}, {4, 6, 4}, {6, 6, 6}, {8, 6, 8}};
    bool all = true;
    for (const Want& w : wants) {
      _putenv(("SR_AUDIO_TEST_DEVICE_CH=" + std::to_string(w.req)).c_str());
      as.resetForTest();
      as.setSinkFactoryForTest(nullptr);
      size_t idx = 0;
      as.registerClient(0x821209F0u, 0, &idx);
      const A::AudioSystem::SinkInfo info = as.sinkInfoForTest(idx);
      auto inv = [&](uint32_t, uint32_t) { as.submitFrame(idx, fr.data()); return true; };
      const A::AudioStats d = runWorkerFor(250, inv);
      as.unregisterClient(idx);
      const bool ok = info.kind && info.channels == w.in && info.hostChannels == w.dev && d.consumed > 10;
      all = all && ok;
      std::printf("      rule: device asked %d ch -> device %d ch, input %d ch, consumed %llu %s\n", w.req,
                  info.hostChannels, info.channels, (unsigned long long)d.consumed, ok ? "" : "<- WRONG");
    }
    _putenv("SR_AUDIO_TEST_DEVICE_CH=");
    std::filesystem::remove(raw, ec);
    check(all, "A21 open rule: a stereo device gets the 2 ch downmix, 1 / 4 / 6 / 8 ch devices the 6 ch frames");
  }
  // A22: the replay harness on a synthetic signal: 6 ch and 2 ch devices play the converted frames bit for
  // bit (device bytes and the WAV observer). Mutants (must FAIL): an FC <-> LFE input channel map, and SDL's
  // own 6 -> 2 downmix in place of the Xenia one.
  {
    const auto tmp = std::filesystem::temp_directory_path() / "sr_audio_replay";
    const std::vector<Frame> syn = syntheticFrames(400);
    const ReplayResult r6 = replayThroughSdl(syn, 6, (tmp / "c6").string(), nullptr, false);
    printReplay("replay 6 ch", r6);
    const ReplayResult r2 = replayThroughSdl(syn, 2, (tmp / "c2").string(), nullptr, false);
    printReplay("replay 2 ch", r2);
    const ReplayResult m1 = replayThroughSdl(syn, 6, (tmp / "m_chanmap").string(), "chanmap", false);
    printReplay("mutant chanmap", m1);
    const ReplayResult m2 = replayThroughSdl(syn, 2, (tmp / "m_downmix").string(), "sdl_downmix", false);
    printReplay("mutant sdl_downmix", m2);
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    check(r6.ok && r6.inCh == 6 && r6.hwCh == 6, "A22 replay 6 ch: the device plays the frames bit for bit (WAV + device bytes)");
    check(r2.ok && r2.inCh == 2 && r2.hwCh == 2, "A22b replay 2 ch: the device plays the Xenia downmix bit for bit");
    check(!m1.ok && m1.wavExact && !m1.diskExact,
          "A22m1 mutant FC<->LFE channel map: the device bytes differ (the WAV before SDL still matches) - detected");
    check(!m2.ok && m2.inCh == 6 && m2.hwCh == 2,
          "A22m2 mutant SDL 6->2 downmix instead of Xenia's: not the expected path - detected");
  }
  // A23 (Astra P1): a frame half-played by a 6 ch device pull, finished by the bridge's 2 ch pull, then a 6 ch pull
  // from mid-frame: each part is the right conversion of the same guest frame. Mutant: the reconversion off.
  {
    char rep[160], repM[160];
    const bool ok = A::queuedSinkContinuitySelfTest(false, rep, sizeof(rep));
    const bool okM = A::queuedSinkContinuitySelfTest(true, repM, sizeof(repM));
    std::printf("      continuity: %s | %s\n", rep, repM);
    check(ok, "A23 partial frame across 6 ch -> 2 ch (bridge) -> 6 ch pulls: every sample the right conversion");
    check(!okM, "A23m mutant (no reconversion): the wrong-stride samples are detected");
  }
  // A24 (lane E E0): the output-device choice rule (audio_system.hpp chooseAudioDevice); nothing is opened.
  // The safety rule: an automated run never plays to the owner's real devices (only "Steam Streaming Speakers").
  {
    const std::vector<std::string> listed = {"Speakers (Realtek(R) Audio)", "Speakers (Steam Streaming Speakers)",
                                             "Headphones (Arctis Nova Pro Wireless)"};
    const std::string steam = listed[1], arctis = listed[2];
    auto pick = [&](const std::string& stored, const std::string& env, bool harness, bool real,
                    const std::string& allow = "") {
      A::AudioDeviceRequest r;
      r.stored = stored;
      r.env = env;
      r.harness = harness;
      r.realDriver = real;
      r.allow = allow;
      return A::chooseAudioDevice(r, listed);
    };
    const A::AudioDeviceChoice a = pick(arctis, "zz_no_such_endpoint", true, true);
    check(a.refuse && a.device.empty(),
          "A24a SR_AUDIO_DEVICE matches nothing + a stored device -> refused (null sink); the stored device is NOT used");
    const A::AudioDeviceChoice a2 = pick(arctis, "zz_no_such_endpoint", false, false);
    check(a2.refuse && a2.device.empty(), "A24b the same outside a harness run on a silent driver -> still refused");
    const A::AudioDeviceChoice b = pick(arctis, "", true, false);
    check(!b.refuse && b.device.empty() && std::strcmp(b.source, "harness-default") == 0,
          "A24c a harness run on the disk driver: the default device, never the stored one");
    const A::AudioDeviceChoice c1 = pick(arctis, "", true, true);
    const A::AudioDeviceChoice c2 = pick(arctis, "", true, true, "*");
    check(c1.refuse && c1.device.empty() && !c2.refuse && c2.device.empty(),
          "A24d a harness run on a real driver: the system default is refused; SR_AUDIO_DEVICE_ALLOW=* allows it");
    const A::AudioDeviceChoice d = pick(arctis, "steam streaming", true, true);
    check(!d.refuse && d.device == steam && d.exclusive && std::strcmp(d.source, "SR_AUDIO_DEVICE") == 0,
          "A24e SR_AUDIO_DEVICE=steam streaming on a real driver -> that endpoint, exclusive (no default fall-back)");
    const A::AudioDeviceChoice e1 = pick("", "arctis", true, true);
    const A::AudioDeviceChoice e2 = pick("", "arctis", true, true, "arctis");
    const A::AudioDeviceChoice e3 = pick("", "arctis", false, true);
    check(e1.refuse && e3.refuse && !e2.refuse && e2.device == arctis && e2.exclusive,
          "A24f SR_AUDIO_DEVICE=<a real headset> on a real driver: refused in and outside a harness run; allowed only "
          "when SR_AUDIO_DEVICE_ALLOW names it");
    const A::AudioDeviceChoice f = pick(arctis, "", false, true);
    const A::AudioDeviceChoice g = pick("", "", false, true);
    check(!f.refuse && f.device == arctis && std::strcmp(f.source, "stored") == 0 && !f.exclusive && !g.refuse &&
              g.device.empty() && std::strcmp(g.source, "default") == 0,
          "A24g a player run: the stored device (may fall back to the default), else the default - unchanged");
    check(A::audioDeviceAllowed(steam, "") && !A::audioDeviceAllowed(arctis, "") && !A::audioDeviceAllowed("", "") &&
              A::audioDeviceAllowed("Foo Bar", "a; bar ;b") && A::audioDeviceAllowed("", " * ") &&
              !A::audioDeviceAllowed("x", ";;") && A::audioDeviceAllowed("SPEAKERS (STEAM STREAMING SPEAKERS)", ""),
          "A24h the allowlist: default 'Steam Streaming Speakers', ';' entries trimmed, case-insensitive, '*' = any");
    check(A::audioDriverIsReal("wasapi") && A::audioDriverIsReal("directsound") && A::audioDriverIsReal(nullptr) &&
              !A::audioDriverIsReal("disk") && !A::audioDriverIsReal("dummy"),
          "A24i real drivers: every SDL driver but disk / dummy (unknown = real)");
  }
  // A25 (lane E E0): the rule's WIRING through the real SDL sink, SILENT (the disk driver; SR_AUDIO_TEST_REAL_DRIVER=1
  // makes the choice treat it as a real driver, so the allowlist guard runs without any real device). The stored
  // PC SETTINGS device = the disk driver's own device, in a temp SR_CONTENT_DIR's settings/display.ini. Mutants
  // (SR_AUDIO_TEST_MUTANT, each must FAIL its case): e0_keep_stored, e0_harness_stored, e0_no_guard.
  {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path() / "sr_audio_e0";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "content" / "settings", ec);
    diskDriver((tmp / "e0.raw").string());
    _putenv("SR_AUDIO_SINK=");
    std::string dev0;   // the disk driver's playback device as SDL lists it
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      int n = 0;
      if (SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n)) {
        if (n > 0)
          if (const char* nm = SDL_GetAudioDeviceName(ids[0])) dev0 = nm;
        SDL_free(ids);
      }
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    auto saved = [](const char* k) {
      const char* v = std::getenv(k);
      return std::string(v ? v : "");
    };
    const std::string inst0 = saved("SR_INSTANCE_ID"), pf0 = saved("SR_PORT_FEATURES"), cd0 = saved("SR_CONTENT_DIR"),
                      dv0 = saved("SR_AUDIO_DEVICE"), al0 = saved("SR_AUDIO_DEVICE_ALLOW");
    const fs::path croot = tmp / "content";
    _putenv(("SR_CONTENT_DIR=" + croot.string()).c_str());
    _wputenv((L"SR_CONTENT_DIR=" + croot.wstring()).c_str());   // xamContentRoot reads the wide environment
    { std::ofstream(croot / "settings" / "display.ini") << "audio_device=" << dev0 << "\n"; }
    struct Got { std::string kind, device, source; };
    auto openOnce = [&](const char* inst, const std::string& env, const char* mutant, bool fakeReal,
                        const std::string& allow) {
      _putenv((std::string("SR_INSTANCE_ID=") + (inst ? inst : "")).c_str());
      _putenv("SR_PORT_FEATURES=");
      _putenv(("SR_AUDIO_DEVICE=" + env).c_str());
      _putenv(("SR_AUDIO_DEVICE_ALLOW=" + allow).c_str());
      _putenv((std::string("SR_AUDIO_TEST_MUTANT=") + (mutant ? mutant : "")).c_str());
      _putenv(fakeReal ? "SR_AUDIO_TEST_REAL_DRIVER=1" : "SR_AUDIO_TEST_REAL_DRIVER=");
      as.resetForTest();
      as.setSinkFactoryForTest(nullptr);
      Got got;
      size_t idx = 0;
      if (as.registerClient(0x821209F0u, 0, &idx) == 0) {
        const A::AudioSystem::SinkInfo info = as.sinkInfoForTest(idx);
        got.kind = info.kind ? info.kind : "";
        got.device = info.deviceName;
        got.source = info.source ? info.source : "";
        as.unregisterClient(idx);
      }
      std::printf("      e0: inst=%s env='%s' mutant=%s real=%d allow='%s' -> sink=%s device='%s' source=%s\n",
                  inst ? inst : "-", env.c_str(), mutant ? mutant : "-", fakeReal ? 1 : 0, allow.c_str(),
                  got.kind.c_str(), got.device.c_str(), got.source.c_str());
      return got;
    };
    const char* kNo = "zz_no_such_endpoint_e0";
    check(!dev0.empty(), "A25 precondition: the disk driver lists a playback device (the stored device of the cases)");
    // I3 first (the control): a player run opens the stored device, so the stored setting IS read in this process
    // (without it the cases below would pass vacuously).
    const Got i3 = openOnce(nullptr, "", nullptr, false, "");
    check(i3.kind == "sdl" && i3.device == dev0 && i3.source == "stored",
          "A25a control: a player run opens the stored PC SETTINGS device (the setting is read)");
    const Got i1 = openOnce(nullptr, kNo, nullptr, false, "");
    const Got i1m = openOnce(nullptr, kNo, "e0_keep_stored", false, "");
    check(i1.kind == "null", "A25b SR_AUDIO_DEVICE matches nothing, a device is stored -> the null sink (the bug: it opened)");
    check(i1m.kind == "sdl" && i1m.device == dev0, "A25b-m mutant e0_keep_stored (the pre-E0 rule) opens the stored device - detected");
    const Got i2 = openOnce("e0i2", "", nullptr, false, "");
    const Got i2m = openOnce("e0i2", "", "e0_harness_stored", false, "");
    check(i2.kind == "sdl" && i2.device.empty() && i2.source == "harness-default",
          "A25c a harness run (SR_INSTANCE_ID) opens the default device, never the stored one");
    check(i2m.kind == "sdl" && i2m.device == dev0, "A25c-m mutant e0_harness_stored opens the stored device - detected");
    const Got i5 = openOnce("e0i5", "", nullptr, true, "");
    const Got i5m = openOnce("e0i5", "", "e0_no_guard", true, "");
    const Got i5a = openOnce("e0i5", "", nullptr, true, "*");
    check(i5.kind == "null" && i5a.kind == "sdl" && i5a.device.empty(),
          "A25d a harness run on a (stand-in) real driver: the system default is refused -> null sink; allow=* opens it");
    check(i5m.kind == "sdl", "A25d-m mutant e0_no_guard opens the system default - detected");
    const Got i4 = openOnce("e0i4", dev0, nullptr, true, "");
    const Got i4a = openOnce("e0i4", dev0, nullptr, true, dev0);
    check(i4.kind == "null" && i4a.kind == "sdl" && i4a.device == dev0 && i4a.source == "SR_AUDIO_DEVICE",
          "A25e SR_AUDIO_DEVICE=<a device off the allowlist> on a real driver -> null sink; on the allowlist -> opened");
    // A26 (lane E E0, Astra r1 P1): the legacy driver's device open (SR_AUDIO_OUT=0) under the same rule. It never
    // read the stored device; the hole was its guard testing only SR_INSTANCE_ID, so an SR_AUDIO_DEVICE-only run
    // opened the system default. Returns the opened device ("default" = the system default; "" = nothing opened).
    // Mutant e0_legacy_noguard (the device rule skipped: always the default) must fail A26b (A26b-m). A26e (the
    // SR_AUDIO_SINK=null check, before the rule) is shown failing by the literal-revert build.
    const std::string sink0 = saved("SR_AUDIO_SINK");
    auto legacyOnce = [&](const char* inst, const std::string& env, const char* mutant, bool fakeReal,
                          const std::string& allow, bool sinkNull) {
      _putenv((std::string("SR_INSTANCE_ID=") + (inst ? inst : "")).c_str());
      _putenv(("SR_AUDIO_DEVICE=" + env).c_str());
      _putenv(("SR_AUDIO_DEVICE_ALLOW=" + allow).c_str());
      _putenv((std::string("SR_AUDIO_TEST_MUTANT=") + (mutant ? mutant : "")).c_str());
      _putenv(fakeReal ? "SR_AUDIO_TEST_REAL_DRIVER=1" : "SR_AUDIO_TEST_REAL_DRIVER=");
      _putenv(sinkNull ? "SR_AUDIO_SINK=null" : "SR_AUDIO_SINK=");
      const std::string got = K::legacyAudioOpenForTest();
      std::printf("      e0 legacy: inst=%s env='%s' mutant=%s real=%d allow='%s' sink=%s -> opened '%s'\n",
                  inst ? inst : "-", env.c_str(), mutant ? mutant : "-", fakeReal ? 1 : 0, allow.c_str(),
                  sinkNull ? "null" : "-", got.c_str());
      return got;
    };
    const std::string l0 = legacyOnce(nullptr, "", nullptr, false, "", false);
    check(l0 == "default", "A26a control: a legacy player run opens the system default (unchanged)");
    const std::string l1 = legacyOnce(nullptr, kNo, nullptr, false, "", false);
    const std::string l1m = legacyOnce(nullptr, kNo, "e0_legacy_noguard", false, "", false);
    check(l1.empty(), "A26b legacy, SR_AUDIO_DEVICE matches nothing, no SR_INSTANCE_ID -> no device (r1: the default)");
    check(l1m == "default", "A26b-m mutant e0_legacy_noguard opens the system default - detected");
    const std::string l2 = legacyOnce(nullptr, dev0, nullptr, true, "", false);
    const std::string l2a = legacyOnce(nullptr, dev0, nullptr, true, dev0, false);
    check(l2.empty() && l2a == dev0,
          "A26c legacy, SR_AUDIO_DEVICE=<a device off the allowlist> on a real driver, no SR_INSTANCE_ID -> no device; "
          "on the allowlist -> THAT device by id (not the default)");
    const std::string l3 = legacyOnce("e0l3", "", nullptr, true, "", false);
    const std::string l3a = legacyOnce("e0l3", "", nullptr, true, "*", false);
    const std::string l3d = legacyOnce("e0l3", "", nullptr, false, "", false);
    check(l3.empty() && l3a == "default" && l3d == "default",
          "A26d legacy harness run: the default is refused on a real driver (allow=* opens it); the disk driver opens it");
    const std::string l4 = legacyOnce(nullptr, "", nullptr, false, "", true);
    check(l4.empty(),
          "A26e legacy, SR_AUDIO_SINK=null -> no device (pre-E0 the legacy path ignored it and opened the default)");
    _putenv(("SR_AUDIO_SINK=" + sink0).c_str());
    _putenv(("SR_INSTANCE_ID=" + inst0).c_str());
    _putenv(("SR_PORT_FEATURES=" + pf0).c_str());
    _putenv(("SR_CONTENT_DIR=" + cd0).c_str());
    _wputenv((L"SR_CONTENT_DIR=" + fs::path(cd0).wstring()).c_str());
    _putenv(("SR_AUDIO_DEVICE=" + dv0).c_str());
    _putenv(("SR_AUDIO_DEVICE_ALLOW=" + al0).c_str());
    _putenv("SR_AUDIO_TEST_MUTANT=");
    _putenv("SR_AUDIO_TEST_REAL_DRIVER=");
    fs::remove_all(tmp, ec);
  }
  as.resetForTest();
  as.setSinkFactoryForTest(nullptr);
  A::audioOutSetEnabledForTest(-1);
  // A16 (iter-467 N6): the census is readable from code (the native meter) and the
  // [AUDIO_NATIVE] line prints the same struct. No guest code ran here, so every count is 0.
  {
    const sr::kernel::AudioNativeCensus c = sr::kernel::audioNativeCensus();
    char line[400];
    sr::kernel::audioNativeCensusLine(line, sizeof(line));
    char want[160];
    std::snprintf(want, sizeof(want), "guest_mmio_fns=%llu ", (unsigned long long)c.guest_mmio_fns);
    const bool keys = std::strstr(line, want) == line && std::strstr(line, " xma_regfile_writes=") &&
                      std::strstr(line, " xma_hal_voices=") && std::strstr(line, " xma_hal_rest=") &&
                      std::strstr(line, " reg600=not-run:");
    std::printf("      census: %s\n", line);
    check(keys && c.guest_mmio_fns == 0 && c.xma_hal_rest == 0 && c.xma_hal_voices == 0,
          "A16 audioNativeCensus() == the [AUDIO_NATIVE] line (stable keys), all 0 without guest code");
  }
  return finish("AUDIO_OUT_TEST");
}

// ── KeWaitForMultipleObjects ─────────────────────────────────────────────────────────────────
namespace {
constexpr uint32_t kEvA = 0x90800000u, kEvB = 0x90800020u, kEvN = 0x90800040u;
constexpr uint32_t kSem = 0x90800060u, kArr = 0x90800100u, kTo = 0x90800200u;
constexpr uint32_t kTimeout = 0x102u;

uint32_t relTimeout(int64_t ms) {
  const int64_t t = -ms * 10000;
  K::writeGuestBE32(kTo, uint32_t(uint64_t(t) >> 32));
  K::writeGuestBE32(kTo + 4, uint32_t(uint64_t(t)));
  return kTo;
}
void setArr(std::initializer_list<uint32_t> v) {
  uint32_t i = 0;
  for (uint32_t x : v) K::writeGuestBE32(kArr + 4 * i++, x);
}
uint32_t st(uint32_t va) { return K::readGuestU32BE(va + 4); }
uint32_t waitMulti(uint32_t n, uint32_t type, uint32_t timeoutVA) {
  return call("KeWaitForMultipleObjects", n, kArr, type, 3, 1, 0, timeoutVA, 0);
}
void initEv(uint32_t va, uint32_t sync, uint32_t state) { call("KeInitializeEvent", va, sync, state); }
}  // namespace

int runKeWaitMultiTest() {
  std::printf("[KE_WAITMULTI_TEST] KeWaitForMultipleObjects vs Xenia (SR_KE_WAITMULTI)\n");
  if (!std::getenv("SR_KE_WAITMULTI")) _putenv("SR_KE_WAITMULTI=1");
  // W13 needs the KeSetEvent atomicity fix; SR_KEVT_SET_ATOMIC=0 is its mutation check (fails).
  if (!std::getenv("SR_KEVT_SET_ATOMIC")) _putenv("SR_KEVT_SET_ATOMIC=1");
  std::printf("  gate SR_KE_WAITMULTI=%s\n", std::getenv("SR_KE_WAITMULTI"));
  g_base = K::getGuestBase();
  K::setGuestMemory(g_base, kGuestSize);
  K::registerStubs();
#ifdef _WIN32
  constexpr uint32_t kAny = 1, kAll = 0;
  // W1: WaitAny returns the signalled index and consumes it (auto-reset).
  initEv(kEvA, 1, 0);
  initEv(kEvB, 1, 1);
  setArr({kEvA, kEvB});
  check(waitMulti(2, kAny, relTimeout(1000)) == 1 && st(kEvB) == 0 && st(kEvA) == 0,
        "W1 WaitAny: B pre-signalled -> STATUS_WAIT_0+1, B consumed, A untouched");
  // W2: lowest index wins; only it is consumed.
  initEv(kEvA, 1, 1);
  initEv(kEvB, 1, 1);
  check(waitMulti(2, kAny, relTimeout(1000)) == 0 && st(kEvA) == 0 && st(kEvB) == 1,
        "W2 WaitAny: both signalled -> index 0, only A consumed");
  // W3: timeout.
  initEv(kEvA, 1, 0);
  initEv(kEvB, 1, 0);
  auto t0 = std::chrono::steady_clock::now();
  uint32_t r = waitMulti(2, kAny, relTimeout(60));
  double ms = msSince(t0);
  check(r == kTimeout && ms >= 40.0 && ms < 1000.0, "W3 WaitAny: nothing signalled -> STATUS_TIMEOUT after ~60 ms");
  // W4: woken by KeSetEvent from another thread.
  {
    std::thread s([] {
      K::setGuestMemory(g_base, kGuestSize);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      call("KeSetEvent", kEvB, 1, 0);
    });
    t0 = std::chrono::steady_clock::now();
    r = waitMulti(2, kAny, relTimeout(5000));
    ms = msSince(t0);
    s.join();
    check(r == 1 && ms >= 30.0 && st(kEvB) == 0, "W4 WaitAny: KeSetEvent(B) on another thread wakes it -> 1, consumed");
  }
  // W5: a notification event is not consumed.
  initEv(kEvN, 0, 1);
  setArr({kEvA, kEvN});
  check(waitMulti(2, kAny, relTimeout(1000)) == 1 && st(kEvN) == 1,
        "W5 WaitAny: notification event satisfies the wait and stays signalled");
  // W6: WaitAll with one missing times out and consumes nothing.
  initEv(kEvA, 1, 1);
  initEv(kEvB, 1, 0);
  setArr({kEvA, kEvB});
  check(waitMulti(2, kAll, relTimeout(50)) == kTimeout && st(kEvA) == 1,
        "W6 WaitAll: one unsignalled -> STATUS_TIMEOUT, the signalled one NOT consumed");
  // W7: WaitAll with both -> 0, both consumed.
  initEv(kEvB, 1, 1);
  check(waitMulti(2, kAll, relTimeout(1000)) == 0 && st(kEvA) == 0 && st(kEvB) == 0,
        "W7 WaitAll: both signalled -> STATUS_SUCCESS, both consumed");
  // W8: a semaphore (host object) in a WaitAny.
  call("KeInitializeSemaphore", kSem, 1, 10);
  setArr({kEvA, kSem});
  const uint32_t w8a = waitMulti(2, kAny, relTimeout(1000));
  const uint32_t w8b = waitMulti(2, kAny, relTimeout(30));
  check(w8a == 1 && w8b == kTimeout, "W8 WaitAny over a KSEMAPHORE: count 1 satisfies once, then times out");
  // W9: the ServiceSync shape through the real import glue (r9 = NULL = infinite).
  {
    initEv(kEvA, 1, 0);
    initEv(kEvB, 1, 0);
    setArr({kEvA, kEvB});
    std::thread s([] {
      K::setGuestMemory(g_base, kGuestSize);
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      call("KeSetEvent", kEvB, 1, 0);
    });
    PPCContext c{};
    c.r1.u32 = 0x90900000u;
    c.r3.u32 = 2; c.r4.u32 = kArr; c.r5.u32 = 1; c.r6.u32 = 3; c.r7.u32 = 1; c.r8.u32 = 0;
    c.r9.u32 = 0; c.r10.u32 = 0x90800300u;
    __imp__KeWaitForMultipleObjects(c, g_base);
    s.join();
    check(c.r3.u32 == 1, "W9 glue: WaitAny infinite (XAudioServiceSync 0x82117FC0 shape) -> 1 after the exit event");
  }
  // W10: argument errors.
  check(waitMulti(0, kAny, relTimeout(10)) == 0xC0000001u, "W10a count 0 -> STATUS_UNSUCCESSFUL (host wait fails)");
  check(waitMulti(65, kAny, relTimeout(10)) == 0xC000000Du, "W10b count 65 -> STATUS_INVALID_PARAMETER");
  // W11: WaitAll completes when the second arrives later.
  {
    initEv(kEvA, 1, 1);
    initEv(kEvB, 1, 0);
    setArr({kEvA, kEvB});
    std::thread s([] {
      K::setGuestMemory(g_base, kGuestSize);
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      call("KeSetEvent", kEvB, 1, 0);
    });
    r = waitMulti(2, kAll, 0);
    s.join();
    check(r == 0 && st(kEvA) == 0 && st(kEvB) == 0, "W11 WaitAll infinite: completes when B is set, both consumed");
  }
  // W12: a guest KTHREAD (signal_state written at exit) in a WaitAny.
  if (K::obKThreadEnabled()) {
    const uint32_t kt = K::kthreadCreate(0x7777u, 0x82000000u, 0, 0, 0);
    initEv(kEvA, 1, 0);
    setArr({kEvA, kt});
    std::thread s([] {
      K::setGuestMemory(g_base, kGuestSize);
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      K::kthreadMarkHandleExited(0x7777u, 0);
    });
    r = waitMulti(2, kAny, relTimeout(3000));
    s.join();
    check(kt && r == 1, "W12 WaitAny over a KTHREAD: its exit satisfies the wait -> 1");
  }
  // W13: the XAudio pump <-> worker handshake, stressed (a_all_r2 deadlocked live: the pump in
  // ServiceSync's WaitAny{094,0D8}, the worker in KeWaitForSingleObject(0B8), all states 0).
  // Pump: KeSetEvent(go) then WaitAny{done, exit}. Worker: KeWaitForSingleObject(go) then
  // KeSetEvent(done). A lost wake shows up as a 2 s timeout; spurious wakes show up as a
  // pump/worker count mismatch.
  {
    constexpr uint32_t kGo = 0x90800400u, kDone = 0x90800420u, kExit = 0x90800440u;
    initEv(kGo, 1, 0);
    initEv(kDone, 1, 0);
    initEv(kExit, 1, 0);
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> wakes{0};
    std::thread w([&] {
      K::setGuestMemory(g_base, kGuestSize);
      while (!stop.load()) {
        call("KeWaitForSingleObject", kGo, 3, 1, 0, 0);
        if (stop.load()) break;
        wakes.fetch_add(1);
        call("KeSetEvent", kDone, 1, 0);
      }
    });
    constexpr uint32_t kIters = 200000;
    uint32_t i = 0, bad = 0, timeouts = 0;
    auto t1 = std::chrono::steady_clock::now();
    for (; i < kIters; ++i) {
      call("KeSetEvent", kGo, 1, 0);
      setArr({kDone, kExit});
      const uint32_t rr = waitMulti(2, kAny, relTimeout(2000));
      if (rr == kTimeout) { ++timeouts; break; }
      if (rr != 0) ++bad;
    }
    const double el = msSince(t1);
    stop.store(true);
    call("KeSetEvent", kGo, 1, 0);
    w.join();
    std::printf("      handshake: %u/%u iterations in %.0f ms, worker wakes %u, timeouts %u, bad %u, "
                "go=%u done=%u\n", i, kIters, el, wakes.load(), timeouts, bad, st(kGo), st(kDone));
    check(i == kIters && timeouts == 0 && bad == 0 && wakes.load() == kIters,
          "W13 pump/worker handshake x200000: no lost wake (no timeout), one worker wake per kick");
  }
  // W14: KeResumeThread(KTHREAD*) (SR_KE_RESUME_THREAD; Xenia KeResumeThread_entry). The XAudio
  // worker 0x82117810 is created CREATE_SUSPENDED and resumed this way (lr 0x82117C88).
  if (K::obKThreadEnabled()) {
    const uint32_t kt = K::kthreadCreate(0x7778u, 0x82000000u, 0, 0, 1);
    static std::atomic<bool> ran{false};
    ran.store(false);
    HANDLE h = CreateThread(
        nullptr, 0, [](LPVOID) -> DWORD { ran.store(true); return 0; }, nullptr,
        CREATE_SUSPENDED, nullptr);
    K::kthreadRecordHostThread(kt, h);
    K::keResumeThreadSetEnabledForTest(0);
    const uint32_t rOff = call("KeResumeThread", kt);
    Sleep(30);
    const bool ranOff = ran.load();
    K::keResumeThreadSetEnabledForTest(1);
    const uint32_t rOn = call("KeResumeThread", kt);
    const bool joined = WaitForSingleObject(h, 2000) == WAIT_OBJECT_0;
    check(kt && rOff == 0 && !ranOff && rOn == 0 && joined && ran.load(),
          "W14 KeResumeThread(KTHREAD) resumes the CREATE_SUSPENDED host thread (gate off: stays suspended)");
    check(call("KeResumeThread", 0x90801234u) == 0xC0000008u,
          "W14b KeResumeThread(unknown KTHREAD) -> X_STATUS_INVALID_HANDLE");
    K::keResumeThreadSetEnabledForTest(-1);
    if (h) CloseHandle(h);
  }
#endif
  return finish("KE_WAITMULTI_TEST");
}

// ── KeGetCurrentProcessType ─────────────────────────────────────────────────────────────────
namespace {
constexpr uint32_t kCfg = 0x90A00000u;
uint32_t procTypeViaGlue(uint32_t r13) {
  PPCContext c{};
  c.r1.u32 = 0x90B00000u;
  c.r13.u32 = r13;
  __imp__KeGetCurrentProcessType(c, g_base);
  return c.r3.u32;
}
bool speakerCfg(uint32_t r13, const uint8_t want[8]) {
  std::memset(K::guestVAToHost(kCfg), 0xEE, 8);
  PPCContext c{};
  c.r1.u32 = 0x90B00000u;
  c.r13.u32 = r13;
  c.r14.u32 = 0x829C0000u;
  c.r3.u32 = 0;
  c.r4.u32 = kCfg;
  sub_82117DF8(c, g_base);
  return std::memcmp(K::guestVAToHost(kCfg), want, 8) == 0;
}
}  // namespace

int runProcTypeTest() {
  std::printf("[PROCTYPE_TEST] KeGetCurrentProcessType from KPCR/KTHREAD vs Xenia (SR_PROCTYPE)\n");
  g_base = K::getGuestBase();
  K::setGuestMemory(g_base, kGuestSize);
  K::registerStubs();
  K::procTypeSetEnabledForTest(1);
  const bool on = K::procTypeEnabled();
  std::printf("  gate SR_PROCTYPE=%s (needs SR_KPCR + SR_OB_KTHREAD)\n", on ? "ON" : "OFF");
  const uint32_t pcr = K::kpcrBindCurrent(0x90B00000u, 0x90A80000u, 0);   // new anon KTHREAD: TITLE
  const uint32_t kt = pcr ? K::readGuestU32BE(pcr + 0x100u) : 0u;
  check(on && kt && *K::guestVAToHost(kt + 0x72u) == 1u && *K::guestVAToHost(kt + 0x73u) == 1u,
        "P0 a new KTHREAD carries TITLE (1) at +0x72/+0x73 (xthread.cc:215-217)");
  check(procTypeViaGlue(pcr) == 1u, "P1 KeGetCurrentProcessType through the glue = 1 (TITLE)");
  const uint8_t titleCfg[8] = {1, 1, 0, 0, 0, 0, 1, 0};
  check(speakerCfg(pcr, titleCfg),
        "P2 real AudioBuildSpeakerConfig_sub_82117DF8 takes the TITLE branch (XAudio worker config)");
  K::kthreadSetProcessType(kt, K::kProcTypeSystem);
  check(procTypeViaGlue(pcr) == 2u, "P3 the XAudio worker's KTHREAD re-typed SYSTEM reads 2");
  {
    K::KpcrDpcScope dpc(pcr);
    check(procTypeViaGlue(pcr) == 1u, "P4 inside a DPC: KPCR+0xC processtype_value_in_dpc (TITLE)");
  }
  check(procTypeViaGlue(pcr) == 2u, "P4b after the DPC: back to the thread's type");
  K::kthreadSetProcessType(kt, K::kProcTypeTitle);
  check(procTypeViaGlue(0) == 0u, "P6 r13 = 0 (no KPCR) -> 0, no fault");
  K::procTypeSetEnabledForTest(0);
  const uint8_t legacyCfg[8] = {0, 1, 0, 0, 0, 0, 4, 0};
  check(procTypeViaGlue(pcr) == 0u && speakerCfg(pcr, legacyCfg),
        "P5 gate off: the old constant 0 -> no-worker speaker config {0,1,0,0,0,0,4,0}");
  K::procTypeSetEnabledForTest(-1);
  return finish("PROCTYPE_TEST");
}
