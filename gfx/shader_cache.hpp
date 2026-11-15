#pragma once

// Host-side cache of XenosRecomp-translated shaders (iter-359c).
//
// Offline pipeline (tools/xenosrecomp/README.md): golden-dump containers →
// XenosRecomp → data/shader_cache/dxil/<xxh3(ucode)>.{vs,ps}.dxil (+ per-VS
// .vfetch sidecar describing each translated vertex-fetch: slot, offset,
// stride, format, usage, signedness). The cache key is XXH3-64 over the RAW
// BIG-ENDIAN ucode bytes — exactly what IM_LOAD delivers at runtime, so the
// live engine hashes the IM_LOAD payload and looks up here. The VS bytes are
// the bind-time vfetch-PATCHED variants (ENGINE_MAP §6d "Shaders").

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include "native_pipeline.hpp"
#endif

namespace sr {
namespace gfx {

struct VfetchElement {
  uint32_t slot = 0;          // vertex fetch constant slot (0..95)
  int32_t offsetDwords = 0;   // offset within the vertex record
  uint32_t strideDwords = 0;  // record stride
  uint32_t format = 0;        // ucode::VertexFormat
  uint32_t usage = 0;         // DeclUsage (0=Position 3=Normal 5=TexCoord 10=Color ...)
  uint32_t usageIndex = 0;
  bool isSigned = false;      // formatCompAll
  bool isInteger = false;     // numFormatAll (1 = integer, 0 = normalized)
  // iter-449u: D3DCOLOR carries a BGRA component swizzle (packed declType bit
  // 11); the decoder swaps R<->B when set. Only ever set from a guest DECL.
  bool bgra = false;
};

// One element of the VS DXIL input signature (container ISG1/ISGN part).
// iter-359h: the D3D12 input layout must cover EVERY signature element or
// CreateGraphicsPipelineState fails E_INVALIDARG — several menu shaders
// declare inputs (e.g. POSITION0) that no vfetch reads, so the sidecar
// alone under-describes the layout.
struct SigElement {
  std::string name;   // semantic name ("POSITION", "TEXCOORD", ...)
  uint32_t index = 0; // semantic index
};

struct CachedShader {
  std::vector<uint8_t> dxil;          // signed DXIL, plain vs_6_0 / ps_6_0
#ifdef _WIN32
  NativeShaderCode nativeCode, nativeFetchCode;  // immutable bytecode owners for native pipelines
#endif
  std::vector<VfetchElement> layout;  // VS only; dedup'd by usage+usageIndex
  std::vector<SigElement> signature;  // VS only; parsed from the DXIL container
  // VS only (P1 PM4 path): the in-shader vertex-fetch variant (<hash>.vs.vsf.dxil).
  // No input attributes: it reads guest memory through t0/b5 space5 (vertex
  // memory + the draw's fetch constants) at the index it computes itself.
  std::vector<uint8_t> dxilFetch;
  // PS only (step 4 lane C, tools/xenosrecomp/0005): the ALPHA-TEST variant (<hash>.ps.at.dxil) - the same
  // translation plus the Xenos alpha test on oC0.w (Xenia CompletePixelShader), driven by the shared constants
  // c17.x (RB_ALPHA_REF) / c17.y (8 | alpha_func). Empty = none (the shader writes no colour 0, or the cache
  // predates 0005). The host draws with it only when RB_COLORCONTROL enables the test (SR_XLAT_ALPHA_TEST).
  std::vector<uint8_t> dxilAlphaTest;
#ifdef _WIN32
  NativeShaderCode nativeAlphaTestCode;
#endif
  std::vector<uint32_t> fetchSlots;   // VS only: every vertex fetch slot the ucode reads
  // PS only (P1): the output signature (OSG1) - which SV_Target slots the
  // translated shader writes and whether it writes SV_Depth. XenosRecomp
  // declares exactly the outputs the ucode header lists, so this is the
  // static export info Xenia's render-target binding uses.
  std::vector<SigElement> outputs;
  // iter-463 VS only: masked key (<hash>.vs.mkey, gfx/shader_mkey.hpp; 0 = no sidecar)
  // and every sidecar row before the semantic dedup (the per-fetch stride/offset/format
  // the translation was made for).
  uint64_t mkey = 0;
  std::vector<VfetchElement> fetchRows;
  uint32_t colorOutMask() const {
    uint32_t m = 0;
    for (const auto& o : outputs)
      if (o.name == "SV_Target" && o.index < 4) m |= 1u << o.index;
    return m;
  }
  bool writesDepth() const {
    for (const auto& o : outputs)
      if (o.name.rfind("SV_Depth", 0) == 0) return true;
    return false;
  }
};

class ShaderCache;

// render/shader-coverage (ported from release/installer cb475b0f): why a draw had no shader - the
// translator's state for it. With the miss policy (WAIT) a draw only meets kDropQueued when the IM_LOAD
// did not wait (SR_SHADER_RUNTIME_XLAT_WAIT=0); every other state is final for this run.
enum DropState : int {
  kDropNoTranslator = 0,  // runtime translation off, not built, or not loaded yet
  kDropQueued = 1,        // queued / being translated (transient; only without WAIT)
  kDropWaiting = 2,       // no template container (retried when the game creates one)
  kDropFailed = 3,        // the translation failed (dxc, exception)
  kDropRefused = 4,       // the translator refused it (ambiguous metadata, a failed proof)
  kDropDisabled = 5,      // the translator is disabled (DXC missing, its cache dir failed to open)
  kDropNoRequest = 6,     // no request was made for it (the request table was full)
  kDropWaitedOut = 7,     // still queued after the IM_LOAD waited its whole budget
  // step 4 lane C round 4 (gfx/alpha_test.hpp use() == kDrop): the PS is translated but the draw's alpha test needs
  // its alpha-test variant, and neither the cache nor the runtime translator (asked at the PS's IM_LOAD) has one.
  kDropAlphaVariantPending = 8,  // the variant's translation is still queued / waiting for a template (transient)
  kDropAlphaVariantFailed = 9,   // the variant could not be made (failed, refused, no translator) - final
};
const char* dropStateName(int st);  // a short name ("no-template", "queued", ...): the release reason

// iter-463 runtime shader translation (gfx/shader_runtime_xlat.cpp, SR_SHADER_RUNTIME_XLAT).
// The translator registers these at static-init time; without it (or with it compiled out)
// every call below is a no-op and the cache behaves exactly as before.
struct ShaderCacheRuntimeHooks {
  // End of ensureLoaded: admit the translations persisted by earlier runs.
  void (*onLoaded)(ShaderCache& cache);
  // IM_LOAD of a shader the cache lacks (ucode = the raw big-endian IM_LOAD bytes; the hook
  // copies them before returning).
  void (*onMiss)(ShaderCache& cache, uint64_t hash, bool isVs, const void* ucode, size_t bytes);
  // A draw had no translation for this shader (admission latency census). Returns its DropState.
  int (*onMissDraw)(uint64_t hash, bool isVs);
  // Does the translator want CreateVertexShader/CreatePixelShader containers? (cheap)
  bool (*wantContainers)();
  // A container the game handed to CreateVertexShader/CreatePixelShader (copied at once).
  void (*onContainer)(const void* blob, size_t bytes, bool isVs);
  // step 4 lane C round 4: IM_LOAD of a cached PS that writes colour 0 but has no alpha-test variant
  // (ShaderCache::needsAlphaTestVariant): make the variant (the miss policy waits for it, as onMiss), admit it with
  // admitRuntimeAlphaTest and persist it. ucode as onMiss.
  void (*onMissAlphaTest)(ShaderCache& cache, uint64_t hash, const void* ucode, size_t bytes);
  // A draw needed that variant and it is not there: the request's DropState (kDropQueued / kDropWaiting /
  // kDropWaitedOut = still possible, else final).
  int (*onMissAlphaTestDraw)(uint64_t hash);
};

class ShaderCache {
 public:
  // ── render/boot-attach (docs/NATIVE_ROADMAP.md "Boot attach"): the boot load in two stages ──
  //   stage 1, installed: SR_SHADER_CACHE_DIR / data/shader_cache/dxil -> the installed maps
  //   stage 2, runtime:   the runtime translator's onLoaded hook (DXC load + hash, persisted translations)
  // WP2: every reader waits for the stage its answer needs. findVs / findPs read the installed maps only
  // after stage 1 and answer a miss only after stage 2, so no answer differs from a fully loaded cache's.
  //
  // D3D12Backend::init calls beginBootLoad() first. SR_SHADER_LOAD_ASYNC (default ON): probe the installed
  // dir (one listing, no file opened), start both stages on the "SR shader load" thread and return: what
  // stays on init's path is that listing and the thread's start (or, if no thread can be created, the whole
  // load - logged). =0: nothing here, createXlatPipeline loads synchronously (the old order, for the A/B).
  void beginBootLoad();
  static bool bootLoadAsync();  // SR_SHADER_LOAD_ASYNC (default ON)
  // Runs both stages on a background thread; a no-op once the load has started anywhere. false = no thread could
  // be created, so the load ran here, synchronously (logged: the attach then waits for it).
  bool startLoadAsync();
  // Block until stage 1 / both stages are published. With nobody loading yet, the caller runs the
  // whole load itself (what call_once did). Safe to call repeatedly from any thread.
  void ensureInstalledLoaded() const { waitStage(kStageInstalled); }
  void ensureLoaded() { waitStage(kStageAll); }
  bool installedLoaded() const { return stage_.load(std::memory_order_acquire) >= kStageInstalled; }
  bool fullyLoaded() const { return stage_.load(std::memory_order_acquire) >= kStageAll; }
  // Is there an installed cache (what decides the xlat path)? Before stage 1 ends: beginBootLoad's probe
  // (never blocks); after it: whether stage 1 admitted any entry - the old createXlatPipeline test
  // (VS + PS count != 0). Without a probe it waits for stage 1.
  bool installedPresent() const;
  // The installed dir as stage 1 resolves it: shippedDataDir("SR_SHADER_CACHE_DIR", "data/shader_cache/dxil").
  static std::filesystem::path resolveInstalledDir();
  // render/shader-coverage (orchestrator 2026-10-17): where a shipped data folder is read from - the env override
  // `envName` if set, else the exe's OWN folder's `rel` when it exists (a package started from any working folder
  // reads its own files), else the working folder's `rel` (the dev fallback: build/ carries no data/shader_cache).
  // cwdFirst, or SR_DATA_CWD_FIRST=1: the old order - the working folder's `rel` when it exists, then the exe's.
  static std::filesystem::path shippedDataDir(const char* envName, const char* rel, bool cwdFirst = false);
  // Does `dir` hold a file stage 1 would admit, judged by name and size only (no file is opened)?
  static bool probeInstalledDir(const std::filesystem::path& dir);
  // A blocking wait (WP1 / WP2) begins / ends on the calling thread. The CP marks its phase with it so
  // the hang watchdog does not read a host wait as a GPU hang (kernel/gpu_command_processor.cpp).
  struct WaitObserver {
    void (*begin)(int stage);
    void (*end)(int stage);
    // render/shader-coverage (Astra round 1, P2): true once the harness's graceful quit asked the CP to drain
    // (kernel/quit.cpp); the miss policy's translation wait then shortens to fit the drain's budget. May be null.
    bool (*draining)() = nullptr;
  };
  static void setWaitObserver(const WaitObserver* obs);
  enum : int { kStageNone = 0, kStageLoading = 1, kStageInstalled = 2, kStageAll = 3 };
  // render/shader-coverage: the miss policy's wait (an IM_LOAD waiting for its runtime translation) is a
  // host wait too; the runtime translator reports it through the same observer with this code.
  enum : int { kWaitTranslation = 4 };
  static void hostWaitBegin(int what);
  static void hostWaitEnd(int what);
  static bool hostWaitDraining();  // the observer's draining() (false without one)
  // Test support (tools/difftest/shader_boot_load_test.cpp): called before every stage-1 file read;
  // returning false fails that read. Files stage 1 has read so far (also the long-wait Warn's progress).
  using ReadHook = bool (*)(const std::filesystem::path& p);
  static void setReadHookForTest(ReadHook hook);
  size_t filesRead() const { return filesRead_.load(std::memory_order_relaxed); }
  // Called on the loader right after a stage is published (T10 delays it: the interleaving where the loader is
  // descheduled just after publication).
  using PublishHook = void (*)(int stage);
  static void setPublishHookForTest(PublishHook hook);
  // Stage 1's end, ms since process start, sampled before its publication (-1 = not yet).
  double installedEndMs() const { return installedEndMs_.load(std::memory_order_relaxed); }
  // The loader's host thread id while or after it ran (0 = no load yet): the host-wait stall report.
  uint32_t loaderThreadId() const { return loaderTid_.load(std::memory_order_relaxed); }

