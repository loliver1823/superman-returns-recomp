# Third-party licences

These are the licence texts and notices that a **binary release** of this port must carry.
Ship this file next to `SupermanReturns.exe`, together with the `LICENSE-*` files the build
already copies there:
- `LICENSE-xenia.txt`
- `LICENSE-FFmpeg-LGPLv2.1.txt`
- `LICENSE-XenosRecomp.md`
- `LICENSE-fmt.txt`
- `LICENSE-DXC.txt`
- `LICENSE-DXC-MS.txt`
- `LICENSE-DXC-MIT.txt`

Who made what, and what we use from each, is in [`CREDITS.md`](CREDITS.md). The source of
truth is [`data/credits.json`](data/credits.json), and
[`scripts/check_credits.py`](scripts/check_credits.py) checks that every shipped component
below has a section here.

Contents: [the shipping checklist](#shipping-checklist), [the game](#the-game),
[our own code](#our-own-code), then one section per component.

## Shipping checklist

A release archive **must** contain:
- `LICENSE` (the MIT licence, this project's licence);
- this file and the seven `LICENSE-*` files listed above;
- the written source offer for FFmpeg (the [FFmpeg](#ffmpeg-xenias-xma-fork-lgpl-21-or-later)
  section below);
- the end-user terms that `dxil.dll`'s licence requires (see [DXC](#directx-shader-compiler)).

A release archive **must not** contain:
- anything from the game disc;
- `guest/ppc/`, which is the game's code translated to C++;
- `data/ui_ast_extracted`, `data/shaders_*` or `data/gpu_traces`;
- the built mod outputs in `mods/*/DATA/` (whole patched game archives).

Port-feature patches (mod recipes) must contain **zero game bytes**: only new code and
data, and operations on names.

## The game

<!-- credits:superman-returns -->
*Superman Returns* (Xbox 360, 2006) was developed by EA Tiburon and published by Electronic
Arts. It is based on the Warner Bros. Pictures film, and its copyright belongs to its rights
holders. Superman and all related characters and elements are
trademarks of DC Comics. All other trademarks belong to their respective owners. This project
is not affiliated with or endorsed by Electronic Arts, Warner Bros. or DC Comics.

**No licence to the game is granted or implied by anything in this repository.** Players must
own a legal copy of the Xbox 360 game. The port reads the disc data from the player's copy at
runtime and the build recompiles the player's own XEX. The recompiled code in the built exe is
a translation of EA's code, including the middleware EA licensed (for example Havok physics
and Lua). Distributing that exe is a separate legal question that this file does not answer.

<!-- credits:lua -->
### Lua 5.1 (part of the game's code)

The game embeds a Lua 5.1 VM. The year below is the one on the Lua 5.1 releases that existed
when the game shipped. The binary's own copyright string is stripped, so the exact point
release is not confirmed.

```
Copyright (C) 1994-2006 Lua.org, PUC-Rio.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## Our own code

The project's own source code is licensed under the **MIT licence** (SPDX `MIT`; owner decision
2026-11-16, replacing GPL-3.0-only; the text is [`LICENSE`](LICENSE)). It covers the code in this
repository that is not credited to someone else in [`CREDITS.md`](CREDITS.md); the components credited
there keep their own licences. It does not cover the game: the recompiled game code in a built exe is a
translation of Electronic Arts' code, and nothing here licenses it (see [the game](#the-game)).

### Licence of every component (audit 2026-10-09, updated 2026-11-16)

Everything compiled into the exe is under a permissive licence (MIT, BSD, zlib). The only copyleft
component, FFmpeg's XMA decoder, is a separate, replaceable DLL under the LGPL.

| Component | Licence | How it is used | Fine alongside this project's MIT licence |
|---|---|---|---|
| Xenia / Xenia Canary | BSD-3-Clause | ported code, embedded shaders | yes (notice ships) |
| NVIDIA FXAA 3.11 | BSD-3-Clause | compiled into embedded shaders | yes |
| AMD FidelityFX CAS, FSR 1 | MIT | compiled into embedded shaders | yes |
| libertyv `xma2dec.c` (credited by Xenia) | none published | none: a historical credit; no code of it is in the tree ([`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md)) | not applicable |
| FFmpeg (Xenia's fork) | LGPL-2.1-or-later | separate, replaceable DLL | yes (the DLL stays LGPL; a binary release carries its source offer) |
| SDL3 (with the Khronos headers, stb_image and HIDAPI it compiles in) | zlib (Apache-2.0, MIT / public domain, HIDAPI's own) | DLL | yes |
| XenonRecomp (`ppc_context.h`) | MIT | compiled in | yes |
| SIMDe | MIT | compiled in | yes |
| XenosRecomp | MIT | compiled in | yes |
| {fmt} | MIT | compiled in | yes |
| xxHash | BSD-2-Clause | compiled in | yes |
| Dear ImGui (and stb) | MIT (stb: MIT or public domain) | compiled in | yes |
| DXC `dxcompiler.dll` | NCSA (LLVM release licence) | DLL loaded at run time | yes |
| DXC `dxil.dll` | Microsoft licence terms (proprietary) | DLL loaded at run time | yes (the DLL keeps Microsoft's terms) |
| Julio Cacko input prompts | CC0 1.0 | image files shipped beside the exe | yes |
| Saira font | OFL-1.1 | font files shipped beside the exe | yes, as a separate work (the fonts stay OFL) |
| Lua 5.1 | MIT | part of the game's code | yes (the game code itself is not ours to license) |
| vcpkg, LLVM, CMake, Ninja, Python libraries, IDA, Ghidra, Frida, WinDbg | various | build and development tools, not shipped | not applicable |
| extract-xiso | BSD-4-Clause (advertising clause) | development tool | excluded from the public repository and from releases |
| xdvdfs | MIT | development tool | not applicable (not shipped) |
| Unleashed Recompiled, OpenSAGE | GPL-3.0 | read only; no code copied (a conventions reference, e.g. `portable.txt`) | yes, because no code is copied; none may be |
| ReXGlue SDK | BSD-3-Clause | read only | yes |
| N64: Recompiled | MIT | inspiration | not applicable |

Open items before anyone distributes a build:
- **`dxil.dll`.** The end-user terms that `dxil.dll`'s licence asks for (see
  [DXC](#directx-shader-compiler)) are still open.
- **The game.** A built exe carries the recompiled game code, as Unleashed Recompiled's releases
  do. That is a legal question this file does not answer. This repository itself carries none of it:
  the build generates it from the builder's own `default.xex`.

The libertyv `xma2dec.c` question listed here on 2026-10-09 is closed: Xenia's credit line is
historical, and no code from that file is in this tree ([`docs/THIRD_PARTY.md`](docs/THIRD_PARTY.md)).

## Xenia / Xenia Canary: BSD-3-Clause

<!-- credits:xenia -->
This covers the ported GPU, audio, XMA and kernel code, the `resolve_*`, `texture_load_*`, FXAA
(`fxaa_cs`, `fxaa_extreme_cs`) and guest-output shaders compiled into the exe (built from the
Xenia sources vendored in `third_party/xenia-shaders/`, each with its own header), and the
FFmpeg config overlay. It is shipped
as `LICENSE-xenia.txt`.

```
Copyright (c) 2015, Ben Vanik.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the project nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL BEN VANIK BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

<!-- credits:amd-fidelityfx -->
**AMD FidelityFX CAS and FSR 1** (compiled into Xenia's guest-output shaders, used by the present
chain's `upscale_filter` / `sharpness` stages; vendored in `third_party/xenia-shaders/third_party/`,
whose `FidelityFX-CAS/LICENSE.txt` and `FidelityFX-FSR/license.txt` hold this text):

```
Copyright (c) 2020 Advanced Micro Devices, Inc. All rights reserved. (FidelityFX CAS)
Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved. (FidelityFX FSR)
Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All rights reserved. (ffx_a.h, ffx_cas.h)
Copyright (c) <2014> <Michal Drobot> (ffx_a.h; "for concepts used in FLOAT APPROXIMATIONS")

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## NVIDIA FXAA 3.11: BSD-3-Clause

<!-- credits:nvidia-fxaa -->
FXAA 3.11 by Timothy Lottes, compiled into Xenia's FXAA shaders that the exe embeds (the present
chain's post stage). Shipped as `third_party/fxaa/LICENSE`:

```
BSD 3-Clause License

Copyright (c) 2014-2015, NVIDIA CORPORATION. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

  * Neither the name of NVIDIA CORPORATION nor the names of its
    contributors may be used to endorse or promote products derived
    from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## FFmpeg (Xenia's XMA fork): LGPL-2.1-or-later

<!-- credits:ffmpeg-xenia -->
`ffmpeg_xenia.dll` is libavcodec and libavutil from https://github.com/has207/FFmpeg (branch
`xmaframes`, commit `d980192e175e6ff95bcd287af77e16fcb6597974`), built LGPL-only by
`cmake/ffmpeg_xenia.cmake`. It is a separate, dynamically loaded DLL, so you may replace it
with your own build.

The full licence text is shipped as `LICENSE-FFmpeg-LGPLv2.1.txt` (FFmpeg's `COPYING.LGPLv2.1`).

**Source offer.** The corresponding source is:
- the fork at the commit above;
- `third_party/ffmpeg/xenia-config/`;
- `cmake/ffmpeg_xenia.cmake`.

All three are in this repository and its submodule `third_party/ffmpeg/FFmpeg`. A binary
release must ship that source, or a written offer to provide it, for at least three years.

FFmpeg is a trademark of Fabrice Bellard, originator of the FFmpeg project.

**Other notices inside `ffmpeg_xenia.dll`.** Besides its LGPL files, the DLL compiles a few
FFmpeg files under permissive licences. None is GPL (a header scan of every compiled file and
the headers it includes, iter-467). Their notices:

- `libavcodec/jfdctfst.c`, `jfdctint_template.c`, `jrevdct.c`: the Independent JPEG Group's
  licence. This software is based in part on the work of the Independent JPEG Group.
- `libavcodec/faandct.c` (ISC):

  ```
  Copyright (c) 2003 Michael Niedermayer <michaelni@gmx.at>
  Copyright (c) 2003 Roman Shaposhnik

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
  ```
- `libavutil/avsscanf.c` (MIT, from musl):

  ```
  Copyright (c) 2005-2014 Rich Felker, et al.

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:

  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
  ```
- `libavutil/adler32.c` (zlib licence): a modified version of `adler32.c` from the zlib
  library, Copyright (C) 1995 Mark Adler.

## SDL3: zlib

<!-- credits:sdl3 -->
`SDL3.dll` (3.4.18, built by vcpkg from the overlay port `third_party/vcpkg-overlay/sdl3`, source
<https://github.com/libsdl-org/SDL/archive/release-3.4.18.tar.gz>, SHA-512 `12fcb559...7741af50`).

```
Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

vcpkg's port notes that "some configurations may use code licensed under the MIT and Apache-2.0
licenses". In this Windows build `SDL3.dll` compiles in: the Khronos Vulkan headers (Apache-2.0,
Copyright 2015-2024 The Khronos Group Inc.) for SDL's Vulkan render / GPU backends; `stb_image`
(Sean Barrett, MIT or public domain, used under the public-domain alternative) for SDL's image
loading; and HIDAPI (Copyright 2009 Alan Ott, Signal 11 Software, used under its original licence:
"This software may be used by anyone for any reason so long as the copyright notice in the source
files remains intact"). SDL2, which this port used before, carried the same Khronos headers and
HIDAPI.

## XenonRecomp and XenosRecomp: MIT

<!-- credits:xenonrecomp -->
<!-- credits:xenosrecomp -->
XenonRecomp's runtime header `ppc_context.h` is compiled into the exe through the generated
guest code. XenosRecomp's translator is compiled into the exe as `xenos_xlat` and shipped as
`LICENSE-XenosRecomp.md`. Both projects use the same text:

```
# MIT License

Copyright (c) 2025 hedge-dev and contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

- The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

- THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## SIMDe: MIT

<!-- credits:simde -->
A header-only library that the generated guest code compiles in.

```
Copyright (c) 2017 Evan Nemerson <evan@nemerson.com>

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## {fmt}: MIT

<!-- credits:fmt -->
A header-only library compiled into `xenos_xlat`, shipped as `LICENSE-fmt.txt`.

```
Copyright (c) 2012 - present, Victor Zverovich and {fmt} contributors

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

--- Optional exception to the license ---

As an exception, if, as a result of your compiling your source code, portions
of this Software are embedded into a machine-executable object form of such
source code, you may redistribute such embedded portions in such object form
without including the above copyright and permission notices.
```

## xxHash: BSD-2-Clause

<!-- credits:xxhash -->
Compiled into the exe. The vendored copy is `gfx/thirdparty/xxhash.h`, and XenosRecomp's
pinned copy (`third_party/XenosRecomp/thirdparty/xxHash`) is compiled into `xenos_xlat`.

```
xxHash - Extremely Fast Hash algorithm
Copyright (C) 2012-2023 Yann Collet

BSD 2-Clause License (https://www.opensource.org/licenses/bsd-license.php)

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

   * Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
   * Redistributions in binary form must reproduce the above
     copyright notice, this list of conditions and the following disclaimer
     in the documentation and/or other materials provided with the
     distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## DirectX Shader Compiler

<!-- credits:dxc -->
- **`dxcompiler.dll` (v1.8.2407):** under the University of Illinois/NCSA Open Source Licence
  (the LLVM release licence), plus third-party notices. The full text is shipped as
  `LICENSE-DXC.txt` (source: `tools/xenosrecomp/LICENSE-DXC.txt`).
- **The release package's MIT notice** (Copyright (c) Microsoft Corporation), `LICENSE-MIT.txt` of
  the v1.8.2407 release zip the build takes both DLLs from, shipped as `LICENSE-DXC-MIT.txt`
  (source: `tools/xenosrecomp/LICENSE-DXC-MIT.txt`, the zip's file byte for byte).
- **`dxil.dll`:** under the Microsoft Software License Terms for the "Microsoft DirectX Shader
  Compiler", shipped as `LICENSE-DXC-MS.txt` (source: `tools/xenosrecomp/LICENSE-DXC-MS.txt`).
  These terms allow the object code to be distributed inside an application, on four
  conditions:
  - the application's end-user terms protect Microsoft at least as much as Microsoft's own
    terms do;
  - the distributor indemnifies Microsoft;
  - Microsoft's trademarks are not used;
  - the code is not combined in a way that puts it under a source-disclosure licence.

  **Open point:** the terms refer to a "distributables file list" that the release zip does
  not include. `dxil.dll` counting as distributable is therefore an inference, not something
  read from such a list. Settle this, and add the flow-down end-user terms, before any public
  build.

## Dear ImGui: MIT

<!-- credits:dear-imgui -->
Compiled into the exe (the mod library). The vendored copy is `third_party/imgui/` (v1.91.9b),
whose `imstb_*` headers carry Sean Barrett's public-domain / MIT notices in their own text.

```
The MIT License (MIT)

Copyright (c) 2014-2025 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Input prompt glyphs: CC0 1.0 / MIT

<!-- credits:julio-cacko-input-prompts -->
These are the button glyphs from the **FREE Input Prompts Pack** by **Julio Cacko**
(https://juliocacko.itch.io/free-input-prompts), under `data/glyphs/`. The store page, read
2026-10-02, licenses the assets under **CC0 1.0 Universal**
(https://creativecommons.org/publicdomain/zero/1.0/) and the code under **MIT**, and says:
*"Free to use in any project, commercial or non-commercial. No attribution required (but
appreciated!)"*. No licence text has to ship; we credit the author anyway, and ship the CC0 1.0
legal code next to the art (`data/glyphs/juliocacko_input_prompts/LICENSE.txt`, with `SOURCE.md`).

CC0 waives the author's copyright only. The PlayStation and Xbox controller symbols that some
sheets depict remain trademarks of Sony Interactive Entertainment and Microsoft, and the Switch labels of
Nintendo.

## Saira font: SIL Open Font License 1.1

<!-- credits:saira-font -->
The mod library's typeface, shipped as `data/fonts/saira/*.ttf` (modified: static instances,
subset, renamed "Saira SR") with the licence text `data/fonts/saira/OFL.txt`:

```
Copyright 2020 The Saira Project Authors (https://github.com/Omnibus-Type/Saira)


This Font Software is licensed under the SIL Open Font License, Version 1.1.
This license is copied below, and is also available with a FAQ at:
https://scripts.sil.org/OFL


-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded, 
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```
