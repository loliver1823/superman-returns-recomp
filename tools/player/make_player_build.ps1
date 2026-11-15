# iter-465 player lane: stage a PLAYER INSTALL of the current build - "press the exe and the
# game opens" (docs/PLAYER_BUILD.md). Reproducible: everything comes from this worktree's build
# folder and git-tracked data, and a hash manifest records every staged file.
#
#   powershell -File tools/player/make_player_build.ps1 -Label <name>
#       [-Out <main checkout>\archive\player] [-Disc "<...\Superman Returns (USA)>"]
#       [-NoBuild] [-UseDevBuild]
#
# iter-467 guestcode: the package comes from the "player" CMake preset (build-player/: the default
# config + SR_THINLTO=ON, docs/PERFORMANCE.md 2.18), configured on first use (toolchain file, ninja,
# vcpkg_installed and the patched translator dir are taken from build/CMakeCache.txt) and built
# through tools/build_slot.py before staging.
# -NoBuild stages build-player/ as it is; -UseDevBuild stages the dev build/ (no ThinLTO) instead.
# -Release (2026-10-17, ledger release.composition's release_target): the package comes from the "release" preset
# instead (build-release/: the default config + SR_RELEASE_COMPOSITION=ON, SR_ORACLE=0 - what ships, C1), configured
# and built the same way. With -BuildDir it stages that tree as it is and REFUSES one not configured with
# SR_RELEASE_COMPOSITION=ON. Every manifest records the configuration it staged (source.release_composition).
#
# Stage only (C1 composition audit, release step 2; tools/cloud/sr_cloud.py audit):
#   powershell -File tools/player/make_player_build.ps1 -Label audit -BuildDir <build dir> -Dest <new folder> -NoDisc
# -BuildDir stages that build tree as it is (no build; any configuration, e.g. a release build-rel); -Dest is the
# exact output folder (it must not exist) instead of <Out>\<Label>; -NoDisc leaves out the disc junction and the disc
# hash (the retail disc is the player's own, never package content). The staging below is the one code path a player
# build runs, so the audited package is what a player gets. manifest.json names this script as the packager
# (tools/player/composition_audit.py reports a package without it INCOMPLETE: a substitute).
#
# Layout of <Out>\<Label>\ :
#   SupermanReturns.exe   the build's exe with its PE subsystem set to WINDOWS (a double-click
#                         opens no console window; the build's own exe stays a console program for
#                         the harness tools). manifest.json records both hashes; the code bytes
#                         are identical (only the 2-byte Subsystem field differs).
#   *.dll, *.pdb, licences  DXC + SDL3 + the XMA decoder; the pdb makes crash dumps readable.
#   port_features\        the built-in recipes (custom front end, autosave, ...): port features.
#   data\glyphs\          the button-prompt glyphs (CC0, CREDITS.md).
#   data\fonts\           the mod library's font (Saira, OFL; CREDITS.md).
#   data\shader_cache\dxil  the installed AOT shader cache (git-tracked, incl. the 8 resolution
#                         scaled pixel shaders); gfx/shader_cache.cpp finds it next to the exe.
#   data\shader_cache\nb  strand B's compute kernels as bytecode (SR_NB_AOT, git-tracked).
#   data\shader_cache\pso_manifest.bin  the shipped pipeline manifest (step 4 2k, gfx/pso_warm.cpp; git-tracked):
#                         the translated pipelines a first run warms before its draws need them.
#   mods\                empty: the player's own mods go here (MODS menu: OPEN MODS FOLDER).
#   Superman Returns (USA)  a directory junction to the retail disc data (never copied, never
#                         written: the game reads it in place).
#   PLAYER_README.txt     what is on, where the settings live.
#   manifest.json         sha256 of every staged file + build provenance.
# Saves and settings live in %LOCALAPPDATA%\SupermanRecomp\content (no portable.txt is staged).
param(
  [Parameter(Mandatory = $true)] [string] $Label,
  [string] $Out = "",       # default <main checkout>\archive\player
  [string] $Disc = "",      # default <main checkout>\Superman Returns (USA)
  [switch] $NoBuild,
  [switch] $UseDevBuild,
  [string] $BuildDir,
  [string] $Dest,
  [switch] $NoDisc,
  [switch] $Release
)
$ErrorActionPreference = "Stop"
$main = if ($env:SR_MAIN_ROOT) { $env:SR_MAIN_ROOT } else { Split-Path -Parent (git -C $PSScriptRoot rev-parse --path-format=absolute --git-common-dir) }
if (-not $Out) { $Out = Join-Path $main "archive\player" }
if (-not $Disc) { $Disc = Join-Path $main "Superman Returns (USA)" }
$root = (Join-Path $PSScriptRoot "..\.." | Resolve-Path).Path
if ($BuildDir) {
  $build = (Resolve-Path -LiteralPath $BuildDir).Path
  $NoBuild = $true
} elseif ($UseDevBuild) {
  $build = Join-Path $root "build"
} else {
  $preset = $(if ($Release) { "release" } else { "player" })
  $buildName = "build-$preset"
  $build = Join-Path $root $buildName
  if (-not $NoBuild) {
    if (-not (Test-Path -LiteralPath (Join-Path $build "CMakeCache.txt"))) {
      $cfg = @("--preset", $preset)
      # A lane worktree's dev build points at the main checkout's vcpkg, a winget ninja and the
      # patched translator clone: the player build takes the same values from the dev cache.
      $devCache = Join-Path $root "build\CMakeCache.txt"
      if (Test-Path -LiteralPath $devCache) {
        foreach ($v in @("CMAKE_TOOLCHAIN_FILE", "CMAKE_MAKE_PROGRAM", "VCPKG_INSTALLED_DIR", "SR_XENOSRECOMP_DIR")) {
          $m = Select-String -LiteralPath $devCache -Pattern ('^' + $v + ':[A-Z]+=(.*)$') | Select-Object -First 1
          if ($m) { $cfg += "-D$v=$($m.Matches[0].Groups[1].Value)" }
        }
      }
      Push-Location $root
      try { & cmake @cfg | Out-Host; if ($LASTEXITCODE) { throw "make_player_build: cmake --preset $preset failed" } }
      finally { Pop-Location }
    }
    Push-Location $root
    try {
      & python tools/build_slot.py --owner $preset -- cmake --build $buildName -j 8 | Out-Host
      if ($LASTEXITCODE) { throw "make_player_build: the $buildName build failed" }
    } finally { Pop-Location }
  }
}
# step 4 (lane D, 2026-10-20): a failed build leaves the previous exe in the tree - never stage it. tools/cloud/
# sr_cloud.py build records each full build's outcome in sr_build_status.json ("full_exit": -1 while it runs or when
# its configure failed); a tree without the file (a local build) is staged as before.
$buildStatus = Join-Path $build "sr_build_status.json"
if (Test-Path -LiteralPath $buildStatus) {
  $bs = Get-Content -LiteralPath $buildStatus -Raw | ConvertFrom-Json
  if ($bs.full_exit -ne 0) {
    throw "make_player_build: the last full build of $build did not succeed (sr_build_status.json full_exit=$($bs.full_exit)) - its SupermanReturns.exe may be the previous build's; build it first"
  }
}
$thinlto = ((Select-String -LiteralPath (Join-Path $build "CMakeCache.txt") -Pattern '^SR_THINLTO:BOOL=(.*)$' | Select-Object -First 1).Matches[0].Groups[1].Value)
# step4/relcrash: an AddressSanitizer build (CMakeLists.txt option ASAN) is a diagnostic, never a package.
$asanMatch = Select-String -LiteralPath (Join-Path $build "CMakeCache.txt") -Pattern '^ASAN:BOOL=(ON|TRUE|1|YES)$' | Select-Object -First 1
if ($asanMatch -or (Test-Path -LiteralPath (Join-Path $build "clang_rt.asan_dynamic-x86_64.dll"))) {
  throw "make_player_build: $build is an AddressSanitizer build (ASAN=ON): a diagnostic, never packaged"
}
# The C1 release configuration (CMakeLists.txt option SR_RELEASE_COMPOSITION, default OFF): recorded in the manifest;
# -Release refuses to stage anything else.
$rcMatch = Select-String -LiteralPath (Join-Path $build "CMakeCache.txt") -Pattern '^SR_RELEASE_COMPOSITION:BOOL=(.*)$' | Select-Object -First 1
$releaseComposition = $(if ($rcMatch -and $rcMatch.Matches[0].Groups[1].Value -match '^(ON|TRUE|1|YES)$') { "ON" } else { "OFF" })
if ($Release -and $releaseComposition -ne "ON") { throw "make_player_build: -Release needs a build configured with SR_RELEASE_COMPOSITION=ON ($build is $releaseComposition)" }
# vcpkg's applocal step (which copies SDL3.dll next to the exe) needs dumpbin / llvm-objdump on PATH
# and silently skips without them; a fresh build-player can lack SDL3.dll. Take it from vcpkg.
# (RELEASE_PLAN step 3: SDL3 replaced SDL2; a build tree with SDL2.dll and no SDL3.dll is pre-SDL3.)
if ((Test-Path -LiteralPath (Join-Path $build "SDL2.dll")) -and -not (Test-Path -LiteralPath (Join-Path $build "SDL3.dll"))) {
  throw "make_player_build: $build is a pre-SDL3 build (SDL2.dll, no SDL3.dll); rebuild it"
}
if (-not (Test-Path -LiteralPath (Join-Path $build "SDL3.dll"))) {
  $vi = Select-String -LiteralPath (Join-Path $build "CMakeCache.txt") -Pattern '^VCPKG_INSTALLED_DIR:[A-Z]+=(.*)$' | Select-Object -First 1
  if ($vi) {
    $sdl = Join-Path $vi.Matches[0].Groups[1].Value "x64-windows\bin\SDL3.dll"
    if (Test-Path -LiteralPath $sdl) { Copy-Item -LiteralPath $sdl -Destination (Join-Path $build "SDL3.dll") }
  }
}
$dest = if ($Dest) { $Dest } else { Join-Path $Out ($Label -replace '[^A-Za-z0-9_.-]', '_') }
# Absolute (the manifest's relative paths are cut from FullName; .NET writes resolve against the process directory).
$dest = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($dest)) { $dest } else { Join-Path (Get-Location).Path $dest }))
if (Test-Path -LiteralPath $dest) { throw "make_player_build: $dest exists (labels are immutable; pick a new one)" }
if (-not $NoDisc -and -not (Test-Path -LiteralPath (Join-Path $Disc "default.xex"))) { throw "make_player_build: no default.xex in the disc folder $Disc" }
$exe = Join-Path $build "SupermanReturns.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "make_player_build: build first ($exe missing)" }

