// GPU trace replay — see gpu_trace_replay.hpp. Format ground truth:
// reference/xenia-canary/src/xenia/gpu/trace_protocol.h + trace_reader.cc
// (ParseTrace is the authoritative skip/advance arithmetic) + trace_player.cc
// (playback semantics: MemoryRead prefills applied in stream order, MemoryWrite
// ignored, PacketStart copies bytes then PacketEnd executes, kSwap = frame end).
//
// PM4 handling note: this executor intentionally mirrors (not calls) the live
// indirect-buffer branch of parsePM4Region in null_renderer.cpp — the live path
// stays conservative for the game, while this replay executor is the growth
// surface for Xenia-fidelity packet handling. Both feed the same
// XenosRegisterFile + DrawCommandQueue, so fixes proven here transfer directly.

#include "gpu_trace_replay.hpp"
#include "native_device.hpp"
#include "d3d12_backend.hpp"
#include "null_renderer.hpp"
#include "pm4_engine.hpp"
#include "pm4_coverage.hpp"   // NATIVE N0.2 (SR_PM4_COVERAGE)
#include "shader_runtime_xlat.hpp"
#include "gamma_ramp.hpp"
#include "res_scale.hpp"
#include "resource_ab.hpp"  // mem.resources frozen-pass A/B: dual mode is refused here
#include "resource_memory.hpp"
#include "xenos_state.hpp"
#include "xenos_tiling.hpp"
#include "xenos_ucode.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/gpu_write_watch.hpp"
#include "../app/log.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace sr {
namespace gfx {

namespace {

// ── Xenia trace_protocol.h mirror (fields after the leading u32 type tag) ────
constexpr uint32_t kTraceVersion = 1;

enum class TraceCmd : uint32_t {
  kPrimaryBufferStart = 0,
  kPrimaryBufferEnd = 1,
  kIndirectBufferStart = 2,
  kIndirectBufferEnd = 3,
  kPacketStart = 4,
  kPacketEnd = 5,
  kMemoryRead = 6,
  kMemoryWrite = 7,
  kEdramSnapshot = 8,
  kEvent = 9,
  kRegisters = 10,
  kGammaRamp = 11,
};

#pragma pack(push, 1)
struct TraceHeaderRest {          // after u32 version
  char build_commit_sha[40];      // NOT nul-terminated
  uint32_t title_id;
};
struct BufferStartRest { uint32_t base_ptr; uint32_t count; };      // 8
struct PacketStartRest { uint32_t base_ptr; uint32_t count; };      // 8
struct MemoryRest {                                                  // 16
  uint32_t base_ptr;
  uint32_t encoding_format;       // 0=None, 1=Snappy
  uint32_t encoded_length;
  uint32_t decoded_length;
};
struct EdramRest { uint32_t encoding_format; uint32_t encoded_length; };  // 8
struct EventRest { uint32_t event_type; };                          // 4 (0 = kSwap)
struct RegistersRest {                                               // 20
  uint32_t first_register;
  uint32_t register_count;
  uint8_t execute_callbacks;
  uint8_t pad[3];
  uint32_t encoding_format;
  uint32_t encoded_length;
};
struct GammaRampRest {                                               // 12
  uint8_t rw_component;
  uint8_t pad[3];
  uint32_t encoding_format;
  uint32_t encoded_length;
};
#pragma pack(pop)

static_assert(sizeof(TraceHeaderRest) == 44, "TraceHeader layout");
static_assert(sizeof(MemoryRest) == 16, "MemoryCommand layout");
static_assert(sizeof(RegistersRest) == 20, "RegistersCommand layout");
static_assert(sizeof(GammaRampRest) == 12, "GammaRampCommand layout");

// ── Minimal snappy decompressor (format: format_description.txt upstream) ────
// Decompress-only; enough for xenia's RawCompress output. Returns false on any
// malformed input rather than over-reading.
bool snappyDecompress(const uint8_t* src, size_t srcLen,
                      uint8_t* dst, size_t dstCap, size_t* outLen) {
  size_t sp = 0;
  // Preamble: varint32 uncompressed length.
  uint64_t declaredLen = 0;
  int shift = 0;
  while (true) {
    if (sp >= srcLen || shift > 28) return false;
    uint8_t b = src[sp++];
    declaredLen |= static_cast<uint64_t>(b & 0x7F) << shift;
    if (!(b & 0x80)) break;
    shift += 7;
  }
  if (declaredLen > dstCap) return false;
  size_t dp = 0;
  while (sp < srcLen) {
    uint8_t tag = src[sp++];
    uint32_t kind = tag & 0x3;
    if (kind == 0) {                    // literal
      size_t len = (tag >> 2) + 1;
      if (len > 60) {
        size_t extra = len - 60;        // 1..4 length bytes, little-endian
        if (sp + extra > srcLen) return false;
        len = 0;
        for (size_t i = 0; i < extra; i++) len |= static_cast<size_t>(src[sp + i]) << (8 * i);
        len += 1;
        sp += extra;
      }
      if (sp + len > srcLen || dp + len > declaredLen) return false;
      std::memcpy(dst + dp, src + sp, len);
      sp += len;
      dp += len;
    } else {                            // copy
      size_t len = 0, offset = 0;
      if (kind == 1) {
        len = 4 + ((tag >> 2) & 0x7);
        if (sp >= srcLen) return false;
        offset = (static_cast<size_t>(tag >> 5) << 8) | src[sp++];
      } else if (kind == 2) {
        len = (tag >> 2) + 1;
        if (sp + 2 > srcLen) return false;
        offset = src[sp] | (static_cast<size_t>(src[sp + 1]) << 8);
        sp += 2;
      } else {
        len = (tag >> 2) + 1;
        if (sp + 4 > srcLen) return false;
        offset = src[sp] | (static_cast<size_t>(src[sp + 1]) << 8) |
                 (static_cast<size_t>(src[sp + 2]) << 16) |
                 (static_cast<size_t>(src[sp + 3]) << 24);
        sp += 4;
      }
      if (offset == 0 || offset > dp || dp + len > declaredLen) return false;
      // Byte-wise copy: overlapping copies are legal LZ77 semantics.
      for (size_t i = 0; i < len; i++) { dst[dp] = dst[dp - offset]; dp++; }
    }
  }
  if (dp != declaredLen) return false;
  *outLen = dp;
  return true;
}

uint32_t readBE32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// ── Replay state ─────────────────────────────────────────────────────────────
struct ReplayCtx {
  uint8_t* guestBase = nullptr;   // 4 GB flat guest memory (replay-owned)
  D3D12Backend* d3d12 = nullptr;
  GpuTraceReplayStats st;
  // iter-358h: the PM4 parse/execute layer (opcode table, register writes,
  // shader ucode capture, [VFETCH] census, resolve latch) lives in the shared
  // Pm4Engine — the same engine the live VdSwap ring scan drives.
  Pm4Engine eng;
  // last executed packet (for frame fb extraction at kEvent/kSwap)
  uint32_t lastPacketPhys = 0;
  uint32_t lastPacketCount = 0;
  uint32_t maxFrames = 0;         // 0 = unlimited
  uint32_t dumpEvery = 60;
};

// Trace physical address -> host pointer. Same mapping as the live PM4 parser:
// physical P is visible at guest VA 0x80000000 + P. Mask to the 512 MB console
// physical space so a stray high bit can't escape guest memory.
inline uint8_t* physToHost(ReplayCtx& rc, uint32_t phys) {
  return rc.guestBase + 0x80000000ull + (phys & 0x1FFFFFFFu);
}

// writeReg / onShaderLoad / emitDrawFromFetchState / executePM4 moved to the
// shared Pm4Engine (gfx/pm4_engine.cpp) so the live VdSwap ring scan runs the
// SAME golden-trace-proven parser (iter-358h).

void dumpFrameBmp(const uint8_t* fb, int w, int h, int pitch, uint32_t frameIdx) {
  char name[128];
  snprintf(name, sizeof(name), "replay_frame_%u.bmp", frameIdx);
  FILE* fp = fopen(name, "wb");
  if (!fp) return;
  const uint32_t imgSize = static_cast<uint32_t>(w) * static_cast<uint32_t>(h) * 4u;
  uint8_t fh[14] = {'B', 'M'};
  uint32_t fsz = 14 + 40 + imgSize;
  std::memcpy(fh + 2, &fsz, 4);
  uint32_t off = 54;
  std::memcpy(fh + 10, &off, 4);
  fwrite(fh, 1, 14, fp);
  uint8_t ih[40] = {};
  uint32_t ihSize = 40;
  std::memcpy(ih, &ihSize, 4);
  int32_t bw = w, bh = -h;  // negative = top-down
  std::memcpy(ih + 4, &bw, 4);
  std::memcpy(ih + 8, &bh, 4);
  uint16_t planes = 1, bpp = 32;
  std::memcpy(ih + 12, &planes, 2);
  std::memcpy(ih + 14, &bpp, 2);
  std::memcpy(ih + 20, &imgSize, 4);
  fwrite(ih, 1, 40, fp);
  for (int y = 0; y < h; y++) fwrite(fb + static_cast<size_t>(y) * pitch, 1, static_cast<size_t>(w) * 4, fp);
  fclose(fp);
}

void onSwapEvent(ReplayCtx& rc) {
  rc.st.frames++;
  // The swap PM4 packet (executed just before this event) carries
  // payload[1..3] = frontbuffer physical ptr, width, height.
  uint32_t fbPhys = 0, w = 0, h = 0;
  if (rc.lastPacketPhys && rc.lastPacketCount >= 4) {
    const uint8_t* pkt = physToHost(rc, rc.lastPacketPhys);
    fbPhys = readBE32(pkt + 4);
    w = readBE32(pkt + 8);
    h = readBE32(pkt + 12);
  }
  if (w < 32 || w > 4096 || h < 32 || h > 4096) {
    // Fallback: resolve latch (RB_COPY_DEST_BASE), assume 720p.
    fbPhys = rc.eng.lastNonZeroCopyDestBase;
    w = 1280; h = 720;
  }
  if (!fbPhys) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[GPU:REPLAY] frame %u: no frontbuffer address (swap pkt + resolve latch both empty)",
                rc.st.frames);
    return;
  }
  // iter-358j — Xenia model: the swap texture is described by TEXTURE FETCH
  // CONSTANT 0, loaded by VdSwap's type-0 packet just before the swap packet
  // (D3D12TextureCache::RequestSwapTexture re-reads it; the PM4 payload
  // ptr/w/h are informational). Read tiled + storage pitch from it.
  const uint32_t f0 = getXenosRegisters().read(0x4800);
  const uint32_t f1 = getXenosRegisters().read(0x4801);
  const uint32_t f5 = getXenosRegisters().read(0x4805);
  const bool fetchValid = (f0 & 3u) == 2u;
  const bool tiled = fetchValid && ((f0 >> 31) & 1u) != 0;
  uint32_t pitchTexels = fetchValid ? (((f0 >> 22) & 0x1FFu) << 5) : 0;
  if (pitchTexels < w) pitchTexels = w;
  const uint8_t* fb = physToHost(rc, fbPhys);
  int pitch = static_cast<int>(pitchTexels) * 4;
  if (rc.st.frames <= 3)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[GPU:REPLAY] frame %u swap: fbPhys=0x%08X %ux%u pitchTexels=%u tiled=%d "
                "fmt=%u endian=%u dim=%u swiz=0x%03X fetchValid=%d",
                rc.st.frames, fbPhys, w, h, pitchTexels, tiled ? 1 : 0,
                f1 & 0x3Fu, (f1 >> 6) & 3u, (f5 >> 9) & 3u,
                (getXenosRegisters().read(0x4803) >> 1) & 0xFFFu, fetchValid ? 1 : 0);
  // iter-358k: OUR renderer executes this frame's draws — raise the resolve so
  // the geometry path inside presentFramebuffer copies the rendered RT back
  // into the guest framebuffer (linear rows), exactly where the console's
  // resolve would land it.
  const bool requestedResolve = (rc.d3d12 != nullptr);
  if (requestedResolve) setResolveRequested();
  // P1 (SR_PM4_RTCACHE): the EDRAM path writes the front buffer TILED into
  // guest memory, as the console does. Poison it first: the trace restored
  // Xenia's own final frame into these bytes, and a missing or partial resolve
  // of ours must show up as magenta, never as Xenia's picture (the iter-359g
  // false-positive class).
  const bool pm4 = pm4RtCacheEnabled();
