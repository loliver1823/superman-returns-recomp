# check_restart.ps1 - the second half of a MODS-menu RESTART roll (iter-464 modmenu lane).
#
# tools/scenarios/femenu_mods.json ends with RESTART NOW: the parent exits 0 (sr_drive checks
# that) and relaunches the exe (kernel/mod_menu.cpp modMenuSpawnRestartIfRequested). The child
# inherits the roll's environment plus SR_RESTART_GENERATION=1, so it logs to
# superman_returns_<instance>_r1.log in the roll's working folder and dumps frames to
# <frames>_r1. This script waits for the child's own evidence, then ENDS THE CHILD (a roll must
# never leave a game process behind - the game slot rule), and moves its log next to the
# parent's artifacts.
#
#   powershell -File tools/modmenu/check_restart.ps1 -Attempt <sr_drive -Out>\attempt1 `
#       -ExpectList "modmenu_fe, sample_mission" [-Instance run0] [-Seconds 240]
#
# PASS = the child logged "[MODMENU] restarted by the MODS menu (generation 1 ...)", mounted
# exactly ExpectList ("[MOD] enabled (highest priority first): <list>  [source: ...]"), and
# reached the title's front-end ticks ([NAT_FE] #256). Exit 0 pass, 1 fail.
param(
    [Parameter(Mandatory=$true)][string] $Attempt,
    [Parameter(Mandatory=$true)][string] $ExpectList,
    [string] $Instance = "run0",
    [string] $WorkDir = "",
    [string] $ExePath = "",          # the roll's exe: the fallback stop only touches processes of this image
    [int]    $Seconds = 240
)
$ErrorActionPreference = "Stop"
if (-not $WorkDir) { $WorkDir = (Join-Path $PSScriptRoot "..\.." | Resolve-Path).Path }
$log = Join-Path $WorkDir ("superman_returns_{0}_r1.log" -f $Instance)
$out = Join-Path $Attempt "restart_check.txt"
$lines = New-Object System.Collections.Generic.List[string]
function Say([string] $s) { $lines.Add($s); Write-Host $s }

$deadline = (Get-Date).AddSeconds($Seconds)
$childPid = 0
$ok = @{ restarted = $false; list = $false; title = $false }
$listPat = [regex]::Escape("[MOD] enabled (highest priority first): $ExpectList  [source:")
try {
  while ((Get-Date) -lt $deadline) {
    if (Test-Path -LiteralPath $log) {
      $text = Get-Content -LiteralPath $log -Raw -ErrorAction SilentlyContinue
      if ($text) {
        $m = [regex]::Match($text, '\[MODMENU\] restarted by the MODS menu \(generation 1, parent pid (\d+), pid (\d+)\)')
        if ($m.Success -and -not $ok.restarted) {
          $ok.restarted = $true; $childPid = [int]$m.Groups[2].Value
          Say ("child: restarted line seen, parent pid {0}, child pid {1}" -f $m.Groups[1].Value, $childPid)
        }
        if (-not $ok.list -and $text -match $listPat) { $ok.list = $true; Say "child: mounted exactly '$ExpectList'" }
        if (-not $ok.title -and $text -match '\[NAT_FE\] #256 natural FE master tick') { $ok.title = $true; Say "child: front end running ([NAT_FE] #256)" }
        if ($ok.restarted -and $ok.list -and $ok.title) { break }
      }
    }
    Start-Sleep -Seconds 2
  }
  if ($ok.title) { Start-Sleep -Seconds 20 }   # a few title frames for the review
  $mods = @()
  if (Test-Path -LiteralPath $log) {
    $mods = @(Select-String -LiteralPath $log -Pattern '^\S*.*\[MOD\] (enabled|overlay|option folder|error)' | ForEach-Object { $_.Line })
  }
  foreach ($l in $mods | Select-Object -First 20) { Say ("child log: " + $l.Trim()) }
} finally {
  # End the child whatever happened: by the pid it logged, else by its image path + instance log.
  if ($childPid -gt 0) {
    $p = Get-Process -Id $childPid -ErrorAction SilentlyContinue
    if ($p) { Stop-Process -Id $childPid -Force; Say "child pid $childPid stopped" }
  } else {
    Say "child pid unknown (no restarted line): looking for a process of this roll's exe image"
    if ($ExePath) {
      $want = [System.IO.Path]::GetFullPath($ExePath)
      Get-Process SupermanReturns -ErrorAction SilentlyContinue | Where-Object { $_.Path -and ([System.IO.Path]::GetFullPath($_.Path) -eq $want) } | ForEach-Object {
        Say ("WARNING: pid {0} runs {1} - stopping it (this roll's child)" -f $_.Id, $_.Path)
        Stop-Process -Id $_.Id -Force
      }
    } else { Say "no -ExePath: nothing stopped (check the process list by hand)" }
  }
  Start-Sleep -Seconds 2
  if (Test-Path -LiteralPath $log) { Move-Item -LiteralPath $log -Destination (Join-Path $Attempt "run_r1.log") -Force }
}
$pass = $ok.restarted -and $ok.list -and $ok.title
Say ("RESTART CHECK: {0} (restarted={1} list={2} title={3})" -f $(if ($pass) { "PASS" } else { "FAIL" }), $ok.restarted, $ok.list, $ok.title)
$lines | Set-Content -LiteralPath $out -Encoding ascii
if ($pass) { exit 0 } else { exit 1 }
