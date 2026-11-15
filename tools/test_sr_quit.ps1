# test_sr_quit.ps1 - tools/graceful-quit: the harness side of the graceful quit (tools/sr_quit.ps1;
# kernel/quit.hpp; docs/TOOLKIT.md 1.8m).
#
# No game, no window. Four parts:
#   A. the event names mirror kernel/quit.cpp (sanitised instance, "default", 64 cap, PID suffix);
#   B. Invoke-SrGracefulQuit through SIMULATED probes: every result (complete, failed, deadline, exit-other,
#      no-channel, no-ack, exited-without-ack, exit-timeout, already-exited), when it falls back to
#      Stop-Process (and only then), the order open -> signal -> wait ack -> wait exit, the events closed on
#      every path that opened them, and exactly ONE hashtable returned (no stray success-stream output);
#      bad bounds (-1, 0, > 600000) and a probe / Stop-Process that throws: a result, the fallback, no throw;
#      the receipt written for every ending; the env bounds (Get-SrQuitEnvMs);
#   C. the LIVE Win32 objects: a stand-in child process creates the game's two named events (its own PID),
#      waits for the request and answers like the game would; the production probes open, signal, see the
#      ack and the exit code; a child without the channel and a child that never acks are killed;
#   D. the wiring by AST / text: sr_drive.ps1 dot-sources sr_quit.ps1, has -GracefulQuit (+ the env opt-in),
#      calls Invoke-SrGracefulQuit inside the finalization in place of the Stop-Process and before the log
#      copy, writes quit.json; sr_roll.ps1 and run_recomp_x.ps1 pass the switch on.
# Exit code = number of failed checks. ASCII only.
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)
. (Join-Path $PSScriptRoot 'sr_quit.ps1')
$pass = 0; $fail = 0
function Check([bool]$ok, [string]$name) {
    if ($ok) { $script:pass++; Write-Host "  ok  $name" }
    else { $script:fail++; Write-Host "FAIL: $name" }
}

# ---- A. names ---------------------------------------------------------------------
$n = Get-SrQuitEventNames 'run0' 1234
Check ($n.request -ceq 'Local\SR_QUIT_run0_1234' -and $n.ack -ceq 'Local\SR_QUIT_ACK_run0_1234') "A1 request / ack names"
Check ((Get-SrQuitInstanceName '') -ceq 'default') "A2 empty -> default"
Check ((Get-SrQuitInstanceName 'a b\c/d:e.f-g_h') -ceq 'a_b_c_d_e.f-g_h') "A3 sanitised like kernel/quit.cpp"
Check ((Get-SrQuitInstanceName ('x' * 80)).Length -eq 64) "A4 at most 64 characters"

# ---- B. simulated probes ------------------------------------------------------------
# $S: the simulated game. exited (before), channel, ack (bool), exitAfterAck (bool), code, exitDuringAckWait.
function New-SimProbes($S) {
    $log = New-Object System.Collections.ArrayList
    $st = @{ exited = [bool]$S.exited; code = $S.code; killed = 0; log = $log }
    $p = @{
        HasExited   = { [bool]$st.exited }.GetNewClosure()
        ExitCode    = { if ($st.exited) { $st.code } else { $null } }.GetNewClosure()
        OpenEvents  = { param($names) [void]$log.Add('open'); if ($S.channel) { return @{ request = 'R'; ack = 'A' } } else { return $null } }.GetNewClosure()
        Signal      = { param($ev) [void]$log.Add('signal') }.GetNewClosure()
        WaitAck     = { param($ev, [int]$ms) [void]$log.Add("waitAck:$ms"); if ($S.exitDuringAckWait) { $st.exited = $true }; return [bool]$S.ack }.GetNewClosure()
        WaitExit    = { param([int]$ms) [void]$log.Add("waitExit:$ms"); if ($S.exitAfterAck) { $st.exited = $true; return $true }; return $false }.GetNewClosure()
        Kill        = { [void]$log.Add('kill'); $st.killed++; $st.exited = $true; if ($null -eq $st.code) { $st.code = -1 } }.GetNewClosure()
        CloseEvents = { param($ev) if ($ev) { [void]$log.Add('close') } }.GetNewClosure()
        NowMs       = { [int64]1000 }
    }
    return @{ P = $p; St = $st }
}
function Sim($S, [int]$Ack = 5000, [int]$Exit = 45000) {
    $sim = New-SimProbes $S
    $out = @(Invoke-SrGracefulQuit -Instance 'run0' -ProcessId 4242 -AckTimeoutMs $Ack -ExitTimeoutMs $Exit -P $sim.P)
    return @{ Out = $out; R = $out[0]; St = $sim.St; Log = (@($sim.St.log) -join ',') }
}