#if SR_ORACLE
  if (rc.d3d12 && rc.d3d12->replayOrderedRestores()) rc.d3d12->replayPrepareTail();
#endif
  if (pm4 && fb && tiled) {
    const size_t span = xenos_tiling::tiledSpanBytes32(pitchTexels, h);
    if (rc.d3d12 && (memres::resourceMode() || rc.d3d12->replayOrderedRestores())) {
      rc.d3d12->replayPoisonResource(fbPhys, uint32_t(span));
    } else {
      for (size_t i = 0; i + 4 <= span; i += 4) {
        uint8_t* p = const_cast<uint8_t*>(fb) + i;
        p[0] = 0xFF; p[1] = 0x00; p[2] = 0xFF; p[3] = 0xFF;
      }
    }
  }
  // iter-421: publish the frame boundary exactly as the live VdSwap scan does,
  // or presentFramebuffer's per-swap drain gate never opens again after frame 1
  // and the draw ring silently overflows (39k drops on the 30s stream trace).
  bumpSwapFrameSeq();
  if (rc.d3d12)
    rc.d3d12->presentFramebuffer(const_cast<uint8_t*>(fb), static_cast<int>(w),
                                 static_cast<int>(h), pitch, tiled);
  // iter-460 (SR_PM4_SHMEM): our resolve of the front buffer landed in the
  // shared memory; the dump below reads guest RAM.
  std::vector<uint8_t> nativeFront;
  if (rc.d3d12 && pm4 && pm4ShmEnabled()) {
    const uint32_t bytes = uint32_t(tiled ? xenos_tiling::tiledSpanBytes32(pitchTexels, h)
                                         : size_t(pitchTexels) * h * 4u);
    if (memres::resourceMode() && !resscale::get().scaled()) {
      rc.d3d12->resourceCpuSnapshot(fbPhys, bytes, nativeFront);
      fb = nativeFront.data();
    } else if (!memres::resourceMode()) {
      rc.d3d12->pm4ShmSyncToGuest(fbPhys, bytes);
    }
  }
  // If the geometry path consumed the request, the guest fb now holds OUR
  // LINEAR resolve; otherwise it still holds the trace's tiled restore.
  const bool weResolved = requestedResolve && !getResolveRequested();
  if (!weResolved) getAndClearResolveRequested();
  // iter-464 resscale: at a draw resolution scale the resolved front buffer exists
  // only in the scaled resolve memory - dump the presented internal-resolution
  // frame (scan-out + gamma ramp, gfx/d3d12_pm4_shmem.cpp pass 1), the same frame
  // Xenia's scaled trace dump writes. SR_REPLAY_DUMP_OUTPUT=1 takes this path at
  // 1x too (a check that the GPU scan-out equals the CPU dump below).
  static const bool s_dumpOutput = [] {
    const char* e = std::getenv("SR_REPLAY_DUMP_OUTPUT");
    return e && e[0] == '1';
  }();
  const bool dumpThis = rc.st.frames <= 3 || (rc.dumpEvery && rc.st.frames % rc.dumpEvery == 0);
  if (dumpThis && rc.d3d12 && pm4 && (resscale::get().scaled() || s_dumpOutput)) {
    std::vector<uint8_t> out;
    uint32_t ow = 0, oh = 0;
    if (rc.d3d12->pm4ScanOutForDump(fbPhys, w, h, pitchTexels, tiled) &&
        rc.d3d12->pm4ReadOutput(out, ow, oh)) {
      dumpFrameBmp(out.data(), int(ow), int(oh), int(ow) * 4, rc.st.frames);
      SR_LOG_BOOT(sr::LogSeverity::Info, "[GPU:REPLAY] frame %u dumped from the output texture %ux%u",
                  rc.st.frames, ow, oh);
    } else {
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[GPU:REPLAY] frame %u: no output texture to dump",
                  rc.st.frames);
    }
  } else if (dumpThis) {
    // replay_frame_N.bmp = the artifact compared against the Xenia oracle -
    // what the console SCANS OUT, i.e. after the display gamma ramp (an 8_8_8_8
    // front buffer goes through the 256-entry table; xenia
    // d3d12_command_processor.cc:2227). SR_GAMMA_RAMP=0 dumps the raw buffer.
    static std::vector<uint8_t> s_frame;
    s_frame.resize(static_cast<size_t>(w) * h * 4u);
    if ((weResolved && !pm4) || !tiled) {
      for (uint32_t y = 0; y < h; y++)
        std::memcpy(s_frame.data() + size_t(y) * w * 4u, fb + size_t(y) * pitch, size_t(w) * 4u);
    } else {
      xenos_tiling::untile32(s_frame.data(), fb, pitchTexels, w, h);
    }
    static const bool s_gamma = [] {
      const char* e = std::getenv("SR_GAMMA_RAMP");
      return !e || e[0] != '0';
    }();
    const uint32_t fbFmt = f1 & 0x3Fu;
    if (s_gamma && pm4 && fbFmt == 6u) {
      // Dump bytes are B, G, R, A (the BMP order the rest of the tooling reads).
      const auto& ramp = sr::gfx::gamma::displayRamp();
      for (size_t i = 0; i < s_frame.size(); i += 4) {
        s_frame[i + 0] = ramp.to8(ramp.table10(s_frame[i + 0], 2));
        s_frame[i + 1] = ramp.to8(ramp.table10(s_frame[i + 1], 1));
        s_frame[i + 2] = ramp.to8(ramp.table10(s_frame[i + 2], 0));
      }
    } else if (s_gamma && pm4 && rc.st.frames <= 1) {
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[GPU:REPLAY] front buffer fmt %u: gamma ramp not applied (PWL path not ported)", fbFmt);
    }
    dumpFrameBmp(s_frame.data(), static_cast<int>(w), static_cast<int>(h), static_cast<int>(w) * 4,
                 rc.st.frames);
  }
  // Publish so the main loop / frame-dump machinery can re-present after replay.
  sr::kernel::setGuestFramebufferVA(0x80000000u + (fbPhys & 0x1FFFFFFFu),
                                    static_cast<int>(w), static_cast<int>(h), pitch, tiled);
  if (rc.st.frames <= 5 || rc.st.frames % 100 == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[GPU:REPLAY] frame %u presented: fbPhys=0x%08X %ux%u (packets=%" PRIu64
                " draws=%" PRIu64 " regWrites=%" PRIu64 ")",
                rc.st.frames, fbPhys, w, h, rc.eng.packets, rc.eng.draws, rc.eng.regWrites);
}

