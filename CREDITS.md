# Credits

This port stands on other people's work. This page names the people and the projects, says
what we use from each, and under which licence. The full licence texts a binary release must
carry are in [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).

The source of truth is [`data/credits.json`](data/credits.json).
[`scripts/check_credits.py`](scripts/check_credits.py) fails, and the pre-commit hook blocks
the commit, when the tracked tree has a submodule, vendored tree, licence file, attribution
header, dependency or shipped asset that has no credits entry.

> **The rule.** If you port code, vendor a library, add a dependency, ship a binary or add an
> asset, add or extend its entry in `data/credits.json` **in the same commit**. Also add its
> section here, and in `THIRD_PARTY_LICENSES.md` if it ships. Each section carries a
> `<!-- credits:ID -->` marker that the checker matches.

## People

- **Lachlan Oliver** ([@loliver1823](https://github.com/loliver1823)): project owner and director.
  He set the goals and priorities, made the product and direction decisions and tested the game
  by playing it. He is the only human author in the development history.
- **Claude** (Anthropic), through Claude Code: wrote nearly all of the code and documentation, as
  AI agents working under the owner's direction. Its Co-Authored-By trailer is on 2,344 of the
  2,625 commits of the development history (2026-10-09). See
  [`docs/HOW_IT_WAS_BUILT.md`](docs/HOW_IT_WAS_BUILT.md).
- **OpenAI Codex**: reviewed claimed results for a period, did one integration pass, and gives
  blind second opinions on visual milestones.
- The authors of every project below. Each is credited the way its own README or licence
  names its authors.

**No endorsement.** The authors of XenonRecomp, XenosRecomp, Xenia, Unleashed Recompiled and the
other projects credited here do not endorse this project and were not involved in it. Crediting
them says only that we use or learned from their work.

## The game

<!-- credits:superman-returns -->
### Superman Returns (Xbox 360)

*Superman Returns* (2006) was developed by EA Tiburon and published by Electronic Arts. It is
based on the Warner Bros. Pictures film. Superman and all related characters and elements are
trademarks of DC Comics. All trademarks belong to their respective owners. This project is not
affiliated with or endorsed by any of them.

**This project contains no game assets and grants no rights to the game.** You need your own
legally obtained copy of the Xbox 360 game. Place the extracted disc at `Superman Returns (USA)/`.
The build recompiles your XEX, and the game reads its data from your copy at runtime.
See [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md#the-game) for what a release may
contain.

<!-- credits:lua -->
### Lua 5.1, inside the game

The game's script VM is stock-shaped Lua 5.1 (Lua.org, PUC-Rio, MIT licence; see
[`docs/ENGINE_MAP.md`](docs/ENGINE_MAP.md) section 6c). Lua is part of the game's own code,
which we recompile, so the exe carries a translation of it. We reproduce its notice as a
courtesy. This project adds no Lua code of its own.

## Code we port, link or compile in (it ships)

<!-- credits:xenia -->
### Xenia / Xenia Canary: BSD-3-Clause

<https://github.com/xenia-canary/xenia-canary>. By Ben Vanik and the Xenia project
contributors, and the Xenia Canary contributors.

Xenia is the project's **oracle**: its trace-dump renders, golden memory dumps and live runs
are what we judge against. It is also the semantic reference for the whole platform layer.
We use the snapshot `reference/xenia-canary` @0b4e6c44 and the oracle build
`canary_experimental` @0bd090dbe.

Every ported file names its Xenia source file in its header:
- **GPU:** Xenos microcode and the control-flow walk (`gfx/xenos_ucode.*`, `gfx/shader_mkey.hpp`),
  texture tiling (`gfx/xenos_tiling.hpp`, `gfx/cube_layout.hpp`), the EDRAM model
  (`gfx/edram.hpp`), the gamma ramp (`gfx/gamma_ramp.hpp`) and the PM4 trace format
  (`gfx/pm4_live_trace.*`, `gfx/gpu_trace_replay.*`).
- **Resolution scaling:** the internal draw resolution scale (`gfx/res_scale.*`, and the scaled
  render target, EDRAM, resolve, texture and present paths in `gfx/d3d12_pm4_*.cpp` and
  `gfx/d3d12_backend.cpp`) ports Xenia's `draw_resolution_scale_x/_y`: its scaled resolve memory,
  the `scaled_resolve` texture key and the scaled viewport rules (`docs/RESOLUTION.md`).
- **Shared-memory write tracking:** `kernel/gpu_write_watch.*` ports the model of Xenia's
  `PhysicalHeap::EnableAccessCallbacks` / `TriggerCallbacks` (`src/xenia/memory.cc`) and
  `SharedMemory::MakeRangeValid` / `MemoryInvalidationCallback` (`src/xenia/gpu/shared_memory.cc`):
  pages the GPU read are write-protected before they are read, a CPU write faults and
  invalidates its 256 KB block, and host kernel-mode reads into guest memory release the
  pages first.
- **Compiled-in shaders:** `gfx/d3d12_pm4_shmem.cpp` `#include`s Xenia's `resolve_*` and
  `texture_load_*` compute shaders (and their `*_scaled` variants) as the DXBC headers Xenia's build
  makes. The build compiles them with the Windows SDK's `fxc.exe` and Xenia's arguments
  (`cmake/xenia_shaders.cmake`) from Xenia's shader sources, vendored verbatim with their headers in
  `third_party/xenia-shaders/` (`tools/vendor_xenia_shaders.py`; `SOURCES.txt` lists every file,
  its commit and its hash).
- **Post-process anti-aliasing:** the present chain's post stage (`gfx/d3d12_present_chain.cpp`,
  PC SETTINGS > GRAPHICS > POST AA) `#include`s Xenia's FXAA compute shaders (`fxaa_cs`,
  `fxaa_extreme_cs`, built the same way) and runs them as Xenia's `postprocess_antialiasing` does, after a
  luma pass that reproduces `apply_gamma_*_fxaa_luma` (NVIDIA FXAA 3.11, credited below).