# The translator the exe embeds must be the resolution-patched one (docs/PLAYER_BUILD.md).
$embed = Join-Path $build "generated\xenos_embed.cpp"
$translatorId = ""
if (Test-Path -LiteralPath $embed) {
  $m = Select-String -LiteralPath $embed -Pattern 'kTranslatorSourceId\[\] = "([0-9a-f]+)"' | Select-Object -First 1
  if ($m) { $translatorId = $m.Matches[0].Groups[1].Value }
}
# The sources the translator was built from: SR_XENOSRECOMP_EFFECTIVE_DIR (cmake/xenos_xlat.cmake: the
# prepared submodule export or the SR_XENOSRECOMP_DIR clone); older build trees only have SR_XENOSRECOMP_DIR.
$xenosDir = ""
foreach ($pat in @('^SR_XENOSRECOMP_EFFECTIVE_DIR:INTERNAL=(.+)$', '^SR_XENOSRECOMP_DIR:PATH=(.+)$')) {
  $m = Select-String -LiteralPath (Join-Path $build "CMakeCache.txt") -Pattern $pat | Select-Object -First 1
  if ($m) { $xenosDir = $m.Matches[0].Groups[1].Value; break }
}
$common = Join-Path $xenosDir "XenosRecomp\shader_common.h"
if (-not (Test-Path -LiteralPath $common) -or -not (Select-String -LiteralPath $common -Pattern 'g_PixelPosScaleBias' -Quiet)) {
  throw "make_player_build: the translator clone the build embeds ($xenosDir) lacks the resolution patch (g_PixelPosScaleBias): configure with -DSR_XENOSRECOMP_DIR=<a clone with tools/xenosrecomp patch 0003> (python tools/xenosrecomp/verify_clone.py)"
}