$x = Sim @{ channel = $true; ack = $true; exitAfterAck = $true; code = 0 }
Check ($x.Out.Count -eq 1 -and $x.R -is [hashtable]) "B1 exactly one hashtable on the success stream"
Check ($x.R.result -eq 'complete' -and $x.R.graceful -and -not $x.R.fellBack -and $x.R.exitCode -eq 0 -and $x.St.killed -eq 0) "B2 ack + exit 0 -> complete, graceful, no Stop-Process"
Check ($x.Log -eq 'open,signal,waitAck:5000,waitExit:45000,close') "B3 order: open both events, signal, wait ack, wait exit, close ($($x.Log))"
Check ($x.R.request -ceq 'Local\SR_QUIT_run0_4242' -and $x.R.ack -ceq 'Local\SR_QUIT_ACK_run0_4242') "B4 the names of THIS process's channel"

$x = Sim @{ channel = $true; ack = $true; exitAfterAck = $true; code = 81 }
Check ($x.R.result -eq 'failed' -and -not $x.R.graceful -and -not $x.R.fellBack -and $x.St.killed -eq 0) "B5 ack + exit 81 -> failed (the game's FAILED line), no Stop-Process"
$x = Sim @{ channel = $true; ack = $true; exitAfterAck = $true; code = 82 }
Check ($x.R.result -eq 'deadline' -and -not $x.R.graceful -and $x.St.killed -eq 0) "B6 ack + exit 82 -> deadline"
$x = Sim @{ channel = $true; ack = $true; exitAfterAck = $true; code = -1073741819 }
Check ($x.R.result -eq 'exit-other' -and -not $x.R.graceful -and $x.R.exitCode -eq -1073741819) "B7 ack + a crash code -> exit-other"

$x = Sim @{ channel = $false }
Check ($x.R.result -eq 'no-channel' -and $x.R.fellBack -and $x.St.killed -eq 1 -and $x.Log -eq 'open,kill') "B8 no channel (an older exe) -> Stop-Process at once, nothing signalled ($($x.Log))"

$x = Sim @{ channel = $true; ack = $false } 1234
Check ($x.R.result -eq 'no-ack' -and $x.R.fellBack -and $x.St.killed -eq 1 -and $x.Log -eq 'open,signal,waitAck:1234,kill,close') "B9 no ack -> Stop-Process after the ack timeout, events closed ($($x.Log))"

$x = Sim @{ channel = $true; ack = $false; exitDuringAckWait = $true; code = 0 }
Check ($x.R.result -eq 'exited-without-ack' -and -not $x.R.fellBack -and $x.St.killed -eq 0) "B10 the game exiting on its own (no ack) -> exited-without-ack, nothing killed"

$x = Sim @{ channel = $true; ack = $true; exitAfterAck = $false } 5000 777
Check ($x.R.result -eq 'exit-timeout' -and $x.R.fellBack -and $x.St.killed -eq 1 -and $x.Log -eq 'open,signal,waitAck:5000,waitExit:777,kill,close') "B11 ack but no exit -> Stop-Process after the exit timeout ($($x.Log))"