- **Native resolve (N4a):** `gfx/native_resolve.inc` reproduces Xenia's `resolve_fast_*` /
  `resolve_full_*` compute shaders (`src/xenia/gpu/shaders/resolve_*.xesli`) with the EDRAM
  load replaced by a host render-target read. It compiles at run time against Xenia's xesl
  include closure (`endian`, `pixel_formats`, `resolve`, `edram`, `texture_address`,
  `ui/shaders/xesl`), embedded verbatim with their headers in
  `third_party/xenia-shaders/xesl_embed.inc` (generated by `tools/gen_xesl_embed.py`).
- **Audio:** the XMA decoder and the audio output path (`kernel/apu/*`, `kernel/ffmpeg_xenia.cpp`),
  plus the FFmpeg config overlay, copied verbatim (`third_party/ffmpeg/xenia-config/`).
- **Kernel:** the KPCR/KTHREAD layouts (`kernel/kpcr.hpp`, `kernel/kthread_objects.*`), the
  physical-address lookup (`kernel/mm_phys.*`), XAM content and profile
  (`kernel/xam_content.*`, `kernel/xam_profile.*`), and the placement of the virtual heaps,
  thread stacks and system-heap blocks (`BaseHeap::AllocRange`, `XThread::AllocateStack`,
  `Memory::SystemHeapAlloc`: `kernel/ntavm_xenia_heap.*`, `kernel/thread_mem_xenia.*`).
- **Achievements:** the XDBF / SPA layout the achievements core reads (`kernel/achievements.*`: the
  XACH achievement table, the XSTR string tables, XSTC and XTHD) follows the same
  `src/xenia/kernel/xam/xdbf/xdbf_io.*` and `spa_info.*`; the table comes from the player's
  `default.xex` at run time.