bool readExact(FILE* f, void* dst, size_t n) { return fread(dst, 1, n, f) == n; }

}  // namespace

int runGpuTraceReplay(const char* tracePath, D3D12Backend* d3d12,
                      GpuTraceReplayStats* outStats) {
  // iter-445 C7 (audit blocker 6): BELT AND BRACES with tools/replay_ab.ps1.
  // This fixture owns the MAIN thread and has no hle::execute producer, so it
  // never needs an open frame window. With the frame list default-ON it would
  // instead lose renderGuestGeometry entirely (the geometry branch is skipped
  // under the gate) while the skip's getAndClearResolveRequested() would make
  // the fixture believe our resolve landed - so it would dump the trace's OWN
  // restored framebuffer and score it as a pass. That is the iter-359g false
  // positive. Force the gate off for this process before the backend reads it.
  _putenv_s("SR_HLE_FRAME_LIST", "0");
  // NATIVE N4a: a fixture is one frame - the native resolve pipelines must be built before
  // its first resolve, not on the background thread the live game uses.
  _putenv_s("SR_NATIVE_RESOLVE_SYNC", "1");
  // mem.resources frozen-pass A/B (gfx/resource_ab.hpp; Astra review1 P2-7): dual mode is a LIVE per-present A/B. The
  // replay's front-buffer routing (the poison, the native snapshot vs the model's guest sync) reads resourceMode() and
  // this fixture latches no frame-set arm: refused, never half-toggled.
  if (memres::resourceAbDual()) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[GPU:REPLAY] SR_AB_TOGGLE=SR_MEM_RESOURCES (the frozen-pass A/B's dual mode) is "
                "a live-roll A/B only: the trace replay refuses it");
    return 2;
  }
  FILE* f = fopen(tracePath, "rb");
  if (!f) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[GPU:REPLAY] cannot open trace '%s'", tracePath);
    return 2;
  }
  uint32_t version = 0;
  TraceHeaderRest hdr{};
  if (!readExact(f, &version, 4) || !readExact(f, &hdr, sizeof(hdr)) || version != kTraceVersion) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[GPU:REPLAY] '%s' is not a v%u .xtr trace (version=%u)", tracePath,
                kTraceVersion, version);
    fclose(f);
    return 3;
  }
  char sha[41] = {};
  std::memcpy(sha, hdr.build_commit_sha, 40);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[GPU:REPLAY] trace open: title=%08X xenia-sha=%.12s file=%s",
              hdr.title_id, sha, tracePath);

  ReplayCtx rc;
  rc.d3d12 = d3d12;
  rc.eng.user = &rc;
  rc.eng.physToHost = [](void* user, uint32_t phys) -> uint8_t* {
    return physToHost(*static_cast<ReplayCtx*>(user), phys);
  };
  if (const char* e = std::getenv("SR_GPU_TRACE_REPLAY_MAXFRAMES"))
    rc.maxFrames = static_cast<uint32_t>(strtoul(e, nullptr, 0));
  if (const char* e = std::getenv("SR_GPU_TRACE_REPLAY_DUMP_EVERY"))
    rc.dumpEvery = static_cast<uint32_t>(strtoul(e, nullptr, 0));

  // Flat 4 GB guest memory, replay-owned (same shape as the live harness alloc).
  constexpr size_t kGuestSize = 0x100000000ull;
