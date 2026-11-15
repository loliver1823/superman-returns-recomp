# Compile the kPointList expansion geometry shader to DXIL (iter-370).
#
# Uses the SAME dxc binary that produced the cached VS/PS DXIL, so the validator
# and signing versions match. A PSO that mixes blobs signed by different
# validators is rejected at CreateGraphicsPipelineState.
#
# The dxc of XenosRecomp's thirdparty/dxc-bin @737ac9f: the third_party/XenosRecomp submodule (a clone of the
# public repository, after `git submodule update --init --recursive`), else an old untracked
# tools/XenosRecomp-clone. Its DLLs are byte-identical to the DXC 1.8.2407 release the runtime translator loads
# (cmake/dxc_release.cmake), and the output is byte-identical to the private tree's tracked
# data/shader_cache/point_sprite.gs.dxil (sha256 095df5a4..., checked 2026-11-16 on a clean clone of the public
# export). The game reads it from data\shader_cache\ beside the shader cache folder (gfx/d3d12_backend.cpp
# pointSpriteGs): without it, point-list draws do not translate.
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $PSScriptRoot)

$dxc = @("third_party\XenosRecomp\thirdparty\dxc-bin\bin\x64\dxc.exe",
         "tools\XenosRecomp-clone\thirdparty\dxc-bin\bin\x64\dxc.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
$src = "tools\xenosrecomp\point_sprite.gs.hlsl"
$out = "data\shader_cache\point_sprite.gs.dxil"

if (-not $dxc) { throw "dxc not found: run git submodule update --init --recursive (third_party/XenosRecomp)" }
New-Item -ItemType Directory -Force (Split-Path -Parent $out) | Out-Null

& $dxc -T gs_6_0 -E main -Fo $out $src
if ($LASTEXITCODE -ne 0) { throw "dxc failed with exit code $LASTEXITCODE" }

$len = (Get-Item $out).Length
Write-Host "wrote $out ($len bytes)"