$x = Sim @{ exited = $true; code = 3; channel = $true }
Check ($x.R.result -eq 'already-exited' -and $x.R.exitCode -eq 3 -and $x.Log -eq '' -and $x.St.killed -eq 0) "B12 already exited -> no request, nothing killed"

$line = Format-SrQuitResult @{ result = 'complete'; ackMs = 12; exitMs = 340; exitCode = 0; fellBack = $false; detail = 'd' }
Check ($line -ceq 'quit: complete ack=12ms exit=340ms code=0 fellBack=False (d)') "B13 the journal line"

# Astra round 1, P2-6: bad bounds, exceptions - always a result, always the fallback, never a throw.
$x = Sim @{ channel = $true; ack = $true; exitAfterAck = $true; code = 0 } -1
Check ($x.Out.Count -eq 1 -and $x.R.result -eq 'bad-args' -and $x.R.fellBack -and $x.St.killed -eq 1 -and $x.Log -eq 'kill') "B14 AckTimeoutMs -1 (= wait forever) -> bad-args: no request, Stop-Process ($($x.Log))"
$x = Sim @{ channel = $true; ack = $true; exitAfterAck = $true; code = 0 } 5000 0
$y = Sim @{ channel = $true; ack = $true; exitAfterAck = $true; code = 0 } 5000 600001
Check ($x.R.result -eq 'bad-args' -and $y.R.result -eq 'bad-args' -and $x.St.killed -eq 1 -and $y.St.killed -eq 1) "B15 ExitTimeoutMs 0 and 600001 -> bad-args, Stop-Process"
$sim = New-SimProbes @{ channel = $true; ack = $true; exitAfterAck = $true; code = 0 }
$sim.P.WaitAck = { param($ev, [int]$ms) throw 'boom: the wait failed' }
$out = @(Invoke-SrGracefulQuit -Instance 'run0' -ProcessId 4242 -P $sim.P)
$lg = @($sim.St.log) -join ','
Check ($out.Count -eq 1 -and $out[0].result -eq 'harness-error' -and $out[0].fellBack -and $sim.St.killed -eq 1 -and $out[0].detail -match 'boom' -and $lg -eq 'open,signal,kill,close') "B16 a probe that throws after the request -> harness-error, Stop-Process, events closed, no throw ($lg)"
$sim = New-SimProbes @{ channel = $true }
$sim.P.OpenEvents = { param($names) throw 'boom: open' }
$out = @(Invoke-SrGracefulQuit -Instance 'run0' -ProcessId 4242 -P $sim.P)
Check ($out.Count -eq 1 -and $out[0].result -eq 'harness-error' -and $sim.St.killed -eq 1) "B17 opening the events throws -> harness-error, Stop-Process"
$sim = New-SimProbes @{ channel = $true; ack = $false }
$sim.P.Kill = { throw 'boom: access denied' }
$out = @(Invoke-SrGracefulQuit -Instance 'run0' -ProcessId 4242 -AckTimeoutMs 10 -P $sim.P)
Check ($out.Count -eq 1 -and $out[0].result -eq 'no-ack' -and $out[0].fellBack -and $out[0].detail -match 'Stop-Process failed') "B18 a Stop-Process that throws is recorded, not thrown"
$rd = Join-Path (Split-Path -Parent $PSScriptRoot) ("archive\test_sr_quit\rec" + $PID)
New-Item -ItemType Directory -Force $rd | Out-Null
$okA = Write-SrQuitRecord (Join-Path $rd 'a.json') @{ result = 'complete'; graceful = $true; fellBack = $false; exitCode = 0 }
$okB = Write-SrQuitRecord (Join-Path $rd 'b.json') $null
$ja = Get-Content (Join-Path $rd 'a.json') -Raw | ConvertFrom-Json
$jb = Get-Content (Join-Path $rd 'b.json') -Raw | ConvertFrom-Json
$okC = Write-SrQuitRecord (Join-Path $rd 'no\such\dir\c.json') @{ result = 'complete' }
Check ($okA -and $okB -and -not $okC -and $ja.schema -eq 'sr-quit/1' -and $ja.result -eq 'complete' -and $ja.exitCode -eq 0 -and $jb.result -eq 'harness-error' -and $jb.fellBack) "B19 the receipt: written for a result and for none (harness-error); an unwritable path returns false, never throws"
# native/quit-drain: the game's stranded record read from its log into the receipt (never silent)
$lg = Join-Path $rd 'run.log'
@('[x] [QUIT] request received: frames=1 swaps=1 vdswaps=1 -> quiesce',
  '[x] [QUIT] stranded tid=7 wait=GpuBlockOnFence dev=70700000 target=00000010 completed=0000000E next=00000012 seq=3 locks=1 (old request)',
  '[x] [QUIT] terminal snapshot FAILED stage=settle reason=threads-not-quiescent',
  '[x] [QUIT] request received: frames=2 swaps=2 vdswaps=2 -> quiesce',
  '[x] [QUIT] stage settle: every thread acknowledged ... stranded=1 untracked=0',
  '[x] [QUIT] stranded tid=42740 wait=GpuBlockOnFence dev=40002880 target=0001D315 completed=0001D313 next=0001D31B seq=900 locks=3 (an XDK GPU wait the parked CP can never end: a publish the ring cut dropped)',
  '[x] [QUIT] terminal snapshot complete frames=2 swaps=2 vdswaps=3 tail=0 rl_pending=0 threads=9 parked=8 waiting=0 host_idle=0 families=9 fields=99 elapsed_ms=400 stranded=1') |
    Set-Content -Path $lg -Encoding ascii
