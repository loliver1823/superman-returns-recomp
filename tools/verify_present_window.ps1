param(
    [int]$DurationSeconds = 290,
    [int]$CaptureFps = 10,
    [int]$OffsetX = 320,
    [int]$OffsetY = 180,
    [int]$Width = 1280,
    [int]$Height = 720,
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = (Join-Path $PSScriptRoot ".." | Resolve-Path).Path
$exePath = Join-Path $projectRoot "build\SupermanReturns.exe"
if (-not (Test-Path -LiteralPath $exePath)) {
    throw "Executable not found: $exePath (run cmake --build build first)"
}
$ffmpeg = (Get-Command ffmpeg -ErrorAction Stop).Source

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class SupermanPresentVerifyWin32 {
    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter,
        int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);
}
"@

if (-not $OutputDirectory) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutputDirectory = Join-Path $env:TEMP "superman-present-$stamp"
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null

$hashPath = Join-Path $OutputDirectory "window.framemd5"
$ffmpegLog = Join-Path $OutputDirectory "ffmpeg.log"
$shotPattern = Join-Path $OutputDirectory "window-%03d.png"
$summaryPath = Join-Path $OutputDirectory "summary.json"

$game = $null
$ffmpegExit = -1
try {
    # iter-463: an automated launch never touches the player's saves - a scratch
    # save root next to this run's output unless the caller named one.
    # It also never mounts the player's mods (mods/enabled.txt): SR_MODS=0 unless
    # the caller named mods itself (docs/MODS.md "Automated runs").
    $prevContentDir = $env:SR_CONTENT_DIR
    $prevMods = $env:SR_MODS
    $prevDevtools = $env:SR_DEVTOOLS
    if ($null -eq $env:SR_DEVTOOLS) { $env:SR_DEVTOOLS = '0' }   # iter-463 phase 2 (docs/DEVTOOLS.md)
    if (-not $env:SR_CONTENT_DIR) { $env:SR_CONTENT_DIR = Join-Path $OutputDirectory "content" }
    if ($null -eq $env:SR_MODS) { $env:SR_MODS = '0' }
    # iter-464 bgruns: no console window and no activation (docs/TOOLKIT.md 1.8a). This
    # tool still makes the window TOPMOST below (gdigrab reads the desktop), so it covers
    # that screen region for the capture - never activated, but visible: run it when the
    # user is away.
    $prevWinBg = $env:SR_WINDOW_BACKGROUND
    $prevRealIn = $env:SR_HARNESS_REAL_INPUT
    if ($null -eq $env:SR_WINDOW_BACKGROUND) { $env:SR_WINDOW_BACKGROUND = '1' }
    # ... and ignores the user's real keyboard/controller (no SR_INSTANCE_ID here, so say it).
    if ($null -eq $env:SR_HARNESS_REAL_INPUT) { $env:SR_HARNESS_REAL_INPUT = '0' }
    . (Join-Path $PSScriptRoot 'bg_window.ps1')
    $game = Start-SrGameProcess -FilePath $exePath -WorkingDirectory $projectRoot
    $env:SR_CONTENT_DIR = $prevContentDir
    $env:SR_MODS = $prevMods
    $env:SR_DEVTOOLS = $prevDevtools
    $env:SR_WINDOW_BACKGROUND = $prevWinBg
    $env:SR_HARNESS_REAL_INPUT = $prevRealIn
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 100
        $game.Refresh()
    } while (-not $game.HasExited -and $game.MainWindowHandle -eq 0 -and
             [DateTime]::UtcNow -lt $deadline)
    if ($game.HasExited) { throw "Game exited before its window was created" }
    if ($game.MainWindowHandle -eq 0) { throw "Game window was not created within 15 seconds" }

    # gdigrab cannot read this D3D12 HWND directly, so capture its desktop
    # rectangle and continuously keep the exact launched HWND above that region.
    # SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE = 0x13.
    $captureWindowHandle = $game.MainWindowHandle
    if (-not [SupermanPresentVerifyWin32]::SetWindowPos(
            $captureWindowHandle, [IntPtr](-1), 0, 0, 0, 0, 0x13)) {
        throw "Could not make the launched game window topmost"
    }
    $windowHandleHex = "0x{0:X}" -f $captureWindowHandle.ToInt64()

    Start-Sleep -Seconds 4
    $filter = "[0:v]split=2[analysis][shots];" +
              "[analysis]blackdetect=d=0.15:pix_th=0.02[analysisout];" +
              "[shots]fps=1/10[shotsout]"
    $ffArgs = @(
        "-hide_banner", "-nostats", "-y",
        "-f", "gdigrab", "-draw_mouse", "0", "-framerate", "$CaptureFps",
        "-offset_x", "$OffsetX", "-offset_y", "$OffsetY",
        "-video_size", "${Width}x${Height}", "-i", "desktop",
        "-t", "$DurationSeconds", "-filter_complex", $filter,
        "-map", "[analysisout]", "-f", "framemd5", $hashPath,
        "-map", "[shotsout]", "-t", "$DurationSeconds", "-fps_mode", "vfr", $shotPattern
    )
    $quotedArgs = @($ffArgs | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' })
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $ffmpeg
    $startInfo.Arguments = $quotedArgs -join " "
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardError = $true
    $ffmpegProcess = [Diagnostics.Process]::new()
    $ffmpegProcess.StartInfo = $startInfo
    if (-not $ffmpegProcess.Start()) { throw "Failed to start ffmpeg" }
    $stderrTask = $ffmpegProcess.StandardError.ReadToEndAsync()
    try {
        while (-not $ffmpegProcess.HasExited) {
            if ($game.HasExited) { throw "Game exited while capture was running" }
            $game.Refresh()
            $currentWindowHandle = $game.MainWindowHandle
            if ($currentWindowHandle -eq 0 -or
                -not [SupermanPresentVerifyWin32]::IsWindow($currentWindowHandle) -or
                -not [SupermanPresentVerifyWin32]::IsWindowVisible($currentWindowHandle)) {
                throw "The launched game process no longer has a visible main window"
            }
            $captureWindowHandle = $currentWindowHandle
            if (-not [SupermanPresentVerifyWin32]::SetWindowPos(
                    $captureWindowHandle, [IntPtr](-1), 0, 0, 0, 0, 0x13)) {
                throw "Could not keep the launched game window topmost"
            }
            Start-Sleep -Milliseconds 50
            $ffmpegProcess.Refresh()
        }
        $ffmpegProcess.WaitForExit()
        $ffmpegExit = $ffmpegProcess.ExitCode
        [IO.File]::WriteAllText($ffmpegLog, $stderrTask.Result)
    }
    finally {
        if (-not $ffmpegProcess.HasExited) {
            Stop-Process -Id $ffmpegProcess.Id -ErrorAction SilentlyContinue
        }
    }
}
finally {
    if ($game -and -not $game.HasExited) {
        Stop-Process -Id $game.Id -ErrorAction SilentlyContinue
        Wait-Process -Id $game.Id -Timeout 10 -ErrorAction SilentlyContinue
    }
}

