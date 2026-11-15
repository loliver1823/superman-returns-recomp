# snapshot_exe.ps1 - copy the current build into archive\exe_snapshots\<id>\ so a
# roll can run from the copy while the next build links build\SupermanReturns.exe
# (Windows locks a running exe, so a roll from build\ blocks every link).
#
# Usage:
#   $exe = .\tools\snapshot_exe.ps1            # prints the snapshot exe path
#   .\tools\sr_drive.ps1 ... -ExePath $exe
#   .\tools\run_recomp_x.ps1 ... -ExePath $exe
#
# Why the copy behaves like build\: the rolls start the child in the repo root.
# kernel/file_io.cpp finds "Superman Returns (USA)" next to the exe, its parent,
# or the working directory (the last one hits). The shader cache and the save
# root come from LOCALAPPDATA / SR_INSTANCE_ID / SR_CONTENT_DIR, not the exe
# folder, unless a portable.txt sits next to the exe (none is copied). DXC/XMA
# runtime DLLs and the built-in port-feature recipes travel with the executable.
# The recipes contain no extracted game bytes; their hashes are recorded too.
# Native blobs travel too when present beside the build or explicitly selected with -NbAotDir. Their source and
# individual hashes are recorded; this preserves a chosen set, not proof that every runtime variant is covered.
#
# The id is the first 12 hex of the exe's sha256 (HEAD moves with doc-only
# commits while the exe does not, so the hash is the identity). A snapshot of an
# exe that already exists is reused, never overwritten. provenance.json records
# HEAD at snapshot time, the dirty source paths and the file hashes; the exe may
# predate HEAD, so the build's own [BUILD]/provenance lines stay authoritative.

param([string] $Label = "", [string] $BuildDir = "build", [string] $NbAotDir = "")
# Native blobs are source-keyed. Default only to the build's adjacent set; an older build must name its
# verified source explicitly with -NbAotDir. Never borrow the current checkout's blobs implicitly.

$ErrorActionPreference = "Stop"
$root  = (Join-Path $PSScriptRoot ".." | Resolve-Path).Path
$build = Join-Path $root $BuildDir
if ([IO.Path]::IsPathRooted($BuildDir)) { $build = [IO.Path]::GetFullPath($BuildDir) }
# Relative components must be resolved before deriving paths from FileInfo.FullName.
$build = [IO.Path]::GetFullPath($build)
$exe   = Join-Path $build "SupermanReturns.exe"
if (-not (Test-Path -LiteralPath $exe)) { [Console]::Error.WriteLine("snapshot_exe: $exe not found (build first)"); exit 1 }

$sha   = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash.ToLower()
$head  = (git -C $root rev-parse --short HEAD).Trim()
$dirty = @(git -C $root status --porcelain --untracked-files=no -- kernel gfx guest app tools/regen CMakeLists.txt)
$id    = $sha.Substring(0, 12)
if ($Label) { $id += "-" + ($Label -replace '[^A-Za-z0-9_.-]', '_') }