$gs = @{ result = 'complete'; graceful = $true; fellBack = $false; exitCode = 0 }
Add-SrQuitTerminalInfo $gs $lg
$okS = Write-SrQuitRecord (Join-Path $rd 's.json') $gs
$js = Get-Content (Join-Path $rd 's.json') -Raw | ConvertFrom-Json
$g0 = @{ result = 'complete' }
Add-SrQuitTerminalInfo $g0 (Join-Path $rd 'missing.log')
@('[x] [QUIT] request received: frames=2', '[x] [QUIT] terminal snapshot complete frames=2 swaps=2 vdswaps=3 tail=0 rl_pending=0 threads=9 parked=9 waiting=0 host_idle=0 families=9 fields=99 elapsed_ms=400') |
    Set-Content -Path (Join-Path $rd 'old.log') -Encoding ascii
$gOld = @{ result = 'complete' }
Add-SrQuitTerminalInfo $gOld (Join-Path $rd 'old.log')
$okO = Write-SrQuitRecord (Join-Path $rd 'o.json') $gOld
$jo = Get-Content (Join-Path $rd 'o.json') -Raw | ConvertFrom-Json
Check ($gs.stranded -eq 1 -and @($gs.strandedThreads).Count -eq 1 -and $gs.strandedThreads[0] -ceq 'tid=42740 wait=GpuBlockOnFence dev=40002880 target=0001D315 completed=0001D313 next=0001D31B seq=900 locks=3' -and $okS -and $js.stranded -eq 1 -and @($js.strandedThreads).Count -eq 1 -and $js.strandedThreads[0] -match '^tid=42740 ' -and (Format-SrQuitResult $gs) -match ' stranded=1 \(') "B21 native/quit-drain: the stranded count and lines after the LAST request go into the receipt and the journal line"
Check ($null -eq $g0.stranded -and @($g0.strandedThreads).Count -eq 0 -and $null -eq $gOld.stranded -and $okO -and $null -eq $jo.stranded -and @($jo.strandedThreads).Count -eq 0 -and (Format-SrQuitResult $gOld) -notmatch 'stranded') "B22 no log, or an exe before the field: stranded null, no lines (never throws)"
Remove-Item -Recurse -Force $rd -ErrorAction SilentlyContinue
$envOut = @()
foreach ($v in @('-1', 'abc', '0', '600001', '5000ms', '', '7000')) {
    [Environment]::SetEnvironmentVariable('SR_TEST_QUIT_MS', $v)
    $g = Get-SrQuitEnvMs 'SR_TEST_QUIT_MS' 5000
    $envOut += ('{0}:{1}:{2}' -f $v, $g.value, $g.bad)
}
[Environment]::SetEnvironmentVariable('SR_TEST_QUIT_MS', $null)
$envLine = $envOut -join ' '
Check ($envLine -ceq '-1:5000:True abc:5000:True 0:5000:True 600001:5000:True 5000ms:5000:True :5000:False 7000:7000:False') "B20 SR_DRIVE_QUIT_*_MS: -1 / letters / 0 / too large / a unit -> the default (flagged), digits in range taken ($envLine)"

