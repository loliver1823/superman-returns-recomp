// iter-22: real XAudioRegisterRenderDriverClient implementation.
//
// On Xbox 360 the XAudio render-driver model is callback-based: the game
// calls XAudioRegisterRenderDriverClient(pClient, pHandleOut) once, where
// pClient is a 2-dword struct {context, callbackFnVA}. The OS audio
// subsystem is then expected to invoke that callback every audio frame
// (~5.33 ms for 48 kHz / 256-sample frames) so the game's mixer can run
// and submit XMA decode work.
//
// Our previous stub registered the client but never called the callback.
// As a result, FMSoundSystem CTOR completed in 3 ms (no work to do without
// the callback firing) and no audio path ever advances.
//
// This module:
//   * captures (callbackArg, callbackFnVA) from the registration descriptor
//     (big-endian on the guest side)
//   * allocates a synthetic client handle and writes it back to pHandleOut
//   * spawns a single host audio thread on the first registration that runs
//     the guest callback at ~5 ms cadence
//   * opens an SDL audio device so the host has a real audio client
//     (silence flows for now — actually-audible PCM requires XMA decoding
//     of sound.AST contents, separate work)
//
// Kernel calls made by the callback (XMASetInputBuffer*, XMAEnableContext,
// file reads from sound.AST, …) will surface in the standard [GUEST #N]
// log lines and tell us what real audio plumbing is needed next.

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "file_io.hpp"
#include "export_registry.hpp"
#include "kpcr.hpp"
#include "kthread_objects.hpp"
#include "apu/audio_system.hpp"   // iter-463 SR_AUDIO_OUT
#include <cstdlib>                 // lane E E0: std::getenv (the harness audio-device rule)
#include "quit.hpp"               // tools/graceful-quit: the legacy loop's safe point
#include "../app/log.hpp"

#include <SDL3/SDL.h>   // lane sdl3-audio (RELEASE_PLAN step 3)
#include <atomic>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <float.h>
#include <xmmintrin.h>
#endif

namespace sr {
namespace kernel {

namespace {

// One registered client at a time — the game uses a single render-driver
// client (FMSoundSystem). If the game ever registers a second, we replace
// the captured fn but keep one host audio thread running.
struct AudioClient {
  std::atomic<uint32_t> callbackVA{0};
  std::atomic<uint32_t> callbackArg{0};
};

AudioClient g_audioClient;
std::atomic<bool> g_audioThreadStarted{false};
std::atomic<uint32_t> g_callbackInvocations{0};
std::atomic<bool> g_sdlAudioOpened{false};
SDL_AudioStream* g_sdlAudioStream = nullptr;   // SDL3: a device stream (SDL2: a queue-mode device)

// Synthetic client handles start above the file/event/thread handle ranges.
// Game treats the handle as opaque, so any unique non-zero u32 works. We
// use a small dedicated base so the value stands out in logs.
constexpr uint32_t kAudioHandleBase = 0xA0D10000u;
std::atomic<uint32_t> g_nextAudioHandle{kAudioHandleBase};

// Audio cadence: 256 samples / 48000 Hz = 5.333 ms. We round to 5 ms so the
// callback fires slightly faster than real hardware — the game's mixer
// tolerates over-eager callbacks (it just sees more headroom).
constexpr int kAudioFrameMs = 5;

// Best-effort big-endian read from guest VA. Returns 0 if VA out of range.
inline uint32_t readGuestBE32(uint32_t guestVA) {
  uint8_t* h = guestVAToHost(guestVA);
  if (!h) return 0;
  return (static_cast<uint32_t>(h[0]) << 24) |
         (static_cast<uint32_t>(h[1]) << 16) |
         (static_cast<uint32_t>(h[2]) << 8)  |
          static_cast<uint32_t>(h[3]);
}

inline void writeGuestBE32_local(uint32_t guestVA, uint32_t value) {
  uint8_t* h = guestVAToHost(guestVA);
  if (!h) return;
  h[0] = (value >> 24) & 0xFF;
  h[1] = (value >> 16) & 0xFF;
  h[2] = (value >> 8)  & 0xFF;
  h[3] = value & 0xFF;
}

#if defined(SR_DIFFTEST)
// Lane E E0 (Astra r1 P1): difftest seams for --audio-out-test A26 (never compiled into the game).
// SR_AUDIO_TEST_REAL_DRIVER=1 treats SDL's disk driver as a real one (as the SDL sink's seam does);
// SR_AUDIO_TEST_MUTANT=e0_legacy_noguard skips the device rule (the pre-E0 path: always the system default).
std::string g_legacyOpenedForTest;   // the opened device's name ("default" = the system default)
bool g_legacySdlInitForTest = false;
bool legacyTestEnv(const char* name, const char* value) {
  const char* e = std::getenv(name);
  return e && std::strcmp(e, value) == 0;
}
#endif

// Lane E E0: the "nothing opened" receipt (tools/audio_safety_check.py needs a receipt in every game log: a run
// that opened no device says so; AUDIO_OPEN is a milestone tag, kept in every log profile).
void logLegacyNoDevice(const char* why) {
  SR_LOG_BOOT(sr::LogSeverity::Info, "[AUDIO_OPEN] legacy device=0 target=none name='null' driver=null source=%s",
              why);
}

void openSdlAudioDeviceOnce() {
  if (g_sdlAudioOpened.exchange(true)) return;

  // Lane E E0: SR_AUDIO_SINK=null (every automated run's interim audio rule) opens no device on this path too; it
  // used to be honoured only by the SR_AUDIO_OUT=1 sink, so an SR_AUDIO_OUT=0 A/B still opened the system default.
  // The tick loop below already runs without a stream (it is the SDL-open-failed path).
  if (const char* sink = std::getenv("SR_AUDIO_SINK"); sink && std::strcmp(sink, "null") == 0) {
    logLegacyNoDevice("SR_AUDIO_SINK");
    return;
  }
  sr::kernel::apu::prepareSdlAudioEnv();   // lane sdl3-audio: SDL2 env names, harness check
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {   // SDL3: bool (SDL2 returned non-zero on failure)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[AUDIO] SDL_InitSubSystem(AUDIO) failed: %s — continuing without device",
      SDL_GetError());
    logLegacyNoDevice("failed");
    return;
  }
#if defined(SR_DIFFTEST)
  g_legacySdlInitForTest = true;
#endif

