param(
  [string]$Out = "$PSScriptRoot\game.png",
  [string]$ProcessName = "SupermanReturns"
)
# Screenshot the game window via PrintWindow(PW_RENDERFULLCONTENT) — captures the
# window's OWN swapchain content, immune to MPO/independent-flip promotion that
# blinds GDI desktop capture (gdigrab, CopyFromScreen). iter-438b: the game window
# was proven foreground+visible while gdigrab recorded the window BEHIND it.
# NOTE: xenia_shot.ps1 still uses CopyFromScreen — its black frames may be MPO.
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class PWS {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
"@
$p = Get-Process $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { Write-Host "$ProcessName not running"; exit 1 }
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { Write-Host "no main window"; exit 1 }
$rc = New-Object PWS+RECT
[PWS]::GetClientRect($h, [ref]$rc) | Out-Null
$w = $rc.R - $rc.L; $hgt = $rc.B - $rc.T
if ($w -le 0 -or $hgt -le 0) { Write-Host "bad client rect"; exit 1 }
# PrintWindow with flag 2 (PW_RENDERFULLCONTENT) renders the full window incl.
# title bar; DX content included on Win8.1+.
$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
[PWS]::PrintWindow($h, $dc, 2) | Out-Null
$g.ReleaseHdc($dc); $g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "saved $Out (${w}x${hgt})"
