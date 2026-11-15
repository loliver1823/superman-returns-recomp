# iter-442 S5 - the HLE command-trace fixture (docs/MIGRATION_API_HLE.md).
#
# THE POINT: S4.4-S4.7 (plane upload, PSO, RT binding, vertex data, present
# copy) are five independent failure points that ALL fail to an identical black
# screen. Against 200 s boots that is 5+ rolls each reporting only "still
# black". This replays a recorded command stream - with its real plane pixels -
# through the SAME hle::execute() the render thread runs, in ~100 ms.
#
#   .\tools\hle_replay.ps1 -Record                 # capture from a live roll
#   .\tools\hle_replay.ps1                         # one-shot batch check
#   .\tools\hle_replay.ps1 -Loops 0                # loop on screen until closed
#
# Traces live in archive/ (gitignored evidence ground), never the repo root.
#
# ASCII ONLY IN THIS FILE. PowerShell 5.1 reads a BOM-less .ps1 as cp1252, so a
# UTF-8 em dash decodes to byte 0x94 = a smart quote, which PS accepts as a
# STRING DELIMITER - the script then fails with a bogus "Missing closing '}'"
# pointing at an innocent line. (Cost one debug cycle here; same cp1252 class
# recorded in project_tooling_confidence.)
param(
    [switch] $Record,           # capture a fresh trace instead of replaying
    # iter-445 C1: PINNED to the era-current trace. s5_movie.srht is from the
    # iter-442 era and PREDATES SetVertexDecl / SetConstants / SetRenderState,
    # so replaying it exercises an older executor than the one that ships -
    # every A/B taken on it was measuring the wrong program. s6i.srht is 60
    # presents of the current movie era; s6i_geom.srht is 40 presents of the
    # geometry era (SR_HLE_TRACE_ARM_GEOM).
    [string] $Trace   = "archive/s6r5.srht",
    [int]    $Frames  = 8,      # presents to capture when recording
    [int]    $Loops   = 1,      # 0 = loop forever (visual), N = N passes
    [int]    $Seconds = 60,     # recording roll length
    [string] $Gates   = ""      # extra NAME=VALUE pairs, comma-separated
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if ($Record) {
    # The recorder ARMS on movie activity and starts on a Present boundary, so a
    # no-movie roll produces no trace at all rather than a useless empty one.
    $g = "SR_HLE_TRACE_REC=$Trace,SR_HLE_TRACE_FRAMES=$Frames"
    if ($Gates) { $g = "$g,$Gates" }
    Write-Host "[S5] recording $Frames presents -> $Trace ($Seconds s roll)"
    & "$root\tools\run_recomp_x.ps1" -K 1 -TimeoutSeconds $Seconds -Gates $g
    if (-not (Test-Path $Trace)) {
        Write-Warning "[S5] no trace written - the roll most likely had NO MOVIE (check the activity gate table above). Re-roll."
        exit 1
    }
    Write-Host ("[S5] trace: {0} bytes" -f (Get-Item $Trace).Length)
    exit 0
}

if (-not (Test-Path $Trace)) {
    Write-Error "[S5] no trace at $Trace - run with -Record first."
}

$env:SR_HLE_TRACE_REPLAY = $Trace
$env:SR_INSTANCE_ID      = "s5replay"
$env:SR_LOG_BOOT         = "1"
if ($Loops -gt 0) { $env:SR_HLE_TRACE_LOOPS = "$Loops" }
else { Remove-Item Env:SR_HLE_TRACE_LOOPS -ErrorAction SilentlyContinue }

# iter-445 C2 BUG FIX: -Gates was applied ONLY in the -Record arm; the replay
# arm silently ignored it. Every gated A/B run through this fixture was
# therefore running with the gate OFF and comparing a build against itself -
# a fixture that cannot fail is worse than no fixture. Caught on its first use
# by the ledger's own dbgLayer field reading "off" when SR_D3D12_DEBUG=1 was
# passed. Applied here, and ECHOED, so a typo in a gate name is visible.
$applied = @()
if ($Gates) {
    foreach ($pair in $Gates.Split(",")) {
        $kv = $pair.Split("=", 2)
        if ($kv.Length -eq 2 -and $kv[0].Trim()) {
            Set-Item -Path ("Env:" + $kv[0].Trim()) -Value $kv[1]
            $applied += ("{0}={1}" -f $kv[0].Trim(), $kv[1])
        }
    }
}
if ($applied.Count -gt 0) { Write-Host ("[S5] gates: " + ($applied -join " ")) }
else { Write-Host "[S5] gates: (none)" }

$sw = [Diagnostics.Stopwatch]::StartNew()
. (Join-Path $PSScriptRoot 'bg_window.ps1')   # iter-464 bgruns: no console window, window in the background
$p = Start-SrGameProcess -FilePath "$root\build\SupermanReturns.exe" -WorkingDirectory $root
if ($Loops -gt 0) { $p.WaitForExit(120000) | Out-Null } else { $p.WaitForExit() }
$sw.Stop()

$log = "$root\superman_returns_s5replay.log"
if (Test-Path $log) {
    Select-String -Path $log -Pattern "\[HLE_REPLAY\]" |
        ForEach-Object { $_.Line -replace '^.*\[BOOT\] ', '' }
}
Write-Host ("[S5] exit={0} elapsed={1:N1}s" -f $p.ExitCode, $sw.Elapsed.TotalSeconds)
# rc 2 = payload hash mismatch: the trace did NOT round-trip byte-exactly, so
# any pixel verdict taken from this run is meaningless.
if ($p.ExitCode -eq 2) {
    Write-Warning "[S5] PAYLOAD HASH MISMATCH - trace corrupt, do not judge pixels from it"
}
exit $p.ExitCode
