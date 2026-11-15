# sr_quit.ps1 - GRACEFUL QUIT for automated rolls (tools/graceful-quit; docs/TOOLKIT.md 1.8m).
#
# Asks a running game (an automated run: SR_INSTANCE_ID set) to quit through its quit channel
# (kernel/quit.hpp): two manual-reset named events the GAME creates at boot,
#     Local\SR_QUIT_<id>_<pid>      request (the harness sets it)
#     Local\SR_QUIT_ACK_<id>_<pid>  ack     (the game sets it when it takes the request)
# <id> = SR_INSTANCE_ID sanitised to [A-Za-z0-9_.-] (at most 64, "default" when empty) and <pid> = the
# game's PID, so a harness can only ever signal the process it launched. On the request the game
# drains its command processor, parks its producers, prints every counter family once more with the
# why token "terminal", prints "[QUIT] terminal snapshot complete frames=N ..." and exits 0 (81 = it
# could not quiesce and printed a FAILED line instead; 82 = its own hard deadline).
#
# The verdict on the COUNTERS is the log's (tools/perf/terminal_snapshot.py). This file runs the
# protocol and always leaves the process ended: a step that fails falls back to Stop-Process -Force,
# exactly the old ending. Results (Invoke-SrGracefulQuit .result):
#   complete           ack, then exit 0 (the game printed its terminal snapshot)
#   failed             ack, then exit 81 (the game printed "[QUIT] terminal snapshot FAILED ...")
#   deadline           ack, then exit 82 (the game's hard deadline ended it)
#   exit-other         ack, then any other exit code (a crash during the quit)
#   no-channel         the request event does not exist (an exe older than the channel, SR_QUIT_CHANNEL=0,
#                      a non-automated run): Stop-Process at once
#   no-ack             no ack within -AckTimeoutMs: Stop-Process
#   exited-without-ack the game exited on its own without taking the request (it was already exiting)
#   exit-timeout       ack, but no exit within -ExitTimeoutMs: Stop-Process
#   already-exited     the game had exited before the request
#   bad-args           a timeout outside [1, 600000] ms (-1 would wait forever) or no process id: no request,
#                      Stop-Process
#   harness-error      anything this side threw (a probe, an event, the runtime): Stop-Process, the error recorded
#
# A receipt is written for EVERY ending (Write-SrQuitRecord <attempt>\quit.json); the graders count a terminal
# snapshot as complete only with result complete, graceful, no fallback and exit code 0
# (tools/perf/terminal_snapshot.py).
#
# Functions only (dot-source it). ASCII only (PowerShell 5.1 reads a BOM-less .ps1 as cp1252).
# Tests: tools/test_sr_quit.ps1 (simulated probes + the live Win32 events against a stand-in process).

$script:SR_QUIT_EXIT_COMPLETE = 0
$script:SR_QUIT_EXIT_FAILED   = 81
$script:SR_QUIT_EXIT_DEADLINE = 82

# Mirrors kernel/quit.cpp sanitizeInstance (and the harness keyboard channel's rule).
function Get-SrQuitInstanceName([string]$Instance) {
    $id = if ($Instance) { $Instance } else { 'default' }
    $id = [regex]::Replace($id, '[^A-Za-z0-9_.-]', '_')
    if ($id.Length -gt 64) { $id = $id.Substring(0, 64) }
    return $id
}

# Mirrors kernel/quit.cpp requestEventName / ackEventName.
function Get-SrQuitEventNames([string]$Instance, [int]$ProcessId) {
    $id = Get-SrQuitInstanceName $Instance
    return @{
        request = ('Local\SR_QUIT_{0}_{1}' -f $id, $ProcessId)
        ack     = ('Local\SR_QUIT_ACK_{0}_{1}' -f $id, $ProcessId)
    }
}

# Opens one named event, or returns $null when it does not exist (never creates one: an event this
# side created would be signalled into the void).
function Open-SrQuitEvent([string]$Name, [System.Security.AccessControl.EventWaitHandleRights]$Rights) {
    $h = $null
    try {
        if ([System.Threading.EventWaitHandle]::TryOpenExisting($Name, $Rights, [ref]$h)) { return $h }
    } catch {}
    return $null
}

