# Run SupermanReturns.exe and close after the log captures what we need.
# Waits for: (1) "[BUGCHECK] Halting guest thread" in log, OR (2) "[VdSwap] #N" (unless -RunFullDuration), OR (3) log line count >= MinLines, OR (4) timeout.
# Usage: .\run-test-capture-log.ps1 [-MinLines 400] [-TimeoutSeconds 45] [-RunFullDuration] [-LogPath "build\superman_returns.log"]

param(
    [int]    $MinLines        = 450,
    [int]    $TimeoutSeconds  = 45,
    [switch]  $RunFullDuration,
    [string]  $LogFile        = "superman_returns.log",
    [string]  $ExeName        = "SupermanReturns"
)

$ErrorActionPreference = "Stop"
$projectRoot = Join-Path $PSScriptRoot ".." | Resolve-Path
$buildDir    = Join-Path $projectRoot "build"
$exePath     = Join-Path $buildDir ($ExeName + ".exe")
# Log is written to cwd (project root). Copy to build/ at end for compatibility.
$logPath     = Join-Path $projectRoot $LogFile

if (-not (Test-Path $exePath)) {
    Write-Error "Exe not found: $exePath"
}

# Start fresh log for this run (app will recreate). Skip if file is locked (exe already running).
if (Test-Path $logPath) {
    try { Remove-Item $logPath -Force -ErrorAction Stop } catch { Write-Host "Log file in use, continuing without delete." }
}

Write-Host "Starting $ExeName..."
# Run from project root so disc content paths (e.g. "Superman Returns (USA)/DATA/") resolve correctly
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
if (-not $env:SR_CONTENT_DIR) { $env:SR_CONTENT_DIR = Join-Path $env:TEMP ("SupermanRecomp\content-capture-" + (Get-Date -Format "yyyyMMdd-HHmmss")) }
Write-Host "Save root (SR_CONTENT_DIR): $env:SR_CONTENT_DIR"
. (Join-Path $PSScriptRoot 'bg_window.ps1')   # iter-464 bgruns: no console window
$proc = Start-SrGameProcess -FilePath $exePath -WorkingDirectory $projectRoot
$env:SR_CONTENT_DIR = $prevContentDir
$env:SR_MODS = $prevMods
$env:SR_DEVTOOLS = $prevDevtools
$env:SR_WINDOW_BACKGROUND = $prevWinBg
$env:SR_HARNESS_REAL_INPUT = $prevRealIn
$env:SR_LOG = $prevLog

$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
$haltSeen = $false
$reason   = ""

while ([DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Milliseconds 800
    if (-not (Test-Path $logPath)) { continue }

    $content = Get-Content $logPath -Raw -ErrorAction SilentlyContinue
    $lines   = (Get-Content $logPath -ErrorAction SilentlyContinue)
    $lineCount = if ($lines) { $lines.Count } else { 0 }

    if ($content -match "\[BUGCHECK\] Halting guest thread") {
        $haltSeen = $true
        $reason   = "halt message in log"
        break
    }
    if (-not $RunFullDuration -and $content -match "\[VdSwap\] #\d") {
        $reason = "VdSwap (frame present) in log"
        break
    }
    if ($lineCount -ge $MinLines) {
        $reason = "log lines >= $MinLines ($lineCount)"
        break
    }
}
if (-not $reason) { $reason = "timeout" }

if ($proc.HasExited) {
    Write-Host "Process already exited (exit code $($proc.ExitCode))."
} else {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Write-Host "Stopped process."
}

$finalLines = 0
if (Test-Path $logPath) {
    $finalLines = (Get-Content $logPath -ErrorAction SilentlyContinue).Count
    # Copy log to build/ for compatibility with build-dir consumers
    $buildLogPath = Join-Path $buildDir $LogFile
    Copy-Item $logPath $buildLogPath -Force -ErrorAction SilentlyContinue
}
Write-Host "Done. Reason: $reason. Log lines: $finalLines. Log: $logPath"