  // Installed (boot-loaded) entries first - lock-free, the maps never change after stage 1 - then
  // the runtime-admitted ones (iter-463) under a shared lock, only once any exist. A returned
  // pointer stays valid for the life of the cache (entries are never erased).
  const CachedShader* findVs(uint64_t ucodeHash) const;
  const CachedShader* findPs(uint64_t ucodeHash) const;
  size_t vsCount() const { waitStage(kStageInstalled); return vs_.size(); }
  size_t psCount() const { waitStage(kStageInstalled); return ps_.size(); }

  // ── iter-463: runtime translation (SR_SHADER_RUNTIME_XLAT) ──
  static void setRuntimeHooks(const ShaderCacheRuntimeHooks* hooks);
  // pm4_engine IM_LOAD miss branch: ask the translator for this shader (no-op without one).
  void requestTranslation(uint64_t hash, bool isVs, const void* ucode, size_t bytes);
  // kernel/shader_container_dump.cpp: CreateShader containers for the translator.
  static bool wantsContainers();
  static void noteContainer(const void* blob, size_t bytes, bool isVs);
  // d3d12_backend gate 2: a draw without its pixel shader. Returns the DropState (the release names it).
  int notePsMissDraw(uint64_t psHash);

  // ── step 4 lane C round 4: the alpha-test variant (gfx/alpha_test.hpp) of a PS whose entry has none ──
  // The order (orchestrator 2026-10-20): (a) the PS's own <hash>.ps.at.dxil; (b) the runtime translator makes it at
  // the PS's IM_LOAD (requestAlphaTestVariant: the miss policy waits for it) and persists it for the next boot; (c)
  // only when (b) failed, a draw the test applies to is dropped (noteAlphaTestDrop: a [SHADER_DROP] census line).
  // Does this PS (the entry findPs returned for `h`) write colour 0 without any variant - its own or a runtime one?
  bool needsAlphaTestVariant(uint64_t h, const CachedShader& ps) const;
  // The variant a draw uses: the PS's own, else a runtime-admitted one; empty / null = none.
  bool hasAlphaTestVariant(uint64_t h, const CachedShader& ps) const;
  const std::vector<uint8_t>* alphaTestDxil(uint64_t h, const CachedShader& ps) const;
#ifdef _WIN32
  NativeShaderCode alphaTestCode(uint64_t h, const CachedShader& ps) const;
#endif
  // record_engine IM_LOAD hit branch: ask the translator for the variant when needsAlphaTestVariant (no-op without a
  // translator, with SR_XLAT_ALPHA_TEST=0, or when the PS has one).
  void requestAlphaTestVariant(uint64_t hash, const void* ucode, size_t bytes);
  // Admit a runtime-made variant for PS `h` (`ps` = its entry: findPs, or findInstalledPs during the boot load).
  // false (nothing changes) when the PS writes no colour 0, already has a variant (its own or an earlier
  // admission), or `dxil` is empty.
  bool admitRuntimeAlphaTest(uint64_t h, const CachedShader& ps, std::vector<uint8_t> dxil);
  size_t runtimeAlphaTestCount() const { return rtAtCount_.load(std::memory_order_acquire); }
  // (c): a draw dropped for want of the variant. Returns the DropState (kDropAlphaVariantPending / Failed).
  int noteAlphaTestDrop(uint64_t psHash);
  uint64_t alphaTestDrops() const { return atDrops_.load(std::memory_order_relaxed); }
  // The installed (offline) cache only - the translator's template/reference preference.
  const CachedShader* findInstalledVs(uint64_t h) const;
  const CachedShader* findInstalledPs(uint64_t h) const;
  const std::filesystem::path& installedDir() const { waitStage(kStageInstalled); return installedDir_; }
  // Build an entry exactly as the boot loader does from a <hash>.{vs,ps}.dxil file set. `variant` is the VS's
  // .vs.vsf.dxil or the PS's .ps.at.dxil (0005); empty = none.
  static void buildEntry(bool isVs, std::vector<uint8_t> dxil, const std::string& sidecarText,
                         std::vector<uint8_t> variant, uint64_t mkey, CachedShader& out);
  // Read <dir>/<hash>.{vs,ps}.dxil (+ .vs.vfetch/.vs.vsf.dxil/.vs.mkey/.ps.at.dxil) through buildEntry.
  static bool loadEntryFromDir(const std::filesystem::path& dir, uint64_t hash, bool isVs,
                               CachedShader& out);
  // Admit a runtime translation. false (nothing changes) when the installed cache or an earlier
  // admission already holds the hash - the installed cache always wins.
  bool admitRuntime(uint64_t hash, bool isVs, CachedShader&& entry);
  size_t runtimeCount() const { return rtCount_.load(std::memory_order_acquire); }

