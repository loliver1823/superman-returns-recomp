# Replay the golden Xenia GPU trace through our renderer with a named env config,
# and bank the resulting frame as scratch_replay_<tag>.png.
#
# The replay is a ~2s, boot-free, deterministic render fixture (TOOLKIT 1.6) — use it
# for ALL PM4/D3D12 iteration instead of waiting on a deep boot roll.
#
#   .\tools\replay_ab.ps1 -Tag rtt -Gates "SR_XLAT_RTT=1"
#
# NOTE: -Gates is a plain string, not a hashtable — a hashtable is stringified
# when crossing a `powershell -File` boundary and arrives as unusable text.
param(
    [string]$Tag = "base",
    [string]$Gates = "",
    [string]$Trace = "data\gpu_traces\menu_pressstart_454107ED_9415.xtr",
    [int]$TimeoutSec = 180,
    # iter-466 gfx: another exe of this tree (e.g. a saved baseline build) for a before/after A/B
    [string]$ExePath = ".\build\SupermanReturns.exe",
    # Optional strict process receipt. A wrapper exit alone is not replay proof.
    [string]$ReceiptPath = ""
)
$ErrorActionPreference = "Continue"
# iter-464 resscale: the tree this script lives in (a lane worktree replays its own
# build/SupermanReturns.exe and writes its own scratch files), not a hardcoded root.
Set-Location (Split-Path -Parent $PSScriptRoot)
if ($ReceiptPath -and (Test-Path -LiteralPath $ReceiptPath)) {
    throw "Preserve prior replay receipt; choose a fresh path: $ReceiptPath"
}

# Clear any gate left over from a previous config so runs are independent.
foreach ($k in @("SR_XLAT_RTT", "SR_XLAT_FULLVP", "SR_RESOLVE_DUMP", "SR_GPU_TRACE_REPLAY_MAXFRAMES")) {
    Remove-Item "env:$k" -EA SilentlyContinue
}
# iter-445 C7 (audit blocker 6): the PM4 replay fixture owns the MAIN thread and
# has no hle::execute producer, so it needs no open window - and a default-ON
# frame list would make renderGuestGeometry dead code here while the geometry
# skip's getAndClearResolveRequested() makes the fixture believe our resolve
# landed, so it would dump the trace's OWN restored framebuffer as the result.
# That is the iter-359g false positive, reintroduced. Pin it OFF.
# iter-463: remember what each variable replaced so the finally-block below
# puts it back - two replays in one shell must not share gates (an A/B "base"
# run silently inherited the previous run's SR_SHADER_CACHE_DIR).
$savedEnv = @{}
foreach ($k in @("SR_HLE_FRAME_LIST", "SR_GPU_TRACE_REPLAY", "SR_INSTANCE_ID", "SR_DEVTOOLS")) {
    $savedEnv[$k] = [Environment]::GetEnvironmentVariable($k, "Process")
}
# iter-463 phase 2: a replay never enables a dev tool unless -Gates names one (docs/DEVTOOLS.md).
$env:SR_DEVTOOLS = "0"
$env:SR_HLE_FRAME_LIST = "0"
$env:SR_GPU_TRACE_REPLAY = $Trace
$env:SR_INSTANCE_ID = "replay$Tag"
foreach ($pair in ($Gates -split ',' | Where-Object { $_ -match '=' })) {
    $kv = $pair.Trim() -split '=', 2
    if (-not $savedEnv.ContainsKey($kv[0])) {
        $savedEnv[$kv[0]] = [Environment]::GetEnvironmentVariable($kv[0], "Process")
    }
    Set-Item "env:$($kv[0])" $kv[1]
}
try {

if (Test-Path replay_frame_1.bmp) { Remove-Item replay_frame_1.bmp -Force }
$t0 = Get-Date
. (Join-Path $PSScriptRoot 'bg_window.ps1')   # iter-464 bgruns: no console window, window in the background
$p = Start-SrGameProcess -FilePath $ExePath `
        -StdoutPath "scratch_replay_${Tag}_out.txt" -StderrPath "scratch_replay_${Tag}_err.txt"
$done = $p.WaitForExit($TimeoutSec * 1000)
$replayTimedOut = -not $done
if (-not $done) {
    Stop-Process -Id $p.Id -Force
    Write-Output "TIMEOUT after ${TimeoutSec}s"
    $done = $p.WaitForExit(10000)
}
Complete-SrGameProcessOutput $p
$replayChildExit = $null
if ($done) { $replayChildExit = $p.ExitCode }
$replayOutputsComplete = @($p.SrOutputCopies | Where-Object {
    -not $_.task.IsCompleted -or $_.task.IsFaulted -or $_.task.IsCanceled
}).Count -eq 0
if ($ReceiptPath) {
    [ordered]@{
        schema = 'sr-replay-process/1'
        pid = $p.Id
        exe = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ExePath)
        trace = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Trace)
        terminal_observed = $done
        timed_out = $replayTimedOut
        exit_code = $replayChildExit
        output_complete = $replayOutputsComplete
        frame_generated = [bool](Test-Path -LiteralPath replay_frame_1.bmp)
        stderr_sha256 = (Get-FileHash -LiteralPath "scratch_replay_${Tag}_err.txt" -Algorithm SHA256).Hash.ToLowerInvariant()
        stdout_sha256 = (Get-FileHash -LiteralPath "scratch_replay_${Tag}_out.txt" -Algorithm SHA256).Hash.ToLowerInvariant()
        scope = "Development replay; raw summary and presented frame need separate checks"
    } | ConvertTo-Json | Set-Content -LiteralPath $ReceiptPath -Encoding UTF8
}
$secs = [int]((Get-Date) - $t0).TotalSeconds

Write-Output "=== $Tag ($secs s) $Gates ==="
Select-String -Path "scratch_replay_${Tag}_err.txt" -Pattern "GPU:REPLAY\] SUMMARY" -EA SilentlyContinue |
    ForEach-Object { $_.Line -replace '.*\[GPU:REPLAY\] ', '' }

if (Test-Path replay_frame_1.bmp) {
    Copy-Item replay_frame_1.bmp "scratch_replay_$Tag.bmp" -Force
    # SR_PYTHON (tools/sr_artifacts.ps1's rule): a machine whose bare `python` is not the tools' interpreter (the
    # Ally's is the Microsoft Store alias) names it; unset = `python`, unchanged.
    $py = if ($env:SR_PYTHON) { $env:SR_PYTHON } else { 'python' }
    & $py -c @"
from PIL import Image
import collections, sys
a = Image.open('scratch_replay_$Tag.bmp').convert('RGB')
a.save('scratch_replay_$Tag.png')
n = a.size[0]*a.size[1]
c = collections.Counter(a.getdata())
white = c[(255,255,255)]
print('  frame %dx%d  pure-white %.1f%%  top: %s' % (a.size[0], a.size[1], 100.0*white/n,
      ', '.join('%s x%d' % (k, v) for k, v in c.most_common(4))))
"@
} else {
    Write-Output "  NO FRAME PRODUCED"
}
if ($ReceiptPath -and ($replayTimedOut -or -not $done -or $replayChildExit -ne 0 -or
    -not $replayOutputsComplete -or -not (Test-Path -LiteralPath replay_frame_1.bmp))) {
    throw "Replay incomplete; inspect receipt $ReceiptPath"
}
} finally {
    foreach ($k in $savedEnv.Keys) {
        [Environment]::SetEnvironmentVariable($k, $savedEnv[$k], "Process")
    }
}
