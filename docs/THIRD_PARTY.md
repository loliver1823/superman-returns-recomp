# Third-party code shipped with the game

What the built game (`build/`) links or ships next to the exe, under which licence, and how the
licence obligations are met. Update this file in the same commit that adds a dependency.

> iter-464 credits audit: the complete list (people, every project, reference-only and tool
> credits) is [`../CREDITS.md`](../CREDITS.md). The licence texts a release must carry, and
> the shipping checklist, are in [`../THIRD_PARTY_LICENSES.md`](../THIRD_PARTY_LICENSES.md).
> Both are kept in sync with [`../data/credits.json`](../data/credits.json) and checked by
> `scripts/check_credits.py`. This file keeps the build-level detail of how the FFmpeg and
> Xenia obligations are met.

## FFmpeg, Xenia fork (XMA frames decoder) - LGPL-2.1-or-later

- **What:** libavcodec + libavutil from https://github.com/has207/FFmpeg.git (branch `xmaframes`),
  pinned at `d980192` - the commit the Xenia oracle build (canary_experimental@0bd090dbe) pins.
  The fork adds `AV_CODEC_ID_XMAFRAMES` (the `xmaframes` decoder in `libavcodec/wmaprodec.c`),
  which the native XMA context port uses, as Xenia's `apu/xma_context_new.cc` does.
- **Where:** submodule `third_party/ffmpeg/FFmpeg`; built by `cmake/ffmpeg_xenia.cmake` into
  `build/ffmpeg_xenia.dll` (CMake option `SR_FFMPEG_XENIA`, default ON). Test:
  `build/ffmpeg_xenia_test.exe` (opens XMAFRAMES at every XMA rate, mono + stereo).
- **Configuration:** Xenia's own (`third_party/ffmpeg/xenia-config/`): LGPL only
  (`FFMPEG_LICENSE "LGPL version 2.1 or later"`, no `--enable-gpl` / nonfree parts), codecs
  mp3, wmapro, wmav2 and xmaframes, no x86 assembly.
- **How the LGPL is met:** the library is a separate DLL that the game loads dynamically
  (delay-load, `kernel/ffmpeg_xenia.cpp`), so it can be rebuilt from the pinned source and
  replaced. The post-build step copies the licence text next to the exe as
  `LICENSE-FFmpeg-LGPLv2.1.txt`. A binary release must also offer the corresponding source
  (the pinned fork commit plus `third_party/ffmpeg/xenia-config/` and `cmake/ffmpeg_xenia.cmake`).
- **Not in git:** the DLL, its import library and the object files live in `build/` only.
- **With the port's MIT licence:** the DLL keeps its LGPL terms because it stays a separate,
  replaceable library that the exe only delay-loads; the port's own code is not LGPL-covered.
- **Licence scan (iter-467):** every file the DLL compiles, and every header those files include
  from the FFmpeg tree, by licence header: 408 LGPL-2.1+, and no GPL file is compiled (the one GPL
  header, `libavutil/macos_kperf.h`, is behind `CONFIG_MACOS_KPERF`, which the overlay leaves
  unset). The permissive rest (IJG `jfdctfst.c` / `jfdctint_template.c` / `jrevdct.c`, ISC
  `faandct.c`, MIT `avsscanf.c`, zlib `adler32.c`) have their notices in
  `THIRD_PARTY_LICENSES.md`; the IJG licence asks that binary releases say the software "is
  based in part on the work of the Independent JPEG Group". Evidence:
  `archive/v1_xma/ffmpeg_licence_scan.json` (scanner `archive/v1_xma/ffmpeg_licence_scan.py`).

## Xenia Canary - BSD-3-Clause

- **What:** `third_party/ffmpeg/xenia-config/` (FFmpeg config overlay, copied verbatim from the
  oracle build's `third_party/ffmpeg-xenia/`) and the XMA decoder logic ported from
  `src/xenia/apu/` (`xma_context_new.cc`, `xma_context.{h,cc}`, `xma_decoder.cc`, `xma_helpers.h`),
  `src/xenia/base/` (`bit_stream`, `ring_buffer`), `src/xenia/kernel/xboxkrnl/xboxkrnl_audio_xma.cc`
  and `src/xenia/memory.cc` (GetPhysicalAddress) - all from the oracle build 0bd090dbe, in
  `kernel/apu/` and `kernel/mm_phys.*`. The render-driver output path (`kernel/apu/audio_system.*`)
  ports `src/xenia/apu/audio_system.{h,cc}`, `apu/sdl/sdl_audio_driver.cc`, `apu/conversion.h`
  and `kernel/xboxkrnl/xboxkrnl_audio.cc`; `KeGetCurrentProcessType` (`kernel/kpcr.cpp`) and
  `KeWaitForMultipleObjects` (`kernel/stubs.cpp`) follow `xboxkrnl_threading.cc`, `xthread.cc`
  and `xobject.cc`. Ported files carry an attribution header naming the
  Xenia source file.
- **Licence:** `third_party/ffmpeg/xenia-config/LICENSE-xenia.txt`, copied next to the exe as
  `LICENSE-xenia.txt`.

### The `xma2dec.c` credit (resolved iter-467: no code from it is here)

Every Xenia XMA context file, including the `xma_context_new.cc` we port, says "Credits for most
of this code goes to" koolkdev/libertyv `libav_wrapper/xma2dec.c`. That repository publishes no
licence, so the credit was listed as a release blocker. The history says the credit is a leftover:

- `xma2dec.c` (@629cfae5, 2013, 216 lines) is a libav wrapper: it rewrites each 2048-byte XMA
  packet header so FFmpeg's WMA Pro decoder accepts it (`(offset & 0x7800 | 0x400) >> 7`, mask
  `0xFFFEFF08`, WMA Pro extradata with decode flags `0x10D6`), calls `avcodec_decode_audio4`, and
  converts float to integer PCM.
