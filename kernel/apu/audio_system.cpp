// iter-463 audio NATIVE: XAudio render-driver output path. Contract: audio_system.hpp.
//
// Ported from Xenia Canary (BSD-3-Clause, see docs/THIRD_PARTY.md), oracle build
// canary_experimental@0bd090dbe: src/xenia/apu/audio_system.cc, apu/sdl/sdl_audio_driver.cc,
// apu/conversion.h and kernel/xboxkrnl/xboxkrnl_audio.cc (Copyright 2020-2022 Ben Vanik).
// Released under the BSD license. Line references: as = audio_system.cc, sd = sdl_audio_driver.cc,
// cv = conversion.h, xa = xboxkrnl_audio.cc.
#include "kernel/apu/audio_system.hpp"
#include "kernel/audio_native_census.hpp"

#include "kernel/export_registry.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/pc_settings.hpp"   // iter-466 gfx: MASTER VOLUME, MUTE IN BACKGROUND, OUTPUT DEVICE
#include "kernel/quit.hpp"          // tools/graceful-quit: the worker loop's safe point
#include "app/log.hpp"

#include <SDL3/SDL.h>   // lane sdl3-audio (RELEASE_PLAN step 3): SDL3 stream API

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sr::kernel {
void dumpGuestThreadNativeStacks(const char* tag, int maxFrames);   // kernel/native_stack_probe.cpp
}

namespace sr::kernel::apu {

namespace {

std::atomic<int> g_outMode{-1};

bool logN(std::atomic<uint32_t>& c, uint32_t first) {
  const uint32_t n = c.fetch_add(1, std::memory_order_relaxed) + 1;
  return n <= first || (n & (n - 1)) == 0;
}
std::atomic<uint32_t> g_invalidSubmitLogs{0}, g_noSubmitLogs{0}, g_regLogs{0}, g_sinkLogs{0},
    g_slowCbLogs{0};

uint64_t nowUs() {
  using namespace std::chrono;
  return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

inline uint32_t be32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline float beFloat(const uint8_t* p) {
  const uint32_t u = be32(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// Xenia xe::threading::NanoSleepPrecise stand-in: a high-resolution waitable timer, then a
// yield-spin for the tail.
void sleepPreciseUs(uint64_t us) {
  if (us == 0) return;
  const uint64_t end = nowUs() + us;
#ifdef _WIN32
  static thread_local HANDLE t_timer = [] {
    HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/,
                                      TIMER_ALL_ACCESS);
    if (!h) h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    return h;
  }();
  if (t_timer && us > 200) {
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>((us - 100) * 10);   // relative, 100 ns units
    if (SetWaitableTimer(t_timer, &due, 0, nullptr, nullptr, FALSE))
      WaitForSingleObject(t_timer, INFINITE);
  }
#endif
  while (nowUs() < end) std::this_thread::yield();
}

// ── async log for the audio threads (iter-467) ──────────────────────────────────────────────
// The logger is synchronous (global spinlock, periodic 1 MB flushes inside it). A line written
// from the SDL callback or the render-callback thread could wait behind another thread's flush =
// an audio gap in exactly the dev-profile rolls that measure gaps. Audio-thread lines are
// formatted on the spot (after the same player-profile filter) and written by a pump thread.
class AudioLogPump {
 public:
  void push(sr::LogSeverity sev, std::string line) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_) {
      started_ = true;
      std::thread([this] { loop(); }).detach();
    }
    if (q_.size() < 4096) q_.emplace_back(sev, std::move(line));
    cv_.notify_one();
  }
 private:
  static void emit(sr::LogSeverity sev, const char* fmt, ...) {
    std::va_list a;
    va_start(a, fmt);
    sr::Log::writeV(sr::LogCategory::Boot, sev, fmt, a);
    va_end(a);
  }
  void loop() {
#ifdef _WIN32
    SetThreadDescription(GetCurrentThread(), L"SR Audio LogPump");
#endif
    for (;;) {
      std::deque<std::pair<sr::LogSeverity, std::string>> q;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&] { return !q_.empty(); });
        q.swap(q_);
      }
      for (auto& l : q) emit(l.first, "%s", l.second.c_str());
    }
  }
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::pair<sr::LogSeverity, std::string>> q_;
  bool started_ = false;
};
// Leaked on purpose: the pump thread is detached and waits on cv_ for the life of the process;
// a static object would be destroyed under it at exit (std::exit runs static destructors).
AudioLogPump& audioLog() {
  static AudioLogPump* p = new AudioLogPump();
  return *p;
}
// SR_LOG_BOOT's contract (player-profile filter on the format string), written off-thread.
#define AUDIO_LOG_ASYNC(sev, fmt, ...)                     \
  do {                                                     \
    if (sr::Log::wouldLog(sev, fmt)) {                     \
      char _b[512];                                        \
      std::snprintf(_b, sizeof(_b), fmt, __VA_ARGS__);     \
      audioLog().push(sev, _b);                            \
    }                                                      \
  } while (0)

// ── async file sink for the observers (iter-467) ─────────────────────────────────────────────
// The WAV and frame dumps used to fwrite (and fflush / fseek every second) ON the SDL callback
// and render-callback threads: a disk stall became an audio stall (SR_AUDIO_CB_PROBE caught the
// render thread 70 ms inside WriteFile from the frame dump). Observers now hand bytes to one
// background writer per file; the audio threads only memcpy under a short lock. If the writer
// falls more than 64 MB behind, data is dropped and counted (an observer never stalls audio).
class AsyncFile {
 public:
  bool open(const std::string& path) {
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return false;
    run_ = true;
    th_ = std::thread([this] { loop(); });
    return true;
  }
  bool isOpen() const { return f_ != nullptr; }
  void push(const void* p, size_t n) {
    if (!f_) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (pendingBytes_ + n > (64u << 20)) {
      dropped_ += n;
      return;
    }
    const uint8_t* b = static_cast<const uint8_t*>(p);
    q_.emplace_back(b, b + n);
    pendingBytes_ += n;
    cv_.notify_one();
  }
  // Patch bytes at an absolute offset once everything queued so far is written (the WAV header).
  void patchAt(uint64_t off, const void* p, size_t n) {
    if (!f_) return;
    std::lock_guard<std::mutex> lk(mu_);
    const uint8_t* b = static_cast<const uint8_t*>(p);
    patches_.push_back({off, std::vector<uint8_t>(b, b + n)});
    cv_.notify_one();
  }
  uint64_t dropped() const { return dropped_; }
  void close() {
    if (!f_) return;
    {
      std::lock_guard<std::mutex> lk(mu_);
      run_ = false;
      cv_.notify_one();
    }
    if (th_.joinable()) th_.join();
    std::fclose(f_);
    f_ = nullptr;
  }
  ~AsyncFile() { close(); }

 private:
  void loop() {
#ifdef _WIN32
    SetThreadDescription(GetCurrentThread(), L"SR Audio DumpWriter");
#endif
    for (;;) {
      std::deque<std::vector<uint8_t>> q;
      std::vector<std::pair<uint64_t, std::vector<uint8_t>>> patches;
      bool run;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(500),
                     [&] { return !q_.empty() || !patches_.empty() || !run_; });
        q.swap(q_);
        patches.swap(patches_);
        pendingBytes_ = 0;
        run = run_;
      }
      for (auto& c : q) std::fwrite(c.data(), 1, c.size(), f_);
      if (!patches.empty()) {
        const long pos = std::ftell(f_);
        for (auto& pa : patches) {
          std::fseek(f_, long(pa.first), SEEK_SET);
          std::fwrite(pa.second.data(), 1, pa.second.size(), f_);
        }
        std::fseek(f_, pos, SEEK_SET);
      }
      if (!q.empty() || !patches.empty()) std::fflush(f_);
      if (!run) break;
    }
  }
  FILE* f_ = nullptr;
  std::thread th_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::vector<uint8_t>> q_;
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> patches_;
  size_t pendingBytes_ = 0;
  std::atomic<uint64_t> dropped_{0};
  bool run_ = false;
};

// ── float WAV writer (observe-only, SR_AUDIO_WAV) ──────────────────────────────────────────
class WavWriter {
 public:
  bool open(const std::string& path, int channels, int rate) {
    ch_ = channels;
    rate_ = rate;
    if (!file_.open(path)) return false;
    uint8_t h[44];
    header(h);
    file_.push(h, sizeof(h));
    lastRefreshUs_ = nowUs();
    return true;
  }
  void write(const float* data, size_t floats) {
    if (!file_.isOpen()) return;
    file_.push(data, floats * sizeof(float));
    dataBytes_ += uint64_t(floats) * 4u;
    const uint64_t t = nowUs();
    if (t - lastRefreshUs_ >= 1000000u) {   // keep the header valid if the run is killed
      lastRefreshUs_ = t;
      uint8_t h[44];
      header(h);
      file_.patchAt(0, h, sizeof(h));
    }
  }
  void close() {
    if (!file_.isOpen()) return;
    uint8_t h[44];
    header(h);
    file_.patchAt(0, h, sizeof(h));
    file_.close();
  }
  ~WavWriter() { close(); }

 private:
  void header(uint8_t* h) const {
    const uint32_t data = uint32_t(std::min<uint64_t>(dataBytes_, 0xFFFFFFF0u));
    auto p32 = [&](int o, uint32_t v) { std::memcpy(h + o, &v, 4); };
    auto p16 = [&](int o, uint16_t v) { std::memcpy(h + o, &v, 2); };
    std::memcpy(h, "RIFF", 4);
    p32(4, 36u + data);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    p32(16, 16);
    p16(20, 3);   // WAVE_FORMAT_IEEE_FLOAT
    p16(22, uint16_t(ch_));
    p32(24, uint32_t(rate_));
    p32(28, uint32_t(rate_ * ch_ * 4));
    p16(32, uint16_t(ch_ * 4));
    p16(34, 32);
    std::memcpy(h + 36, "data", 4);
    p32(40, data);
  }
  AsyncFile file_;
  int ch_ = 2, rate_ = 48000;
  uint64_t dataBytes_ = 0, lastRefreshUs_ = 0;
};

// ── raw submitted-frame dump (observe-only, SR_AUDIO_FRAMES=<path>) ────────────────────────
// iter-467 sound lane: every frame the guest hands to XAudioSubmitRenderDriverFrame, BEFORE
// pacing, credits and the host device - the same capture point and the same record format as
// the Xenia oracle side (tools/xenia_audio_capture.js), so tools/audio_oracle_compare.py reads
// both: u32 'AFR1' | u32 seq | u64 t_us (unix epoch us, steady-clock based) | 6144 B raw guest
// frame (256 samples x 6 ch planar big-endian float). Off unless the variable is set.
class FrameDump {
 public:
  void write(const uint8_t* be6ch) {
    if (!tried_.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lk(mu_);
      if (!tried_.load(std::memory_order_relaxed)) {
        const char* e = std::getenv("SR_AUDIO_FRAMES");
        if (e && e[0]) {
          const bool ok = file_.open(e);
          SR_LOG_BOOT(ok ? sr::LogSeverity::Info : sr::LogSeverity::Warn,
                      "[AUDIO_OUT] raw frame dump %s -> %s", ok ? "open" : "open FAILED", e);
          epochUs0_ = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count();
          steadyUs0_ = nowUs();
        }
        tried_.store(true, std::memory_order_release);
      }
    }
    if (!file_.isOpen()) return;
    uint8_t rec[16 + kAudioFrameBytes];
    struct { uint32_t magic, seq; uint64_t t; } h{0x31524641u, seq_++, epochUs0_ + (nowUs() - steadyUs0_)};
    std::memcpy(rec, &h, 16);
    std::memcpy(rec + 16, be6ch, kAudioFrameBytes);
    file_.push(rec, sizeof(rec));
  }
 private:
  std::mutex mu_;
  std::atomic<bool> tried_{false};
  AsyncFile file_;
  uint32_t seq_ = 0;   // the one submitting client's thread (client 0) writes it
  uint64_t epochUs0_ = 0, steadyUs0_ = 0;
};
// Leaked on purpose (the audio worker may still submit while static destructors run at exit).
FrameDump& frameDump() {
  static FrameDump* p = new FrameDump();
  return *p;
}
std::atomic<uint64_t> g_sinkUnderruns{0};   // device pulls that found the queue empty (all sinks)

