param([int]$ProcessId = 0, [int]$WaitSeconds = 12)
# Post F4 ("Trace Frame") to a RUNNING instrumented Xenia and report the new .xtr.
# For captures at a scene reached by interactive navigation (virtual pad +
# xenia_shot.ps1), which tools/xenia_gpu_trace.ps1 cannot do because it fires F4
# after a fixed wait. Same PostMessage mechanism as that script: needs no focus
# (SendKeys/foreground tricks land in the wrong window from a background session).
# Only the trace-writer build writes anything (tools/xenia-trace/xenia_gpu_trace_release.patch).
$ErrorActionPreference = 'Stop'
$p = if ($ProcessId) { Get-Process -Id $ProcessId } else { Get-Process xenia_canary | Select-Object -First 1 }
if (-not $p) { Write-Host "xenia not running"; exit 1 }
$traceDir = Join-Path (Split-Path -Parent $p.Path) 'scratch\gpu'
$before = @(Get-ChildItem $traceDir -Filter *.xtr -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class KF4 {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { Write-Host "no xenia window handle"; exit 1 }
# lParam: repeat=1, scancode 0x3E (F4); keyup adds transition bits 30/31.
[KF4]::PostMessage($h, 0x100, [IntPtr]0x73, [IntPtr]0x003E0001) | Out-Null
Start-Sleep -Milliseconds 80
[KF4]::PostMessage($h, 0x101, [IntPtr]0x73, [IntPtr]0xC03E0001) | Out-Null
Write-Host "posted F4 to xenia pid $($p.Id) hwnd $h; waiting $WaitSeconds s for the .xtr in $traceDir"
Start-Sleep -Seconds $WaitSeconds
$new = Get-ChildItem $traceDir -Filter *.xtr -ErrorAction SilentlyContinue |
       Where-Object { $before -notcontains $_.FullName } | Sort-Object LastWriteTime -Descending
if (-not $new) { Write-Host "NO NEW .xtr - stock build (no trace writer), or the frame is still writing"; exit 1 }
foreach ($f in $new) { Write-Host ("captured: {0}  ({1} MB)" -f $f.FullName, [math]::Round($f.Length / 1MB, 1)) }
exit 0