# The production probes for one game process (a System.Diagnostics.Process whose .Handle was touched
# right after launch, so ExitCode survives the exit under Windows PowerShell 5.1).
function New-SrQuitProbes($Proc) {
    $p = $Proc
    return @{
        HasExited  = { [bool]$p.HasExited }.GetNewClosure()
        ExitCode   = { if ($p.HasExited) { $p.ExitCode } else { $null } }.GetNewClosure()
        # Both events, opened BEFORE the request is set: an exited game's events vanish with it.
        OpenEvents = {
            param($names)
            $mod = [System.Security.AccessControl.EventWaitHandleRights]'Modify, Synchronize'
            $syn = [System.Security.AccessControl.EventWaitHandleRights]'Synchronize'
            $req = Open-SrQuitEvent $names.request $mod
            if (-not $req) { return $null }
            $ack = Open-SrQuitEvent $names.ack $syn
            if (-not $ack) { $req.Dispose(); return $null }
            return @{ request = $req; ack = $ack }
        }
        Signal      = { param($ev) [void]$ev.request.Set() }
        WaitAck     = { param($ev, [int]$ms) return [bool]$ev.ack.WaitOne($ms) }
        WaitExit    = { param([int]$ms) return [bool]$p.WaitForExit($ms) }.GetNewClosure()
        Kill        = {
            try { Stop-Process -Id $p.Id -Force -ErrorAction Stop } catch {}
            try { [void]$p.WaitForExit(5000) } catch {}
        }.GetNewClosure()
        CloseEvents = { param($ev) if ($ev) { foreach ($k in @('request', 'ack')) { if ($ev[$k]) { try { $ev[$k].Dispose() } catch {} } } } }
        NowMs       = { [int64]([DateTime]::UtcNow - [DateTime]'1970-01-01').TotalMilliseconds }
    }
}

$script:SR_QUIT_MAX_MS = 600000

# The timeouts' accepted range: whole milliseconds in [1, 600000]. A negative value (-1 = infinite to
# WaitOne / WaitForExit), 0 or anything larger is rejected.
function Test-SrQuitTimeout($Ms) {
    $v = 0L
    if ($null -eq $Ms -or -not [int64]::TryParse([string]$Ms, [ref]$v)) { return $false }
    return ($v -ge 1 -and $v -le $script:SR_QUIT_MAX_MS)
}

# A timeout from the environment (SR_DRIVE_QUIT_ACK_MS / SR_DRIVE_QUIT_EXIT_MS): digits only and in range, else
# the default. Returns @{ value; bad } (bad = the text was set and rejected).
function Get-SrQuitEnvMs([string]$Name, [int]$Default) {
    $t = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrEmpty($t)) { return @{ value = $Default; bad = $false } }
    if ($t -notmatch '^[0-9]{1,10}$' -or -not (Test-SrQuitTimeout $t)) { return @{ value = $Default; bad = $true } }
    return @{ value = [int]$t; bad = $false }
}

# Ends the process (the old ending) and records it as a fallback; never throws.
function Stop-SrQuitFallback($P, $R) {
    $R.fellBack = $true
    try { $null = & $P.Kill } catch { $R.detail += (' | Stop-Process failed: ' + $_.Exception.Message) }
    try { $R.exitCode = & $P.ExitCode } catch { $R.exitCode = $null }
}

