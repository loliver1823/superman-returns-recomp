cmake_minimum_required(VERSION 3.20)

# When using clang-cl, use LLVM's resource compiler (same dir as clang-cl) so the compiler check passes.
if(CMAKE_CXX_COMPILER AND CMAKE_CXX_COMPILER MATCHES "[Cc]lang")
  get_filename_component(_LLVM_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
  set(CMAKE_RC_COMPILER "${_LLVM_BIN}/llvm-rc.exe" CACHE FILEPATH "Resource compiler")
  unset(_LLVM_BIN)
endif()

# iter-467 clean-build: vcpkg's applocal step (msbuild/applocal.ps1) finds a DLL's imports with dumpbin,
# llvm-objdump or objdump from PATH, and without one it prints an error and copies nothing - a fresh build
# from a plain shell then shipped no SDL2.dll. The exes copy their one vcpkg runtime DLL themselves
# (sr_deploy_sdl3 below, from the imported target), so the PATH-dependent step stays off. A new vcpkg
# runtime dependency needs the same explicit copy.
set(VCPKG_APPLOCAL_DEPS OFF CACHE BOOL "vcpkg applocal DLL deployment (needs dumpbin/objdump on PATH; the build deploys SDL3.dll itself)")

project(SupermanReturns LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# step4/repro (release engineering, 2026-10-20): deterministic output. Two clean builds of one commit give
# byte-identical binaries in any build directory:
#  - lld-link /Brepro: the PE header and debug-directory timestamps and the PDB signature come from a hash of the
#    output, not the clock;
#  - /pdbaltpath:%_PDB% (the targets that link /DEBUG, below): the exe names its PDB by file name, not by the build
#    directory's absolute path;
#  - -mno-incremental-linker-compatible: clang writes its COFF objects with a zero timestamp, so objects, static
#    libraries and a link from cached objects are reproducible too;
#  - no build-tree path in the objects or the PDB (the build directory's name would change the PDB, and /Brepro
#    hashes the PDB into the exe's timestamp and PDB signature):
#    - -ffile-prefix-map=<build dir>=. names the generated sources (the recompiled guest code, the embedded
#      shaders) relative to the build tree in debug info and __FILE__;
#    - -fdebug-compilation-dir=. keeps each object's own name (S_OBJNAME, which the PDB's module streams copy)
#      relative to the build tree;
#    - -gno-codeview-command-line drops the compile command line (its -I / -c paths) from the CodeView build record;
#    - lld-link /pdbsourcepath:. makes the PDB's relative paths (objects, its working directory) absolute against
#      "." instead of the build directory;
#    - SupermanReturns's guest library folder goes to the link as /libpath:guest (relative), not an absolute
#      -libpath, because the PDB records the link command line.
#    Proof (VM, 2026-10-20): two clean builds in build-xda and build-xxdb (different names and lengths): all 43
#    binaries byte-identical, the exe and its PDB included.
# Our sources use no __DATE__ / __TIME__ / __TIMESTAMP__, and sr_version.h carries only the commit.
# The PE TimeDateStamp stays a per-build identity: the user PSO manifest's build id is TimeDateStamp + SizeOfImage,
# and /Brepro's stamp is a hash of the output, so builds of different content get different ids (two builds of the
# same content get the same id - the same code and pipelines, so the user's cache stays valid).
# SR_REPRODUCIBLE=OFF restores the old links (the A/B).
option(SR_REPRODUCIBLE "Deterministic builds: lld-link /Brepro, /pdbaltpath:%_PDB%, timestamp-free objects" ON)
if(SR_REPRODUCIBLE AND WIN32)
  add_link_options("LINKER:/Brepro")
  add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-mno-incremental-linker-compatible>)
  add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-ffile-prefix-map=${CMAKE_BINARY_DIR}=.>"
                      "$<$<COMPILE_LANGUAGE:C,CXX>:-fdebug-compilation-dir=.>"
                      "$<$<COMPILE_LANGUAGE:C,CXX>:-gno-codeview-command-line>")
  add_link_options("LINKER:/pdbsourcepath:.")
endif()

# Guest code (XenonRecomp output) requires Clang - use -DCMAKE_CXX_COMPILER=clang-cl
option(BUILD_GUEST "Build XenonRecomp guest output" ON)

# Build using ReXGlue-generated guest code instead of XenonRecomp (for comparison).
# Requires: run ReXGlue codegen first and optionally copy guest/ppc/ppc_context.h → guest/ppc_rexglue/
# See docs/REXGLUE_BUILD.md.
option(BUILD_GUEST_REXGLUE "Use ReXGlue-generated guest code (guest/ppc_rexglue/)" OFF)

# iter-465 perf2: the PPC_STORE debug watchpoints (ppc_context.h) are compiled out of normal
# builds; ON restores them (then SR_STORE_WATCH=1 / the kernel arm sites switch them at run
# time). Applies to every target that includes ppc_context.h (guest lib, kernel overrides,
# difftest), so changing it rebuilds them all.
option(SR_STORE_WATCHPOINTS "Compile the PPC_STORE debug watchpoints into guest stores" OFF)
if(SR_STORE_WATCHPOINTS)
  add_compile_definitions(SR_STORE_WATCHPOINTS=1)
endif()

# step4/audit (docs/LOGIC_AUDIT.md 2, the runtime half): SR_AUDIT_IMPORTS compiles the per-import call census
# (kernel/import_audit.cpp: calls, caller lr, results, first arguments per export, JSON every 10 s) into the import
# glue. An AUDIT build, in its own build tree: never a proof, gate or release build - the release configuration
# (SR_RELEASE_COMPOSITION) refuses it below. Changing it rebuilds every target (a global definition).
option(SR_AUDIT_IMPORTS "Compile the logic audit's per-import call census into the import glue (audit builds only)" OFF)
if(SR_AUDIT_IMPORTS)
  add_compile_definitions(SR_AUDIT_IMPORTS=1)
endif()

# iter-465 perf2: x86 ISA level of the recompiled guest lib. XenonRecomp lowers every VMX
# load/store byte swap, vperm, lvsl/lvsr and vsldoi to simde_mm_shuffle_epi8 /
# simde_mm_alignr_epi8 (42K + 666 sites); on the SSE2 baseline simde emulates each with a
# 16-iteration byte loop through the stack. ssse3 makes them one pshufb / palignr (integer
# shuffles: bit-identical results). Float-sensitive ISA levels (SSE4.1 dpps, AVX2/FMA) stay
# off: x86-64-v3 diverged from the oracle (docs/PERFORMANCE_CODEGEN.md).
set(SR_GUEST_ISA "ssse3" CACHE STRING "x86 ISA level for the guest lib: sse2 | ssse3")
set_property(CACHE SR_GUEST_ISA PROPERTY STRINGS sse2 ssse3)
if(SR_GUEST_ISA STREQUAL "ssse3")
  add_compile_definitions(SR_GUEST_NEEDS_SSSE3=1)
endif()

# iter-466 tick: ISO volatile for the guest lib. Every guest load/store is a `volatile`
# access (ppc_context.h); clang's MSVC-target default (-fms-volatile) makes each one an
# acquire load / release store, i.e. a compiler barrier: every live PPCContext field was
# reloaded (and re-stored) around every guest memory access. ON = plain volatile: guest
# accesses keep their order among themselves (volatile) and on x86 (TSO) the emitted order
# is the hardware order; only thread-private ctx/stack-local values move across them.
option(SR_GUEST_ISO_VOLATILE "Compile the guest lib with -fno-ms-volatile" ON)

# iter-466 tick: profile-guided optimisation of the guest lib. "gen" = instrumented guest lib
# (every exe linking it gets the profile runtime; SR_PGO_DUMP_SECS / LLVM_PROFILE_FILE, see
# kernel/guest_context.cpp); a path to a merged .profdata = optimise with it. "" = off.
set(SR_GUEST_PGO "" CACHE STRING "guest lib PGO: empty | gen | <path to .profdata>")

# iter-467 guestcode EXPERIMENT (default OFF): the guest lib addresses guest memory as base + VA
# with no alias fold (PPC_GUEST_OFFSET / PPC_RAW_ADDR = identity). Only correct when every window
# is a real view of the same memory (SR_PHYS_UNIFY=1); the exe refuses to run otherwise. Sizes what
# "alias windows as views" would save in codegen (PERFORMANCE 2.18).
option(SR_GUEST_ALIAS_IDENTITY "EXPERIMENT: guest lib uses base + VA, requires SR_PHYS_UNIFY=1" OFF)
if(SR_GUEST_ALIAS_IDENTITY)
  add_compile_definitions(SR_ALIAS_IDENTITY_BUILD=1)
endif()

# iter-467 guestcode codegen: ThinLTO for the whole exe (guest lib + app). PERFORMANCE 2.18: -0.20 ms
# tick / +3.7 fps over 6 interleaved pairs (L6, golden_call, replay 9/9 identical). Too small
# for every lane's dev loop (full rebuild, ~105 s relinks, link-time RAM), so OFF for dev builds and
# ON in the "player" preset (build-player/, tools/player/make_player_build.ps1). Backends run in
# the link (lld-link /opt:lldltojobs; each job holds one guest module); /lldltocache keeps unchanged
# modules' objects across relinks.
option(SR_THINLTO "Build with ThinLTO (-flto=thin, lld-link LTO backends)" OFF)
# Dev f04f8805 briefly defaulted it ON; a dev build dir configured then kept ON in its cache. Reset
# it once (the player preset passes ON explicitly; set it again afterwards to keep ON in a dev dir).
if(NOT DEFINED SR_THINLTO_POLICY_V2)
  if(NOT CMAKE_BINARY_DIR MATCHES "build-player$")
    set(SR_THINLTO OFF CACHE BOOL "Build with ThinLTO (-flto=thin, lld-link LTO backends)" FORCE)
  endif()
  set(SR_THINLTO_POLICY_V2 1 CACHE INTERNAL "ThinLTO default reset done (dev OFF, player ON)")
