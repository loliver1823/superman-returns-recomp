# smoke_pkg.ps1 - THE canonical PLAYER-BUILD smoke (iter-466 tools consolidation; folds
# archive/v1_smoke/launch_pkg.ps1, launch.ps1 and v1_devtools/.../player_route.ps1).
#
# Launches a staged player package (tools/player/make_player_build.ps1 output) EXACTLY as the
# owner would - its own exe, cwd = the package, its content\ root and its mods.json rule - but in
# the background (SR_INSTANCE_ID: no focus, harness channel, real input ignored). Only
# observability gates are added (present dumps, KB pad obs, mod-library shots). Holds the game
# slot; stops only the process it started; writes smoke.json with a verdict.
#
#   powershell -NoProfile -File tools\player\smoke_pkg.ps1 -Pkg <package dir | name under archive\player>
#       [-Tag <name>] [-Owner smoke] [-Seconds 120] [-DumpEvery 10] [-Out <empty dir>] [-SlotHeld]
#       [-Press "START:300 wait:5 A:300"]      # optional: sr_press.py steps once the channel is up
#
# Verdict (smoke.json "verdict"):
#   ALIVE    still running at -Seconds and presenting (frames grew) -> killed by us, pass
#   EXITED   the game exited on its own (exit code recorded; 0 after a menu EXIT is a pass)
#   CRASH    a fatal marker (scripts/crash_markers.py FATAL_MARKERS) is in its log
#   STALLED  running but no new present dump in the last 30 s of the window
# Exit code: 0 for ALIVE/EXITED(0), 1 otherwise, 3 = slot not acquired, 2 = bad arguments.
# Output default: <main>\archive\v1_<Owner>\smoke\<Tag>\ (frames\, modlib\, run.log, smoke.json).
# ASCII ONLY (PowerShell 5.1 reads a BOM-less .ps1 as cp1252).
param(
  [Parameter(Mandatory = $true)] [string] $Pkg,
  [string] $Tag = "",
  [string] $Owner = "smoke",
  [int] $Seconds = 120,
  [int] $DumpEvery = 10,
  [string] $Out = "",
  [string] $Instance = "smoke",
  [string] $Press = "",
  [switch] $SlotHeld
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$main = if ($env:SR_MAIN_ROOT) { $env:SR_MAIN_ROOT } else { Split-Path -Parent (git -C $PSScriptRoot rev-parse --path-format=absolute --git-common-dir) }
$slotTool = Join-Path $main "tools\game_slot.py"
if (-not (Test-Path -LiteralPath $Pkg)) { $Pkg = Join-Path $main ("archive\player\" + $Pkg) }
$exe = Join-Path $Pkg "SupermanReturns.exe"
if (-not (Test-Path -LiteralPath $exe)) { Write-Output "smoke_pkg: no SupermanReturns.exe in $Pkg"; exit 2 }
$Pkg = (Resolve-Path -LiteralPath $Pkg).Path
$exe = (Resolve-Path -LiteralPath $exe).Path
if (-not $Tag) { $Tag = (Split-Path -Leaf $Pkg) + "_" + (Get-Date -Format "MMdd_HHmmss") }
if (-not $Out) { $Out = Join-Path $main ("archive\v1_{0}\smoke\{1}" -f $Owner, $Tag) }
if (Test-Path -LiteralPath $Out) { Write-Output "smoke_pkg: -Out exists: $Out"; exit 2 }
$frames = Join-Path $Out "frames"
New-Item -ItemType Directory -Force -Path $frames, (Join-Path $Out "modlib") | Out-Null
. (Join-Path $root "tools\bg_window.ps1")

# The owner's environment: nothing inherited from a lane shell may change what the package does.
foreach ($n in @("SR_CONTENT_DIR", "SR_MODS", "SR_DEVTOOLS", "SR_DEVTOOLS_CMDS", "SR_MOUSE_FEED", "SR_ALLOW_FORCING",
                 "SR_PORT_FEATURES", "SR_FORCING_ISOLATE", "SR_PRESENT_DUMP", "SR_PRESENT_DUMP_DIR")) {
  Remove-Item "Env:$n" -ErrorAction SilentlyContinue
}
$env:SR_INSTANCE_ID = $Instance
$env:SR_PORT_FEATURES = "1"
$env:SR_CONTENT_DIR = (Join-Path $Pkg "content")   # = the portable.txt root the owner gets
$env:SR_MODS = "@player"                           # = the owner's mods.json rule, read-only
$env:SR_KB_PAD_OBS = "1"
if ($DumpEvery -gt 0) {
  $env:SR_PRESENT_DUMP = "1"; $env:SR_PRESENT_DUMP_DIR = $frames
  $env:SR_PRESENT_DUMP_SPARSE_EVERY = "$DumpEvery"; $env:SR_PRESENT_DUMP_SPARSE_CAP = "6000"
}
$env:SR_MOD_LIBRARY_SHOTS = (Join-Path $Out "modlib")
$env:SDL_AUDIODRIVER = "disk"; $env:SDL_DISKAUDIOFILE = "NUL"
$env:SDL_AUDIO_DRIVER = "disk"; $env:SDL_AUDIO_DISK_OUTPUT_FILE = "NUL"   # SDL3 spellings (tools/sdl_audio_env.py)
$log = Join-Path $Pkg ("superman_returns_{0}.log" -f $Instance)

$meta = [ordered]@{ tool = "tools/player/smoke_pkg.ps1"; pkg = $Pkg; exe = $exe
  exe_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash.ToLower()
  instance = $Instance; seconds = $Seconds; started = (Get-Date -Format o); kind = "PLAYER BUILD (port features on)" }
$acquired = $false
$p = $null
$rc = 1
try {
  if ($SlotHeld) {
    $st = (& python $slotTool status) -join "`n"
    if ($st -notmatch ('lock: \{"owner": "' + [regex]::Escape($Owner) + '"')) { throw "SLOT: -SlotHeld but $Owner does not hold the game slot" }
  } else {
    & python $slotTool acquire --owner $Owner --note ("smoke_pkg {0} ({1} s)" -f $Tag, $Seconds) --poll 10
    if ($LASTEXITCODE -ne 0) { throw "SLOT: game slot not acquired" }
    $acquired = $true
  }
  # an earlier smoke's log is evidence: move it into this roll's folder, never delete it
  if (Test-Path -LiteralPath $log) { Move-Item -LiteralPath $log (Join-Path $Out "previous_run.log") }
  $p = Start-SrGameProcess -FilePath $exe -WorkingDirectory $Pkg
  $meta.pid = $p.Id
  if ($Press) {
    $pressArgs = @("-B", (Join-Path $root "tools\sr_press.py"), "--instance", $Instance, "--log", "`"$log`"",
                   "--frames", "`"$frames`"", "--timeout", "90") + ($Press -split '\s+')
    Start-Process -FilePath python -ArgumentList $pressArgs -WindowStyle Hidden -PassThru `
      -RedirectStandardOutput (Join-Path $Out "press.txt") -RedirectStandardError (Join-Path $Out "press_err.txt") | Out-Null
  }
  $deadline = (Get-Date).AddSeconds($Seconds)
  $nextBeat = (Get-Date).AddSeconds(60)
  while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    if ((Get-Date) -gt $nextBeat) { & python $slotTool heartbeat --owner $Owner | Out-Null; $nextBeat = (Get-Date).AddSeconds(60) }
  }
  $alive = -not $p.HasExited
  $files = @(Get-ChildItem -LiteralPath $frames -Filter "present_*" -ErrorAction SilentlyContinue)
  $recent = @($files | Where-Object { $_.LastWriteTime -gt (Get-Date).AddSeconds(-30) })
  if ($alive) { try { Stop-Process -Id $p.Id -Force } catch {} ; $p.WaitForExit(10000) | Out-Null }
  $meta.frames = $files.Count
  $meta.exit_code = $(if ($alive) { "killed at window end" } else { $p.ExitCode })
  $fatal = ""
  if (Test-Path -LiteralPath $log) {
    Copy-Item -LiteralPath $log (Join-Path $Out "run.log") -ErrorAction SilentlyContinue
    $fatal = (& python -B -c "import sys,re; sys.path.insert(0, sys.argv[1]); from crash_markers import FATAL_MARKERS as F; t=open(sys.argv[2],encoding='utf-8',errors='replace').read(); print(','.join(n for n,r in F if r.search(t)))" (Join-Path $root "scripts") $log) -join ""
  }
  $meta.fatal_markers = $fatal
  if ($fatal) { $meta.verdict = "CRASH" }
  elseif ($alive -and ($DumpEvery -le 0 -or $recent.Count -gt 0)) { $meta.verdict = "ALIVE" }
  elseif ($alive) { $meta.verdict = "STALLED" }
  else { $meta.verdict = "EXITED" }
  $rc = $(if ($meta.verdict -eq "ALIVE" -or ($meta.verdict -eq "EXITED" -and $p.ExitCode -eq 0)) { 0 } else { 1 })
} catch {
  if ("$_" -like "SLOT:*") { Write-Output ("smoke_pkg: " + "$_".Substring(6)); $rc = 3 } else { $meta.error = "$_"; throw }
} finally {
  if ($p -and -not $p.HasExited) { try { Stop-Process -Id $p.Id -Force } catch {} }
  if ($acquired) { & python $slotTool release --owner $Owner | Out-Null }
  $meta.ended = (Get-Date -Format o)
  [IO.File]::WriteAllText((Join-Path $Out "smoke.json"), ($meta | ConvertTo-Json -Depth 4))
}
Write-Output ("smoke_pkg: {0} verdict={1} frames={2} out={3}" -f $Tag, $meta.verdict, $meta.frames, $Out)
exit $rc
