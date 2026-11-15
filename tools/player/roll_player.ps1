# iter-465 player lane: ONE background PORT FEATURE roll of the combined player build
# (natural=false): tools/scenarios/player_wholerun.json through tools/sr_drive.ps1 (which follows
# the RESTART NOW relaunch), with tools/controls/harness_play.py driving the gameplay half
# (tools/player/gameplay_play.txt) on the same harness channel. Holds the game slot for the roll
# and releases it on every path. Writes everything under -Out.
#
#   powershell -File tools/player/roll_player.ps1 -ExePath <snapshot exe> -Out <empty dir>
#       [-Gates "SR_RES_SCALE=3,SR_WINDOW_SIZE=3840x2160"] [-DumpEvery 120] [-TimeoutSeconds 1500]
#
# The base gates make it a PORT FEATURE roll: SR_INSTANCE_ID=player (private log + channel,
# background window, real input ignored) + SR_PORT_FEATURES=1 (kernel/host_automation.hpp
# portFeaturesFor: every port feature at its player default). -Gates adds/overrides
# (display size, scale, fps target). Frames: sparse present dumps every -DumpEvery presents,
# taken after the cursor overlay (SR_PRESENT_DUMP_WITH_CURSOR=1).
param(
  [Parameter(Mandatory = $true)] [string] $ExePath,
  [Parameter(Mandatory = $true)] [string] $Out,
  [string] $Gates = "",
  [int] $DumpEvery = 120,
  [int] $DumpCap = 1500,
  [int] $TimeoutSeconds = 1500,
  [string] $Scenario = "",
  [string] $PlayScript = "",
  [string] $Owner = "player",
  [string] $ShaderRuntimeDir = "",
  [switch] $SlotHeld   # the caller holds the game slot for a batch (no acquire / release here)
)
$ErrorActionPreference = "Stop"
$root = (Join-Path $PSScriptRoot "..\.." | Resolve-Path).Path
if (-not $Scenario) { $Scenario = Join-Path $root "tools\scenarios\player_wholerun.json" }
if (-not $PlayScript) { $PlayScript = Join-Path $root "tools\player\gameplay_play.txt" }
# The game slot is the MAIN checkout's (tools/game_slot.py: SR_MAIN_ROOT, else the main checkout git knows), and the
# interpreter is SR_PYTHON when set (tools/sr_artifacts.ps1's rule): a second machine (the Ally, 2026-10-16) names
# its own main checkout and Python without editing this script. Unset = the main checkout git knows.
$main = if ($env:SR_MAIN_ROOT) { $env:SR_MAIN_ROOT } else { Split-Path -Parent (git -C $PSScriptRoot rev-parse --path-format=absolute --git-common-dir) }
$slotTool = Join-Path $main "tools\game_slot.py"
$python = if ($env:SR_PYTHON) { $env:SR_PYTHON } else { "python" }
if (Test-Path -LiteralPath $Out) { throw "roll_player: -Out exists: $Out (every roll gets a fresh folder)" }
New-Item -ItemType Directory -Path $Out | Out-Null
$instance = "player"
$baseGates = "SR_INSTANCE_ID=$instance,SR_PORT_FEATURES=1,SDL_AUDIODRIVER=disk,SDL_AUDIO_DRIVER=disk,SDL_DISKAUDIOFILE=NUL,SDL_AUDIO_DISK_OUTPUT_FILE=NUL,SR_HIGHFPS_PACE=1,SR_PM4_GPU_TIME=1," +
             "SR_PRESENT_DUMP_SPARSE_EVERY=$DumpEvery,SR_PRESENT_DUMP_SPARSE_CAP=$DumpCap"
if ($ShaderRuntimeDir) { $baseGates += ",SR_SHADER_RUNTIME_DIR=$ShaderRuntimeDir" }
$allGates = $baseGates + $(if ($Gates) { "," + $Gates } else { "" })
$childLog = Join-Path $root ("superman_returns_{0}_r1.log" -f $instance)
$meta = [ordered]@{ started = (Get-Date -Format o); exe = $ExePath
                    exe_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $ExePath).Hash.ToLower()
                    scenario = $Scenario; play = $PlayScript; gates = $allGates; natural = $false
                    tag = "PORT FEATURE roll (SR_PORT_FEATURES=1)" }
[IO.File]::WriteAllText((Join-Path $Out "roll.json"), ($meta | ConvertTo-Json -Depth 4))

$acquired = $false
$play = $null
try {
  if (-not $SlotHeld) {
    # the free-RAM floor is game_slot.py's alone: SR_SLOT_MIN_FREE_GB when set (validated there: a non-finite or
    # negative value is an error, never a floor turned off), else 8 as before - so no --min-free-gb here
    & $python $slotTool acquire --owner $Owner --note "player lane whole-run PORT FEATURE roll" --poll 10
    if ($LASTEXITCODE -ne 0) { throw "game slot not acquired" }
    $acquired = $true
  }
  # A stale child log from an earlier roll must not feed harness_play (sr_drive removes it too).
  if (Test-Path -LiteralPath $childLog) { Remove-Item -LiteralPath $childLog -Force }
  # harness_play waits for the child's HUD line; it opens the channel once a game stamped it.
  $playOut = Join-Path $Out "play.jsonl"
  $playArgs = @((Join-Path $root "tools\controls\harness_play.py"), "--instance", $instance, "--log", $childLog,
                "--script", $PlayScript, "--out", $playOut)
  $play = Start-Process -FilePath $python -ArgumentList $playArgs -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $Out "play_stdout.txt") -RedirectStandardError (Join-Path $Out "play_stderr.txt")
  $drive = Join-Path $Out "drive"
  & powershell -NoProfile -File (Join-Path $root "tools\sr_drive.ps1") -Scenario $Scenario -Out $drive `
      -ExePath $ExePath -TimeoutSeconds $TimeoutSeconds -Gates $allGates *> (Join-Path $Out "sr_drive.txt")
  $rc = $LASTEXITCODE
  $meta.sr_drive_exit = $rc
} finally {
  if ($play -and -not $play.HasExited) { try { Stop-Process -Id $play.Id -Force } catch {} }
  # sr_drive stops the process it drives (the child after followRestart); a child left behind
  # by a failed follow is still ours: stop it by its exact path, never by name alone.
  Get-CimInstance Win32_Process -Filter "Name='SupermanReturns.exe'" | Where-Object {
    $_.ExecutablePath -and ([IO.Path]::GetFullPath($_.ExecutablePath) -ieq [IO.Path]::GetFullPath($ExePath))
  } | ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force } catch {} }
  if ($acquired) { & $python $slotTool release --owner $Owner }
  $meta.ended = (Get-Date -Format o)
  [IO.File]::WriteAllText((Join-Path $Out "roll.json"), ($meta | ConvertTo-Json -Depth 4))
}
