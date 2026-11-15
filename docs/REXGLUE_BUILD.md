# Building ReXGlue (`rexglue.exe`) — the second-opinion PPC codegen

Written iter-441. This file is referenced by `CMakeLists.txt` and
`tools/run-rexglue-codegen.ps1` (including a user-facing `Write-Error`), but had
**never existed** — anyone hitting the "rexglue not found" error was sent to a
missing page. This is the page.

## What ReXGlue is, and why we keep it

ReXGlue is an actively-maintained AOT PowerPC→C codegen, a sibling of XenonRecomp
(which produces our `guest/ppc/`). We do **not** build the game with it. We keep it
as a **cross-check oracle**: when a specific function looks miscompiled, regenerate
just that function with ReXGlue and diff it against ours.

> **Consult, don't switch.** See `docs/OTHER_RECOMPS_LESSONS.md` §6. This is the
> "second opinion" named in CLAUDE.md's *codegen is a peer suspect* hard rule — the
> first opinion is the L6 difftest (recomp vs PPC emulator, `docs/TOOLKIT.md` §1.3).

## Getting `rexglue.exe`

**The prebuilt `rexglue-sdk-win-amd64.zip` ships libraries only — it does NOT contain
`rexglue.exe`.** That is the single most common failure here. You must build the
tool from source:

```powershell
git clone --recurse-submodules https://github.com/Sajidur78/rexglue   # upstream (404 on 2026-10-02, see below)
cd rexglue
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
# resulting binary: build\rexglue.exe (path varies by upstream layout)
```

If upstream has moved or the layout differs, the only hard requirement is a
`rexglue.exe` this repo's scripts can locate.

> **2026-10-02 credits audit:** `github.com/Sajidur78/rexglue` returns 404. The project is
> published as <https://github.com/rexglue/rexglue-sdk> under **BSD-3-Clause** (copyright Tom
> Clay; portions derived from Xenia). The "unclear licence" noted elsewhere predates that
> LICENSE file. Whether that repository also holds the codegen tool's source is not verified
> here. See `CREDITS.md` (rexglue).

## How this repo finds it

`tools/run-rexglue-codegen.ps1` resolves, in order:

1. `$env:REXGLUE_EXE` — full path to `rexglue.exe` (most explicit; prefer this)
2. `$env:REXSDK\bin\rexglue.exe` — an SDK install dir containing `bin\`
3. `rexglue` on `PATH`

```powershell
$env:REXGLUE_EXE = "C:\src\rexglue\build\rexglue.exe"
pwsh tools/run-rexglue-codegen.ps1
```

The generated output lands in `guest/ppc_rexglue/` (gitignored, like `guest/ppc/`).

## Building the guest library with it (rarely wanted)

```powershell
cmake -B build -DBUILD_GUEST_REXGLUE=ON
cmake --build build
```

This is an experiment path, not the shipping configuration. The supported build is
**Ninja + clang + x64 Release** over `guest/ppc/` (XenonRecomp output) — see
`CLAUDE.md` → Build & repo.

## Using it as the cross-check (the actual workflow)

1. L6 difftest flags a function, or an oracle disagrees with our run and attribution
   points at a specific `sub_XXXXXXXX`.
2. Regenerate that function via ReXGlue.
3. Diff against `guest/ppc/`'s version — look for dropped instructions (the
   recompiler has silently dropped whole classes: iter-434e found 59 VMX
   instructions emitted as comments), wrong flag semantics, or a mangled switch.
4. Only after that differential may a "miscompiled" claim be made.