  // Lane E E0: the same device rule as the SDL sink (audio_system.hpp chooseAudioDevice): SR_AUDIO_DEVICE = that
  // listed device or nothing; a harness run (SR_INSTANCE_ID) or an SR_AUDIO_DEVICE choice on a real driver opens only
  // a device on SR_AUDIO_DEVICE_ALLOW (the system default only with "*"); a picked device is exclusive. This path never
  // used the stored PC SETTINGS device (a player run opens the system default, as before). Astra E0 r1 P1: the first
  // guard checked only SR_INSTANCE_ID, so an SR_AUDIO_DEVICE-only run reached the default open here.
  SDL_AudioDeviceID target = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
  const char* drvName = SDL_GetCurrentAudioDriver();
  const char* source = "default";
  {
    const char* inst = std::getenv("SR_INSTANCE_ID");
    const char* env = std::getenv("SR_AUDIO_DEVICE");
    const char* allow = std::getenv("SR_AUDIO_DEVICE_ALLOW");
    sr::kernel::apu::AudioDeviceRequest req;
    req.harness = inst && inst[0];
    req.env = env ? env : "";
    req.allow = allow ? allow : "";
    req.realDriver = sr::kernel::apu::audioDriverIsReal(drvName);
#if defined(SR_DIFFTEST)
    if (legacyTestEnv("SR_AUDIO_TEST_REAL_DRIVER", "1")) req.realDriver = true;
#endif
    std::vector<std::string> names;
    std::vector<SDL_AudioDeviceID> ids;
    int n = 0;
    if (SDL_AudioDeviceID* list = SDL_GetAudioPlaybackDevices(&n)) {
      for (int k = 0; k < n; ++k)
        if (const char* nm = SDL_GetAudioDeviceName(list[k])) {
          names.push_back(nm);
          ids.push_back(list[k]);
        }
      SDL_free(list);
    }
    sr::kernel::apu::AudioDeviceChoice c = sr::kernel::apu::chooseAudioDevice(req, names);
#if defined(SR_DIFFTEST)
    if (legacyTestEnv("SR_AUDIO_TEST_MUTANT", "e0_legacy_noguard"))
      c = sr::kernel::apu::AudioDeviceChoice{};   // the mutant: the system default, always
#endif
    if (c.refuse) {
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[AUDIO_DEV] legacy driver: output device REFUSED (%s; driver=%s, source=%s, allow='%s') -> no device",
        c.why.c_str(), drvName ? drvName : "?", c.source,
        req.allow.empty() ? sr::kernel::apu::kAudioDeviceAllowDefault : req.allow.c_str());
      logLegacyNoDevice("refused");
      return;
    }
    if (!c.device.empty()) {
      for (size_t k = 0; k < names.size(); ++k)
        if (names[k] == c.device) target = ids[k];
      if (target == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) {   // cannot happen (chosen from this list); never the default
        SR_LOG_BOOT(sr::LogSeverity::Error,
          "[AUDIO_DEV] legacy driver: output device REFUSED (chosen device '%s' has no id) -> no device",
          c.device.c_str());
        logLegacyNoDevice("refused");
        return;
      }
    }
    source = c.source;
  }

  // Queue-style output on SDL3: a device stream with no callback; the tick loop puts silence into it.
  SDL_AudioSpec want{};
  want.format = SDL_AUDIO_F32;   // native-endian float (SDL2: AUDIO_F32SYS)
  want.channels = 2;
  want.freq = 48000;
  g_sdlAudioStream = SDL_OpenAudioDeviceStream(target, &want, nullptr, nullptr);
  if (!g_sdlAudioStream) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[AUDIO] SDL_OpenAudioDeviceStream failed: %s — continuing without device",
      SDL_GetError());
    logLegacyNoDevice("failed");
    return;
  }
  // Lane E E0: the device-open receipt tools/audio_safety_check.py reads (AUDIO_OPEN is a milestone tag, so it is
  // kept in every log profile, SR_LOG=player included), written as soon as the device is open.
  // target=default = SDL's system-default playback device; target=named = a listed device by id.
  const char* openedName = target == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ? nullptr : SDL_GetAudioDeviceName(target);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[AUDIO_OPEN] legacy device=%u target=%s name='%s' driver=%s source=%s",
    (unsigned)SDL_GetAudioStreamDevice(g_sdlAudioStream), openedName ? "named" : "default",
    openedName ? openedName : "default", drvName ? drvName : "?", source);
#if defined(SR_DIFFTEST)
  g_legacyOpenedForTest = openedName ? openedName : "default";
#endif

  SDL_ResumeAudioStreamDevice(g_sdlAudioStream);   // device streams open paused
  SDL_AudioSpec have{};
  int frames = 0;
  SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(g_sdlAudioStream), &have, &frames);
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[AUDIO] SDL audio device opened: freq=%d channels=%d samples=%d format=0x%04X",
    have.freq, have.channels, frames, (unsigned)have.format);
}

#if defined(_WIN32)
// iter-439 P7: the callback has thrown ~50K swallowed exceptions per run —
// the game's mixer never completes a frame, voice packets stay PENDING, the
// packet-wait spins holding a GameKernelLoop manager CS, and the ident-era
// boot starves (the no-movie-roll wedge). Name the fault: code, host RIP
// (va_to_source resolves it to the recompiled guest fn) and the AV operand.
static unsigned auditAudioCallbackException(EXCEPTION_POINTERS* ep) {
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (ep && ep->ExceptionRecord && (n <= 5 || n % 5000 == 0)) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const uintptr_t modBase = (uintptr_t)GetModuleHandleW(nullptr);
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[AUDIO] callback EXC #%u code=0x%08X hostRIP=%p imageOfs=0x%llX%s",
        n, (unsigned)er->ExceptionCode, er->ExceptionAddress,
        (unsigned long long)((uintptr_t)er->ExceptionAddress - modBase),
        er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? "" : " (non-AV)");
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        er->NumberParameters >= 2) {
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[AUDIO]   AV %s target=%p",
          er->ExceptionInformation[0] ? "WRITE" : "READ",
          (void*)er->ExceptionInformation[1]);
    }
  }
  return EXCEPTION_EXECUTE_HANDLER;
}
#endif

// Audio render thread: mirrors the setup runGuestThreadEntry does for
// ExCreateThread guest threads (TLS guest base, FPU mask, dispatch lookup,
// SEH around the call). Runs forever until the process exits.
void audioRenderThreadMain(uint8_t* guestBase, size_t guestSize) {
  setGuestMemory(guestBase, guestSize);
  uint8_t* verify = getGuestBase();
  if (!verify || verify != guestBase) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[AUDIO] setGuestMemory failed on render thread (got %p expected %p)",
      (void*)verify, (void*)guestBase);
    return;
  }

  // Allocate a dedicated guest stack for this thread (256 KiB). Mirrors
  // runGuestThreadEntry's stack handling.
  constexpr size_t kStackSize = 256 * 1024;
  uint32_t stackVA = guestHeapAlloc(kStackSize);
  if (stackVA == 0) {
    stackVA = guestHeapAllocFallback(kStackSize);
  }
  if (stackVA == 0) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[AUDIO] failed to allocate %zu-byte guest stack — render thread exiting",
      kStackSize);
    return;
  }
  std::memset(guestVAToHost(stackVA), 0, kStackSize);

#if defined(_WIN32)
  _fpreset();
  _controlfp(_MCW_EM, _MCW_EM);
  unsigned int mxcsr = _mm_getcsr();
  mxcsr |= 0x1F80;
  _mm_setcsr(mxcsr);
#endif

  // iter-463 SR_AUDIO_OUT: the Xenia AudioSystem worker (kernel/apu/audio_system.hpp) - oracle
  // pacing (5333 us deadline per client, 64-credit back-pressure released by the host device),
  // the wrapped-arg cell in r3, a KPCR whose KTHREAD is the SYSTEM process (Xenia creates this
  // thread with GetSystemProcess(): KeGetCurrentProcessType = 2 under SR_PROCTYPE), sampler
  // visibility. The legacy loop below is unchanged with the gate off.
  if (apu::audioOutEnabled()) {
    const uint32_t kpcrVA = kpcrBindCurrent(stackVA + uint32_t(kStackSize), stackVA, -1);
    kthreadSetProcessType(kthreadCurrent(true), kProcTypeSystem);
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);   // Xenia SetPriority(24)
    SetThreadDescription(GetCurrentThread(), L"SR XAudio Worker");
