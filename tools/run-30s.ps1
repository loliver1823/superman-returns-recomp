# iter-463: an automated launch never reads or writes the player's saves - the
# game gets a fresh scratch save root unless the caller named one (seeded roll).
# iter-463: and never mounts the player's mods (mods/enabled.txt) - SR_MODS=0
# unless the caller named mods itself (docs/MODS.md "Automated runs").
$prevContentDir = $env:SR_CONTENT_DIR
$prevMods = $env:SR_MODS
$prevDevtools = $env:SR_DEVTOOLS
$prevWinBg = $env:SR_WINDOW_BACKGROUND
$prevRealIn = $env:SR_HARNESS_REAL_INPUT
# iter-464 bgruns: an automated launch never takes the foreground (kernel/host_automation.hpp).
if ($null -eq $env:SR_WINDOW_BACKGROUND) { $env:SR_WINDOW_BACKGROUND = '1' }
# ... and ignores the user's real keyboard/controller (no SR_INSTANCE_ID here, so say it).
if ($null -eq $env:SR_HARNESS_REAL_INPUT) { $env:SR_HARNESS_REAL_INPUT = '0' }
if ($null -eq $env:SR_DEVTOOLS) { $env:SR_DEVTOOLS = '0' }   # iter-463 phase 2 (docs/DEVTOOLS.md)
if ($null -eq $env:SR_MODS) { $env:SR_MODS = '0' }
$prevLog = $env:SR_LOG
if ($null -eq $env:SR_LOG) { $env:SR_LOG = 'dev' }   # iter-464 perf: this tool reads the full log (app/log.cpp profiles)
if (-not $env:SR_CONTENT_DIR) { $env:SR_CONTENT_DIR = Join-Path $env:TEMP ("SupermanRecomp\content-run30s-" + (Get-Date -Format "yyyyMMdd-HHmmss")) }
Write-Host "Save root (SR_CONTENT_DIR): $env:SR_CONTENT_DIR"
. (Join-Path $PSScriptRoot 'bg_window.ps1')   # iter-464 bgruns: no console window
$p = Start-SrGameProcess -FilePath ".\build\SupermanReturns.exe"
$env:SR_CONTENT_DIR = $prevContentDir
$env:SR_MODS = $prevMods
$env:SR_DEVTOOLS = $prevDevtools
$env:SR_WINDOW_BACKGROUND = $prevWinBg
$env:SR_HARNESS_REAL_INPUT = $prevRealIn
$env:SR_LOG = $prevLog
Write-Host "Started PID: $($p.Id)"
Start-Sleep -Seconds 30
Write-Host "Stopping..."
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Write-Host "Done."
