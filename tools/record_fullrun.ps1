param(
  [int]$DurationSeconds = 300,
  [int]$Fps = 10,
  [string]$Out = "",
  [string]$ProcessName = "SupermanReturns",
  [int]$JpegQuality = 80
)
# THE full-run recorder - the producer for `python tools/verify_run.py <framesDir>`
# (the mandatory recording-verdict gate, feedback_verification_scope).
#
# iter-441: verify_run.py's docstring cited "tools record_fullrun*.ps1" but no such
# script was ever committed - the whole-run capture lived only as ad-hoc untracked
# code, so the project's hard verification rule was not executable from a clean tree.
# This is that script, written to verify_run.py's exact input contract:
#   frame files  f_<elapsedMs, 7 digits>.jpg   in a single flat directory.
#
# !!! iter-442 - READ THIS BEFORE TRUSTING A BLACK RESULT FROM THIS TOOL !!!
# PrintWindow ALSO returns all-black for our D3D12 FLIP-model swapchain. Measured:
# a live roll that provably rendered (in-engine GPU readback showed the animated
# EA logo and the full-colour WBIE logo, max=255) produced 1010 frames here that
# were ENTIRELY BLACK. The iter-438b note below is therefore INCOMPLETE - MPO /
# independent-flip blinds gdigrab and CopyFromScreen AND PrintWindow.
#   => A black result from this tool is NOT evidence the renderer is dark.
#   => The authoritative capture is the engine's own backbuffer readback:
#        SR_PRESENT_DUMP=1 (+ SR_PRESENT_DUMP_EVERY=1 for short runs), which
#        CopyResource's the actual render target and writes BMPs to archive/.
#      That is a GPU-side read of the exact pixels presented; nothing in the
#      desktop compositor can blind it.
# This tool remains useful for run-shape/timing (freeze detection, era lengths).
#
# CAPTURE METHOD - PrintWindow(PW_RENDERFULLCONTENT), same as tools/game_shot.ps1.
# This is NOT interchangeable with desktop capture: MPO / independent-flip
# promotion blinds gdigrab and CopyFromScreen even with the window PROVEN
# foreground (iter-438b - gdigrab recorded the window BEHIND the game). ffmpeg
# gdigrab recordings of this game are not admissible evidence; PrintWindow reads
# the window's own swapchain content.
#
# Usage:
#   .\tools\record_fullrun.ps1 -DurationSeconds 300 -Fps 10 -Out archive\run_iter441
#   python tools/verify_run.py archive\run_iter441        # then EYEBALL the sheets
#
# Writes into the OUT dir only. Default lands under archive/ (gitignored evidence
# ground) - never the repo root (CLAUDE.md "Repo hygiene").

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class PWRec {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
"@

$projectRoot = (Join-Path $PSScriptRoot ".." | Resolve-Path).Path
if (-not $Out) {
  $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
  $Out = Join-Path $projectRoot "archive\fullrun-$stamp"
}
$Out = [IO.Path]::GetFullPath($Out)
[IO.Directory]::CreateDirectory($Out) | Out-Null

# JPEG encoder at an explicit quality - verify_run.py reads .jpg, and the default
# encoder quality loses movie-era gradients the artifact classes depend on.
$jpegCodec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() |
  Where-Object { $_.MimeType -eq "image/jpeg" } | Select-Object -First 1
$encParams = New-Object System.Drawing.Imaging.EncoderParameters 1
$encParams.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter(
  [System.Drawing.Imaging.Encoder]::Quality, [int]$JpegQuality)

Write-Host "Waiting for $ProcessName ..."
$deadlineWait = (Get-Date).AddSeconds(60)
$p = $null
while ((Get-Date) -lt $deadlineWait) {
  $p = Get-Process $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($p -and $p.MainWindowHandle -ne [IntPtr]::Zero) { break }
  Start-Sleep -Milliseconds 200
}
if (-not $p -or $p.MainWindowHandle -eq [IntPtr]::Zero) {
  throw "$ProcessName window not found within 60s (start the run first)."
}
$h = $p.MainWindowHandle

$intervalMs = [int](1000 / [Math]::Max(1, $Fps))
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$deadlineMs = $DurationSeconds * 1000
$n = 0; $failed = 0
Write-Host "Recording $DurationSeconds s @ ${Fps}fps -> $Out"

while ($sw.ElapsedMilliseconds -lt $deadlineMs) {
  $tickStart = $sw.ElapsedMilliseconds
  # Re-check liveness: a crashed run must end the recording, not fill the dir
  # with identical stale frames (which read as a "freeze" era in the sheets).
  if ($p.HasExited) { Write-Host "process exited at ${tickStart}ms - stopping"; break }
  try {
    $rc = New-Object PWRec+RECT
    [PWRec]::GetClientRect($h, [ref]$rc) | Out-Null
    $w = $rc.R - $rc.L; $hgt = $rc.B - $rc.T
    if ($w -gt 0 -and $hgt -gt 0) {
      $bmp = New-Object System.Drawing.Bitmap $w, $hgt
      $g = [System.Drawing.Graphics]::FromImage($bmp)
      $dc = $g.GetHdc()
      [PWRec]::PrintWindow($h, $dc, 2) | Out-Null   # 2 = PW_RENDERFULLCONTENT
      $g.ReleaseHdc($dc); $g.Dispose()
      $name = "f_{0:D7}.jpg" -f [int]$tickStart      # verify_run.py contract
      $bmp.Save((Join-Path $Out $name), $jpegCodec, $encParams)
      $bmp.Dispose()
      $n++
    }
  } catch {
    $failed++
    if ($failed -le 3) { Write-Host "capture error: $($_.Exception.Message)" }
  }
  $spent = $sw.ElapsedMilliseconds - $tickStart
  $sleep = $intervalMs - $spent
  if ($sleep -gt 0) { Start-Sleep -Milliseconds $sleep }
}

$sw.Stop()
$elapsed = [Math]::Round($sw.ElapsedMilliseconds / 1000, 1)
$effFps = if ($elapsed -gt 0) { [Math]::Round($n / $elapsed, 2) } else { 0 }
Write-Host "Captured $n frames in ${elapsed}s (effective ${effFps} fps, $failed errors)"
Write-Host "NEXT (mandatory): python tools/verify_run.py `"$Out`"  - then EYEBALL every sheet."