// iter-467 sound lane: SR_AUDIO_PREROLL (kernel/apu/audio_system.hpp "Pre-roll").
std::atomic<int> g_prerollMode{-1};
bool prerollOn() {
  const int t = g_prerollMode.load(std::memory_order_relaxed);
  if (t >= 0) return t != 0;
  static const bool v = [] {
    const char* e = std::getenv("SR_AUDIO_PREROLL");
    return !e || e[0] != '0';   // iter-467: default ON (difftest A17; roll o13); =0 keeps the A/B
  }();
  return v;
}
constexpr size_t kPrerollFrames = 3;   // 16 ms: covers WASAPI's first pulls at device start

std::string wavPathForClient(size_t index) {
  const char* e = std::getenv("SR_AUDIO_WAV");
  if (!e || !e[0]) return {};
  std::string p(e);
  if (index > 0) p += ".c" + std::to_string(index);
  return p;
}

#if defined(SR_DIFFTEST)
std::atomic<bool> g_noReconvertForTest{false};   // the continuity self-test's mutant
bool noReconvertForTest() { return g_noReconvertForTest.load(std::memory_order_relaxed); }
#else
constexpr bool noReconvertForTest() { return false; }
#endif

// ── frame-queue core shared by the SDL and null sinks (sd:120-205) ─────────────────────────
class QueuedSink : public AudioSink {
 public:
  QueuedSink(size_t index, std::function<void()> consumed)
      : index_(index), consumed_(std::move(consumed)) {}

  void submitFrame(const uint8_t* be6ch) override {
    std::array<uint8_t, kAudioFrameBytes> fr;
    std::memcpy(fr.data(), be6ch, kAudioFrameBytes);
    bool start = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      queued_.push_back(fr);
      if (!streaming_.load(std::memory_order_relaxed) && queued_.size() >= kPrerollFrames) {
        streaming_.store(true, std::memory_order_relaxed);
        start = true;
      }
    }
    if (start && prerollOn()) onStreamStart();
  }
  // SR_AUDIO_PREROLL: the stream starts when the guest has queued its first frames (the SDL
  // device opens paused); before that a pull plays silence and is not an underrun.
  bool streaming() const { return streaming_.load(std::memory_order_relaxed); }

  // Fill `samples` interleaved samples of `ch` channels. Pops (and credits) whole frames as it
  // goes; plays silence for whatever the queue cannot supply (no credit for silence, sd:180).
  void pull(float* out, int ch, unsigned samples) { pullImpl(out, ch, samples, true); }
  // iter-467 device guard: consume at the console rate while the device pulls nothing. The
  // frames are credited (the guest's clock runs) but never heard and never reach the observers.
  void pullUnheard(float* out2ch, unsigned samples) { pullImpl(out2ch, 2, samples, false); }
  // A re-opened device may have another channel count: drop the half-converted frame.
  void dropPartialFrame() {
    std::lock_guard<std::mutex> lk(mu_);
    curOff_ = kAudioFrameSamples;
  }
  void pullImpl(float* out, int ch, unsigned samples, bool heard) {
    if (prerollOn() && !streaming()) {   // before the guest's first frames: silence, no underrun
      std::memset(out, 0, size_t(samples) * ch * sizeof(float));
      if (heard) observe(out, ch, samples, 0, false);
      return;
    }
    unsigned done = 0;
    unsigned framesPopped = 0;
    bool under = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      while (done < samples) {
        if (curOff_ >= kAudioFrameSamples) {
          if (queued_.empty()) {
            under = true;
            break;
          }
          curRaw_ = queued_.front();
          queued_.pop_front();
          convertCur(ch);
          curOff_ = 0;
          ++framesPopped;
        } else if (curCh_ != ch && !noReconvertForTest()) {
          // lane sdl3-audio (Astra P1): the rest of this frame for another channel count - the device guard's
          // bridge pulls 2 ch, and SDL3's device period (480) leaves frames half-played, which SDL2's 256-sample
          // callbacks never did; a format switch lands mid-frame the same way. Reconvert from the frame's own bytes.
          convertCur(ch);
        }
        const unsigned take = std::min(samples - done, kAudioFrameSamples - curOff_);
        std::memcpy(out + size_t(done) * ch, cur_.data() + size_t(curOff_) * ch,
                    size_t(take) * ch * sizeof(float));
        done += take;
        curOff_ += take;
      }
    }
    if (done < samples)
      std::memset(out + size_t(done) * ch, 0, size_t(samples - done) * ch * sizeof(float));
    for (unsigned i = 0; i < framesPopped; ++i)
      if (consumed_) consumed_();
    if (heard) observe(out, ch, samples, framesPopped, under);
  }

  const char* kind() const override { return "queued"; }

 protected:
  virtual void onStreamStart() {}
  void observe(const float* out, int ch, unsigned samples, unsigned frames, bool under) {
    if (!wavTried_) {
      wavTried_ = true;
      const std::string p = wavPathForClient(index_);
      if (!p.empty()) {
        wavCh_ = ch;
        if (wav_.open(p, ch, 48000))
          SR_LOG_BOOT(sr::LogSeverity::Info, "[AUDIO_OUT] client %zu WAV dump -> %s (%d ch)",
                      index_, p.c_str(), ch);
        else
          SR_LOG_BOOT(sr::LogSeverity::Warn, "[AUDIO_OUT] client %zu WAV dump open FAILED: %s",
                      index_, p.c_str());
      }
    }
    if (ch == wavCh_ || wavCh_ == 0) wav_.write(out, size_t(samples) * ch);   // iter-467: a re-opened device with another channel count is not mixed into the WAV
    // [AUDIO_RMS]: one line per second of played audio (fixed rate, count-safe).
    for (size_t i = 0; i < size_t(samples) * ch; ++i) {
      const double v = out[i];
      sumSq_ += v * v;
      peak_ = std::max(peak_, std::fabs(v));
    }
    rmsSamples_ += samples;
    rmsFrames_ += frames;
    if (under) {
      ++rmsUnder_;
      g_sinkUnderruns.fetch_add(1, std::memory_order_relaxed);   // [AUDIO_TIMING] underruns=
    }
    totalSamples_ += samples;
    if (rmsSamples_ >= 48000u) {
      const double rms = std::sqrt(sumSq_ / double(rmsSamples_ * ch));
      AUDIO_LOG_ASYNC(sr::LogSeverity::Info,
                  "[AUDIO_RMS] c=%zu t=%.1fs rms=%.5f peak=%.5f frames=%u underruns=%u sink=%s ch=%d",
                  index_, double(totalSamples_) / 48000.0, rms, peak_, rmsFrames_, rmsUnder_,
                  kind(), ch);
      sumSq_ = 0;
      peak_ = 0;
      rmsSamples_ = 0;
      rmsFrames_ = 0;
      rmsUnder_ = 0;
    }
  }
  void clearQueue() {
    std::lock_guard<std::mutex> lk(mu_);
    queued_.clear();
    curOff_ = kAudioFrameSamples;
  }

  std::atomic<bool> streaming_{false};
  size_t index_;
  std::function<void()> consumed_;
  std::mutex mu_;
  std::deque<std::array<uint8_t, kAudioFrameBytes>> queued_;
  std::array<float, kAudioFrameSamples * 6> cur_{};
  std::array<uint8_t, kAudioFrameBytes> curRaw_{};   // the current frame as the guest gave it
  int curCh_ = 0;                                     // the channel count cur_ holds it in
  unsigned curOff_ = kAudioFrameSamples;
  void convertCur(int ch) {   // under mu_
    if (ch == 6)
      convertSeq6BEToInterleaved6LE(cur_.data(), curRaw_.data(), kAudioFrameSamples);
    else
      convertSeq6BEToInterleaved2LE(cur_.data(), curRaw_.data(), kAudioFrameSamples);
    curCh_ = ch;
  }
  WavWriter wav_;
  bool wavTried_ = false;
  int wavCh_ = 0;
  double sumSq_ = 0, peak_ = 0;
  uint64_t rmsSamples_ = 0, totalSamples_ = 0;
  uint32_t rmsFrames_ = 0, rmsUnder_ = 0;
};

// SR_AUDIO_SINK=null (and the SDL fallback): a wall-clock consumer at the console rate.
class NullSink final : public QueuedSink {
 public:
  NullSink(size_t index, std::function<void()> consumed) : QueuedSink(index, std::move(consumed)) {
    run_ = true;
    th_ = std::thread([this] {
#ifdef _WIN32
      SetThreadDescription(GetCurrentThread(), L"SR Audio NullSink");
#endif
      std::array<float, kAudioFrameSamples * 2> buf;
      uint64_t next = nowUs();
      while (run_.load(std::memory_order_relaxed)) {
        next += kAudioPumpIntervalUs;
        const uint64_t t = nowUs();
        if (next > t) sleepPreciseUs(next - t);
        else if (t - next > 100000u) next = t;   // host stall: do not burst-catch-up
        pull(buf.data(), 2, kAudioFrameSamples);
      }
    });
  }
  ~NullSink() override { shutdown(); }
  void shutdown() override {
    if (run_.exchange(false) && th_.joinable()) th_.join();
    clearQueue();
    wav_.close();
  }
  int deviceChannels() const override { return 2; }
  const char* kind() const override { return "null"; }

 private:
  std::atomic<bool> run_{false};
  std::thread th_;
};