  // XXH3-64 over the raw big-endian ucode byte stream (the IM_LOAD payload).
  static uint64_t hashUcode(const void* data, size_t bytes);

  // ── iter-463: vertex-shader miss report (SR_SHADER_MISS_REPORT, default ON) ──
  // A VS the cache lacks drops every draw that uses it (d3d12_backend gate 1), with
  // no placeholder. Xenia translates on first use, so a miss must never be silent.
  //
  // Why this reports instead of reusing a cached twin's DXIL: the PM4 path draws with
  // <hash>.vs.vsf.dxil, and XenosRecomp's fetch mode BAKES each vfetch's stride,
  // offset, format, signedness and exp_adjust into it as literals (XeVfAddr/XeVfLoad
  // arguments; FAF94BDB multiplies the index by 32, its stride-9 twin DBE06113 by 36).
  // Only the input-layout variant (.vs.dxil) takes them from the layout - the two
  // twins' .vs.dxil are byte-identical - and the PM4 path uses it only for rect lists
  // and clip-disabled draws. Reusing a twin would draw with the wrong stride.
  static bool missReportEnabled();
  static uint64_t maskedKey(const void* ucode, size_t bytes, bool* ok);
  // A cached VS running the same program with another vertex layout (same mkey).
  const CachedShader* findVsTwin(uint64_t mkey, uint64_t* twinHash) const;
  // IM_LOAD side: remember the ucode of a VS the cache misses (bounded, first 256).
  // Called from gfx/pm4_engine.cpp's VS miss branch (wired iter-463 runtime translation).
  void noteVsMissUcode(uint64_t hash, const void* ucode, size_t bytes);
  // Draw side: a draw without its VS translation. Returns the DropState (the release names it).
  int noteVsMissDraw(uint64_t vsHash, uint64_t psHash);
  // End of a drain: log first sightings at once; every 2 s while drops continue, log
  // the draws dropped since the previous summary (they accumulate across drains).
  // Also the [SHADER_DROP] census (VS and PS; a milestone tag, kept in a player log).
  void flushVsMissReport();
  // Every draw without its VS or PS (both note*MissDraw paths): the first one of each shader and state is
  // logged ([SHADER_DROP]), totals in a 2 s summary.
  void noteDrop(uint64_t hash, bool isVs, int state, uint64_t otherHash);
  // The diagnosis text for one missed VS (also used by the unit test).
  std::string diagnoseVsMiss(uint64_t hash) const;