# Runs the protocol. Returns ONE hashtable: result, graceful (the game ended itself with exit 0 after the
# ack), fellBack (Stop-Process was used), exitCode, ackMs, exitMs, request, ack, detail. Never throws: a
# protocol failure, bad arguments and any exception all end in Stop-Process and a result; the process is
# always ended when it returns (unless it could not be killed at all).
function Invoke-SrGracefulQuit {
    param(
        [string] $Instance,
        $ProcessId,
        $AckTimeoutMs  = 5000,
        $ExitTimeoutMs = 45000,
        $P
    )
    $r = @{ result = ''; graceful = $false; fellBack = $false; exitCode = $null; ackMs = $null; exitMs = $null
            request = ''; ack = ''; detail = '' }
    $ev = $null
    try {
        $bad = @()
        if (-not (Test-SrQuitTimeout $AckTimeoutMs)) { $bad += ('AckTimeoutMs {0}' -f $AckTimeoutMs) }
        if (-not (Test-SrQuitTimeout $ExitTimeoutMs)) { $bad += ('ExitTimeoutMs {0}' -f $ExitTimeoutMs) }
        $pidValue = 0
        if ($null -eq $ProcessId -or -not [int]::TryParse([string]$ProcessId, [ref]$pidValue) -or $pidValue -le 0) {
            $bad += ('ProcessId {0}' -f $ProcessId)
        }
        if ($bad.Count) {
            $r.result = 'bad-args'
            $r.detail = ('{0} out of range (timeouts: 1..{1} ms) - no request, Stop-Process' -f ($bad -join ', '), $script:SR_QUIT_MAX_MS)
            Stop-SrQuitFallback $P $r
            return $r
        }
        $AckTimeoutMs = [int]$AckTimeoutMs
        $ExitTimeoutMs = [int]$ExitTimeoutMs
        $names = Get-SrQuitEventNames $Instance $pidValue
        $r.request = $names.request; $r.ack = $names.ack
        return (Invoke-SrGracefulQuitCore $names $AckTimeoutMs $ExitTimeoutMs $P $r ([ref]$ev))
    } catch {
        $r.result = 'harness-error'
        $r.graceful = $false
        $r.detail = ('the harness side threw: {0} - Stop-Process' -f $_.Exception.Message)
        if ($P -and $P.Kill) { Stop-SrQuitFallback $P $r } else { $r.fellBack = $true }
        return $r
    } finally {
        if ($ev -and $P -and $P.CloseEvents) { try { $null = & $P.CloseEvents $ev } catch {} }
    }
}

# The protocol proper (Invoke-SrGracefulQuit validates, catches and closes the events; $EvRef receives them).
function Invoke-SrGracefulQuitCore($names, [int]$AckTimeoutMs, [int]$ExitTimeoutMs, $P, $r, [ref]$EvRef) {
    if (& $P.HasExited) {
        $r.result = 'already-exited'; $r.exitCode = & $P.ExitCode
        $r.detail = 'the game had exited before the request (no request sent)'
        return $r
    }
    $ev = & $P.OpenEvents $names
    if (-not $ev) {
        $r.result = 'no-channel'
        $r.detail = ('{0} does not exist (an exe without the quit channel, SR_QUIT_CHANNEL=0, or not an automated run) - Stop-Process' -f $names.request)
        Stop-SrQuitFallback $P $r
        return $r
    }
    $EvRef.Value = $ev  # closed by Invoke-SrGracefulQuit's finally, whatever happens below
    $t0 = & $P.NowMs
    $null = & $P.Signal $ev
    $acked = [bool](& $P.WaitAck $ev $AckTimeoutMs)
    $r.ackMs = [int64]((& $P.NowMs) - $t0)
    if (-not $acked) {
        if (& $P.HasExited) {
            $r.result = 'exited-without-ack'; $r.exitCode = & $P.ExitCode
            $r.detail = 'the game exited on its own without taking the request (it was already exiting)'
        } else {
            $r.result = 'no-ack'
            $r.detail = ('no ack within {0} ms - Stop-Process' -f $AckTimeoutMs)
            Stop-SrQuitFallback $P $r
        }
        return $r
    }
    $t1 = & $P.NowMs
    $exited = [bool](& $P.WaitExit $ExitTimeoutMs)
    $r.exitMs = [int64]((& $P.NowMs) - $t1)
    if (-not $exited) {
        $r.result = 'exit-timeout'
        $r.detail = ('acked, but still running {0} ms later - Stop-Process (no terminal snapshot)' -f $ExitTimeoutMs)
        Stop-SrQuitFallback $P $r
        return $r
    }
    $code = & $P.ExitCode
    $r.exitCode = $code
    if ($null -ne $code -and [int]$code -eq $script:SR_QUIT_EXIT_COMPLETE) {
        $r.result = 'complete'; $r.graceful = $true
        $r.detail = 'the game drained, printed its terminal snapshot and exited 0'
    } elseif ($null -ne $code -and [int]$code -eq $script:SR_QUIT_EXIT_FAILED) {
        $r.result = 'failed'
        $r.detail = 'the game could not quiesce (exit 81): see its [QUIT] terminal snapshot FAILED line'
    } elseif ($null -ne $code -and [int]$code -eq $script:SR_QUIT_EXIT_DEADLINE) {
        $r.result = 'deadline'
        $r.detail = 'the game hit its own hard deadline (exit 82, SR_QUIT_HARD_MS)'
    } else {
        $r.result = 'exit-other'
        $r.detail = ('acked, then exited with code {0} (a crash during the quit?)' -f $code)
    }
    return $r
}