// iter-467 sound lane: SR_AUDIO_DEVICE_GUARD (kernel/apu/audio_system.hpp "Device guard").
std::atomic<int> g_devGuardMode{-1};
std::atomic<int> g_devForceStoppedForTest{0};
std::atomic<uint64_t> g_devReopens{0}, g_devBridges{0}, g_devBridgedFrames{0}, g_devFormatSwitches{0};
bool deviceGuardOn() {
  const int t = g_devGuardMode.load(std::memory_order_relaxed);
  if (t >= 0) return t != 0;
  static const bool v = [] {
    const char* e = std::getenv("SR_AUDIO_DEVICE_GUARD");
    return !e || e[0] != '0';   // iter-467: baked ON after the rolls; =0 keeps the A/B
  }();
  return v;
}
// Lane sdl3-audio: SR_AUDIO_DEVICE_FOLLOW (kernel/apu/audio_system.hpp "Device follow").
std::atomic<int> g_devFollowMode{-1};
bool deviceFollowOn() {
  const int t = g_devFollowMode.load(std::memory_order_relaxed);
  if (t >= 0) return t != 0;
  static const bool v = [] {
    const char* e = std::getenv("SR_AUDIO_DEVICE_FOLLOW");
    return !e || e[0] != '0';   // sdl3-audio: default ON (the device-change checks); =0 = the SDL2 behaviour
  }();
  return v;
}
// Lane sdl3-audio: SR_AUDIO_REOPEN_RESTART (kernel/apu/audio_system.hpp "Device guard").
std::atomic<int> g_reopenRestartMode{-1};
bool reopenRestartOn() {
  const int t = g_reopenRestartMode.load(std::memory_order_relaxed);
  if (t >= 0) return t != 0;
  static const bool v = [] {
    const char* e = std::getenv("SR_AUDIO_REOPEN_RESTART");
    return !e || e[0] != '0';   // sdl3-audio: default ON (the endpoint-cycle test); =0 = re-open only
  }();
  return v;
}
std::atomic<int> g_sdlSinksOpen{0};             // SdlSinks holding SDL's audio subsystem (only they use it)
std::atomic<uint64_t> g_devRestarts{0}, g_nextSinkToken{1};
constexpr uint64_t kStarveUs = 250000;    // no device callback for this long = the device stopped pulling
constexpr uint64_t kReopenEveryUs = 2000000;
constexpr uint64_t kFormatPollUs = 100000;
// The bridge ends when the device pulled at least this many samples in the last 250 ms (half the console
// rate). SDL2 counted 24 callbacks of 256 samples; SDL3's callbacks follow the device period (480 frames
// requested here), so the same rule is stated in samples.
constexpr uint64_t kBridgeExitSamples = 24u * kAudioFrameSamples;

#if defined(SR_DIFFTEST)
// difftest seams (never compiled into the game): SR_AUDIO_TEST_DEVICE_CH=<1..8> asks the device for that many
// channels (the disk / dummy drivers honour it - a stand-in for a stereo / quad / 7.1 endpoint);
// SR_AUDIO_TEST_MUTANT=chanmap|sdl_downmix breaks the output on purpose (the replay harness must fail).
int testRequestedChannels() {
  const char* e = std::getenv("SR_AUDIO_TEST_DEVICE_CH");
  const int n = e ? std::atoi(e) : 0;
  return (n >= 1 && n <= 8) ? n : int(kAudioFrameChannels);
}
bool testMutant(const char* name) {
  const char* e = std::getenv("SR_AUDIO_TEST_MUTANT");
  return e && std::strcmp(e, name) == 0;
}
// Lane E E0: SR_AUDIO_TEST_REAL_DRIVER=1 makes the device choice treat SDL's disk driver as a real one, so the
// difftest drives the allowlist guard's wiring without ever opening a real device. Mutants (must FAIL, difftest
// A25m): e0_keep_stored = the pre-E0 rule (the stored device survives an SR_AUDIO_DEVICE no-match),
// e0_harness_stored = a harness run takes the stored device, e0_no_guard = no allowlist on a real driver.
bool testForceRealDriver() {
  const char* e = std::getenv("SR_AUDIO_TEST_REAL_DRIVER");
  return e && e[0] == '1';
}
#endif

// One playback device as SDL3 lists it.
struct SdlPlayback {
  SDL_AudioDeviceID id;
  std::string name;
};
std::vector<SdlPlayback> sdlPlaybackDevices() {
  std::vector<SdlPlayback> out;
  int n = 0;
  if (SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n)) {
    for (int k = 0; k < n; ++k)
      if (const char* nm = SDL_GetAudioDeviceName(ids[k])) out.push_back({ids[k], nm});
    SDL_free(ids);
  }
  return out;
}

class SdlSink;
// The sinks a main-thread restart (SR_AUDIO_REOPEN_RESTART) may still reach, by token: a callback queued with
// SDL_RunOnMainThread can run after its sink shut down. Leaked on purpose (static-destructor order at exit).
std::mutex& liveSinksMu() {
  static auto* m = new std::mutex();
  return *m;
}
std::map<uint64_t, SdlSink*>& liveSinks() {
  static auto* m = new std::map<uint64_t, SdlSink*>();
  return *m;
}

// sd:48-110 + 160-205, on SDL3's stream API (lane sdl3-audio, RELEASE_PLAN step 3). The device is opened
// with SDL_OpenAudioDeviceStream (6 ch float 48 kHz requested, as the SDL2 sink did; the device opens
// paused). The input format follows the device as SDL2's SDL_AUDIO_ALLOW_CHANNELS_CHANGE did: a stereo
// device gets our 2 ch Xenia downmix, every other device our 6 ch frames (SDL converts 6 -> 1 / 4 / 8 like
// SDL2 did). SDL calls streamCb on its device thread when the device needs data; the callback pulls exactly
// that much (pull() pops and credits whole frames, as before) and puts it into the stream. Equal formats are
// copied unchanged (SDL_audiocvt.c ConvertAudio), so 2 ch and 6 ch devices play our bytes.
class SdlSink final : public QueuedSink {
 public:
  SdlSink(size_t index, std::function<void()> consumed) : QueuedSink(index, std::move(consumed)) {}
  ~SdlSink() override { shutdown(); }

  bool open() {
    prepareSdlAudioEnv();
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {   // SDL3: bool (SDL2 returned < 0 on failure)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[AUDIO_OUT] SDL_InitSubSystem(AUDIO) failed: %s",
                  SDL_GetError());
      return false;
    }
    sdlInit_ = true;
    g_sdlSinksOpen.fetch_add(1, std::memory_order_relaxed);
    token_ = g_nextSinkToken.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> g(liveSinksMu());
      liveSinks()[token_] = this;
    }
    cbBuf_.reserve(size_t(4096) * kAudioFrameChannels);
    // The output device (lane E E0, audio_system.hpp chooseAudioDevice): iter-466 gfx's PC SETTINGS > AUDIO >
    // OUTPUT DEVICE for a player (absent = the system default, which SDL3 follows: a stream opened on the
    // default device migrates when Windows' default changes); iter-467's SR_AUDIO_DEVICE test seam (the first
    // SDL output whose name contains it, e.g. the silent virtual "Steam Streaming Speakers", so a roll measures
    // the real WASAPI path without sound reaching the owner's speakers); and the harness safety rule.
    {
      const char* inst = std::getenv("SR_INSTANCE_ID");
      const char* env = std::getenv("SR_AUDIO_DEVICE");
      const char* allow = std::getenv("SR_AUDIO_DEVICE_ALLOW");
      const char* drv = SDL_GetCurrentAudioDriver();
      AudioDeviceRequest req;
      req.harness = inst && inst[0];
      req.env = env ? env : "";
      req.allow = allow ? allow : "";
      req.realDriver = audioDriverIsReal(drv);
      if (!req.harness && req.env.empty()) req.stored = sr::kernel::pcsettings::audioDeviceSetting();
#if defined(SR_DIFFTEST)
      if (testForceRealDriver()) req.realDriver = true;
      if (testMutant("e0_no_guard")) req.realDriver = false;
      if (testMutant("e0_harness_stored") && req.harness && req.env.empty()) {
        req.stored = sr::kernel::pcsettings::audioDeviceStored();
        req.harness = false;
      }
#endif
      std::vector<std::string> names;
      for (const SdlPlayback& d : sdlPlaybackDevices()) names.push_back(d.name);
      AudioDeviceChoice c = chooseAudioDevice(req, names);
#if defined(SR_DIFFTEST)
      if (testMutant("e0_keep_stored") && c.refuse && !req.env.empty()) {
        c.refuse = false;   // the pre-E0 rule: the stored device was read first and survived a no-match
        c.device = sr::kernel::pcsettings::audioDeviceStored();
      }
#endif
      chosen_ = c.device;
      testDeviceOnly_ = c.exclusive;   // never fall back to the default (= the owner's speakers)
      choiceSource_ = c.source;
      if (!req.env.empty()) {
        const char* shown = !c.refuse ? chosen_.c_str()
                            : c.why.rfind("no listed", 0) == 0 ? "(no match: null sink, never the default)"
                                                                : "(refused: not on SR_AUDIO_DEVICE_ALLOW: null sink)";
        SR_LOG_BOOT(sr::LogSeverity::Info, "[AUDIO_DEV] SR_AUDIO_DEVICE='%s' -> '%s'", req.env.c_str(), shown);
      }
      if (c.refuse) {
        SR_LOG_BOOT(sr::LogSeverity::Error,
                    "[AUDIO_DEV] client %zu: output device REFUSED (%s; driver=%s, source=%s, allow='%s') -> null sink",
                    index_, c.why.c_str(), drv ? drv : "?", c.source,
                    req.allow.empty() ? kAudioDeviceAllowDefault : req.allow.c_str());
        return false;
      }
    }
    bool ok;
    {
      std::lock_guard<std::mutex> lk(devMu_);
      ok = openDeviceLocked();
    }
    if (!ok) return false;
    // SDL3 reports a lost device by an event (SDL2: SDL_GetAudioDeviceStatus == STOPPED). The watch runs
    // on the thread that pumps SDL events and only sets flags; the guard acts on them.
    watching_ = SDL_AddEventWatch(&SdlSink::eventWatch, this);
    if (!watching_)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[AUDIO_DEV] SDL_AddEventWatch failed: %s (device loss is seen only by starvation)",
                  SDL_GetError());
    if (deviceGuardOn()) {
      lastCbUs_.store(nowUs(), std::memory_order_relaxed);
      guardRun_ = true;
      guard_ = std::thread([this] { guardMain(); });
    }
    return true;
  }
  void shutdown() override {
    {
      // A queued main-thread restart finds nothing from here on; one running now finishes first.
      std::lock_guard<std::mutex> g(liveSinksMu());
      liveSinks().erase(token_);
    }
    if (guardRun_.exchange(false) && guard_.joinable()) guard_.join();
    if (watching_) {
      SDL_RemoveEventWatch(&SdlSink::eventWatch, this);   // returns after a running watch call
      watching_ = false;
    }
    {
      std::lock_guard<std::mutex> lk(devMu_);
      closeDeviceLocked();
    }
    if (sdlInit_) {
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
      sdlInit_ = false;
      g_sdlSinksOpen.fetch_sub(1, std::memory_order_relaxed);
    }
    clearQueue();
    wav_.close();
  }
  int deviceChannels() const override { return devCh_; }
  const char* kind() const override { return "sdl"; }
  uint32_t hostDeviceId() const override { return devId_.load(std::memory_order_relaxed); }
  int hostDeviceChannels() const override { return hwCh_.load(std::memory_order_relaxed); }
  std::string hostDeviceName() const override {
    std::lock_guard<std::mutex> g(infoMu_);   // a leaf lock: never held while another is taken
    return openedName_;
  }
  const char* deviceSource() const override { return choiceSource_; }

 private:
  // SDL_DestroyAudioStream closes the logical device SDL_OpenAudioDeviceStream opened with it and returns
  // after any running callback (the unbind takes the device lock the device thread holds while it pulls).
  void closeDeviceLocked() {
    if (stream_) {
      SDL_DestroyAudioStream(stream_);
      stream_ = nullptr;
    }
    devId_.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> g(infoMu_);
    openedName_.clear();
  }
  // Opens chosen_ when SDL lists it, else (or when that open fails) the system default.
  bool openDeviceLocked() {
    const std::vector<SdlPlayback> devs = sdlPlaybackDevices();
    std::vector<std::string> names;
    names.reserve(devs.size());
    for (const SdlPlayback& d : devs) names.push_back(d.name);
    sr::kernel::pcsettings::noteAudioDevices(names);
    devName_.clear();
    SDL_AudioDeviceID target = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (!chosen_.empty())
      for (const SdlPlayback& d : devs)
        if (d.name == chosen_) {
          devName_ = chosen_;
          target = d.id;
          break;
        }
    if (testDeviceOnly_ && devName_.empty()) return false;   // SR_AUDIO_DEVICE test endpoint gone
    chosenWouldNotOpen_ = false;
    SDL_AudioSpec want{};
    want.format = SDL_AUDIO_F32;
    want.channels = int(kAudioFrameChannels);
    want.freq = 48000;
#if defined(SR_DIFFTEST)
    want.channels = testRequestedChannels();
#endif
    for (int attempt = 0; attempt < 2; ++attempt) {
      stream_ = SDL_OpenAudioDeviceStream(target, &want, &SdlSink::streamCb, this);
      if (stream_) break;
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[AUDIO_OUT] SDL_OpenAudioDeviceStream(%s) failed: %s",
                  devName_.empty() ? "default" : devName_.c_str(), SDL_GetError());
      // iter-467: a chosen device that will not open falls back to the default (it used to
      // drop the client to the silent null sink). Without the guard, keep the old behaviour.
      if (!devName_.empty()) chosenWouldNotOpen_ = true;   // listed, but SDL will not open it (SR_AUDIO_REOPEN_RESTART)
      if (devName_.empty() || !deviceGuardOn() || testDeviceOnly_) return false;
      devName_.clear();
      target = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    }
    if (!stream_) return false;
    devId_.store(SDL_GetAudioStreamDevice(stream_), std::memory_order_relaxed);
    const char* drv = SDL_GetCurrentAudioDriver();
    // Lane E E0: the device-open receipt tools/audio_safety_check.py reads (AUDIO_OPEN is a milestone tag: kept in
    // every log profile, SR_LOG=player included; one line per open / re-open). Written as soon as SDL has opened the
    // device, before the setup below that can still close it (Astra E0 r2), so no open goes unreported.
    // target=default = SDL's system-default playback device; target=named = a listed device by id.
    SR_LOG_BOOT(sr::LogSeverity::Info, "[AUDIO_OPEN] client %zu device=%u target=%s name='%s' driver=%s source=%s",
                index_, (unsigned)devId_.load(), devName_.empty() ? "default" : "named",
                devName_.empty() ? "default" : devName_.c_str(), drv ? drv : "?", choiceSource_);
    {
      std::lock_guard<std::mutex> g(infoMu_);
      openedName_ = devName_;
    }
    removed_.store(false, std::memory_order_relaxed);
    added_.store(false, std::memory_order_relaxed);
    SDL_AudioSpec have{};
    int frames = 0;
    if (!SDL_GetAudioDeviceFormat(devId_.load(std::memory_order_relaxed), &have, &frames)) have = want;
    hwCh_ = have.channels;
    // Paused until the pre-roll starts it, so no callback reads devCh_ before it is set here.
    if (!setInputChannelsLocked(inputChannelsFor(have.channels))) {
      closeDeviceLocked();
      return false;
    }