- **Window icon:** the XDBF reader and the title-icon key (image section, id `0x8000`) in
  `app/window_icon.cpp` follow `src/xenia/kernel/xam/xdbf/xdbf_io.*` and `spa_info.*`; like
  `emulator.cc`, the port shows the game's own title icon (read from the player's `default.xex`
  at run time) on its window and taskbar button.
- **Tests and tools:** the conformance reference functions (`tests/xenos_conformance/xenia_ref_*.hpp`),
  the trace and oracle patches applied to a Xenia build (`tools/xenia-trace/`), the PPC
  interpreter's VMX semantics (`scripts/difftest/golden_call/xenon_interp.py`),
  `tools/check_cube_layout.py`, `tools/check_lut_untile.py` and `tools/difftest/edram_test.cpp`.

- **Present effects:** `gfx/d3d12_present_chain.cpp` embeds Xenia's guest-output pixel shaders
  (bilinear, FidelityFX CAS resample / sharpen, FSR EASU / RCAS; built the same way) and follows its
  presenter's effect flow (`src/xenia/ui/presenter.cc`) for the `upscale_filter` / `sharpness`
  options (docs/RESOLUTION.md).

<!-- credits:amd-fidelityfx -->
### AMD FidelityFX CAS and FSR 1: MIT

<https://github.com/GPUOpen-Effects/FidelityFX-CAS> and
<https://github.com/GPUOpen-Effects/FidelityFX-FSR>, by Advanced Micro Devices, Inc. (`ffx_a.h` also
carries a 2014 copyright of Michal Drobot under the same MIT notice). Contrast Adaptive Sharpening
and FidelityFX Super Resolution 1 (EASU, RCAS), compiled into the Xenia guest-output shaders above.
The headers (`ffx_a.h`, `ffx_cas.h`, `ffx_fsr1.h`) and the two licence files are vendored verbatim
from Xenia's submodules (both v1.0.2) in `third_party/xenia-shaders/third_party/`.

<!-- credits:nvidia-fxaa -->
### NVIDIA FXAA 3.11: BSD-3-Clause

By Timothy Lottes, Copyright (c) 2014-2015 NVIDIA Corporation. FXAA 3.11 (`FXAA3_11.h`, Xenia's
`third_party/fxaa`, vendored verbatim in `third_party/xenia-shaders/third_party/fxaa/`) is compiled
into the Xenia FXAA shaders the post stage embeds. The licence text ships as
`third_party/fxaa/LICENSE`.

<!-- credits:ffmpeg-xenia -->
### FFmpeg, Xenia's XMA fork: LGPL-2.1-or-later

<https://github.com/has207/FFmpeg> (branch `xmaframes`), by the FFmpeg developers. Xenia Canary
pins this fork. The submodule is `third_party/ffmpeg/FFmpeg` @d980192.