# iter-467 clean-build: every licence notice the build ships next to the exe goes into the package. The list
# is THIRD_PARTY_LICENSES.md's own (the LICENSE-* bullets above its first section; scripts/check_credits.py
# fails when a LICENSE-* file the CMake build copies is not named there). A listed notice missing from the
# build, or a LICENSE-* file in the build that the list does not name, stops the staging: a package without
# them may not be distributed.
$licences = @()
foreach ($line in [IO.File]::ReadAllLines((Join-Path $root "THIRD_PARTY_LICENSES.md"))) {
  if ($line -match '^## ') { break }
  if ($line -match '^- `(LICENSE-[A-Za-z0-9_.+-]+\.(txt|md))`') { $licences += $Matches[1] }
}
if ($licences.Count -eq 0) { throw "make_player_build: THIRD_PARTY_LICENSES.md lists no LICENSE-* files" }
$unlisted = @(Get-ChildItem -LiteralPath $build -Filter "LICENSE-*" -File | ForEach-Object { $_.Name } |
              Where-Object { $licences -notcontains $_ })
if ($unlisted.Count) { throw "make_player_build: the build ships $($unlisted -join ', '), which THIRD_PARTY_LICENSES.md does not list" }
$missing = @($licences | Where-Object { -not (Test-Path -LiteralPath (Join-Path $build $_)) })
if ($missing.Count) { throw "make_player_build: licence notice(s) missing from $build : $($missing -join ', ')" }