#if defined(SR_DIFFTEST)
    if (testMutant("chanmap") && devCh_ == int(kAudioFrameChannels)) {   // FC <-> LFE: the harness must fail
      static const int kSwap[6] = {0, 1, 3, 2, 4, 5};
      SDL_SetAudioStreamInputChannelMap(stream_, kSwap, 6);
    }
#endif
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[AUDIO_OUT] client %zu SDL device %u (%s): driver=%s freq=%d ch=%d device_ch=%d frames=%d fmt=0x%04X "
                "source=%s",
                index_, (unsigned)devId_.load(), devName_.empty() ? "default" : devName_.c_str(),
                drv ? drv : "?", have.freq, devCh_.load(), have.channels, frames, (unsigned)have.format,
                choiceSource_);
    if (!prerollOn() || streaming()) SDL_ResumeAudioStreamDevice(stream_);   // else: onStreamStart
    return true;
  }
  // SDL2 opened with SDL_AUDIO_ALLOW_CHANNELS_CHANGE and kept a 2 or 6 ch device as is; anything else was
  // re-opened as 6 ch for SDL to convert. Same rule: a stereo device gets the Xenia downmix (conversion.h),
  // every other device the 6 ch frames.
  static int inputChannelsFor(int deviceChannels) {
#if defined(SR_DIFFTEST)
    if (testMutant("sdl_downmix")) return int(kAudioFrameChannels);   // SDL's own 6 -> 2 downmix: must fail
#endif
    return deviceChannels == 2 ? 2 : int(kAudioFrameChannels);
  }
  // The stream's input format = what the callback puts in. Taken under the stream lock (SDL holds it while
  // it runs the callback), so a callback sees the old format or the new one, never a mix.
  bool setInputChannelsLocked(int ch) {
    SDL_AudioSpec src{};
    src.format = SDL_AUDIO_F32;
    src.channels = ch;
    src.freq = 48000;
    if (!SDL_LockAudioStream(stream_)) return false;
    const bool ok = SDL_SetAudioStreamFormat(stream_, &src, nullptr);
    if (ok) devCh_ = ch;   // the frame in progress is reconverted for the new count by the next pull
    SDL_UnlockAudioStream(stream_);
    if (!ok)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[AUDIO_OUT] SDL_SetAudioStreamFormat(%d ch) failed: %s", ch, SDL_GetError());
    return ok;
  }
  void onStreamStart() override {
    std::lock_guard<std::mutex> lk(devMu_);
    lastCbUs_.store(nowUs(), std::memory_order_relaxed);   // the guard's starvation clock starts now
    if (stream_) SDL_ResumeAudioStreamDevice(stream_);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[AUDIO_DEV] client %zu: stream starts (%zu frames pre-rolled)",
                index_, kPrerollFrames);
  }

  // SDL event watch (the thread that pumps SDL events, i.e. the host main loop). SDL3 keeps a lost device
  // as a "zombie" that goes on consuming at the device rate (SDL_audio.c SDL_AudioDeviceDisconnected), so the
  // guest's clock does not stop; the REMOVED event is what says the sound goes nowhere.
  static bool SDLCALL eventWatch(void* ud, SDL_Event* e) {
    auto* s = static_cast<SdlSink*>(ud);
    if (!s || !e) return true;
    if (e->type == SDL_EVENT_AUDIO_DEVICE_REMOVED) {
      const SDL_AudioDeviceID mine = s->devId_.load(std::memory_order_relaxed);
      if (mine && e->adevice.which == mine) s->removed_.store(true, std::memory_order_relaxed);
    } else if (e->type == SDL_EVENT_AUDIO_DEVICE_ADDED && !e->adevice.recording) {
      s->added_.store(true, std::memory_order_relaxed);
    }
    return true;   // a watch's return value is ignored
  }

  // The console's DAC never stops consuming; a PC device can (unplugged, invalidated, blocked in
  // its driver). The guest's audio clock rides on our consumption (credits -> render callbacks ->
  // XAudio frames -> the RWAudioCore DAC drain), so a device that stops pulling stops that clock:
  // the RWAC command queue overruns in ~8 s (D-AUDIO-CMDBUF's ICALL). The guard (1) re-opens a
  // lost device (the chosen one if SDL still lists it, else the default) every 2 s, and (2)
  // while the device pulls nothing for 250 ms, consumes frames at the console rate itself
  // (one frame per 5.333 ms, never heard, not written to the WAV observer) so the game's timing
  // is the console's. With SR_AUDIO_DEVICE_FOLLOW it also (3) re-picks the input channel count when the
  // device's format changes (a stereo headset <-> a 5.1 receiver as the Windows default) and (4) goes back to
  // the chosen device when it is plugged in again. No guest memory is touched.
  void guardMain() {
#ifdef _WIN32
    SetThreadDescription(GetCurrentThread(), L"SR Audio DeviceGuard");
#endif
    std::array<float, kAudioFrameSamples * 2> scratch;
    std::array<std::pair<uint64_t, uint64_t>, 64> hist{};   // (time, samples pulled), 20 ms apart
    size_t histHead = 0;
    uint64_t nextPull = 0, nextReopen = 0, nextFormatPoll = 0, bridgeStart = 0, bridged = 0;
    bool bridging = false;
    while (guardRun_.load(std::memory_order_relaxed)) {
      const uint64_t now = nowUs();
      bool stopped, back = false;
      {
        std::lock_guard<std::mutex> lk(devMu_);
        stopped = !stream_ || removed_.load(std::memory_order_relaxed) ||
                  g_devForceStoppedForTest.load(std::memory_order_relaxed) != 0;
        if (!stopped && deviceFollowOn() && added_.exchange(false, std::memory_order_relaxed) &&
            !chosen_.empty() && devName_.empty() && !testDeviceOnly_)
          for (const SdlPlayback& d : sdlPlaybackDevices())
            if (d.name == chosen_) back = true;   // the chosen device is back: leave the fallback
      }
      if ((stopped && now >= nextReopen) || back) {
        nextReopen = now + kReopenEveryUs;
        g_devForceStoppedForTest.store(0, std::memory_order_relaxed);
        reopen(back ? "chosen device is back" : "lost");
      } else if (!stopped && deviceFollowOn() && now >= nextFormatPoll) {
        nextFormatPoll = now + kFormatPollUs;
        followFormat();
      }
      // Device pull rate over the last 250 ms (a real device: ~12,000 samples).
      const uint64_t pulled = cbSamples_.load(std::memory_order_relaxed);
      hist[histHead] = {now, pulled};
      histHead = (histHead + 1) % hist.size();
      uint64_t pulledThen = pulled, tThen = now;
      for (const auto& h : hist)
        if (h.first && now - h.first <= kStarveUs && h.first < tThen) { tThen = h.first; pulledThen = h.second; }
      const uint64_t recent = pulled - pulledThen;
      const uint64_t last = lastCbUs_.load(std::memory_order_relaxed);
      const bool silentLong = now > last && now - last > kStarveUs && (!prerollOn() || streaming());
      // enter: nothing pulled for 250 ms; leave: the device pulled >= 24 x 256 samples in the last
      // 250 ms (hysteresis, so a device that pulls in rare bursts does not flap the bridge).
      const bool starved = bridging ? (recent < kBridgeExitSamples || now - tThen < kStarveUs / 2) : silentLong;
      if (starved && !bridging) {
        bridging = true;
        bridgeStart = now;
        bridged = 0;
        nextPull = now;
        g_devBridges.fetch_add(1, std::memory_order_relaxed);
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[AUDIO_DEV] client %zu: device pulled nothing for %llu ms -> console-rate bridge ON",
                    index_, (unsigned long long)((now - last) / 1000));
      } else if (!starved && bridging) {
        bridging = false;
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[AUDIO_DEV] client %zu: device pulling again -> bridge OFF after %llu ms, %llu frames",
                    index_, (unsigned long long)((now - bridgeStart) / 1000),
                    (unsigned long long)bridged);
      }
      if (bridging) {
        while (nextPull <= now) {
          pullUnheard(scratch.data(), kAudioFrameSamples);
          ++bridged;
          g_devBridgedFrames.fetch_add(1, std::memory_order_relaxed);
          nextPull += kAudioPumpIntervalUs;
        }
        const uint64_t t2 = nowUs();
        if (nextPull > t2) sleepPreciseUs(std::min<uint64_t>(nextPull - t2, 20000));
      } else {
        sleepPreciseUs(20000);
      }
    }
  }

  // SR_AUDIO_DEVICE_FOLLOW: the device's channel count changed (SDL3 migrated the default stream to another
  // endpoint, or the endpoint's format changed): pick the input format again by the open-time rule.
  // SDL_GetAudioDeviceFormat takes the device lock, so it runs before the stream lock is taken (the device
  // thread takes device, then stream).
  void followFormat() {
    std::lock_guard<std::mutex> lk(devMu_);
    if (!stream_) return;
    SDL_AudioSpec have{};
    int frames = 0;
    if (!SDL_GetAudioDeviceFormat(devId_.load(std::memory_order_relaxed), &have, &frames)) return;
    const int want = inputChannelsFor(have.channels);
    if (want == devCh_ && have.channels == hwCh_) return;
    const int was = devCh_, wasHw = hwCh_;
    hwCh_ = have.channels;
    if (want != devCh_ && setInputChannelsLocked(want)) g_devFormatSwitches.fetch_add(1, std::memory_order_relaxed);
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[AUDIO_DEV] client %zu: device format %d -> %d ch (freq=%d) -> input %d -> %d ch",
                index_, wasHw, have.channels, have.freq, was, devCh_.load());
  }

  void reopen(const char* why) {
    std::lock_guard<std::mutex> lk(devMu_);
    const SDL_AudioDeviceID old = devId_.load(std::memory_order_relaxed);
    closeDeviceLocked();   // waits for a running callback (the callback takes mu_, not devMu_)
    dropPartialFrame();
    const bool ok = openDeviceLocked();
    // SR_AUDIO_REOPEN_RESTART: the device is listed again but will not open. Observed 2026-10-18 (SDL 3.4.18,
    // Steam Streaming Speakers disabled + re-enabled with tools/audio_endpoint_toggle.ps1): every re-open in the
    // process failed with "WASAPI can't initialize audio client", while SDL2 recovered at once and a restarted SDL
    // audio subsystem opened it at once (archive NOTES, repro1-3). The restart runs on SDL's main thread, inside its
    // event pump, because SDL_QuitAudio destroys state SDL_UpdateAudio reads there. Only when this sink is SDL's only
    // audio user (its stream is closed above) and the device the open wants is listed (devName_ set).
    // (also when the fall-back to the default opened: the chosen device is back but SDL will not open it)
    if (chosenWouldNotOpen_ && reopenRestartOn() && sdlInit_ &&
        g_sdlSinksOpen.load(std::memory_order_relaxed) == 1 && !restartPending_.exchange(true)) {
      if (!SDL_RunOnMainThread(&SdlSink::mainThreadRestart, (void*)(uintptr_t)token_, false)) {
        restartPending_.store(false);
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[AUDIO_DEV] client %zu: SDL_RunOnMainThread failed: %s", index_,
                    SDL_GetError());
      }
    }
    g_devReopens.fetch_add(1, std::memory_order_relaxed);
    SR_LOG_BOOT(ok ? sr::LogSeverity::Info : sr::LogSeverity::Warn,
                "[AUDIO_DEV] client %zu: device %u %s -> re-open %s (now %u, %s, ch=%d, reopens=%llu)",
                index_, (unsigned)old, why, ok ? "OK" : "FAILED (retry in 2 s; bridge keeps the clock)",
                (unsigned)devId_.load(), devName_.empty() ? "default" : devName_.c_str(), devCh_.load(),
                (unsigned long long)g_devReopens.load());
  }

  // SR_AUDIO_REOPEN_RESTART, on SDL's main thread (its event pump): a fresh audio subsystem, then the re-open.
  static void SDLCALL mainThreadRestart(void* ud) {
    const uint64_t token = uint64_t(uintptr_t(ud));
    std::lock_guard<std::mutex> g(liveSinksMu());
    auto it = liveSinks().find(token);
    if (it != liveSinks().end()) it->second->restartOnMain();
  }
  void restartOnMain() {
    std::lock_guard<std::mutex> lk(devMu_);
    restartPending_.store(false);
    // Nothing to do when the guard re-opened the device it wants meanwhile (a stream on the chosen device, or no
    // chosen device that would not open); a stream on the fall-back default is closed and the chosen one tried again.
    if ((stream_ && !chosenWouldNotOpen_) || !sdlInit_ || g_sdlSinksOpen.load(std::memory_order_relaxed) != 1) return;
    closeDeviceLocked();
    dropPartialFrame();
    if (watching_) {
      SDL_RemoveEventWatch(&SdlSink::eventWatch, this);
      watching_ = false;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdlInit_ = SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!sdlInit_) g_sdlSinksOpen.fetch_sub(1, std::memory_order_relaxed);
    watching_ = sdlInit_ && SDL_AddEventWatch(&SdlSink::eventWatch, this);
    g_devRestarts.fetch_add(1, std::memory_order_relaxed);
    const bool ok = sdlInit_ && openDeviceLocked();
    if (ok) lastCbUs_.store(nowUs(), std::memory_order_relaxed);
    SR_LOG_BOOT(ok ? sr::LogSeverity::Info : sr::LogSeverity::Warn,
                "[AUDIO_DEV] client %zu: listed but would not open -> SDL audio subsystem restarted (main thread): %s "
                "(now %u, %s, ch=%d, restarts=%llu)",
                index_, ok ? "re-open OK" : (sdlInit_ ? "re-open still FAILED" : SDL_GetError()),
                (unsigned)devId_.load(), devName_.empty() ? "default" : devName_.c_str(), devCh_.load(),
                (unsigned long long)g_devRestarts.load());
  }

  // SDL3 playback stream callback (SDL's device thread, the device and stream locks held): `additional`
  // bytes of input format are needed now. Pull exactly that (whole samples, rounded up) and put it in.
  static void SDLCALL streamCb(void* ud, SDL_AudioStream* stream, int additional, int /*total*/) {
    auto* s = static_cast<SdlSink*>(ud);
    if (!s || !stream || additional <= 0) return;
    s->lastCbUs_.store(nowUs(), std::memory_order_relaxed);
    const int ch = s->devCh_;
    const unsigned frameBytes = unsigned(sizeof(float)) * unsigned(ch);
    const unsigned samples = (unsigned(additional) + frameBytes - 1) / frameBytes;
    s->cbSamples_.fetch_add(samples, std::memory_order_relaxed);
    if (s->cbBuf_.size() < size_t(samples) * ch) s->cbBuf_.resize(size_t(samples) * ch);
    float* f = s->cbBuf_.data();
    s->pull(f, ch, samples);
    // iter-466 gfx: the host output gain (MASTER VOLUME / MUTE IN BACKGROUND; 1.0 = untouched,
    // the default). Host output only: the guest's mix and the WAV observer are before it.
    const float g = sr::kernel::pcsettings::audioGain(sr::kernel::hostWindowFocused());
    if (g != 1.0f)
      for (size_t i = 0, n = size_t(samples) * size_t(ch); i < n; ++i) f[i] *= g;
    SDL_PutAudioStreamData(stream, f, int(size_t(samples) * frameBytes));
  }
  SDL_AudioStream* stream_ = nullptr;
  std::atomic<SDL_AudioDeviceID> devId_{0};  // the logical device (the event watch compares it)
  std::string devName_, chosen_;
  bool testDeviceOnly_ = false;            // chooseAudioDevice's exclusive: that endpoint or nothing
  const char* choiceSource_ = "default";   // chooseAudioDevice's source (a string literal)
  std::atomic<int> devCh_{2};              // the stream's input channels (2 or 6)
  std::atomic<int> hwCh_{2};               // the device's own channel count
  bool sdlInit_ = false, watching_ = false;
  std::mutex devMu_;                       // stream_ open / close / format (guard vs shutdown)
  mutable std::mutex infoMu_;              // openedName_ (tests read it from another thread)
  std::string openedName_;                 // devName_ of the open stream ("" = the default / none)
  std::atomic<uint64_t> lastCbUs_{0}, cbSamples_{0};
  std::atomic<bool> removed_{false}, added_{false}, restartPending_{false};
  bool chosenWouldNotOpen_ = false;        // the last open found the wanted device listed but could not open it
  uint64_t token_ = 0;                     // liveSinks() key (SR_AUDIO_REOPEN_RESTART)
  std::vector<float> cbBuf_;               // the device thread's scratch (one device thread per sink)
  std::thread guard_;
  std::atomic<bool> guardRun_{false};
};

