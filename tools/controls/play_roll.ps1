# tools/controls/play_roll.ps1 - one roll of a (worktree) build driven by tools/controls/harness_play.py:
# the harness keyboard channel (player or test keymap), the mouse-cursor feed and ViGEm virtual pads.
# Promoted from the iter-465 menu-keys roll (archive/v1_menukeys/roll.ps1); iter-466 controls2.
#
#   python tools/game_slot.py acquire --owner <owner>      # one game process at a time
#   .\tools\controls\play_roll.ps1 -Owner <owner> -Tag <t> -ExePath <worktree>\build\SupermanReturns.exe `
#       -Script tools\controls\scripts\<name>.play -OutRoot archive\v1_<lane>
#   python tools/game_slot.py release --owner <owner>
#
# Always on: SR_PORT_FEATURES=1 (the player features the scripts exercise), SR_HARNESS_VPAD=1 (only
# ViGEm pads are opened - never the owner's controller), the cursor feed + its log, the action-layer
# trace (<out>\actions.jsonl), sparse presented frames (<out>\frames). The game runs in the
# background (run_recomp_x / SR_INSTANCE_ID); real keyboard / mouse never reach it.
param(
    [Parameter(Mandatory)] [string] $Owner,
    [Parameter(Mandatory)] [string] $Tag,
    [Parameter(Mandatory)] [string] $ExePath,
    [Parameter(Mandatory)] [string] $Script,
    [string] $OutRoot = "archive\v1_controls2",
    [string] $Gates = "",
    [int] $TimeoutSeconds = 900
)
$ErrorActionPreference = "Stop"
$root = if ($env:SR_MAIN_ROOT) { $env:SR_MAIN_ROOT } else { Split-Path -Parent (git -C $PSScriptRoot rev-parse --path-format=absolute --git-common-dir) }
$st = (python (Join-Path $root "tools\game_slot.py") status) -join "`n"
if ($st -notmatch ('lock: \{"owner": "' + [regex]::Escape($Owner) + '"')) { Write-Error "$Owner does not hold the game slot"; exit 2 }
$out = Join-Path $root (Join-Path $OutRoot $Tag)
if (Test-Path $out) { Write-Error "$out exists"; exit 1 }
New-Item -ItemType Directory -Force $out | Out-Null
$frames = Join-Path $out "frames"
$feed = Join-Path $out "mouse_feed.txt"
New-Item -ItemType File -Force $feed | Out-Null
$trace = Join-Path $out "actions.jsonl"
$g = "SR_PORT_FEATURES=1,SR_HARNESS_VPAD=1,SR_MOUSE_FEED=$feed,SR_MOUSE_UI_LOG=1,SR_PC_ACTIONS_TRACE=$trace," +
     "SR_PRESENT_DUMP=1,SR_PRESENT_DUMP_DIR=$frames,SR_PRESENT_DUMP_SPARSE_EVERY=20,SR_PRESENT_DUMP_SPARSE_CAP=1500"
if ($Gates) { $g = "$g,$Gates" }
Set-Location $root
$log = Join-Path $root "superman_returns_run0.log"
$runOut = Join-Path $out "run_recomp_x.txt"
$job = Start-Process -FilePath "powershell.exe" -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput $runOut -RedirectStandardError (Join-Path $out "run_recomp_x.err") `
    -ArgumentList @("-NoProfile", "-File", "tools\run_recomp_x.ps1", "-K", "1", "-TimeoutSeconds", "$TimeoutSeconds",
                    "-Gates", "`"$g`"", "-ExePath", "`"$ExePath`"")
$game = $null
for ($i = 0; $i -lt 120 -and -not $game; $i++) {
    Start-Sleep -Milliseconds 500
    $game = Get-Process -Name SupermanReturns -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $ExePath } | Select-Object -First 1
}
if (-not $game) { Write-Error "game did not start"; exit 1 }
"game pid $($game.Id)" | Set-Content (Join-Path $out "pids.txt")
Start-Sleep -Seconds 2
$hp = Join-Path (Split-Path $PSScriptRoot -Parent) "controls\harness_play.py"
$play = Start-Process -FilePath "python" -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $out "play.out") -RedirectStandardError (Join-Path $out "play.err") `
    -ArgumentList @("`"$hp`"", "--instance", "run0", "--log", "`"$log`"", "--mouse-feed", "`"$feed`"",
                    "--script", "`"$(Resolve-Path $Script)`"", "--out", "`"$(Join-Path $out 'play.jsonl')`"")
# heartbeat the slot while the roll runs; stop the game 20 s after the script finished
$doneAt = $null
while (-not $job.HasExited) {
    Start-Sleep -Seconds 15
    python (Join-Path $root "tools\game_slot.py") heartbeat --owner $Owner | Out-Null
    if ($play.HasExited -and -not $doneAt) { $doneAt = (Get-Date).AddSeconds(20) }
    if ($doneAt -and (Get-Date) -gt $doneAt) {
        $g2 = Get-Process -Id $game.Id -ErrorAction SilentlyContinue
        if ($g2) { Stop-Process -Id $game.Id -Force }
        $doneAt = (Get-Date).AddYears(1)
    }
}
if (-not $play.HasExited) { try { Stop-Process -Id $play.Id -Force } catch {} }
Copy-Item $log (Join-Path $out "run.log") -ErrorAction SilentlyContinue
"done: $out"
