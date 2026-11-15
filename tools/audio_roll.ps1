# audio_roll.ps1 - iter-467 sound lane: one SILENT audio-capture roll of our build (sr_drive).
#
# The game's audio goes to the null sink (SR_AUDIO_SINK=null: a wall-clock consumer at the console
# rate - nothing reaches the owner's speakers, and unlike the SDL disk driver it keeps real time),
# every submitted frame to <Out>/ours.afr (SR_AUDIO_FRAMES, the same capture point and format as
# the Xenia oracle side, tools/xenia_audio_capture.py) and what the sink played to <Out>/ours.wav.
# Compare with: python tools/audio_oracle_compare.py compare --oracle <x>/xenia.afr --ours <Out>/ours.afr
#
# Hold the game slot (tools/game_slot.py acquire --owner audio) around the call. No ViGEm pad from
# tools/xenia_pad*.py may be alive: SR_HARNESS_VPAD=1 makes the game open ViGEm pads only.
#
#   powershell -File tools/audio_roll.ps1 -Out archive/v1_audio3/o1 [-Scenario tools/scenarios/audio_tutorial.json]
#       [-Tape <main checkout>/archive/v1_tape3/tutorial.inpr] [-Gates "SR_HIGHFPS=uncapped"]
#       [-ExePath <exe>] [-TimeoutSeconds 1500]
param(
    [Parameter(Mandatory=$true)][string] $Out,
    [string] $Scenario = "tools/scenarios/audio_tutorial.json",
    [string] $Tape = "",    # default <main checkout>/archive/v1_tape3/tutorial.inpr ("none" = no tape)
    [string] $Gates = "",
    [string] $ExePath = "",
    [string] $Instance = "audio3",
    [string] $Sink = "null",          # null (wall-clock consumer) | sdl (needs -Device: a SILENT endpoint)
    [string] $Device = "",            # SR_AUDIO_DEVICE substring, e.g. "Steam Streaming Speakers"
    [int] $TimeoutSeconds = 1500
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$outAbs = [System.IO.Path]::GetFullPath((Join-Path $root $Out))
if (Test-Path $outAbs) { Write-Error "audio_roll: $outAbs exists (use a fresh -Out)"; exit 2 }
$vg = Get-CimInstance Win32_Process -Filter "Name='python.exe' OR Name='py.exe'" |
      Where-Object { $_.CommandLine -match 'xenia_pad' }
if ($vg) { Write-Error ("audio_roll: a ViGEm pad tool is running (pid {0}) - stop it first" -f ($vg.ProcessId -join ',')); exit 2 }
$capDir = $outAbs + "_cap"
New-Item -ItemType Directory -Force $capDir | Out-Null
if ($Sink -eq "sdl" -and -not $Device) { Write-Error "audio_roll: -Sink sdl needs -Device <silent endpoint> (never the owner's speakers)"; exit 2 }
$g = @("SR_INSTANCE_ID=$Instance", "SR_HARNESS_VPAD=1", "SR_AUDIO_SINK=$Sink",
       "SR_AUDIO_FRAMES=$capDir\ours.afr", "SR_AUDIO_WAV=$capDir\ours.wav")
if ($Device) { $g += "SR_AUDIO_DEVICE=$Device" }
if (-not $Tape) { $Tape = Join-Path $(if ($env:SR_MAIN_ROOT) { $env:SR_MAIN_ROOT } else { Split-Path -Parent (git -C $PSScriptRoot rev-parse --path-format=absolute --git-common-dir) }) "archive/v1_tape3/tutorial.inpr" }
if ($Tape -and $Tape -ne 'none') { $g += "SR_INPUT_REPLAY=$Tape" }
if ($Gates) { $g += $Gates }
$gs = $g -join ","
$dargs = @("-NoProfile", "-File", "tools/sr_drive.ps1", "-Scenario", $Scenario, "-Out", $Out,
          "-TimeoutSeconds", "$TimeoutSeconds", "-Gates", $gs)
if ($ExePath) { $dargs += @("-ExePath", $ExePath) }
"audio_roll: out=$Out cap=$capDir gates=$gs" | Out-File -Encoding ascii "$capDir\roll.txt"
(Get-Date -Format "yyyy-MM-dd HH:mm:ss") + " start" | Add-Content -Encoding ascii "$capDir\roll.txt"
& powershell @dargs 2>&1 | Tee-Object -FilePath "$capDir\drive_stdout.txt"
$rc = $LASTEXITCODE
(Get-Date -Format "yyyy-MM-dd HH:mm:ss") + " end rc=$rc" | Add-Content -Encoding ascii "$capDir\roll.txt"
exit $rc