std::unique_ptr<AudioSink> makeDefaultSink(size_t index, std::function<void()> consumed) {
  const char* e = std::getenv("SR_AUDIO_SINK");
  const bool wantNull = e && std::strcmp(e, "null") == 0;
  if (!wantNull) {
    auto s = std::make_unique<SdlSink>(index, consumed);
    if (s->open()) return s;
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[AUDIO_OUT] client %zu: SDL sink unavailable -> null sink",
                index);
  }
  // Lane E E0 (Astra r1): the "nothing opened" receipt. tools/audio_safety_check.py needs a receipt in every game
  // log (a log with none is NO-EVIDENCE, never a PASS); this one says the client plays to no device.
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[AUDIO_OPEN] client %zu device=0 target=none name='null' driver=null source=%s", index,
              wantNull ? "SR_AUDIO_SINK" : "fallback");
  return makeNullSink(index, std::move(consumed));
}

}  // namespace

// ── conversion (cv:69-107, the AMD64 path Xenia runs on Windows) ─────────────────────────────
void convertSeq6BEToInterleaved6LE(float* out, const uint8_t* in, unsigned n) {
  for (unsigned s = 0; s < n; ++s)
    for (unsigned c = 0; c < 6; ++c) out[s * 6 + c] = beFloat(in + (size_t(c) * n + s) * 4);
}
void convertSeq6BEToInterleaved2LE(float* out, const uint8_t* in, unsigned n) {
  // SSE path channel roles: fl=ch0 fr=ch1 fc=ch2 bl=ch4 br=ch5 (LFE ch3 dropped).
  // left = ((fl + bl) + fc*0.5) * (1/2.5), same association order as the _mm_add_ps chain.
  const float half = 0.5f, twoFifths = 1.0f / 2.5f;
  for (unsigned s = 0; s < n; ++s) {
    const float fl = beFloat(in + (0 * size_t(n) + s) * 4);
    const float fr = beFloat(in + (1 * size_t(n) + s) * 4);
    const float fc = beFloat(in + (2 * size_t(n) + s) * 4);
    const float bl = beFloat(in + (4 * size_t(n) + s) * 4);
    const float br = beFloat(in + (5 * size_t(n) + s) * 4);
    const float ch = fc * half;
    out[s * 2 + 0] = ((fl + bl) + ch) * twoFifths;
    out[s * 2 + 1] = ((fr + br) + ch) * twoFifths;
  }
}

std::unique_ptr<AudioSink> makeNullSink(size_t index, std::function<void()> consumed) {
  return std::make_unique<NullSink>(index, std::move(consumed));
}

