// iter-463 audio NATIVE: the XAudio render-driver output path - a port of Xenia's AudioSystem
// (client registry, oracle pacing) and its SDL audio driver, plus the render-driver exports.
//
// Ported from Xenia Canary (BSD-3-Clause, see docs/THIRD_PARTY.md), oracle build
// canary_experimental@0bd090dbe (the build the user runs):
//   src/xenia/apu/audio_system.cc:84-330      WorkerThreadMain (deadline pacing + semaphore
//                                             back-pressure), Register/Unregister/SubmitFrame
//   src/xenia/apu/audio_system.h:32-34        kMaximumQueuedFrames 64, kAudioPumpInterval 5333 us,
//                                             kAudioIntervalSlack 400 us
//   src/xenia/apu/sdl/sdl_audio_driver.cc     device open (6 ch, 256 samples, may fall to 2 ch),
//                                             callback: pop one frame, convert, release 1 credit
//                                             (lane sdl3-audio: on SDL3's stream API since RELEASE_PLAN
//                                             step 3; the open rule, the frames and the credits unchanged)
//   src/xenia/apu/conversion.h:69-107         sequential_6_BE_to_interleaved_2_LE (the AMD64 SSE
//                                             path: L = (FL + ch4 + FC/2) / 2.5, LFE dropped)
//   src/xenia/kernel/xboxkrnl/xboxkrnl_audio.cc  the XAudio* exports
// Copyright 2020-2022 Ben Vanik / Xenia Canary. Released under the BSD license.
//
// NATIVE: the console's audio hardware output is replaced by a host device; nothing here
// emulates a register. The guest's own mixer (XAudioServiceSync, called by the render-driver
// callback) produces every frame; we only move it to the PC's speakers at the console rate.
//
// Gates (default OFF until the Rolls stage proves them; =0 keeps the A/B):
//   SR_AUDIO_OUT=1        XAudioRegister/UnregisterRenderDriverClient, XAudioSubmitRenderDriverFrame,
//                         XAudioGetVoiceCategoryVolume(ChangeMask) = this port; the render-driver
//                         callback thread (kernel/audio_render_driver.cpp) runs the oracle pacing
//                         loop below instead of the legacy fixed 5 ms tick + SDL silence queue.
//   SR_AUDIO_SINK=sdl|null  sdl (default): an SDL3 device stream per client (SDL_AUDIO_DRIVER /
//                         SDL_AUDIO_DISK_OUTPUT_FILE are honoured by SDL; the SDL2 names SDL_AUDIODRIVER /
//                         SDL_DISKAUDIOFILE too, see prepareSdlAudioEnv). null: a wall-clock consumer
//                         (one frame per 5.333 ms) - also the fallback when SDL cannot open.
//   SR_AUDIO_WAV=<path>   observe-only: a float32 WAV of exactly what each client's device plays
//                         (device channel count, 48 kHz; client i>0 gets ".c<i>" appended). The
//                         header is refreshed once a second so a killed run leaves a valid file.
//
// Deviations from the oracle (each deliberate):
//   * UnregisterClient takes the client lock BEFORE the global lock (the oracle takes global then
//     client while the worker holds client and SubmitFrame takes global = a lock-order deadlock,
//     critique A2). The client lock is recursive so a callback that unregisters itself is safe.
//   * The 8 wrapped-argument cells are one 0x20-byte guest block allocated once (Xenia: one
//     SystemHeapAlloc(4) per registration, freed on unregister). Superman's callback
//     0x821209F0 ignores r3 either way.
//   * A client with no SDL device gets the null sink (Xenia's nop driver fails the registration).
//   * SDL3 (lane sdl3-audio): the device pulls through an SDL_AudioStream get-callback (SDL2: a device
//     callback of 256 samples); the callback asks for one device period (480 frames requested) and pulls
//     exactly that, so frames, credits and the 2 / 6 ch open rule are unchanged.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sr::kernel {
class ExportRegistry;
}