$dest = Join-Path $root ("archive\exe_snapshots\" + $id)
$destExe = Join-Path $dest "SupermanReturns.exe"
$featureRoot = Join-Path $build 'port_features'
$featureHashes = [ordered]@{}
if (-not (Test-Path -LiteralPath $featureRoot -PathType Container)) {
    [Console]::Error.WriteLine("snapshot_exe: missing built-in recipes at $featureRoot (build first)"); exit 1
}
foreach ($featureFile in (Get-ChildItem -LiteralPath $featureRoot -Recurse -File | Sort-Object FullName)) {
    $relativeFeature = $featureFile.FullName.Substring($featureRoot.Length + 1).Replace('\', '/')
    $featureHashes["port_features/$relativeFeature"] = (Get-FileHash -Algorithm SHA256 -LiteralPath $featureFile.FullName).Hash.ToLower()
}
if ($featureHashes.Count -eq 0) { [Console]::Error.WriteLine('snapshot_exe: built-in recipes are empty'); exit 1 }
$nbRoot = if ($NbAotDir) { [IO.Path]::GetFullPath($NbAotDir) } else { Join-Path $build 'data\shader_cache\nb' }
function Get-NbHashes([string]$Folder) {
    $result = [ordered]@{}
    if (-not (Test-Path -LiteralPath $Folder -PathType Container)) { return ,$result }
    $parent = Get-Item -LiteralPath $Folder
    while ($parent) {
        if ($parent.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "snapshot_exe: linked NB source $($parent.FullName)" }
        $parent = $parent.Parent
    }
    foreach ($item in (Get-ChildItem -LiteralPath $Folder -Force | Sort-Object Name)) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "snapshot_exe: linked NB entry $($item.FullName)" }
        if ($item.PSIsContainer -or $item.Extension -ne '.dxbc') { continue }
        if ($item.Length -le 32) { throw "snapshot_exe: empty/truncated NB blob $($item.FullName)" }
        $result["data/shader_cache/nb/$($item.Name)"] = (Get-FileHash -Algorithm SHA256 -LiteralPath $item.FullName).Hash.ToLower()
    }
    return ,$result
}
$nbHashes = Get-NbHashes $nbRoot
if (($NbAotDir -or (Test-Path -LiteralPath $nbRoot)) -and $nbHashes.Count -eq 0) {
    throw "snapshot_exe: no native blobs at $nbRoot"
}
function Assert-NbHashes([string]$Folder, $Expected) {
    $actual = Get-NbHashes $Folder
    if ($actual.Count -ne $Expected.Count) { throw "snapshot_exe: native blob set changed/missing at $Folder; pass a new -Label" }
    foreach ($entry in $Expected.GetEnumerator()) {
        if ($actual[$entry.Key] -ne $entry.Value) { throw "snapshot_exe: native blob changed/missing at $Folder; pass a new -Label" }
    }
}
if (Test-Path -LiteralPath $destExe) {
    if (-not (Test-Path -LiteralPath (Join-Path $dest 'provenance.json') -PathType Leaf)) {
        throw "snapshot_exe: incomplete snapshot at $dest; preserve it and pass a new -Label"
    }
    $have = (Get-FileHash -Algorithm SHA256 -LiteralPath $destExe).Hash.ToLower()
    if ($have -ne $sha) { [Console]::Error.WriteLine("snapshot_exe: $dest exists with a different exe; pass -Label"); exit 2 }
    foreach ($featureEntry in $featureHashes.GetEnumerator()) {
        $existingFeature = Join-Path $dest $featureEntry.Key
        if (-not (Test-Path -LiteralPath $existingFeature -PathType Leaf) -or
            (Get-FileHash -Algorithm SHA256 -LiteralPath $existingFeature).Hash.ToLower() -ne $featureEntry.Value) {
            [Console]::Error.WriteLine("snapshot_exe: $dest has different/missing built-in recipes; pass a new -Label"); exit 2
        }
    }
    Assert-NbHashes (Join-Path $dest 'data\shader_cache\nb') $nbHashes
    Assert-NbHashes $nbRoot $nbHashes
    Write-Output $destExe
    exit 0
}