#if defined(SR_DIFFTEST)
// Lane sdl3-audio (Astra P1): a frame left half-played by a 6 ch device pull, finished by the bridge's 2 ch pull, and
// the next 6 ch pull starting mid-frame must each get their own conversion of the same guest frame.
namespace {
class TestQueuedSink final : public QueuedSink {
 public:
  TestQueuedSink() : QueuedSink(7, nullptr) {}
  void shutdown() override {}
  int deviceChannels() const override { return 6; }
  const char* kind() const override { return "test"; }
};
}  // namespace
bool queuedSinkContinuitySelfTest(bool mutant, char* report, size_t n) {
  g_noReconvertForTest.store(mutant);
  const int pr = g_prerollMode.load();
  g_prerollMode.store(0);   // pulls play the queue at once
  TestQueuedSink s;
  std::vector<std::array<uint8_t, kAudioFrameBytes>> f(5);
  for (unsigned k = 0; k < f.size(); ++k)
    for (unsigned c = 0; c < 6; ++c)
      for (unsigned i = 0; i < kAudioFrameSamples; ++i) {
        const float v = float(k + 1) * 0.1f + float(c + 1) * 0.01f + float(i) * 1e-5f;
        uint32_t u;
        std::memcpy(&u, &v, 4);
        u = __builtin_bswap32(u);
        std::memcpy(f[k].data() + (size_t(c) * kAudioFrameSamples + i) * 4, &u, 4);
      }
  for (auto& x : f) s.submitFrame(x.data());
  std::vector<float> a(480 * 6), b(256 * 2), c(480 * 6);
  s.pull(a.data(), 6, 480);          // F0 + F1[0:224] at 6 ch
  s.pullUnheard(b.data(), 256);      // F1[224:256] + F2[0:224] at 2 ch (the bridge)
  s.pull(c.data(), 6, 480);          // F2[224:256] + F3 + F4[0:192] at 6 ch
  std::vector<float> e6(256 * 6), e2(256 * 2);
  bool ok = true;
  size_t bad = 0;
  convertSeq6BEToInterleaved2LE(e2.data(), f[1].data(), 256);
  for (size_t i = 0; i < 32 * 2; ++i) if (b[i] != e2[224 * 2 + i]) { ok = false; ++bad; }
  convertSeq6BEToInterleaved6LE(e6.data(), f[2].data(), 256);
  for (size_t i = 0; i < 32 * 6; ++i) if (c[i] != e6[224 * 6 + i]) { ok = false; ++bad; }
  convertSeq6BEToInterleaved6LE(e6.data(), f[3].data(), 256);
  for (size_t i = 0; i < 256 * 6; ++i) if (c[32 * 6 + i] != e6[i]) { ok = false; ++bad; }
  std::snprintf(report, n, "mutant=%d mismatched samples=%zu", mutant ? 1 : 0, bad);
  g_noReconvertForTest.store(false);
  g_prerollMode.store(pr);
  return ok;
}
#endif

// Lane E E0: the output-device choice (contract: audio_system.hpp chooseAudioDevice).
namespace {
std::string lowerAscii(std::string x) {
  for (char& ch : x) ch = (char)std::tolower((unsigned char)ch);
  return x;
}
std::string trimSpaces(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t");
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}
}  // namespace
bool audioDeviceAllowed(const std::string& name, const std::string& allow) {
  const std::string list = allow.empty() ? std::string(kAudioDeviceAllowDefault) : allow;
  const std::string n = lowerAscii(name);
  size_t pos = 0;
  while (pos <= list.size()) {
    size_t end = list.find(';', pos);
    if (end == std::string::npos) end = list.size();
    const std::string item = trimSpaces(list.substr(pos, end - pos));
    if (item == "*") return true;
    if (!item.empty() && !n.empty() && n.find(lowerAscii(item)) != std::string::npos) return true;
    pos = end + 1;
  }
  return false;
}
bool audioDriverIsReal(const char* d) {
  return !(d && (std::strcmp(d, "disk") == 0 || std::strcmp(d, "dummy") == 0));
}
AudioDeviceChoice chooseAudioDevice(const AudioDeviceRequest& r, const std::vector<std::string>& listed) {
  AudioDeviceChoice c;
  // (3): the allowlist binds a harness run and every SR_AUDIO_DEVICE choice on a real driver.
  const bool guarded = r.realDriver && (r.harness || !r.env.empty());
  if (!r.env.empty()) {
    // (1): that device or nothing; the stored device plays no part.
    c.source = "SR_AUDIO_DEVICE";
    c.exclusive = true;
    const std::string want = lowerAscii(r.env);
    for (const std::string& n : listed)
      if (lowerAscii(n).find(want) != std::string::npos) {
        c.device = n;
        break;
      }
    if (c.device.empty()) {
      c.refuse = true;
      c.why = "no listed playback device matches SR_AUDIO_DEVICE";
      return c;
    }
  } else if (r.harness) {
    c.source = "harness-default";   // (2): never the stored PC SETTINGS device
  } else if (!r.stored.empty()) {
    c.source = "stored";            // (4)
    c.device = r.stored;
  }
  if (guarded) {
    if (!audioDeviceAllowed(c.device, r.allow)) {
      c.refuse = true;
      c.why = c.device.empty() ? "a harness run on a real audio driver may not open the system default device"
                               : "'" + c.device + "' is not on SR_AUDIO_DEVICE_ALLOW";
      c.device.clear();
      c.exclusive = false;
      return c;
    }
    if (!c.device.empty()) c.exclusive = true;   // never fall back to the (unchecked) default
  }
  return c;
}

// Lane sdl3-audio: the SDL3 audio environment (contract: audio_system.hpp prepareSdlAudioEnv).
namespace {
std::atomic<bool> g_envMapLogged{false}, g_envHarnessLogged{false};
bool envSet(const char* v) { return v && v[0]; }
}  // namespace
SdlAudioEnvReport prepareSdlAudioEnv() {
  SdlAudioEnvReport r{};
  // SDL's own view of the environment (SDL3 snapshots it; SDL_getenv reads that snapshot).
  const char* oldDrv = SDL_getenv("SDL_AUDIODRIVER");
  const char* newDrv = SDL_getenv("SDL_AUDIO_DRIVER");
  const char* oldFile = SDL_getenv("SDL_DISKAUDIOFILE");
  const char* newFile = SDL_getenv("SDL_AUDIO_DISK_OUTPUT_FILE");
  // (1) The SDL2 names the harness used. SDL3 already falls back from SDL_AUDIO_DRIVER to SDL_AUDIODRIVER
  // (src/SDL_hints.c), so the driver needs nothing; the disk file has no fallback (the disk driver would write
  // sdlaudio.raw into the working directory), so it becomes the SDL3 hint.
  if (envSet(oldFile) && !envSet(newFile)) {
    SDL_SetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE, oldFile);
    r.mappedFile = true;
  }
  r.mappedDriver = envSet(oldDrv) && !envSet(newDrv);
  if ((r.mappedFile || r.mappedDriver) && !g_envMapLogged.exchange(true)) {
    const char* d = SDL_GetHint(SDL_HINT_AUDIO_DRIVER);
    const char* f = SDL_GetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE);
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[AUDIO_ENV] SDL2 env names used with SDL3: SDL_AUDIODRIVER=%s -> SDL_AUDIO_DRIVER (in effect: %s), "
                "SDL_DISKAUDIOFILE=%s -> SDL_AUDIO_DISK_OUTPUT_FILE (in effect: %s); set the SDL3 names "
                "(tools/sdl_audio_env.py)",
                oldDrv ? oldDrv : "(unset)", d ? d : "(unset)", oldFile ? oldFile : "(unset)", f ? f : "(unset)");
  }
  // (2) Device buffers of 480 sample frames = 10 ms at 48 kHz (SDL3's default is 1024 = 21.3 ms). On WASAPI
  // that is the default engine period, the one SDL2's WASAPI backend used whatever the app asked (SDL2 then
  // re-buffered into our 256-sample callbacks); SDL3 asks IAudioClient3 for it, so the latency is SDL2's, not
  // more, and no low-latency period is forced on a driver. The disk / dummy drivers wait
  // frames * 1000 / freq ms (integer) per buffer: 480 -> exactly 10 ms = 48.000 kHz (256 -> 5 ms = 51.2 kHz,
  // 6.7 % fast: harness rolls would underrun). An SDL_AUDIO_DEVICE_SAMPLE_FRAMES environment variable still
  // wins (the A/B knob for the latency check).
  if (!SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES)) SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "480");
  // (3) A harness run (SR_INSTANCE_ID) that names no audio driver would open the REAL default device = the owner's
  // speakers. Since lane E's E0 the sink refuses that device (chooseAudioDevice: null sink); the harness is still
  // misconfigured, so it says so loudly every run it happens.
  const char* inst = std::getenv("SR_INSTANCE_ID");
  const char* sink = std::getenv("SR_AUDIO_SINK");
  const char* dev = std::getenv("SR_AUDIO_DEVICE");
  r.harnessRealDevice = envSet(inst) && !envSet(SDL_GetHint(SDL_HINT_AUDIO_DRIVER)) && !envSet(dev) &&
                        !(sink && std::strcmp(sink, "null") == 0);
  if (r.harnessRealDevice && !g_envHarnessLogged.exchange(true))
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[AUDIO_ENV] HARNESS RUN (SR_INSTANCE_ID=%s) WITH NO AUDIO DRIVER SET: the REAL default output "
                "device (the owner's speakers) is refused, the client gets the null sink. Set SDL_AUDIO_DRIVER=disk "
                "(tools/sdl_audio_env.py), SR_AUDIO_SINK=null, or SR_AUDIO_DEVICE=<a silent endpoint on "
                "SR_AUDIO_DEVICE_ALLOW>",
                inst);
  return r;
}
void prepareSdlAudioEnvResetLogForTest() {
  g_envMapLogged.store(false);
  g_envHarnessLogged.store(false);
}

bool audioOutEnabled() {
  const int t = g_outMode.load(std::memory_order_relaxed);
  if (t >= 0) return t != 0;
  static const bool v = [] {
    const char* e = std::getenv("SR_AUDIO_OUT");   // iter-463 rolls: default ON (=0 keeps the A/B)
    return !e || e[0] != '0';
  }();
  return v;
}
void audioOutSetEnabledForTest(int mode) { g_outMode.store(mode < 0 ? -1 : (mode ? 1 : 0)); }
void audioDeviceGuardSetForTest(int mode) { g_devGuardMode.store(mode < 0 ? -1 : (mode ? 1 : 0)); }
void audioDeviceForceStoppedForTest() { g_devForceStoppedForTest.store(1); }
void audioDeviceFollowSetForTest(int mode) { g_devFollowMode.store(mode < 0 ? -1 : (mode ? 1 : 0)); }
void audioPrerollSetForTest(int mode) { g_prerollMode.store(mode, std::memory_order_relaxed); }
AudioDeviceGuardStats audioDeviceGuardStats() {
  return {g_devReopens.load(), g_devBridges.load(), g_devBridgedFrames.load(), g_devFormatSwitches.load(),
          g_devRestarts.load()};
}