namespace sr::kernel::apu {

bool audioOutEnabled();                 // SR_AUDIO_OUT (default ON since the iter-463 rolls; =0 = legacy driver)
void audioOutSetEnabledForTest(int mode);   // -1 = env, 0 off, 1 on

// iter-467 sound lane - Device guard (SR_AUDIO_DEVICE_GUARD, default ON since the rolls; =0 keeps
// the A/B). For the SDL sink: (1) a chosen OUTPUT DEVICE that will not open falls back to the
// system default instead of the silent null sink; (2) a device SDL reports STOPPED (unplugged,
// invalidated, no endpoint left) is re-opened every 2 s - the chosen device if SDL still lists
// it, else the default; (3) while the device pulls nothing for 250 ms, a host thread consumes
// frames at the console rate (credits keep flowing, so the guest's render callbacks, XAudio
// frames and the RWAudioCore DAC drain keep their console timing; the console's DAC never
// stops). Log: [AUDIO_DEV]. The default device itself is followed by SDL's WASAPI backend.
// SDL3 (lane sdl3-audio): "STOPPED" is SDL's SDL_EVENT_AUDIO_DEVICE_REMOVED for our logical device (SDL3
// keeps a lost device as a zombie that goes on consuming, so the clock never stopped there); a stream on
// the system default is migrated by SDL itself and never sees REMOVED.
void audioDeviceGuardSetForTest(int mode);   // -1 = env, 0 off, 1 on
void audioDeviceForceStoppedForTest();       // the guard treats the device as STOPPED once
// SR_AUDIO_REOPEN_RESTART (default ON; =0 = re-open only): a device SDL lists again that still will not open is opened
// after a restart of SDL's audio subsystem, run on SDL's main thread (its event pump), when this sink is SDL's only
// audio user. SDL 3.4.18 failed every in-process re-open of a disabled + re-enabled WASAPI endpoint ("WASAPI can't
// initialize audio client"; observed 2026-10-18; SDL2 recovered; a fresh subsystem opens it at once).
struct AudioDeviceGuardStats { uint64_t reopens, bridges, bridgedFrames, formatSwitches, restarts; };

// Lane sdl3-audio - Device follow (SR_AUDIO_DEVICE_FOLLOW, default ON; =0 = the SDL2 sink's behaviour),
// part of the device guard: (1) when the device's channel count changes (SDL3 moved the default stream to
// another endpoint, e.g. a stereo headset -> a 5.1 receiver) the input format is picked again by the open
// rule (stereo device = our 2 ch Xenia downmix, otherwise our 6 ch frames); SDL2 kept the open-time format
// and let SDL convert it. (2) After a fall-back to the default because the chosen OUTPUT DEVICE was lost,
// the stream goes back to the chosen device when SDL reports it again. Log: [AUDIO_DEV].
void audioDeviceFollowSetForTest(int mode);   // -1 = env, 0 off, 1 on

// Lane sdl3-audio - the SDL3 audio environment, applied before every SDL audio init (SdlSink, the legacy
// driver): (1) the SDL2 harness names: SDL_DISKAUDIOFILE -> SDL_HINT_AUDIO_DISK_OUTPUT_FILE when the SDL3
// name is unset (SDL3 maps SDL_AUDIODRIVER -> SDL_AUDIO_DRIVER itself); one WARN [AUDIO_ENV] line names both.
// (2) SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES = 480 unless set (10 ms: SDL2's WASAPI period; the disk / dummy
// drivers then run at exactly 48 kHz). (3) A harness run
// (SR_INSTANCE_ID set) with no audio driver, no SR_AUDIO_DEVICE and no null sink logs an ERROR [AUDIO_ENV]
// line: it would open the real default device; since lane E's E0 the sink refuses that device (null sink,
// chooseAudioDevice below). Renames only; it is not an SDL2 compatibility layer.
struct SdlAudioEnvReport { bool mappedDriver, mappedFile, harnessRealDevice; };
SdlAudioEnvReport prepareSdlAudioEnv();

// Lane E (RELEASE_PLAN step 5, item E0, 2026-10-22) - the output-device choice, one rule for every SDL sink.
// The safety rule (owner / orchestrator, 2026-10-18): an automated run never plays to the owner's real devices;
// audio tests use only a silent virtual endpoint ("Steam Streaming Speakers").
//   (1) SR_AUDIO_DEVICE=<substring> (a test seam): the first listed playback device whose name contains it
//       (case-insensitive) and nothing else - no match = the null sink. The stored PC SETTINGS device is never
//       used then (before E0 it survived a no-match: a stored device opened instead of the null sink).
//   (2) A harness run (SR_INSTANCE_ID set) never uses the stored PC SETTINGS device (a seeded SR_CONTENT_DIR
//       carries the owner's display.ini).
//   (3) On a real audio driver (any SDL driver but "disk" / "dummy"), a harness run and every SR_AUDIO_DEVICE
//       choice open only a device whose name matches SR_AUDIO_DEVICE_ALLOW (';'-separated, case-insensitive
//       substrings; default kAudioDeviceAllowDefault; "*" = anything, for an owner-attended run). The system
//       default device has no name to check, so such a run without SR_AUDIO_DEVICE gets the null sink unless
//       the allowlist is "*". A device the rule picked is exclusive: no fall-back to the default, also on a
//       re-open after a loss.
//   (4) A player run (no SR_INSTANCE_ID, no SR_AUDIO_DEVICE): the stored device, else the system default
//       (unchanged; a stored device that will not open still falls back to the default, see the device guard).
// Log: every device open writes one [AUDIO_OPEN] line (a milestone tag: kept with SR_LOG=player) with the device
// name, the SDL driver and the choice's source; the [AUDIO_OUT] open line ends with source= too; a refusal is one
// ERROR [AUDIO_DEV] line.
// The legacy driver (SR_AUDIO_OUT=0) applies (3) to its default-device open. tools/audio_safety_check.py
// checks a roll's log for any real endpoint off the allowlist.
constexpr const char* kAudioDeviceAllowDefault = "Steam Streaming Speakers";
struct AudioDeviceRequest {
  std::string stored;       // PC SETTINGS > AUDIO > OUTPUT DEVICE ("" = the system default)
  std::string env;          // SR_AUDIO_DEVICE ("" = unset)
  std::string allow;        // SR_AUDIO_DEVICE_ALLOW ("" = kAudioDeviceAllowDefault)
  bool harness = false;     // SR_INSTANCE_ID set
  bool realDriver = false;  // SDL's current audio driver plays to hardware (audioDriverIsReal)
};
struct AudioDeviceChoice {
  std::string device;               // the listed device name to open; "" = the system default
  bool exclusive = false;           // that device or nothing: never fall back to the default
  bool refuse = false;              // open nothing (the caller takes the null sink)
  const char* source = "default";   // "SR_AUDIO_DEVICE" | "stored" | "default" | "harness-default"
  std::string why;                  // the refusal's reason (log)
};
// true when `name` matches an entry of the allowlist (see (3)); "" never matches unless an entry is "*".
bool audioDeviceAllowed(const std::string& name, const std::string& allow);
// false only for SDL's silent drivers ("disk", "dummy"); an unknown driver (nullptr) counts as real.
bool audioDriverIsReal(const char* sdlDriverName);
AudioDeviceChoice chooseAudioDevice(const AudioDeviceRequest& req, const std::vector<std::string>& listed);
#if defined(SR_DIFFTEST)
// The sink's partial-frame continuity across channel counts (6 ch device pull -> 2 ch bridge -> 6 ch); mutant = the
// reconversion off (must fail). difftest A23.
bool queuedSinkContinuitySelfTest(bool mutant, char* report, size_t n);
#endif
void prepareSdlAudioEnvResetLogForTest();   // the WARN / ERROR lines print once per process

// iter-467 sound lane - Pre-roll (SR_AUDIO_PREROLL, default ON; =0 keeps the A/B).
// The output stream starts when the guest has queued its first 3 frames (16 ms) instead of when
// the client registers: the SDL device opens paused and is started then; before it, a pull
// plays silence and is not counted as an underrun. Xenia's XAudio2 voice likewise plays nothing
// until the first buffer. Without it every boot logs 4-6 "underruns" that are only the silence
// before the guest's first frame ([AUDIO_TIMING] underruns=).
void audioPrerollSetForTest(int mode);   // -1 = env, 0 off, 1 on
AudioDeviceGuardStats audioDeviceGuardStats();

constexpr uint32_t kAudioHandleBase = 0x41550000u;
constexpr size_t kAudioMaxClients = 8;
constexpr uint32_t kAudioMaxQueuedFrames = 64;
constexpr uint32_t kAudioPumpIntervalUs = 5333;
constexpr uint32_t kAudioIntervalSlackUs = 400;
constexpr uint32_t kAudioFrameSamples = 256;     // per channel
constexpr uint32_t kAudioFrameChannels = 6;      // FL FR FC LFE BL BR, planar, big-endian float
constexpr uint32_t kAudioFrameBytes = kAudioFrameSamples * kAudioFrameChannels * 4;   // 6144
constexpr uint32_t kXEInvalidArg = 0x80070057u;  // X_E_INVALIDARG
constexpr uint32_t kXEFail = 0x80004005u;        // X_E_FAIL (no free client slot)

// Xenia conversion.h (AMD64 path). `in` = 6144 guest bytes (planar BE float).
void convertSeq6BEToInterleaved6LE(float* out, const uint8_t* in, unsigned samples);
void convertSeq6BEToInterleaved2LE(float* out, const uint8_t* in, unsigned samples);

// A host output for one client. submitFrame copies the 6144-byte guest frame; the sink calls
// `consumed()` once per frame it actually plays (= one credit back, Xenia OnBufferEnd /
// SDLCallback's semaphore release). An empty queue plays silence and releases nothing.
class AudioSink {
 public:
  virtual ~AudioSink() = default;
  virtual void submitFrame(const uint8_t* be6ch) = 0;
  virtual void shutdown() = 0;
  virtual int deviceChannels() const = 0;
  virtual const char* kind() const = 0;
  // Lane sdl3-audio (tests): the host device behind the sink - SDL's logical device id and the device's own
  // channel count (the input format is deviceChannels()); 0 for sinks without one.
  virtual uint32_t hostDeviceId() const { return 0; }
  virtual int hostDeviceChannels() const { return 0; }
  // Lane E E0 (tests): the device name the sink opened ("" = the system default) and its choice's source.
  virtual std::string hostDeviceName() const { return {}; }
  virtual const char* deviceSource() const { return nullptr; }
};
using AudioSinkFactory =
    std::function<std::unique_ptr<AudioSink>(size_t index, std::function<void()> consumed)>;

struct AudioStats {
  uint64_t registers, unregisters, callbacks, submits, invalidSubmits, consumed, underruns,
      noSubmitCallbacks, creditMisses, callbackExceptions;
};

class AudioSystem {
 public:
  static AudioSystem& instance();

