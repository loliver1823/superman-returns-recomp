param(
  [string]$Iso = $env:SR_GAME_ISO,      # your own disc image
  [string]$XeniaExe = "$PSScriptRoot\..\reference\xenia-canary\build\bin\Windows\Release\xenia_canary.exe",
  [int]$WaitSeconds = 110,
  [switch]$Stream,          # capture a full stream trace (trace_gpu_stream) instead of one F4 frame
  [switch]$KeepOpen,        # don't kill xenia after capture
  [switch]$PressStart,      # advance PAST the title screen and trace the MENU instead
  [int]$MenuWaitSeconds = 25
)
# Capture a golden Xenos GPU trace (.xtr) from the instrumented Xenia Canary.
# Pipeline doc: docs/TOOLKIT.md section 1.6 (GPU trace replay). Requires the
# trace-writer-enabled build (tools/xenia-trace/xenia_gpu_trace_release.patch).
# Frame mode: boots the game, waits $WaitSeconds (press-start ~= 100 s), sends F4
# ("Trace Frame"), waits for the .xtr to land in <exe dir>\scratch\gpu\.
# Replay with: SR_GPU_TRACE_REPLAY=<path.xtr> .\build\SupermanReturns.exe

$ErrorActionPreference = 'Stop'
$exeDir = Split-Path -Parent (Resolve-Path $XeniaExe)
$traceDir = Join-Path $exeDir 'scratch\gpu'
New-Item -ItemType Directory -Force $traceDir | Out-Null
$before = @(Get-ChildItem $traceDir -Filter *.xtr -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })

# The virtual pad must exist BEFORE xenia starts, or xenia never enumerates it
# and @START goes nowhere (docs/oracle/README.md).
$padProc = $null
if ($PressStart) {
  $padProc = Start-Process -FilePath 'python' -ArgumentList "`"$PSScriptRoot\xenia_pad.py`"" -PassThru -WindowStyle Hidden
  Write-Host "started virtual pad (pid $($padProc.Id)); settling..."
  Start-Sleep -Seconds 5
}

# Owner rule: automated runs never play sound through his speakers (--mute; the APU still runs).
$xeArgs = @('--mute=true')
if ($Stream) { $xeArgs += '--trace_gpu_stream' }
$xeArgs += ('"' + $Iso + '"')
Write-Host "launching: $XeniaExe $($xeArgs -join ' ')"
# iter-464 bgruns: automated runs never take the foreground - launched without
# activation, parked at the bottom of the z-order, guarded (tools/bg_window.ps1).
. (Join-Path $PSScriptRoot 'bg_window.ps1')
$proc = Start-SrBackgroundProcess -FilePath $XeniaExe -ArgumentList ($xeArgs -join ' ') -WorkingDirectory $exeDir
$bg = Move-SrWindowToBottom -ProcessId $proc.Id -TimeoutMs 30000 -GuardMs 15000 -PreviousForeground $proc.SrPrevForeground
$null = Start-SrWindowGuard -ProcessId $proc.Id -PreviousForeground $proc.SrPrevForeground   # for Xenia's whole life (it can activate itself later)
Write-Host ("xenia PID {0} in the background ({1} window(s) to the bottom, foreground handed back {2}x)" -f $proc.Id, $bg.moved, $bg.gaveBack)

Write-Host "waiting $WaitSeconds s for the game to reach the menu..."
Start-Sleep -Seconds $WaitSeconds
if ($proc.HasExited) { Write-Host "XENIA EXITED EARLY (code $($proc.ExitCode))"; exit 1 }

if ($PressStart) {
  # @START advances the title screen to the "LOAD A GAME?" menu. A bare 'A'
  # token did NOT advance it in testing (docs/oracle/README.md).
  & "$PSScriptRoot\xenia_shot.ps1" -Out "$PSScriptRoot\xenia_trace_prestart.png" | Out-Null
  Set-Content -Path "$PSScriptRoot\pad_cmd.txt" -Value "@START" -Encoding ASCII
  Write-Host "sent @START; waiting $MenuWaitSeconds s for the menu..."
  Start-Sleep -Seconds $MenuWaitSeconds
  if ($proc.HasExited) { Write-Host "XENIA EXITED after START (code $($proc.ExitCode))"; exit 1 }
}

# Screenshot for the log (verifies what was on screen at capture time).
& "$PSScriptRoot\xenia_shot.ps1" -Out "$PSScriptRoot\xenia_trace_capture.png" | Out-Null

if (-not $Stream) {
  # PostMessage the F4 hotkey ("Trace Frame") straight to the Xenia window.
  # SendKeys/SetForegroundWindow are useless from a background session: Windows
  # denies focus-stealing, and the keys land in whatever IS foreground (proven
  # by a capture screenshot full of the dev's editor). PostMessage needs no focus.
  Add-Type @"
using System;
using System.Runtime.InteropServices;
public class KX {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@
  $hwnd = $proc.MainWindowHandle
  if ($hwnd -eq [IntPtr]::Zero) { $proc.Refresh(); $hwnd = $proc.MainWindowHandle }
  if ($hwnd -eq [IntPtr]::Zero) { Write-Host "no xenia window handle"; exit 1 }
  $WM_KEYDOWN = 0x100; $WM_KEYUP = 0x101; $VK_F4 = 0x73
  # lParam: repeat=1, scancode 0x3E (F4); keyup adds transition bits 30/31.
  [KX]::PostMessage($hwnd, $WM_KEYDOWN, [IntPtr]$VK_F4, [IntPtr]0x003E0001) | Out-Null
  Start-Sleep -Milliseconds 80
  [KX]::PostMessage($hwnd, $WM_KEYUP, [IntPtr]$VK_F4, [IntPtr]0xC03E0001) | Out-Null
  Write-Host "posted F4 (Trace Frame) to xenia hwnd $hwnd; waiting for the .xtr..."
  Start-Sleep -Seconds 10
} else {
  Write-Host "stream mode: trace has been recording since boot; closing to flush..."
}

if (-not $KeepOpen) { try { $proc.Kill(); $proc.WaitForExit(10000) } catch {} ; Start-Sleep -Seconds 2 }
if ($padProc) { try { $padProc.Kill() } catch {} }

$after = Get-ChildItem $traceDir -Filter *.xtr -ErrorAction SilentlyContinue |
         Where-Object { $before -notcontains $_.FullName } |
         Sort-Object LastWriteTime -Descending
if (-not $after) {
  Write-Host "NO NEW .xtr IN $traceDir -- is the trace-writer patch built in?"
  exit 1
}
foreach ($f in $after) {
  $mb = [math]::Round($f.Length / 1MB, 1)
  Write-Host ("captured: " + $f.FullName + "  (" + $mb + " MB)")
}
exit 0