- Xenia's first XMA decoder, `audio_decoder.cpp` (f31b34256b, Dr. Chat, 2015-05-22), adapted it
  and added the credit line (moved into `xma_context.cc` by 021b5a3d17, gibbed, 2015-06-21).
  "New WIP audio decoder" (0f9cd8cfb, 2015-08-22) dropped the packet-header rewrite for a
  frame-level decode in Xenia's own libav fork, keeping the WMA Pro extradata. Joel Linn's
  "Rewrite XMA packet parser" (6547fa174, 2020-06-24) removed the last of it (the `0x10D6`
  extradata and `xma2_decode_frame`) and introduced the FFmpeg `xmaframes` decoder. Gliniak's
  `xma_context_new.cc` (122f58c9d, 2024-03-25), the file we port, and the versioned contexts
  (4494e40c7, 2025-12-02) are later code again; each copied the credit line.
- Our port contains none of `xma2dec.c`. `tools/provenance_check.py` compares it with every
  tracked C/C++ file (names, numbers and member chains normalised, so a renamed copy still
  matches). The 2015 adaptation, used as the positive control, scores a longest shared run of
  76 tokens and a 20-token-window share of 0.205. The worst of our 451 files scores 28 and
  0.015, and none contains `WMAPRO`, `avcodec_decode_audio4`, `0xFFFEFF08` or `0x10D6`.

Rerun (the foreign file is fetched into `archive/`, never committed; the control comes from the
Xenia history in `reference/xenia-canary`):

```
curl -sSL -o archive/v1_xma/provenance/xma2dec_629cfae5.c https://raw.githubusercontent.com/koolkdev/libertyv/629cfae55394fb139526bf6fa9a3b3d743ae89ce/libav_wrapper/xma2dec.c
git -C reference/xenia-canary show f31b34256b:src/xenia/apu/audio_decoder.cpp > archive/v1_xma/provenance/control_xenia_f31b3425_audio_decoder.cpp
python tools/provenance_check.py --foreign archive/v1_xma/provenance/xma2dec_629cfae5.c --control archive/v1_xma/provenance/control_xenia_f31b3425_audio_decoder.cpp --all-tracked
```

`xma2dec.c` SHA-256 `9ef68696cf283f9f67c4eb8f83c80624358c60fef417ad302252e6b722ec618b`. The
headers of `kernel/apu/xma_context_new.*` and `xma_ffmpeg_decoder.cpp` keep the credit, marked
historical. CREDITS.md lists koolkdev as credited, with no code used.

## Other runtime dependencies (already documented where they are built)

| Component | Licence | Built / shipped by |
|---|---|---|
| SDL3 (`SDL3.dll`, 3.4.18) | zlib | vcpkg (`vcpkg.json` + the overlay port `third_party/vcpkg-overlay/sdl3`), copied next to the exe |
| XenosRecomp (runtime shader translation) | MIT (`LICENSE-XenosRecomp.md`) | `cmake/xenos_xlat.cmake` |
| DXC (`dxcompiler.dll`, `dxil.dll`) | LLVM / MS (`LICENSE-DXC*.txt`) | `cmake/xenos_xlat.cmake` |
| fmt (header-only in xenos_xlat) | MIT (`LICENSE-fmt.txt`) | `cmake/xenos_xlat.cmake` |
| xxHash (`gfx/thirdparty/xxhash.h` + the clone's copy in xenos_xlat) | BSD-2-Clause (text in `THIRD_PARTY_LICENSES.md`; NO `LICENSE-*` copy next to the exe yet) | compiled in |
| XenonRecomp `ppc_context.h` (generated guest code) | MIT (text in `THIRD_PARTY_LICENSES.md`) | compiled in |
| SIMDe (included by the generated guest code) | MIT (text in `THIRD_PARTY_LICENSES.md`) | compiled in |
| Xenia precompiled `resolve_*` / `texture_load_*` compute shaders | BSD-3 (`LICENSE-xenia.txt`, but that copy is made only when FFmpeg is built: see `THIRD_PARTY_LICENSES.md`) | `#include`d by `gfx/d3d12_pm4_shmem.cpp` from `reference/xenia-canary` |
