# iter-465 polish lane: a PLAYER launch must never kill another process (docs/PLAYER_BUILD.md 7).
# Holds the game slot, then:
#   1. starts a background "roll" (SR_INSTANCE_ID=instance_bg) - the process a lane's roll is;
#   2. starts player launch P1 (no SR_INSTANCE_ID);
#   3. checks the roll is still alive;
#   4. starts player launch P2: it must exit 0 on its own, naming P1 ("already running"),
#      while the roll and P1 keep running.
# The player launches are made background-safe for this test only (SR_WINDOW_BACKGROUND=1: no
# focus steal, which also skips P2's focus switch; SR_HARNESS_REAL_INPUT=0; windowed;
# SR_PORT_FEATURES=0; a scratch save root and working folder - never the owner's).
# Stops every process it started, by PID; releases the slot on every path.
#
#   powershell -File tools/player/instance_test.ps1 -ExePath <exe> -Out <empty dir>
param(
  [Parameter(Mandatory = $true)] [string] $ExePath,
  [Parameter(Mandatory = $true)] [string] $Out,
  [int] $SettleSeconds = 25,
  [string] $GameDir = "",   # working folder holding "Superman Returns (USA)" (default: this repo)
  [switch] $SlotHeld
)
$ErrorActionPreference = "Stop"

# Never launch a game unless the game slot's lock is OURS right now (a -SlotHeld caller's hold
# included): 2026-10-03 A5 ran while another lane held the lock.
function Assert-SlotOwner([string] $owner) {
  $st = (& python (Join-Path $PSScriptRoot "..\game_slot.py") status 2>&1) -join "`n"
  if ($st -notmatch ('(?m)^lock: \{"owner": "' + [regex]::Escape($owner) + '"')) {   # the LOCK line, not "next:"
    throw "game slot is not held by $owner - refusing to launch a game:`n$st"
  }
}
if (-not $GameDir) { $GameDir = (Join-Path $PSScriptRoot "..\.." | Resolve-Path).Path }
$slotTool = Join-Path $PSScriptRoot "..\game_slot.py"   # it finds the main checkout's lock itself
if (Test-Path -LiteralPath $Out) { throw "instance_test: -Out exists: $Out" }
New-Item -ItemType Directory -Path $Out | Out-Null
$started = @()
$acquired = $false
$result = [ordered]@{ exe = $ExePath; started = (Get-Date -Format o) }

function Start-Instance([string] $name, [hashtable] $vars) {
  Assert-SlotOwner "polish"
  $dir = Join-Path $Out $name
  New-Item -ItemType Directory -Path $dir | Out-Null
  $keys = @('SR_INSTANCE_ID', 'SR_PORT_FEATURES', 'SR_WINDOW_BACKGROUND', 'SR_HARNESS_REAL_INPUT',
            'SR_DISPLAY_MODE', 'SR_CONTENT_DIR', 'SDL_AUDIODRIVER', 'SDL_AUDIO_DRIVER', 'SDL_DISKAUDIOFILE',
            'SDL_AUDIO_DISK_OUTPUT_FILE', 'SR_RESTART_PARENT_PID')
  $saved = @{}
  foreach ($k in $keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k, 'Process')
                          [Environment]::SetEnvironmentVariable($k, $null, 'Process') }
  foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, [string]$vars[$k], 'Process') }
  try {
    # stdout/stderr go to files (drained by the OS; the [INSTANCE] line lands in stderr.txt)
    # The game finds its disc folder in the working directory: every instance runs in $GameDir
    # (the roll logs superman_returns_instance_bg.log, a player superman_returns.log there;
    # both are moved into -Out afterwards).
    $p = Start-Process -FilePath $ExePath -WorkingDirectory $GameDir -PassThru -NoNewWindow `
           -RedirectStandardOutput (Join-Path $dir 'stdout.txt') -RedirectStandardError (Join-Path $dir 'stderr.txt')
    $null = $p.Handle   # cache the handle so ExitCode is readable after exit
    return $p
  } finally {
    foreach ($k in $keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') }
  }
}

try {
  if (-not $SlotHeld) {
    & python $slotTool acquire --owner polish --note "polish lane: player-launch instance test" --min-free-gb 8 --poll 3
    if ($LASTEXITCODE -ne 0) { throw "game slot not acquired" }
    $acquired = $true
  }
  $playerEnv = @{ SR_WINDOW_BACKGROUND = '1'; SR_HARNESS_REAL_INPUT = '0'; SR_DISPLAY_MODE = 'windowed'
                  SR_PORT_FEATURES = '0'; SDL_AUDIODRIVER = 'disk'; SDL_AUDIO_DRIVER = 'disk'; SDL_DISKAUDIOFILE = 'NUL'
                  SDL_AUDIO_DISK_OUTPUT_FILE = 'NUL'; SR_CONTENT_DIR = (Join-Path $Out 'content') }
  New-Item -ItemType Directory -Path (Join-Path $Out 'content') | Out-Null
  $roll = Start-Instance 'roll' @{ SR_INSTANCE_ID = 'instance_bg'; SDL_AUDIODRIVER = 'disk'; SDL_AUDIO_DRIVER = 'disk'
                                   SDL_DISKAUDIOFILE = 'NUL'; SDL_AUDIO_DISK_OUTPUT_FILE = 'NUL' }
  $started += $roll
  Start-Sleep -Seconds $SettleSeconds
  $result.roll_alive_before_p1 = -not $roll.HasExited
  if ($roll.HasExited) { $result.roll_exit_code = $roll.ExitCode }
  $p1 = Start-Instance 'p1' $playerEnv
  $started += $p1
  Start-Sleep -Seconds $SettleSeconds
  $result.roll_alive_after_p1 = -not $roll.HasExited
  $result.p1_alive = -not $p1.HasExited
  $p2 = Start-Instance 'p2' $playerEnv
  $started += $p2
  $p2Exited = $p2.WaitForExit(20000)
  $result.p2_exited = $p2Exited
  $result.p2_exit_code = if ($p2Exited) { $p2.ExitCode } else { $null }
  Start-Sleep -Seconds 3
  $result.roll_alive_after_p2 = -not $roll.HasExited
  $result.p1_alive_after_p2 = -not $p1.HasExited
  $result.pids = @{ roll = $roll.Id; p1 = $p1.Id; p2 = $p2.Id }
} finally {
  foreach ($p in $started) { try { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } } catch {} }
  if ($acquired) { & python $slotTool release --owner polish }
  $result.ended = (Get-Date -Format o)
  $result.pass = [bool]($result.roll_alive_after_p1 -and $result.p1_alive -and $result.p2_exited -and
                        $result.p2_exit_code -eq 0 -and $result.roll_alive_after_p2 -and $result.p1_alive_after_p2)
  [IO.File]::WriteAllText((Join-Path $Out "result.json"), ($result | ConvertTo-Json -Depth 4))
  Start-Sleep -Seconds 2
  foreach ($lf in @('superman_returns.log', 'superman_returns_instance_bg.log')) {
    $src = Join-Path $GameDir $lf
    if (Test-Path -LiteralPath $src) { Move-Item -LiteralPath $src -Destination (Join-Path $Out $lf) -Force -ErrorAction SilentlyContinue }
  }
  $result | ConvertTo-Json -Depth 4
}
