param(
  [string]$Iso = $env:SR_GAME_ISO,      # your own disc image
  [string]$XeniaExe = "",
  [string]$OutDir = "",
  [string]$Tag = "burndown_a",
  [string]$VaList = "$PSScriptRoot\burndown_vas.txt",
  [string]$IcallList = "$PSScriptRoot\burndown_icall_funcs.txt",
  [switch]$IcallBad,           # also record EVERY out-of-code-range indirect call (tag 3), all functions
  [int]$TotalSeconds = 480,    # whole run; the flow below reaches Metropolis gameplay by ~260 s
  [int]$PressStartAt = 120,    # findings.json: PRESS START at ~115 s on this build + full ISO
  [switch]$NoPad
)
# iter-463 burndown ORACLE run: the v2-traced Xenia (tools/xenia-trace/README.md "Oracle v2")
# boots Superman Returns to Metropolis gameplay and records
#   tag 0: entries into every VA in $VaList (stub VAs, PPC_CALL_INDIRECT_FUNC special cases,
#          liveness VAs) - a VA never entered is NEVER called on the console-faithful emulator;
#   tag 2: every indirect-call target made from the functions in $IcallList;
#   tag 3: (-IcallBad) every indirect call anywhere whose target is out of the recomp's
#          code range - our "out-of-range" condition, measured on the oracle.
# It runs a SEPARATE exe (reference/xenia-canary-oracle/build-trace), never the user's install,
# whose config file is only COPIED (read) once into the build's portable dir.
# One emulator process at a time; do not overlap with a recomp roll.
$ErrorActionPreference = 'Stop'