// ── AudioSystem ─────────────────────────────────────────────────────────────────────────────
struct AudioSystem::Impl {
  struct Client {
    bool inUse = false;
    uint32_t callback = 0, arg = 0, wrapped = 0;
    uint64_t nextPumpUs = 0;
    std::recursive_mutex lock;             // held across the guest callback (as:147)
    std::unique_ptr<AudioSink> sink;
    std::atomic<int> credits{0};           // Xenia client semaphore (0..64)
    std::atomic<uint64_t> submits{0};
    // Set by UnregisterClient before it blocks on `lock`: the worker stops picking the client,
    // so a long callback cannot starve the unregistering guest thread (std mutexes are unfair;
    // measured: the worker re-took the lock ahead of it for > 2 s in difftest A12).
    std::atomic<bool> unregPending{false};
  };
  std::mutex global;
  Client clients[kAudioMaxClients];
  // Xenia pending_work_event_ (auto-reset, as:54): a flag under a mutex + condition variable.
  std::mutex pendMu;
  std::condition_variable pendCv;
  bool pending = false;
  std::atomic<bool> running{false}, stopReq{false};
  uint32_t cellBase = 0, cellBaseTest = 0;
  AudioSinkFactory factory;
  std::atomic<uint64_t> registers{0}, unregisters{0}, callbacks{0}, submits{0}, invalidSubmits{0},
      consumed{0}, noSubmit{0}, creditMisses{0}, cbExc{0};
  // iter-467 observe-only callback timing (worker thread writes, the summary reads).
  std::atomic<uint64_t> slowCbs{0}, lateCbs{0}, maxDurUs{0}, maxLateUs{0}, sumDurUs{0};
  // SR_AUDIO_CB_PROBE=1 (diagnostic, default OFF, observe only): a host thread that snapshots
  // every guest thread's lr/r1/r3/ctr ([AUDIO_CB_SLOW_THREADS], first 12) when one render
  // callback has run for > 60 ms - names what XAudioServiceSync is waiting behind.
  std::atomic<uint64_t> cbRunningSinceUs{0}, cbSeq{0};
  void startCbProbeOnce() {
    static std::atomic<bool> started{false};
    static const bool on = [] {
      const char* e = std::getenv("SR_AUDIO_CB_PROBE");
      return e && e[0] == '1';
    }();
    if (!on || started.exchange(true)) return;
    std::thread([this] {
#ifdef _WIN32
      SetThreadDescription(GetCurrentThread(), L"SR Audio CbProbe");
#endif
      uint64_t lastDumpedSeq = ~0ull;
      int dumps = 0;
      while (dumps < 12) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const uint64_t since = cbRunningSinceUs.load(std::memory_order_relaxed);
        const uint64_t seq = cbSeq.load(std::memory_order_relaxed);
        if (!since || seq == lastDumpedSeq) continue;
        const uint64_t now = nowUs();
        if (now < since || now - since < 60000) continue;
        lastDumpedSeq = seq;
        ++dumps;
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[AUDIO_CB_SLOW_THREADS] callback #%llu running %.1f ms - guest threads:",
                    (unsigned long long)seq, (now - since) / 1000.0);
        sr::kernel::dumpGuestThreadNativeStacks("AUDIO_CB_SLOW_STACKS", 20);
      }
    }).detach();
  }
  void noteTiming(uint64_t lateUs, uint64_t durUs) {
    sumDurUs.fetch_add(durUs, std::memory_order_relaxed);
    if (durUs > 20000) slowCbs.fetch_add(1, std::memory_order_relaxed);
    if (lateUs > 20000) lateCbs.fetch_add(1, std::memory_order_relaxed);
    if (durUs > maxDurUs.load(std::memory_order_relaxed)) maxDurUs.store(durUs, std::memory_order_relaxed);
    if (lateUs > maxLateUs.load(std::memory_order_relaxed)) maxLateUs.store(lateUs, std::memory_order_relaxed);
  }

  void releaseCredit(size_t i) {
    consumed.fetch_add(1, std::memory_order_relaxed);
    auto& c = clients[i].credits;
    int cur = c.load(std::memory_order_relaxed);
    while (cur < int(kAudioMaxQueuedFrames) &&
           !c.compare_exchange_weak(cur, cur + 1, std::memory_order_acq_rel)) {
    }
  }
  bool tryTakeCredit(size_t i) {
    auto& c = clients[i].credits;
    int cur = c.load(std::memory_order_relaxed);
    while (cur > 0) {
      if (c.compare_exchange_weak(cur, cur - 1, std::memory_order_acq_rel)) return true;
    }
    return false;
  }
  uint32_t cellFor(size_t i) {
    if (cellBaseTest) return cellBaseTest + uint32_t(i) * 4u;
    if (!cellBase) {
      cellBase = guestHeapAlloc(kAudioMaxClients * 4);
      if (!cellBase) cellBase = guestHeapAllocFallback(kAudioMaxClients * 4);
    }
    return cellBase ? cellBase + uint32_t(i) * 4u : 0u;
  }
  void wake() {
    {
      std::lock_guard<std::mutex> lk(pendMu);
      pending = true;
    }
    pendCv.notify_all();
  }
  bool takePending() {
    std::lock_guard<std::mutex> lk(pendMu);
    const bool p = pending;
    pending = false;
    return p;
  }
  // Sleep until absolute wall time `targetUs` or a registration (true = pending work).
  // Xenia: Wait(pending_work_event_, (target-now) ms) + NanoSleepPrecise(rest). A millisecond
  // wait overshoots by up to a scheduler quantum (1 ms with timeBeginPeriod(1), 15.6 ms without)
  // and every overshoot past the deadline pushes all later deadlines back (next = now + 5333):
  // measured 127 callbacks/s instead of 187.5 in difftest. So the coarse event wait covers only
  // what lies beyond one quantum; the rest is the high-resolution precise sleep. A registration
  // during that sleep stays latched and is seen on the next pass (auto-reset semantics kept).
  bool waitPendingUntil(uint64_t targetUs) {
    const uint64_t now = nowUs();
    if (targetUs > now + 20000u) {
      std::unique_lock<std::mutex> lk(pendMu);
      pendCv.wait_for(lk, std::chrono::microseconds(targetUs - now - 16000u), [&] { return pending; });
      if (pending) {
        pending = false;
        return true;
      }
    } else if (takePending()) {
      return true;
    }
    const uint64_t n2 = nowUs();
    if (targetUs > n2) sleepPreciseUs(targetUs - n2);
    return false;
  }
  // Wait on the pending-work flag (true = signalled).
  bool waitPending(uint64_t timeoutMs) {
    std::unique_lock<std::mutex> lk(pendMu);
    pendCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return pending; });
    const bool p = pending;
    pending = false;
    return p;
  }
};

AudioSystem::AudioSystem() : impl_(std::make_unique<Impl>()) {}

AudioSystem& AudioSystem::instance() {
  static AudioSystem s;
  return s;
}

// as:236-280
uint32_t AudioSystem::registerClient(uint32_t callback, uint32_t arg, size_t* outIndex) {
  Impl& I = *impl_;
  std::lock_guard<std::mutex> g(I.global);
  size_t idx = kAudioMaxClients;
  for (size_t i = 0; i < kAudioMaxClients; ++i)
    if (!I.clients[i].inUse) { idx = i; break; }
  if (idx == kAudioMaxClients) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[AUDIO_OUT] RegisterClient: all %zu client slots in use",
                kAudioMaxClients);
    return kXEFail;
  }
  auto& c = I.clients[idx];
  // Release(kMaximumQueuedFrames) on a semaphore whose max is 64: saturates at 64.
  c.credits.store(int(kAudioMaxQueuedFrames), std::memory_order_release);
  Impl* ip = &I;
  auto consumed = [ip, idx] { ip->releaseCredit(idx); };
  c.sink = I.factory ? I.factory(idx, consumed) : makeDefaultSink(idx, consumed);
  if (!c.sink) {
    c.credits.store(0);
    return kXEFail;
  }
  const uint32_t cell = I.cellFor(idx);
  if (cell) writeGuestBE32(cell, arg);   // xe::store_and_swap(callback_arg)
  c.callback = callback;
  c.arg = arg;
  c.wrapped = cell;
  c.nextPumpUs = 0;
  c.inUse = true;
  I.registers.fetch_add(1, std::memory_order_relaxed);
  I.wake();   // re-scan and start pacing this client now (as:270)
  if (outIndex) *outIndex = idx;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[AUDIO_OUT] RegisterClient idx=%zu cb=%08X arg=%08X cell=%08X sink=%s ch=%d",
              idx, callback, arg, cell, c.sink->kind(), c.sink->deviceChannels());
  return 0;
}

// as:307-330 (lock order fixed: client, then global)
void AudioSystem::unregisterClient(size_t index) {
  Impl& I = *impl_;
  if (index >= kAudioMaxClients) return;
  auto& c = I.clients[index];
  c.unregPending.store(true, std::memory_order_release);
  std::lock_guard<std::recursive_mutex> cl(c.lock);
  struct ClearPending {
    std::atomic<bool>& f;
    ~ClearPending() { f.store(false, std::memory_order_release); }
  } clearPending{c.unregPending};
  std::unique_ptr<AudioSink> old;
  {
    std::lock_guard<std::mutex> g(I.global);
    if (!c.inUse) return;
    old = std::move(c.sink);
    c.callback = 0;
    c.arg = 0;
    c.wrapped = 0;
    c.inUse = false;
    c.credits.store(0, std::memory_order_release);   // drain the semaphore
    I.unregisters.fetch_add(1, std::memory_order_relaxed);
  }
  if (old) old->shutdown();
  SR_LOG_BOOT(sr::LogSeverity::Info, "[AUDIO_OUT] UnregisterClient idx=%zu", index);
}

// as:282-305
void AudioSystem::submitFrame(size_t index, const uint8_t* be6ch) {
  Impl& I = *impl_;
  std::lock_guard<std::mutex> g(I.global);
  if (index >= kAudioMaxClients || !I.clients[index].inUse || !I.clients[index].sink) {
    I.invalidSubmits.fetch_add(1, std::memory_order_relaxed);
    if (logN(g_invalidSubmitLogs, 10))
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[AUDIO_OUT] SubmitFrame for invalid client %zu lr=%08X",
                  index, getCurrentGuestLR());
    if (index < kAudioMaxClients && I.clients[index].sink) {
      static const std::array<uint8_t, kAudioFrameBytes> kSilence{};
      I.clients[index].sink->submitFrame(kSilence.data());
    }
    return;
  }
  static const std::array<uint8_t, kAudioFrameBytes> kSilence{};
  if (index == 0) frameDump().write(be6ch ? be6ch : kSilence.data());
  I.clients[index].sink->submitFrame(be6ch ? be6ch : kSilence.data());
  I.clients[index].submits.fetch_add(1, std::memory_order_relaxed);
  I.submits.fetch_add(1, std::memory_order_relaxed);
}

