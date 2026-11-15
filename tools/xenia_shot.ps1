param([string]$Out = "$PSScriptRoot\xenia.png", [switch]$Foreground)
# Screenshot the Xenia window client area. Part of the Xenia harness (see tools/XENIA_HARNESS.md).
# The agent Reads $Out to "see" the screen.
#
# iter-464 bgruns: automated runs never take the foreground (the user works on this PC). The
# default capture is PrintWindow(PW_CLIENTONLY | PW_RENDERFULLCONTENT): the window's OWN content
# via DWM, with NO ShowWindow / SetForegroundWindow - it works while Xenia sits behind other
# windows. -Foreground restores the old path (restore + foreground + screen BitBlt), which steals
# the foreground; use it only if PrintWindow returns black on some driver.
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WC {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [StructLayout(LayoutKind.Sequential)] public struct PT { public int X,Y; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref PT p);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
}
"@
# Physical-pixel coordinates: without this, on a scaled display GetClientRect/ClientToScreen
# return logical units and CopyFromScreen grabs a zoomed top-left crop (seen 2026-09-24).
[WC]::SetProcessDPIAware() | Out-Null
$p = Get-Process xenia_canary -ErrorAction SilentlyContinue | Select-Object -First 1
if(-not $p){ Write-Host "xenia not running"; exit 1 }
$h = $p.MainWindowHandle
if($h -eq [IntPtr]::Zero){ Write-Host "no xenia window"; exit 1 }
if($Foreground){
  [WC]::ShowWindow($h,9) | Out-Null
  [WC]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 400
} elseif([WC]::IsIconic($h)){
  Write-Host "xenia is minimized - PrintWindow cannot capture it (restore it, or -Foreground)"; exit 1
}
$rc = New-Object WC+RECT
[WC]::GetClientRect($h, [ref]$rc) | Out-Null
$w = $rc.R - $rc.L; $hgt = $rc.B - $rc.T
if($w -le 0 -or $hgt -le 0){ Write-Host "bad client rect"; exit 1 }
$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
if($Foreground){
  $pt = New-Object WC+PT; $pt.X = 0; $pt.Y = 0
  [WC]::ClientToScreen($h, [ref]$pt) | Out-Null
  $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, (New-Object System.Drawing.Size $w,$hgt))
  $g.Dispose()
} else {
  $dc = $g.GetHdc()
  $okPw = [WC]::PrintWindow($h, $dc, 3)   # 1 = PW_CLIENTONLY | 2 = PW_RENDERFULLCONTENT
  $g.ReleaseHdc($dc); $g.Dispose()
  if(-not $okPw){ Write-Host "PrintWindow failed"; $bmp.Dispose(); exit 1 }
}
# A coarse all-black check, so a blind capture is reported rather than silently saved.
$lit = $false
for($y = 0; $y -lt $hgt -and -not $lit; $y += [Math]::Max(1, [int]($hgt / 32))){
  for($x = 0; $x -lt $w; $x += [Math]::Max(1, [int]($w / 32))){
    $c = $bmp.GetPixel($x, $y); if(($c.R + $c.G + $c.B) -gt 0){ $lit = $true; break }
  }
}
$tw = [int][Math]::Min(1024, $w); $th = [int]($hgt * ($tw / $w))
$small = New-Object System.Drawing.Bitmap $tw, $th
$g2 = [System.Drawing.Graphics]::FromImage($small)
$g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g2.DrawImage($bmp, 0, 0, $tw, $th)
$g2.Dispose(); $bmp.Dispose()
$small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$small.Dispose()
Write-Host ("saved $Out  (${tw}x${th}, src ${w}x${hgt}, {0}{1})" -f $(if ($Foreground) { 'screen copy' } else { 'PrintWindow, background' }), $(if ($lit) { '' } else { ', ALL BLACK at the 32x32 sample - the game may be dark, or try -Foreground' }))