$mainRepo = if ($env:SR_MAIN_ROOT) { $env:SR_MAIN_ROOT } else { Split-Path -Parent (git -C $PSScriptRoot rev-parse --path-format=absolute --git-common-dir) }
if (-not $XeniaExe) {
  $cand = Join-Path $PSScriptRoot '..\..\reference\xenia-canary-oracle\build-trace\bin\Windows\Release\xenia_canary.exe'
  if (Test-Path $cand) { $XeniaExe = (Resolve-Path $cand).Path }
  else { $XeniaExe = Join-Path $mainRepo 'reference\xenia-canary-oracle\build-trace\bin\Windows\Release\xenia_canary.exe' }
}
if (-not (Test-Path $XeniaExe)) { Write-Host "traced xenia not built: $XeniaExe"; exit 2 }
if (-not (Test-Path $Iso)) { Write-Host "ISO not found: $Iso"; exit 2 }
if (-not $OutDir) { $OutDir = Join-Path $mainRepo ("archive\v1_burndown2\xtrace\" + $Tag) }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$exeDir = Split-Path -Parent $XeniaExe

# Portable config next to the traced exe (copied once from the user's install; the
# install itself is never written).
$userCfg = if ($env:SR_XENIA_CONFIG) { $env:SR_XENIA_CONFIG } else { '' }   # your Xenia install's config
if (-not (Test-Path (Join-Path $exeDir 'portable.txt'))) { Set-Content -Path (Join-Path $exeDir 'portable.txt') -Value '' -Encoding ASCII }
if (-not (Test-Path (Join-Path $exeDir 'xenia-canary.config.toml')) -and $userCfg -and (Test-Path $userCfg)) {
  Copy-Item $userCfg (Join-Path $exeDir 'xenia-canary.config.toml')
}

$trace = Join-Path $OutDir 'oracle.trace'
$env:XENIA_GUEST_TRACE = '1'
$env:XENIA_GUEST_TRACE_FILE = $trace
$env:XENIA_GUEST_TRACE_VAS = '@' + (Resolve-Path $VaList).Path
$env:XENIA_GUEST_TRACE_ICALL_FUNCS = '@' + (Resolve-Path $IcallList).Path
if ($IcallBad) { $env:XENIA_GUEST_TRACE_ICALL_BAD = '1' } else { Remove-Item Env:\XENIA_GUEST_TRACE_ICALL_BAD -ErrorAction SilentlyContinue }

$padProc = $null
if (-not $NoPad) {
  $padProc = Start-Process -FilePath 'python' -ArgumentList ('"' + (Join-Path $mainRepo 'tools\xenia_pad.py') + '"') -PassThru -WindowStyle Hidden
  Start-Sleep -Seconds 5
}
$padCmd = Join-Path $mainRepo 'tools\pad_cmd.txt'
$shot = Join-Path $mainRepo 'tools\xenia_shot.ps1'

# Owner rule: automated runs never play sound through his speakers. The traced Xenia's portable config has apu=any,
# mute=false, so mute here (the APU still runs; the trace is unaffected). Found by the forcing lane, 2026-10-10.
$xeArgs = @(('"' + $Iso + '"'), ('--log_file="' + (Join-Path $OutDir 'xenia.log') + '"'), '--mute=true')
Write-Host "launching $XeniaExe $($xeArgs -join ' ')"
# iter-464 bgruns: automated runs never take the foreground - launched without
# activation, parked at the bottom of the z-order, guarded (tools/bg_window.ps1).
. (Join-Path $mainRepo 'tools\bg_window.ps1')
$proc = Start-SrBackgroundProcess -FilePath $XeniaExe -ArgumentList ($xeArgs -join ' ') -WorkingDirectory $exeDir
$bg = Move-SrWindowToBottom -ProcessId $proc.Id -TimeoutMs 30000 -GuardMs 15000 -PreviousForeground $proc.SrPrevForeground
$null = Start-SrWindowGuard -ProcessId $proc.Id -PreviousForeground $proc.SrPrevForeground   # for Xenia's whole life (it can activate itself later)
Write-Host ("xenia PID {0} in the background ({1} window(s) to the bottom, foreground handed back {2}x)" -f $proc.Id, $bg.moved, $bg.gaveBack)
$t0 = Get-Date

# Timed flow (archive/v1_audio/oracle/findings.json): PRESS START -> profile popup ->
# DOWN A (continue without a profile) -> opening cinematic -> A (skip) -> gameplay.
$steps = @(
  @{ At = $PressStartAt;       Cmd = '@START'; Name = 's1_press_start' },
  @{ At = $PressStartAt + 20;  Cmd = 'DOWN A'; Name = 's2_no_profile' },
  @{ At = $PressStartAt + 60;  Cmd = 'A';      Name = 's3_skip_cinematic' },
  @{ At = $PressStartAt + 120; Cmd = 'A';      Name = 's4_gameplay' }
)
foreach ($s in $steps) {
  while (((Get-Date) - $t0).TotalSeconds -lt $s.At) {
    if ($proc.HasExited) { break }
    Start-Sleep -Seconds 1
  }
  if ($proc.HasExited) { Write-Host "XENIA EXITED at $([int]((Get-Date) - $t0).TotalSeconds) s (code $($proc.ExitCode))"; break }
  & $shot -Out (Join-Path $OutDir ($s.Name + '_before.png')) | Out-Null
  if ($padProc) { Set-Content -Path $padCmd -Value $s.Cmd -Encoding ASCII }
  Write-Host ("t=" + [int]((Get-Date) - $t0).TotalSeconds + "s sent '" + $s.Cmd + "' (" + $s.Name + ")")
}
while (-not $proc.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt $TotalSeconds) { Start-Sleep -Seconds 2 }
if (-not $proc.HasExited) {
  & $shot -Out (Join-Path $OutDir 'final.png') | Out-Null
  try { $proc.CloseMainWindow() | Out-Null; $proc.WaitForExit(15000) | Out-Null } catch {}
  if (-not $proc.HasExited) { try { $proc.Kill(); $proc.WaitForExit(10000) | Out-Null } catch {} }
}
if ($padProc) { try { $padProc.Kill() } catch {} }

# Records are fflush'ed per write under the v2 filters, so a kill keeps them.
$sum = Join-Path $OutDir 'summary.txt'
& python (Join-Path $PSScriptRoot 'summarize_trace.py') $trace --vas $VaList --json (Join-Path $OutDir 'summary.json') | Tee-Object -FilePath $sum
exit 0
