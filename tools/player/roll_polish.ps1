# iter-465 polish lane: ONE background PORT FEATURE roll of tools/scenarios/polish_display_glyphs.json
# (natural=false) with tools/glyphs/vpad_hotplug.py --plan polish beside it (a ViGEm DualShock 4:
# system-wide, so only while holding the game slot). Holds the game slot for the roll and
# releases it on every path; stops only the processes it started.
#
#   powershell -File tools/player/roll_polish.ps1 -ExePath <snapshot exe> -Out <empty dir> [-Gates "..."]
param(
  [Parameter(Mandatory = $true)] [string] $ExePath,
  [Parameter(Mandatory = $true)] [string] $Out,
  [string] $Gates = "",
  [string] $Scenario = "",
  [string] $VpadPlan = "polish",
  [int] $TimeoutSeconds = 1800,
  [switch] $SlotHeld   # the caller holds the game slot (no acquire / release here)
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
$root = (Join-Path $PSScriptRoot "..\.." | Resolve-Path).Path
if (-not $Scenario) { $Scenario = Join-Path $root "tools\scenarios\polish_display_glyphs.json" }
$slotTool = Join-Path $PSScriptRoot "..\game_slot.py"   # it finds the main checkout's lock itself
if (Test-Path -LiteralPath $Out) { throw "roll_polish: -Out exists: $Out" }
New-Item -ItemType Directory -Path $Out | Out-Null
$instance = "polish"
$allGates = "SR_INSTANCE_ID=$instance,SR_PORT_FEATURES=1,SR_HARNESS_REAL_INPUT=1,SDL_AUDIODRIVER=disk,SDL_AUDIO_DRIVER=disk,SDL_DISKAUDIOFILE=NUL,SDL_AUDIO_DISK_OUTPUT_FILE=NUL" +
            $(if ($Gates) { "," + $Gates } else { "" })
$parentLog = Join-Path $root ("superman_returns_{0}.log" -f $instance)
$meta = [ordered]@{ started = (Get-Date -Format o); exe = $ExePath
                    exe_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $ExePath).Hash.ToLower()
                    scenario = $Scenario; vpad = $VpadPlan; gates = $allGates; natural = $false
                    tag = "PORT FEATURE roll (SR_PORT_FEATURES=1)" }
[IO.File]::WriteAllText((Join-Path $Out "roll.json"), ($meta | ConvertTo-Json -Depth 4))
$acquired = $false
$vpad = $null
try {
  if (-not $SlotHeld) {
    & python $slotTool acquire --owner polish --note "polish lane: display options + glyph hot-swap roll" --min-free-gb 8 --poll 3
    if ($LASTEXITCODE -ne 0) { throw "game slot not acquired" }
    $acquired = $true
  }
  Assert-SlotOwner "polish"
  if (Test-Path -LiteralPath $parentLog) { Remove-Item -LiteralPath $parentLog -Force }
  if ($VpadPlan) {
    $vpadArgs = @((Join-Path $root "tools\glyphs\vpad_hotplug.py"), "--plan", $VpadPlan, "--log", $parentLog,
                  "--mark", (Join-Path $Out "marks.txt"))
    $vpad = Start-Process -FilePath "python" -ArgumentList $vpadArgs -WindowStyle Hidden -PassThru `
              -RedirectStandardOutput (Join-Path $Out "vpad_stdout.txt") -RedirectStandardError (Join-Path $Out "vpad_stderr.txt")
  }
  & powershell -NoProfile -File (Join-Path $root "tools\sr_drive.ps1") -Scenario $Scenario -Out (Join-Path $Out "drive") `
      -ExePath $ExePath -TimeoutSeconds $TimeoutSeconds -Gates $allGates *> (Join-Path $Out "sr_drive.txt")
  $meta.sr_drive_exit = $LASTEXITCODE
} finally {
  if ($vpad -and -not $vpad.HasExited) { try { Stop-Process -Id $vpad.Id -Force } catch {} }
  Get-CimInstance Win32_Process -Filter "Name='SupermanReturns.exe'" | Where-Object {
    $_.ExecutablePath -and ([IO.Path]::GetFullPath($_.ExecutablePath) -ieq [IO.Path]::GetFullPath($ExePath))
  } | ForEach-Object { try { Stop-Process -Id $_.ProcessId -Force } catch {} }
  if ($acquired) { & python $slotTool release --owner polish }
  $meta.ended = (Get-Date -Format o)
  [IO.File]::WriteAllText((Join-Path $Out "roll.json"), ($meta | ConvertTo-Json -Depth 4))
}