 private:
  // render/boot-attach: the load's stage (kStage*), published with release, read with acquire.
  void waitStage(int stage) const {
    if (stage_.load(std::memory_order_acquire) < stage) waitStageSlow(stage);
  }
  void waitStageSlow(int stage) const;
  bool claimLoad();      // kStageNone -> kStageLoading: true when the caller now owns the load
  void runLoad();        // both stages on the calling thread (the owner)
  void loadInstalled();  // stage 1's body
  void publish(int stage);
  std::atomic<int> stage_{kStageNone};
  mutable std::mutex stageMu_;
  mutable std::condition_variable stageCv_;
  std::atomic<std::thread::id> loaderId_{};  // the owner while it runs (its hooks never wait on it)
  std::atomic<int> probe_{-1};               // beginBootLoad's probe: -1 none, 0 no cache, 1 a cache
  std::atomic<bool> loadedEmpty_{false};     // stage 1 admitted nothing (published with stage 1)
  std::atomic<size_t> filesRead_{0};
  std::atomic<double> installedEndMs_{-1.0};
  bool installedAbsent_ = false;  // stage 1 found no cache folder (the loader's own; the boot-load lines say ABSENT)
  std::atomic<uint32_t> loaderTid_{0};
  std::unordered_map<uint64_t, CachedShader> vs_, ps_;
  std::unordered_multimap<uint64_t, uint64_t> vsByMkey_;  // mkey -> cached VS hash
  std::filesystem::path installedDir_;
  struct MissState;
  MissState* missState();      // created on first use, independent of the load (flushVsMissReport runs every drain)
  std::once_flag missOnce_;
  MissState* miss_ = nullptr;  // lives for the process
  // iter-463 runtime admissions (never erased; see findVs).
  mutable std::shared_mutex rtMu_;
  std::unordered_map<uint64_t, std::unique_ptr<CachedShader>> rtVs_, rtPs_;
  std::atomic<size_t> rtCount_{0};
  // step 4 lane C round 4: runtime-made alpha-test variants of cached PS that have none (under rtMu_; never erased).
  struct RuntimeAlphaTest {
    std::vector<uint8_t> dxil;
#ifdef _WIN32
    NativeShaderCode code;
#endif
  };
  std::unordered_map<uint64_t, std::unique_ptr<RuntimeAlphaTest>> rtAt_;
  std::atomic<size_t> rtAtCount_{0};
  std::atomic<uint64_t> atDrops_{0};
};

ShaderCache& getShaderCache();

// render/boot-attach: milliseconds since this process was created (GetProcessTimes), the common clock of the
// boot lines - the probe, the loader thread's start, the stages' ends, the backend init's end, the attach, the CP
// start - so a roll's log states each bound on one scale.
double msSinceProcessStart();

}  // namespace gfx
}  // namespace sr