#endif
    static PPCContext s_ctx{};   // persistent: the sampler reads it between callbacks
    setCurrentGuestContext(&s_ctx, offsetof(PPCContext, lr), offsetof(PPCContext, ctr),
                           offsetof(PPCContext, r3), offsetof(PPCContext, r1),
                           offsetof(PPCContext, r31));
    setGuestThreadName("XAudioWorker");
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[AUDIO_OUT] worker up - stackVA=0x%08X kpcr=0x%08X (Xenia AudioSystem pacing)",
      stackVA, kpcrVA);
    auto invoke = [&](uint32_t fnVA, uint32_t wrappedArg) -> bool {
      PPCFunc* fn = PPC_LOOKUP_FUNC_SAFE(guestBase, fnVA);
      if (!fn) {
        static std::atomic<bool> s_logged{false};
        if (!s_logged.exchange(true))
          SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[AUDIO_OUT] callbackVA=0x%08X has no dispatch entry", fnVA);
        return false;
      }
      PPCContext& ctx = s_ctx;
      ctx = PPCContext{};
      ctx.r1.u32 = stackVA + static_cast<uint32_t>(kStackSize) - 0x100;
      ctx.r3.u32 = wrappedArg;      // as:161 args[] = {wrapped_callback_arg}
      ctx.r13.u32 = kpcrVA;
      ctx.r14.u32 = 0x829C0000u;
      bool ok = true;
      ExportRegistry::setGuestCallContext(true);
#if defined(_WIN32)
      __try {
        fn(ctx, guestBase);
      } __except (auditAudioCallbackException(GetExceptionInformation())) {
        ok = false;
      }
#else
      fn(ctx, guestBase);
#endif
      ExportRegistry::setGuestCallContext(false);
      g_callbackInvocations.fetch_add(1, std::memory_order_relaxed);
      return ok;
    };
    apu::AudioSystem::instance().runWorker(invoke);
    return;
  }

  // Open SDL audio late — give the harness a moment to finish video init.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  openSdlAudioDeviceOnce();

  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[AUDIO] render thread up — stackVA=0x%08X cadence=%dms",
    stackVA, kAudioFrameMs);

  // iter-463 SR_KPCR: the render-driver callback thread's own KPCR (0 = off).
  const uint32_t kpcrVA = kpcrBindCurrent(stackVA + uint32_t(kStackSize), stackVA, -1);

  // Each iteration: rebuild a fresh PPC context (callback is short-lived),
  // call the guest fn, sleep one frame.
  // tools/graceful-quit (kernel/quit.hpp): a quit's thread cut stops this loop at its top, holding nothing.
  sr::kernel::quit::markExempt("AUDIO_RENDER");
  while (true) {
    sr::kernel::quit::safePoint("AUDIO_RENDER");
    uint32_t fnVA = g_audioClient.callbackVA.load(std::memory_order_acquire);
    uint32_t arg  = g_audioClient.callbackArg.load(std::memory_order_relaxed);

    if (fnVA != 0) {
      PPCFunc* fn = PPC_LOOKUP_FUNC_SAFE(guestBase, fnVA);
      if (fn) {
        PPCContext ctx{};
        ctx.r1.u32 = stackVA + static_cast<uint32_t>(kStackSize) - 0x100;
        ctx.r3.u32 = arg;
        ctx.r14.u32 = 0x829C0000u;  // SDA2 base (matches harness/ExCreateThread)
        ctx.r13.u32 = kpcrVA;
        ctx.lr = 0;
        ctx.ctr.u32 = 0;

        ExportRegistry::setGuestCallContext(true);
#if defined(_WIN32)
        __try {
          fn(ctx, guestBase);
        } __except (auditAudioCallbackException(GetExceptionInformation())) {
          static std::atomic<unsigned> s_excCount{0};
          unsigned n = s_excCount.fetch_add(1, std::memory_order_relaxed) + 1;
          if (n <= 5 || (n % 200 == 0)) {
            SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[AUDIO] callback exception #%u (fn=0x%08X arg=0x%08X) — continuing",
              n, fnVA, arg);
          }
        }
#else
        fn(ctx, guestBase);
#endif
        ExportRegistry::setGuestCallContext(false);

        unsigned n = g_callbackInvocations.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 10 || n == 100 || n == 1000 || (n % 5000 == 0)) {
          SR_LOG_BOOT(sr::LogSeverity::Info,
            "[AUDIO] callback fired #%u (fn=0x%08X arg=0x%08X)", n, fnVA, arg);
        }
      } else {
        static std::atomic<bool> s_logged{false};
        if (!s_logged.exchange(true)) {
          SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[AUDIO] callbackVA=0x%08X has no dispatch entry — sleeping",
            fnVA);
        }
      }
    }

    // Push silence to keep the SDL queue alive and avoid underrun warnings.
    if (g_sdlAudioStream) {
      static float s_silence[256 * 2] = {};  // 256 stereo float frames
      SDL_PutAudioStreamData(g_sdlAudioStream, s_silence, sizeof(s_silence));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(kAudioFrameMs));
  }
}

void startAudioRenderThreadOnce() {
  if (g_audioThreadStarted.exchange(true)) return;

  uint8_t* base = nullptr;
  size_t size = 0;
  getGlobalGuestMemory(&base, &size);
  if (!base || size == 0) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[AUDIO] guest memory not registered yet — cannot start render thread");
    g_audioThreadStarted.store(false);
    return;
  }

  std::thread t(audioRenderThreadMain, base, size);
  t.detach();
}

}  // anonymous namespace

#if defined(SR_DIFFTEST)
// Lane E E0 (Astra r1 P1): difftest --audio-out-test A26 runs the legacy driver's device open (SR_AUDIO_OUT=0's
// openSdlAudioDeviceOnce) once under the caller's environment and closes it again. Returns the opened device's name
// ("default" = the system default), or "" when nothing was opened.
std::string legacyAudioOpenForTest() {
  g_legacyOpenedForTest.clear();
  g_legacySdlInitForTest = false;
  g_sdlAudioOpened.store(false);
  openSdlAudioDeviceOnce();
  const std::string r = g_sdlAudioStream ? g_legacyOpenedForTest : std::string();
  if (g_sdlAudioStream) {
    SDL_DestroyAudioStream(g_sdlAudioStream);
    g_sdlAudioStream = nullptr;
  }
  if (g_legacySdlInitForTest) SDL_QuitSubSystem(SDL_INIT_AUDIO);
  g_legacySdlInitForTest = false;
  g_sdlAudioOpened.store(false);
  return r;
}
#endif

// Exposed export: wired into stubs.cpp registerStubs() in place of the
// previous stub_generic_success registration.
//
// Signature (Xbox 360 XAudio render-driver):
//   HRESULT XAudioRegisterRenderDriverClient(PXAUDIODRIVERCLIENT pClient,
//                                            PHANDLE             phHandle);
// pClient layout (BE on guest) — Xenia contract (xboxkrnl_audio_xma):
//   [+0x00] uint32 callbackVA  — guest VA of frame callback
//   [+0x04] uint32 context     — opaque arg passed to callback
// iter-429: these were read SWAPPED, so the "callback" was the heap context
// object (no dispatch entry -> the render thread slept forever) and NO movie
// audio callback ever fired. Harmless for the preloaded logo movies; fatal
// for the streamed '0100' attract movie: the undrained audio queue filled at
// ~25s, the demuxer blocked on queue-full, the video decoder starved
// (BitReader guard a1+20=2), the presenter parked holding sm+560, and the FE
// service thread wedged behind it — the whole post-intro stall. The observed
// values prove the order: [+0]=0x821209F0 (static XEX code), [+4]=heap obj.
uint32_t stub_XAudioRegisterRenderDriverClient(uint32_t r3, uint32_t r4,
                                               uint32_t r5, uint32_t r6,
                                               uint32_t r7, uint32_t r8) {
  (void)r5; (void)r6; (void)r7; (void)r8;

  // iter-463 SR_AUDIO_OUT: Xenia XAudioRegisterRenderDriverClient (xboxkrnl_audio.cc:55-81):
  // INVALIDARG on a null pClient / callback, handle 0x41550000|index, a client slot in the
  // ported AudioSystem; the worker thread starts on the first successful registration.
  if (apu::audioOutEnabled()) {
    const uint32_t st = apu::AudioSystem::instance().registerClientExport(r3, r4);
    SR_LOG_KERNEL_IMPORT(
      "[AUDIO_OUT] XAudioRegisterRenderDriverClient: pClient=0x%08X pDriver=0x%08X -> 0x%08X handle=0x%08X",
      r3, r4, st, (r4 && guestVAToHost(r4)) ? readGuestBE32(r4) : 0u);
    if (st == 0) startAudioRenderThreadOnce();
    return st;
  }

  if (r3 == 0) {
    SR_LOG_KERNEL_IMPORT(
      "[AUDIO] XAudioRegisterRenderDriverClient: pClient=NULL — returning success no-op");
    return 0;
  }

  uint32_t fn  = readGuestBE32(r3 + 0x00);
  uint32_t arg = readGuestBE32(r3 + 0x04);

  uint32_t handle = g_nextAudioHandle.fetch_add(1, std::memory_order_relaxed);
  if (r4 != 0) {
    writeGuestBE32_local(r4, handle);
  }

  g_audioClient.callbackArg.store(arg, std::memory_order_relaxed);
  g_audioClient.callbackVA.store(fn, std::memory_order_release);

  SR_LOG_KERNEL_IMPORT(
    "[AUDIO] XAudioRegisterRenderDriverClient: pClient=0x%08X arg=0x%08X fn=0x%08X "
    "pHandleOut=0x%08X handle=0x%08X — render thread starting",
    r3, arg, fn, r4, handle);

  startAudioRenderThreadOnce();
  return 0;
}