New-Item -ItemType Directory -Path $dest | Out-Null
$files = @("SupermanReturns.pdb", "SDL3.dll", "dxcompiler.dll", "dxil.dll", "ffmpeg_xenia.dll",
           "THIRD_PARTY_LICENSES.md", "CREDITS.md") + $licences
foreach ($f in $files) {
  $src = Join-Path $build $f
  if (-not (Test-Path -LiteralPath $src)) { throw "make_player_build: missing $src" }
  Copy-Item -LiteralPath $src -Destination (Join-Path $dest $f)
}
Write-Host "make_player_build: staged $($licences.Count) licence notices: $($licences -join ', ')"
# The exe, with the PE optional header's Subsystem set to IMAGE_SUBSYSTEM_WINDOWS_GUI (2).
$bytes = [IO.File]::ReadAllBytes($exe)
$pe = [BitConverter]::ToInt32($bytes, 0x3C)
if ([BitConverter]::ToUInt32($bytes, $pe) -ne 0x00004550) { throw "make_player_build: $exe is not a PE file" }
$subOff = $pe + 24 + 68
$buildSubsystem = [BitConverter]::ToUInt16($bytes, $subOff)
$bytes[$subOff] = 2; $bytes[$subOff + 1] = 0
[IO.File]::WriteAllBytes((Join-Path $dest "SupermanReturns.exe"), $bytes)
$bytes = $null

Copy-Item -LiteralPath (Join-Path $build "port_features") -Destination (Join-Path $dest "port_features") -Recurse
New-Item -ItemType Directory -Path (Join-Path $dest "data") | Out-Null
Copy-Item -LiteralPath (Join-Path $build "data\glyphs") -Destination (Join-Path $dest "data\glyphs") -Recurse
# The mod library's OFL font (gfx/mod_library_ui.cpp looks in <exe>/data/fonts/saira; without it the
# library falls back to a default font and truncated names end in '?' - smoke test 2026-10-04).
Copy-Item -LiteralPath (Join-Path $root "data\fonts") -Destination (Join-Path $dest "data\fonts") -Recurse
New-Item -ItemType Directory -Path (Join-Path $dest "data\shader_cache") | Out-Null
Copy-Item -LiteralPath (Join-Path $root "data\shader_cache\dxil") -Destination (Join-Path $dest "data\shader_cache\dxil") -Recurse
# render/shader-coverage: the point-list sprite-expansion geometry shader (tools/build_point_gs.ps1; gfx/d3d12_backend.cpp
# pointSpriteGs() reads it beside the dxil folder). Without it every point-list draw is untranslatable: dev drops it,
# the release stops at the first one (missing-pso, lane F's lF_R4b 2026-10-17).
$pointGs = Join-Path $root "data\shader_cache\point_sprite.gs.dxil"
if (-not (Test-Path -LiteralPath $pointGs)) { throw "missing $pointGs (tools/build_point_gs.ps1)" }
Copy-Item -LiteralPath $pointGs -Destination (Join-Path $dest "data\shader_cache\point_sprite.gs.dxil")
# step 4 2k (lane D): the SHIPPED pipeline manifest (gfx/pso_warm.cpp) - recorded on the release build itself, what a
# player's first run (a cold driver cache) warms its pipelines from; read-only (the player's own recording goes to
# %LOCALAPPDATA%, never into the install).
$psoManifest = Join-Path $root "data\shader_cache\pso_manifest.bin"
if (Test-Path -LiteralPath $psoManifest) {
  Copy-Item -LiteralPath $psoManifest -Destination (Join-Path $dest "data\shader_cache\pso_manifest.bin")
} else {
  Write-Host "make_player_build: no data\shader_cache\pso_manifest.bin - the package ships no pipeline manifest"
}
# render/shader-coverage: the template corpus the runtime translator indexes at its first miss (the disc's compiled
# containers, tools/scan_disc_shaders.py): a bind-time vertex-shader variant the AOT cache lacks still translates.
if (Test-Path -LiteralPath (Join-Path $root "data\shader_cache\containers")) {
  Copy-Item -LiteralPath (Join-Path $root "data\shader_cache\containers") -Destination (Join-Path $dest "data\shader_cache\containers") -Recurse
}
# iter-467 native-b (SR_NB_AOT): strand B's compute kernels (native resolves, native targets, the direct texture
# loader) as shipped bytecode - gfx/native_resolve.inc nbAotDir() finds them next to the exe; without them the
# first ~25 s of a run resolve through the EDRAM model while the kernels compile.
if (Test-Path -LiteralPath (Join-Path $root "data\shader_cache\nb")) {
  Copy-Item -LiteralPath (Join-Path $root "data\shader_cache\nb") -Destination (Join-Path $dest "data\shader_cache\nb") -Recurse
}
New-Item -ItemType Directory -Path (Join-Path $dest "mods") | Out-Null
if (-not $NoDisc) { New-Item -ItemType Junction -Path (Join-Path $dest "Superman Returns (USA)") -Target $Disc | Out-Null }