$hashes = @()
$capturedDimensions = ""
if (Test-Path -LiteralPath $hashPath) {
    foreach ($line in [IO.File]::ReadLines($hashPath)) {
        if ($line -match '^#dimensions 0:\s*([0-9]+x[0-9]+)') {
            $capturedDimensions = $Matches[1]
        }
        if ($line -match ',\s*([0-9a-fA-F]{32})\s*$') { $hashes += $Matches[1].ToLowerInvariant() }
    }
}

$blackSegments = @()
$shortBlackSegments = @()
$totalBlackSeconds = 0.0
if (Test-Path -LiteralPath $ffmpegLog) {
    foreach ($line in [IO.File]::ReadLines($ffmpegLog)) {
        if ($line -match 'black_start:([0-9.]+)\s+black_end:([0-9.]+)\s+black_duration:([0-9.]+)') {
            $start = [double]$Matches[1]
            $end = [double]$Matches[2]
            $duration = [double]$Matches[3]
            $segment = [pscustomobject]@{
                Start = $start
                End = $end
                Duration = $duration
            }
            $blackSegments += $segment
            $totalBlackSeconds += $duration
            if ($start -gt 2.0 -and $end -lt ($DurationSeconds - 2.0) -and $duration -le 0.3) {
                $shortBlackSegments += $segment
            }
        }
    }
}