uint32_t stub_XAudioUnregisterRenderDriverClient(uint32_t r3, uint32_t r4,
                                                 uint32_t r5, uint32_t r6,
                                                 uint32_t r7, uint32_t r8) {
  (void)r3; (void)r4; (void)r5; (void)r6; (void)r7; (void)r8;
  if (apu::audioOutEnabled()) {   // iter-463 SR_AUDIO_OUT (xboxkrnl_audio.cc:83-92)
    SR_LOG_KERNEL_IMPORT("[AUDIO_OUT] XAudioUnregisterRenderDriverClient: handle=0x%08X", r3);
    return apu::AudioSystem::instance().unregisterClientExport(r3);
  }
  // Stop invoking the callback. Thread itself stays alive (cheap, sleeps
  // most of the time) so a re-register can pick up where we left off.
  g_audioClient.callbackVA.store(0, std::memory_order_release);
  SR_LOG_KERNEL_IMPORT(
    "[AUDIO] XAudioUnregisterRenderDriverClient: handle=0x%08X — callback paused", r3);
  return 0;
}

}  // namespace kernel
}  // namespace sr

// iter-439 P7 OBSERVE — XAudioVoiceComputeAndClearBufferRange_sub_821202C0
// divides 48000 by THREE voice-rate fields, each guarded on console by a
// twllei (trap-if-zero) the recompiler DROPS (comment-only emission — the
// iter-434e dropped-instruction class). One field is 0 in our runs, the x86
// divide faults, the SEH aborts the whole mixer callback (~50K/run), voice
// packets never complete, and the packet-wait spins holding a GameKernelLoop
// manager CS = the no-movie-roll ident wedge. Log the operands + caller.
extern "C" void __imp__sub_821202C0(PPCContext& __restrict ctx, uint8_t* base);
void sub_821202C0(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 20 || n % 2000 == 0) {
    const uint32_t voice = ctx.r3.u32, fmt = ctx.r4.u32;
    const uint32_t v3 = fmt ? fmt : voice + 12;
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[AUDIO_DIV] #%u voice=%08X fmt=%08X lr=%08X byte8=%u "
        "rate(v3+4)=%u d16=%u d24=%u",
        n, voice, fmt, uint32_t(ctx.lr),
        sr::kernel::readGuestBE32(voice + 8) >> 24,
        sr::kernel::readGuestBE32(v3 + 4), sr::kernel::readGuestBE32(voice + 16),
        sr::kernel::readGuestBE32(voice + 24));
  }
  __imp__sub_821202C0(ctx, base);
}

// iter-439 P7 OBSERVE hop 2 — AudioVoiceSubmitBuffer_sub_82126CD8 receives
// a3=NULL (no buffer object; v5=a3-8 clamped to 0) and hands the null to the
// divide. Console would trap here too => the null buffer is upstream of THIS
// frame. Name the caller + voice when it happens.
extern "C" void __imp__sub_82126CD8(PPCContext& __restrict ctx, uint8_t* base);
void sub_82126CD8(PPCContext& __restrict ctx, uint8_t* base) {
  if (ctx.r5.u32 == 0) {
    static std::atomic<unsigned> s_n{0};
    const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 10 || n % 5000 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[AUDIO_NULLBUF] #%u SubmitBuffer a1=%08X a2=%08X a3=NULL lr=%08X",
          n, ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr));
  }
  __imp__sub_82126CD8(ctx, base);
}

// iter-22 diagnostic: did FMUI CTOR reach the audio class construction snippet
// (ppc_recomp.6.cpp:16451 — `bl 0x826d4d60`)? Strong-override `sub_826D4D60`
// to log first call and delegate to the original. If we never see the log line
// we know the CTOR exits before reaching the audio init.
extern "C" void __imp__sub_826D4D60(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826D12B8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_822A44C0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_822A2C28(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8278D258(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8278D530(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826D1298(PPCContext& __restrict ctx, uint8_t* base);

void sub_826D4D60(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_calls{0};
  unsigned n = s_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 5) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[AUDIO_DIAG] sub_826D4D60 entry #%u: r3=0x%08X r4=0x%08X r5=0x%08X lr=0x%08X — audio factory call",
      n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_826D4D60(ctx, base);
  if (n <= 5) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[AUDIO_DIAG] sub_826D4D60 return #%u: r3=0x%08X (factory ptr)",
      n, ctx.r3.u32);
  }
}

// Diagnostic: did the factory constructor actually run? sub_826D4760 is called
// only AFTER the allocator's vtable[2] returned non-NULL, so if this fires we
// know the alloc succeeded and any subsequent NULL must come from sub_826D4760
// itself (which always returns r3=r31, so cannot really be NULL unless r3 was
// passed as 0).
extern "C" void __imp__sub_826D4760(PPCContext& __restrict ctx, uint8_t* base);
void sub_826D4760(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_calls{0};
  unsigned n = s_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 5) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[AUDIO_DIAG] sub_826D4760 entry #%u: r3=0x%08X r4=0x%08X r5=0x%08X — factory ctor",
      n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  }
  __imp__sub_826D4760(ctx, base);
  if (n <= 5) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[AUDIO_DIAG] sub_826D4760 return #%u: r3=0x%08X", n, ctx.r3.u32);
  }
}

// iter-23 image-corruption: bisect inside sub_82181D80 (FMGameSerialization
// inner). Strong-override that logs *(0x8209A458) value transitions around its
// inner calls, so we can pin the rogue write to either sub_8278D258 (DOGMA
// alloc) or sub_82181C98 (struct init + 16-byte stride zero loop).
extern "C" void __imp__sub_82181D80(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82181C98(PPCContext& __restrict ctx, uint8_t* base);

namespace { uint32_t snapshotImageVA(uint8_t* base) {
  if (!base) return 0;
  uint8_t* h = base + 0x8209A458u;
  return (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) |
         (uint32_t(h[2]) <<  8) |  uint32_t(h[3]);
}}

void sub_82181D80(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_calls{0};
  unsigned n = s_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t before = snapshotImageVA(base);
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[IMAGE_BISECT] sub_82181D80 entry #%u: *(0x8209A458)=0x%08X (FMGameSerialization inner)",
    n, before);
  __imp__sub_82181D80(ctx, base);
  uint32_t after = snapshotImageVA(base);
  if (after != before) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[IMAGE_BISECT] sub_82181D80 #%u WROTE: 0x%08X -> 0x%08X",
      n, before, after);
  } else {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[IMAGE_BISECT] sub_82181D80 #%u clean: *(0x8209A458)=0x%08X", n, after);
  }
}