# ---- C. live Win32 events against a stand-in game --------------------------------------------
# The child plays the game's side of kernel/quit.cpp: it creates both events under ITS OWN PID, writes a
# ready file, waits for the request, sets the ack and exits with the mode's code.
$tmp = Join-Path (Split-Path -Parent $PSScriptRoot) ("archive\test_sr_quit\" + $PID)
New-Item -ItemType Directory -Force $tmp | Out-Null
function Start-StandIn([string]$Mode, [string]$Inst) {
    $ready = Join-Path $tmp ($Mode + '.ready')
    if (Test-Path $ready) { Remove-Item $ready -Force }
    $code = @"
`$mode = '$Mode'; `$inst = '$Inst'; `$ready = '$ready'
`$mr = [System.Threading.EventResetMode]::ManualReset
if (`$mode -ne 'nochannel') {
  `$req = New-Object System.Threading.EventWaitHandle(`$false, `$mr, ('Local\SR_QUIT_' + `$inst + '_' + `$PID))
  `$ack = New-Object System.Threading.EventWaitHandle(`$false, `$mr, ('Local\SR_QUIT_ACK_' + `$inst + '_' + `$PID))
}
Set-Content -Path `$ready -Value 'ready' -Encoding ascii
if (`$mode -eq 'nochannel' -or `$mode -eq 'noack') { Start-Sleep -Seconds 60; exit 5 }
if (-not `$req.WaitOne(30000)) { exit 6 }
[void]`$ack.Set()
Start-Sleep -Milliseconds 300
if (`$mode -eq 'ack81') { exit 81 }
exit 0
"@
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))
    $proc = Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile', '-NonInteractive', '-EncodedCommand', $enc) `
              -WindowStyle Hidden -PassThru
    $null = $proc.Handle
    $deadline = (Get-Date).AddSeconds(30)
    while (-not (Test-Path $ready) -and (Get-Date) -lt $deadline -and -not $proc.HasExited) { Start-Sleep -Milliseconds 100 }
    return $proc
}
$inst = "gqtest$PID"
$c = Start-StandIn 'ack0' $inst
$r = Invoke-SrGracefulQuit -Instance $inst -ProcessId $c.Id -AckTimeoutMs 5000 -ExitTimeoutMs 15000 -P (New-SrQuitProbes $c)
Check ($r.result -eq 'complete' -and $r.graceful -and $r.exitCode -eq 0 -and -not $r.fellBack -and $c.HasExited) "C1 live: request -> ack -> exit 0 = complete ($(Format-SrQuitResult $r))"

$c = Start-StandIn 'ack81' $inst
$r = Invoke-SrGracefulQuit -Instance $inst -ProcessId $c.Id -AckTimeoutMs 5000 -ExitTimeoutMs 15000 -P (New-SrQuitProbes $c)
Check ($r.result -eq 'failed' -and $r.exitCode -eq 81 -and -not $r.fellBack) "C2 live: exit 81 read back = failed ($(Format-SrQuitResult $r))"

$c = Start-StandIn 'nochannel' $inst
$r = Invoke-SrGracefulQuit -Instance $inst -ProcessId $c.Id -AckTimeoutMs 2000 -ExitTimeoutMs 5000 -P (New-SrQuitProbes $c)
Check ($r.result -eq 'no-channel' -and $r.fellBack -and $c.HasExited) "C3 live: no channel -> killed at once ($(Format-SrQuitResult $r))"

$c = Start-StandIn 'noack' $inst
$t0 = Get-Date
$r = Invoke-SrGracefulQuit -Instance $inst -ProcessId $c.Id -AckTimeoutMs 1500 -ExitTimeoutMs 5000 -P (New-SrQuitProbes $c)
$el = ((Get-Date) - $t0).TotalMilliseconds
Check ($r.result -eq 'no-ack' -and $r.fellBack -and $c.HasExited -and $el -ge 1400) "C4 live: no ack -> killed after the 1.5 s ack timeout ($(Format-SrQuitResult $r))"

# A request for ANOTHER pid's channel opens nothing: the stand-in (its own PID) is never signalled.
$c = Start-StandIn 'ack0' $inst
$names = Get-SrQuitEventNames $inst ($c.Id + 1)
$ev = & (New-SrQuitProbes $c).OpenEvents $names
Check ($null -eq $ev) "C5 live: the channel of another PID does not exist (a harness signals only the game it launched)"
$r = Invoke-SrGracefulQuit -Instance $inst -ProcessId $c.Id -AckTimeoutMs 5000 -ExitTimeoutMs 15000 -P (New-SrQuitProbes $c)
Check ($r.result -eq 'complete') "C6 live: ... and its own channel still works"
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

# ---- D. wiring -----------------------------------------------------------------------
$drive = Get-Content 'tools/sr_drive.ps1' -Raw
$t = $null; $e = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path 'tools/sr_drive.ps1').Path, [ref]$t, [ref]$e)
Check (-not $e -or $e.Count -eq 0) "D0 sr_drive.ps1 parses"
$params = @($ast.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath })
Check ($params -contains 'GracefulQuit') "D1 sr_drive has -GracefulQuit"
Check ($drive -match "\. \(Join-Path \`$PSScriptRoot 'sr_quit\.ps1'\)") "D2 sr_drive dot-sources sr_quit.ps1"
Check ($drive -match "\`$script:GracefulQuit = \[bool\]\`$GracefulQuit -or \(\`$env:SR_DRIVE_GRACEFUL_QUIT -eq '1'\)") "D3 the switch or SR_DRIVE_GRACEFUL_QUIT=1 opts in"
$iGates = $drive.IndexOf('Set-Item -Path ("Env:" + $kv[0].Trim())'); $iOpt = $drive.IndexOf('$script:GracefulQuit = [bool]$GracefulQuit')
Check ($iGates -gt 0 -and $iOpt -gt $iGates) "D4 the opt-in is read AFTER the gates are applied (a -Gates SR_DRIVE_GRACEFUL_QUIT=1 counts)"
# The finalization: Invoke-SrGracefulQuit sits in the branch that used to be the bare Stop-Process, before the log copy.
$iFin = $drive.IndexOf('$fin = Invoke-CleanupAndFinalize')
$iQuit = $drive.IndexOf('$gq = Invoke-SrGracefulQuit')
$iStop = $drive.IndexOf('try { Stop-Process -Id $proc.Id -Force -ErrorAction Stop } catch {}', $iFin)
$iCopy = $drive.IndexOf('Copy-Item $script:LogPath $finalLogCopy')
$iKeep = $drive.LastIndexOf('if (-not $KeepAlive -and $proc -and -not $proc.HasExited) {', $iQuit)
Check ($iFin -gt 0 -and $iQuit -gt $iFin -and $iStop -gt $iQuit -and $iCopy -gt $iStop) "D5 finalization -> graceful quit -> (else) Stop-Process -> log copy"
Check ($iKeep -gt $iFin -and $iKeep -lt $iQuit -and $drive.Substring($iKeep, $iQuit - $iKeep) -match 'if \(\$script:GracefulQuit\) \{') "D6 the quit runs only without -KeepAlive, and only when opted in"
Check ($drive -match 'Invoke-SrGracefulQuit -Instance \$env:SR_INSTANCE_ID -ProcessId \$proc\.Id') "D7 the quit targets THIS roll's instance and PID (the child after a RESTART follow)"
Check ($drive -match 'Write-SrQuitRecord \(Join-Path \$roots\.AttemptDir "quit\.json"\) \$gq' -and $drive -match 'Note \(Format-SrQuitResult \$gq\)') "D8 quit.json in the attempt dir + one journal line"
$roll = Get-Content 'tools/sr_roll.ps1' -Raw
Check ($roll -match '\[switch\] \$GracefulQuit' -and $roll -match 'if \(\$GracefulQuit\) \{ \$driveArgs \+= "-GracefulQuit" \}') "D9 sr_roll -GracefulQuit is passed to sr_drive"
$rx = Get-Content 'tools/run_recomp_x.ps1' -Raw
Check ($rx -match '\[switch\] \$GracefulQuit' -and $rx -match 'Invoke-SrGracefulQuit -Instance \("run\{0\}" -f \$r\.Index\)') "D10 run_recomp_x -GracefulQuit asks each instance to quit"
# P2-6: the receipt in a finally (written whatever Invoke does), the env bounds parsed by Get-SrQuitEnvMs.
$iTry = $drive.IndexOf('$gq = $null', $iKeep)
$iFinally = $drive.IndexOf('} finally {', $iTry)
$iRec = $drive.IndexOf('Write-SrQuitRecord (Join-Path $roots.AttemptDir "quit.json") $gq', $iTry)
Check ($iTry -gt $iKeep -and $iTry -lt $iQuit -and $iFinally -gt $iQuit -and $iRec -gt $iFinally -and $iRec -lt $iCopy) "D12 sr_drive writes quit.json in a finally around the quit (every ending has a receipt)"
Check ($drive -match "Get-SrQuitEnvMs 'SR_DRIVE_QUIT_ACK_MS' 5000" -and $drive -match "Get-SrQuitEnvMs 'SR_DRIVE_QUIT_EXIT_MS' 45000" -and $drive -notmatch '\[int\]\$env:SR_DRIVE_QUIT') "D13 sr_drive's quit bounds go through Get-SrQuitEnvMs (no bare [int] cast of the env)"
Check ($rx -match "Write-SrQuitRecord \(\[System\.IO\.Path\]::ChangeExtension\(\`$r\.Log, '\.quit\.json'\)\) \`$gq" -and $roll -match 'Copy-Item -LiteralPath \(Join-Path \$receipt\.FullName "quit\.json"\) \(Join-Path \$Out "quit\.json"\)') "D14 run_recomp_x writes <log>.quit.json; sr_roll copies the last attempt's quit.json beside its run.log"
$iAdd = $drive.IndexOf('Add-SrQuitTerminalInfo $gq $script:LogPath', $iQuit)
Check ($iAdd -gt $iQuit -and $iAdd -lt $iFinally -and $rx -match 'Add-SrQuitTerminalInfo \$gq \$r\.Log') "D15 native/quit-drain: sr_drive reads the game's stranded record after the quit, before the receipt; run_recomp_x too"
$qs = Get-Content 'tools/sr_quit.ps1' -Raw
Check ($qs -notmatch 'Add-Type' -and ([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((Resolve-Path 'tools/sr_quit.ps1').Path)) -ceq [IO.File]::ReadAllText((Resolve-Path 'tools/sr_quit.ps1').Path))) "D11 sr_quit.ps1 is ASCII and compiles nothing (safe to dot-source before -ValidateOnly)"

Write-Host ("test_sr_quit: {0} passed, {1} failed" -f $pass, $fail)
exit $fail