New-Item -ItemType Directory -Force -Path $dest | Out-Null
# RELEASE_PLAN step 3: the SDL runtime the exe links - SDL3.dll; a pre-SDL3 build (a baseline) has SDL2.dll.
$sdlDll = $(if (Test-Path -LiteralPath (Join-Path $build "SDL3.dll")) { "SDL3.dll" } else { "SDL2.dll" })
$files = @("SupermanReturns.exe", "SupermanReturns.pdb", "SupermanReturns.map", $sdlDll, "dxcompiler.dll", "dxil.dll", "ffmpeg_xenia.dll", "THIRD_PARTY_LICENSES.md", "CREDITS.md", "LICENSE-xenia.txt")
$hashes = [ordered]@{}
foreach ($f in $files) {
    $src = Join-Path $build $f
    if (-not (Test-Path -LiteralPath $src)) {
        if ($f -eq "SupermanReturns.map") { continue }
        # Optional: a build with -DSR_FFMPEG_XENIA=OFF has no XMA decoder DLL (delay-loaded).
        if ($f -eq "ffmpeg_xenia.dll") { continue }
        [Console]::Error.WriteLine("snapshot_exe: missing $src"); exit 1
    }
    Copy-Item -LiteralPath $src -Destination (Join-Path $dest $f)
    $hashes[$f] = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $dest $f)).Hash.ToLower()
}
Copy-Item -LiteralPath $featureRoot -Destination (Join-Path $dest 'port_features') -Recurse
foreach ($featureEntry in $featureHashes.GetEnumerator()) {
    $copiedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $dest $featureEntry.Key)).Hash.ToLower()
    if ($copiedHash -ne $featureEntry.Value) { [Console]::Error.WriteLine('snapshot_exe: built-in recipes changed while copying'); exit 1 }
    $hashes[$featureEntry.Key] = $copiedHash
}
if ($nbHashes.Count) {
    New-Item -ItemType Directory -Force -Path (Join-Path $dest 'data\shader_cache\nb') | Out-Null
    foreach ($nbEntry in $nbHashes.GetEnumerator()) {
        Copy-Item -LiteralPath (Join-Path $nbRoot ([IO.Path]::GetFileName($nbEntry.Key))) -Destination (Join-Path $dest $nbEntry.Key)
        $hashes[$nbEntry.Key] = $nbEntry.Value
    }
    Assert-NbHashes (Join-Path $dest 'data\shader_cache\nb') $nbHashes
    Assert-NbHashes $nbRoot $nbHashes
}
if ($hashes["SupermanReturns.exe"] -ne $sha) { [Console]::Error.WriteLine("snapshot_exe: exe changed while copying; rebuild finished mid-copy?"); exit 1 }
# iter-464 input lane: the button-prompt glyphs ship next to the exe (gfx/glyph_subst.cpp reads
# <exe dir>/data/glyphs; CMake's post-build step puts them in build\data\glyphs).
$glyphs = Join-Path $build "data\glyphs"
if (Test-Path -LiteralPath $glyphs) {
    New-Item -ItemType Directory -Force -Path (Join-Path $dest "data") | Out-Null
    Copy-Item -LiteralPath $glyphs -Destination (Join-Path $dest "data\glyphs") -Recurse
    $hashes["data/glyphs/glyph_map.txt"] = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $dest "data\glyphs\glyph_map.txt")).Hash.ToLower()
}
# iter-465 modlib: the mod library's fonts ship next to the exe too (gfx/mod_library_ui.cpp reads
# <exe dir>/data/fonts; CMake's post-build step puts them in build\data\fonts).
$fonts = Join-Path $build "data\fonts"
if (Test-Path -LiteralPath $fonts) {
    New-Item -ItemType Directory -Force -Path (Join-Path $dest "data") | Out-Null
    Copy-Item -LiteralPath $fonts -Destination (Join-Path $dest "data\fonts") -Recurse
    foreach ($ff in Get-ChildItem -LiteralPath (Join-Path $dest "data\fonts") -Recurse -File) {
        $rel = "data/fonts/" + $ff.FullName.Substring((Join-Path $dest "data\fonts").Length + 1).Replace('\', '/')
        $hashes[$rel] = (Get-FileHash -Algorithm SHA256 -LiteralPath $ff.FullName).Hash.ToLower()
    }
}

$prov = [ordered]@{
    id      = $id
    created = (Get-Date).ToString("o")
    head    = (git -C $root rev-parse HEAD).Trim()
    dirty   = $dirty
    nbSource = $nbRoot
    nbFiles = $nbHashes.Count
    files   = $hashes
}
$prov | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 -Path (Join-Path $dest "provenance.json")
Write-Output $destExe