# One journal line.
function Format-SrQuitResult($R) {
    $code = if ($null -eq $R.exitCode) { '-' } else { [string]$R.exitCode }
    $ack = if ($null -eq $R.ackMs) { '-' } else { [string]$R.ackMs }
    $ex = if ($null -eq $R.exitMs) { '-' } else { [string]$R.exitMs }
    $st = ''
    if ($null -ne $R.stranded) { $st = (' stranded={0}' -f $R.stranded) }   # native/quit-drain: never silent
    return ('quit: {0} ack={1}ms exit={2}ms code={3} fellBack={4}{5} ({6})' -f $R.result, $ack, $ex, $code, $R.fellBack, $st, $R.detail)
}

# native/quit-drain: the game's own record of STRANDED threads (kernel/quit.cpp), read from its log once it has
# exited: stranded = the N of the LAST "[QUIT] terminal snapshot complete ... stranded=N" after the last request
# ($null: no complete line, or an exe before the field), strandedThreads = its "[QUIT] stranded tid=.." lines (the
# text after "stranded "). A stranded thread is acknowledged, never silent: graders read these (or the log) and can
# forbid stranded > 0 (tools/perf/terminal_snapshot.py --forbid-stranded). Sets $R.stranded / $R.strandedThreads;
# never throws.
function Add-SrQuitTerminalInfo($R, [string]$LogPath) {
    if (-not $R) { return }
    $R.stranded = $null
    $R.strandedThreads = @()
    try {
        if (-not $LogPath -or -not (Test-Path -LiteralPath $LogPath)) { return }
        $lines = @(Get-Content -LiteralPath $LogPath -ErrorAction Stop)
        $req = -1
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match '\[QUIT\] request received: ') { $req = $i }
        }
        if ($req -lt 0) { return }
        $threads = @()
        for ($i = $req + 1; $i -lt $lines.Count; $i++) {
            $l = $lines[$i]
            if ($l -match '\[QUIT\] stranded (tid=\d+ wait=\S+ .*?locks=\d+)') { $threads += $Matches[1] }
            elseif ($l -match '\[QUIT\] terminal snapshot complete ') {
                if ($l -match ' stranded=(\d+)\s*$') { $R.stranded = [int]$Matches[1] }
                $R.strandedThreads = $threads
            }
        }
    } catch {
        $R.stranded = $null
    }
}

# <attempt>\quit.json (ASCII). Written for every ending - a missing result (the harness failed before it had
# one) is recorded as harness-error. Returns $true when the file was written; never throws.
function Write-SrQuitRecord([string]$Path, $R) {
    try {
        if (-not $R) {
            $R = @{ result = 'harness-error'; graceful = $false; fellBack = $true; exitCode = $null
                    detail = 'no quit result: the harness side failed before it had one' }
        }
        $o = [ordered]@{ schema = 'sr-quit/1'; result = [string]$R.result; graceful = [bool]$R.graceful
                         fellBack = [bool]$R.fellBack; exitCode = $R.exitCode; ackMs = $R.ackMs; exitMs = $R.exitMs
                         request = $R.request; ack = $R.ack; detail = [string]$R.detail
                         stranded = $R.stranded
                         # a typed array, never $(...): a subexpression unrolls one line to a bare string and none to null
                         strandedThreads = [string[]]@($R.strandedThreads | Where-Object { $_ }) }
        ($o | ConvertTo-Json) | Set-Content -Path $Path -Encoding ascii -ErrorAction Stop
        return $true
    } catch {
        return $false
    }
}