`cmake/ffmpeg_xenia.cmake` builds libavcodec and libavutil (the mp3, wmapro, wmav2 and
xmaframes decoders, LGPL-only, no asm) into **one DLL, `ffmpeg_xenia.dll`**, which the game
delay-loads. Users can rebuild or replace it. Its `xmaframes` decoder is the game's XMA audio
decoder: Xenia's packet logic in `kernel/apu/` hands it one XMA frame at a time.
[`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md) explains how the LGPL is met.

<!-- credits:sdl3 -->
### SDL3: zlib

<https://www.libsdl.org/>, by Sam Lantinga and the SDL contributors. Provides the window, input
(keyboard, mouse, gamepads and their hot-plug) and the audio device. vcpkg builds it (version 3.4.18,
the overlay port `third_party/vcpkg-overlay/sdl3`), and the build copies `SDL3.dll` next to the
exe. SDL2 (2.32.10) did the same job until RELEASE_PLAN step 3 (2026-10-18).

<!-- credits:xenonrecomp -->
### XenonRecomp (and XenonAnalyse): MIT

<https://github.com/hedge-dev/XenonRecomp>, by hedge-dev and contributors. The UnleashedRecomp
README names Skyth as its creator and Sajid as the author of XenonAnalyse. Submodule
`tools/XenonRecomp` @ddd128b, upstream `main`. The build applies our patches
(`tools/regen/patches/0001`-`0005`) to a copy of that commit in the build tree
(`tools/regen/regen.py`); the submodule itself stays unmodified.

XenonRecomp is the PowerPC → C++ recompiler that generates `guest/ppc/` from your XEX. Its
runtime header `ppc_context.h` is compiled into the exe; our modified copy is
`tools/regen/hand/ppc_context.h`. Our patches to the recompiler are in `tools/regen/patches/`.
XenonRecomp's own README credits Xenia, and
Xenia Canary's XEX patching code.

<!-- credits:simde -->
### SIMDe: MIT

<https://github.com/simd-everywhere/simde>, by Evan Nemerson and the SIMDe contributors. A
header-only SIMD layer that the generated guest code includes, from XenonRecomp's
`thirdparty/simde`.

<!-- credits:xenosrecomp -->
### XenosRecomp: MIT

<https://github.com/hedge-dev/XenosRecomp>, by hedge-dev and contributors (created by Skyth,
per the UnleashedRecomp README).

XenosRecomp translates Xenos shaders to HLSL/DXIL. We use it offline to build
`data/shader_cache`, and it is compiled **into** the game as the static library `xenos_xlat`
for runtime cache misses (`cmake/xenos_xlat.cmake`, `gfx/xenos_bridge.*`). Our patch series
(`tools/xenosrecomp/*.patch`, which partly ports Xenia code) is applied at configure time
(`tools/xenosrecomp/prepare.py`) to an export of the commit the `third_party/XenosRecomp`
submodule pins (upstream 990d03b); the submodule itself stays pristine.

<!-- credits:fmt -->
### {fmt}: MIT

<https://github.com/fmtlib/fmt>, by Victor Zverovich and the {fmt} contributors. Header-only,
compiled into `xenos_xlat`.

<!-- credits:xxhash -->
### xxHash: BSD-2-Clause

<https://github.com/Cyan4973/xxHash>, by Yann Collet. The vendored single header is
`gfx/thirdparty/xxhash.h` (v0.8.3). It hashes for the shader cache, textures, pages and the
runtime translator.

<!-- credits:dxc -->
### DirectX Shader Compiler: NCSA and Microsoft terms

<https://github.com/microsoft/DirectXShaderCompiler>, by Microsoft and the LLVM /
DirectXShaderCompiler contributors. The game loads `dxcompiler.dll` and `dxil.dll` (v1.8.2407)
at runtime to translate shaders: the official release zip, which the build downloads and checks
by SHA-256 (`cmake/dxc_release.cmake`). The licence texts are `tools/xenosrecomp/LICENSE-DXC.txt`,
`LICENSE-DXC-MS.txt` and `LICENSE-DXC-MIT.txt` (the release zip's three licence files); the build
copies them next to the exe.

<!-- credits:dear-imgui -->
### Dear ImGui: MIT

<https://github.com/ocornut/imgui>, by Omar Cornut and the Dear ImGui contributors. It draws
the in-game **mod library** (MAIN MENU > MODS): the font atlas, the draw lists and the
DirectX 12 renderer backend. Vendored unmodified under `third_party/imgui/` (v1.91.9b, commit
4806a19, with its `LICENSE.txt`); its `imstb_*` headers are Sean Barrett's stb libraries
(public domain or MIT). The library's look and widgets are our own draw code.

## Assets

<!-- credits:julio-cacko-input-prompts -->
### FREE Input Prompts Pack, by Julio Cacko: CC0 1.0 (assets) / MIT (code)

<https://juliocacko.itch.io/free-input-prompts> (also <https://juliocacko.gumroad.com/l/free-input-pack>).
Thank you, Julio. The PlayStation, Switch and keyboard button prompts the port shows come from
this pack: an unmodified subset under `data/glyphs/juliocacko_input_prompts/` (its `SOURCE.md`
lists what and why; [`docs/INPUT_GLYPHS.md`](docs/INPUT_GLYPHS.md) describes the feature). Xbox
pads keep the game's own Xbox 360 prompt art.

The store page, read 2026-10-02, says: *"Free to use in any project, commercial or
non-commercial. No attribution required (but appreciated!)"*. We credit Julio Cacko anyway.
The downloaded pack has no licence file, so we ship the CC0 1.0 legal code next to the art
(`LICENSE.txt`). No code from the pack is used.

### The app icon and the mouse cursor: our own, AI-made

`app/app_icon.ico`, the icon Explorer shows for `SupermanReturns.exe`, is an original design
(a red double chevron over a gold line on a midnight-blue tile) drawn by
`tools/player/make_app_icon.py`. It contains no game art and no DC trademark. The window itself
shows the game's own title icon once the player's `default.xex` has loaded (see Xenia above).

The in-game mouse cursor is procedural: a pixel shader in `gfx/d3d12_overlay.cpp` draws it, with
no image file ([`docs/MOUSE_CURSOR.md`](docs/MOUSE_CURSOR.md)).

**Both were designed by the AI agents, not by a human artist.**

<!-- credits:saira-font -->
### Saira, by Omnibus-Type: SIL Open Font License 1.1

<https://github.com/Omnibus-Type/Saira>, Copyright 2020 The Saira Project Authors. The mod
library's typeface: its wide, flat-terminal italic is the closest open match we found to the
game's own extended menu lettering. We ship static instances of the variable fonts (Expanded
Black Italic, Expanded Bold Italic, Expanded SemiBold, Medium), subset to Latin and renamed
"Saira SR" as the OFL asks of a modified version, under `data/fonts/saira/` with `OFL.txt`.
`tools/modlib/make_fonts.py` rebuilds them from the google/fonts originals.

## Reference projects (read, never compiled in)

<!-- credits:unleashedrecomp -->
### Unleashed Recompiled: GPL-3.0

<https://github.com/hedge-dev/UnleashedRecomp>, by hedge-dev: Skyth, Sajid, Hyper, Darío and
the contributors its README lists. It is the **structural** reference for the D3D12 host
(`gpu/video.cpp`: render thread, PSO cache, command-queue shape) and for the HLE hook surface.
It is never built or linked.

**Copied-code audit, 2026-10-02:** we scanned every tracked source line by line against
UnleashedRecomp's own sources: the 201 C++ and HLSL files under `UnleashedRecomp/`, plus
`UnleashedRecompLib/` and `tools/`. Nothing was copied. The only matches are MIT licence
boilerplate, XenosRecomp's shader ABI line `cbuffer SharedConstants : register(b2, space4)`,
XenonAnalyse's jump-table section headers, and one-line idioms such as
`std::chrono::system_clock::now()`.

`gfx/hle_video.hpp` and `kernel/hle_api_observe.cpp` mirror the reference's *design*: a
`RenderCommandType` enum and a tagged-union command, cited by `video.cpp` line. The members and
the code are our own. This project is MIT-licensed (since 2026-11-16; GPL-3.0 before that), so
code from Unleashed Recompiled must not be copied in.

<!-- credits:rexglue -->
### ReXGlue SDK: BSD-3-Clause

<https://github.com/rexglue/rexglue-sdk>, by Tom Clay and the rexglue contributors (portions
derived from Xenia). We use it as a second-opinion PowerPC codegen for regenerating single
functions ([`docs/REXGLUE_BUILD.md`](docs/REXGLUE_BUILD.md)). The tree holds only our config
(`guest/config/superman_rexglue.toml`) and an entry shim (`guest/ppc_rexglue_entry.cpp`).

<!-- credits:opensage -->
### OpenSAGE: GPL-3.0

<https://github.com/OpenSAGE/OpenSAGE>, by Tim Jones and the OpenSAGE contributors. Apt format
work by Stephan Vedder (feliwir, [libapt](https://github.com/feliwir/libapt)) and jonwil
(apt2xml). We use its documentation of EA's Apt UI format
([`docs/OPENSAGE_REFERENCE.md`](docs/OPENSAGE_REFERENCE.md)) for **format knowledge only**:
character and type ids, and the `.const` layout. No OpenSAGE code is in the tree. OpenSAGE is
GPL-3.0 and includes EA-derived code, so do not copy from it.

<!-- credits:n64recomp -->
### N64: Recompiled: MIT (inspiration)

<https://github.com/N64Recomp/N64Recomp>, by Mr-Wiseguy and contributors. It is the
static-recompilation approach that inspired XenonRecomp and XenosRecomp.

<!-- credits:libertyv-xma2dec -->
### koolkdev/libertyv `xma2dec.c` (credited by Xenia; no code used)

<https://github.com/koolkdev/libertyv>, by koolkdev. `libav_wrapper/xma2dec.c` (2013) decoded
XMA by rewriting each packet header so FFmpeg's WMA Pro decoder would accept it. The repository
publishes no licence. Xenia's first XMA decoder adapted it in May 2015; Xenia dropped the
header rewrite in August 2015 and the rest of that approach in its 2020 packet-parser rewrite.
Every later Xenia XMA context, including the `xma_context_new.cc` we port (2024), still
carries the line "Credits for most of this code goes to" `xma2dec.c`, and our ported file
headers repeat that credit as historical.

**No `xma2dec.c` code is in this tree.** `tools/provenance_check.py` compares it with every
tracked C/C++ file and finds no copy, while it does detect the 2015 Xenia adaptation
(iter-467; `docs/THIRD_PARTY.md` has the command). XMA frames are decoded by FFmpeg (above),
and the packet logic around it is Xenia's BSD-3 code.

## Development tools (used, not shipped)

<!-- credits:vcpkg -->
- **vcpkg** (MIT), <https://github.com/microsoft/vcpkg>, by Microsoft and contributors.
  Submodule `third_party/vcpkg`; it builds SDL3.
<!-- credits:extract-xiso -->
- **extract-xiso**, <https://github.com/XboxDev/extract-xiso>, by *in* (in@fishtank.com),
  maintained by the XboxDev organization. Used to extract disc images during development. Its
  4-clause BSD-style licence (an advertising clause) adds conditions this repository does not want
  to pass on, so it is not part of the public repository or of any release.
<!-- credits:xdvdfs -->
- **xdvdfs** (MIT), <https://github.com/antangelo/xdvdfs>, by antangelo. An XDVDFS image tool
  used during development.
<!-- credits:ida-pro -->
- **IDA Pro** (Hex-Rays, proprietary), the primary decompiler, and **ida-pro-mcp** (MIT),
  <https://github.com/mrexodia/ida-pro-mcp>, by mrexodia.
<!-- credits:ghidra -->
- **Ghidra** (Apache-2.0, NSA), <https://github.com/NationalSecurityAgency/ghidra>, the fallback
  decompiler and PPC emulator, and **ghidra-mcp** (Apache-2.0),
  <https://github.com/bethington/ghidra-mcp>, by bethington.
<!-- credits:frida -->
- **Frida** (wxWindows Library Licence 3.1), <https://frida.re/>, by the Frida developers. Live
  inspection of the game and of Xenia.
<!-- credits:windbg -->
- **WinDbg / cdb** (Microsoft) and **mcp-windbg** (MIT), <https://github.com/svnscha/mcp-windbg>,
  by svnscha. Used for minidumps and the golden dump.
<!-- credits:build-toolchain -->
- **LLVM/Clang**, **CMake**, **Ninja** and **sccache**: the build toolchain.
<!-- credits:python-libs -->
- **Python**, **pycryptodome**, **Pillow** and **NumPy**: the `tools/` and `scripts/` pipelines.