  // xboxkrnl_audio.cc XAudioRegisterRenderDriverClient: reads {callback, arg} from pClient,
  // writes 0x41550000|index to *pDriver. Returns the X status.
  uint32_t registerClientExport(uint32_t pClientVA, uint32_t pDriverVA);
  uint32_t unregisterClientExport(uint32_t driver);
  uint32_t submitFrameExport(uint32_t driver, uint32_t samplesVA);

  // Direct entry points (tests, and the exports above).
  uint32_t registerClient(uint32_t callback, uint32_t arg, size_t* outIndex);
  void unregisterClient(size_t index);
  void submitFrame(size_t index, const uint8_t* be6ch);   // null = invalid pointer -> silence

  // The worker loop (audio_system.cc WorkerThreadMain), run ON the caller's thread until
  // stopWorker(). invoke(callbackVA, wrappedArgVA) runs the guest callback and returns false if
  // it faulted.
  void runWorker(const std::function<bool(uint32_t, uint32_t)>& invoke);
  void stopWorker();
  bool workerRunning() const;

  AudioStats stats() const;
  uint32_t creditsAvailable(size_t index);     // test: the semaphore count (non-consuming probe)
  uint32_t wrappedArgCell(size_t index) const;
  struct SinkInfo {
    const char* kind;
    int channels, hostChannels;
    uint32_t hostDevice;
    std::string deviceName;          // lane E E0: hostDeviceName() ("" = the system default)
    const char* source = nullptr;    // lane E E0: deviceSource()
  };
  SinkInfo sinkInfoForTest(size_t index);                // {nullptr,...} = no client / no sink

  // Test seams.
  void setSinkFactoryForTest(AudioSinkFactory f);        // nullptr = default (SDL/null by env)
  void setCellBaseForTest(uint32_t va);                  // 0 = allocate from the guest heap
  void resetForTest();                                   // drop all clients, zero the stats

 private:
  AudioSystem();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Registers the five XAudio render-driver exports when audioOutEnabled(); otherwise leaves the
// legacy registrations (stubs.cpp / audio_render_driver.cpp) untouched.
void registerAudioExports(ExportRegistry& reg);

// Null sink factory (tests + SR_AUDIO_SINK=null): consumes one frame per 256/48000 s.
std::unique_ptr<AudioSink> makeNullSink(size_t index, std::function<void()> consumed);

}  // namespace sr::kernel::apu
