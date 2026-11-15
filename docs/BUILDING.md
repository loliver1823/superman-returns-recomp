# Building

> Drafted with AI assistance (Claude) for the project owner.

The only supported configuration is Windows x64, Ninja, Clang, Release (`CMakePresets.json`).
Building needs your own copy of the game: the recompiled code is generated from your
`default.xex` on your machine, and the repository never contains it.

## 1. Clone the repository

```
git clone --recurse-submodules https://github.com/loliver1823/superman-returns-recomp.git
```

If you cloned without `--recurse-submodules`, run `git submodule update --init --recursive`.
The submodules are vcpkg, XenonRecomp, XenosRecomp and Xenia's FFmpeg fork, and the recompilers'
own third-party submodules.

## 2. Add the game files

Extract your USA disc (see [`DUMPING.md`](DUMPING.md)) to `Superman Returns (USA)/` in the
repository root. It must contain `default.xex` and `DATA/`. The build reads `default.xex`; the game
reads `DATA/` at run time. Nothing in that folder is ever written or committed (it is in
`.gitignore`). To keep `default.xex` elsewhere, add `-DSR_XEX_PATH=<path to default.xex>` to the
configure command in step 4.

## 3. Install the tools

- Visual Studio 2022 or its Build Tools, with **Desktop development with C++** (the MSVC runtime
  libraries and the Windows SDK, whose `fxc.exe` compiles Xenia's shaders).
- [LLVM](https://github.com/llvm/llvm-project/releases) installed at `C:\Program Files\LLVM`
  (the presets name `clang.exe` and `clang++.exe` there; the recompiler build uses `clang-cl.exe`).
- CMake 3.21 or newer, and Ninja, on `PATH`.
- Python 3.11 or newer on `PATH` as `python`, and Git.

No developer command prompt is needed, and LLVM does not have to be on `PATH`: the build names
clang by its full path, and clang finds Visual Studio and the Windows SDK by itself.
The first configure needs network access: vcpkg builds SDL3 from source, and the build downloads
the DirectX Shader Compiler release it ships (step 4).

The build was checked on a clean Windows Server 2025 machine with LLVM 22.1.6, CMake 4.2.1,
Ninja 1.13.2, Python 3.11.9, Git 2.56 and the Visual Studio 2022 Build Tools (MSVC 14.44, Windows
SDK 10.0.26100), last on 2026-11-16 from a fresh clone of this public repository, in a shell with no
LLVM, sccache or developer prompt on `PATH`.

## 4. Configure

From the repository root:

```
cmake --preset default
```

The first configure does everything that turns your disc and the pinned sources into a buildable
tree. Nothing has to be prepared by hand:

- **SDL3:** vcpkg (the `third_party/vcpkg` submodule, manifest `vcpkg.json`) builds it. The version
  is pinned by an overlay port, `third_party/vcpkg-overlay/sdl3` (selected by
  `vcpkg-configuration.json`): SDL **3.4.18**, from
  `https://github.com/libsdl-org/SDL/archive/release-3.4.18.tar.gz`, SHA-512
  `12fcb5592016b27922b08d1580d75f5f15c72e67cf04415e2bd824dc7563ac940c25bbe368b03d0a755f5832803049467e3d0ee6784ed3684792bc4d7741af50`
  (the submodule's own port is 3.4.0). The overlay is vcpkg's sdl3 portfile with that version and
  hash; to move to another SDL release, change both, then re-pin `SDL3.dll` in
  `docs/release_composition.json`. CMake finds it with `find_package(SDL3 3.4.18 CONFIG)`; there is no
  SDLmain library (`app/main.cpp` includes SDL3's header-only `SDL_main.h`).
- **Guest code:** `tools/regen/regen.py` builds the pinned XenonRecomp (the `tools/XenonRecomp`
  submodule plus `tools/regen/patches/`, in `build/regen/`), recompiles your `default.xex` into
  `guest/ppc/`, and runs the deterministic post-processing (`tools/regen/README.md`). The result
  is stamped with the hash of every input, so a later configure checks it in about a second and
  regenerates only when an input changes.
- **Shader translator:** `tools/xenosrecomp/prepare.py` applies `tools/xenosrecomp/*.patch` to the
  commit the `third_party/XenosRecomp` submodule pins, in `build/xenosrecomp/`
  (`tools/xenosrecomp/README.md`); the game compiles it in for shaders the cache lacks.
- **DirectX Shader Compiler:** `cmake/dxc_release.cmake` downloads the official v1.8.2407 release
  and checks its SHA-256; its `dxcompiler.dll` and `dxil.dll` ship next to the exe. Without network
  access, unpack `dxc_2024_07_31.zip` yourself and add `-DSR_DXC_DIR=<that folder>`.

On a 12-thread machine the first configure takes about 3 minutes.

## 5. Build

```
cmake --build build
```

This also compiles Xenia's resolve, texture-load and presentation shaders from
`third_party/xenia-shaders/` with the Windows SDK's `fxc.exe` (`cmake/xenia_shaders.cmake`). A first
build takes about 6 minutes on 12 threads, most of it the recompiled game code.

The output is `build/SupermanReturns.exe` with its DLLs (`SDL3.dll`, `ffmpeg_xenia.dll`, DXC) and the
licence files that must ship with it, plus the test programs (`build/difftest.exe` and others).

Then, once:

```
powershell -File tools\build_point_gs.ps1
```

It compiles the port's own point-sprite geometry shader (`tools/xenosrecomp/point_sprite.gs.hlsl`) with
the DXC in the XenosRecomp submodule, to `data\shader_cache\point_sprite.gs.dxil`, where the game looks for
it. Without it the game cannot translate point-list draws: the default build drops them, and the release
build stops at the first one. (The build does not make this file yet.)

### The release configuration (optional)

```
cmake --preset release
cmake --build build-release
```

This builds the same game without the emulation layer that the default (development) build keeps as its
test oracle (`SR_RELEASE_COMPOSITION=ON`; the README's "Native status"). It builds from a fresh clone, but
it was only ever run with the private shader cache, which this repository cannot carry; with no cache
every shader is translated at run time, and that was not tested in the release build. Play with the
default build.

## 6. Run

Run from the repository root, so that `Superman Returns (USA)/` is found next to the working
directory:

```
build\SupermanReturns.exe
```

The first start translates shaders at run time, because the repository carries no shader cache
(a cache made from the game's shaders cannot be published). Expect short hitches the first time a
shader appears.

## 7. Checks

```
python scripts/check_credits.py      # every dependency credited, licences in place
python scripts/audit_forcing.py      # no ungated forcing (docs/NATURAL_PATH_CHARTER.md)
build\difftest.exe --pc-actions-test # one of the C++ unit and differential tests (--<name>-test)
python scripts/difftest/compare.py --all --difftest build\difftest.exe   # recompiled code vs a PowerPC emulator
```

Some tests need inputs that are not in the repository because they come from the game. For example
`tools/difftest/shader_xlat_test_run.py` compares the runtime shader translator with an offline
translation of the game's shaders, so from a fresh clone its parity and corpus checks fail; the tests
above need nothing but the build.

[`VERIFICATION.md`](VERIFICATION.md) describes the oracle checks, most of which need captures you
make from your own copy of the game.

## How changes are made

Generated code (`guest/ppc/`) is never edited by hand. To change or observe a game function, define
it in host code and call the recompiled original:

```cpp
PPC_FUNC(sub_82721E68) {
  __imp__sub_82721E68(ctx, base);   // the game's own body, unchanged
  // host work after it
}
```

Register every such override in `docs/OVERRIDES.json` (`python scripts/gen_override_manifest.py`);
`scripts/audit_forcing.py` fails on an unregistered one. New behaviour goes behind an `SR_*`
environment switch first. See [`../CONTRIBUTING.md`](../CONTRIBUTING.md).