// as:84-165
void AudioSystem::runWorker(const std::function<bool(uint32_t, uint32_t)>& invoke) {
  Impl& I = *impl_;
  I.stopReq.store(false);
  I.running.store(true);
  I.startCbProbeOnce();
  uint64_t lastSumUs = nowUs(), lastCensusUs = lastSumUs;
  // tools/graceful-quit (kernel/quit.hpp): the guest callback runs under the client lock, so it never parks at an
  // import; the loop top, holding nothing, is where a quit's thread cut stops this worker.
  sr::kernel::quit::markExempt("AUDIO_WORKER");
  while (!I.stopReq.load(std::memory_order_relaxed)) {
    sr::kernel::quit::safePoint("AUDIO_WORKER");
    const uint64_t now = nowUs();
    size_t ci = kAudioMaxClients;
    uint64_t earliest = std::numeric_limits<uint64_t>::max();
    uint32_t cb = 0, cbArg = 0;
    {
      std::lock_guard<std::mutex> g(I.global);
      for (size_t i = 0; i < kAudioMaxClients; ++i) {
        if (!I.clients[i].inUse || I.clients[i].unregPending.load(std::memory_order_acquire) ||
            I.clients[i].nextPumpUs >= earliest)
          continue;
        earliest = I.clients[i].nextPumpUs;
        ci = i;
      }
      if (ci != kAudioMaxClients) {
        cb = I.clients[ci].callback;
        cbArg = I.clients[ci].wrapped;
        // guest_time_scalar = 1 (the user's config).
        I.clients[ci].nextPumpUs = (earliest > now ? earliest : now) + kAudioPumpIntervalUs;
      }
    }
    if (now - lastSumUs >= 5000000u) {
      lastSumUs = now;
      const AudioStats s = stats();
      AUDIO_LOG_ASYNC(sr::LogSeverity::Info,
                  "[AUDIO_OUT_SUM] cb=%llu submit=%llu consumed=%llu noSubmit=%llu creditMiss=%llu "
                  "invalidSubmit=%llu cbExc=%llu credits0=%d",
                  (unsigned long long)s.callbacks, (unsigned long long)s.submits,
                  (unsigned long long)s.consumed, (unsigned long long)s.noSubmitCallbacks,
                  (unsigned long long)s.creditMisses, (unsigned long long)s.invalidSubmits,
                  (unsigned long long)s.callbackExceptions,
                  I.clients[0].credits.load(std::memory_order_relaxed));
      // iter-467: cumulative callback timing since boot (>20 ms counts; max in the last 5 s).
      const uint64_t cbs = s.callbacks ? s.callbacks : 1;
      AUDIO_LOG_ASYNC(sr::LogSeverity::Info,
                  "[AUDIO_TIMING] cb=%llu underruns=%llu dur_avg_us=%llu slow20=%llu late20=%llu max5s_dur_ms=%.1f max5s_late_ms=%.1f",
                  (unsigned long long)s.callbacks,
                  (unsigned long long)g_sinkUnderruns.load(std::memory_order_relaxed),
                  (unsigned long long)(I.sumDurUs.load(std::memory_order_relaxed) / cbs),
                  (unsigned long long)I.slowCbs.load(std::memory_order_relaxed),
                  (unsigned long long)I.lateCbs.load(std::memory_order_relaxed),
                  I.maxDurUs.exchange(0, std::memory_order_relaxed) / 1000.0,
                  I.maxLateUs.exchange(0, std::memory_order_relaxed) / 1000.0);
    }
    if (now - lastCensusUs >= 30000000u) {   // iter-467 N6: the audio-MMIO census, whole-run in its last line
      lastCensusUs = now;
      char line[400];
      if (sr::kernel::audioNativeCensusLine(line, sizeof(line)) > 0)
        AUDIO_LOG_ASYNC(sr::LogSeverity::Info, "[AUDIO_NATIVE] %s", line);
    }
    if (ci == kAudioMaxClients) {   // no clients: park until one registers (as:120-127)
      I.waitPending(100);
      continue;
    }
    // Pace to kAudioIntervalSlack ahead of the deadline (as:129-150).
    const uint64_t wake = earliest > kAudioIntervalSlackUs ? earliest - kAudioIntervalSlackUs : 0;
    if (wake > now && I.waitPendingUntil(wake)) continue;   // registration: re-scan
    // tools/graceful-quit: and after the pacing wait, before the credit and the client lock - a cut during the wait
    // never runs one more guest callback.
    sr::kernel::quit::safePoint("AUDIO_WORKER");
    // Submit only if the host has a free output slot (as:152-162).
    if (!cb) continue;
    if (!I.tryTakeCredit(ci)) {
      I.creditMisses.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    auto& c = I.clients[ci];
    std::lock_guard<std::recursive_mutex> cl(c.lock);
    if (!c.sink || c.unregPending.load(std::memory_order_acquire)) continue;
    const uint64_t before = c.submits.load(std::memory_order_relaxed);
    I.callbacks.fetch_add(1, std::memory_order_relaxed);
    // iter-467 sound lane (observe only): how late the callback starts vs its deadline, and how
    // long the guest callback (XAudioServiceSync + the worker handshake) runs. A frame later than
    // the host queue covers is a gap the player hears ([AUDIO_TIMING] in [AUDIO_OUT_SUM]).
    const uint64_t cbStart = nowUs();
    // (earliest == 0: a client's first pump has no deadline yet)
    const uint64_t lateUs = (earliest && cbStart > earliest) ? cbStart - earliest : 0;
    I.cbRunningSinceUs.store(cbStart, std::memory_order_relaxed);
    const bool ok = invoke(cb, cbArg);
    I.cbRunningSinceUs.store(0, std::memory_order_relaxed);
    I.cbSeq.fetch_add(1, std::memory_order_relaxed);
    {
      const uint64_t durUs = nowUs() - cbStart;
      I.noteTiming(lateUs, durUs);
      if ((durUs > 50000 || lateUs > 50000) && logN(g_slowCbLogs, 40))
        AUDIO_LOG_ASYNC(sr::LogSeverity::Warn,
                    "[AUDIO_CB_SLOW] #%llu late=%.1f ms dur=%.1f ms credits=%d (a gap > ~5 ms empties the host queue)",
                    (unsigned long long)I.slowCbs.load(), lateUs / 1000.0, durUs / 1000.0,
                    c.credits.load(std::memory_order_relaxed));
    }
    if (!ok) I.cbExc.fetch_add(1, std::memory_order_relaxed);
    if (c.submits.load(std::memory_order_relaxed) == before) {
      // Critique B3: a callback that does not submit never returns its credit (the oracle does
      // the same); name it.
      I.noSubmit.fetch_add(1, std::memory_order_relaxed);
      if (logN(g_noSubmitLogs, 20))
        AUDIO_LOG_ASYNC(sr::LogSeverity::Warn,
                    "[AUDIO_CB_NOSUBMIT] #%llu client=%zu cb=%08X exc=%d credits=%d",
                    (unsigned long long)I.noSubmit.load(), ci, cb, ok ? 0 : 1,
                    c.credits.load(std::memory_order_relaxed));
    }
  }
  I.running.store(false);
}

void AudioSystem::stopWorker() {
  impl_->stopReq.store(true);
  impl_->wake();
}
bool AudioSystem::workerRunning() const { return impl_->running.load(); }

AudioStats AudioSystem::stats() const {
  const Impl& I = *impl_;
  AudioStats s{};
  s.registers = I.registers.load();
  s.unregisters = I.unregisters.load();
  s.callbacks = I.callbacks.load();
  s.submits = I.submits.load();
  s.invalidSubmits = I.invalidSubmits.load();
  s.consumed = I.consumed.load();
  s.noSubmitCallbacks = I.noSubmit.load();
  s.creditMisses = I.creditMisses.load();
  s.callbackExceptions = I.cbExc.load();
  s.underruns = g_sinkUnderruns.load();
  return s;
}
uint32_t AudioSystem::creditsAvailable(size_t index) {
  return index < kAudioMaxClients ? uint32_t(impl_->clients[index].credits.load()) : 0u;
}
uint32_t AudioSystem::wrappedArgCell(size_t index) const {
  return index < kAudioMaxClients ? impl_->clients[index].wrapped : 0u;
}
AudioSystem::SinkInfo AudioSystem::sinkInfoForTest(size_t index) {
  if (index >= kAudioMaxClients) return {nullptr, 0, 0, 0};
  std::lock_guard<std::mutex> g(impl_->global);
  const auto& c = impl_->clients[index];
  if (!c.inUse || !c.sink) return {nullptr, 0, 0, 0};
  return {c.sink->kind(),          c.sink->deviceChannels(), c.sink->hostDeviceChannels(),
          c.sink->hostDeviceId(),  c.sink->hostDeviceName(), c.sink->deviceSource()};
}
void AudioSystem::setSinkFactoryForTest(AudioSinkFactory f) { impl_->factory = std::move(f); }
void AudioSystem::setCellBaseForTest(uint32_t va) { impl_->cellBaseTest = va; }
void AudioSystem::resetForTest() {
  for (size_t i = 0; i < kAudioMaxClients; ++i) unregisterClient(i);
  Impl& I = *impl_;
  I.registers = I.unregisters = I.callbacks = I.submits = I.invalidSubmits = I.consumed =
      I.noSubmit = I.creditMisses = I.cbExc = 0;
  for (auto& c : I.clients) c.submits = 0;
}

// ── exports (xa:55-108) ─────────────────────────────────────────────────────────────────────
uint32_t AudioSystem::registerClientExport(uint32_t pClientVA, uint32_t pDriverVA) {
  if (!pClientVA || !guestVAToHost(pClientVA) || !guestVAToHost(pClientVA + 7)) return kXEInvalidArg;
  const uint32_t callback = readGuestU32BE(pClientVA);
  if (!callback) return kXEInvalidArg;
  const uint32_t arg = readGuestU32BE(pClientVA + 4);
  size_t idx = 0;
  const uint32_t r = registerClient(callback, arg, &idx);
  if (r) return r;
  if (pDriverVA && guestVAToHost(pDriverVA))
    writeGuestBE32(pDriverVA, kAudioHandleBase | (uint32_t(idx) & 0xFFFFu));
  return 0;
}

uint32_t AudioSystem::unregisterClientExport(uint32_t driver) {
  if ((driver & 0xFFFF0000u) != kAudioHandleBase && logN(g_regLogs, 10))
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[AUDIO_OUT] Unregister with foreign handle %08X", driver);
  unregisterClient(driver & 0xFFFFu);
  return 0;
}

uint32_t AudioSystem::submitFrameExport(uint32_t driver, uint32_t samplesVA) {
  if ((driver & 0xFFFF0000u) != kAudioHandleBase && logN(g_regLogs, 10))
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[AUDIO_OUT] Submit with foreign handle %08X", driver);
  const uint8_t* p = nullptr;
  if (samplesVA && guestVAToHost(samplesVA) && guestVAToHost(samplesVA + kAudioFrameBytes - 1))
    p = guestVAToHost(samplesVA);
  else if (logN(g_sinkLogs, 10))
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[AUDIO_OUT] Submit samples=%08X unmapped -> silence",
                samplesVA);
  submitFrame(driver & 0xFFFFu, p);
  return 0;
}

void registerAudioExports(ExportRegistry& reg) {
  if (!audioOutEnabled()) return;
  using u32 = uint32_t;
  reg.registerExport("XAudioSubmitRenderDriverFrame", +[](u32 r3, u32 r4, u32, u32, u32, u32) {
    return AudioSystem::instance().submitFrameExport(r3, r4);
  });
  // xa:30-41: *out = 0 ("no category volume changed"), then MaybeYield.
  reg.registerExport("XAudioGetVoiceCategoryVolumeChangeMask",
                     +[](u32, u32 r4, u32, u32, u32, u32) -> u32 {
                       if (r4 && guestVAToHost(r4)) writeGuestBE32(r4, 0u);
                       std::this_thread::yield();
                       return 0;
                     });
  // xa:43-50: *out = 1.0f.
  reg.registerExport("XAudioGetVoiceCategoryVolume", +[](u32, u32 r4, u32, u32, u32, u32) -> u32 {
    if (r4 && guestVAToHost(r4)) writeGuestBE32(r4, 0x3F800000u);
    return 0;
  });
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[AUDIO_OUT] SR_AUDIO_OUT=1: XAudio render driver = native port of Xenia AudioSystem "
              "(oracle 0bd090dbe: 64 credits, %u us pump, %u us slack)",
              kAudioPumpIntervalUs, kAudioIntervalSlackUs);
}

}  // namespace sr::kernel::apu