void sub_82181C98(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_calls{0};
  unsigned n = s_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t before = snapshotImageVA(base);
  uint32_t inputR3 = ctx.r3.u32;
  __imp__sub_82181C98(ctx, base);
  uint32_t after = snapshotImageVA(base);
  if (after != before) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[IMAGE_BISECT] sub_82181C98 #%u (r3=0x%08X) WROTE: 0x%08X -> 0x%08X",
      n, inputR3, before, after);
  }
}

// Diagnostic: did the alloc fn fire? sub_8278D588 is the allocator.vtable[2]
// (the alloc method on the audio allocator object).
extern "C" void __imp__sub_8278D588(PPCContext& __restrict ctx, uint8_t* base);
void sub_8278D588(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_calls{0};
  unsigned n = s_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 10) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[AUDIO_DIAG] sub_8278D588 entry #%u: r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X — alloc fn",
      n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  }
  // r4 = requested size (confirmed via disasm @0x8278D5AC: r4 untouched before the
  // pool->vt[9] bctrl). Save it before the original call clobbers it.
  uint32_t reqSize = ctx.r4.u32;
  __imp__sub_8278D588(ctx, base);
  // iter-292 FIX: this is the FlashPlayer ICoreAllocator::Alloc (vt[2]). It delegates
  // RAW to pool DAT_8296d8a4 -> vt[9] (no host-fallback, unlike the sub_8278D258 wrapper
  // which IS overridden in alloc_overlap_watch.cpp). On the healthy boot the DOGMA pool
  // subsystem isn't stood up, so vt[9] returns 0 -> the resMgr/CoreImpl + AnimationTargetImpl
  // factory allocs fail -> flow client g3@0x8296EB80 = NULL -> menu never loads. Serve the
  // failed grant from the host guest-heap, same policy as the sub_8278D258 / sub_82709470
  // fallbacks. Only fires on a 0 return, so it never aliases a real pool grant. (These objects
  // are long-lived menu singletons; the host block not being a pool block is acceptable here.)
  // iter-463 charter (burndown U1): substituting a host block for the guest
  // pool's NULL is a band-aid (registry) that ran with no gate;
  // "[FLASH_ALLOC_FALLBACK]" is in 0/150 run logs. Master-gated: the default
  // exe hands the guest its own allocator's result.
  if (ctx.r3.u32 == 0 && reqSize > 0 && reqSize <= 0x1000000u && sr::kernel::forcingAllowed()) {
    uint32_t repl = sr::kernel::guestHeapAlloc(reqSize);
    if (!repl) repl = sr::kernel::guestHeapAllocFallback(reqSize);
    if (repl) {
      ctx.r3.u32 = repl;
      static std::atomic<unsigned> s_fb{0};
      if (s_fb.fetch_add(1, std::memory_order_relaxed) < 16) {
        SR_LOG_BOOT(sr::LogSeverity::Info,
          "[FLASH_ALLOC_FALLBACK] sub_8278D588 pool->vt9 returned 0 for size=0x%X -> host block 0x%08X",
          reqSize, repl);
      }
    }
  }
  if (n <= 10) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[AUDIO_DIAG] sub_8278D588 return #%u: r3=0x%08X", n, ctx.r3.u32);
  }
}

// iter-23 image-corruption poller: spawn a host thread that watches the audio
// vtable byte at guest VA 0x8209A458. When it transitions away from the
// expected value 0x8278D5E0, snapshot the main-thread guest context (LR / ctr /
// r3 / r1 / r31) and the recent kernel call ring so we can identify which
// guest function is in flight when the rogue write hits.
extern void dumpMainThreadCallRing();

namespace sr { namespace kernel {

namespace {

void imageCorruptionPollerMain(uint8_t* base) {
  // Wait for the image to actually be populated (first sample matches expected).
  uint8_t* watchHost = base + 0x8209A458u;
  constexpr uint32_t kExpected = 0x8278D5E0u;

  // Spin until the expected value appears (usually within tens of ms after load).
  for (int i = 0; i < 5000; ++i) {
    uint32_t v = (uint32_t(watchHost[0]) << 24) | (uint32_t(watchHost[1]) << 16) |
                 (uint32_t(watchHost[2]) <<  8) |  uint32_t(watchHost[3]);
    if (v == kExpected) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  uint32_t v0 = (uint32_t(watchHost[0]) << 24) | (uint32_t(watchHost[1]) << 16) |
                (uint32_t(watchHost[2]) <<  8) |  uint32_t(watchHost[3]);
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[CORRUPTION_WATCH] poller armed: *(0x8209A458)=0x%08X (waiting for transition)", v0);

  uint32_t prev = v0;
  // Sliding history: every iteration we record the current main-thread LR/CTR
  // so when the corruption fires we know the last N steps of the main thread.
  constexpr int kHistorySize = 32;
  struct HistoryEntry { uint32_t lr, ctr, r3, r1, r31; };
  HistoryEntry history[kHistorySize] = {};
  int histIdx = 0;
  while (true) {
    uint32_t cur = (uint32_t(watchHost[0]) << 24) | (uint32_t(watchHost[1]) << 16) |
                   (uint32_t(watchHost[2]) <<  8) |  uint32_t(watchHost[3]);
    // Always sample main-thread context for history.
    history[histIdx] = {
      sr::kernel::getCurrentGuestPC(),
      sr::kernel::getCurrentGuestCtr(),
      sr::kernel::getCurrentGuestR3(),
      sr::kernel::getCurrentGuestR1(),
      sr::kernel::getCurrentGuestR31()
    };
    histIdx = (histIdx + 1) % kHistorySize;
    if (cur != prev) {
      // Snapshot the main thread's guest context as quickly as possible.
      uint32_t lr  = sr::kernel::getCurrentGuestPC();
      uint32_t ctr = sr::kernel::getCurrentGuestCtr();
      uint32_t r3  = sr::kernel::getCurrentGuestR3();
      uint32_t r1  = sr::kernel::getCurrentGuestR1();
      uint32_t r31 = sr::kernel::getCurrentGuestR31();
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[CORRUPTION_WATCH] *** *(0x8209A458) 0x%08X -> 0x%08X — main-thread ctx: lr=0x%08X ctr=0x%08X r3=0x%08X r1=0x%08X r31=0x%08X",
        prev, cur, lr, ctr, r3, r1, r31);
      // Dump the main-thread kernel call ring for a wider lens.
      ::dumpMainThreadCallRing();
      // Snapshot a wider context: also check whether the surrounding bytes
      // were zeroed (i.e. is this part of a memset, or a single 4-byte write?).
      uint32_t v_minus4 = (uint32_t(watchHost[-4]) << 24) | (uint32_t(watchHost[-3]) << 16) |
                          (uint32_t(watchHost[-2]) <<  8) |  uint32_t(watchHost[-1]);
      uint32_t v_plus4  = (uint32_t(watchHost[4])  << 24) | (uint32_t(watchHost[5])  << 16) |
                          (uint32_t(watchHost[6])  <<  8) |  uint32_t(watchHost[7]);
      uint32_t v_plus8  = (uint32_t(watchHost[8])  << 24) | (uint32_t(watchHost[9])  << 16) |
                          (uint32_t(watchHost[10]) <<  8) |  uint32_t(watchHost[11]);
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[CORRUPTION_WATCH] surrounding bytes: -4=0x%08X 0=0x%08X +4=0x%08X +8=0x%08X (memset-style if all zero, single-write if only +0 changed)",
        v_minus4, cur, v_plus4, v_plus8);
      // Dump the rolling history of main-thread context samples — newest
      // first, so the bottom of the list is the oldest.
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[CORRUPTION_WATCH] main-thread history (%d-deep, newest first):", kHistorySize);
      for (int i = 0; i < kHistorySize; ++i) {
        int idx = (histIdx - 1 - i + kHistorySize) % kHistorySize;
        const auto& e = history[idx];
        if (e.lr == 0 && e.ctr == 0 && e.r1 == 0) continue;  // empty slot
        SR_LOG_BOOT(sr::LogSeverity::Error,
          "[CORRUPTION_WATCH]   [-%2d] lr=0x%08X ctr=0x%08X r3=0x%08X r1=0x%08X r31=0x%08X",
          i, e.lr, e.ctr, e.r3, e.r1, e.r31);
      }
      // Sample farther bytes around the corruption to estimate the memset extent.
      auto rd = [&](int32_t off) {
        uint8_t* p = watchHost + off;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
               (uint32_t(p[2]) <<  8) |  uint32_t(p[3]);
      };
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[CORRUPTION_WATCH] extent probe: -0x100=0x%08X -0x40=0x%08X -0x10=0x%08X +0x10=0x%08X +0x40=0x%08X +0x100=0x%08X +0x1000=0x%08X +0x10000=0x%08X",
        rd(-0x100), rd(-0x40), rd(-0x10), rd(0x10), rd(0x40), rd(0x100), rd(0x1000), rd(0x10000));
      prev = cur;
      if (cur == 0u) {
        // Stop after the first full transition to zero — we have enough.
        return;
      }
    }
    // Poll at ~50 kHz so the rolling history is dense enough to identify
    // the writer.
    std::this_thread::sleep_for(std::chrono::microseconds(20));
  }
}

}  // namespace

