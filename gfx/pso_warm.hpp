#pragma once
// Step 4 item 2k (lane D, 2026-10-20): the first-run pipeline-compile stall - census, manifest, warm-up. BAKED in
// fix10 after the acceptance (pso_acc1 + the replicate pso_rep9: a cold boot's longest gap 1.37-1.39 s without the
// warm-up, 0.27-0.45 s with it; warm boots unchanged): SR_PSO_RECORD and SR_PSO_WARM default ON (=0 turns each off),
// SR_PSO_CENSUS a diagnostic (=1). With SR_PSO_RECORD=0 SR_PSO_WARM=0 nothing here runs, as before the bake.
//
//   SR_PSO_CENSUS=1   [PSO_CREATE] lines: every 10 s while pipelines are being created, the creates by thread kind
//                     (the CP / the PM4 render thread / another / the warm-up) with their ms and max, the cache's
//                     hits and single-flight waits; each of the first 16 creates over 100 ms on a presenting thread
//                     named (a [PSO_CREATE] slow line). The 03:20 plan's item 1: a miss storm made visible.
//   SR_PSO_RECORD     (default ON) the session's translated pipelines (the native xlat root's) that a DRAW used
//                     merged into the USER manifest (gfx/pso_manifest.hpp) every 30 s when new ones exist, and at
//                     exit. The user manifest is per build (orchestrator 2026-10-20: an update never warms stale
//                     pipelines): %LOCALAPPDATA%\SupermanReturnsRecomp\pso_manifest_v1_<build id>.bin, the build id
//                     = the image's PE TimeDateStamp + SizeOfImage; an older build's file there is removed at the
//                     first write. Recording never writes into the install.
//   SR_PSO_WARM       (default ON) at boot (the CP, after its backend wait + WP1), a below-normal "SR pipeline warm" thread
//                     creates the warm list's pipelines through the cache (NativePipelineAcquire::Warm): the user
//                     manifest's (first-use order), then every entry of the SHIPPED manifest the user's lacks - the
//                     package's data\shader_cache\pso_manifest.bin (tools/player/make_player_build.ps1), recorded on
//                     the release build itself, read-only, so a player's FIRST run (the cold driver cache, the stall
//                     this fixes) is covered. It turns single flight on (SR_PSO_SINGLE_FLIGHT), so a warm compile
//                     never holds the cache lock a draw's lookup needs. SR_PSO_WARM_MAX=<n> caps the count.
//   SR_PSO_MANIFEST=<path>  replaces the user manifest's path (tests, the acceptance sets).
//   SR_PSO_SHIPPED_MANIFEST=<path>  replaces the shipped manifest (default: beside the exe, then under the working
//                     folder); =0 turns it off.
//
// Called once from cpEarlyInit (null_renderer.cpp) through D3D12Backend::psoAttach - which does no file I/O: the warm
// thread reads the manifests (the service thread the user's when the warm-up is off), and the recorder writes nothing
// before the user manifest is read; psoShutdown() at the graceful quit (kernel/quit.cpp) and at exit.
#include <cstdint>

struct ID3D12RootSignature;

namespace sr::gfx {

class NativePipelineCache;

bool psoAnyOn();       // any of SR_PSO_CENSUS / _RECORD / _WARM
// Stops the workers: the warm-up after its current pipeline, the service after the last census line and the last
// manifest write (both on the service thread). Waits at most deadlineMs (Astra 10:2x #5; an attach in progress
// included, #4). true = the workers returned and were joined (or never ran): the backend may be torn down. false = a
// worker is still busy (a stuck driver compile / disk): nothing it uses may be torn down - the caller ends the
// process (quick_exit). Idempotent; a later call waits again with its own deadline.
bool psoShutdown(uint32_t deadlineMs = 2000);
// The attach (D3D12Backend::psoAttach: its native pipeline cache and the native xlat root). Once; nothing after a
// shutdown began. Runs whole under the lifecycle mutex psoShutdown takes (Astra 10:2x #4).
void psoAttachTo(NativePipelineCache* cache, ID3D12RootSignature* root);
// Tests (tools/difftest/pso_warm_test.cpp): a hook run inside the attach (under the lifecycle mutex), whether a worker
// still runs, and a reset to the never-attached state. The reset stops and joins any worker still running (a shutdown
// that returned false leaves them running): it may wait, and the cache attached must outlive it.
void psoSetAttachHookForTest(void (*hook)());
void psoSetLoadHookForTest(void (*hook)());   // runs on the worker reading the manifests, before it publishes them
void psoWriteManifestForTest();   // one recorder write now (as the service's periodic write)
bool psoWorkersRunningForTest();
void psoResetForTest();

}  // namespace sr::gfx