#if defined(_WIN32)
  rc.guestBase = static_cast<uint8_t*>(
      VirtualAlloc(reinterpret_cast<LPVOID>(0x2A00000000ULL), kGuestSize,
                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (!rc.guestBase)
    rc.guestBase = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kGuestSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#endif
  if (!rc.guestBase) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[GPU:REPLAY] guest memory alloc failed");
    fclose(f);
    return 4;
  }
  sr::kernel::setGuestMemory(rc.guestBase, kGuestSize);
  // iter-358k: also publish the arena as the PROCESS-GLOBAL guest memory —
  // the D3D12 resolve/untile bounds checks use getGlobalGuestMemory(), which
  // only registerExCreateThreadRunner() sets; without this every replay
  // resolve write is rejected as out-of-range.
  sr::kernel::registerExCreateThreadRunner(nullptr, rc.guestBase, kGuestSize);

  // iter-463: this backend is replaying - start the supplied-page map empty.
  if (rc.d3d12) {
    rc.d3d12->replayNoteSupplied(0, 0);
    rc.d3d12->nbReplayBegin();
  }

  // NATIVE mode B 1a (SR_REPLAY_NRT=1): the trace's .nrt sidecar - one record per draw bracket
  // the live run took natively. In order of execution: the bracket's packets are NOT executed;
  // at its DRAW packet the registers it wrote come from the record's native projection
  // (native_device nrtApply), then the DRAW runs. Without the gate the same .xtr replays as
  // plain PM4 (the oracle P).
  std::vector<uint8_t> nrt;
  size_t nrtNext = 0, nrtUsed = 0, nrtSkipped = 0;
  const size_t nrtRec = sr::native::dev::nrtRecordBytes();
  if (const char* e = std::getenv("SR_REPLAY_NRT"); e && e[0] == '1') {
    const std::string np = std::string(tracePath) + ".nrt";
    if (FILE* g = fopen(np.c_str(), "rb")) {
      fseek(g, 0, SEEK_END);
      const long n = ftell(g);
      fseek(g, 0, SEEK_SET);
      nrt.resize(n > 0 ? size_t(n) : 0u);
      if (!nrt.empty() && fread(nrt.data(), 1, nrt.size(), g) != nrt.size()) nrt.clear();
      fclose(g);
    }
    SR_LOG_BOOT(sr::LogSeverity::Info, "[GPU:REPLAY] native replay: %zu records from %s.nrt", nrt.size() / nrtRec,
                tracePath);
  }
  std::vector<uint8_t> scratch;      // compressed payload staging
  std::vector<uint8_t> decodeBuf;    // decompressed staging (registers etc.)
  uint32_t pendingPacketPhys = 0, pendingPacketCount = 0;
  bool ok = true;

  // SR_GPU_TRACE_REPLAY_LOOPS=N: replay the whole trace N times in one process
  // (backend caches kept) - the steady-state cost of a frame, not the first.
  uint32_t loops = 1;
  if (const char* e = std::getenv("SR_GPU_TRACE_REPLAY_LOOPS"))
    loops = std::max<uint32_t>(1u, static_cast<uint32_t>(strtoul(e, nullptr, 0)));
  const long bodyStart = ftell(f);
  for (uint32_t loop = 0; loop < loops && ok; loop++) {
  if (loop) fseek(f, bodyStart, SEEK_SET);
  // iter-463: a later pass sees every runtime shader translation an earlier pass requested
  // (async mode drops the draws of pass 1; pass 2 must draw them - a deterministic proof).
  if (loop && sr::gfx::rtxlat::enabled() && !sr::gfx::rtxlat::waitIdle(300000))
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[GPU:REPLAY] runtime shader translation still busy after 300 s at pass %u",
                loop + 1);
  uint32_t tag = 0;
  while (readExact(f, &tag, 4)) {
    if (rc.maxFrames && rc.st.frames >= rc.maxFrames) break;
    switch (static_cast<TraceCmd>(tag)) {
      case TraceCmd::kPrimaryBufferStart:
      case TraceCmd::kIndirectBufferStart: {
        BufferStartRest b{};
        if (!(ok = readExact(f, &b, sizeof(b)))) break;
        // writer always records count=0 here; nothing to skip (trace_reader.cc)
        break;
      }
      case TraceCmd::kPrimaryBufferEnd:
      case TraceCmd::kIndirectBufferEnd:
        break;  // tag only
      case TraceCmd::kPacketStart: {
        PacketStartRest p{};
        if (!(ok = readExact(f, &p, sizeof(p)))) break;
        size_t bytes = static_cast<size_t>(p.count) * 4;
        if (bytes > 0x400000) { ok = false; break; }  // sanity: 4 MB packet cap
        if (!(ok = readExact(f, physToHost(rc, p.base_ptr), bytes))) break;
        pendingPacketPhys = p.base_ptr;
        pendingPacketCount = p.count;
        break;
      }
      case TraceCmd::kPacketEnd: {
        if (pendingPacketCount) {
          bool nativeSkip = false;
          if (nrtNext * nrtRec + nrtRec <= nrt.size()) {
            const uint8_t* rec = nrt.data() + nrtNext * nrtRec;
            const int role = sr::native::dev::nrtPacket(rec, pendingPacketPhys);
            if (role == 3) {
              nrtNext++;
              nrtUsed++;
            } else if (role == 1) {
              nativeSkip = true;
              nrtSkipped++;
            }
          }
          if (!nativeSkip)
          rc.eng.execute(physToHost(rc, pendingPacketPhys),
                         static_cast<size_t>(pendingPacketCount) * 4, 0);
          // P1: Xenia's command processor reads a draw's memory AT the draw; the
          // stream's later MemoryRead restores reuse the same ring addresses
          // (the UI vertex ring), so a swap-time drain decodes the frame's LAST
          // contents for every earlier draw. Translate now.
          if (rc.d3d12 && pm4RtCacheEnabled()) rc.d3d12->pm4DrainNow();
          rc.lastPacketPhys = pendingPacketPhys;
          rc.lastPacketCount = pendingPacketCount;
          pendingPacketPhys = pendingPacketCount = 0;
        }
        break;
      }
      case TraceCmd::kMemoryRead:
      case TraceCmd::kMemoryWrite: {
        MemoryRest m{};
        if (!(ok = readExact(f, &m, sizeof(m)))) break;
        if (static_cast<TraceCmd>(tag) == TraceCmd::kMemoryWrite ||
            m.decoded_length == 0 || m.decoded_length > 0x20000000) {
          // MemoryWrite is ignored on playback (trace_player.cc): the replayed
          // packets reproduce the write. Oversize decoded lengths are malformed.
          ok = (fseek(f, static_cast<long>(m.encoded_length), SEEK_CUR) == 0);
          break;
        }
        const uint32_t phys = m.base_ptr & 0x1FFFFFFFu;
        // Reject a malformed payload before writing the guest arena. Page
        // snapshots below must describe exactly the successful decoded write.
        if (uint64_t(phys) + m.decoded_length > 0x20000000ull ||
            (m.encoding_format == 0 && m.encoded_length != m.decoded_length)) {
          ok = false;
          break;
        }
        uint8_t* dest = physToHost(rc, m.base_ptr);
        sr::kernel::gww::HostWriteScope writeScope(0x80000000u | phys, m.decoded_length);
        bool restored = true;
        if (m.encoding_format == 0) {
          // iter-466 cp lane: fread fills guest memory in kernel mode - release the pages the
          // GPU write tracker protected first (SR_SHM_WRITE_TRACK; a no-op when it is off).
          if (!(ok = readExact(f, dest, m.encoded_length))) break;
        } else {
          scratch.resize(m.encoded_length);
          if (!(ok = readExact(f, scratch.data(), m.encoded_length))) break;
          size_t got = 0;
          if (!snappyDecompress(scratch.data(), scratch.size(), dest,
                                m.decoded_length, &got) || got != m.decoded_length) {
            rc.st.snappyFailures++;
            restored = false;
          }
        }
        if (restored && memres::resourceMode())
          memres::resourceMemory().noteCpu(m.base_ptr & 0x1FFFFFFFu,
                                         m.encoding_format == 0 ? m.encoded_length : m.decoded_length,
                                         memres::Src::kReadFile);
#if SR_ORACLE
        if (restored && rc.d3d12) rc.d3d12->replayRestoredRange(phys, m.decoded_length);
#endif
        rc.st.memReadCmds++;
        rc.st.memReadBytes += m.decoded_length;
        // The trace rewrote guest memory: textures over it must re-validate.
        if (rc.d3d12) rc.d3d12->invalidateGuestRange(m.base_ptr, m.decoded_length);
        // iter-463: which pages the trace supplied (an array whose mip region it
        // never supplied loads base-only - D3D12Backend::replaySupplied).
        if (rc.d3d12) rc.d3d12->replayNoteSupplied(m.base_ptr, m.decoded_length);
        // iter-358j diagnostic: does the trace ever restore the front-buffer
        // region (0x1F130000 + 1280x736x4)? If not, our fb dump shows leftover
        // arena content, not the menu image (Xenia re-RENDERS the fb at replay).
        {
          constexpr uint32_t kFbLo = 0x1F130000u, kFbHi = 0x1F130000u + 1280u * 736u * 4u;
          if (m.base_ptr < kFbHi && m.base_ptr + m.decoded_length > kFbLo) {
            static int s_fbRestoreLog = 0;
            if (++s_fbRestoreLog <= 10)
              SR_LOG_BOOT(sr::LogSeverity::Info,
                          "[GPU:REPLAY] MemoryRead intersects FB: base=0x%08X len=0x%X",
                          m.base_ptr, m.decoded_length);
          }
        }
        break;
      }
      case TraceCmd::kEdramSnapshot: {
        EdramRest e{};
        if (!(ok = readExact(f, &e, sizeof(e)))) break;
        if (rc.d3d12 && pm4RtCacheEnabled()) {
          // P1: the frame starts from the EDRAM Xenia captured (trace_player.cc
          // RestoreEdramSnapshot) - a resolve of a surface the frame has not
          // drawn yet copies LAST frame's pixels, not zeros.
          constexpr size_t kEdramBytes = 10u << 20;
          scratch.resize(e.encoded_length);
          if (!(ok = readExact(f, scratch.data(), e.encoded_length))) break;
          std::vector<uint8_t> snap(kEdramBytes);
          size_t got = 0;
          if (e.encoding_format == 0) {
            got = std::min(scratch.size(), snap.size());
            std::memcpy(snap.data(), scratch.data(), got);
          } else if (!snappyDecompress(scratch.data(), scratch.size(), snap.data(), snap.size(),
                                       &got)) {
            rc.st.snappyFailures++;
          }
          if (got == kEdramBytes) rc.d3d12->pm4RestoreEdram(snap.data(), got);
          break;
        }
        // Without the EDRAM path there is nothing to restore into.
        ok = (fseek(f, static_cast<long>(e.encoded_length), SEEK_CUR) == 0);
        break;
      }
      case TraceCmd::kEvent: {
        EventRest ev{};
        if (!(ok = readExact(f, &ev, sizeof(ev)))) break;
        if (ev.event_type == 0) onSwapEvent(rc);  // kSwap
        break;
      }
      case TraceCmd::kRegisters: {
        RegistersRest r{};
        if (!(ok = readExact(f, &r, sizeof(r)))) break;
        if (r.register_count > 0x20000) { ok = false; break; }
        size_t decodedBytes = static_cast<size_t>(r.register_count) * 4;
        decodeBuf.resize(decodedBytes);
        if (r.encoding_format == 0) {
          if (!(ok = readExact(f, decodeBuf.data(), r.encoded_length))) break;
        } else {
          scratch.resize(r.encoded_length);
          if (!(ok = readExact(f, scratch.data(), r.encoded_length))) break;
          size_t got = 0;
          if (!snappyDecompress(scratch.data(), scratch.size(), decodeBuf.data(),
                                decodedBytes, &got) || got != decodedBytes) {
            rc.st.snappyFailures++;
            break;
          }
        }
        // Register payload is host-endian u32 (xenia register_file values).
        const uint32_t* vals = reinterpret_cast<const uint32_t*>(decodeBuf.data());
        for (uint32_t i = 0; i < r.register_count; i++) {
          const uint32_t reg = r.first_register + i;
          // A state restore without callbacks must not replay the DC_LUT write
          // side effects (xenia RestoreRegisters(..., execute_callbacks)); the
          // ramp itself arrives as kGammaRamp.
          if (!r.execute_callbacks && reg >= sr::gfx::gamma::kRegRwMode &&
              reg <= sr::gfx::gamma::kRegWriteEnMask) {
            getXenosRegisters().write(reg, vals[i]);
            continue;
          }
          rc.eng.writeReg(reg, vals[i]);
        }
        break;
      }
      case TraceCmd::kGammaRamp: {
        // The display ramp state at trace start: 256 DC_LUT_30_COLOR then
        // 3 x 128 DC_LUT_PWL_DATA, host-endian (trace_writer.cc:315).
        GammaRampRest g{};
        if (!(ok = readExact(f, &g, sizeof(g)))) break;
        std::vector<uint8_t> enc(g.encoded_length);
        if (!(ok = readExact(f, enc.data(), enc.size()))) break;
        constexpr size_t kRampBytes = sizeof(uint32_t) * (256 + 3 * 128);
        std::vector<uint8_t> dec(kRampBytes);
        size_t n = 0;
        const bool decoded = g.encoding_format == 1
                                 ? snappyDecompress(enc.data(), enc.size(), dec.data(), dec.size(), &n)
                                 : (n = enc.size(), n == kRampBytes && (std::memcpy(dec.data(), enc.data(), n), true));
        auto& ramp = sr::gfx::gamma::displayRamp();
        if (decoded && n == kRampBytes) {
          std::memcpy(ramp.table, dec.data(), 256 * 4);
          std::memcpy(ramp.pwl, dec.data() + 256 * 4, 3 * 128 * 4);
          ramp.rwComponent = g.rw_component;
          ramp.version++;
          SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[GPU:REPLAY] gamma ramp: table[0]=%08X [64]=%08X [128]=%08X [255]=%08X",
                      ramp.table[0], ramp.table[64], ramp.table[128], ramp.table[255]);
        } else {
          SR_LOG_BOOT(sr::LogSeverity::Warn, "[GPU:REPLAY] gamma ramp payload undecodable (fmt %u, %u bytes)",
                      g.encoding_format, g.encoded_length);
        }
        break;
      }
      default:
        SR_LOG_BOOT(sr::LogSeverity::Error,
                    "[GPU:REPLAY] unknown command tag %u at offset %ld — stopping",
                    tag, ftell(f));
        ok = false;
        break;
    }
    if (!ok) break;
  }
  }  // loops
  fclose(f);

  // Pull the parse-layer counters out of the shared engine.
  rc.st.packets = rc.eng.packets;
  rc.st.draws = rc.eng.draws;
  rc.st.regWrites = rc.eng.regWrites;
  rc.st.vertexShaderLoads = rc.eng.vertexShaderLoads;
  rc.st.pixelShaderLoads = rc.eng.pixelShaderLoads;
  for (int i = 0; i < 256; i++)
    if (rc.eng.opcodeUnknown[i]) rc.st.unknownOpcodes++;
  pm4cov::report("final");  // NATIVE N0.2: no-op unless SR_PM4_COVERAGE=1
  // No extra frame or present: resolve the final census inventory before the
  // synthetic kernel tests or a successful replay summary can be emitted.
  if (rc.d3d12 && !rc.d3d12->nbReplayFinish()) ok = false;
  if (rc.d3d12) rc.d3d12->i3bCensusLine(" at=replay-end");  // step 6 I3-B: a census after every trace's draws
  // release/emu-stage3: SR_NAT_DIFFTEST=1 - the native resolve kernel's shape cases against the EDRAM word model,
  // after the frames (the backend, its pipelines and the shared memory are up; a no-op without the gate).
  if (rc.d3d12) rc.d3d12->natResolveDifftest();
  if (!nrt.empty())
    SR_LOG_BOOT(sr::LogSeverity::Info, "[GPU:REPLAY] native replay: %zu of %zu records applied, %zu bracket packets not executed",
                nrtUsed, nrt.size() / nrtRec, nrtSkipped);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[GPU:REPLAY] SUMMARY frames=%u packets=%" PRIu64 " draws=%" PRIu64
              " regWrites=%" PRIu64 " memReads=%" PRIu64 " (%" PRIu64
              " MB) vsLoads=%" PRIu64 " psLoads=%" PRIu64
              " unknownOps=%u snappyFail=%u parse=%s",
              rc.st.frames, rc.st.packets, rc.st.draws, rc.st.regWrites,
              rc.st.memReadCmds, rc.st.memReadBytes >> 20,
              rc.st.vertexShaderLoads, rc.st.pixelShaderLoads,
              rc.st.unknownOpcodes,
              rc.st.snappyFailures, ok ? "CLEAN-EOF" : "TRUNCATED");
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[VFETCH] SUMMARY vsParsed=%u vsFailed=%u bindings=%u attrs=%u "
              "mustBeOneViolations=%u",
              rc.eng.vfetchOk, rc.eng.vfetchFail, rc.eng.vfetchBindings,
              rc.eng.vfetchAttrs, ucode::debugMustBeOneViolations());
  for (uint32_t i = 0; i < 64; i++) {
    if (!rc.eng.vfetchFormatHist[i]) continue;
    SR_LOG_BOOT(sr::LogSeverity::Info, "[VFETCH] SUMMARY   fmt=%s(%u) x%u",
                ucode::formatName(ucode::VertexFormat(i)), i,
                rc.eng.vfetchFormatHist[i]);
  }
  for (int i = 0; i < 256; i++) {
    if (rc.eng.opcodeHist[i])
      SR_LOG_BOOT(sr::LogSeverity::Info, "[GPU:REPLAY]   opcode 0x%02X x%u%s", i,
                  rc.eng.opcodeHist[i], rc.eng.opcodeUnknown[i] ? "  (UNHANDLED)" : "");
  }
  if (outStats) *outStats = rc.st;
  // iter-463: park the runtime shader translator while the process is still normal (a job left
  // running into exit() never finished there: the exit waited out the whole drain timeout).
  if (!rtxlat::stop(30000))
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[GPU:REPLAY] runtime shader translator still busy after 30 s at exit");
  return ok ? 0 : 5;
}

}  // namespace gfx
}  // namespace sr