endif()
set(SR_THINLTO_JOBS "2" CACHE STRING "lld-link ThinLTO backend jobs (RAM: ~1-2 GB each)")
if(SR_THINLTO)
  add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-flto=thin>)
  add_link_options(-flto=thin "LINKER:/opt:lldltojobs=${SR_THINLTO_JOBS}"
                   "LINKER:/lldltocache:${CMAKE_BINARY_DIR}/lto_cache")
endif()

# vcpkg toolchain: use third_party/vcpkg if available
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(VCPKG_DIR "${CMAKE_SOURCE_DIR}/third_party/vcpkg")
  if(EXISTS "${VCPKG_DIR}/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_DIR}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
  elseif(DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
  else()
    message(WARNING "No vcpkg found. Run: cmake -B build -DCMAKE_TOOLCHAIN_FILE=third_party/vcpkg/scripts/buildsystems/vcpkg.cmake")
  endif()
endif()

# Help find_package locate vcpkg-installed packages (manifest mode)
if(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows")
  list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows")
endif()

# MSVC runtime
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ── sccache compiler cache ────────────────────────────────────────────────────
# Install: winget install Mozilla.sccache
# Speeds up incremental builds by caching compiled object files.
# Set SCCACHE=OFF to disable: cmake -B build -DSCCACHE=OFF
find_program(SCCACHE_PROGRAM sccache)
option(SCCACHE "Use sccache compiler cache" ON)
if(SCCACHE AND SCCACHE_PROGRAM)
  message(STATUS "sccache found: ${SCCACHE_PROGRAM}")
  set(CMAKE_C_COMPILER_LAUNCHER   "${SCCACHE_PROGRAM}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${SCCACHE_PROGRAM}")
else()
  if(SCCACHE)
    message(STATUS "sccache not found — builds will not be cached (install: winget install Mozilla.sccache)")
  endif()
endif()

# iter-463 (burndown2): a goto that jumps past an initialized declaration is an
# ERROR, never the silent Microsoft extension clang allows for *-windows-msvc.
# clang treats such a jump as UB and at -O3 dropped `beq cr6,0x8271e6ac` in the
# hand-spliced sub_8271E630, so the FM init-graph walk ran the body of an EMPTY
# dependency array and called a NULL vtable slot on every boot (masked by the
# legacy r3=0 indirect-call return). Hand edits in generated code make this
# easy to reintroduce; the compiler now refuses it.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Werror=microsoft-goto>)
endif()

# ── ASAN (AddressSanitizer) ───────────────────────────────────────────────────
# Catches: heap buffer overflows, use-after-free, stack overflows in the host harness.
# step4/relcrash: a DIAGNOSTIC build only. It used to be add_compile_options (every later target, i.e. also the
# 144 MB guest library, against the note's own intent); it is now target-scoped to SupermanReturns' own sources (set
# after add_executable below): the guest library, difftest and the tools stay uninstrumented, the ASan runtime DLL
# is deployed next to the exe and the exe logs an [ASAN] banner. tools/player/make_player_build.ps1 REFUSES a build
# tree configured with ASAN=ON, so it never becomes a player or release package. Any configuration, e.g. the
# release composition: cmake ... -DSR_RELEASE_COMPOSITION=ON -DASAN=ON.
option(ASAN "AddressSanitizer on the SupermanReturns harness sources (a diagnostic build; never packaged)" OFF)
# step 6 N4 (NEWPAIR_CHECKS section 4): AddressSanitizer on the native unit tests - difftest (its own compile of the
# harness sources, and its suites) and every sr_*_test executable - with ASAN=ON's flags (applied at the end of this
# file). A diagnostic TEST build: SupermanReturns is not instrumented, and the ASan runtime DLL beside the tests makes
# tools/player/make_player_build.ps1 refuse the tree.
option(SR_ASAN_TESTS "AddressSanitizer on difftest and the sr_*_test unit tests (step 6 N4; a diagnostic test build)" OFF)

set(APP_SOURCES
  app/main.cpp
  app/log.cpp
  app/stdio_guard.cpp
  app/platform_sdl.cpp
  app/window_icon.cpp
  gfx/null_renderer.cpp
  gfx/gpu_trace_replay.cpp
  gfx/pm4_engine.cpp
  gfx/native_record_executor.cpp
  gfx/native_draw_builder.cpp
  gfx/native_image_store.cpp
  gfx/native_failure_diagnostics.cpp
  gfx/native_pipeline.cpp
  gfx/native_draw.cpp
  gfx/native_upload.cpp
  gfx/translated_constants.cpp
  gfx/translated_samplers.cpp
  gfx/translated_textures.cpp
  gfx/record_engine.cpp             # C1 stage 2a: the record engine (the shipping half of pm4_engine.cpp)
  gfx/record_context.cpp            # the NativeRenderContext over it (was pm4_native_record_adapter.cpp)
  gfx/draw_ledger.cpp
  gfx/pm4_coverage.cpp
  gfx/native_device.cpp
  gfx/native_vs_patch.cpp
  gfx/shader_cache.cpp
  gfx/hash_avx2.cpp
  gfx/cp_simd.cpp
  gfx/shader_runtime_xlat.cpp
  gfx/xenos_ucode.cpp
  gfx/d3d12_backend.cpp
  gfx/d3d12_frame_render.cpp  # C1 stage 3: the PM4 frame renderer (was d3d12_pm4_edram.cpp)
  gfx/d3d12_edram_model.cpp   # C1 stage 3: the EDRAM word model, the development build's oracle unit (EMULATION_SOURCES)
  gfx/d3d12_pm4_shmem.cpp
  gfx/native_zpd.cpp          # p2.zpd: EVENT_WRITE_ZPD as host occlusion queries (SR_NATIVE_ZPD)

  gfx/resource_registry.cpp   # mem.resources step 2: the resource registry (RELEASE_COMPOSITION.md 9.2)
  gfx/resource_memory.cpp     # mem.resources step 2b: native boundary and upload protocol
  gfx/resource_memory_check.cpp # independent native kept-page observer, oracle only
  gfx/resource_reader_probe.cpp
  kernel/resource_reader_observe.cpp
  gfx/resource_census.cpp     # mem.resources step 2a: the registry in shadow beside the page model (oracle)
  gfx/resource_ab.cpp         # mem.resources: the frozen-pass per-present A/B of the native route (oracle)
  gfx/resource_texid.cpp      # mem.resources: the texture-identity census of resolve output (oracle)
  gfx/d3d12_present_chain.cpp # iter-466 gfx: the present chain (post, colour, scale, sharpen, fit)
  gfx/gfx_options.cpp         # iter-466 gfx: the host graphics options (display.ini)
  gfx/d3d12_overlay.cpp
  gfx/present_luma.cpp        # step 4 X2: SR_PRESENT_LUMA, non-blocking presented-luminance probe (default OFF)
  gfx/present_dump_writer.cpp # step 4 4(c): SR_PRESENT_DUMP_QUEUE, the present dump's BMP writer queue
  gfx/present_dump_queue.cpp  # step 4 4(c): the process writer (app/log lines, exit drain)
  gfx/host_hlsl.cpp           # step 4 7a: SR_HOST_HLSL_AOT, the host HLSL's ahead-of-time blobs
  gfx/pso_manifest.cpp        # step 4 2k: the pipeline manifest's file format (standalone)
  gfx/pso_warm.cpp            # step 4 2k: SR_PSO_CENSUS / _RECORD / _WARM (census, recorder, boot warm-up)
  gfx/overlay_kit.cpp         # iter-466 gfx: the host overlays' shared drawing kit (from mod_library_ui)
  gfx/mod_library_ui.cpp      # iter-465 modlib: the mod library overlay (Dear ImGui drawing)
  gfx/settings_ui.cpp         # iter-466 gfx: the PC SETTINGS screen (host overlay)
  gfx/achievements_ui.cpp     # lane E L-a: the ACHIEVEMENTS page (host overlay, through the settings slot)
  gfx/costume_viewer_draw.cpp # iter-465 viewer: the COSTUME VIEWER model pass
  gfx/res_scale.cpp
  gfx/pm4_live_trace.cpp
  gfx/hle_video.cpp
  gfx/glyph_subst.cpp         # iter-464 input lane: device-matched button prompts
  gfx/cube_bind_rule.cpp
  kernel/export_registry.cpp
  kernel/guest_context.cpp
  kernel/native_hardware_memory.cpp
  kernel/guest_profile.cpp
  kernel/native_stack_probe.cpp
  kernel/perf_stats.cpp
  kernel/host_profile.cpp
  kernel/hitch_log.cpp
  kernel/phys_window.cpp
  kernel/gpu_write_watch.cpp
  kernel/tex_census.cpp
  kernel/phys_alloc.cpp
  kernel/file_io.cpp
  kernel/xex_loader.cpp
  kernel/stubs.cpp
  kernel/guest_alloc_fail.cpp   # step4 stuck-load: [GUEST_ALLOC_FAIL]
  kernel/load_marker.cpp        # step4 ruling 12: [LOAD_BEGIN] / [LOAD_DONE] / [LOAD_OFF]
  kernel/kthread_objects.cpp
  kernel/kpcr.cpp
  kernel/xam_content.cpp
  kernel/xam_profile.cpp
  kernel/import_fixes.cpp      # step4/audit: Xenia-equal import answers (logic audit 9a)
  kernel/achievements.cpp      # step5 E14a: the achievements core (no UI; SR_ACHIEVEMENTS)
  kernel/achievements_page.cpp   # lane E L-a: the ACHIEVEMENTS page model (pure)
  kernel/mod_overlay.cpp
  kernel/mod_manager.cpp
  kernel/asset_receipt.cpp    # lane E step 5 E3-E6 r3: the [ASSET] MAIN receipt
  kernel/mod_costume_data.cpp
  kernel/mod_density.cpp      # iter-466 density: population / traffic density (port feature)
  kernel/costume_viewer_model.cpp
  kernel/costume_viewer.cpp
  kernel/mod_costume_preview.cpp
  kernel/costume_watch.cpp
  kernel/anim_heap.cpp
  kernel/cheats_menu.cpp
  kernel/cloth_capacity.cpp
  kernel/mod_formats.cpp
  kernel/mod_zip.cpp
  kernel/mod_bridge.cpp
  kernel/mouse_ui.cpp
  kernel/mod_library.cpp      # iter-465 modlib: the mod library's model + input routing
  kernel/map_markers.cpp      # iter-466 map: the useful pause map (docs/MAP.md)
  kernel/harness.cpp
  kernel/splash_hooks.cpp
  kernel/midasm_hooks.cpp
  kernel/r1_hooks_core.cpp
  kernel/r1_platform.cpp
  kernel/r1_hooks_b1.cpp
  kernel/r1_hooks_b2.cpp
  kernel/r1_hooks_b3.cpp
  kernel/heap_chunk_guard.cpp
  kernel/apt_as_watch.cpp
  kernel/rtam_gameconfig_watch.cpp
  kernel/fe_flow_watch.cpp
  kernel/apt_set_watch.cpp
  kernel/apt_render_watch.cpp
  kernel/shader_container_dump.cpp
  kernel/gpu_ring_doorbell.cpp
  kernel/gpu_command_processor.cpp
  kernel/gpu_submit.cpp
  kernel/native_brackets.cpp
  kernel/native_brackets.gen.cpp
  kernel/native_leaves.cpp
  kernel/hdr_adaptation.cpp
  kernel/native_const_dedup.cpp
  kernel/native_meter.cpp
  kernel/native_drain_census.cpp
  kernel/native_exec.cpp
  kernel/native_builder_census.cpp
  kernel/native_flush.cpp
  kernel/native_resource_writes.cpp
  kernel/native_resource_stamps.cpp
  kernel/native_transport.cpp
  kernel/native_pump.cpp
  kernel/native_display.cpp
  kernel/native_vblank.cpp
  kernel/graphics_interrupt_registration.cpp
  kernel/native_recording.cpp
  kernel/native_rec_b.cpp
  kernel/p2_trace.cpp
  kernel/gpu_ringless.cpp
  kernel/native_devcreate.cpp
  kernel/native_display_emit.cpp  # boot gaps: the display emitters (gamma ramp, scaler) as records
  kernel/native_cloth.cpp
  kernel/native_clothsim.cpp
  kernel/native_pose.cpp
  kernel/native_params.cpp
  kernel/native_vtx.cpp
  kernel/ab_toggle.cpp
  kernel/native_draw_ledger.cpp
  kernel/gpu_hang_wd.cpp
  kernel/mem_sites.cpp        # step 4 7d: SR_MEM_SITES, [MEM_SITE] names for the big host allocations (diagnostic)
  kernel/rtam_service.cpp
  kernel/dogma_hash_hooks.cpp
  kernel/init_wait_bypass.cpp
  kernel/cs_timeout_underflow_fix.cpp
  kernel/hle_api_observe.cpp
  kernel/tex_identity_domain.cpp
  kernel/tex_unlock_test.cpp
  kernel/tex_identity_test.cpp
  kernel/recomp_switch_fixes.cpp
  kernel/indirect_call.cpp
  kernel/apt_parse_trace.cpp
  kernel/sr_timebase.cpp
  kernel/highfps.cpp
  kernel/timebase_obs.cpp
  kernel/apt_h7_entrywatch.cpp
  kernel/forcing_isolation_test.cpp
  kernel/wffsm_obs.cpp
  gfx/hle_prov_ring.cpp
  kernel/crt_file_lock_init_fix.cpp
  kernel/alloc_overlap_watch.cpp
  kernel/observe_vpk_f16.cpp
  kernel/simd_memcpy_null_guard.cpp
  kernel/audio_render_driver.cpp
  kernel/audio_cmdq_obs.cpp
  kernel/ntavm_xenia_heap.cpp
  kernel/thread_mem_xenia.cpp
  kernel/ffmpeg_xenia.cpp
  kernel/mm_phys.cpp
  kernel/apu/xma_context_new.cpp
  kernel/apu/xma_ffmpeg_decoder.cpp
  kernel/apu/xma_device.cpp
  kernel/apu/xma_legacy_exports.cpp
  kernel/apu/audio_system.cpp
  kernel/xma_hal_observe.cpp
  kernel/audio_native_census.cpp    # iter-467 N6: [AUDIO_NATIVE] + native reg 0x600
  kernel/image_write_watchdog.cpp
  kernel/pool_witness.cpp
  kernel/das_tree_witness.cpp     # fix/postgame-apt: SR_DAS_WITNESS (DAS attribute-tree witness)
  kernel/hw_watchpoint.cpp
  kernel/fm_world_ptr_patcher.cpp
  kernel/gpu_ring_alloc_fix.cpp
  kernel/natural_gauges.cpp
  kernel/savestate.cpp
  kernel/input_recorder.cpp
  kernel/host_automation.cpp   # iter-464 bgruns: automated runs stay in the background
  kernel/quit.cpp              # tools/graceful-quit: harness-requested quit + terminal snapshot (TOOLKIT 1.8m)
  # iter-464 controls lane: the PC control scheme (docs/PC_CONTROLS.md)
  kernel/pc_controls.cpp
  kernel/pc_actions.cpp
  kernel/apt_call.cpp
  kernel/pc_controls_bindings.cpp
  kernel/pc_controls_map.cpp
  kernel/pc_controls_game.cpp
  kernel/pc_moves.cpp
  kernel/pc_moves_math.cpp
  kernel/host_input.cpp       # iter-464 input lane: SDL controllers, hot-plug, bindings, rumble
  kernel/vpad_filter.cpp      # iter-466 tooling: SR_HARNESS_VPAD, automated rolls open only ViGEm pads
  kernel/collision_watch.cpp
  # iter-463 phase 2: dev tools (gate + tags, level-skip executor core, tick hooks)
  kernel/devtools.cpp
  kernel/devtools_skip_core.cpp
  kernel/devtools_levelskip.cpp
  kernel/devtools_menu.cpp
  kernel/display_menu.cpp     # iter-465 polish: DISPLAY OPTIONS (resolution, mode, fps, vsync)
  kernel/pc_settings.cpp      # iter-466 gfx: the PC SETTINGS model (options, stores, apply)
  kernel/game_options.cpp     # lane E L-b: the OPTIONS GAME tab (the game's own options and setters)
  kernel/host_pages.cpp
  kernel/mod_menu.cpp
  kernel/mod_menu_modcore.cpp   # iter-464 modmenu: adapter over modcore's mod_manager.hpp (stub until it lands)
  # iter-33: kernel/dogma_pool_serializer.cpp tried per-pool host
  # std::recursive_mutex serialization on sub_82182700 entry. Result: reliably
  # unblocked FMGameRtam/FMGameRtamIni contention but regressed FMHud/FMGameScript
  # (which need concurrent worker access). 0/3 WALK_END. Disabled, kept source
  # for reference. See PROGRESS.md row 33.
  # kernel/dogma_pool_serializer.cpp
)
if(WIN32)
  list(APPEND APP_SOURCES app/crash_handler_win.cpp)
  list(APPEND APP_SOURCES app/heap_sentinel.cpp)  # step4/relcrash: SR_HEAP_SENTINEL (default OFF)
  set(CRASH_LIBS dbghelp)
else()
  list(APPEND APP_SOURCES app/crash_handler.cpp)
  set(CRASH_LIBS "")
endif()

# Harness sources WITHOUT app/main.cpp — reused by the difftest tool so it can link
# the SAME kernel/gfx implementations the game uses (resolving the import-glue and
# watchpoint symbols the recomp objects reference) without a second main().
if(BUILD_GUEST AND NOT BUILD_GUEST_REXGLUE)
  find_program(SR_CLOTH_PYTHON NAMES python REQUIRED)
  set(SR_CLOTH_SIZED "${CMAKE_CURRENT_BINARY_DIR}/generated/cloth_tangents_sized.inc")
  add_custom_command(OUTPUT "${SR_CLOTH_SIZED}"
    COMMAND "${SR_CLOTH_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/regen/cloth_capacity.py"
      "${CMAKE_SOURCE_DIR}/guest/ppc/ppc_recomp.35.cpp" "${SR_CLOTH_SIZED}"
    DEPENDS tools/regen/cloth_capacity.py guest/ppc/ppc_recomp.35.cpp
    VERBATIM)
  list(APPEND APP_SOURCES "${SR_CLOTH_SIZED}")
  set_source_files_properties(kernel/cloth_capacity.cpp PROPERTIES
    OBJECT_DEPENDS "${SR_CLOTH_SIZED}"
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/generated")
else()
  list(REMOVE_ITEM APP_SOURCES kernel/cloth_capacity.cpp)
endif()
if(SR_AUDIT_IMPORTS)
  list(APPEND APP_SOURCES kernel/import_audit.cpp)   # step4/audit: the import census (audit builds only)
endif()
include(cmake/host_hlsl.cmake)   # step 4 7a: <build>/generated/host_hlsl_blobs*.cpp
set(HARNESS_SOURCES_NO_MAIN ${APP_SOURCES})
list(REMOVE_ITEM HARNESS_SOURCES_NO_MAIN app/main.cpp)

# iter-465 modlib: Dear ImGui (MIT, vendored unmodified in third_party/imgui, v1.91.9b) draws the
# mod library overlay (gfx/mod_library_ui.cpp). Core + the DX12 renderer backend only.
add_library(sr_imgui STATIC
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_dx12.cpp)
target_include_directories(sr_imgui PUBLIC ${CMAKE_SOURCE_DIR}/third_party/imgui)
target_compile_definitions(sr_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_DEMO_WINDOWS
                                           IMGUI_DISABLE_DEBUG_TOOLS)
if(WIN32)
  target_compile_definitions(sr_imgui PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(sr_imgui PUBLIC d3d12 dxgi d3dcompiler)
endif()

# C1 (RELEASE_PLAN section 1, ledger release.composition): the release configuration compiles the development
# oracles out of SupermanReturns (kernel/sr_oracle.hpp: their headers become inline no-ops, their sources leave the
# target). The dev build and difftest keep them (SR_ORACLE=1). tools/player/composition_audit.py audits the result.
option(SR_RELEASE_COMPOSITION "Compile the development oracles out of SupermanReturns (the C1 release configuration)" OFF)
if(SR_RELEASE_COMPOSITION AND SR_AUDIT_IMPORTS)
  message(FATAL_ERROR "SR_AUDIT_IMPORTS is an audit build (the logic audit's import census): the release configuration (SR_RELEASE_COMPOSITION=ON) refuses it. Configure the audit in its own build tree without SR_RELEASE_COMPOSITION.")
endif()
set(ORACLE_SOURCES
  gfx/resource_reader_probe.cpp
  gfx/resource_memory_check.cpp
  gfx/gpu_trace_replay.cpp
  gfx/draw_ledger.cpp
  gfx/pm4_coverage.cpp
  gfx/pm4_live_trace.cpp
  gfx/hle_prov_ring.cpp
  kernel/tex_census.cpp
  kernel/native_brackets.cpp
  kernel/native_brackets.gen.cpp
  gfx/resource_census.cpp     # mem.resources step 2a: the registry's shadow census (SR_MEM_RESOURCES=census)
  gfx/resource_ab.cpp         # mem.resources: SR_AB_TOGGLE=SR_MEM_RESOURCES (dual bookkeeping, alternating decider)
  gfx/resource_texid.cpp      # mem.resources: SR_MEM_RES_TEXID=1 (textures_from_resolves)
  kernel/native_drain_census.cpp
  kernel/native_builder_census.cpp
  kernel/p2_trace.cpp
  kernel/ab_toggle.cpp
  kernel/native_draw_ledger.cpp
  kernel/tex_unlock_test.cpp
  kernel/tex_identity_test.cpp
  kernel/apt_h7_entrywatch.cpp
  kernel/forcing_isolation_test.cpp
  kernel/observe_vpk_f16.cpp
  kernel/xma_hal_observe.cpp
  # the D3D12 backend split (step 2): the HLE renderer A/B and the observers wired into the backend. Under
  # SR_ORACLE=0 the backend's HLE draw / present path and main.cpp's SR_HLE_TRACE_REPLAY mode are compiled out with
  # them, and their headers are inline no-ops.
  gfx/hle_video.cpp
  gfx/cube_bind_rule.cpp
  kernel/tex_identity_domain.cpp
  kernel/wffsm_obs.cpp
  gfx/native_device.cpp
  ${SR_HOST_HLSL_OUT_ORACLE}  # step 4 7a: the oracle-only host shaders' blobs
)
# C1 (docs/RELEASE_COMPOSITION.md section 3.1): emulation units the default path no longer reaches. They leave the
# release configuration like the oracles (their headers answer as the shipping code sees them with the gate unset);
# the dev build and difftest keep them. Stage 0: the SR_NATIVE_EXEC executor (default OFF) and the SR_XMA_NEW=0 legacy XMA
# fakes (Astra round 1: a unit of their own, so a release build that links them fails the audit). Stage 3: the EDRAM
# word model (the tile ownership map, the 10 MiB EDRAM buffer and its store / load passes, the CPU mirror, the
# EDRAM-path resolve, the trace snapshot) - the frame renderer's oracle half (docs/RELEASE_COMPOSITION.md section 8).
set(EMULATION_SOURCES
  kernel/gpu_command_processor.cpp
  kernel/gpu_ring_doorbell.cpp
  kernel/gpu_write_watch.cpp
  kernel/native_exec.cpp
  kernel/apu/xma_legacy_exports.cpp
  gfx/d3d12_edram_model.cpp
)
# C1 stage 1 (docs/RELEASE_COMPOSITION.md 9.9): the PM4 packet walker (Pm4Engine: execute, cpPacket, the window scan,
# the parse-health counters, the record -> PM4 oracle) - the release configuration's CP applies records through the
# record engine only, and a literal-PM4 gap, a kind-1 IB item or a ring packet is fatal there.
list(APPEND EMULATION_SOURCES gfx/pm4_engine.cpp)
set(SR_GAME_SOURCES ${APP_SOURCES})
if(SR_RELEASE_COMPOSITION)
  list(REMOVE_ITEM SR_GAME_SOURCES ${ORACLE_SOURCES} ${EMULATION_SOURCES})
  message(STATUS "SR_RELEASE_COMPOSITION: the development oracles and the unreached emulation units are compiled out of SupermanReturns (SR_ORACLE=0)")
endif()
add_executable(SupermanReturns ${SR_GAME_SOURCES})
target_link_libraries(SupermanReturns PRIVATE sr_imgui)
if(ASAN)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ASAN=ON needs the pinned clang toolchain")
  endif()
  # The dynamic CRT (vcpkg x64-windows) pairs with the dynamic ASan runtime + its /MD thunk: say so to the driver.
  # -asan-stack=0: no stack redzones (smaller frames on the game's fixed-size host stacks). Heap overflow and
  # use-after-free detection - what this build is for - are unaffected. (2026-10-20 correction: relcrash_asan6a's
  # __chkstk death 3 s into boot was NOT ASan's frames - the -asan-stack=0 exe died at the same point, in
  # imageWriteVeh. Both were a VEH registered ahead of ASan's shadow-commit handler recursing on uncommitted shadow;
  # fixed by sr::kVehFirst, app/crash_handler.hpp.)
  target_compile_options(SupermanReturns PRIVATE -fsanitize=address -fno-omit-frame-pointer -fms-runtime-lib=dll
                         "SHELL:-mllvm -asan-stack=0")
  target_link_options(SupermanReturns PRIVATE -fsanitize=address -fms-runtime-lib=dll)
  # The guest library and the import glue stay uninstrumented, and the MSVC STL's ASan container annotations must
  # match across every object (lld /failifmismatch 'annotate_string' / 'annotate_vector'): keep them off.
  # Heap / stack / use-after-free detection is unaffected; only container-overflow checks are lost.
  target_compile_definitions(SupermanReturns PRIVATE SR_ASAN_BUILD=1 _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
  execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir OUTPUT_VARIABLE _sr_clang_rd
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(_sr_asan_dll "${_sr_clang_rd}/lib/windows/clang_rt.asan_dynamic-x86_64.dll")
  if(NOT EXISTS "${_sr_asan_dll}")
    message(FATAL_ERROR "ASAN=ON: no ASan runtime at ${_sr_asan_dll}")
  endif()
  add_custom_command(TARGET SupermanReturns POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_sr_asan_dll}" "$<TARGET_FILE_DIR:SupermanReturns>")
  message(WARNING "ASAN=ON: SupermanReturns is a DIAGNOSTIC build (AddressSanitizer on the harness sources); "
                  "make_player_build.ps1 refuses to package it")
endif()
if(SR_RELEASE_COMPOSITION)
  target_compile_definitions(SupermanReturns PRIVATE SR_ORACLE=0)
endif()

# Xenia's D3D12 shaders (resolves, texture loads, FXAA, guest output), compiled with fxc at build time from
# third_party/xenia-shaders. See cmake/xenia_shaders.cmake.
include(cmake/xenia_shaders.cmake)
sr_use_xenia_shaders(SupermanReturns)

# iter-465 window: the window title carries the build's short commit (sr_version.h, regenerated
# every build, rewritten only when the hash changes), and the exe carries its own neutral app
# icon (app/app_icon.rc: an original design, tools/player/make_app_icon.py; no game art).
set(SR_VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/sr_version.h")
execute_process(COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${SR_VERSION_HEADER}
                -P ${CMAKE_SOURCE_DIR}/cmake/gen_version.cmake)
add_custom_target(sr_version_header
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${SR_VERSION_HEADER}
          -P ${CMAKE_SOURCE_DIR}/cmake/gen_version.cmake
  BYPRODUCTS ${SR_VERSION_HEADER}
  DEPENDS ${CMAKE_SOURCE_DIR}/cmake/gen_version.cmake
  VERBATIM)
add_dependencies(SupermanReturns sr_version_header)
set_source_files_properties(app/main.cpp PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(WIN32)
  target_sources(SupermanReturns PRIVATE app/app_icon.rc)
  set_property(SOURCE app/app_icon.rc APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/app/app_icon.ico")
endif()

target_include_directories(SupermanReturns PRIVATE
  ${CMAKE_SOURCE_DIR}/app
  ${CMAKE_SOURCE_DIR}
)

if(WIN32)
  target_compile_definitions(SupermanReturns PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# RELEASE_PLAN step 3: SDL3 (vcpkg manifest vcpkg.json; vcpkg-configuration.json selects the overlay port
# third_party/vcpkg-overlay/sdl3 = SDL 3.4.18). SDL3 has no SDLmain library: app/main.cpp includes
# <SDL3/SDL_main.h>, whose header-only WinMain calls SDL_main.
find_package(SDL3 3.4.18 CONFIG REQUIRED)
# iter-467 clean-build: SDL3.dll goes next to every exe that links it, copied from the imported target
# (its IMPORTED_LOCATION: <build>/vcpkg_installed in manifest mode, the prefix's bin/ otherwise). Without
# it the exe exits 0xC0000135 at start. See VCPKG_APPLOCAL_DEPS at the top for why vcpkg's step is off.
set(_sr_sdl3 SDL3::SDL3)
get_target_property(_sr_sdl3_alias SDL3::SDL3 ALIASED_TARGET)
if(_sr_sdl3_alias)
  set(_sr_sdl3 ${_sr_sdl3_alias})
endif()
get_target_property(_sr_sdl3_type ${_sr_sdl3} TYPE)
if(WIN32 AND NOT _sr_sdl3_type STREQUAL "SHARED_LIBRARY")
  message(FATAL_ERROR "SDL3::SDL3 is a ${_sr_sdl3_type}; the exe needs the shared SDL3 (SDL3.dll) next to it")
endif()
function(sr_deploy_sdl3 tgt)
  if(WIN32)
    add_custom_command(TARGET ${tgt} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${_sr_sdl3}>" "$<TARGET_FILE_DIR:${tgt}>")
  endif()
endfunction()
sr_deploy_sdl3(SupermanReturns)
target_link_libraries(SupermanReturns PRIVATE
  SDL3::SDL3
  ${CRASH_LIBS}
)
if(WIN32)
  # WIC decodes shipped glyph PNGs; shell32 supplies the mod library links.
  target_link_libraries(SupermanReturns PRIVATE d3d12 dxgi xinput winmm shell32 windowscodecs ole32)
  # Unicode filesystem APIs support long install paths when Windows policy AND
  # the executable opt-in are enabled. Keep the game and difftest identical.
  set(SR_WINDOWS_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/app/windows.manifest")
  target_link_options(SupermanReturns PRIVATE
    "LINKER:/MANIFEST:EMBED" "LINKER:/MANIFESTINPUT:${SR_WINDOWS_MANIFEST}")
  set_property(TARGET SupermanReturns APPEND PROPERTY LINK_DEPENDS "${SR_WINDOWS_MANIFEST}")
  # Main thread host stack: 64 MB — matches FM worker threads (32 MB) with headroom.
  # The guest call chain fills the default 1 MB main thread stack, causing
  # STATUS_STACK_OVERFLOW on every kernel call after the stack is exhausted.
  # Use LINKER: prefix so CMake passes it via -Xlinker to lld-link.
  target_link_options(SupermanReturns PRIVATE "LINKER:/STACK:67108864")
  # iter-461: CodeView debug info for the HARNESS sources only (the guest code
  # would make a multi-GB PDB) so a stall/crash minidump names our frames.
  # Release codegen is unchanged; the PDB lands next to the exe.
  set_source_files_properties(${APP_SOURCES} PROPERTIES COMPILE_OPTIONS "-gcodeview")
  # iter-465 native CP costs: the AVX2 build of XXH3 (run-time selected in ShaderCache::hashUcode).
  set_source_files_properties(gfx/hash_avx2.cpp PROPERTIES COMPILE_OPTIONS "-gcodeview;-mavx2"
                              SKIP_PRECOMPILE_HEADERS ON)
  # iter-466 cp lane: the command processor's SSE4.1 kernels (run-time selected, cp_simd.hpp).
  set_source_files_properties(gfx/cp_simd.cpp PROPERTIES COMPILE_OPTIONS "-gcodeview;-msse4.1"
                              SKIP_PRECOMPILE_HEADERS ON)
  # iter-467 guestcode: the native cloth light mask / cloth sim / pose leaf transcribe guest VMX code and must lower
  # every simde op exactly as the guest lib does (same ISA level: SSSE3, no SSE4.1/FMA).
  set_source_files_properties(kernel/native_cloth.cpp kernel/native_clothsim.cpp kernel/native_pose.cpp kernel/native_params.cpp kernel/native_vtx.cpp PROPERTIES COMPILE_OPTIONS "-gcodeview;-mssse3"
                              SKIP_PRECOMPILE_HEADERS ON)
  target_link_options(SupermanReturns PRIVATE "LINKER:/DEBUG")
  if(SR_REPRODUCIBLE)
    target_link_options(SupermanReturns PRIVATE "LINKER:/pdbaltpath:%_PDB%")   # step4/repro: no build path in the exe
  endif()

  # (SDL3.dll: sr_deploy_sdl3 above.)
  # iter-464 input lane: the button-prompt glyphs ship next to the exe (data/glyphs: the
  # manifest + the CC0 Julio Cacko subset; gfx/glyph_subst.cpp reads <exe dir>/data/glyphs).
  add_custom_command(TARGET SupermanReturns POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_SOURCE_DIR}/data/glyphs" "$<TARGET_FILE_DIR:SupermanReturns>/data/glyphs")
  # iter-465 modlib: the mod library's fonts (Saira, OFL; gfx/mod_library_ui.cpp reads
  # <exe dir>/data/fonts).
  add_custom_command(TARGET SupermanReturns POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_SOURCE_DIR}/data/fonts" "$<TARGET_FILE_DIR:SupermanReturns>/data/fonts")
endif()

# iter-463 phase 2: the level-skip core as an offline task host (tools/devtools/skipcore_host.cpp),
# the C++ third of the executor differential (tools/devtools/test_skipcore_cpp.py compiles the same two
# files itself; this target is for running it by hand). Pure C++, no harness.
add_executable(sr_skipcore_host tools/devtools/skipcore_host.cpp kernel/devtools_skip_core.cpp)
target_include_directories(sr_skipcore_host PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_features(sr_skipcore_host PRIVATE cxx_std_20)
add_executable(sr_native_record_executor_test
  tools/difftest/native_record_executor_test.cpp gfx/native_record_executor.cpp)
target_include_directories(sr_native_record_executor_test PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_features(sr_native_record_executor_test PRIVATE cxx_std_20)
add_executable(sr_native_draw_builder_test
  tools/difftest/native_draw_builder_test.cpp gfx/native_draw_builder.cpp)
target_include_directories(sr_native_draw_builder_test PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_features(sr_native_draw_builder_test PRIVATE cxx_std_20)
# Exercise both command layouts against the same preserved draw-state oracle.
target_compile_definitions(sr_native_draw_builder_test PRIVATE SR_ORACLE=0)
add_executable(sr_native_draw_builder_oracle_test
  tools/difftest/native_draw_builder_test.cpp gfx/native_draw_builder.cpp)
target_include_directories(sr_native_draw_builder_oracle_test PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_features(sr_native_draw_builder_oracle_test PRIVATE cxx_std_20)
target_compile_definitions(sr_native_draw_builder_oracle_test PRIVATE SR_ORACLE=1)
add_executable(sr_native_owner_rects_test tools/difftest/native_owner_rects_test.cpp)
target_include_directories(sr_native_owner_rects_test PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_features(sr_native_owner_rects_test PRIVATE cxx_std_20)
# Step-4 lane C (M6): renderPm4Batches' fail-closed decision, both configurations (gfx/pm4_frame_gate.hpp).
add_executable(sr_pm4_frame_gate_test tools/difftest/pm4_frame_gate_test.cpp)
target_include_directories(sr_pm4_frame_gate_test PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_features(sr_pm4_frame_gate_test PRIVATE cxx_std_20)
target_compile_definitions(sr_pm4_frame_gate_test PRIVATE SR_ORACLE=1)
add_executable(sr_pm4_frame_gate_release_test tools/difftest/pm4_frame_gate_test.cpp)
target_include_directories(sr_pm4_frame_gate_release_test PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_features(sr_pm4_frame_gate_release_test PRIVATE cxx_std_20)
target_compile_definitions(sr_pm4_frame_gate_release_test PRIVATE SR_ORACLE=0)
# step4/relcrash: gfx/par.hpp's late-worker race, forced through its SR_PAR_TEST_HOOK seam (fix + the pre-fix mutant).
add_executable(sr_par_pool_test tools/difftest/par_pool_test.cpp)
target_include_directories(sr_par_pool_test PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_features(sr_par_pool_test PRIVATE cxx_std_20)
# Step-4 lane C: the Xenos alpha test word / semantics vs Xenia's DXBC, both configurations (gfx/alpha_test.hpp).
add_executable(sr_alpha_test_test tools/difftest/alpha_test_test.cpp)
target_include_directories(sr_alpha_test_test PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_features(sr_alpha_test_test PRIVATE cxx_std_20)
target_compile_definitions(sr_alpha_test_test PRIVATE SR_ORACLE=1)
add_executable(sr_alpha_test_release_test tools/difftest/alpha_test_test.cpp)
target_include_directories(sr_alpha_test_release_test PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_features(sr_alpha_test_release_test PRIVATE cxx_std_20)
target_compile_definitions(sr_alpha_test_release_test PRIVATE SR_ORACLE=0)
# Step-4 lane C round 2: the pure volume helpers (gfx/volume_layout.hpp fetchSpans / swapGuestWords).
# step 4 4(c): the present-dump writer queue alone (no file, no log)
add_executable(sr_present_dump_writer_test tools/difftest/present_dump_writer_test.cpp gfx/present_dump_writer.cpp)
target_include_directories(sr_present_dump_writer_test PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_features(sr_present_dump_writer_test PRIVATE cxx_std_20)
if(WIN32)  # step 4 7a: the host HLSL key (= tools/regen/host_hlsl_aot.py key()) and the gated lookup
  add_executable(sr_host_hlsl_test tools/difftest/host_hlsl_test.cpp gfx/host_hlsl.cpp)
  target_include_directories(sr_host_hlsl_test PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_features(sr_host_hlsl_test PRIVATE cxx_std_20)
  target_compile_definitions(sr_host_hlsl_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(sr_host_hlsl_test PRIVATE d3dcompiler)
  # step 4 2k: the pipeline manifest round trip (the parsed descriptions hit the original pipelines; WARP)
  add_executable(sr_pso_manifest_test tools/difftest/pso_manifest_test.cpp gfx/pso_manifest.cpp gfx/native_pipeline.cpp)
  target_include_directories(sr_pso_manifest_test PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_features(sr_pso_manifest_test PRIVATE cxx_std_20)
  target_compile_definitions(sr_pso_manifest_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(sr_pso_manifest_test PRIVATE d3d12 dxgi d3dcompiler)
  # step 4 2k (Astra 10:2x #4 / #5): the warm-up's lifecycle - a bounded exit, attach / shutdown serialized (WARP)
  add_executable(sr_pso_warm_test tools/difftest/pso_warm_test.cpp gfx/pso_warm.cpp gfx/pso_manifest.cpp
    gfx/native_pipeline.cpp app/log.cpp)
  target_include_directories(sr_pso_warm_test PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_features(sr_pso_warm_test PRIVATE cxx_std_20)
  target_compile_definitions(sr_pso_warm_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(sr_pso_warm_test PRIVATE d3d12 dxgi d3dcompiler)
endif()
add_executable(sr_volume_layout_test tools/difftest/volume_layout_test.cpp)
target_include_directories(sr_volume_layout_test PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_features(sr_volume_layout_test PRIVATE cxx_std_20)
# step 4 G7: the SR_TEX_CACHE_EVICT policy (age / byte budget victims, the fence-retired slot list)
add_executable(sr_tex_evict_policy_test tools/difftest/tex_evict_policy_test.cpp)
target_include_directories(sr_tex_evict_policy_test PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_features(sr_tex_evict_policy_test PRIVATE cxx_std_20)
add_executable(sr_resource_memory_test
  tools/difftest/resource_memory_test.cpp gfx/resource_memory.cpp gfx/resource_registry.cpp
  kernel/native_resource_writes.cpp)
target_include_directories(sr_resource_memory_test PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_features(sr_resource_memory_test PRIVATE cxx_std_20)
add_executable(sr_native_resource_memory_test EXCLUDE_FROM_ALL
  tools/difftest/resource_memory_test.cpp gfx/resource_memory.cpp gfx/resource_registry.cpp
  kernel/native_resource_writes.cpp
)
target_include_directories(sr_native_resource_memory_test PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_features(sr_native_resource_memory_test PRIVATE cxx_std_20)
target_compile_definitions(sr_native_resource_memory_test PRIVATE SR_ORACLE=0)
if(WIN32)
  add_executable(sr_pc_guest_access_test
    tools/difftest/pc_guest_access_test.cpp kernel/pc_controls_game.cpp
    kernel/pc_actions.cpp kernel/pc_moves.cpp kernel/pc_moves_math.cpp
    kernel/apt_call.cpp kernel/native_hardware_memory.cpp)
  target_include_directories(sr_pc_guest_access_test BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/difftest/pc_guest_fixture ${CMAKE_SOURCE_DIR})
  target_compile_features(sr_pc_guest_access_test PRIVATE cxx_std_20)
  target_compile_definitions(sr_pc_guest_access_test PRIVATE SR_ORACLE=0 NOMINMAX WIN32_LEAN_AND_MEAN)
  add_executable(sr_native_meter_witness_test
    tools/difftest/native_meter_witness_test.cpp kernel/native_meter.cpp kernel/audio_native_census.cpp)
  target_include_directories(sr_native_meter_witness_test BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/difftest/native_meter_fixture ${CMAKE_SOURCE_DIR})
  target_compile_features(sr_native_meter_witness_test PRIVATE cxx_std_20)
  target_compile_definitions(sr_native_meter_witness_test PRIVATE SR_ORACLE=0 NOMINMAX WIN32_LEAN_AND_MEAN)
  # Exercises the shipping constant-residency path, including an unreadable
  # command buffer that must never be treated as packet input.
  add_executable(sr_native_const_dedup_test
    tools/difftest/native_const_dedup_test.cpp kernel/native_const_dedup.cpp)
  target_include_directories(sr_native_const_dedup_test PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_features(sr_native_const_dedup_test PRIVATE cxx_std_20)
  target_compile_definitions(sr_native_const_dedup_test PRIVATE SR_ORACLE=0 NOMINMAX WIN32_LEAN_AND_MEAN)
  add_executable(sr_native_hardware_memory_test
    tools/difftest/native_hardware_memory_test.cpp kernel/native_hardware_memory.cpp)
  target_include_directories(sr_native_hardware_memory_test PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_features(sr_native_hardware_memory_test PRIVATE cxx_std_20)
  target_compile_definitions(sr_native_hardware_memory_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  # The test deliberately faults loads/stores inside __try; keep their SEH
  # handling in optimized builds, including exceptions outside function calls.
  target_compile_options(sr_native_hardware_memory_test PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:-fasync-exceptions> $<$<CXX_COMPILER_ID:MSVC>:/EHa>)
  add_executable(sr_native_image_store_test
    tools/difftest/native_image_store_test.cpp gfx/native_image_store.cpp)
  target_include_directories(sr_native_image_store_test PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_features(sr_native_image_store_test PRIVATE cxx_std_20)
  target_link_libraries(sr_native_image_store_test PRIVATE d3d12 dxgi)
  add_executable(sr_native_image_replay
    tools/difftest/native_image_replay.cpp gfx/native_image_store.cpp)
  target_include_directories(sr_native_image_replay PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_features(sr_native_image_replay PRIVATE cxx_std_20)
  target_compile_definitions(sr_native_image_replay PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(sr_native_image_replay PRIVATE d3d12 dxgi)
  add_executable(sr_native_pipeline_test
    tools/difftest/native_pipeline_test.cpp gfx/native_pipeline.cpp gfx/native_image_store.cpp)
  target_include_directories(sr_native_pipeline_test PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_features(sr_native_pipeline_test PRIVATE cxx_std_20)
  target_compile_definitions(sr_native_pipeline_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(sr_native_pipeline_test PRIVATE d3d12 dxgi d3dcompiler)
  # step 6 (Astra on 6af7487e): the cache as the release configuration compiles it (SR_ORACLE=0) - fails to BUILD if a
  # test seam (setCreateGateForTest / failInsertsForTest) is declared there
  add_executable(sr_native_pipeline_release_test
    tools/difftest/native_pipeline_release_test.cpp gfx/native_pipeline.cpp gfx/native_image_store.cpp)
  target_include_directories(sr_native_pipeline_release_test PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_features(sr_native_pipeline_release_test PRIVATE cxx_std_20)
  target_compile_definitions(sr_native_pipeline_release_test PRIVATE SR_ORACLE=0 NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(sr_native_pipeline_release_test PRIVATE d3d12 dxgi d3dcompiler)
  add_executable(sr_native_draw_test
    tools/difftest/native_draw_test.cpp gfx/native_draw.cpp gfx/native_upload.cpp
    gfx/native_pipeline.cpp gfx/native_image_store.cpp)
  target_include_directories(sr_native_draw_test PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_features(sr_native_draw_test PRIVATE cxx_std_20)
  target_compile_definitions(sr_native_draw_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(sr_native_draw_test PRIVATE d3d12 dxgi d3dcompiler)
  add_executable(sr_native_texture_views_test
    tools/difftest/native_texture_views_test.cpp gfx/native_draw.cpp gfx/native_upload.cpp
    gfx/native_pipeline.cpp gfx/native_image_store.cpp)
  target_include_directories(sr_native_texture_views_test PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_features(sr_native_texture_views_test PRIVATE cxx_std_20)
  target_compile_definitions(sr_native_texture_views_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(sr_native_texture_views_test PRIVATE d3d12 dxgi d3dcompiler)
  add_executable(sr_translated_constants_test
    tools/difftest/translated_constants_test.cpp gfx/translated_constants.cpp
    gfx/native_draw.cpp gfx/native_upload.cpp gfx/native_pipeline.cpp gfx/native_image_store.cpp)
  target_include_directories(sr_translated_constants_test PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_features(sr_translated_constants_test PRIVATE cxx_std_20)
  target_compile_definitions(sr_translated_constants_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(sr_translated_constants_test PRIVATE d3d12 dxgi d3dcompiler)
  # step 6 I10p: the write-only upload twin of the CB thirds on WARP + M1 (every block-use mask writes only its blocks)
  add_executable(sr_xlat_cb_gpu_test tools/difftest/xlat_cb_gpu_test.cpp)
  target_include_directories(sr_xlat_cb_gpu_test PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_features(sr_xlat_cb_gpu_test PRIVATE cxx_std_20)
  target_compile_definitions(sr_xlat_cb_gpu_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(sr_xlat_cb_gpu_test PRIVATE d3d12 dxgi)
  add_executable(sr_translated_samplers_test
    tools/difftest/translated_samplers_test.cpp gfx/translated_samplers.cpp
    gfx/native_draw.cpp gfx/native_upload.cpp gfx/native_pipeline.cpp gfx/native_image_store.cpp)
  target_include_directories(sr_translated_samplers_test PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_features(sr_translated_samplers_test PRIVATE cxx_std_20)
  target_compile_definitions(sr_translated_samplers_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(sr_translated_samplers_test PRIVATE d3d12 dxgi d3dcompiler)
  add_executable(sr_translated_textures_test
    tools/difftest/translated_textures_test.cpp gfx/translated_textures.cpp gfx/translated_samplers.cpp
    gfx/native_draw.cpp gfx/native_upload.cpp gfx/native_pipeline.cpp gfx/native_image_store.cpp)
  target_include_directories(sr_translated_textures_test PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_features(sr_translated_textures_test PRIVATE cxx_std_20)
  target_compile_definitions(sr_translated_textures_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(sr_translated_textures_test PRIVATE d3d12 dxgi d3dcompiler)
endif()
# iter-465 viewer lane: offline check of the COSTUME VIEWER model decode on the player's disc
add_executable(sr_costume_viewer_test tools/costume_viewer/viewer_test.cpp kernel/costume_viewer_model.cpp)
target_include_directories(sr_costume_viewer_test PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_features(sr_costume_viewer_test PRIVATE cxx_std_20)

# iter-463: XenosRecomp linked in for runtime shader translation (SR_SHADER_RUNTIME_XLAT) +
# dxcompiler.dll/dxil.dll/licences copied next to the exe. See cmake/xenos_xlat.cmake.
include(cmake/xenos_xlat.cmake)
sr_link_xenos_xlat(SupermanReturns)

# iter-463 audio: Xenia's FFmpeg fork (XMA frames decoder) as ffmpeg_xenia.dll, delay-loaded,
# for the native XMA context port. See cmake/ffmpeg_xenia.cmake (-DSR_FFMPEG_XENIA=OFF drops it).
include(cmake/ffmpeg_xenia.cmake)
sr_link_ffmpeg_xenia(SupermanReturns)

# iter-464 credits: the licence notices of everything compiled INTO the exe (Xenia ports and
# precompiled shaders, xxHash, SIMDe, XenonRecomp's ppc_context.h, Lua, ...) must sit next to
# it whatever options are on (THIRD_PARTY_LICENSES.md has the full texts; CREDITS.md the list).
# Copy on every game build, including recipe-only edits that do not relink.
# SRPATCH1 recipes use the player's disc; this ships no extracted game assets.
# Lane E step 5 E3-E6 r3: a mirror, not a merge - copy_directory never deletes, so a recipe file removed from
# data/port_features (r3 dropped SCRGENPOP.srpatch / BUTTONICON.srpatch) stayed in the build folder and shipped.
add_custom_target(sr_port_features
  COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:SupermanReturns>/port_features"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/data/port_features"
    "$<TARGET_FILE_DIR:SupermanReturns>/port_features")
add_dependencies(SupermanReturns sr_port_features)

add_custom_command(TARGET SupermanReturns POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/THIRD_PARTY_LICENSES.md"
    "$<TARGET_FILE_DIR:SupermanReturns>/THIRD_PARTY_LICENSES.md"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/CREDITS.md"
    "$<TARGET_FILE_DIR:SupermanReturns>/CREDITS.md"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/third_party/ffmpeg/xenia-config/LICENSE-xenia.txt"
    "$<TARGET_FILE_DIR:SupermanReturns>/LICENSE-xenia.txt")

if(SR_HAVE_FFMPEG_XENIA)
  # build/ffmpeg_xenia_test.exe: loads the DLL through the game's guard and opens XMAFRAMES.
  add_executable(ffmpeg_xenia_test tools/ffmpeg_xenia/xmaframes_open_test.cpp kernel/ffmpeg_xenia.cpp)
  target_include_directories(ffmpeg_xenia_test PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_definitions(ffmpeg_xenia_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  sr_link_ffmpeg_xenia(ffmpeg_xenia_test)

  # build/xma_offline.exe: the native XMA port (kernel/apu) run OFFLINE over real game packets
  # (EA SNR/SNS streams carved from sound.AST, or the Xenia oracle's golden-dump contexts) ->
  # PCM WAVs. No guest code, no game process. See tools/xma_offline/xma_offline.cpp.
  add_executable(xma_offline tools/xma_offline/xma_offline.cpp
    kernel/apu/xma_context_new.cpp kernel/apu/xma_ffmpeg_decoder.cpp kernel/ffmpeg_xenia.cpp
    app/log.cpp)
  target_include_directories(xma_offline PRIVATE ${CMAKE_SOURCE_DIR})
  target_compile_definitions(xma_offline PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_features(xma_offline PRIVATE cxx_std_20)
  sr_link_ffmpeg_xenia(xma_offline)
endif()

if(BUILD_GUEST OR BUILD_GUEST_REXGLUE)
  add_subdirectory(guest)
  if(SR_REPRODUCIBLE AND WIN32)
    # step4/repro: relative to the build directory the link runs in, so the PDB's recorded link command line does not
    # name the build tree (an absolute -libpath would make the PDB, and through /Brepro the exe, differ per directory)
    target_link_options(SupermanReturns PRIVATE "LINKER:/libpath:guest")
  else()
    target_link_directories(SupermanReturns PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/guest)
  endif()
  target_link_libraries(SupermanReturns PRIVATE SupermanReturnsGuest)
  if(SR_RELEASE_COMPOSITION)
    # C1: the guest library's SR_ORACLE readers (the import glue, fm_module_walker), compiled for THIS executable with
    # SR_ORACLE=0 (guest/CMakeLists.txt: difftest gets its own SR_ORACLE=1 compile of the same units).
    target_sources(SupermanReturns PRIVATE $<TARGET_OBJECTS:sr_guest_glue_release>)
  endif()
  target_compile_definitions(SupermanReturns PRIVATE BUILD_GUEST=1)

  # ── L6 differential CPU-tester (tools/difftest) ─────────────────────────────
  # Standalone executable that runs ONE recompiled guest function in isolation and
  # dumps its register deltas in the golden format (docs/DIFFTEST_DESIGN.md §4), so
  # the recomp result can be diffed against the Ghidra/hand oracle.
  #
  # It links the SAME recomp static library (SupermanReturnsGuest) the game links,
  # and includes the SAME guest headers, so the PPCContext layout is bit-identical
  # (the one correctness-critical constraint per §6). It also links the SAME harness
  # sources the game uses, minus app/main.cpp (HARNESS_SOURCES_NO_MAIN), because the
  # recomp objects can't be cherry-picked at TU granularity — pulling the leaf's
  # object drags in sibling functions that reference the kernel import glue. The real
  # kernel objects resolve those symbols; Tier-0 leaves call NONE of that machinery
  # at runtime (no memory, no calls), they only need it linked. Leaves are invoked by
  # symbol directly (no PPCFuncMappings/dispatch table).
  #
  #   cmake --build build --target difftest
  #   ./build/difftest --self-test
  #   ./build/difftest --cases scripts/difftest/cases/sub_82670774.cases.json
  add_executable(difftest
    tools/difftest/difftest_main.cpp
    tools/difftest/arena_wait_test.cpp
    tools/difftest/trace_idle_bench.cpp
    tools/difftest/physwin_test.cpp
    tools/difftest/physalloc_test.cpp
    tools/difftest/physlifo_test.cpp
    tools/difftest/guest_alloc_fail_test.cpp
    tools/difftest/load_marker_test.cpp
    tools/difftest/tex_census_test.cpp
    tools/difftest/gpu_phys_route_test.cpp
    tools/difftest/physib_test.cpp
    tools/difftest/hrr_test.cpp
    tools/difftest/vfetch_pair_test.cpp
    tools/difftest/edram_test.cpp
    tools/difftest/obref_test.cpp
    tools/difftest/kpcr_test.cpp
    tools/difftest/xam_content_test.cpp
    tools/difftest/gpu_hang_wd_test.cpp
    tools/difftest/mod_overlay_test.cpp
    tools/difftest/mod_manager_test.cpp
    tools/difftest/costume_animation_test.cpp
    tools/difftest/cloth_capacity_test.cpp
    tools/difftest/mouse_ui_test.cpp
    tools/difftest/wait_sem_test.cpp
    tools/difftest/cs_reinit_test.cpp
    tools/difftest/entry_pool_test.cpp
    tools/difftest/seg_arena_test.cpp
    tools/difftest/import_fixes_test.cpp
    tools/difftest/import_audit_test.cpp   # SR_AUDIT_IMPORTS builds test the census; others answer "not compiled"
    tools/difftest/fp_platform_test.cpp
    tools/difftest/callring_test.cpp
    tools/difftest/heap_dump_test.cpp   # step4/relcrash: SR_CRASH_DUMP_HEAP + SR_HEAP_SENTINEL
    tools/difftest/fp_ref_sse41.cpp
    tools/difftest/collision_watch_test.cpp
    tools/difftest/icall_test.cpp
    tools/difftest/xma_test.cpp
    kernel/apu/xma_register_oracle.cpp # test oracle only; excluded from the application
    tools/difftest/cmdq_test.cpp
    tools/difftest/native_hardware_allocation_test.cpp
    tools/difftest/audio_out_test.cpp
    tools/difftest/timebase_test.cpp
    tools/difftest/shtime_hold_test.cpp
    tools/difftest/hdr_adaptation_test.cpp
    tools/difftest/seg_tile_test.cpp
    tools/difftest/callback_test.cpp
    tools/difftest/native_zpd_test.cpp
    tools/difftest/native_bindless_test.cpp   # step 6 I3-B (SR_ORACLE): --native-bindless-test
    tools/difftest/apc_guard_test.cpp   # step 6 G14b (the APC spin guard): --apc-guard-test
    tools/difftest/diag_g6a_test.cpp   # step 6 G6a (the DIAG, SR_ORACLE): --diag-g6a-test
    tools/difftest/native_display_test.cpp
    tools/difftest/native_vblank_test.cpp
    tools/difftest/graphics_interrupt_registration_test.cpp
    tools/difftest/present_gate_test.cpp
    tools/difftest/swap_block_test.cpp
    tools/difftest/swap_sdk_test.cpp
    tools/difftest/present_composed_test.cpp
    tools/difftest/native_memory_test.cpp
    tools/difftest/native_transport_test.cpp
    tools/difftest/native_pump_test.cpp
    tools/difftest/native_pump_driver_test.cpp
    tools/difftest/native_recording_test.cpp
    tools/difftest/native_record_adapter_test.cpp
    tools/difftest/shader_const_runs_test.cpp
    tools/difftest/highfps_test.cpp
    tools/difftest/devtools_test.cpp
    tools/difftest/host_automation_test.cpp
    tools/difftest/quit_test.cpp
    tools/difftest/stdio_guard_test.cpp
    tools/difftest/log_async_test.cpp
    tools/difftest/log_legacy_ref.cpp   # test oracle only: the d71946e2 logger (app/log-async same-bytes case)
    tools/difftest/pc_controls_test.cpp
    tools/difftest/game_options_test.cpp
    tools/difftest/asset_receipt_test.cpp
    tools/difftest/pc_actions_test.cpp
    tools/difftest/host_input_test.cpp
    tools/difftest/sdl3_platform_test.cpp   # RELEASE_PLAN step 3: SDL2 semantics kept on SDL3
    tools/difftest/glyph_test.cpp
    tools/difftest/mod_library_test.cpp
    tools/difftest/mod_library_render.cpp
    tools/difftest/map_markers_test.cpp
    tools/difftest/native_census_test.cpp
    tools/difftest/native_meter_test.cpp
    tools/difftest/mem_resources_test.cpp
    tools/difftest/resource_memory_d3d12_test.cpp
    tools/difftest/xlat_cb_zerocopy_test.cpp   # step 6 I10p (Astra r2): the view path's binder + lifetime on WARP
    tools/difftest/resource_consumer_test.cpp
    tools/difftest/resource_reader_hooks_test.cpp
    tools/difftest/shader_boot_load_test.cpp
    tools/difftest/achievements_test.cpp   # step5 E14a: --achievements-test
    tools/difftest/achievements_page_test.cpp   # lane E L-a: --achievements-page-test
    ${HARNESS_SOURCES_NO_MAIN})
  target_include_directories(difftest PRIVATE
    ${CMAKE_SOURCE_DIR}/guest
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/XenonRecomp/thirdparty/simde
  )
  target_compile_features(difftest PRIVATE cxx_std_20)
  sr_use_xenia_shaders(difftest)
  # iter-463: the ROUNDPS reference for --fp-platform-test is the ONLY -msse4.1 TU (the guest
  # lib and every other difftest TU stay without it, so simde's libm fallbacks are what runs).
  set_source_files_properties(tools/difftest/fp_ref_sse41.cpp PROPERTIES COMPILE_OPTIONS "-msse4.1")
  target_compile_definitions(difftest PRIVATE BUILD_GUEST=1 SR_DIFFTEST=1)
  set_source_files_properties(tools/difftest/native_hardware_allocation_test.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:Clang>:-fasync-exceptions>;$<$<CXX_COMPILER_ID:MSVC>:/EHa>")
  target_link_directories(difftest PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/guest)
  # SupermanReturnsGuest twice-bracketed with the harness so the static-lib member
  # resolution (recomp objects <-> kernel objects) closes in either order.
  target_link_libraries(difftest PRIVATE SupermanReturnsGuest ${CRASH_LIBS} sr_imgui)
  if(SR_RELEASE_COMPOSITION)
    # C1 (Astra round 1 on release/emu-removal stage 0): difftest links the oracle-enabled (SR_ORACLE=1) compile of the
    # guest library's SR_ORACLE readers, so its selectors (xmaNewEnabled, the N0 frame-split gate) follow its own
    # xmaSetEnabledForTest / env as in the development configuration (guest/CMakeLists.txt).
    target_sources(difftest PRIVATE $<TARGET_OBJECTS:sr_guest_glue_dev>)
  endif()
  # iter-463 audio: the XMA port's real-FFmpeg cases (--xma-test) - delay-loaded, guarded.
  sr_link_ffmpeg_xenia(difftest)
  target_link_libraries(difftest PRIVATE SDL3::SDL3)
  sr_deploy_sdl3(difftest)
  if(WIN32)
    target_compile_definitions(difftest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(difftest PRIVATE d3d12 dxgi xinput shell32 windowscodecs ole32 d3dcompiler)  # d3dcompiler: --native-bindless-test's shaders
    target_link_options(difftest PRIVATE
      "LINKER:/MANIFEST:EMBED" "LINKER:/MANIFESTINPUT:${SR_WINDOWS_MANIFEST}")
    set_property(TARGET difftest APPEND PROPERTY LINK_DEPENDS "${SR_WINDOWS_MANIFEST}")
    # The recomp call chain can be stack-hungry; match the game's generous stack.
    target_link_options(difftest PRIVATE "LINKER:/STACK:67108864")
  endif()
endif()

# ── Precompiled header for harness sources ────────────────────────────────────
# Eliminates redundant header parsing across kernel/*.cpp and app/*.cpp.
# Expected improvement: 20-40% faster incremental harness builds.
# PCH is NOT applied to the guest library (ppc_recomp.*.cpp) — those files
# include a different, heavier header set and are already parallelized well.
target_precompile_headers(SupermanReturns PRIVATE
  <cstdint>
  <cstddef>
  <cstring>
  <cassert>
  <atomic>
  <mutex>
  <thread>
  <string>
  <vector>
  <unordered_map>
  <functional>
  <memory>
  "$<$<BOOL:${WIN32}>:<windows.h$<ANGLE-R>>"
)

# NATIVE N0.9 (docs/NATIVE_ROADMAP.md §6.2 L0): Xenos conformance units - Xenia-ported
# reference functions vs our header-only gfx/ code. Not part of ALL (the game build is
# unchanged); build and run explicitly:
#   cmake --build build/ --target xenos_conformance && build/tests/xenos_conformance/xenos_conformance.exe --verify
add_subdirectory(tests/xenos_conformance EXCLUDE_FROM_ALL)

# Admission fixture with the actual release harness and guest glue. It owns a
# small WARP device and queues native draws; no fixture code enters the game.
if(WIN32 AND BUILD_GUEST AND SR_RELEASE_COMPOSITION)
  get_target_property(_sr_draw_test_sources SupermanReturns SOURCES)
  list(REMOVE_ITEM _sr_draw_test_sources app/main.cpp app/app_icon.rc)
  add_executable(sr_native_draw_admission_test EXCLUDE_FROM_ALL
    tools/difftest/native_draw_admission_test.cpp ${_sr_draw_test_sources})
  foreach(_sr_prop INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS
                   COMPILE_FEATURES LINK_LIBRARIES LINK_DIRECTORIES LINK_OPTIONS)
    get_target_property(_sr_value SupermanReturns ${_sr_prop})
    if(_sr_value)
      set_property(TARGET sr_native_draw_admission_test PROPERTY ${_sr_prop} "${_sr_value}")
    endif()
  endforeach()
  target_link_libraries(sr_native_draw_admission_test PRIVATE d3dcompiler)
  target_precompile_headers(sr_native_draw_admission_test REUSE_FROM SupermanReturns)
  sr_use_xenia_shaders(sr_native_draw_admission_test)
  sr_deploy_sdl3(sr_native_draw_admission_test)
endif()


# Metadata controls only; actual hook/wiring proof remains a difftest obligation.
add_executable(sr_guest_reader_probe_test EXCLUDE_FROM_ALL
  tools/difftest/resource_reader_probe_test.cpp gfx/resource_reader_probe.cpp)
target_include_directories(sr_guest_reader_probe_test PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_features(sr_guest_reader_probe_test PRIVATE cxx_std_20)

# ── SR_ASAN_TESTS (step 6 N4; the option's note is beside ASAN above) ─────────────────────────────────────────────
# Every difftest / sr_*_test executable of this directory: -fsanitize=address with ASAN=ON's flags (the dynamic runtime
# and its /MD thunk, no stack redzones, the MSVC STL container annotations off - the guest library, sr_imgui and the
# other static libraries stay uninstrumented), CodeView + /DEBUG so a report names its frames, and the runtime DLL
# copied beside each. Under it kernel/native_flush.cpp poisons a parked Entry::Impl (step 6 I6), and
# difftest --entry-pool-test touch-after-give | touch-after-cross-give must stop with a use-after-poison report.
# (A test that reuses another target's precompiled header is skipped: its flags must match that target's.)
if(SR_ASAN_TESTS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SR_ASAN_TESTS=ON needs the pinned clang toolchain")
  endif()
  execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir OUTPUT_VARIABLE _sr_asan_rd
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(_sr_asan_tests_dll "${_sr_asan_rd}/lib/windows/clang_rt.asan_dynamic-x86_64.dll")
  if(NOT EXISTS "${_sr_asan_tests_dll}")
    message(FATAL_ERROR "SR_ASAN_TESTS=ON: no ASan runtime at ${_sr_asan_tests_dll}")
  endif()
  get_property(_sr_asan_all DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  set(_sr_asan_tests "")
  foreach(_sr_t IN LISTS _sr_asan_all)
    get_target_property(_sr_ty ${_sr_t} TYPE)
    if(_sr_ty STREQUAL "EXECUTABLE" AND (_sr_t STREQUAL "difftest" OR _sr_t MATCHES "^sr_.+_test$"))
      get_target_property(_sr_pch ${_sr_t} PRECOMPILE_HEADERS_REUSE_FROM)
      if(NOT _sr_pch)
        target_compile_options(${_sr_t} PRIVATE -fsanitize=address -fno-omit-frame-pointer -fms-runtime-lib=dll
                               "SHELL:-mllvm -asan-stack=0" -gcodeview)
        target_link_options(${_sr_t} PRIVATE -fsanitize=address -fms-runtime-lib=dll "LINKER:/DEBUG")
        target_compile_definitions(${_sr_t} PRIVATE SR_ASAN_BUILD=1 _DISABLE_STRING_ANNOTATION
                                   _DISABLE_VECTOR_ANNOTATION)
        add_custom_command(TARGET ${_sr_t} POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_sr_asan_tests_dll}" "$<TARGET_FILE_DIR:${_sr_t}>")
        list(APPEND _sr_asan_tests ${_sr_t})
      endif()
    endif()
  endforeach()
  list(LENGTH _sr_asan_tests _sr_asan_n)
  message(WARNING "SR_ASAN_TESTS=ON: ${_sr_asan_n} test executables built with AddressSanitizer (a diagnostic test "
                  "build; make_player_build.ps1 refuses the tree): ${_sr_asan_tests}")
endif()
