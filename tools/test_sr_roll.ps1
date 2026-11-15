# test_sr_roll.ps1 - sr_roll.ps1 records sr_drive's REAL exit code (lane E, step 5 E3-E6 r3; lane B's audit 2026-10-23:
# every Ally roll recorded "sr_drive_exit": null and exited 0 because a Start-Process -PassThru object reports
# ExitCode $null unless its Handle was opened while the process ran).
#   powershell -NoProfile -File tools\test_sr_roll.ps1
# No game, no slot (-NoSlot), no frames: a stub drive script stands in for tools\sr_drive.ps1 (-DriveScript).
#   T1 a drive that exits 3: sr_roll exits 3 and roll.json records sr_drive_exit 3;
#   T2 a drive that exits 0: sr_roll exits 0, roll.json 0;
#   T3 an unreadable exit (this sr_roll with its Handle read removed): sr_roll exits 5 - never 0 - and roll.json
#      records sr_drive_exit null with an error.
# Exit 0 = all pass. ASCII ONLY (PowerShell 5.1 reads a BOM-less .ps1 as cp1252).
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$roll = Join-Path $root "tools\sr_roll.ps1"
# the scratch folder lives under this checkout's archive\ (the repo rule for run artifacts) and is removed again
$tmp = Join-Path $root ("archive\sr_roll_test_" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$fails = 0
function Check($ok, $what) {
  if ($ok) { Write-Output "  ok  $what" } else { Write-Output "FAIL: $what"; $script:fails++ }
}
try {
  $stubBody = @'
param([string]$Scenario, [string]$Out, [string]$ExePath, [int]$TimeoutSeconds, [string]$Gates, [switch]$GracefulQuit)
Start-Sleep -Milliseconds 1500
exit __RC__
'@
  $stub3 = Join-Path $tmp "drive_exit3.ps1"
  $stub0 = Join-Path $tmp "drive_exit0.ps1"
  [IO.File]::WriteAllText($stub3, $stubBody.Replace("__RC__", "3"))
  [IO.File]::WriteAllText($stub0, $stubBody.Replace("__RC__", "0"))
  $scn = Join-Path $tmp "scenario.json"
  [IO.File]::WriteAllText($scn, '{"name": "sr-roll-test", "steps": []}')
  $exe = Join-Path $tmp "SupermanReturns.exe"     # never run: the stub drive ignores it (roll.json hashes it)
  [IO.File]::WriteAllText($exe, "not an exe")

  function Run-Roll($script, $tag, $stub) {
    $out = Join-Path $tmp $tag
    $null = & powershell.exe -NoProfile -File $script -Owner "srrolltest" -Tag $tag -Scenario $scn -ExePath $exe `
      -NoSlot -DumpEvery 0 -Out $out -DriveScript $stub 2>&1
    $rc = $LASTEXITCODE
    $meta = $null
    $rj = Join-Path $out "roll.json"
    if (Test-Path -LiteralPath $rj) { $meta = Get-Content -LiteralPath $rj -Raw | ConvertFrom-Json }
    return @{ rc = $rc; meta = $meta }
  }

  $r = Run-Roll $roll "t1" $stub3
  Check ($r.rc -eq 3) "T1 a drive that exits 3: sr_roll exits 3 (got $($r.rc))"
  Check ($r.meta -and $r.meta.sr_drive_exit -eq 3 -and $r.meta.sr_drive_exit -is [int]) "T1 roll.json records sr_drive_exit 3 (got '$($r.meta.sr_drive_exit)')"

  $r = Run-Roll $roll "t2" $stub0
  Check ($r.rc -eq 0) "T2 a drive that exits 0: sr_roll exits 0 (got $($r.rc))"
  Check ($r.meta -and $r.meta.sr_drive_exit -eq 0 -and $null -ne $r.meta.sr_drive_exit) "T2 roll.json records sr_drive_exit 0 (got '$($r.meta.sr_drive_exit)')"

  # T3: the guard. A copy of this sr_roll without the drive's Handle read (in a tools\ folder of its own, so its
  # $PSScriptRoot\.. is the temp dir) cannot read the exit code: it must fail hard (5), never pass as 0.
  $src = [IO.File]::ReadAllText($roll)
  $line = 'try { $null = $drive.Handle } catch {}'
  Check ($src.Contains($line)) "T3 sr_roll reads the drive's Handle right after Start-Process"
  $copyDir = Join-Path $tmp "nohandle\tools"
  New-Item -ItemType Directory -Path $copyDir | Out-Null
  $copy = Join-Path $copyDir "sr_roll.ps1"
  [IO.File]::WriteAllText($copy, $src.Replace($line, '# (test: no Handle read)'))
  $r = Run-Roll $copy "t3" $stub3
  Check ($r.rc -eq 5) "T3 an unreadable exit code: sr_roll exits 5, not 0 (got $($r.rc))"
  Check ($r.meta -and $null -eq $r.meta.sr_drive_exit -and "$($r.meta.error)" -match "could not be read") "T3 roll.json records sr_drive_exit null and the error"
} finally {
  try { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction Stop } catch {}
  Check (-not (Test-Path -LiteralPath $tmp)) "the test's scratch folder is removed ($tmp)"
}
Write-Output ("sr_roll tests: {0} ({1} failing)" -f $(if ($fails) { "FAIL" } else { "PASS" }), $fails)
exit $(if ($fails) { 1 } else { 0 })