// iter-24 module-globals band-aid (post-investigation): the four game-tick
// module pointers at 0x8296D8D8/DC/E0/E4 get SET during init (MODULE_GLOBAL
// watchpoint hits #1-4) to valid heap pointers, then later get zeroed —
// modD provably by sub_82181C98's stride-16 init loop at ppc_recomp.11.cpp
// :21573 (writes 0 to *r10 in stride 16, where r10 is a buffer returned by
// sub_8278D258). modB/C/A get zeroed by an as-yet-unidentified path (not
// PPC_STORE_U32; possibly dcbz/stvx/host-direct memcpy bypassing macros).
//
// Without the four pointers, the game's tick loop iterates over an empty
// module list, never advances state, and we stall post-WALK_END.
//
// TODO(deeper-bug): Root cause looks like sub_8278D258 is returning its
// fallback global (loaded from *(0x82189734)) because its registry table is
// not populated. That fallback IS 0x8296D8D8 — i.e., the registry "default
// empty buffer" overlaps the modA-D globals. The real fix is to populate
// sub_8278D258's registry before any caller looks up an entry. Other
// lookups going through the same registry are likely silently broken.
// Revisit once we see the next blocker — if it also smells like registry
// corruption, escalate.
//
// Band-aid: snapshot the post-init valid pointers; if any of the four
// transitions to 0, restore from snapshot. Allow non-zero updates (game
// re-pointing to a new module is legitimate). 20 kHz poll is fast enough
// to restore within ~50 µs of a clobber, before any game-tick consumer
// reads through the now-NULL pointer.
void moduleGlobalsPollerMain(uint8_t* base) {
  constexpr uint32_t kAddrs[] = {
    0x8296D8D8u,  // modD
    0x8296D8DCu,  // modB
    0x8296D8E0u,  // modC
    0x8296D8E4u,  // modA
  };
  static const char* const kNames[] = { "modD", "modB", "modC", "modA" };

  auto readBE32 = [](uint8_t* h) -> uint32_t {
    return (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) |
           (uint32_t(h[2]) <<  8) |  uint32_t(h[3]);
  };
  auto writeBE32 = [](uint8_t* h, uint32_t v) {
    h[0] = uint8_t(v >> 24);
    h[1] = uint8_t(v >> 16);
    h[2] = uint8_t(v >>  8);
    h[3] = uint8_t(v      );
  };

  // Wait for ALL four to become non-zero (full post-init), so the snapshot
  // captures the complete set of valid pointers — not a partial state.
  uint32_t snapshot[4] = {0, 0, 0, 0};
  uint32_t prev[4] = {0, 0, 0, 0};
  bool armed = false;
  for (int wait = 0; wait < 5000 && !armed; ++wait) {
    bool allSet = true;
    for (int i = 0; i < 4; ++i) {
      uint32_t v = readBE32(base + kAddrs[i]);
      prev[i] = v;
      if (v == 0) allSet = false;
    }
    if (allSet) {
      for (int i = 0; i < 4; ++i) snapshot[i] = prev[i];
      armed = true;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  if (!armed) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[MODGLOB_WATCH] gave up waiting for full init (one or more globals never became non-zero)");
    return;
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[MODGLOB_WATCH] armed + snapshot taken: modA=0x%08X modB=0x%08X modC=0x%08X modD=0x%08X",
    snapshot[3], snapshot[1], snapshot[2], snapshot[0]);

  // iter-38: validate non-zero updates against the struct's vtable field.
  // iter-37 logs showed the slots get re-pointed 4+ times during boot
  // (modA: 0x894A71E0 → 0x894A7130 → 0x894B6AF0 → 0x83DF7F80, all from main
  // lr=0x827848A0). The FINAL pointer at gameloop_enter time lands on a
  // struct with vtable=NULL (TICK_DIAG val2=0) → per-frame work no-ops →
  // 0 DRAW_INDX. The earlier pointers (e.g. 0x894A71E0) DID have valid
  // vtables (0x82005BEC etc., now protected against memcpy clobber by
  // iter-37). So: when the slot transitions to a new pointer, peek at
  // *(new_va + 0). If it's not a plausible vtable (in image .rdata range,
  // not a sentinel), reject the update and restore the previous snapshot —
  // keep main pointing at the original good struct.
  auto plausibleVtable = [](uint32_t v) {
    if (v == 0) return false;
    if ((v >> 24) == 0xFFu) return false;          // 0xFFxxxxxx sentinels
    return v >= 0x82000000u && v < 0x82A00000u;    // image .text/.rdata range
  };

  unsigned restoreCount = 0;
  unsigned rejectCount = 0;
  while (true) {
    for (int i = 0; i < 4; ++i) {
      uint8_t* h = base + kAddrs[i];
      uint32_t v = readBE32(h);
      if (v != prev[i]) {
        uint32_t lr = sr::kernel::getMainGuestPC();
        uint32_t r1 = sr::kernel::getMainGuestR1();
        if (v == 0) {
          // Bad transition — restore from snapshot.
          writeBE32(h, snapshot[i]);
          ++restoreCount;
          if (restoreCount <= 16 || (restoreCount & (restoreCount - 1)) == 0) {
            SR_LOG_BOOT(sr::LogSeverity::Error,
              "[MODGLOB_WATCH] *** %s @0x%08X clobbered to 0; RESTORED to 0x%08X (#%u) — main lr=0x%08X r1=0x%08X",
              kNames[i], kAddrs[i], snapshot[i], restoreCount, lr, r1);
          }
          prev[i] = snapshot[i];
        } else {
          // Non-zero update — verify the new pointer's struct has a real
          // vtable before accepting. If not, this is a fallback-pool re-issue
          // (sub_8278D258 returning the same default-empty buffer); reject
          // and restore the snapshot.
          uint32_t newVtable = (v + 4 <= sr::kernel::getGuestSize())
                                 ? readBE32(base + v) : 0;
          if (!plausibleVtable(newVtable)) {
            writeBE32(h, snapshot[i]);
            ++rejectCount;
            if (rejectCount <= 16 || (rejectCount & (rejectCount - 1)) == 0) {
              SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[MODGLOB_WATCH] %s @0x%08X re-point 0x%08X -> 0x%08X REJECTED "
                "(new vtable=0x%08X looks bogus; kept snapshot 0x%08X) — main lr=0x%08X",
                kNames[i], kAddrs[i], prev[i], v, newVtable, snapshot[i], lr);
            }
            prev[i] = snapshot[i];
          } else {
            SR_LOG_BOOT(sr::LogSeverity::Info,
              "[MODGLOB_WATCH] %s @0x%08X updated 0x%08X -> 0x%08X "
              "(vtable=0x%08X — accepted; new snapshot) — main lr=0x%08X",
              kNames[i], kAddrs[i], prev[i], v, newVtable, lr);
            snapshot[i] = v;
            prev[i] = v;
          }
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(50));  // 20 kHz poll
  }
}

// iter-26 main-thread idle sampler: sample main's PPCContext at 1 Hz to find
// where main is parked once the game enters its post-tick=1000 idle. We have
// strong evidence main is "asleep" (only 6402 kernel calls in 95s in iter-25a)
// but we don't know the exact wait site. The sampler logs every second; if
// the same lr persists across N samples, that's the hang-site PC and we can
// hook the loop's caller (same playbook as iter-DOGMA-RE-18 → iter-19).
void mainThreadIdleSamplerMain() {
  // Wait until the harness has registered main's PPCContext.
  for (int i = 0; i < 600 && sr::kernel::getMainGuestPC() == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

  uint32_t prevLR = 0;
  unsigned stableCount = 0;
  uint64_t sampleNum = 0;
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ++sampleNum;
    uint32_t lr  = sr::kernel::getMainGuestPC();
    uint32_t ctr = sr::kernel::getMainGuestCtr();
    uint32_t r1  = sr::kernel::getMainGuestR1();
    uint32_t r3  = sr::kernel::getMainGuestR3();
    if (lr == prevLR) ++stableCount; else stableCount = 1;
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[MAIN_IDLE_SAMPLE] #%llu lr=0x%08X ctr=0x%08X r1=0x%08X r3=0x%08X stable=%u",
      (unsigned long long)sampleNum, lr, ctr, r1, r3, stableCount);
    // iter-433d: when main wedges (the shape-B stall: yield loop at 828A7988,
    // flow FSM frozen one tick after state-0 enter), dump the guest back-chain
    // so the wait-loop's CALLER names itself every roll — same bounded walk as
    // THREAD_FATAL (stack is mapped; reads bounded and fault-free).
    if (stableCount >= 8 && (stableCount == 8 || (stableCount & 31) == 0) &&
        r1 >= 0x40000000u && r1 < 0xC0000000u) {
      uint8_t* gbase = nullptr; size_t gsize = 0;
      getGlobalGuestMemory(&gbase, &gsize);
      if (gbase && r1 < gsize) {
        char chain[360]; int cn = 0; chain[0] = 0;
        uint32_t sp = r1; int words = 24;
        for (int hop = 0; hop < 8 && sp >= 0x40000000u && sp + 0x140 < gsize && words > 0; ++hop) {
          auto rd = [&](uint32_t va) -> uint32_t {
            const uint8_t* p = gbase + va;
            return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                   (uint32_t(p[2]) << 8) | uint32_t(p[3]);
          };
          const uint32_t next = rd(sp);
          for (uint32_t o = 4; o < 0x120 && words > 0; o += 4) {
            const uint32_t v = rd(sp + o);
            if (v >= 0x82000000u && v < 0x82990000u) {
              int w = snprintf(chain + cn, sizeof(chain) - cn, " %08X", v);
              if (w <= 0 || cn + w >= (int)sizeof(chain) - 1) { words = 0; break; }
              cn += w; --words;
            }
            if (next && sp + o >= next) break;
          }
          if (!next || next <= sp || next - sp > 0x10000u) break;
          sp = next;
        }
        SR_LOG_BOOT(sr::LogSeverity::Info,
          "[MAIN_IDLE_SAMPLE] wedge chain:%s", chain[0] ? chain : " (none)");
      }
    }
    prevLR = lr;
  }
}

void startMainThreadIdleSampler() {
  static std::atomic<bool> s_started{false};
  if (s_started.exchange(true)) return;
  std::thread(mainThreadIdleSamplerMain).detach();
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[MAIN_IDLE_SAMPLE] sampler thread spawned (1 Hz)");
}

void startModuleGlobalsPoller() {
  // iter-463 charter: the poller writes the modA-D slots back over the guest's
  // own zeroing / re-pointing - FORCING, master-gated at the launch.
  if (!sr::kernel::forcingAllowed()) return;
  static std::atomic<bool> s_started{false};
  if (s_started.exchange(true)) return;
  uint8_t* base = nullptr;
  size_t size = 0;
  getGlobalGuestMemory(&base, &size);
  if (!base) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[MODGLOB_WATCH] cannot start: guest memory not registered");
    return;
  }
  std::thread(moduleGlobalsPollerMain, base).detach();
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[MODGLOB_WATCH] poller thread spawned, watching modA-D at 0x8296D8D8/DC/E0/E4");
}