$abaAlternations = 0
$abaWithinBlackSegments = 0
for ($i = 1; $i + 1 -lt $hashes.Count; ++$i) {
    if ($hashes[$i - 1] -ne $hashes[$i + 1] -or $hashes[$i] -eq $hashes[$i - 1]) {
        continue
    }
    $seconds = $i / [double]$CaptureFps
    $insideBlack = $false
    foreach ($segment in $blackSegments) {
        if ($seconds -ge $segment.Start -and $seconds -le $segment.End) {
            $insideBlack = $true
            break
        }
    }
    if ($insideBlack) { ++$abaWithinBlackSegments } else { ++$abaAlternations }
}

$lateStart = [Math]::Min($hashes.Count, 150 * $CaptureFps)
$longestLateHold = 0
$currentHold = 0
$previousHash = $null
for ($i = $lateStart; $i -lt $hashes.Count; ++$i) {
    if ($hashes[$i] -eq $previousHash) { ++$currentHold } else { $currentHold = 1 }
    if ($currentHold -gt $longestLateHold) { $longestLateHold = $currentHold }
    $previousHash = $hashes[$i]
}

$shots = @(Get-ChildItem -LiteralPath $OutputDirectory -Filter "window-*.png" -File)
$uniqueFrameHashes = @($hashes | Sort-Object -Unique).Count
$minimumFrames = [int]($DurationSeconds * $CaptureFps * 0.75)
$minimumShots = [Math]::Max(1, [int]($DurationSeconds / 10) - 1)
$minimumUniqueFrames = $(if ($DurationSeconds -ge 60) { 10 } else { 1 })
$passed = $ffmpegExit -eq 0 -and $hashes.Count -ge $minimumFrames -and
          $shots.Count -ge $minimumShots -and $shortBlackSegments.Count -eq 0 -and
          $abaAlternations -eq 0 -and $capturedDimensions -eq "${Width}x${Height}" -and
          $uniqueFrameHashes -ge $minimumUniqueFrames -and
          $totalBlackSeconds -lt ($DurationSeconds * 0.8)

$summary = [ordered]@{
    Status = $(if ($passed) { "PASS" } else { "FAIL" })
    Capture = "real topmost game window via desktop gdigrab; no video file"
    WindowHandle = $windowHandleHex
    CapturedDimensions = $capturedDimensions
    DurationSeconds = $DurationSeconds
    CaptureFps = $CaptureFps
    CapturedFrames = $hashes.Count
    UniqueFrameHashes = $uniqueFrameHashes
    MilestonePngs = $shots.Count
    BlackSegments = $blackSegments
    TotalBlackSeconds = [Math]::Round($totalBlackSeconds, 3)
    ShortBlackSegments = $shortBlackSegments
    AbaFrameAlternations = $abaAlternations
    AbaWithinBlackSegments = $abaWithinBlackSegments
    LongestIdenticalLateHoldSeconds = [Math]::Round($longestLateHold / [double]$CaptureFps, 3)
    FfmpegExitCode = $ffmpegExit
    OutputDirectory = $OutputDirectory
}
[IO.File]::WriteAllText($summaryPath, ($summary | ConvertTo-Json -Depth 4))
$summary | ConvertTo-Json -Depth 4
if (-not $passed) { exit 1 }
