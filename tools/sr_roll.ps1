# sr_roll.ps1 - THE canonical "one live roll" runner (iter-466 tools consolidation).
#
# Folds the per-lane roll.ps1 / m_roll.ps1 / chain*.sh copies (archive/v1_*/) into one tool:
#   game slot acquire + heartbeat + release on every path  ->  tools/game_slot.py
#   background launch, scenario steps, timeout, crash scan  ->  tools/sr_drive.ps1
#   optional input script on the same harness channel     ->  tools/controls/harness_play.py
#   stop ONLY the exe this roll started (by full path, never by name), write roll.json
#
#   powershell -NoProfile -File tools\sr_roll.ps1 -Owner <lane> -Tag <name> -Scenario <json>
#       [-ExePath <exe>] [-PlayScript <txt>] [-PlayLog <log>] [-Gates "SR_X=1,SR_Y=2"]
#       [-PortFeatures 0|1] [-DumpEvery 20] [-DumpCap 1500] [-TimeoutSeconds 900]
#       [-Out <empty dir>] [-Instance <id>] [-SlotHeld] [-NoSlot] [-GracefulQuit]
#
#   -Owner        game-slot owner = your lane name (also the default SR_INSTANCE_ID).
#   -Tag          roll name; -Out defaults to <main>\archive\v1_<Owner>\rolls\<Tag> (must not exist).
#   -Scenario     tools\scenarios\*.json (sr_drive contract). A boot-and-watch roll can use
#                 tools\scenarios\frontend_long_observe.json.
#   -ExePath      default: this checkout's build\SupermanReturns.exe. Prefer an exe SNAPSHOT
#                 (archive\exe_snapshots\...) for long rolls so a rebuild cannot swap it mid-roll.
#   -PlayScript   harness_play.py script run beside the drive (player/legacy keymap, mouse...).
#   -PlayLog      log harness_play awaits on; default superman_returns_<Instance>.log in this
#                 checkout (a RESTART NOW relaunch writes superman_returns_<Instance>_r1.log).
#   -PortFeatures 0 = NATURAL roll (tagged natural=true, adds SR_MODS=0); 1 = PORT FEATURE roll.
#   -DumpEvery    sparse SR_PRESENT_DUMP every N presents into <Out>\frames (0 = no frames).
#   -SlotHeld     the caller holds the slot for a batch: check ownership, no acquire/release.
#   -NoSlot       skip the slot entirely (only for a process that is not a game - you rarely want this).
#   -GracefulQuit end the roll by asking the game to quit (sr_drive -GracefulQuit, tools/sr_quit.ps1): it drains,
#                 prints its terminal snapshot ([QUIT] terminal snapshot complete) and exits; Stop-Process only as
#                 the fallback (docs/TOOLKIT.md 1.8m). Grade with tools/perf/terminal_snapshot.py.
#
# Background-run rules (TOOLKIT 1.8a): SR_INSTANCE_ID is always set, so the game window opens
# without activation at the bottom of the z-order, takes keys only from the harness channel and
# ignores the real keyboard/pads. One game process at a time: the slot enforces it.
# Output: <Out>\roll.json (exe sha256, gates, natural flag, exit codes, times), gates.txt,
# drive\ (sr_drive artifacts), frames\, play.jsonl, sr_drive.txt, run.log (copy of the log).
# Exit code: sr_drive's (0 ok), 3 = slot not acquired, 2 = bad arguments, 5 = sr_drive's exit code could not be read
# (roll.json then has sr_drive_exit null and an error; never 0 - lane E step 5 E3-E6 r3, lane B's audit 2026-10-23).
#   -DriveScript  the drive to run (default this checkout's tools\sr_drive.ps1); tools\test_sr_roll.ps1's seam only.
# ASCII ONLY (PowerShell 5.1 reads a BOM-less .ps1 as cp1252).
param(
  [Parameter(Mandatory = $true)] [string] $Owner,
  [Parameter(Mandatory = $true)] [string] $Tag,
  [Parameter(Mandatory = $true)] [string] $Scenario,
  [string] $ExePath = "",
  [string] $PlayScript = "",
  [string] $PlayLog = "",
  [string] $Gates = "",
  [ValidateSet(0, 1)] [int] $PortFeatures = 0,
  [int] $DumpEvery = 20,
  [int] $DumpCap = 1500,
  [int] $TimeoutSeconds = 900,
  [string] $Out = "",
  [string] $Instance = "",
  [int] $HeartbeatSeconds = 60,
  [switch] $SlotHeld,
  [switch] $NoSlot,
  [switch] $GracefulQuit,  # tools/graceful-quit (TOOLKIT 1.8m): sr_drive ends the roll by asking the game to quit (terminal snapshot)
  [string] $DriveScript = ""   # test seam (tools/test_sr_roll.ps1): the drive script; default tools\sr_drive.ps1
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path          # this checkout (main or worktree)
$main = if ($env:SR_MAIN_ROOT) { $env:SR_MAIN_ROOT } else { Split-Path -Parent (git -C $PSScriptRoot rev-parse --path-format=absolute --git-common-dir) }
$slotTool = Join-Path $main "tools\game_slot.py"                   # the ONE slot lives in the main tree
if (-not (Test-Path -LiteralPath $slotTool)) { $slotTool = Join-Path $root "tools\game_slot.py" }
$python = if ($env:SR_PYTHON) { $env:SR_PYTHON } else { "python" }   # SR_PYTHON: tools/sr_artifacts.ps1's rule

if (-not $ExePath) { $ExePath = Join-Path $root "build\SupermanReturns.exe" }
if (-not (Test-Path -LiteralPath $ExePath)) { Write-Output "sr_roll: no exe at $ExePath"; exit 2 }
$ExePath = (Resolve-Path -LiteralPath $ExePath).Path
if (-not (Test-Path -LiteralPath $Scenario)) {
  $cand = Join-Path $root ("tools\scenarios\" + $Scenario)
  if (Test-Path -LiteralPath $cand) { $Scenario = $cand } else { Write-Output "sr_roll: no scenario $Scenario"; exit 2 }
}
$Scenario = (Resolve-Path -LiteralPath $Scenario).Path
if ($PlayScript) { $PlayScript = (Resolve-Path -LiteralPath $PlayScript).Path }
if (-not $DriveScript) { $DriveScript = Join-Path $root "tools\sr_drive.ps1" }
if (-not (Test-Path -LiteralPath $DriveScript)) { Write-Output "sr_roll: no drive script $DriveScript"; exit 2 }
$DriveScript = (Resolve-Path -LiteralPath $DriveScript).Path
if (-not $Instance) { $Instance = ($Owner -replace '[^A-Za-z0-9_.-]', '_') }
if (-not $Out) { $Out = Join-Path $main ("archive\v1_{0}\rolls\{1}" -f $Owner, $Tag) }
if (Test-Path -LiteralPath $Out) { Write-Output "sr_roll: -Out exists: $Out (every roll gets a fresh folder)"; exit 2 }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
if (-not $PlayLog) { $PlayLog = Join-Path $root ("superman_returns_{0}.log" -f $Instance) }

# Gates: instance (background window + private log/channel) + port features or natural + frames.
$g = @("SR_INSTANCE_ID=$Instance", "SR_PORT_FEATURES=$PortFeatures", "SDL_AUDIODRIVER=disk", "SDL_AUDIO_DRIVER=disk", "SDL_DISKAUDIOFILE=NUL", "SDL_AUDIO_DISK_OUTPUT_FILE=NUL", "SR_KB_PAD_OBS=1")
if ($PortFeatures -eq 0) { $g += "SR_MODS=0" }
if ($DumpEvery -gt 0) {
  $g += @("SR_PRESENT_DUMP=1", ("SR_PRESENT_DUMP_DIR=" + (Join-Path $Out "frames")),
          "SR_PRESENT_DUMP_SPARSE_EVERY=$DumpEvery", "SR_PRESENT_DUMP_SPARSE_CAP=$DumpCap")
}
$allGates = ($g -join ",") + $(if ($Gates) { "," + $Gates } else { "" })
Set-Content -LiteralPath (Join-Path $Out "gates.txt") -Value $allGates -Encoding ascii
$head = ""
try { $head = (& git -C $root rev-parse --short HEAD 2>$null) } catch {}
$meta = [ordered]@{
  tool = "tools/sr_roll.ps1"; owner = $Owner; tag = $Tag; started = (Get-Date -Format o)
  checkout = $root; head = "$head"; exe = $ExePath
  exe_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $ExePath).Hash.ToLower()
  scenario = $Scenario; play = $PlayScript; gates = $allGates
  natural = ($PortFeatures -eq 0 -and -not $Gates)
  graceful_quit = [bool]$GracefulQuit
  kind = $(if ($PortFeatures -eq 1) { "PORT FEATURE roll (SR_PORT_FEATURES=1)" } elseif ($Gates) { "natural + extra gates (check them)" } else { "NATURAL roll" })
}
function Save-Meta { [IO.File]::WriteAllText((Join-Path $Out "roll.json"), ($meta | ConvertTo-Json -Depth 4)) }
Save-Meta

$acquired = $false
$play = $null
$drive = $null
$rc = 1
try {
  if ($SlotHeld) {
    $st = (& $python $slotTool status) -join "`n"
    if ($st -notmatch ('lock: \{"owner": "' + [regex]::Escape($Owner) + '"')) {
      throw "SLOT: -SlotHeld but $Owner does not hold the game slot"
    }
  } elseif (-not $NoSlot) {
    & $python $slotTool acquire --owner $Owner --note ("sr_roll {0} (<= {1} s)" -f $Tag, $TimeoutSeconds) --poll 10
    if ($LASTEXITCODE -ne 0) { throw "SLOT: game slot not acquired" }
    $acquired = $true
  }
  # A stale log from an earlier roll must not satisfy harness_play's awaits; it is evidence, so
  # it moves into this roll's folder instead of being deleted.
  if ($PlayScript -and (Test-Path -LiteralPath $PlayLog)) { Move-Item -LiteralPath $PlayLog (Join-Path $Out "previous_run.log") }
  if ($PlayScript) {
    $playArgs = @((Join-Path $root "tools\controls\harness_play.py"), "--instance", $Instance, "--log", "`"$PlayLog`"",
                  "--script", "`"$PlayScript`"", "--out", "`"$(Join-Path $Out 'play.jsonl')`"")
    $play = Start-Process -FilePath $python -ArgumentList $playArgs -WindowStyle Hidden -PassThru `
              -RedirectStandardOutput (Join-Path $Out "play_stdout.txt") -RedirectStandardError (Join-Path $Out "play_stderr.txt")
    try { $null = $play.Handle } catch {}   # see the drive below: ExitCode is readable only with the handle opened
  }
  $driveArgs = @("-NoProfile", "-File", "`"$DriveScript`"", "-Scenario", "`"$Scenario`"",
                 "-Out", "`"$(Join-Path $Out 'drive')`"", "-ExePath", "`"$ExePath`"",
                 "-TimeoutSeconds", "$TimeoutSeconds", "-Gates", "`"$allGates`"")
  if ($GracefulQuit) { $driveArgs += "-GracefulQuit" }   # tools/graceful-quit: <Out>\drive\attempt*\quit.json
  $drive = Start-Process -FilePath "powershell.exe" -ArgumentList $driveArgs -WindowStyle Hidden -PassThru `
             -RedirectStandardOutput (Join-Path $Out "sr_drive.txt") -RedirectStandardError (Join-Path $Out "sr_drive_err.txt")
  # Windows PowerShell: a Start-Process -PassThru object reports ExitCode $null unless its Handle was opened while the
  # process ran (then `exit $null` exits 0 - every Ally roll of 2026-10-23 recorded sr_drive_exit null). Open it now.
  try { $null = $drive.Handle } catch {}
  $nextBeat = (Get-Date).AddSeconds($HeartbeatSeconds)
  while (-not $drive.HasExited) {
    Start-Sleep -Seconds 2
    if (($acquired -or $SlotHeld) -and (Get-Date) -gt $nextBeat) {
      & $python $slotTool heartbeat --owner $Owner | Out-Null
      $nextBeat = (Get-Date).AddSeconds($HeartbeatSeconds)
    }
  }
  $drive.WaitForExit()
  $rc = $drive.ExitCode
  if ($rc -isnot [int]) {   # $null (or anything else): a hard error, never a pass
    $meta.error = "sr_drive's exit code could not be read (got '$rc'); the roll is not graded as passed"
    $meta.sr_drive_exit = $null
    $rc = 5
  } else {
    $meta.sr_drive_exit = $rc
  }
} catch {
  if ("$_" -like "SLOT:*") { Write-Output ("sr_roll: " + "$_".Substring(6)); $rc = 3 } else { $meta.error = "$_"; throw }
} finally {
  if ($play -and -not $play.HasExited) { try { Stop-Process -Id $play.Id -Force } catch {} }
  if ($play) { $meta.play_exit = $(if ($play.HasExited) { $play.ExitCode } else { "killed" }) }
  if ($drive -and -not $drive.HasExited) { try { Stop-Process -Id $drive.Id -Force } catch {} }
  # sr_drive stops the game it drove; a child left by a failed RESTART follow is still ours:
  # stop it by its exact path, never by name alone (another lane's snapshot exe is not ours).
  Get-CimInstance Win32_Process -Filter "Name='SupermanReturns.exe'" | Where-Object {
    $_.ExecutablePath -and ([IO.Path]::GetFullPath($_.ExecutablePath) -ieq $ExePath)
  } | ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force } catch {} }
  if (Test-Path -LiteralPath $PlayLog) { Copy-Item -LiteralPath $PlayLog (Join-Path $Out "run.log") -ErrorAction SilentlyContinue }
  # tools/graceful-quit: the last attempt's quit receipt beside the run.log copy (tools/perf/terminal_snapshot.py
  # counts a terminal snapshot complete only with it). A stale one from an earlier roll in -Out goes first.
  Remove-Item -LiteralPath (Join-Path $Out "quit.json") -ErrorAction SilentlyContinue
  $receipt = Get-ChildItem -LiteralPath (Join-Path $Out "drive") -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^attempt(\d+)$' -and (Test-Path -LiteralPath (Join-Path $_.FullName "quit.json")) } |
    Sort-Object { [int]($_.Name -replace '^attempt', '') } | Select-Object -Last 1
  if ($receipt) { Copy-Item -LiteralPath (Join-Path $receipt.FullName "quit.json") (Join-Path $Out "quit.json") -ErrorAction SilentlyContinue }
  if ($acquired) { & $python $slotTool release --owner $Owner | Out-Null }
  $meta.ended = (Get-Date -Format o)
  Save-Meta
}
if ($rc -isnot [int]) { $rc = 5 }   # never `exit $null` (= 0)
Write-Output ("sr_roll: {0} exit={1} out={2}" -f $Tag, $rc, $Out)
exit $rc