void startImageCorruptionPoller() {
  static std::atomic<bool> s_started{false};
  if (s_started.exchange(true)) return;

  uint8_t* base = nullptr;
  size_t size = 0;
  getGlobalGuestMemory(&base, &size);
  if (!base) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[CORRUPTION_WATCH] cannot start: guest memory not registered");
    return;
  }
  std::thread(imageCorruptionPollerMain, base).detach();
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[CORRUPTION_WATCH] poller thread spawned, watching *(0x8209A458)");
}

// ---------------------------------------------------------------------------
// iter-36: Module-struct content watcher.
//
// iter-24z's MODGLOB_WATCH protects the four pointer SLOTS at
// 0x8296D8D8/DC/E0/E4 — restoring them when zero'd. iter-35 found that the
// struct CONTENTS at the heap addresses those slots point at also get zeroed
// (which is why TICK_DIAG sees vtable=NULL even with the slot restored).
//
// This watcher is the analog of MODGLOB_WATCH but one level of indirection
// deeper. It snapshots modA and modB heap addresses after init, then 50 kHz-
// polls *(modA+0) and *(modB+0) (the vtable field). On any transition,
// captures main-thread guest PC/r1/etc. + a 32-deep history + surrounding
// bytes, so we can pinpoint the writer (same playbook as iter-23
// imageCorruptionPoller).
//
// Bounded log volume: first 4 + powers of 2 transitions per slot.
namespace {

void moduleStructWatchPollerMain(uint8_t* base) {
  uint8_t* slotA_host = base + 0x8296D8E4u;  // modA pointer slot
  uint8_t* slotB_host = base + 0x8296D8DCu;  // modB pointer slot

  auto readBE32 = [](uint8_t* h) -> uint32_t {
    return (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) |
           (uint32_t(h[2]) <<  8) |  uint32_t(h[3]);
  };

  // Wait for both slots to be populated (= sub_8218CFE8 has run its writers).
  uint32_t modA_va = 0;
  uint32_t modB_va = 0;
  for (int i = 0; i < 5000; ++i) {
    modA_va = readBE32(slotA_host);
    modB_va = readBE32(slotB_host);
    if (modA_va != 0 && modB_va != 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (modA_va == 0 || modB_va == 0) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[MODSTRUCT_WATCH] gave up waiting: modA=0x%08X modB=0x%08X", modA_va, modB_va);
    return;
  }

  // Snapshot the heap addresses where the vtable field lives.
  // Per iter-35: modA expected vtable = 0x82005bec, modB = 0x82005b98.
  uint8_t* vtblA_host = base + modA_va;
  uint8_t* vtblB_host = base + modB_va;
  constexpr uint32_t kExpectedA = 0x82005BECu;
  constexpr uint32_t kExpectedB = 0x82005B98u;

  // Wait briefly for the per-slot ctor to populate the vtable field. Don't
  // hard-fail if it never matches — we still want to watch transitions.
  for (int i = 0; i < 500; ++i) {
    if (readBE32(vtblA_host) == kExpectedA && readBE32(vtblB_host) == kExpectedB) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  uint32_t vA0 = readBE32(vtblA_host);
  uint32_t vB0 = readBE32(vtblB_host);
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[MODSTRUCT_WATCH] armed: modA=0x%08X *(modA+0)=0x%08X (expected 0x%08X) | "
    "modB=0x%08X *(modB+0)=0x%08X (expected 0x%08X)",
    modA_va, vA0, kExpectedA, modB_va, vB0, kExpectedB);

  // 32-deep main-thread context ring.
  constexpr int kHistorySize = 32;
  struct HistoryEntry { uint32_t lr, ctr, r3, r1, r31; };
  HistoryEntry history[kHistorySize] = {};
  int histIdx = 0;

  uint32_t prevA = vA0, prevB = vB0;
  unsigned hitsA = 0, hitsB = 0;

  // iter-39: active-restore for the vtable field. iter-38 found that the
  // struct contents at modA/modB heap addresses get partially memcpy'd over
  // by a downstream writer with valid-looking args (not caught by iter-37's
  // NULL guard). Mirror MODGLOB_WATCH's restore pattern at struct level.
  // Snapshot = the canonical vtable VA from iter-35 (we know what these
  // should be permanently). On any transition to a non-image-range value,
  // restore the snapshot.
  auto plausibleVtable = [](uint32_t v) {
    if (v == 0) return false;
    if ((v >> 24) == 0xFFu) return false;
    return v >= 0x82000000u && v < 0x82A00000u;
  };
  // Use the expected canonical vtables as the restore snapshots. If the
  // initial read disagrees, prefer the canonical value — we trust iter-35's
  // Ghidra mapping over a possibly-already-corrupted runtime read.
  uint32_t snapA = kExpectedA;
  uint32_t snapB = kExpectedB;
  unsigned restoreA = 0, restoreB = 0;

  auto writeBE32 = [](uint8_t* h, uint32_t v) {
    h[0] = uint8_t(v >> 24);
    h[1] = uint8_t(v >> 16);
    h[2] = uint8_t(v >>  8);
    h[3] = uint8_t(v      );
  };

  auto report = [&](const char* who, uint32_t modVa, uint32_t prev, uint32_t cur) {
    // Cross-thread snapshot: this poller runs on its own thread, so the TLS-
    // based getCurrentGuestPC() returns 0. Use the process-global accessor
    // populated by setMainGuestContext() (iter-24z).
    uint32_t lr  = sr::kernel::getMainGuestPC();
    uint32_t ctr = sr::kernel::getMainGuestCtr();
    uint32_t r3  = sr::kernel::getMainGuestR3();
    uint32_t r1  = sr::kernel::getMainGuestR1();
    uint32_t r31 = 0;  // no cross-thread r31 accessor; r1/r3 are sufficient signal
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[MODSTRUCT_WATCH] *** %s vtable @0x%08X+0: 0x%08X -> 0x%08X | "
      "main lr=0x%08X ctr=0x%08X r3=0x%08X r1=0x%08X r31=0x%08X",
      who, modVa, prev, cur, lr, ctr, r3, r1, r31);
    // Surrounding bytes — tells us if this is single-store vs memset/memcpy.
    uint8_t* h = base + modVa;
    auto rd = [&](int32_t off) {
      uint8_t* p = h + off;
      return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
             (uint32_t(p[2]) <<  8) |  uint32_t(p[3]);
    };
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[MODSTRUCT_WATCH]   surrounding: -0x10=0x%08X -8=0x%08X -4=0x%08X "
      "+0=0x%08X +4=0x%08X +8=0x%08X +0x10=0x%08X +0x40=0x%08X +0x80=0x%08X",
      rd(-0x10), rd(-8), rd(-4), rd(0), rd(4), rd(8), rd(0x10), rd(0x40), rd(0x80));
    // Recent main-thread context history.
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[MODSTRUCT_WATCH]   main history (%d-deep, newest first):", kHistorySize);
    for (int i = 0; i < kHistorySize; ++i) {
      int idx = (histIdx - 1 - i + kHistorySize) % kHistorySize;
      const auto& e = history[idx];
      if (e.lr == 0 && e.ctr == 0 && e.r1 == 0) continue;
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[MODSTRUCT_WATCH]     [-%2d] lr=0x%08X ctr=0x%08X r3=0x%08X r1=0x%08X r31=0x%08X",
        i, e.lr, e.ctr, e.r3, e.r1, e.r31);
    }
  };

  while (true) {
    history[histIdx] = {
      sr::kernel::getMainGuestPC(),
      sr::kernel::getMainGuestCtr(),
      sr::kernel::getMainGuestR3(),
      sr::kernel::getMainGuestR1(),
      0  // no cross-thread r31 accessor
    };
    histIdx = (histIdx + 1) % kHistorySize;

    uint32_t curA = readBE32(vtblA_host);
    uint32_t curB = readBE32(vtblB_host);

    // iter-39 ATTEMPTED active vtable restore here. REVERTED: the heap
    // allocator re-issues these addresses to other modules' constructors
    // (FMPhysics, etc.) during walker. The restore wrote modA's vtable
    // over FMPhysics's freshly-init'd vtable, causing FMPhysics's ctor to
    // AV and walker to hang. Restoring struct contents requires first
    // fixing the heap-reuse bug (iter-24z's TODO: populate sub_8278D258's
    // registry) so 0x894A71E0 stays modA's. Until then, observation only.
    (void)plausibleVtable; (void)snapA; (void)snapB; (void)restoreA; (void)restoreB;
    (void)writeBE32;

    if (curA != prevA) {
      ++hitsA;
      if (hitsA <= 4 || (hitsA & (hitsA - 1)) == 0) report("modA", modA_va, prevA, curA);
      prevA = curA;
    }
    if (curB != prevB) {
      ++hitsB;
      if (hitsB <= 4 || (hitsB & (hitsB - 1)) == 0) report("modB", modB_va, prevB, curB);
      prevB = curB;
    }

    std::this_thread::sleep_for(std::chrono::microseconds(20));
  }
}

}  // namespace

void startModuleStructWatch() {
  static std::atomic<bool> s_started{false};
  if (s_started.exchange(true)) return;
  uint8_t* base = nullptr;
  size_t size = 0;
  getGlobalGuestMemory(&base, &size);
  if (!base) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[MODSTRUCT_WATCH] cannot start: guest memory not registered");
    return;
  }
  std::thread(moduleStructWatchPollerMain, base).detach();
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[MODSTRUCT_WATCH] poller thread spawned (50 kHz, watches modA+0 and modB+0)");
}

}}  // namespace sr::kernel

// iter-463 charter: bootstrapAudioClient (iter-23 host-driven audio-client construction:
// guest-heap config struct, sub_826D12B8/sub_826D4D60 calls, a write to 0x8296EB80, plus a
// full image-range restore) was deleted. It had no callers; natural FMUI init owns the
// audio client.