$readme = @"
Superman Returns - PC port (player build '$Label', built $(Get-Date -Format 'yyyy-MM-dd HH:mm'))

Start: double-click SupermanReturns.exe. The game opens full screen (borderless, your desktop's
resolution); Alt+Enter switches to a window. Nothing else is needed.

On by default (port features): the built-in front end (NEW GAME / LOAD GAME / SETTINGS / MODS /
EXIT), autosave without asking, the MODS menu (mods go in the 'mods' folder here), PC controls
(WASD + mouse-look, Space take-off/land, Shift sprint, Esc pause), the menu mouse cursor, SDL
controllers with matching button prompts, 120 fps target, internal resolution 'auto' (4K -> 3x,
1440p/1080p -> 2x), the movie memory cache and the audio fix.

Settings files (created on first start) in %LOCALAPPDATA%\SupermanRecomp\content\settings\ :
  display.ini   res_scale = auto | 1..7     display_mode = borderless | windowed | exclusive
                window_width / window_height  target_fps = 120 | 60 | monitor | uncapped | off
  input.ini     key bindings, mouse look / sensitivity, glyphs = auto | keyboard | xbox | ps4 ...
  mod_options.ini  autosave_without_asking = 1 | 0
Saves are in %LOCALAPPDATA%\SupermanRecomp\content. The log is superman_returns.log next to the exe.
Known issues: docs/PLAYER_BUILD.md in the source tree.
"@
[IO.File]::WriteAllText((Join-Path $dest "PLAYER_README.txt"), $readme)

$hashes = [ordered]@{}
foreach ($f in (Get-ChildItem -LiteralPath $dest -Recurse -File | Where-Object { $_.FullName -notlike "*\Superman Returns (USA)\*" } | Sort-Object FullName)) {
  $rel = $f.FullName.Substring($dest.Length + 1).Replace('\', '/')
  $hashes[$rel] = (Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash.ToLower()
}
$scaled = @('264a28262b73c6c3', '3a6876055fec1674', '3b15e845131cc1d9', '5703f997dd6479c0',
            '9b16c6a8418ae1ab', '9bafe30bdf04f4c5', 'cb3736b2e52b6285', 'dbaef5bdc94ab47b')
$manifest = [ordered]@{
  label = $Label
  created = (Get-Date -Format o)
  packager = [ordered]@{ script = "tools/player/make_player_build.ps1"
                         sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $PSCommandPath).Hash.ToLower()
                         mode = $(if ($BuildDir) { "stage" } else { "player" }) }
  source = [ordered]@{
    worktree = $root
    head = (git -C $root rev-parse HEAD).Trim()
    dirty = @(git -C $root status --porcelain --untracked-files=no -- app kernel gfx guest tools CMakeLists.txt cmake)
    build_exe_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash.ToLower()
    build_exe_subsystem = $buildSubsystem
    staged_exe_subsystem = 2
    translator_source_id = $translatorId
    xenosrecomp_dir = $xenosDir
    build_dir = $build
    thinlto = $thinlto
    release_composition = $releaseComposition
  }
  disc = $(if ($NoDisc) { $null } else {
    [ordered]@{ junction = "Superman Returns (USA)"; target = $Disc
                default_xex_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Disc "default.xex")).Hash.ToLower() } })
  resolution_scaled_pixel_shaders = @($scaled | ForEach-Object { [ordered]@{ id = $_; sha256 = $hashes["data/shader_cache/dxil/$_.ps.dxil"] } })
  files = $hashes
}
[IO.File]::WriteAllText((Join-Path $dest "manifest.json"), ($manifest | ConvertTo-Json -Depth 6))
Write-Output (Join-Path $dest "SupermanReturns.exe")
